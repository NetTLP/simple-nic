
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

/* structure describing snic queue, actually this is a thread */
struct nettlp_snic_queue {
	struct nettlp nt;	/* nettlp context (socket )*/
	int fd;			/* tap fd socket */

	uintptr_t addr;		/* start address of struct bar4 in BAR4 */
	struct snic_bar4 bar4;	/* bar4 field for this queue */

	char buf[2048];		/* rx packet buffer */
};

#define is_mwr_addr_tx_desc_ptr(q, a) \
	(a - q->addr == (char *)(&q->bar4.tx_desc_ptr) - (char *)(&q->bar4))

#define is_mwr_addr_rx_desc_ptr(q, a) \
	(a - q->addr == (char *)(&q->bar4.rx_desc_ptr) - (char *)(&q->bar4))

#define is_mwr_addr_tx_irq_ptr(q, a)\
	(a - q->addr == (char *)(&q->bar4.tx_irq_ptr) - (char *)(&q->bar4))

#define is_mwr_addr_rx_irq_ptr(q, a)\
	(a - q->addr == (char *)(&q->bar4.rx_irq_ptr) - (char *)(&q->bar4))

int nettlp_snic_mwr(struct nettlp *nt, struct tlp_mr_hdr *mh,
		    void *m, size_t count, void *arg)
{
	int ret;
	struct nettlp_snic_queue *q = arg;
	struct descriptor desc;
	uintptr_t dma_addr, addr;
	char buf[4096];

	dma_addr = tlp_mr_addr(mh);
	printf("%s: dma_addr is %#lx\n", __func__, dma_addr);


	if (is_mwr_addr_tx_desc_ptr(q, dma_addr)) {
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
		
		printf("pkt length is %lu, addr is %#lx\n\n",
		       desc.length, desc.addr);

		/* read packet from the pointer in the tx descriptor */
		ret = dma_read(nt, desc.addr, buf, desc.length);
		if (ret < desc.length) {
			fprintf(stderr, "failed to read tx pkt form %#lx, "
				"%lu-byte\n", desc.addr, desc.length);
			goto tx_end;
		}

		/* ok, the tx packet is located in the buffer. xmit to tap */
		ret = write(q->fd, buf, desc.length);
		if (ret < 0) {
			fprintf(stderr, "failed to tx pkt to tap\n");
			perror("write");
			goto tx_end;
		}

	tx_end:
		/* set pseudo interrupt */
		if (q->bar4.tx_irq_ptr == 0) {
			fprintf(stderr, "tx irq ptr is not set (0)\n");
		} else {
			/* set the irq to 1 (this iteration is done ) */
			uint32_t val = 1;
			ret = dma_write(nt, q->bar4.tx_irq_ptr, &val,
					sizeof(val));
			if (ret < sizeof(val)) {
				fprintf(stderr, "failed to update tx irq\n");
			}
		}

		printf("tx done\n");
		return 0;

	} else if (is_mwr_addr_rx_desc_ptr(q, dma_addr)) {
		/* RX descriptor is udpated. start RX process */
		memcpy(&addr, m, sizeof(addr));
		printf("dma to %#lx, rx desc ptr is %#lx\n", dma_addr, addr);

	} else if (is_mwr_addr_tx_irq_ptr(q, dma_addr)) {
		/* set pseudo tx irq address */
		memcpy(&q->bar4.tx_irq_ptr, m, count);

	} else if (is_mwr_addr_rx_irq_ptr(q, dma_addr)) {
		/* set pseudo rx irq address */
		memcpy(&q->bar4.rx_irq_ptr, m, count);
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
	       "    -a BAR4 base address\n"
	       "    -i tap interface name to be created\n"
		);
}

int main(int argc, char **argv)
{
	int ch;
	struct nettlp nt;
	struct nettlp_cb cb;
	struct nettlp_snic_queue q;
	uintptr_t addr = 0;
	uint16_t busn, devn;
	char *ifname = "tap0";	/* default */
	
	memset(&nt, 0, sizeof(nt));

	while ((ch = getopt(argc, argv, "r:l:b:a:i:")) != -1) {
		switch (ch) {
		case 'r':
			if (inet_pton(AF_INET, optarg, &nt.remote_addr) < 1) {
				perror("inet_pton for remote addr");
				return -1;
			}
			break;
		case 'l':
			if (inet_pton(AF_INET, optarg, &nt.local_addr) < 1) {
				perror("inet_pton for local addr");
				return -1;
			}
			break;
		case 'b':
			sscanf(optarg, "%hx:%hx", &busn, &devn);
			nt.requester = (busn << 8 | devn);
			break;
		case 'a':
			addr = strtoul(optarg, NULL, 16);
			break;
		case 'i':
			ifname = optarg;
			break;
		default:
			usage();
			return -1;
		}
	}

	/* initialize nettlp */
	if (nettlp_init(&nt) < 0) {
		perror("nettlp_init");
		return -1;
	}

	/* initialize callback */
	memset(&cb, 0, sizeof(cb));
	cb.mwr = nettlp_snic_mwr;

	/* initalize tap interface */
	memset(&q, 0, sizeof(q));
	q.nt = nt;
	q.addr = addr;
	if ((q.fd = tap_alloc(ifname)) < 0) {
		fprintf(stderr, "failed to open tap interface '%s'\n", ifname);
		return -1;
	}
	if (tap_up(ifname) < 0) {
		fprintf(stderr, "failed to up tap interface '%s'\n", ifname);
		return -1;
	}
	

	printf("start to callback for simple nic device\n");
	printf("BAR4 start address is %#lx\n", q.addr);
	nettlp_run_cb(&nt, &cb, &q);

	return 0;
}
