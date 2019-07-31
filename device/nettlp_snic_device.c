
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

#include <libtlp.h>
#include <nettlp_snic.h>

struct nettlp_snic {

	int fd;		/* tun fd */

	/* filled by message API */
	uintptr_t bar4_start;
	struct nettlp_msix tx_irq, rx_irq;
};

#define BAR4_TX_DESC_OFFSET	0
#define BAR4_RX_DESC_OFFSET	8

#define is_mwr_addr_tx_desc_ptr(bar4, a)			\
	(a - bar4 == BAR4_TX_DESC_OFFSET)
#define is_mwr_addr_rx_desc_ptr(bar4, a)			\
	(a - bar4 == BAR4_RX_DESC_OFFSET)
	


int nettlp_snic_mwr(struct nettlp *nt, struct tlp_mr_hdr *mh,
		    void *m, size_t count, void *arg)
{
	int ret;
	struct nettlp_snic *snic = arg;
	struct descriptor desc;
	uintptr_t dma_addr, addr;
	char buf[4096];

	dma_addr = tlp_mr_addr(mh);
	printf("%s: dma_addr is %#lx\n", __func__, dma_addr);

	if (is_mwr_addr_tx_desc_ptr(snic->bar4_start, dma_addr)) {
		/* TX descriptor is updated. start TX process */
		memcpy(&addr, m, sizeof(addr));
		printf("dma to %#lx, tx desc ptr is %#lx\n", dma_addr, addr);

		/* read tx descriptor from the specified address */
		ret = dma_read(nt, addr, &desc, sizeof(desc));
		if (ret < sizeof(desc)) {
			fprintf(stderr, "failed to read tx desc from %#lx\n",
				addr);
			goto tx_end;
		}

		printf("pkt length is %lu, addr is %#lx\n",
		       desc.length, desc.addr);

		/* read packet from the pointer in the tx descriptor */
		ret = dma_read(nt, desc.addr, buf, desc.length);
		if (ret < desc.length) {
			fprintf(stderr, "failed to read tx pkt form %#lx, "
				"%lu-byte\n", desc.addr, desc.length);
			goto tx_end;
		}

		/* ok, the tx packet is located in the buffer. xmit to tap */
		ret = write(snic->fd, buf, desc.length);
		if (ret < 0) {
			fprintf(stderr, "failed to tx pkt to tap\n");
			perror("write");
			goto tx_end;
		}

	tx_end:
		/* send tx interrupt */
		ret = dma_write(nt, snic->tx_irq.addr, &snic->tx_irq.data,
				sizeof(snic->tx_irq.data));
		if (ret < 0) {
			fprintf(stderr, "failed to send TX interrupt\n");
			perror("dma_write");
		}

		printf("tx done\n\n");
		return 0;

	} else if (is_mwr_addr_rx_desc_ptr(snic->bar4_start, dma_addr)) {
		/* RX descriptor is udpated. start RX process */
		memcpy(&addr, m, sizeof(addr));
		printf("dma to %#lx, rx desc ptr is %#lx\n", dma_addr, addr);

	}

	return 0;
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



void usage(void)
{
	printf("usage\n"
	       "    -r remote addr\n"
	       "    -l local addr\n"
	       "    -b bus number, XX:XX\n"
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
	uint16_t busn, devn;
	struct nettlp_snic snic;
	struct in_addr host;
	struct nettlp_msix msix[2];	/* tx and rx interrupt */

	memset(&nt, 0, sizeof(nt));
	busn = 0;
	devn = 0;

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
                case 'b':
                        ret = sscanf(optarg, "%hx:%hx", &busn, &devn);
                        nt.requester = (busn << 8 | devn);
                        break;
		case 'R':
			ret = inet_pton(AF_INET, optarg, &host);
			if (ret < 1) {
				perror("inet_pton");
				return -1;
			}
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

	printf("BAR4 start address is %#lx\n", snic.bar4_start);
	printf("TX IRQ address is %#lx, data is 0x%08x\n", snic.tx_irq.addr,
	       snic.tx_irq.data);
	printf("RX IRQ address is %#lx, data is 0x%08x\n", snic.rx_irq.addr,
	       snic.rx_irq.data);

	/* start nettlp call back */
	memset(&cb, 0, sizeof(cb));
	cb.mwr = nettlp_snic_mwr;
	nettlp_run_cb(nts_ptr, 16, &cb, &snic);

	return 0;
}
