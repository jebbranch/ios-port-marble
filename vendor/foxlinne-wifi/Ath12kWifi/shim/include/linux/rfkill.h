#ifndef LX_RFKILL_H
#define LX_RFKILL_H
#include "lx_skb.h"
#include <uapi/linux/rfkill.h>
struct rfkill_ops {
  void (*poll)(struct rfkill *, void *);
  void (*query)(struct rfkill *, void *);
  int (*set_block)(void *, bool);
};
struct rfkill *rfkill_alloc(const char *name, struct device *parent, enum rfkill_type type,
                           const struct rfkill_ops *ops, void *data);
int rfkill_register(struct rfkill *rfkill);
void rfkill_unregister(struct rfkill *rfkill);
void rfkill_destroy(struct rfkill *rfkill);
void rfkill_pause_polling(struct rfkill *rfkill);
void rfkill_resume_polling(struct rfkill *rfkill);
bool rfkill_set_hw_state_reason(struct rfkill *rfkill, bool blocked, enum rfkill_hard_block_reasons reason);
bool rfkill_set_sw_state(struct rfkill *rfkill, bool blocked);
void rfkill_init_sw_state(struct rfkill *rfkill, bool blocked);
bool rfkill_blocked(struct rfkill *rfkill);
int lx_rfkill_set_block(struct rfkill *rfkill, bool blocked);
#endif
