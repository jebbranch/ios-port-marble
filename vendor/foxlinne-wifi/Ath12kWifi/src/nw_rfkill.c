/* Radio state and optional polling, owned by the native wireless control path. */
#include "lx_pci.h"
#include <linux/rfkill.h>

struct rfkill {
  char *name; struct device *parent; enum rfkill_type type;
  const struct rfkill_ops *ops; void *data;
  struct delayed_work poll;
  unsigned hard_reasons; bool soft, registered, polling;
};
static void rfkill_poll(struct work_struct *work) {
  struct rfkill *radio = container_of(to_delayed_work(work), struct rfkill, poll);
  if (!radio->registered || !radio->polling || !radio->ops->poll) return;
  radio->ops->poll(radio, radio->data);
  if (radio->registered && radio->polling) lx_queue_delayed(&radio->poll, 5000);
}
struct rfkill *rfkill_alloc(const char *name, struct device *parent, enum rfkill_type type,
                           const struct rfkill_ops *ops, void *data) {
  if (!name || !ops || type >= NUM_RFKILL_TYPES) return NULL;
  struct rfkill *radio = kzalloc(sizeof(*radio), GFP_KERNEL); if (!radio) return NULL;
  radio->name = lx_strdup(name); if (!radio->name) { kfree(radio); return NULL; }
  radio->parent = parent; radio->ops = ops; radio->data = data; radio->type = type;
  INIT_DELAYED_WORK(&radio->poll, rfkill_poll); return radio;
}
int rfkill_register(struct rfkill *radio) {
  if (!radio || radio->registered) return -EINVAL;
  get_device(radio->parent); radio->registered = true;
  if (radio->ops->query) radio->ops->query(radio, radio->data);
  rfkill_resume_polling(radio); return 0;
}
void rfkill_pause_polling(struct rfkill *radio) {
  if (!radio) return;
  radio->polling = false; lx_cancel_delayed(&radio->poll);
}
void rfkill_resume_polling(struct rfkill *radio) {
  if (!radio || !radio->registered || !radio->ops->poll || radio->polling) return;
  radio->polling = true; lx_queue_delayed(&radio->poll, 0);
}
void rfkill_unregister(struct rfkill *radio) {
  if (!radio || !radio->registered) return;
  radio->registered = false; rfkill_pause_polling(radio); put_device(radio->parent);
}
void rfkill_destroy(struct rfkill *radio) {
  if (!radio) return;
  rfkill_unregister(radio); kfree(radio->name); kfree(radio);
}
bool rfkill_blocked(struct rfkill *radio) {
  return radio && (__atomic_load_n(&radio->soft, __ATOMIC_ACQUIRE) ||
                   __atomic_load_n(&radio->hard_reasons, __ATOMIC_ACQUIRE));
}
bool rfkill_set_hw_state_reason(struct rfkill *radio, bool blocked, enum rfkill_hard_block_reasons reason) {
  if (!radio) return false;
  if (blocked) __atomic_fetch_or(&radio->hard_reasons, (unsigned)reason, __ATOMIC_ACQ_REL);
  else __atomic_fetch_and(&radio->hard_reasons, ~(unsigned)reason, __ATOMIC_ACQ_REL);
  return rfkill_blocked(radio);
}
bool rfkill_set_sw_state(struct rfkill *radio, bool blocked) {
  if (!radio) return false;
  __atomic_store_n(&radio->soft, blocked, __ATOMIC_RELEASE); return rfkill_blocked(radio);
}
void rfkill_init_sw_state(struct rfkill *radio, bool blocked) { rfkill_set_sw_state(radio, blocked); }
int lx_rfkill_set_block(struct rfkill *radio, bool blocked) {
  if (!radio || !radio->registered) return -ENODEV;
  if (__atomic_load_n(&radio->soft, __ATOMIC_ACQUIRE) == blocked) return 0;
  if (!radio->ops->set_block) return -EOPNOTSUPP;
  int result = radio->ops->set_block(radio->data, blocked);
  if (!result) rfkill_set_sw_state(radio, blocked);
  return result;
}
