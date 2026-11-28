/* The original mac80211 drop-reason names remain available to native stats. */
#include "lx_pci.h"
#include <net/dropreason.h>
const struct drop_reason_list __rcu *drop_reasons_by_subsys[SKB_DROP_REASON_SUBSYS_NUM];
void drop_reasons_register_subsys(enum skb_drop_reason_subsys subsystem, const struct drop_reason_list *list) {
  if (WARN_ON(subsystem >= SKB_DROP_REASON_SUBSYS_NUM || !list ||
              rcu_dereference(drop_reasons_by_subsys[subsystem]))) return;
  rcu_assign_pointer(drop_reasons_by_subsys[subsystem], list);
}
void drop_reasons_unregister_subsys(enum skb_drop_reason_subsys subsystem) {
  if (WARN_ON(subsystem >= SKB_DROP_REASON_SUBSYS_NUM)) return;
  RCU_INIT_POINTER(drop_reasons_by_subsys[subsystem], NULL); synchronize_rcu();
}
