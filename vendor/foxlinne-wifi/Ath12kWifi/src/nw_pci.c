/* The PCI endpoint for the WiFi driver, over the board's PCIe0 access: config space through a platform hook, the register window mapped by
 * the board, message interrupts raised into the dispatcher. */
#include "lx_pci.h"
#include "lx_plat.h"
#include "nw_board.h"

const struct nw_board *nw_board;
struct atomic_notifier_head lx_panic_list;
static struct pci_driver *g_drv;
static struct pci_dev g_dev;
static struct msi_desc g_msi[32];
static struct pci_bus g_bus = { 1 };

int lx_pci_read_config(struct pci_dev *d, int off, int size, u32 *v) { (void)d; return nw_board->cfg_read(off, size, v); }
int lx_pci_write_config(struct pci_dev *d, int off, int size, u32 v) { (void)d; return nw_board->cfg_write(off, size, v); }
int pcie_capability_clear_and_set_word(struct pci_dev *d, int off, u16 clear, u16 set) {
  u32 v; int r = lx_pci_read_config(d, 0x80000000 | off, 2, &v); if (r) return r;
  v = (v & ~(u32)clear) | set; return lx_pci_write_config(d, 0x80000000 | off, 2, v); }
int pci_enable_device(struct pci_dev *d) { (void)d; return 0; }
void pci_disable_device(struct pci_dev *d) { (void)d; }
int pci_is_enabled(struct pci_dev *d) { (void)d; return 1; }
void pci_set_master(struct pci_dev *d) { (void)d; }
int pci_request_region(struct pci_dev *d, int bar, const char *n) { (void)d; (void)bar; (void)n; return 0; }
void pci_release_region(struct pci_dev *d, int bar) { (void)d; (void)bar; }
int pci_assign_resource(struct pci_dev *d, int bar) { (void)d; (void)bar; return 0; }
void *pci_iomap(struct pci_dev *d, int bar, unsigned long max) { (void)d; (void)bar; (void)max; return nw_board->bar; }
void pci_iounmap(struct pci_dev *d, void *p) { (void)d; (void)p; }
unsigned long lx_pci_resource_len(struct pci_dev *d, int bar) { (void)d; (void)bar; return nw_board->bar_len; }
void *lx_ioremap(phys_addr_t pa, size_t n) { (void)n; return nw_board->map_phys ? nw_board->map_phys(pa, n) : (void *)(unsigned long)pa; }

/* MSI: the board programs the endpoint's MSI capability and tells where a vector lands; vectors 0..n-1 become irq numbers base..base+n-1 */
int pci_alloc_irq_vectors(struct pci_dev *d, unsigned int min, unsigned int max, unsigned int flags) {
  (void)flags; unsigned int n = max < 32 ? max : 32; if (n < min) return -ENOSPC;
  u32 lo, hi, data;
  if (nw_board->msi_target(&lo, &hi, &data)) return -ENOSPC;
  for (unsigned int i = 0; i < n; i++) { g_msi[i].msg.address_lo = lo; g_msi[i].msg.address_hi = hi; g_msi[i].msg.data = data + i; g_msi[i].pci.msi_attrib.is_64 = 1; }
  /* program the endpoint's MSI capability: target address, base data, number of vectors enabled, enable */
  int mme = 0; while ((1u << mme) < n) mme++;
  u32 ctl; nw_board->cfg_read(nw_board->msi_cap, 4, &ctl);
  int is64 = (ctl >> 23) & 1;
  nw_board->cfg_write(nw_board->msi_cap + 4, 4, lo);
  if (is64) { nw_board->cfg_write(nw_board->msi_cap + 8, 4, hi); nw_board->cfg_write(nw_board->msi_cap + 12, 4, data); }
  else nw_board->cfg_write(nw_board->msi_cap + 8, 4, data);
  ctl = (ctl & 0xff8fffffu) | ((u32)mme << 20) | (1u << 16);
  nw_board->cfg_write(nw_board->msi_cap, 4, ctl);
  d->nvec = n; d->msi_enabled = 1; d->irq = nw_board->irq_base; return n; }
void pci_free_irq_vectors(struct pci_dev *d) { d->nvec = 0; d->msi_enabled = 0; }
int pci_irq_vector(struct pci_dev *d, unsigned int nr) { (void)d; return nw_board->irq_base + nr; }
struct msi_desc *irq_get_msi_desc(unsigned int irq) { unsigned int i = irq - nw_board->irq_base; return i < 32 ? &g_msi[i] : 0; }

/* the driver is probed once, with the endpoint the board found */
static u32 sub_ctl_after;
int pci_register_driver(struct pci_driver *drv) {
  g_drv = drv;
  memset(&g_dev, 0, sizeof g_dev);
  g_dev.vendor = 0x17cb; g_dev.device = nw_board->device_id; g_dev.dev.name = "wlan0pci"; g_dev.dev.dma_mask = 0xffffffffull;
  device_initialize(&g_dev.dev);
  g_dev.msi_cap = nw_board->msi_cap; g_dev.bus = &g_bus;
  { u32 sub = 0; if (!nw_board->cfg_read(0x2c, 4, &sub)) { g_dev.subsystem_vendor = sub & 0xffff; g_dev.subsystem_device = sub >> 16; } }   /* the board file is picked by these */
  /* The endpoint reads host memory in the largest pieces its device control register allows: ask for the smallest (128 bytes). */
  { u32 ctl = 0; if (!nw_board->cfg_read(0x80000008, 2, &ctl)) { nw_board->cfg_write(0x80000008, 2, ctl & ~0x7000u); nw_board->cfg_read(0x80000008, 2, &sub_ctl_after); lx_log(6, "pci: device control %04x -> %04x", ctl, sub_ctl_after); } }
  for (const struct pci_device_id *id = drv->id_table; id && id->vendor; id++)
    if (id->device == g_dev.device) return drv->probe(&g_dev, id);
  return -ENODEV; }
void pci_unregister_driver(struct pci_driver *drv) { if (g_drv == drv) { drv->remove(&g_dev); g_drv = 0; } }
