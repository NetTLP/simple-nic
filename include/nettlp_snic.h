
/* nettlp_simple_nic.h */

#ifndef _NETTLP_SIMPLE_NIC_H_
#define _NETTLP_SIMPLE_NIC_H_

/* The simple nic model is described in the paper, Rolf Neugebauer, et
 * al, PCIe performance for end host networking in SIGCOMM 2018. And,
 * this source code implements the simple nic model as a pseudo device
 * implemented in software, but it can co-exist with actual pyhsical
 * chipsets by using NetTLP.
 *
 * The simple nic behavior is described in
 * https://github.com/pcie-bench/pcie-model/blob/master/model/simple_nic.py
 *
 * The step-by-step procedure shown below is quoted from the source code.

    TX from the host:
    1. Host updates the TX queue tail pointer            (PCIe write: rx)
    2. Device DMAs descriptor                            (PCIe read:  rx/tx)
    3. Device DMAs packet content                        (PCIe read:  rx/tx)
    4. Device generates interrupt                        (PCIe write: tx)
    5. Host reads TX queue head pointer                  (PCIe read:  rx/tx)

    RX to the host:
    1. Host updates RX Queue Tail Pointer -> free buf    (PCIe write: rx)
    2. Device DMAs descriptor from host                  (PCIe read:  rx/tx)
    3. Device DMAs packet to host                        (PCIe write: tx)
    4. Device writes back RX descriptor                  (PCIe write: tx)
    5. Device generates interrupt                        (PCIe write: tx)
    6. Host reads RX queue head pointer                  (PCIe read:  rx/tx)

    We assume these steps are performed for every packet.

 *
 */

/*
 * BAR4 layout of NetTLP device.
 */
struct snic_bar4 {

	/* pseudo device process watches the ptrs to start DMA for
	 * TX and RX packets */
	uint64_t tx_desc_ptr;	/* address of TX descriptor to be sent */
	uint64_t rx_desc_ptr;	/* address of RX descriptor to receive */

	uint32_t enabled;	/* if 1, device enabled by driver */
} __attribute__((packed));


/* packet descriptor */
struct descriptor {
	uint64_t addr;
	uint64_t length; 
} __attribute__((packed));

/*
 * BAR0 layout for configuration
 */
struct snic_bar0 {
	uint32_t magic;		/* 0x67 45 23 01 */

	uint8_t dstmac[6];
	uint16_t rsv1;

	uint8_t srcmac[6];
	uint16_t rsv2;
	
	__be32 srcip;
	__be32 dstip;
};

#define snic_get_mac(dst, src) do {			\
		dst[0] = src[5];		\
		dst[1] = src[4];		\
		dst[2] = src[3];		\
		dst[3] = src[2];		\
		dst[4] = src[1];		\
		dst[5] = src[0];		\
	}while(0)


#endif /*  _NETTLP_SIMPLE_NIC_H_ */
