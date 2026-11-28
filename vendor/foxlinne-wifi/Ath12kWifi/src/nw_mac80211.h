#ifndef NW_MAC80211_H
#define NW_MAC80211_H
struct ieee80211_hw; struct ieee80211_vif; struct sk_buff; struct ieee80211_ops;
struct nw_wifi_hooks {
  void (*registered)(struct ieee80211_hw *);
  void (*rx)(struct ieee80211_hw *, struct sk_buff *);          /* every received frame; the callee does not keep the buffer */
  void (*scan_done)(struct ieee80211_hw *, bool aborted);
  void (*beacon_loss)(void);
  void (*eapol_status)(bool acked);                            /* a sent handshake frame: acknowledged or not */
  void (*rx_flush)(void);
};
void nw_wifi_set_hooks(const struct nw_wifi_hooks *h);
struct ieee80211_hw *nw_wifi_hw(void);
struct ieee80211_channel; void nw_wifi_bss_set(const u8 *bssid, struct ieee80211_channel *ch, const u8 *ie, unsigned len);   /* the network joined (0: none) */
unsigned nw_rx_backlog(void); unsigned nw_rx_dropped(void);   /* received frames waiting for the station code; dropped because that queue was full */
extern unsigned nw_eapol_tx[3];   /* sent handshake frames: acknowledged, not acknowledged, dropped by the driver */
const struct ieee80211_ops *nw_wifi_ops(void);
void nw_wifi_vif_add(struct ieee80211_vif *v);
void nw_wifi_vif_del(struct ieee80211_vif *v);
struct ieee80211_sta; struct ieee80211_chanctx_conf;
void nw_wifi_sta_set(struct ieee80211_sta *s);
void nw_wifi_chanctx_set(struct ieee80211_chanctx_conf *c);
#endif
