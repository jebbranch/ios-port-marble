/* Starts the whole stack on a board whose register window is plain memory: nothing answers, so the driver has to time out cleanly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_os.h"
#include "lx_plat.h"
#include "nw_board.h"
#include "nw_wifi.h"

static u64 g_ms; static unsigned char *g_bar;
static u64 p_now(void) { return g_ms; }
static void p_sleep(unsigned ms) { g_ms += ms; }
static void p_poll(void) {}
static void p_log(int l, const char *s) { if (getenv("V") || l <= 4) printf("[%d] %s\n", l, s); }
static void p_halt(void) { abort(); }
static void *p_alloc(size_t n) { return malloc(n); }
static void p_free(void *p) { free(p); }
static void *p_dma_alloc(size_t n, dma_addr_t *b) { void *p = aligned_alloc(4096, (n + 4095) & ~4095UL); memset(p, 0, n); *b = (dma_addr_t)p & 0xffffffff; return p; }
static void p_dma_free(size_t n, void *p, dma_addr_t b) { (void)n; (void)b; free(p); }
static dma_addr_t p_dma_map(void *p, size_t n, int d) { (void)n; (void)d; return (dma_addr_t)p; }
static void p_dma_sync(dma_addr_t b, size_t n, int d) { (void)b; (void)n; (void)d; }
static int p_fw(const char *n, const u8 **d, size_t *s) { static u8 blob[4096]; if (getenv("V")) printf("[fw] %s\n", n); if (!strstr(n, "bin")) return -1; *d = blob; *s = sizeof blob; return 0; }
static const struct lx_platform plat = { p_now, p_sleep, p_poll, p_log, p_halt, p_alloc, p_free, p_dma_alloc, p_dma_free, p_dma_map, p_dma_sync, p_fw };

static int cfg_read(int off, int size, u32 *v) { (void)size; *v = off == 2 ? 0x1107 : 0; return 0; }
static int cfg_write(int off, int size, u32 v) { (void)off; (void)size; (void)v; return 0; }
static int msi_target(u32 *lo, u32 *hi, u32 *data) { *lo = 0x1000; *hi = 0; *data = 0; return 0; }
static struct nw_board board;

int main(void) {
  g_bar = calloc(1, 0x200000); *(unsigned *)(g_bar + 0x80000) = 0x200;
  board = (struct nw_board){ .bar = g_bar, .bar_len = 0x200000, .device_id = 0x1107, .msi_cap = 0x50, .irq_base = 100, .cfg_read = cfg_read, .cfg_write = cfg_write, .msi_target = msi_target };
  int r = nw_wifi_start(&plat, &board);
  printf("start: %d stage=%u\n", r, nw_wifi_stats.stage);
  for (int i = 0; i < 60000 && g_ms < 60000; i++) { nw_wifi_step(); lx_idle(1); }
  printf("after 60 s: stage=%u err=%d time=%llu\n", nw_wifi_stats.stage, nw_wifi_stats.last_err, (unsigned long long)g_ms);
  return 0;
}
