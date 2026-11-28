/* Resource-free devices used for asynchronous regulatory firmware requests. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <linux/device/faux.h>
struct native_faux_device { struct faux_device device; struct faux_device_ops ops; };
static void faux_release(struct device *device) {
  struct native_faux_device *native = container_of(device, struct native_faux_device, device.dev);
  kfree(native);
}
struct faux_device *faux_device_create(const char *name, struct device *parent, const struct faux_device_ops *ops) {
  struct native_faux_device *native = kzalloc(sizeof(*native), GFP_KERNEL);
  if (!native) return ERR_PTR(-ENOMEM);
  if (ops) native->ops = *ops;
  struct device *device = &native->device.dev;
  device_initialize(device); device->release = faux_release; device->parent = parent;
  int result = dev_set_name(device, "%s", name);
  if (!result) result = device_add(device);
  if (!result && native->ops.probe) result = native->ops.probe(&native->device);
  if (result) { device_del(device); put_device(device); return ERR_PTR(result); }
  return &native->device;
}
void faux_device_destroy(struct faux_device *device) {
  if (IS_ERR_OR_NULL(device)) return;
  struct native_faux_device *native = container_of(device, struct native_faux_device, device);
  if (native->ops.remove) native->ops.remove(device);
  device_unregister(&device->dev);
}

/* Registered and dispatched by the serialized wireless worker. */
static struct notifier_block *pm_notifiers;
int register_pm_notifier(struct notifier_block *n) {
  struct notifier_block **at=&pm_notifiers;
  while (*at && (*at)->priority >= n->priority) {
    if (*at == n) return -EEXIST;
    at=&(*at)->next;
  }
  n->next=*at; *at=n; return 0;
}
int unregister_pm_notifier(struct notifier_block *n) {
  struct notifier_block **at=&pm_notifiers;
  while (*at && *at != n) at=&(*at)->next;
  if (!*at) return -ENOENT;
  *at=n->next; n->next=NULL; return 0;
}
int lx_pm_notify(unsigned long action) {
  int result=NOTIFY_DONE;
  for (struct notifier_block *n=pm_notifiers; n; n=n->next) {
    result=n->notifier_call(n,action,NULL);
    if (result & NOTIFY_STOP_MASK) break;
  }
  return notifier_to_errno(result);
}

static int mac_hex(char c) {
  if (c >= '0' && c <= '9') return c-'0';
  if (c >= 'a' && c <= 'f') return c-'a'+10;
  if (c >= 'A' && c <= 'F') return c-'A'+10;
  return -1;
}
int device_get_mac_address(struct device *device, u8 *address) {
  (void)device;
  const u8 *data; size_t length;
  if (!lx_plat->fw_get || lx_plat->fw_get("wlan_mac.bin",&data,&length)) return -ENOENT;
  size_t at=0;
  for (size_t i=0; i<length; ++i) if (data[i]=='=') at=i+1;
  if (length-at<12) return -EINVAL;
  u8 mac[6];
  for (unsigned i=0; i<6; ++i) {
    int hi=mac_hex(data[at+2*i]),lo=mac_hex(data[at+2*i+1]);
    if (hi<0 || lo<0) return -EINVAL;
    mac[i]=(hi<<4)|lo;
  }
  if (!is_valid_ether_addr(mac)) return -EINVAL;
  memcpy(address,mac,6); return 0;
}
