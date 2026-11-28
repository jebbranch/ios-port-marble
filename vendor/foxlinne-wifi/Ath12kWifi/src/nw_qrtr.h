#ifndef NW_QRTR_H
#define NW_QRTR_H
#include "lx_net.h"
/* Where QRTR packets of the remote node go; the MHI channel provides it. */
typedef int (*nw_qrtr_xmit_fn)(void *ctx, const void *pkt, size_t len);
void nw_qrtr_init(void);
void nw_qrtr_set_endpoint(nw_qrtr_xmit_fn xmit, void *ctx);   /* sends our HELLO */
void nw_qrtr_rx(const void *pkt, size_t len);                 /* a packet from the remote node */
void nw_qrtr_endpoint_down(void);
int nw_qrtr_mhi_register(void);
void nw_qrtr_mhi_unregister(void);
#endif
