/* CONFIG_KCOV is disabled, matching upstream's non-instrumented definitions. */
#ifndef LX_KCOV_H
#define LX_KCOV_H
#include "lx_skb.h"
static inline void kcov_remote_start_common(u64 handle) { (void)handle; }
static inline void kcov_remote_stop(void) { }
static inline u64 skb_get_kcov_handle(struct sk_buff *skb) { (void)skb; return 0; }
#endif
