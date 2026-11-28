/* Runs the MHI host sources over the runtime against a simulated WCN7850 boot ROM: PBL takes the first 512 KiB over BHI and becomes SBL,
 * SBL takes the vector table over BHIe and reports mission mode. Checks the host's sequence and the runtime's waits/work/interrupts. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "lx_os.h"
#include "lx_plat.h"
#include "nw_mhi.h"

#define MHIREGLEN 0x00
#define MHIVER 0x08
#define MHICFG 0x10
#define CHDBOFF 0x18
#define ERDBOFF 0x20
#define BHIOFF 0x28
#define BHIEOFF 0x2c
#define MHICTRL 0x38
#define MHISTATUS 0x48
#define ECABAP_LOWER 0x60
#define BHI_IMGADDR_LOW 0x08
#define BHI_IMGADDR_HIGH 0x0c
#define BHI_IMGSIZE 0x10
#define BHI_IMGTXDB 0x18
#define BHI_EXECENV 0x28
#define BHI_STATUS 0x2c

#define TXVECADDR_LOW 0x2c
#define TXVECADDR_HIGH 0x30
#define TXVECSIZE 0x34
#define TXVECDB 0x3c
#define TXVECSTATUS 0x44

static u8 g_bar[0x1000];
static u64 g_ms;
static const u8 *g_fw; static size_t g_fwsz;
static int g_stage, g_irqs_sent, g_sbl_sum_ok, g_vec_sum_ok;
static u32 *R(u32 off) { return (u32 *)(g_bar + off); }
static u32 g_ctl_prev;

static void ring_event(int ring, u32 dword0, u32 dword1) {
  u64 ec = ((u64)*R(ECABAP_LOWER + 4) << 32) | *R(ECABAP_LOWER);
  u8 *ctx = (u8 *)ec + ring * 44;
  u64 rbase, rp, rlen;
  memcpy(&rbase, ctx + 12, 8); memcpy(&rlen, ctx + 20, 8); memcpy(&rp, ctx + 28, 8);
  u32 *el = (u32 *)rp;
  el[0] = 0; el[1] = 0; el[2] = dword0; el[3] = dword1;
  rp += 16; if (rp >= rbase + rlen) rp = rbase;
  memcpy(ctx + 28, &rp, 8);
  lx_irq_raise(ring + 1);
  g_irqs_sent++;
}
#define EV_STATE_CHANGE(state) ((u32)(state) << 24), (0x20u << 16)
#define EV_EE(ee) ((u32)(ee) << 24), (0x40u << 16)

static void dev_poll(void) {
  if (!(*R(MHICTRL) & 2) && g_stage == 0) { /* host released reset */ }
  /* PBL: image sent over BHI */
  if (g_stage == 0 && *R(BHIOFF) && *R(BHIOFF + 0) && *R(0x124 + 0) == 0) { }
  u32 bhi = *R(BHIOFF);
  if (g_stage == 0 && *R(bhi + BHI_IMGTXDB)) {
    u64 a = ((u64)*R(bhi + BHI_IMGADDR_HIGH) << 32) | *R(bhi + BHI_IMGADDR_LOW); u32 sz = *R(bhi + BHI_IMGSIZE);
    g_sbl_sum_ok = sz == 0x80000 && !memcmp((void *)a, g_fw, sz);
    *R(bhi + BHI_STATUS) = 0x80000000u | 0x00005eed;  /* success */
    *R(bhi + BHI_EXECENV) = 1; g_stage = 1;
    *R(MHISTATUS) = 0;                  /* wait for host to program contexts and set ready */
    g_ms += 0;
  }
  if (g_stage == 1) { *R(MHISTATUS) = 1 | 0; g_stage = 2; }
  if (g_stage == 2 && (*R(MHICTRL) >> 8 & 0xff) == 2 /* M0 requested */) {
    *R(MHISTATUS) = 1 | (2 << 8); g_stage = 3;
    ring_event(0, EV_STATE_CHANGE(2));       /* M0 */
    ring_event(0, EV_EE(1));                 /* SBL */
  }
  u32 bhie = *R(BHIEOFF);
  if (g_stage == 3 && *R(bhie + TXVECDB)) {
    u64 t = ((u64)*R(bhie + TXVECADDR_HIGH) << 32) | *R(bhie + TXVECADDR_LOW); u32 sz = *R(bhie + TXVECSIZE);
    /* walk the table and compare with the image */
    size_t off = 0; int ok = sz && !(sz & 15);
    for (u32 i = 0; i + 16 <= sz && ok; i += 16) { u64 ea, es; memcpy(&ea, (u8 *)t + i, 8); memcpy(&es, (u8 *)t + i + 8, 8);
      size_t n = off + es > g_fwsz ? g_fwsz - off : es; if (memcmp((void *)ea, g_fw + off, n)) ok = 0; off += es; }
    g_vec_sum_ok = ok && off >= g_fwsz;
    *R(bhie + TXVECSTATUS) = (*R(bhie + TXVECDB) & 0x3fffffff) | (2u << 30);
    *R(BHI_EXECENV + bhi - BHI_EXECENV) = 0; *R(bhi + BHI_EXECENV) = 2; g_stage = 4;
    ring_event(0, EV_EE(2));                 /* AMSS */
  }
}
static u64 p_now(void) { return g_ms; }
static void p_sleep(unsigned ms) { g_ms += ms; }
static void p_log(int l, const char *s) { if (l <= 4 || getenv("V")) fprintf(stderr, "[%d] %s\n", l, s); }
static void p_halt(void) { abort(); }
static void *p_alloc(size_t n) { return malloc(n); }
static void p_free(void *p) { free(p); }
static void *p_dma_alloc(size_t n, dma_addr_t *b) { void *p = aligned_alloc(4096, (n + 4095) & ~4095UL); memset(p, 0, n); *b = (dma_addr_t)p; return p; }
static void p_dma_free(size_t n, void *p, dma_addr_t b) { (void)n; (void)b; free(p); }
static dma_addr_t p_dma_map(void *p, size_t n, int d) { (void)n; (void)d; return (dma_addr_t)p; }
static void p_dma_sync(dma_addr_t b, size_t n, int d) { (void)b; (void)n; (void)d; }
static int p_fw(const char *n, const u8 **d, size_t *s) { (void)n; *d = g_fw; *s = g_fwsz; return 0; }
static const struct lx_platform plat = { p_now, p_sleep, dev_poll, p_log, p_halt, p_alloc, p_free, p_dma_alloc, p_dma_free, p_dma_map, p_dma_sync, p_fw };

int mhi_init_module_for_test(void);
extern int mhi_init_hook(void);

int main(void) {
  g_fwsz = 3 * 512 * 1024 + 1000; u8 *fw = malloc(g_fwsz); for (size_t i = 0; i < g_fwsz; i++) fw[i] = (u8)(i * 7 + 3); g_fw = fw;
  *R(MHIREGLEN) = 0x1000; *R(MHIVER) = 0x01000000; *R(CHDBOFF) = 0x300; *R(ERDBOFF) = 0x600; *R(BHIOFF) = 0x100; *R(BHIEOFF) = 0x124;
  *R(0x100 + BHI_EXECENV) = 0;  /* PBL */
  lx_root_init();
  mhi_init_hook();
  lx_plat = &plat;
  static struct nw_mhi n;
  int r = nw_mhi_register(&n, g_bar, sizeof g_bar, 0, 0xffffffffull, fw, g_fwsz);
  printf("register: %d\n", r); if (r) return 1;
  r = nw_mhi_power_up(&n);
  printf("power_up: %d stage=%d sbl_ok=%d vec_ok=%d irqs=%d mission=%d time=%llums\n", r, g_stage, g_sbl_sum_ok, g_vec_sum_ok, g_irqs_sent, n.mission, (unsigned long long)g_ms);
  return !(r == 0 && g_stage == 4 && g_sbl_sum_ok && g_vec_sum_ok);
}
