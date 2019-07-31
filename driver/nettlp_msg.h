
#ifndef _NETTLP_MSG_H_
#define _NETTLP_MSG_H_


#define NETTLP_MSG_PORT 12287   /* NETTLP_PORT_BASE - 1 */

/* request types */
#define NETTLP_MSG_GET_BAR4_ADDR        1
#define NETTLP_MSG_GET_MSIX_TABLE       2


/* NETTLP_MSG_GET_BAR4_ADDR */
struct nettlp_msg_bar4_addr {
        uint64_t        addr;
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


int nettlp_msg_init(uint64_t bar4_start, void *bar2_virt);
void nettlp_msg_fini(void);

#endif 
