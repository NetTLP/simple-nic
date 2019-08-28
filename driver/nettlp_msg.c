
#include <linux/pci.h>
#include <net/udp_tunnel.h>

#include "nettlp_msg.h"

/*
 * Messaging between NetTLP driver and libtlp.
 *
 * Messageing module for communication between libtlp and this
 * driver. The messages are identified by signed 32bit integer. libtlp
 * sends a 32bit int to the driver, and then the driver returns a
 * corresponding struct.
 *
 * This code is independent from nettlp_main.c. It can be and will be
 * shared with other NetTLP drivers, such as simple-nic software
 * implementation.
 *
 * Implemented for NetTLP adapter v0.17.
 */

MODULE_LICENSE("GPL");


/* structure describing nettlp udp socket */
struct nettlp_sock {
	struct socket *sock;

	/* values to be used fro generating messages */
	uint64_t	bar4_start;	/* physical addr of BAR4 for msg 1 */
	struct nettlp_msg_id id;	/** device id for msg 2 */
	struct nettlp_msix msix[NETTLP_MAX_VEC]; /* MSIX table on BAR2 */
};

struct nettlp_sock *nsock = NULL;	/* XXX: terrified */



/* callback from net/ipv4/udp.c */
static int nettlp_msg_rcv(struct sock *sk, struct sk_buff *skb)
{
	int ret, req;
	struct nettlp_sock *ns;
	struct sockaddr_in sin;
	struct iphdr *ip;
	struct udphdr *udp;
	struct kvec iov[1];
	struct msghdr msg;

	ns = rcu_dereference_sk_user_data(sk);
	if (!ns) {
		pr_err("%s: this socket doesn't have nettlp_sock struct\n",
			__func__);
		goto drop;
	}

	if (!pskb_may_pull(skb, sizeof(struct udphdr) + sizeof(int))) {
		pr_err("%s: too short UDP packet\n", __func__);
		goto drop;
	}

	/* prepare sockaddr for reply messages */
	ip = ip_hdr(skb);
	udp = udp_hdr(skb);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ip->saddr;
	sin.sin_port = udp->source;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sin;
	msg.msg_namelen = sizeof(sin);

	req = *((int *)(udp_hdr(skb) + 1));

	switch (req) {
	case NETTLP_MSG_GET_BAR4_ADDR:
		iov[0].iov_base = &ns->bar4_start;
		iov[0].iov_len = sizeof(ns->bar4_start);
		break;
	case NETTLP_MSG_GET_DEV_ID:
		iov[0].iov_base = &ns->id;
		iov[0].iov_len = sizeof(ns->id);
		break;
	case NETTLP_MSG_GET_MSIX_TABLE:
		iov[0].iov_base = &ns->msix;
		iov[0].iov_len = sizeof(struct nettlp_msix) * NETTLP_MAX_VEC;
		break;
	}

	ret = kernel_sendmsg(ns->sock, &msg, iov, 1, iov[0].iov_len);
	if (ret < 0) {
		pr_err("%s: failed to send reply\n", __func__);
		return ret;
	}

drop:
	kfree_skb(skb);
	return 0;
}



static int nettlp_msg_get_msix_table(void *bar2_virt,
				     struct nettlp_msix *msix)
{
        int n;
        uint64_t upper, lower;
        uint32_t data;

	/*
         * MSIX table address and data are located in BAR2. Read the
         * BAR2, and fill the nettlp_dev->msix array with it.
         */
        struct msix_table_entry {
                uint32_t lower_addr;
                uint32_t upper_addr;
                uint32_t data;
                uint32_t rsv;
        } *e;

        if (!bar2_virt) {
                pr_err("%s: BAR2 is not ioremapped\n", __func__);
                return -1;
        }

        for (n = 0; n < NETTLP_MAX_VEC; n++) {
                e = bar2_virt + sizeof(struct msix_table_entry) * n;

                upper = readl(&e->upper_addr);
                lower = readl(&e->lower_addr);
                data = readl(&e->data);

                msix[n].addr = (upper << 32 | lower);
                msix[n].data = data;
        }

        return 0;
}

/* initialize the messaging module */
int nettlp_msg_init(uint64_t bar4_start, uint16_t dev_id, void *bar2_virt)
{
	int err;
	struct nettlp_sock *ns;
	struct socket *sock;
	struct udp_port_cfg udp_conf;
	struct udp_tunnel_sock_cfg tunnel_cfg;

	pr_info("initialize nettlp message socket\n");

	if (nsock) {
		pr_err("%s: NetTLP message socket is already initialized\n",
			__func__);
		return -EEXIST;
	}

	/* initialize the structure and a socket */
	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return -ENOMEM;

	/* gather the information to be sent */
	ns->bar4_start = bar4_start;
	ns->id.id = dev_id;
	nettlp_msg_get_msix_table(bar2_virt, ns->msix);

	/* open UDP socket for receiving requests */
	memset(&udp_conf, 0, sizeof(udp_conf));
	udp_conf.family = AF_INET;
	udp_conf.local_udp_port = htons(NETTLP_MSG_PORT);
	err = udp_sock_create(&init_net, &udp_conf, &sock);
	if (err < 0)
		return err;
	ns->sock = sock;

	/* setup callback on udp tunnel */
	memset(&tunnel_cfg, 0, sizeof(tunnel_cfg));
	tunnel_cfg.sk_user_data = ns;
	tunnel_cfg.encap_type = 1;
	tunnel_cfg.encap_rcv = nettlp_msg_rcv;
	tunnel_cfg.encap_destroy = NULL;
	tunnel_cfg.gro_receive = NULL;
	tunnel_cfg.gro_complete = NULL;

	setup_udp_tunnel_sock(&init_net, sock, &tunnel_cfg);

	nsock = ns;	/* XXX: terrified. used for only release */

	return 0;
}


void nettlp_msg_fini(void)
{
	if (!nsock) {
		pr_err("%s: NetTLP message socket is already released\n",
		       __func__);
		return;
	}

	udp_tunnel_sock_release(nsock->sock);
	kfree(nsock);
	nsock = NULL;
}
