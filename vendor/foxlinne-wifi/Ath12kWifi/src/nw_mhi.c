/* The WCN7850's MHI controller: describes the device's channels and event rings to the MHI host and starts it. */
#include "lx_os.h"
#include "linux/mhi.h"
#include "nw_mhi.h"


/* Two channels carry the QRTR control traffic (host-to-device 20, device-to-host 21). */
static const struct mhi_channel_config nw_channels[] = {
  { .num = 20, .name = "IPCR", .num_elements = 64, .event_ring = 1, .dir = DMA_TO_DEVICE, .ee_mask = 0x4, .doorbell = MHI_DB_BRST_DISABLE },
  { .num = 21, .name = "IPCR", .num_elements = 64, .event_ring = 1, .dir = DMA_FROM_DEVICE, .ee_mask = 0x4, .doorbell = MHI_DB_BRST_DISABLE },
};
/* Ring 0 takes the device's state changes, ring 1 the channel traffic. */
static struct mhi_event_config nw_events[] = {
  { .num_elements = 32, .irq_moderation_ms = 0, .irq = 1, .mode = MHI_DB_BRST_DISABLE, .data_type = MHI_ER_CTRL },
  { .num_elements = 256, .irq_moderation_ms = 1, .irq = 2, .mode = MHI_DB_BRST_DISABLE, .priority = 1 },
};
static const struct mhi_controller_config nw_config = {
  .max_channels = 128, .timeout_ms = 2000, .buf_len = 8192,
  .num_channels = ARRAY_SIZE(nw_channels), .ch_cfg = nw_channels,
  .num_events = ARRAY_SIZE(nw_events), .event_cfg = nw_events,
};

static int op_runtime_get(struct mhi_controller *c) { (void)c; return 0; }
static void op_runtime_put(struct mhi_controller *c) { (void)c; }
static void op_status_cb(struct mhi_controller *c, enum mhi_callback cb) {
  struct nw_mhi *n = dev_get_drvdata(c->cntrl_dev);
  n->last_cb = cb;
  /* the bulk image stays: the firmware pages its code in from it on demand for as long as it runs */
  if (cb == MHI_CB_EE_MISSION_MODE) n->mission = 1;
  if (cb == MHI_CB_SYS_ERROR || cb == MHI_CB_FATAL_ERROR || cb == MHI_CB_EE_RDDM) n->failed = 1;
}
static int op_read_reg(struct mhi_controller *c, void __iomem *a, u32 *out) { (void)c; *out = readl(a); return 0; }
static void op_write_reg(struct mhi_controller *c, void __iomem *a, u32 v) { (void)c; writel(v, a); }

int nw_mhi_register(struct nw_mhi *n, void *bar, size_t bar_len, u32 msi_base, u64 dma_limit,
                    const u8 *amss, size_t amss_len)
{
  struct mhi_controller *c = mhi_alloc_controller();
  if (!c) return -ENOMEM;
  memset(&n->dev, 0, sizeof n->dev);
  device_initialize(&n->dev);
  n->dev.name = "wcn7850"; n->dev.drvdata = n; n->dev.dma_mask = dma_limit;
  n->c = c;
  c->cntrl_dev = &n->dev;
  c->regs = bar; c->reg_len = bar_len;
  c->fw_data = amss; c->fw_sz = amss_len;
  c->irq = lx_alloc(sizeof(int) * (nw_config.num_events + 1), 1);
  if (!c->irq) { mhi_free_controller(c); return -ENOMEM; }
  c->nr_irqs = nw_config.num_events + 1;     /* vector 0 carries the device status, then one per event ring */
  for (u32 i = 0; i < c->nr_irqs; i++) c->irq[i] = msi_base + i;
  c->iova_start = 0; c->iova_stop = dma_limit;
  c->sbl_size = SZ_512K; c->seg_len = SZ_512K; c->fbc_download = true;
  c->runtime_get = op_runtime_get; c->runtime_put = op_runtime_put; c->status_cb = op_status_cb;
  c->read_reg = op_read_reg; c->write_reg = op_write_reg;
  int r = mhi_register_controller(c, &nw_config);
  if (r) { lx_free(c->irq); mhi_free_controller(c); n->c = 0; }
  return r;
}

/* Starts the device: loads the secondary bootloader over BHI, the rest over BHIe, and returns once the firmware runs (or failed). */
int nw_mhi_power_up(struct nw_mhi *n)
{
  int r = mhi_prepare_for_power_up(n->c);
  if (r) return r;
  return mhi_sync_power_up(n->c);
}
void nw_mhi_power_down(struct nw_mhi *n) { if (n->c) mhi_power_down(n->c, false); }
void nw_mhi_unregister(struct nw_mhi *n) {
  if (!n->c) return; mhi_unregister_controller(n->c); lx_free(n->c->irq); mhi_free_controller(n->c); n->c = 0; }

/* The boot loader of the chip answered the image with an error code when it was offered right after the reset with no MHI contexts programmed.
 * The loader that worked on this board waited, then wrote the context registers, then offered the image: do the same (the registers are written
 * again after the loader's start, as the host's own sequence does). */
#ifndef NW_BOOT_SETTLE_MS
#define NW_BOOT_SETTLE_MS 200
#endif
int mhi_init_mmio(struct mhi_controller *mhi_cntrl);
void nw_mhi_pre_load(struct mhi_controller *c) {
  static int done; if (done) return; done = 1;
  lx_delay_ms(NW_BOOT_SETTLE_MS);
  int r = mhi_init_mmio(c);
  lx_log(6, "mhi: contexts programmed before the image: %d", r);
}
