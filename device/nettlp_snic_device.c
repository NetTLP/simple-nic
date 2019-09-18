
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>

#include <libtlp.h>
#include <nettlp_snic.h>

static int caught_signal = 0;

struct nettlp_snic {

	int fd;		/* tun fd */

	/* filled by message API */
	uintptr_t bar4_start;
	struct nettlp_msix tx_irq, rx_irq;

	/* descriptor base */
	uintptr_t tx_desc_base;
	uintptr_t rx_desc_base;

	/* XXX: should be protected among threads by mutex */
	int rx_state;
#define RX_STATE_INIT	0
#define	RX_STATE_READY	1
#define RX_STATE_BUSY	2
#define RX_STATE_DONE	3
	uintptr_t rx_desc_addr;
	struct descriptor rx_desc;
	struct nettlp *rx_nt;
};

#define BAR4_TX_DESC_OFFSET	0
#define BAR4_RX_DESC_OFFSET	8

#define BAR4_TX_INDEX_OFFSET	16
#define BAR4_RX_INDEX_OFFSET	20

#define is_mwr_addr_tx_desc_ptr(bar4, a)			\
	(a - bar4 == BAR4_TX_DESC_OFFSET)
#define is_mwr_addr_rx_desc_ptr(bar4, a)			\
	(a - bar4 == BAR4_RX_DESC_OFFSET)
#define is_mwr_addr_tx_index_ptr(bar4, a)	     \
	(a - bar4 == BAR4_TX_INDEX_OFFSET)
#define is_mwr_addr_rx_index_ptr(bar4, a)	     \
	(a - bar4 == BAR4_RX_INDEX_OFFSET)


int nettlp_snic_mwr(struct nettlp *nt, struct tlp_mr_hdr *mh,
		    void *m, size_t count, void *arg)
{
	int ret;
	struct nettlp_snic *snic = arg;
	struct descriptor desc;
	uint32_t idx;
	uintptr_t dma_addr, addr;
	char buf[4096];

	dma_addr = tlp_mr_addr(mh);
	printf("%s: dma_addr is %#lx\n", __func__, dma_addr);

	if (is_mwr_addr_tx_desc_ptr(snic->bar4_start, dma_addr)) {
		/* save tx desc base */
		memcpy(&snic->tx_desc_base, m, 8);
		printf("TX desc base is %#lx\n", snic->tx_desc_base);
	} else if (is_mwr_addr_rx_desc_ptr(snic->bar4_start, dma_addr)) {
		/* save rx desc base */
		memcpy(&snic->rx_desc_base, m, 8);
		printf("RX desc base is %#lx\n", snic->rx_desc_base);
	} else if (is_mwr_addr_tx_index_ptr(snic->bar4_start, dma_addr)) {

		if (snic->tx_desc_base == 0) {
			fprintf(stderr, "tx_desc_base is 0\n");
			goto tx_end;
		}

		memcpy(&idx, m, sizeof(idx));
		addr = snic->tx_desc_base + (sizeof(struct descriptor) * idx);

		/* 1. TX descriptor is updated. start TX process */
		printf("idx %u, dma to %#lx, tx desc ptr is %#lx\n",
		       idx, dma_addr, addr);

		/* 2. Read tx descriptor from the specified address */
		ret = dma_read(nt, addr, &desc, sizeof(desc));
		if (ret < sizeof(desc)) {
			fprintf(stderr, "failed to read tx desc from %#lx\n",
				addr);
			goto tx_end;
		}

		printf("TX: pkt length is %lu, addr is %#lx\n",
		       desc.length, desc.addr);

		/* 3. read packet from the pointer in the tx descriptor */
		ret = dma_read(nt, desc.addr, buf, desc.length);
		if (ret < desc.length) {
			fprintf(stderr, "failed to read tx pkt form %#lx, "
				"%lu-byte\n", desc.addr, desc.length);
			goto tx_end;
		}

		/* 3.5 ok, we got the packet to be xmitted. xmit to tap */
		ret = write(snic->fd, buf, desc.length);
		if (ret < 0) {
			fprintf(stderr, "failed to tx pkt to tap\n");
			perror("write");
			goto tx_end;
		}

	tx_end:
		/* 4. Generate TX interrupt */
		ret = dma_write(nt, snic->tx_irq.addr, &snic->tx_irq.data,
				sizeof(snic->tx_irq.data));
		if (ret < 0) {
			fprintf(stderr, "failed to send TX interrupt\n");
			perror("dma_write");
		}

		printf("TX done\n");

	} else if (is_mwr_addr_rx_index_ptr(snic->bar4_start, dma_addr)) {

		if (snic->rx_desc_base == 0) {
			fprintf(stderr, "rx_desc_base is 0\n");
			return -1;
		}

		/* wait untile last DMA write done */
		while (snic->rx_state != RX_STATE_DONE &&
		       snic->rx_state != RX_STATE_INIT)
			sched_yield();	

		/* 1. RX descriptor is udpated. start RX process */
		memcpy(&idx, m, sizeof(idx));
		addr = snic->rx_desc_base + (sizeof(struct descriptor) * idx);
		snic->rx_desc_addr = addr;
		printf("RX update: dma to %#lx, rx desc ptr is %#lx\n",
		       dma_addr, snic->rx_desc_addr);

		/* 2. Read descriptor from host */
		ret = dma_read(nt, snic->rx_desc_addr, &snic->rx_desc,
			       sizeof(snic->rx_desc));
		if (ret < sizeof(snic->rx_desc)) {
			fprintf(stderr, "failed to read rx desc from %#lx\n",
				addr);
			return -1;
		}

		/* 2.1. Set RX State Ready because we have new buffer  */
		snic->rx_nt = nt;
		snic->rx_state = RX_STATE_READY;
	}

	return 0;
}


void *nettlp_snic_tap_read_thread(void * arg)
{
	int ret, pktlen;
	char buf[2048];
	struct nettlp_snic *snic = arg;
	struct pollfd x[1] = {{ .fd = snic->fd, .events = POLLIN}};

	/* This is the actual part of RX. This thread read tap socket,
	 * and if rx buffer is available, DMA Write the packet from
	 * the tap to the RX buffer on the NetTLP adapter host.
	 */

	while (1) {

		if (caught_signal)
			break;

		ret = poll(x, 1, 500);
		if (ret < 0 || ret == 0 || !(x[0].revents & POLLIN))
			continue;

		/* 2.2. read a packet from the tap interface */
		pktlen = read(snic->fd, buf, sizeof(buf));
		if (pktlen < 0) {
			perror("read");
			continue;
		}

		if (snic->rx_state != RX_STATE_READY)
			continue;

		snic->rx_state = RX_STATE_BUSY;

		/* 3. DMA the packet to host */
		ret = dma_write(snic->rx_nt, snic->rx_desc.addr, buf, pktlen);
		if (ret < 0) {
			fprintf(stderr, "failed to write rx pkt to %#lx\n",
				snic->rx_desc.addr);
			continue;
		}

		/* 4. Write back RX descriptor */
		snic->rx_desc.length = pktlen;
		ret = dma_write(snic->rx_nt, snic->rx_desc_addr,
				&snic->rx_desc, sizeof(snic->rx_desc));
		if (ret < sizeof(snic->rx_desc)) {
			fprintf(stderr, "failed to write rx desc to %#lx\n",
				snic->rx_desc_addr);
			continue;
		}

		/* 5. Generate RX interrupt */
		ret = dma_write(snic->rx_nt, snic->rx_irq.addr,
				&snic->rx_irq.data,
				sizeof(snic->rx_irq.data));
		if (ret < 0) {
			fprintf(stderr, "failed to generate RX interrupt\n");
			perror("dma_write");
		}

		printf("RX done. DMA write to %#lx %d byte\n",
		       snic->rx_desc.addr, pktlen);

		snic->rx_state = RX_STATE_DONE;
	}

	return NULL;
}



int tap_alloc(char *dev)
{
	/* create tap interface */
	int fd;
	struct ifreq ifr;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		perror("open");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
		perror("ioctl");
		close(fd);
		return -1;
	}

	return fd;
}

int tap_up(char *dev)
{
	int fd;
	struct ifreq ifr;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_UP;
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if (ioctl(fd, SIOCSIFFLAGS, (void *)&ifr) < 0) {
		perror("ioctl");
		return -1;
	}

	close(fd);

	return 0;
}


void sig_handler(int sig)
{
	caught_signal = 1;
	nettlp_stop_cb();
}

void usage(void)
{
	printf("usage\n"
	       "    -r remote addr\n"
	       "    -l local addr\n"
	       "    -R remote host addr (not TLP NIC)\n"
	       "\n"
	       "    -t tunif name (default tap0)\n"
		);
}

int main(int argc, char **argv)
{
	int ret, ch, n, fd;
	struct nettlp nt, nts[16], *nts_ptr[16];
	struct nettlp_cb cb;
	char *ifname = "tap0";
	struct nettlp_snic snic;
	struct in_addr host;
	struct nettlp_msix msix[2];	/* tx and rx interrupt */
	pthread_t rx_tid;	/* tap_read_thread */

	memset(&nt, 0, sizeof(nt));

	while ((ch = getopt(argc, argv, "r:l:b:R:t:")) != -1) {
		switch (ch) {
                case 'r':
                        ret = inet_pton(AF_INET, optarg, &nt.remote_addr);
                        if (ret < 1) {
                                perror("inet_pton");
                                return -1;
                        }
                        break;
                case 'l':
                        ret = inet_pton(AF_INET, optarg, &nt.local_addr);
                        if (ret < 1) {
                                perror("inet_pton");
                                return -1;
                        }
                        break;
		case 'R':
			ret = inet_pton(AF_INET, optarg, &host);
			if (ret < 1) {
				perror("inet_pton");
				return -1;
			}

			nt.requester = nettlp_msg_get_dev_id(host);
			break;
		case 't':
			ifname = optarg;
			break;
		default:
			usage();
			return -1;
		}
	}

	/* initalize tap interface */
	fd = tap_alloc(ifname);
	if (fd < 0) {
		perror("tap_alloc");
		return -1;
	}

	if (tap_up(ifname) < 0) {
		perror("tap_up");
		return -1;
	}

	/* initialize nettlp structures for all tags */
	for (n = 0; n < 16; n++) {
		nts[n] = nt;
		nts[n].tag = n;
		nts_ptr[n] = &nts[n];

		ret = nettlp_init(nts_ptr[n]);
		if (ret < 0) {
			printf("failed to init nettlp on tag %x\n", n);
			perror("nettlp_init");
			return ret;
		}
	}

	/* fill the snic structure */
	memset(&snic, 0, sizeof(snic));
	snic.fd = fd;
	snic.bar4_start = nettlp_msg_get_bar4_start(host);
	if (snic.bar4_start == 0) {
		printf("failed to get BAR4 addr from %s\n", inet_ntoa(host));
		perror("nettlp_msg_get_bar4_start");
		return -1;
	}
	ret = nettlp_msg_get_msix_table(host, msix, 2);
	if (ret < 0) {
		printf("failed to get MSIX from %s\n", inet_ntoa(host));
		perror("nettlp_msg_get_msix_table");
		return -1;
	}

	snic.tx_irq = msix[0];
	snic.rx_irq = msix[1];

	printf("Device is %04x\n", nt.requester);
	printf("BAR4 start address is %#lx\n", snic.bar4_start);
	printf("TX IRQ address is %#lx, data is 0x%08x\n", snic.tx_irq.addr,
	       snic.tx_irq.data);
	printf("RX IRQ address is %#lx, data is 0x%08x\n", snic.rx_irq.addr,
	       snic.rx_irq.data);

        /* set signal handler to stop callback threads */
        if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("cannot set signal\n");
		return -1;
        }

	/* start RX thread */
	printf("create tap read thread\n");
	pthread_create(&rx_tid, NULL, nettlp_snic_tap_read_thread, &snic);

	/* start nettlp call back */
	printf("start nettlp callback\n");
	memset(&cb, 0, sizeof(cb));
	cb.mwr = nettlp_snic_mwr;
	nettlp_run_cb(nts_ptr, 16, &cb, &snic);

	printf("nettlp callback done\n");

	pthread_join(rx_tid, NULL);

	return 0;
}
