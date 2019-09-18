
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/ip_tunnels.h>

#include <nettlp_snic.h>
#include "nettlp_msg.h"


#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DRV_NAME		"nettlp_snic_driver"
#define NETTLP_SNIC_VERSION	"0.0.1"

#define SNIC_DESC_RING_LEN	1


/* netdev private date structure (netdev_priv). pci_drvdata is netdev */
struct nettlp_snic_adapter {

	struct pci_dev *pdev;
	struct net_device *dev;

	struct snic_bar4 *bar4;	/* ioremaped virt addr of BAR4 */
	struct snic_bar0 *bar0;	/* ioremaped virt addr of BAR0 */
	void *bar2;	/* ioremapped BAR2 for MSIX */

	struct descriptor *tx_desc;	/* base of TX descriptors */
	struct descriptor *rx_desc;	/* base of RX descriptors */
	dma_addr_t tx_desc_paddr;	/* phy addr of tx_desc */
	dma_addr_t rx_desc_paddr;	/* phy addr of rx_desc */

	struct rx_buf *rx_bufs;		/* rx packet buffers */

	uint32_t	tx_desc_idx;	/* index of TX desc to be sent */
	uint32_t	rx_desc_idx;	/* index of RX desc for next buf 
					 * XXX: in simple NIC, always 0.
					 */

	spinlock_t	tx_lock;
	int		tx_state;
#define TX_STATE_READY	1
#define TX_STATE_BUSY	2
	/* when tx_state is ready, start xmit and set tx_tate busy.
	 * tx_irq_handler set tx_state ready.
	 */

	spinlock_t	rx_lock;
	struct tasklet_struct	*rx_tasklet;

	/* one rx packet buffer */
	void		*rx_buf;
	dma_addr_t	rx_buf_paddr;
};

#define next_index(idx) (((idx + 1) % SNIC_DESC_RING_LEN) - 1)


#if 0
static int nettlp_setup_rx_buf(struct net_device *dev)
{
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);

	/* allocate first rx packet buffer */
	adapter->rx_skb = netdev_alloc_skb(dev, 2048);
	if (!adapter->rx_skb) {
		pr_err("%s: failed to alloc skb\n", __func__);
		return -ENOMEM;
	}
	adapter->rx_desc->addr = dma_map_single(&adapter->pdev->dev,
						skb_mac_header(adapter->rx_skb),
						2048, DMA_FROM_DEVICE);
	adapter->rx_desc->length = 2048;

	return 0;
}
#endif


void rx_tasklet(unsigned long data)
{
	unsigned long flags;
	struct nettlp_snic_adapter *adapter =
		(struct nettlp_snic_adapter *)data;
	struct sk_buff *skb;

	pr_err("rx_tasklet!\n");

	spin_lock_irqsave(&adapter->rx_lock, flags);


	/*
	 * RX interrupt means DMA to the rx buffer is done.
	 * finish building rx skb and receive it to upper protocols.
	 */
	pr_info("%s: start\n", __func__);

	dma_unmap_single(&adapter->pdev->dev, adapter->rx_desc->addr, 2048,
			 DMA_FROM_DEVICE);

	pr_info("%s: packet length is %llu\n", __func__,
		adapter->rx_desc->length);

	pr_info("%s: build rx skb like sume\n", __func__);

	skb = netdev_alloc_skb_ip_align(adapter->dev,
					adapter->rx_desc->length +
					NET_IP_ALIGN);
	if (!skb) {
		adapter->dev->stats.rx_dropped++;
		pr_err("%s: failed to allocate rx skb\n", __func__);
		goto out;
	}

	skb_copy_to_linear_data_offset(skb, NET_IP_ALIGN,
				       adapter->rx_buf,
				       adapter->rx_desc->length);
	skb_put(skb, adapter->rx_desc->length);
	skb->protocol = eth_type_trans(skb, adapter->dev);
	skb->ip_summed = CHECKSUM_NONE;

	pr_info("%s: go netif_rx\n", __func__);
	netif_rx(skb);
	adapter->dev->stats.rx_packets++;
	adapter->dev->stats.rx_bytes += adapter->rx_desc->length;

	/* notify new rx skb by DMA write rx_desc address  */
	pr_info("%s: notify the new desc to libtlp\n", __func__);
	adapter->rx_desc->addr = adapter->rx_buf_paddr;
	adapter->bar4->rx_desc_idx = adapter->rx_desc_idx;

	pr_info("%s: done\n", __func__);


out:
	spin_unlock_irqrestore(&adapter->rx_lock, flags);

	return;
}

static irqreturn_t rx_handler(int irq, void *nic_irq)
{
	unsigned long flags;
	struct nettlp_snic_adapter *adapter = nic_irq;

	spin_lock_irqsave(&adapter->rx_lock, flags);

	pr_err("rx interrupt! irq=%d\n", irq);
	tasklet_schedule(adapter->rx_tasklet);

	spin_unlock_irqrestore(&adapter->rx_lock, flags);

	return IRQ_HANDLED;
}



static int nettlp_snic_init(struct net_device *dev)
{
	pr_info("%s\n", __func__);

	/* setup counters */
	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;
	return 0;
}

static void nettlp_snic_uninit(struct net_device *dev)
{
	pr_info("%s\n", __func__);

	/* free counters */
	free_percpu(dev->tstats);
}

static int nettlp_snic_open(struct net_device *dev)
{
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);

	pr_info("%s\n", __func__);
	adapter->bar4->enabled = 1;
	adapter->tx_state = TX_STATE_READY;

	/* prepare desc and notify rx descriptor to device */
	adapter->rx_desc->addr = adapter->rx_buf_paddr;
	adapter->bar4->rx_desc_idx = adapter->rx_desc_idx;

	return 0;
}

static int nettlp_snic_stop(struct net_device *dev)
{
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);

	pr_info("%s\n", __func__);
	adapter->bar4->enabled = 0;

	tasklet_kill(adapter->rx_tasklet);

	return 0;
}

static irqreturn_t tx_handler(int irq, void *nic_irq)
{
	unsigned long flags;
	struct nettlp_snic_adapter *adapter = nic_irq;

	pr_info("tx interrupt! irq=%d\n", irq);

	spin_lock_irqsave(&adapter->tx_lock, flags);

	if (adapter->tx_state != TX_STATE_BUSY)
		goto out;

	adapter->tx_state = TX_STATE_READY;
out:
	spin_unlock_irqrestore(&adapter->tx_lock, flags);

	return IRQ_HANDLED;
}



static netdev_tx_t nettlp_snic_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	dma_addr_t dma;
	uint32_t pktlen;
	unsigned long flags;
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);
	struct descriptor *tx_desc;


	/* the current implementation allows only a single CPU can
	 * start TX */
	spin_lock_irqsave(&adapter->tx_lock, flags);

	tx_desc = adapter->tx_desc + adapter->tx_desc_idx;

	if (adapter->tx_state != TX_STATE_READY) {
		adapter->dev->stats.tx_dropped++;
		spin_unlock_irqrestore(&adapter->tx_lock, flags);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	adapter->tx_state = TX_STATE_BUSY;

	/* prepare the tx descriptor */
	pktlen = skb->len;
	dma = dma_map_single(&adapter->pdev->dev, skb_mac_header(skb),
			     pktlen, DMA_TO_DEVICE);
	pr_info("%s: skb dma addr is %#llx\n", __func__, dma);
	tx_desc->addr = dma;
	tx_desc->length = pktlen;

	/* notify the device to start DMA */
	adapter->bar4->tx_desc_idx = adapter->tx_desc_idx;
	adapter->dev->stats.tx_packets++;
	adapter->dev->stats.tx_bytes += pktlen;

	dev_kfree_skb_any(skb);

	adapter->tx_desc_idx = next_index(adapter->tx_desc_idx);

	spin_unlock_irqrestore(&adapter->tx_lock, flags);

	pr_info("%s done\n", __func__);

	return NETDEV_TX_OK;
}

static int nettlp_snic_set_mac(struct net_device *dev, void *p)
{
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	snic_get_mac(adapter->bar0->srcmac, dev->dev_addr);

	return 0;
}

/* netdevice ops */
static const struct net_device_ops nettlp_snic_ops = {
	.ndo_init		= nettlp_snic_init,
	.ndo_uninit		= nettlp_snic_uninit,
	.ndo_open		= nettlp_snic_open,
	.ndo_stop		= nettlp_snic_stop,
	.ndo_start_xmit		= nettlp_snic_xmit,
	.ndo_get_stats64	= ip_tunnel_get_stats64, /* ??? */
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= nettlp_snic_set_mac,
};




static int nettlp_register_interrupts(struct nettlp_snic_adapter *adapter)
{
	int ret;

	// Enable MSI-X
	ret = pci_alloc_irq_vectors(adapter->pdev, 2, 2, PCI_IRQ_MSIX);
	if (ret < 0) {
		pr_info("Request for #%d msix vectors failed, returned %d\n",
			NETTLP_NUM_VEC, ret);
		return 1;
	}

	// register interrupt handler 
	ret = request_irq(pci_irq_vector(adapter->pdev, 0),
			  tx_handler, 0, DRV_NAME, adapter);
	if (ret) {
		pr_err("%s: failed to register TX IRQ\n", __func__);
		return 1;
	}

	ret = request_irq(pci_irq_vector(adapter->pdev, 1),
			  rx_handler, 0, DRV_NAME, adapter);
	if (ret) {
		pr_err("%s: failed to register RX IRQ\n", __func__);
		return 1;
	}

	return 0;
}

static void nettlp_unregister_interrupts(struct nettlp_snic_adapter *adapter)
{
	free_irq(pci_irq_vector(adapter->pdev, 0), adapter);
	free_irq(pci_irq_vector(adapter->pdev, 1), adapter);
}

/* this it the identical device id with the original NetTLP driver */
static const struct pci_device_id nettlp_snic_pci_tbl[] = {
        {0x3776, 0x8022, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        {0,}
};
MODULE_DEVICE_TABLE(pci, nettlp_snic_pci_tbl);

static int nettlp_snic_pci_init(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	int rc;
	void *bar4, *bar2, *bar0;
	uint64_t bar4_start, bar4_len;
	uint64_t bar0_start, bar0_len;
	uint64_t bar2_start, bar2_len;
	struct nettlp_snic_adapter *adapter;
	struct net_device *dev;

	pr_info("%s: register nettlp simple nic device %s\n",
		__func__, pci_name(pdev));

	rc = pci_enable_device(pdev);
	if (rc)
		goto err1;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto err2;

	/* map BAR4 */
	bar4_start = pci_resource_start(pdev, 4);
	bar4_len = pci_resource_len(pdev, 4);
	bar4 = ioremap(bar4_start, bar4_len);
	if (!bar4) {
		pr_err("failed to ioremap BAR4 %#llx\n", bar4_start);
		goto err3;
	}
	pr_info("BAR4 %#llx is mapped to %p\n", bar4_start, bar4);

	/* map BAR0 */
	bar0_start = pci_resource_start(pdev, 0);
	bar0_len = pci_resource_len(pdev, 0);
	bar0 = ioremap(bar0_start, bar0_len);
	if (!bar0) {
		pr_err("failed to ioremap BAR0 %#llx\n", bar0_start);
		goto err4;
	}
	pr_info("BAR0 %#llx is mapped to %p\n", bar0_start, bar0);

	/* marp BAR2 */
	bar2_start = pci_resource_start(pdev, 2);
	bar2_len = pci_resource_len(pdev, 2);
	bar2 = ioremap(bar2_start, bar2_len);
	if (!bar2) {
		pr_err("failed to ioremap BAR2 %#llx\n", bar2_start);
		goto err5;
	}
	pr_info("BAR2 %#llx is mamped to %p\n", bar2_start, bar2);



	/* set BUS Master Mode */
	pci_set_master(pdev);

	/* setup struct netdevice */
	rc = -ENOMEM;
	dev = alloc_etherdev(sizeof(*adapter));
	if (!dev)
		goto err6;

	SET_NETDEV_DEV(dev, &pdev->dev);
	pci_set_drvdata(pdev, dev);
	adapter = netdev_priv(dev);
	adapter->dev = dev;
	adapter->pdev = pdev;
	adapter->bar4 = bar4;
	adapter->bar0 = bar0;
	adapter->bar2 = bar2;
	
	/* allocate DMA region for descriptors and pseudo interrupts */
	adapter->tx_desc = dma_alloc_coherent(&pdev->dev,
					      sizeof(struct descriptor) *
					      SNIC_DESC_RING_LEN,
					      &adapter->tx_desc_paddr,
					      GFP_KERNEL);
	if (!adapter->tx_desc) {
		pr_err("%s: failed to alloc tx descriptor\n", __func__);
		goto err6;
	}

	adapter->rx_desc = dma_alloc_coherent(&pdev->dev,
					      sizeof(struct descriptor) *
					      SNIC_DESC_RING_LEN,
					      &adapter->rx_desc_paddr,
					      GFP_KERNEL);
	if (!adapter->rx_desc) {
		pr_err("%s: failed to alloc rx descriptor\n", __func__);
		goto err6;
	}

	adapter->rx_buf = dma_alloc_coherent(&pdev->dev, 2048,
					     &adapter->rx_buf_paddr,
					     GFP_KERNEL);
	if (!adapter->rx_desc) {
		pr_err("%s: failed to alloc rx buffer\n", __func__);
		goto err6;
	}

	spin_lock_init(&adapter->tx_lock);
	spin_lock_init(&adapter->rx_lock);

	snic_get_mac(dev->dev_addr, adapter->bar0->srcmac);
	dev->needs_free_netdev = true;
	dev->netdev_ops = &nettlp_snic_ops;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_MAX_MTU;
	/* XXX: should handle feature */

	rc = register_netdev(dev);
	if (rc)
		goto err7;

	/* initialize tasklet for RX interrupt */
	adapter->rx_tasklet = kmalloc(sizeof(struct tasklet_struct),
				      GFP_KERNEL);
	if (!adapter->rx_tasklet) {
		rc = -ENOMEM;
		goto err8;
	}
	tasklet_init(adapter->rx_tasklet, rx_tasklet, (unsigned long)adapter);

	/* register irq */
	rc = nettlp_register_interrupts(adapter);
	if (rc)
		goto err9;

	/* initialize nettlp_msg module */
	nettlp_msg_init(bar4_start,
			PCI_DEVID(pdev->bus->number, pdev->devfn),
			bar2);

	/* initialize base addresses for descriptor and indexes */
	adapter->bar4->tx_desc_base = (uintptr_t)adapter->tx_desc;
	adapter->bar4->rx_desc_base = (uintptr_t)adapter->rx_desc;
	adapter->tx_desc_idx = 0;
	adapter->rx_desc_idx = 0;

	pr_info("%s: probe finished.", __func__);
	pr_info("%s: tx desc is %#llx, rx desc is %#llx\n", __func__,
		adapter->tx_desc_paddr, adapter->rx_desc_paddr);

	return 0;



err9:
	kfree(adapter->rx_tasklet);
err8:
	unregister_netdev(dev);
err7:
	dma_free_coherent(&pdev->dev,
			  sizeof(struct descriptor) * SNIC_DESC_RING_LEN,
			  (void *)adapter->tx_desc, adapter->tx_desc_paddr);
	dma_free_coherent(&pdev->dev,
			  sizeof(struct descriptor) * SNIC_DESC_RING_LEN,
			  (void *)adapter->rx_desc, adapter->rx_desc_paddr);
err6:
	iounmap(bar2);
err5:
	iounmap(bar0);
err4:
	iounmap(bar4);
err3:
	pci_release_regions(pdev);
err2:
	pci_disable_device(pdev);
err1:

	return rc;
}

static void nettlp_snic_pci_remove(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);

	pr_info("%s\n", __func__);

	kfree(adapter->rx_tasklet);

	nettlp_msg_fini();
	nettlp_unregister_interrupts(adapter);
	pci_free_irq_vectors(pdev);

	unregister_netdev(dev);

	dma_free_coherent(&pdev->dev,
			  sizeof(struct descriptor) * SNIC_DESC_RING_LEN,
			  (void *)adapter->tx_desc, adapter->tx_desc_paddr);
	dma_free_coherent(&pdev->dev,
			  sizeof(struct descriptor) * SNIC_DESC_RING_LEN,
			  (void *)adapter->rx_desc, adapter->rx_desc_paddr);
	dma_free_coherent(&pdev->dev, 2048, (void *)adapter->rx_buf,
			  adapter->rx_buf_paddr);

	iounmap(adapter->bar4);
	iounmap(adapter->bar2);
	iounmap(adapter->bar0);
	
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	return;
}


struct pci_driver nettlp_snic_pci_driver = {
        .name = DRV_NAME,
        .id_table = nettlp_snic_pci_tbl,
        .probe = nettlp_snic_pci_init,
        .remove = nettlp_snic_pci_remove,
};


static int __init nettlp_snic_module_init(void)
{
	pr_info("nettlp_snic (v%s) is loaded\n", NETTLP_SNIC_VERSION);
	return pci_register_driver(&nettlp_snic_pci_driver);
}
module_init(nettlp_snic_module_init);

static void __exit nettlp_snic_module_exit(void)
{
	pci_unregister_driver(&nettlp_snic_pci_driver);
	pr_info("nettlp_snic (v%s) is unloaded\n", NETTLP_SNIC_VERSION);
	return;
}
module_exit(nettlp_snic_module_exit);

MODULE_AUTHOR("Ryo Nakamura <upa@haeena.net>");
MODULE_DESCRIPTION("nettlp_snic");
MODULE_LICENSE("GPL");
MODULE_VERSION(NETTLP_SNIC_VERSION);
