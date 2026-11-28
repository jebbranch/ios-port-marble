/* Start the original cfg80211/mac80211 stack and the board's ath12k transport. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "nw_mhi.h"
#include "nw_qrtr.h"
#include "nw_mainline.h"
#include "nw_wifi.h"
#include "nw_board.h"
#include "nw_events.h"
#include "nw_control_endpoint.h"
#include "pci.h"
#include "dp_cmn.h"
#include <linux/mhi.h>

int ath12k_wifi7_pci_init(void);
void ath12k_wifi7_pci_exit(void);
int mhi_init_hook(void);
extern unsigned int ath12k_debug_mask;

struct nw_wifi_stats nw_wifi_stats;
static struct ieee80211_hw *g_hw;
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { g_hw = hw; }
int lx_init_cfg80211_init(void);
int lx_init_ieee80211_init(void);
void lx_exit_cfg80211_exit(void);
void lx_exit_ieee80211_exit(void);
static bool cfg_started, mac_started, pci_started, mhi_started, qrtr_started;
extern const struct bus_type mhi_bus_type;

/* The first time the MHI core reports the device's RAM dump state, the boot interface's registers say where the boot stopped. */
static int has_text(const char *s, const char *w) {
  for (; *s; s++) { const char *a = s, *b = w; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
  return 0; }
/* The first WMI command that times out: the copy engines' ring positions (host and device side) and the WMI credits are logged, then the device is
 * asked for its RAM dump, which shows where the firmware stands. */
static int g_wmi_stuck;
static void ring_state(struct ath12k_base *ab, int p) {
  struct ath12k_ce_pipe *pipe = &ab->ce.ce_pipe[p];
  if (pipe->src_ring) {
    struct hal_srng *s = &ab->hal.srng_list[pipe->src_ring->hal_ring_id];
    lx_log(3, "stuck: ce%d src sw %u write %u hp %u reap %u cached-tp %u tp %u", p, pipe->src_ring->sw_index, pipe->src_ring->write_index,
           s->u.src_ring.hp, s->u.src_ring.reap_hp, s->u.src_ring.cached_tp, s->u.src_ring.tp_addr ? *(volatile u32 *)s->u.src_ring.tp_addr : 0xffffffffu);
  }
  if (pipe->dest_ring) {   /* the host fills the buffer ring (a source ring to the host) and reads the status ring */
    struct hal_srng *s = &ab->hal.srng_list[pipe->dest_ring->hal_ring_id];
    lx_log(3, "stuck: ce%d buf sw %u write %u hp %u cached-tp %u tp %u needed %u", p, pipe->dest_ring->sw_index, pipe->dest_ring->write_index,
           s->u.src_ring.hp, s->u.src_ring.cached_tp, s->u.src_ring.tp_addr ? *(volatile u32 *)s->u.src_ring.tp_addr : 0xffffffffu, pipe->rx_buf_needed);
  }
  if (pipe->status_ring) {
    struct hal_srng *s = &ab->hal.srng_list[pipe->status_ring->hal_ring_id];
    lx_log(3, "stuck: ce%d status sw %u tp %u cached-hp %u hp %u", p, pipe->status_ring->sw_index, s->u.dst_ring.tp, s->u.dst_ring.cached_hp,
           s->u.dst_ring.hp_addr ? *s->u.dst_ring.hp_addr : 0xffffffffu);
  }
}
/* The firmware copies its image into the host memory segment of type 1 it asked for over QMI and pages from there (host address = segment +
 * offset in the firmware file). Its read-only pages (code and constants, file offsets below the first writable data) must still equal the file. */
static void ddr_check(struct ath12k_base *ab) {
  const u8 *fw; size_t n;
  if (lx_plat->fw_get("amss.bin", &fw, &n)) return;
  for (int i = 0; i < ab->qmi.mem_seg_count; i++) {
    struct target_mem_chunk *c = &ab->qmi.target_mem[i];
    lx_log(3, "stuck: qmi segment %d type %u at %llx size %x", i, c->type, (unsigned long long)c->paddr, c->size);
    if (c->type != 1 || !c->v.addr) continue;
    const u8 *p = c->v.addr; size_t end = n < c->size ? n : c->size; if (end > 0x3b3000) end = 0x3b3000;
    int bad = 0, shown = 0;
    for (size_t off = 0xe9000; off < end; off += 4096) {
      size_t m = end - off < 4096 ? end - off : 4096;
      if (!memcmp(p + off, fw + off, m)) continue;
      bad++;
      if (shown < 16) {
        size_t k = 0; while (k < m && p[off + k] == fw[off + k]) k++; k &= ~(size_t)15;
        const u32 *q = (const u32 *)(p + off + k);
        lx_log(3, "stuck: ddr page %zx differs at %zx: %08x %08x %08x %08x", off, off + k, q[0], q[1], q[2], q[3]); shown++;
      }
    }
    lx_log(3, "stuck: ddr segment: %d read-only pages of %zu differ from the file", bad, (end - 0xe9000) / 4096);
  }
}
/* What the endpoint's configuration space says about the link and about errors it saw: command and status, the PCI Express device and link
 * registers, and the advanced error reporting registers (uncorrectable and correctable status, first error pointer, header log). */
static void pci_state(void) {
  u32 v, v2, v3, v4;
  if (!nw_board->cfg_read(0x04, 4, &v)) lx_log(3, "stuck: pci command/status %08x", v);
  if (!nw_board->cfg_read(0x80000008, 4, &v) && !nw_board->cfg_read(0x8000000c, 4, &v2) && !nw_board->cfg_read(0x80000010, 4, &v3))
    lx_log(3, "stuck: pcie devctl/sta %08x linkcap %08x linkctl/sta %08x", v, v2, v3);
  for (u32 at = 0x100, guard = 0; at >= 0x100 && at < 0x1000 && guard < 32; guard++) {
    if (nw_board->cfg_read(at, 4, &v)) break;
    if ((v & 0xffff) == 1) {
      u32 ue = 0, uem = 0, ces = 0, cap = 0;
      nw_board->cfg_read(at + 4, 4, &ue); nw_board->cfg_read(at + 8, 4, &uem); nw_board->cfg_read(at + 0x10, 4, &ces); nw_board->cfg_read(at + 0x18, 4, &cap);
      lx_log(3, "stuck: aer at %x ue %08x mask %08x ce %08x cap %08x", at, ue, uem, ces, cap);
      nw_board->cfg_read(at + 0x1c, 4, &v); nw_board->cfg_read(at + 0x20, 4, &v2); nw_board->cfg_read(at + 0x24, 4, &v3); nw_board->cfg_read(at + 0x28, 4, &v4);
      lx_log(3, "stuck: aer header log %08x %08x %08x %08x", v, v2, v3, v4);
      return;
    }
    at = v >> 20;
  }
  lx_log(3, "stuck: no aer capability");
}
static void wmi_stuck(void) {
  if (g_wmi_stuck != 1 || !g_hw) return;
  g_wmi_stuck = 2;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0); struct ath12k_base *ab = ar->ab;
  for (int p = 1; p <= 4; p++) ring_state(ab, p);
  lx_log(3, "stuck: wmi credits %d, mhi ee %d state %d", ab->htc.endpoint[ATH12K_HTC_EP_2].tx_credits, ath12k_pci_priv(ab)->mhi_ctrl->ee,
         ath12k_pci_priv(ab)->mhi_ctrl->dev_state);
  pci_state();
  ddr_check(ab);
  lx_log(3, "stuck: asking for the RAM dump: %d", mhi_force_rddm_mode(ath12k_pci_priv(ab)->mhi_ctrl));
}
/* The driver's counts of transmit completions: by the module that released the frame, the firmware's status and the queue manager's reason. */
void nw_wifi_tx_reasons(char *b, int n) {
  b[0] = 0; if (!g_hw) return;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0); struct ath12k_device_dp_stats *st = &ath12k_ab_to_dp(ar->ab)->device_stats; int o = 0;
  o += snprintf(b + o, n - o, "src"); for (int i = 0; i < HAL_WBM_REL_SRC_MODULE_MAX && o < n; i++) o += snprintf(b + o, n - o, " %u", st->tx_wbm_rel_source[i]);
  if (o < n) o += snprintf(b + o, n - o, " fw"); for (int i = 0; i < MAX_FW_TX_STATUS && o < n; i++) o += snprintf(b + o, n - o, " %u", st->fw_tx_status[i]);
  if (o < n) o += snprintf(b + o, n - o, " tqm"); for (int i = 0; i < MAX_TQM_RELEASE_REASON && o < n; i++) o += snprintf(b + o, n - o, " %u", st->tqm_rel_reason[i]); }
void nw_wifi_link_state(char *text, int size) {
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  text[0] = 0;
  if (!g_hw || state.state != NW_STA_UP) return;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0);
  struct ath12k_dp_link_peer_rate_info info = {};
  ath12k_dp_link_peer_get_sta_rate_info_stats(ath12k_ab_to_dp(ar->ab), state.bssid, &info);
  u32 rate = cfg80211_calculate_bitrate(&info.txrate);
  nw_mainline_rate_update(state.bssid, rate);
  if (info.rssi_comb) {
    int signal = (s8)info.rssi_comb;
    if (!test_bit(WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT, ar->ab->wmi_ab.svc_map))
      signal += ath12k_pdev_get_noise_floor(ar);
    nw_mainline_signal_update(state.bssid, signal);
  }
  struct nw_wifi_status updated; nw_wifi_status_get(&updated);
  snprintf(text, size, "freq %u rssi %d tx %u.%u Mbps flags %x mcs %u nss %u bw-code %u gi %u",
           state.freq_mhz, updated.signal_dbm, rate / 10, rate % 10, info.txrate.flags, info.txrate.mcs,
           info.txrate.nss, info.txrate.bw, info.txrate.he_gi);
}
/* Entries a ring holds for its consumer (dst: waiting for the host; src: free for the host to fill); -1 for a ring that was never set up. */
static int ring_count(struct ath12k_base *ab, struct dp_srng *r, int dst) {
  if (!r->vaddr) return -1;
  struct hal_srng *s = &ab->hal.srng_list[r->ring_id]; spin_lock_bh(&s->lock);
  int n = dst ? ath12k_hal_srng_dst_num_free(ab, s, true) : ath12k_hal_srng_src_num_free(ab, s, true); spin_unlock_bh(&s->lock); return n; }
/* Every other ring the host empties (exception, release, status, receive DMA errors, transmit completions) and the buffer rings it fills, with
 * the idle link descriptor ring the hardware takes a descriptor from for each received MPDU. */
void nw_wifi_ring_state(char *b, int n) {
  b[0] = 0; if (!g_hw) return;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0); struct ath12k_base *ab = ar->ab; struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
  snprintf(b, n, "exc %d rel %d st %d dmaerr %d/%d txc %d/%d/%d; free mon %d mac %d/%d idle %d; relsrc %u %u %u %u",
           ring_count(ab, &dp->reo_except_ring, 1), ring_count(ab, &dp->rx_rel_ring, 1), ring_count(ab, &dp->reo_status_ring, 1),
           ring_count(ab, &dp->rxdma_err_dst_ring[0], 1), ring_count(ab, &dp->rxdma_err_dst_ring[1], 1),
           ring_count(ab, &dp->tx_ring[0].tcl_comp_ring, 1), ring_count(ab, &dp->tx_ring[1].tcl_comp_ring, 1), ring_count(ab, &dp->tx_ring[2].tcl_comp_ring, 1),
           ring_count(ab, &dp->rx_mon_status_refill_ring[0].refill_buf_ring, 0), ring_count(ab, &dp->rx_mac_buf_ring[0], 0), ring_count(ab, &dp->rx_mac_buf_ring[1], 0), ring_count(ab, &dp->wbm_idle_ring, 0),
           dp->device_stats.rx_wbm_rel_source[0][0], dp->device_stats.rx_wbm_rel_source[1][0], dp->device_stats.rx_wbm_rel_source[2][0], dp->device_stats.rx_wbm_rel_source[3][0]); }
/* The receive side as the host sees it: frames taken from each destination ring, error counts, free entries of the refill ring, entries waiting in
 * each destination ring, and the frames queued for the station code. */
void nw_wifi_rx_state(char *b, int n) {
  b[0] = 0; if (!g_hw) return;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0); struct ath12k_base *ab = ar->ab; struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
  struct ath12k_device_dp_stats *st = &dp->device_stats; int o = 0;
  o += snprintf(b + o, n - o, "reo"); for (int i = 0; i < 4 && o < n; i++) o += snprintf(b + o, n - o, " %u", st->reo_rx[i][0]);
  if (o < n) o += snprintf(b + o, n - o, " err %u push", st->err_ring_pkts); for (int i = 0; i < 4 && o < n; i++) o += snprintf(b + o, n - o, " %u", st->hal_reo_error[i]);
  for (int i = 0; i < HAL_REO_ENTR_RING_RXDMA_ECODE_MAX && o < n; i++) if (st->rxdma_error[i]) o += snprintf(b + o, n - o, " dma%d=%u", i, st->rxdma_error[i]);
  for (int i = 0; i < HAL_REO_DEST_RING_ERROR_CODE_MAX && o < n; i++) if (st->reo_error[i]) o += snprintf(b + o, n - o, " reo%d=%u", i, st->reo_error[i]);
  { int f = ring_count(ab, &dp->rx_refill_buf_ring.refill_buf_ring, 0); if (o < n) o += snprintf(b + o, n - o, "; refill free %d; waiting", f); }
  for (int i = 0; i < 4 && o < n; i++) { if (!dp->reo_dst_ring[i].vaddr) continue; struct hal_srng *s = &ab->hal.srng_list[dp->reo_dst_ring[i].ring_id];
    spin_lock_bh(&s->lock); int w = ath12k_hal_srng_dst_num_free(ab, s, true); spin_unlock_bh(&s->lock); o += snprintf(b + o, n - o, " %d", w); }
  if (o < n) snprintf(b + o, n - o, "; queued %u, dropped %u", nw_rx_backlog(), nw_rx_dropped()); }
/* The firmware's own statistics (HTT extended stats; 2 is the radio's receive side): asked for on demand, every block of the answer is
 * logged with its non-zero words. */
void nw_wifi_htt_stats(int type) {
  if (!g_hw) return;
  struct ath12k *ar = ath12k_ah_to_ar(g_hw->priv, 0); struct htt_ext_stats_cfg_params cfg = {};
  int r = ath12k_dp_tx_htt_h2t_ext_stats_req(ar, type, &cfg, (u64)ATH12K_HTT_STATS_MAGIC_VALUE << 32);
  lx_log(6, "wifi: firmware statistics %d requested: %d", type, r); }
/* One line per run of non-zero words, "index=value", each line short enough for a board log that keeps about 100 characters of a line. */
void nw_htt_ext_stats(struct ath12k_base *ab, struct sk_buff *skb) {
  (void)ab; const struct ath12k_htt_extd_stats_msg *m = (const void *)skb->data;
  if (skb->len < sizeof *m) return;
  u32 len = u32_get_bits(le32_to_cpu(m->info1), ATH12K_HTT_T2H_EXT_STATS_INFO1_LENGTH);
  if (len > skb->len - sizeof *m) len = skb->len - sizeof *m;
  const u8 *p = m->data, *end = m->data + len;
  while (p + 4 <= end) {
    u32 h = get_unaligned_le32(p); u16 tag = h & 0xfff, tl = (h >> 12) & 0xfff; p += 4; if (p + tl > end) break;
    char b[96]; int o = 0;
    for (unsigned i = 0; i + 4 <= tl; i += 4) {
      u32 v = get_unaligned_le32(p + i); if (!v) continue;
      if (!o) o = snprintf(b, sizeof b, "wifi: fw stats t%u:", tag);
      o += snprintf(b + o, sizeof b - o, " %u=%u", i / 4, v);
      if (o > (int)sizeof b - 18) { lx_log(6, "%s", b); o = 0; }
    }
    if (o) lx_log(6, "%s", b);
    p += tl; } }
static void log_tap(int level, const char *line) {
  static int done; (void)level;
  if (!g_wmi_stuck && has_text(line, "wmi command") && has_text(line, "timeout")) g_wmi_stuck = 1;
  if (done || !has_text(line, "MHI_CB_EE_RDDM") || !nw_board || !nw_board->bar) return;
  done = 1;
  const volatile u8 *m = nw_board->bar; u32 bhi = readl(m + 0x28);
  if (bhi >= nw_board->bar_len) bhi = 0x100;
  lx_log(3, "rddm: mhistatus %08x mhictrl %08x ee %08x bhi-status %08x err %08x dbg %08x %08x %08x",
         readl(m + 0x48), readl(m + 0x38), readl(m + bhi + 0x28), readl(m + bhi + 0x2c), readl(m + bhi + 0x30), readl(m + bhi + 0x34),
         readl(m + bhi + 0x38), readl(m + bhi + 0x3c)); }

/* The address of a radio whose firmware reports none (or a random default one): the board's file wlan_mac.bin (text, the address as twelve hex digits after the
 * last '='), else a locally administered address made up once. */
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
void nw_board_mac(u8 *a) {
  static u8 made[6]; static int have;
  const u8 *d; size_t n;
  if (!have && lx_plat->fw_get && !lx_plat->fw_get("wlan_mac.bin", &d, &n)) {
    size_t s = 0; for (size_t i = 0; i < n; i++) if (d[i] == '=') s = i + 1;
    u8 m[6]; size_t k = 0;
    for (size_t i = s; i < n && k < 12; i++) { int v = hexval((char)d[i]); if (v < 0) { if (k) break; continue; } m[k / 2] = (u8)(k & 1 ? m[k / 2] << 4 | v : v); k++; }
    if (k == 12 && is_valid_ether_addr(m)) { memcpy(made, m, 6); have = 1; lx_log(6, "wifi: address from wlan_mac.bin"); }
  }
  if (!have) {
    for (int i = 0; i < 6; i++) made[i] = (u8)(lx_random_u32() ^ lx_now_ms() >> (i * 3));
    made[0] = (made[0] & 0xfe) | 0x02; have = 1; lx_log(4, "wifi: the firmware and the board give no address: a made-up one is used");
  }
  memcpy(a, made, 6); }

void nw_wifi_irq_mode(int delivered) { lx_irq_polling(!delivered); }
void nw_wifi_poll(void) { nw_mainline_poll(); nw_wifi_control_poll(); }
void nw_wifi_irq_pending(uint32_t vectors) {
  if (!nw_board) return;
  while (vectors) {
    unsigned bit = __builtin_ctz(vectors); vectors &= vectors - 1;
    lx_irq_raise(nw_board->irq_base + bit);
  }
}
unsigned nw_wifi_wait_ms(unsigned maximum) {
  return nw_mainline_pending() || nw_wifi_control_pending() ? 0 : lx_next_wait_ms(maximum);
}
void nw_wifi_set_platform(const struct lx_platform *p) { lx_plat = p; }
int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) {
  lx_plat = p; nw_board = b; ath12k_debug_mask = 0; /* Keep info/warnings and counters; skip HTT/WMI debug formatting in normal traffic. */
  nw_wifi_buffer_pool_configure(1024 * 1024);
  lx_root_init(); lx_log_tap = log_tap; nw_qrtr_init();
  int r = nw_mainline_init(); if (r) goto fail;
  r = lx_init_cfg80211_init(); if (r) goto fail;
  cfg_started = true;
  r = lx_init_ieee80211_init(); if (r) goto fail;
  mac_started = true;
  r = mhi_init_hook(); if (r) goto fail;
  mhi_started = true;
  r = nw_qrtr_mhi_register(); if (r) goto fail;
  qrtr_started = true;
  nw_wifi_stats.stage = 1;
  r = ath12k_wifi7_pci_init();
  if (r) goto fail;
  pci_started = true; nw_wifi_stats.stage = 2; return 0;
fail:
  nw_wifi_stats.last_err = r; nw_wifi_stop(); lx_log(3, "wifi: driver start failed: %d", r); return r;
}
/* A counter line of any length as log lines of at most 80 characters after the prefix, broken at spaces (a board log keeps about
 * 100 characters of a line). */
static void log_counters(const char *prefix, const char *text) {
  while (*text == ' ') text++;
  while (*text) {
    size_t n = strlen(text);
    if (n > 80) { n = 80; while (n > 1 && text[n] != ' ') n--; }
    lx_log(6, "%s %.*s", prefix, (int)n, text);
    text += n; while (*text == ' ') text++;
  }
}
/* Every 30 s while joined, when frames arrived since the last time: the driver's receive counters (frames per destination ring, error
 * pushes by code, where reo0 counts frames that came without a reorder queue; then the free refill entries, the entries waiting and
 * the frames dropped at the boundary). With the board's diagnostics on and at least 20000 frames in that time the firmware's receive
 * statistics are asked for as well, at most once in two minutes; their answer is logged when it comes. */
static void receive_report(void) {
  static u64 next, next_firmware; static unsigned reported;
  u64 now = lx_now_ms(); if (now < next) return;
  next = now + 30000;
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  unsigned frames = nw_wifi_stats.frames, moved = frames - reported; reported = frames;
  if (state.state != NW_STA_UP || !moved) return;
  char text[240]; nw_wifi_rx_state(text, sizeof text);
  char *rings = strchr(text, ';'); if (rings) *rings++ = 0;
  log_counters("wifi: rx", text); if (rings) log_counters("wifi: rx rings", rings);
  if (nw_board && nw_board->diagnostics && moved >= 20000 && now >= next_firmware) { next_firmware = now + 120000; nw_wifi_htt_stats(2); }
}
/* Returns nonzero while traffic moves (frames arrived or frames wait to be sent): the board then calls again at once instead of sleeping.
 * lx_pump's own result is no guide, every handler is offered a look on each call. */
int nw_wifi_step(void) {
  static unsigned last_frames; nw_wifi_control_poll(); lx_pump(); nw_mainline_run(); wmi_stuck();
  static u64 next_signal;
  if (g_hw && lx_now_ms() >= next_signal) {
    next_signal = lx_now_ms() + 1000;
    char link[128]; nw_wifi_link_state(link, sizeof(link));
    receive_report();
  }
  const unsigned f = nw_wifi_stats.frames; const int moved = f != last_frames; last_frames = f;
  return moved || nw_mainline_pending() || nw_wifi_control_pending() || lx_next_wait_ms(1000) == 0; }
void nw_wifi_stop(void) {
  nw_mainline_shutdown(); nw_wifi_control_shutdown();
  if (pci_started) { ath12k_wifi7_pci_exit(); pci_started = false; }
  if (qrtr_started) { nw_qrtr_mhi_unregister(); qrtr_started = false; }
  if (mhi_started) { bus_unregister(&mhi_bus_type); mhi_started = false; }
  if (mac_started) { lx_exit_ieee80211_exit(); mac_started = false; }
  if (cfg_started) { lx_exit_cfg80211_exit(); cfg_started = false; }
  g_hw = NULL; nw_wifi_buffer_pool_configure(0);
}
