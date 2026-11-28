/* XNU boundary around original mac80211, not a second station state machine. */
#ifndef NW_MAINLINE_H
#define NW_MAINLINE_H
struct ieee80211_hw;
void nw_wifi_hw_ready(struct ieee80211_hw *hw);
int nw_mainline_init(void);
void nw_mainline_run(void);
void nw_mainline_poll(void);
void nw_mainline_rate_update(const unsigned char *bssid, unsigned rate);
void nw_mainline_signal_update(const unsigned char *bssid, int signal);
void nw_mainline_shutdown(void);
int nw_mainline_pending(void);
unsigned nw_rx_backlog(void);
unsigned nw_rx_dropped(void);
#endif
