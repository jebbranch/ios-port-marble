#ifndef NW_STA_H
#define NW_STA_H
#include "lx_os.h"
struct ieee80211_hw; struct sk_buff;
void nw_sta_ready(struct ieee80211_hw *hw);                 /* hardware registered: start it and make the station interface */
void nw_sta_rx(struct sk_buff *skb);                        /* every received frame (not kept) */
void nw_sta_scan_done(bool aborted);
void nw_sta_link_lost(void);
void nw_sta_eapol_status(bool acked);                      /* the outcome of a sent handshake frame */
void nw_sta_run(void);                                      /* executes queued requests; called from the dispatcher */
#endif
int nw_sta_tx_pending(void);   /* frames from the board wait in the transmit queue */
