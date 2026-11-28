/* OS-facing bindings shared by the original cfg80211 and mac80211 sources. */
#include "lx_pci.h"
#include <net/cfg80211.h>

struct wiphy *lx_g_wiphy_enter(struct wiphy *wiphy) { wiphy_lock(wiphy); return wiphy; }
void lx_g_wiphy_exit(struct wiphy **wiphy) { wiphy_unlock(*wiphy); }
