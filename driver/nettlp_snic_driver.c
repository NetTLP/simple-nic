
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/ip_tunnels.h>

#include <nettlp_snic.h>


#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DRV_NAME		"nettlp_snic_driver"
#define NETTLP_SNIC_VERSION	"0.0.1"


/* netdev private date structure (netdev_priv). pci_drvdata is netdev */
struct nettlp_snic_adapter {

	struct pci_dev *pdev;
	struct net_device *dev;

	struct snic_bar4 *bar4;	/* ioremaped virt addr of BAR4 */
	struct snic_bar0 *bar0;	/* ioremaped virt addr of BAR0 */

	struct descriptor *tx_desc, *rx_desc;
	dma_addr_t tx_desc_paddr, rx_desc_paddr;

	struct interrupt *irq;
	dma_addr_t irq_paddr;
};



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
	adapter->bar4->tx_irq_ptr = adapter->irq_paddr;
	adapter->bar4->rx_irq_ptr = adapter->irq_paddr + 4;	/* 32bit irq */
	
	/* start RX thread */

	return 0;
}

static int nettlp_snic_stop(struct net_device *dev)
{
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);

	pr_info("%s\n", __func__);

	adapter->bar4->enabled = 0;
	adapter->bar4->tx_irq_ptr = 0;
	adapter->bar4->rx_irq_ptr = 0;

	/* stop RX thread */

	return 0;
}

static netdev_tx_t nettlp_snic_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	dma_addr_t dma;
	uint32_t pktlen;
	uint64_t timeout = 0;
	struct nettlp_snic_adapter *adapter = netdev_priv(dev);


	/* prepare descriptor */
	pktlen = skb->mac_len + skb->len;
	dma = dma_map_single(&adapter->pdev->dev, skb->data,
			     pktlen, DMA_FROM_DEVICE);
	adapter->tx_desc->addr = dma;
	adapter->tx_desc->length = pktlen;

	/* notify the device to start DMA */
	adapter->bar4->tx_desc_ptr = adapter->tx_desc_paddr;

	/* poll the tx interrupt intead of MSI */
#define POLL_TX_WAIT_USEC	100
#define POLL_TX_TIMEOUT		10000 /* 1sec */
	while (1) {
		udelay(POLL_TX_WAIT_USEC);

		if (adapter->irq->tx_irq)
			break;

		if (++timeout > POLL_TX_TIMEOUT) {
			net_warn_ratelimited("%s: tx timeout\n", __func__);
			break;
		}
	}

	adapter->irq->tx_irq = 0;

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
	void *bar4, *bar0;
	uint64_t bar4_start, bar4_len;
	uint64_t bar0_start, bar0_len;
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


	/* set BUS Master Mode */
	pci_set_master(pdev);

	/* setup struct netdevice */
	rc = -ENOMEM;
	dev = alloc_etherdev(sizeof(*adapter));
	if (!dev)
		goto err5;

	SET_NETDEV_DEV(dev, &pdev->dev);
	pci_set_drvdata(pdev, dev);
	adapter = netdev_priv(dev);
	adapter->dev = dev;
	adapter->pdev = pdev;
	adapter->bar4 = bar4;
	adapter->bar0 = bar0;
	
	/* allocate DMA region for descriptors and pseudo interrupts */
	adapter->tx_desc = dma_alloc_coherent(&pdev->dev,
					      sizeof(struct descriptor),
					      &adapter->tx_desc_paddr,
					      GFP_KERNEL);
	if (!adapter->tx_desc) {
		pr_err("%s: failed to alloc tx descriptor\n", __func__);
		goto err5;
	}

	adapter->rx_desc = dma_alloc_coherent(&pdev->dev,
					      sizeof(struct descriptor),
					      &adapter->rx_desc_paddr,
					      GFP_KERNEL);
	if (!adapter->rx_desc) {
		pr_err("%s: failed to alloc rx descriptor\n", __func__);
		goto err5;
	}

	adapter->irq = dma_alloc_coherent(&pdev->dev,
					  sizeof(struct interrupt),
					  &adapter->irq_paddr,
					  GFP_KERNEL);
	if (!adapter->rx_desc) {
		pr_err("%s: failed to alloc irq \n", __func__);
		goto err5;
	}


	snic_get_mac(dev->dev_addr, adapter->bar0->srcmac);
	dev->needs_free_netdev = true;
	dev->netdev_ops = &nettlp_snic_ops;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_MAX_MTU;
	/* XXX: should handle feature */

	rc = register_netdev(dev);
	if (rc)
		goto err6;

	return 0;

err6:
	dma_free_coherent(&pdev->dev, sizeof(struct descriptor),
			  (void *)adapter->tx_desc, adapter->tx_desc_paddr);
	dma_free_coherent(&pdev->dev, sizeof(struct descriptor),
			  (void *)adapter->rx_desc, adapter->rx_desc_paddr);
	dma_free_coherent(&pdev->dev, sizeof(struct interrupt),
			  (void *)adapter->irq, adapter->irq_paddr);
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

	unregister_netdev(dev);

	dma_free_coherent(&pdev->dev, sizeof(struct descriptor),
			  (void *)adapter->tx_desc, adapter->tx_desc_paddr);
	dma_free_coherent(&pdev->dev, sizeof(struct descriptor),
			  (void *)adapter->rx_desc, adapter->rx_desc_paddr);
	dma_free_coherent(&pdev->dev, sizeof(struct interrupt),
			  (void *)adapter->irq, adapter->irq_paddr);

	iounmap(adapter->bar4);
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
