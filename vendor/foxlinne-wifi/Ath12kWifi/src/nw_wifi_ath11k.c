/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Marble WCN6855 hw2.1 transport over the shared mac80211/control runtime. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "nw_qrtr.h"
#include "nw_mainline.h"
#include "nw_wifi.h"
#include "nw_board.h"
#include "nw_control_endpoint.h"
#include <linux/mhi.h>
int ath11k_pci_init(void);
void ath11k_pci_exit(void);
int mhi_init_hook(void);
int lx_init_cfg80211_init(void);
int lx_init_ieee80211_init(void);
void lx_exit_cfg80211_exit(void);
void lx_exit_ieee80211_exit(void);
extern const struct bus_type mhi_bus_type;
struct nw_wifi_stats nw_wifi_stats;
static struct ieee80211_hw *g_hw;
static bool cfg_started, mac_started, pci_started, mhi_started, qrtr_started;
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { g_hw=hw; }
int mhi_init_mmio(struct mhi_controller *controller);
void nw_mhi_pre_load(struct mhi_controller *controller) { mhi_init_mmio(controller); }
void nw_wifi_tx_reasons(char *text, int size) {
  struct nw_buffer_stats stats; nw_wifi_buffer_stats_get(&stats);
  snprintf(text,size,"skb alloc %llu free %llu copied %llu",stats.skb_allocations,stats.skb_frees,stats.copied_bytes);
}
void nw_wifi_link_state(char *text, int size) {
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  text[0]=0;
  if (!g_hw || state.state != NW_STA_UP) return;
  struct ath11k *ar=g_hw->priv;
  rcu_read_lock();
  struct ieee80211_sta *sta=ieee80211_find_sta_by_ifaddr(g_hw,state.bssid,NULL);
  if (sta) {
    struct ath11k_sta *peer=(void *)sta->drv_priv;
    spin_lock_bh(&ar->data_lock);
    u32 rate=cfg80211_calculate_bitrate(&peer->txrate);
    int signal=(s8)peer->rssi_comb;
    if (signal && !test_bit(WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT,ar->ab->wmi_ab.svc_map))
      signal+=ATH11K_DEFAULT_NOISE_FLOOR;
    spin_unlock_bh(&ar->data_lock);
    if (rate) nw_mainline_rate_update(state.bssid,rate);
    if (signal) nw_mainline_signal_update(state.bssid,signal);
    snprintf(text,size,"freq %u rssi %d tx %u.%u Mbps",state.freq_mhz,signal,rate/10,rate%10);
  }
  rcu_read_unlock();
}
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
  lx_plat = p; nw_board = b; ath11k_debug_mask = 0; /* Keep info/warnings and counters; skip HTT/WMI debug formatting in normal traffic. */
  nw_wifi_buffer_pool_configure(1024 * 1024);
  lx_root_init(); lx_log_tap = NULL; nw_qrtr_init();
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
  r = ath11k_pci_init();
  if (r) goto fail;
  pci_started = true; nw_wifi_stats.stage = 2; return 0;
fail:
  nw_wifi_stats.last_err = r; nw_wifi_stop(); lx_log(3, "wifi: driver start failed: %d", r); return r;
}
/* Returns nonzero while traffic moves (frames arrived or frames wait to be sent): the board then calls again at once instead of sleeping.
 * lx_pump's own result is no guide, every handler is offered a look on each call. */
int nw_wifi_step(void) {
  static unsigned last_frames; nw_wifi_control_poll(); lx_pump(); nw_mainline_run();
  static u64 next_signal;
  if (g_hw && lx_now_ms() >= next_signal) {
    next_signal = lx_now_ms() + 1000;
    char link[128]; nw_wifi_link_state(link, sizeof(link));
  }
  const unsigned f = nw_wifi_stats.frames; const int moved = f != last_frames; last_frames = f;
  return moved || nw_mainline_pending() || nw_wifi_control_pending() || lx_next_wait_ms(1000) == 0; }
void nw_wifi_stop(void) {
  nw_mainline_shutdown(); nw_wifi_control_shutdown();
  if (pci_started) { ath11k_pci_exit(); pci_started = false; }
  if (qrtr_started) { nw_qrtr_mhi_unregister(); qrtr_started = false; }
  if (mhi_started) { bus_unregister(&mhi_bus_type); mhi_started = false; }
  if (mac_started) { lx_exit_ieee80211_exit(); mac_started = false; }
  if (cfg_started) { lx_exit_cfg80211_exit(); cfg_started = false; }
  g_hw = NULL; nw_wifi_buffer_pool_configure(0);
}
