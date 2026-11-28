/* The parts of the wireless stack the driver calls into: hardware registration, channel arithmetic, frame helpers and the receive hook.
 * Station behaviour (scanning policy, association) is driven from nw_wifi.c on top of the driver's operations. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "nw_mac80211.h"

static struct ieee80211_hw *g_hw;
const struct ieee80211_ops *g_ops;
static struct nw_wifi_hooks g_hooks;
extern const struct ieee80211_ops *g_ops;
static inline u32 ieee80211_channel_to_khz_lx(struct ieee80211_channel *c) { return c->center_freq * 1000 + c->freq_offset; }

void nw_wifi_set_hooks(const struct nw_wifi_hooks *h) { g_hooks = *h; }
struct ieee80211_hw *nw_wifi_hw(void) { return g_hw; }

struct ieee80211_hw *ieee80211_alloc_hw_nm(size_t priv_len, const struct ieee80211_ops *ops, const char *name) {
  (void)name; struct ieee80211_hw *hw = lx_alloc(sizeof *hw, 1);
  struct wiphy *w = lx_alloc(sizeof *w + sizeof(void *) * 2, 1);
  void *priv = lx_alloc(priv_len + 64, 1);
  if (!hw || !w || !priv) { lx_free(hw); lx_free(w); lx_free(priv); return 0; }
  mutex_init(&w->mtx); INIT_LIST_HEAD(&w->wdev_list);
  hw->wiphy = w; hw->priv = priv; hw->queues = 1; hw->max_rates = 4; hw->max_rate_tries = 1;
  *(struct ieee80211_hw **)wiphy_priv(w) = hw;
  g_hw = hw; g_ops = ops; return hw; }
const struct ieee80211_ops *nw_wifi_ops(void) { return g_ops; }
void ieee80211_free_hw(struct ieee80211_hw *hw) { if (!hw) return; lx_free(hw->priv); lx_free(hw->wiphy); lx_free(hw); if (g_hw == hw) g_hw = 0; }
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *w) { return *(struct ieee80211_hw **)wiphy_priv(w); }
int ieee80211_register_hw(struct ieee80211_hw *hw) {
  int nch = 0; for (int b = 0; b < NUM_NL80211_BANDS; b++) if (hw->wiphy->bands[b]) nch += hw->wiphy->bands[b]->n_channels;
  lx_log(6, "wifi: hardware registered, %d channels", nch);
  if (g_hooks.registered) g_hooks.registered(hw); return 0; }
void ieee80211_unregister_hw(struct ieee80211_hw *hw) { (void)hw; }
int regulatory_set_wiphy_regd(struct wiphy *w, struct ieee80211_regdomain *rd) { (void)w; (void)rd; return 0; }
void wiphy_rfkill_set_hw_state_reason(struct wiphy *w, bool blocked, enum rfkill_hard_block_reasons r) { (void)w; (void)blocked; (void)r; }

/* wiphy work: queued items run from the dispatcher */
static struct list_head g_wwork = LIST_HEAD_INIT(g_wwork);
static struct work_struct g_wwork_run;
static void wwork_run(struct work_struct *w) {
  (void)w;
  while (!list_empty(&g_wwork)) {
    struct wiphy_work *ww = list_first_entry(&g_wwork, struct wiphy_work, entry);
    list_del_init(&ww->entry);
    if (ww->func) ww->func(g_hw ? g_hw->wiphy : 0, ww); } }
void wiphy_work_queue(struct wiphy *w, struct wiphy_work *ww) {
  (void)w; if (!g_wwork_run.func) INIT_WORK(&g_wwork_run, wwork_run);
  if (!ww->entry.next) INIT_LIST_HEAD(&ww->entry);
  if (!list_empty(&ww->entry)) return; list_add_tail(&ww->entry, &g_wwork); lx_queue_work(&g_wwork_run); }
void wiphy_work_cancel(struct wiphy *w, struct wiphy_work *ww) { (void)w; if (ww->entry.next && !list_empty(&ww->entry)) list_del_init(&ww->entry); }
void wiphy_work_flush(struct wiphy *w, struct wiphy_work *ww) { (void)w; if (ww->entry.next && !list_empty(&ww->entry)) { list_del_init(&ww->entry); ww->func(g_hw ? g_hw->wiphy : 0, ww); } }
void ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *d, unsigned long delay) { (void)hw; lx_queue_delayed(d, delay); }

/* channel arithmetic */
u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band) {
  switch (band) {
  case NL80211_BAND_2GHZ: if (chan == 14) return 2484000; if (chan > 0 && chan < 14) return (2407 + chan * 5) * 1000; break;
  case NL80211_BAND_5GHZ: if (chan >= 182 && chan <= 196) return (4000 + chan * 5) * 1000; if (chan > 0) return (5000 + chan * 5) * 1000; break;
  case NL80211_BAND_6GHZ: if (chan == 2) return 5935000; if (chan > 0 && chan <= 233) return (5950 + chan * 5) * 1000; break;
  default: break; }
  return 0; }
int ieee80211_freq_khz_to_channel(u32 khz) {
  u32 f = khz / 1000;
  if (f == 2484) return 14; if (f < 2484 && f >= 2412) return (f - 2407) / 5;
  if (f >= 4910 && f <= 4980) return (f - 4000) / 5;
  if (f >= 5955 && f <= 7115) return (f - 5950) / 5; if (f == 5935) return 2;
  if (f >= 5005 && f < 5955) return (f - 5000) / 5;
  return 0; }
struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *w, u32 khz) {
  for (int b = 0; b < NUM_NL80211_BANDS; b++) { struct ieee80211_supported_band *sb = w->bands[b]; if (!sb) continue;
    for (int i = 0; i < sb->n_channels; i++) if (ieee80211_channel_to_khz_lx(&sb->channels[i]) == khz) return &sb->channels[i]; }
  return 0; }
unsigned int ieee80211_hdrlen(__le16 fc) {
  unsigned int hl = 24; u16 f = le16_to_cpu(fc);
  if ((f & 0x0c) == 0x08 && (f & 0x0300) == 0x0300) hl = 30;   /* data frame with both DS bits: four addresses */
  if ((f & 0x0c) == 0x08 && (f & 0x0080)) hl += 2;             /* QoS */
  if ((f & 0x0c) == 0x04) { /* control */
    switch (f & 0xf0) { case 0xd0: case 0xc0: hl = 10; break; case 0xb0: case 0xa0: hl = 16; break; default: hl = 16; } }
  if ((f & 0x0c) == 0x00 || (f & 0x0c) == 0x08) { if (f & 0x8000) hl += 4; }   /* HT control */
  return hl; }
unsigned int ieee80211_get_hdrlen_from_skb(const struct sk_buff *skb) { return ieee80211_hdrlen(((struct ieee80211_hdr *)skb->data)->frame_control); }

/* element lookup in information element lists */
const struct element *cfg80211_find_elem_match(u8 eid, const u8 *ies, unsigned int len, const u8 *match, unsigned int match_len, unsigned int match_offset) {
  const u8 *p = ies, *end = ies + len;
  while (p + 2 <= end && p + 2 + p[1] <= end) {
    if (p[0] == eid && (!match || (p[1] >= match_offset + match_len && !memcmp(p + 2 + match_offset, match, match_len)))) return (const struct element *)p;
    p += 2 + p[1]; }
  return 0; }
const struct element *cfg80211_find_vendor_elem(unsigned int oui, int oui_type, const u8 *ies, unsigned int len) {
  u8 m[4] = { oui >> 16, oui >> 8, oui, oui_type };
  const struct element *e = cfg80211_find_elem_match(WLAN_EID_VENDOR_SPECIFIC, ies, len, m, oui_type < 0 ? 3 : 4, 0);
  return e; }
const struct cfg80211_bss_ies *lx_unused_ies;
void cfg80211_put_bss(struct wiphy *w, struct cfg80211_bss *b) { (void)w; (void)b; }
const struct element *ieee80211_bss_get_elem(struct cfg80211_bss *bss, u8 id) {
  const struct cfg80211_bss_ies *ies = bss ? rcu_dereference(bss->ies) : 0; return ies ? cfg80211_find_elem_match(id, ies->data, ies->len, 0, 0, 0) : 0; }
bool cfg80211_chandef_dfs_usable(struct wiphy *w, const struct cfg80211_chan_def *c) { (void)w; (void)c; return false; }
unsigned int cfg80211_chandef_dfs_cac_time(struct wiphy *w, const struct cfg80211_chan_def *c) { (void)w; (void)c; return 0; }

/* Received frames and a lost link go to the station code from a work item of its own, the way mac80211 handles them in its interface work: the
 * station code sends commands and waits for their answers, which come through the same copy engine whose service delivered the frame. */
#define RXQ_MAX 512
static struct sk_buff_head g_rxq; static struct work_struct g_rx_work; static int g_rx_init, g_lost; static unsigned g_rx_dropped;
static void rx_work(struct work_struct *w) {
  static int seen; struct sk_buff *skb; (void)w;
  if (g_lost) { g_lost = 0; if (g_hooks.beacon_loss) g_hooks.beacon_loss(); }
  for (unsigned budget = 64; budget && (skb = skb_dequeue(&g_rxq)); budget--) {
    int n = seen < 12 ? ++seen : 0;
    if (n) lx_log(6, "wifi: frame %d to the station code, %u bytes", n, skb->len);
    if (g_hooks.rx) g_hooks.rx(g_hw, skb);
    if (n) lx_log(6, "wifi: frame %d handled", n);
    kfree_skb(skb); }
  if (g_hooks.rx_flush) g_hooks.rx_flush();
  if (!skb_queue_empty(&g_rxq)) lx_queue_work(&g_rx_work);
}
unsigned nw_rx_backlog(void) { return g_rx_init ? skb_queue_len(&g_rxq) : 0; }
unsigned nw_rx_dropped(void) { return g_rx_dropped; }
static void rx_defer(struct sk_buff *skb) {
  if (!g_rx_init) { skb_queue_head_init(&g_rxq); INIT_WORK(&g_rx_work, rx_work); g_rx_init = 1; }
  if (skb) { if (skb_queue_len(&g_rxq) >= RXQ_MAX) { if (!(g_rx_dropped++ & 255)) lx_log(4, "wifi: receive queue full, %u frames dropped", g_rx_dropped); kfree_skb(skb); return; }
             skb_queue_tail(&g_rxq, skb); }
  queue_work(system_wq, &g_rx_work); }
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta, struct sk_buff *skb, struct napi_struct *napi) {
  (void)hw; (void)napi; (void)sta; rx_defer(skb); }
void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info) { if (g_hooks.scan_done) g_hooks.scan_done(hw, info ? info->aborted : 0); }
void ieee80211_ready_on_channel(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_remain_on_channel_expired(struct ieee80211_hw *hw) { (void)hw; }

/* transmit completion and queue control */
unsigned nw_eapol_tx[3];
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb) { (void)hw; if (skb && skb->protocol == cpu_to_be16(ETH_P_PAE)) nw_eapol_tx[2]++; kfree_skb(skb); }
void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb) { (void)hw; kfree_skb(skb); }
void ieee80211_tx_status_ext(struct ieee80211_hw *hw, struct ieee80211_tx_status *st) {
  (void)hw; if (!st || !st->skb) return;
  if (st->skb->protocol == cpu_to_be16(ETH_P_PAE)) { bool ack = !!(IEEE80211_SKB_CB(st->skb)->flags & IEEE80211_TX_STAT_ACK);
    nw_eapol_tx[!ack]++; if (g_hooks.eapol_status) g_hooks.eapol_status(ack); }
  kfree_skb(st->skb); }
void ieee80211_stop_queues(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_wake_queues(struct ieee80211_hw *hw) { (void)hw; }
int ieee80211_queue_stopped(struct ieee80211_hw *hw, int q) { (void)hw; (void)q; return 0; }
void ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq) { (void)hw; (void)txq; }
void ieee80211_report_low_ack(struct ieee80211_sta *sta, u32 n) { (void)sta; (void)n; }

/* events that only matter once a connection is managed */
void ieee80211_beacon_loss(struct ieee80211_vif *v) { (void)v; g_lost = 1; rx_defer(0); }
void ieee80211_connection_loss(struct ieee80211_vif *v) { (void)v; g_lost = 1; rx_defer(0); }
void ieee80211_gtk_rekey_notify(struct ieee80211_vif *v, const u8 *bssid, const u8 *replay, gfp_t f) { (void)v; (void)bssid; (void)replay; (void)f; }
void ieee80211_hw_restart_disconnect(struct ieee80211_vif *v) { (void)v; }
void ieee80211_restart_hw(struct ieee80211_hw *hw) { (void)hw; lx_log(4, "wifi: firmware restart requested"); }
void ieee80211_radar_detected(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *c) { (void)hw; (void)c; }
void ieee80211_csa_finish(struct ieee80211_vif *v, unsigned int link) { (void)v; (void)link; }
void ieee80211_color_change_finish(struct ieee80211_vif *v, u8 link) { (void)v; (void)link; }
void ieee80211_obss_color_collision_notify(struct ieee80211_vif *v, u64 bitmap, u8 link) { (void)v; (void)bitmap; (void)link; }
int ieee80211_set_active_links(struct ieee80211_vif *v, u16 links) { (void)v; (void)links; return 0; }
bool ieee80211_beacon_cntdwn_is_complete(struct ieee80211_vif *v, unsigned int link) { (void)v; (void)link; return true; }
u8 ieee80211_beacon_update_cntdwn(struct ieee80211_vif *v, unsigned int link) { (void)v; (void)link; return 0; }
struct sk_buff *ieee80211_beacon_get_template(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_mutable_offsets *o, unsigned int link) { (void)hw; (void)v; (void)o; (void)link; return 0; }
struct ieee80211_ema_beacons *ieee80211_beacon_get_template_ema_list(struct ieee80211_hw *hw, struct ieee80211_vif *v, unsigned int link) { (void)hw; (void)v; (void)link; return 0; }
void ieee80211_beacon_free_ema_list(struct ieee80211_ema_beacons *b) { (void)b; }
struct sk_buff *ieee80211_get_fils_discovery_tmpl(struct ieee80211_hw *hw, struct ieee80211_vif *v) { (void)hw; (void)v; return 0; }
struct sk_buff *ieee80211_get_unsol_bcast_probe_resp_tmpl(struct ieee80211_hw *hw, struct ieee80211_vif *v) { (void)hw; (void)v; return 0; }

/* interface, station and channel iteration: the station code registers what exists */
static struct ieee80211_vif *g_vif[4]; static int g_nvif;
void nw_wifi_vif_add(struct ieee80211_vif *v) { if (g_nvif < 4) g_vif[g_nvif++] = v; }
void nw_wifi_vif_del(struct ieee80211_vif *v) { for (int i = 0; i < g_nvif; i++) if (g_vif[i] == v) { g_vif[i] = g_vif[--g_nvif]; return; } }
void ieee80211_iterate_active_interfaces_atomic(struct ieee80211_hw *hw, u32 fl, void (*it)(void *, u8 *, struct ieee80211_vif *), void *data) {
  (void)hw; (void)fl; for (int i = 0; i < g_nvif; i++) it(data, g_vif[i]->addr, g_vif[i]); }
static struct ieee80211_sta *g_sta; static struct ieee80211_chanctx_conf *g_ctx;
void nw_wifi_sta_set(struct ieee80211_sta *s) { g_sta = s; }
void nw_wifi_chanctx_set(struct ieee80211_chanctx_conf *c) { g_ctx = c; }
void ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*it)(void *, struct ieee80211_sta *), void *data) { (void)hw; if (g_sta) it(data, g_sta); }
void ieee80211_iter_chan_contexts_atomic(struct ieee80211_hw *hw, void (*it)(struct ieee80211_hw *, struct ieee80211_chanctx_conf *, void *), void *data) { if (g_ctx) it(hw, g_ctx, data); }
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *v, const u8 *addr) { (void)v; return g_sta && ether_addr_equal(g_sta->addr, addr) ? g_sta : 0; }

/* The station code keeps its own list of networks; the one it joins is known here too, with its elements: the driver reads its security
 * element from it and so tells the firmware that the station needs the key handshake (the firmware drops message 4 otherwise). */
static struct cfg80211_bss *g_bss;
void nw_wifi_bss_set(const u8 *bssid, struct ieee80211_channel *ch, const u8 *ie, unsigned len) {
  if (g_bss) { lx_free((void *)rcu_access_pointer(g_bss->ies)); lx_free(g_bss); g_bss = 0; }
  if (!bssid) return;
  struct cfg80211_bss *b = lx_alloc(sizeof *b + 64, 1); struct cfg80211_bss_ies *ies = lx_alloc(sizeof *ies + len, 1);
  if (!b || !ies) { lx_free(b); lx_free(ies); return; }
  ies->len = len; memcpy(ies->data, ie, len); memcpy(b->bssid, bssid, 6); b->channel = ch;
  rcu_assign_pointer(b->ies, ies); rcu_assign_pointer(b->beacon_ies, ies); g_bss = b; }
struct cfg80211_bss *__cfg80211_get_bss(struct wiphy *w, struct ieee80211_channel *c, const u8 *bssid, const u8 *ssid, size_t ssid_len, enum ieee80211_bss_type t, enum ieee80211_privacy p, u32 use_for) {
  (void)w; (void)c; (void)ssid; (void)ssid_len; (void)t; (void)p; (void)use_for;
  return g_bss && (!bssid || ether_addr_equal(g_bss->bssid, bssid)) ? g_bss : 0; }
struct ieee80211_sta *__ieee80211_iterate_stations(struct ieee80211_hw *hw, struct ieee80211_sta *prev) { (void)hw; return prev ? 0 : g_sta; }
