/* cfg80211 device ownership in the native device registry; no Linux sysfs. */
#include "lx_pci.h"
#include <net/cfg80211.h>
#include "core.h"
#include "sysfs.h"
static void native_wiphy_release(struct device *device) {
  cfg80211_dev_free(container_of(device, struct cfg80211_registered_device, wiphy.dev));
}
struct class ieee80211_class = { .name = "ieee80211", .dev_release = native_wiphy_release };
int wiphy_sysfs_init(void) { return class_register(&ieee80211_class); }
void wiphy_sysfs_exit(void) { class_unregister(&ieee80211_class); }
