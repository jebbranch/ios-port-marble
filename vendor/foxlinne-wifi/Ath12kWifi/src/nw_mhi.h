#ifndef NW_MHI_H
#define NW_MHI_H
#include "lx_os.h"
struct mhi_controller;
struct nw_mhi { struct device dev; struct mhi_controller *c; int mission, failed, last_cb; };
int nw_mhi_register(struct nw_mhi *n, void *bar, size_t bar_len, u32 msi_base, u64 dma_limit, const u8 *amss, size_t amss_len);
int nw_mhi_power_up(struct nw_mhi *n);
void nw_mhi_power_down(struct nw_mhi *n);
void nw_mhi_unregister(struct nw_mhi *n);
#endif
