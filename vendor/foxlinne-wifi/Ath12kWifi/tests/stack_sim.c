/* The whole stack against a simulated WCN7850: boot ROM (PBL, SBL, mission mode over BHI/BHIe), MHI rings and channels, a QRTR node that offers the
 * WLAN firmware service, and a QMI peer that answers the driver's setup requests. Register-level firmware behaviour (copy engines, WMI) is not simulated:
 * the run ends where the driver starts talking to those, which must then time out cleanly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_os.h"
#include "lx_net.h"
#include "lx_plat.h"
#include "nw_board.h"
#include "nw_wifi.h"
#include "linux/soc/qcom/qmi.h"
#include "../out/ath12k-src/qmi.c"

#define BHIOFF_V 0x100
#define BHIEOFF_V 0x124
#define CHDBOFF_V 0x300
#define ERDBOFF_V 0x600
static u8 *g_bar; static u64 g_ms; static int g_stage;
static u32 *R(u32 off) { return (u32 *)(g_bar + off); }
static u64 R64(u32 lo) { return ((u64)*R(lo + 4) << 32) | *R(lo); }

/* ---- host memory model: DMA addresses are host pointers masked to 32 bits by the allocator; keep a table ---- */
static struct { u8 *p; u64 bus; size_t n; } g_mem[8192]; static int g_nmem; static u64 g_next_bus = 0x10000000;
static size_t g_live, g_peak;
static size_t arena_size(size_t n) { n = (n + 63) & ~63UL; if (n >= 4096) { n = (n + 4095) & ~4095UL; size_t a = 4096; while (a < n && a < (2u << 20)) a <<= 1; return n + a / 2; } return n; }
static void *p_dma_alloc(size_t n, uint64_t *bus) {
  if (n >= 262144) { void *ra = __builtin_return_address(0); fprintf(stderr, "BIG %zu from %p\n", n, ra); }
  g_live += arena_size(n); if (g_live > g_peak) g_peak = g_live;
  if (g_nmem >= 8192) { { int live = 0; size_t sz[4] = {0}; for (int k = 0; k < g_nmem; k++) if (g_mem[k].p) { live++; } printf("SIM: dma table full, live=%d last sizes:", live); for (int k = g_nmem - 8; k < g_nmem; k++) printf(" %zu", g_mem[k].n); printf("\n"); (void)sz; } abort(); }
  n = (n + 4095) & ~4095UL; void *p = aligned_alloc(4096, n); memset(p, 0, n);
  g_mem[g_nmem].p = p; g_mem[g_nmem].bus = g_next_bus; g_mem[g_nmem].n = n; g_nmem++; *bus = g_next_bus; g_next_bus += n; return p; }
static void p_dma_free(size_t n, void *p, uint64_t b) { (void)n; (void)b; for (int i = 0; i < g_nmem; i++) if (g_mem[i].p == p) { g_mem[i].p = 0; free(p); return; } }
static u8 *host(u64 bus) { for (int i = 0; i < g_nmem; i++) if (g_mem[i].p && bus >= g_mem[i].bus && bus < g_mem[i].bus + g_mem[i].n) return g_mem[i].p + (bus - g_mem[i].bus); return 0; }
static uint64_t p_dma_map(void *p, size_t n, int d) { (void)n; (void)d; for (int i = 0; i < g_nmem; i++) if (g_mem[i].p && (u8 *)p >= g_mem[i].p && (u8 *)p < g_mem[i].p + g_mem[i].n) return g_mem[i].bus + ((u8 *)p - g_mem[i].p); return 0; }

/* ---- MHI rings, device side ---- */
struct ctx { u32 a, b, c; u64 rbase, rlen, rp, wp; } __attribute__((packed));
static struct ctx *chctx(int ch) { return (struct ctx *)host(R64(0x58)) + ch; }
static struct ctx *erctx(int r) { return (struct ctx *)host(R64(0x60)) + r; }
static struct ctx *cmdctx(void) { return (struct ctx *)host(R64(0x68)); }
static void ev_post(int ring, u64 ptr, u32 d0, u32 d1) {
  struct ctx *e = erctx(ring); u32 *el = (u32 *)host(e->rp); memcpy(el, &ptr, 8); el[2] = d0; el[3] = d1;
  e->rp += 16; if (e->rp >= e->rbase + e->rlen) e->rp = e->rbase; lx_irq_raise(101 + ring); }

/* ---- QRTR node 7 with the firmware service ---- */
#define NODE 7
#define SVC_PORT 0x10
struct dl { u8 *d; size_t n; struct dl *next; }; static struct dl *g_dl_head, *g_dl_tail;
static void dl_push(const void *d, size_t n) { struct dl *q = malloc(sizeof *q); q->d = malloc(n); memcpy(q->d, d, n); q->n = n; q->next = 0; if (g_dl_tail) g_dl_tail->next = q; else g_dl_head = q; g_dl_tail = q; }
struct h1 { u32 ver, type, sn, sp, conf, size, dn, dp; };
static void qrtr_send(u32 type, u32 sp, u32 dn, u32 dp, const void *pl, size_t n) {
  u8 buf[2048]; struct h1 h = { 1, type, NODE, sp, 0, (u32)n, dn, dp }; memcpy(buf, &h, sizeof h); memcpy(buf + sizeof h, pl, n); dl_push(buf, sizeof h + ((n + 3) & ~3UL)); }
static void ctrl(u32 cmd, u32 svc, u32 inst, u32 node, u32 port) { u32 p[5] = { cmd, svc, inst, node, port }; qrtr_send(cmd == 4 ? 4 : cmd, 0xfffffffe, 0xffffffff, 0xfffffffe, p, sizeof p); }
static u32 g_host_port;
static void qmi_ind(u16 msg, const struct qmi_elem_info *ei, const void *st) {
  size_t len = 2048; void *b = qmi_encode_message(QMI_INDICATION, msg, &len, 0, ei, st); qrtr_send(1, SVC_PORT, 1, g_host_port, b, len); lx_free(b); }
static void qmi_resp(u8 *req, size_t n) {
  (void)n; u16 txn = req[1] | req[2] << 8, msg = req[3] | req[4] << 8;
  u8 r[16]; r[0] = 2; r[1] = txn; r[2] = txn >> 8; r[3] = msg; r[4] = msg >> 8; r[5] = 7; r[6] = 0; r[7] = 2; r[8] = 4; r[9] = 0; r[10] = r[11] = r[12] = r[13] = 0;
  qrtr_send(1, SVC_PORT, 1, g_host_port, r, 14);
  if (getenv("V")) printf("[sim %llu] qmi request 0x%x answered\n", (unsigned long long)g_ms, msg);
  if (msg == 0x20) {   /* indication register: firmware asks for memory */
    static struct qmi_wlanfw_request_mem_ind_msg_v01 m; memset(&m, 0, sizeof m); m.mem_seg_len = 1; m.mem_seg[0].size = 0x100000; m.mem_seg[0].type = 1;
    qmi_ind(QMI_WLANFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei, &m); }
  if (msg == 0x36) {   /* memory given: firmware is ready for capabilities */
    static struct { u8 dummy; } e; (void)e; u8 b[7] = { 4, 0, 0, 0x37, 0, 0, 0 }; qrtr_send(1, SVC_PORT, 1, g_host_port, b, 7); } }

static int g_cmd_rp_seen, g_ul_seen;
static void dev_poll(void) {
  u32 bhi = *R(0x28), bhie = *R(0x2c);
  if (*R(0x38) & 2) *R(0x38) &= ~2u;       /* a reset request completes at once */
  if (g_stage == 0 && *R(bhi + 0x18)) {          /* PBL took the boot loader */
    *R(bhi + 0x2c) = 0x80005eed; *R(bhi + 0x28) = 1; g_stage = 1; }
  if (g_stage == 1) { *R(0x48) = 1; g_stage = 2; }
  if (g_stage == 2 && ((*R(0x38) >> 8) & 0xff) == 2) { *R(0x48) = 1 | 2 << 8; g_stage = 3; ev_post(0, 0, 2u << 24, 0x20u << 16); ev_post(0, 0, 1u << 24, 0x40u << 16); }
  if (g_stage == 3 && *R(bhie + 0x3c)) {
    *R(bhie + 0x44) = (*R(bhie + 0x3c) & 0x3fffffff) | 2u << 30; *R(bhi + 0x28) = 2; g_stage = 4; ev_post(0, 0, 2u << 24, 0x40u << 16); }
  if (g_stage < 4) return;
  /* command ring */
  struct ctx *c = cmdctx();
  while (c->rp != c->wp) {
    u32 *tre = (u32 *)host(c->rp); u32 type = (tre[3] >> 16) & 0xff, ch = tre[3] >> 24; u64 at = c->rp;
    if (type == 0x12) { chctx(ch)->a = (chctx(ch)->a & ~0xffu) | 2; }
    else if (type == 0x10) { chctx(ch)->a = (chctx(ch)->a & ~0xffu) | 1; }
    ev_post(0, at, 1u << 24, 0x21u << 16);
    c->rp += 16; if (c->rp >= c->rbase + c->rlen) c->rp = c->rbase; }
  /* host to device channel 20 */
  struct ctx *u = chctx(20);
  while (u->rbase && u->rp != u->wp) {
    u32 *tre = (u32 *)host(u->rp); u64 ptr; memcpy(&ptr, tre, 8); u32 len = tre[2] & 0xffff; u64 at = u->rp;
    u8 *pl = host(ptr); const struct h1 *h = (void *)pl;
    if (h->type == 2) { ctrl(2, 0, 0, 0, 0); /* answered: our hello */ ctrl(4, 0x45, 0x101, NODE, SVC_PORT); }
    else if (h->type == 1 && h->dp == SVC_PORT) { g_host_port = h->sp; qmi_resp(pl + sizeof *h, h->size); }
    else if (h->type == 10) { /* lookup goes to the host's own name service, not here */ }
    u->rp += 16; if (u->rp >= u->rbase + u->rlen) u->rp = u->rbase;
    ev_post(chctx(20)->c, at, 2u << 24 | len, 20u << 24 | 0x22u << 16); }
  /* device to host channel 21 */
  struct ctx *d = chctx(21);
  while (d->rbase && g_dl_head && d->rp != d->wp) {
    u32 *tre = (u32 *)host(d->rp); u64 ptr; memcpy(&ptr, tre, 8); u64 at = d->rp; struct dl *q = g_dl_head;
    memcpy(host(ptr), q->d, q->n); g_dl_head = q->next; if (!g_dl_head) g_dl_tail = 0;
    d->rp += 16; if (d->rp >= d->rbase + d->rlen) d->rp = d->rbase;
    ev_post(chctx(21)->c, at, 2u << 24 | (u32)q->n, 21u << 24 | 0x22u << 16); free(q->d); free(q); }
  (void)g_cmd_rp_seen; (void)g_ul_seen;
}
static u64 p_now(void) { return g_ms; }
static void p_sleep(unsigned ms) { g_ms += ms; }
static void p_log(int l, const char *s) { if (getenv("V") || l <= 4) printf("[%d %llu] %s\n", l, (unsigned long long)g_ms, s); }
static void p_halt(void) { abort(); }
static void *p_alloc(size_t n) { return malloc(n); }
static void p_free(void *p) { free(p); }
static void p_dma_sync(uint64_t b, size_t n, int d) { (void)b; (void)n; (void)d; }
extern const u8 *g_fw_amss; extern size_t g_fw_amss_len;
static u8 *g_blob; static size_t g_blob_n = 6 * 1024 * 1024;
static int p_fw(const char *n, const uint8_t **d, size_t *s) {
  if (strstr(n, "amss.bin") || strstr(n, "m3.bin")) { if (!g_blob) { g_blob = malloc(g_blob_n); for (size_t i = 0; i < g_blob_n; i++) g_blob[i] = (u8)(i * 7 + 3); }
    *d = g_blob; *s = strstr(n, "amss") ? g_blob_n : 200000; return 0; }
  return -1; }
static const struct lx_platform plat = { p_now, p_sleep, dev_poll, p_log, p_halt, p_alloc, p_free, p_dma_alloc, p_dma_free, p_dma_map, p_dma_sync, p_fw };
static int cfg_read(int off, int size, u32 *v) { static u32 msi[4] = { 0x00809005, 0, 0, 0 }; (void)size;
  if (off == 2) *v = 0x1107; else if (off == 0x2c) *v = 0x110717cb; else if (off >= 0x50 && off < 0x60) *v = msi[(off - 0x50) / 4]; else *v = 0; return 0; }
static int cfg_write(int off, int size, u32 v) { (void)off; (void)size; (void)v; return 0; }
static int msi_target(u32 *lo, u32 *hi, u32 *data) { *lo = 0x1000; *hi = 0; *data = 0; return 0; }
static struct nw_board board;
void lx_irq_raise(unsigned int irq);

static u32 g_win;
static void trace(volatile void *a, u32 v) {
  if (!getenv("T")) return; unsigned long off = (unsigned long)((u8 *)a - g_bar); if ((u8 *)a < g_bar || off >= 0x200000) return;
  if (off == 0x310c) { g_win = v & 0x3f; printf("[w] window <- %02x\n", v); return; }
  if (off >= 0x80000) printf("[w] chip %07lx <- %08x\n", ((unsigned long)g_win << 19) | (off - 0x80000), v); else printf("[w] bar %04lx <- %08x\n", off, v); }
int main(void) {
  extern void (*lx_mmio_trace)(volatile void *, u32); lx_mmio_trace = trace;
  setvbuf(stdout, 0, _IONBF, 0);
  g_bar = calloc(1, 0x200000); *(unsigned *)(g_bar + 0x80000) = 0x200;
  *R(0) = 0x200000; *R(8) = 0x01000000; *R(0x18) = CHDBOFF_V; *R(0x20) = ERDBOFF_V; *R(0x28) = BHIOFF_V; *R(0x2c) = BHIEOFF_V; *R(BHIOFF_V + 0x28) = 0;
  board = (struct nw_board){ .bar = g_bar, .bar_len = 0x200000, .device_id = 0x1107, .msi_cap = 0x50, .irq_base = 100, .cfg_read = cfg_read, .cfg_write = cfg_write, .msi_target = msi_target };
  /* the boot ROM clears the reset bit once the host asks for a reset; MHICTRL.RESET is bit 1 */
  int r = nw_wifi_start(&plat, &board);
  printf("start: %d stage=%u dev-stage=%d\n", r, nw_wifi_stats.stage, g_stage);
  for (int i = 0; i < 40000 && g_ms < 200000; i++) { nw_wifi_step(); lx_idle(1); }
  printf("after run: stage=%u scans=%u err=%d dev-stage=%d time=%llu ms\n", nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.last_err, g_stage, (unsigned long long)g_ms);
  { extern unsigned long lx_stack_peak, lx_depth_peak; printf("stack peak %lu bytes at pump depth %lu\n", lx_stack_peak, lx_depth_peak); }
  for (int k = 0; k < g_nmem; k++) if (g_mem[k].p && g_mem[k].n >= 65536) printf("SIM: live %zu KiB\n", g_mem[k].n / 1024);
  printf("SIM: dma allocated, frees not counted: %zu KiB\n", g_peak / 1024);
  return 0;
}
