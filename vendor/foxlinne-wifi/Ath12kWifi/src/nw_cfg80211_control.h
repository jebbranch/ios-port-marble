/* Worker-only native control adapter. cfg80211 owns connection state. */
#ifndef NW_CFG80211_CONTROL_H
#define NW_CFG80211_CONTROL_H
#include "nw_control.h"
struct net_device;
struct nw_cfg80211_observer {
  void (*ready)(struct net_device *dev);
  void (*event)(unsigned operation, int status, const struct nw_control_view *view);
};
void nw_cfg80211_observe(const struct nw_cfg80211_observer *observer);
struct net_device *nw_cfg80211_primary(void);
/* Starting an attempt closes the old interface before changing its generation. */
int nw_cfg80211_begin_attempt(void);
int nw_cfg80211_set_power(int enabled);
int nw_cfg80211_event_begin(struct nw_control_builder *builder, void *bytes, size_t size, unsigned operation, int status);
int nw_cfg80211_event_send(struct nw_control_builder *builder);
#endif
