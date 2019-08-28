
#ifndef _NETTLP_MSG_H_
#define _NETTLP_MSG_H_


#define NETTLP_MSG_PORT 12287   /* NETTLP_PORT_BASE - 1 */

/* request types */
#define NETTLP_MSG_GET_BAR4_ADDR        1
#define NETTLP_MSG_GET_DEV_ID		2
#define NETTLP_MSG_GET_MSIX_TABLE       3


/* NETTLP_MSG_GET_BAR4_ADDR */
struct nettlp_msg_bar4_addr {
        uint64_t        addr;
};

/* NETTLP_MSG_GET_BUSN */
struct nettlp_msg_id {
	uint16_t	id;	/* PCI device ID (requester/completer Id) */
};

/* NETTLP_MSG_GET_MSIX_TABLE */

#define NETTLP_MAX_VEC  16
#define NETTLP_NUM_VEC  4

struct nettlp_msix {
	uint64_t addr;
	uint32_t data;
} __attribute__((__packed__));

struct nettlp_msg_msix_table {
	struct nettlp_msix msix[NETTLP_MAX_VEC];
};



int nettlp_msg_init(uint64_t bar4_start, uint16_t dev_id, void *bar2_virt);
void nettlp_msg_fini(void);

#endif 
