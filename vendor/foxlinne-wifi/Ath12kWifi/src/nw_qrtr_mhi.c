/* The QRTR endpoint on the MHI channel pair "IPCR": device-to-host buffers are kept queued and handed to the router, packets for the
 * device are queued on the host-to-device channel. */
#include "lx_os.h"
#include "linux/mhi.h"
#include "nw_qrtr.h"
#include "lx_plat.h"

struct ep { struct mhi_device *dev; };
static struct ep g_ep;

static void dl_cb(struct mhi_device *d, struct mhi_result *r) {
  if (r->transaction_status == -ENOTCONN) { lx_plat->dma_free(0, r->buf_addr, 0); return; }
  if (r->transaction_status) return;
  nw_qrtr_rx(r->buf_addr, r->bytes_xferd);
  int rc = mhi_queue_buf(d, DMA_FROM_DEVICE, r->buf_addr, d->mhi_cntrl->buffer_len, MHI_EOT);
  if (rc) lx_log(3, "qrtr: cannot recycle the receive buffer: %d", rc);
}
static void ul_cb(struct mhi_device *d, struct mhi_result *r) { (void)d; lx_plat->dma_free(0, r->buf_addr, 0); }
static int xmit(void *ctx, const void *pkt, size_t len) {
  struct ep *e = ctx; u64 bus; void *b = lx_plat->dma_alloc(len, &bus); if (!b) return -ENOMEM;
  memcpy(b, pkt, len);
  int rc = mhi_queue_buf(e->dev, DMA_TO_DEVICE, b, len, MHI_EOT);
  if (rc) lx_plat->dma_free(0, b, 0); return rc; }
static int probe(struct mhi_device *d, const struct mhi_device_id *id) {
  (void)id; g_ep.dev = d;
  int rc = mhi_prepare_for_transfer(d); if (rc) { lx_log(3, "qrtr: cannot start the IPCR channels: %d", rc); return rc; }
  for (int n = mhi_get_free_desc_count(d, DMA_FROM_DEVICE); n > 0; n--) {
    u64 bus; void *b = lx_plat->dma_alloc(d->mhi_cntrl->buffer_len, &bus); if (!b) { lx_log(3, "qrtr: out of memory"); return -ENOMEM; }
    rc = mhi_queue_buf(d, DMA_FROM_DEVICE, b, d->mhi_cntrl->buffer_len, MHI_EOT);
    if (rc) { lx_plat->dma_free(0, b, 0); lx_log(3, "qrtr: cannot queue a receive buffer: %d", rc); return rc; } }
  nw_qrtr_set_endpoint(xmit, &g_ep);
  return 0; }
static void removed(struct mhi_device *d) { nw_qrtr_endpoint_down(); mhi_unprepare_from_transfer(d); g_ep.dev = 0; }

static const struct mhi_device_id ids[] = { { .chan = "IPCR" }, {} };
static struct mhi_driver drv = { .id_table = ids, .probe = probe, .remove = removed, .ul_xfer_cb = ul_cb, .dl_xfer_cb = dl_cb, .driver = { .name = "nw_qrtr_mhi" } };
int nw_qrtr_mhi_register(void) { return mhi_driver_register(&drv); }
void nw_qrtr_mhi_unregister(void) { mhi_driver_unregister(&drv); }
