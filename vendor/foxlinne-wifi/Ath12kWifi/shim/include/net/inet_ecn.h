#ifndef LX_INET_ECN_H
#define LX_INET_ECN_H
#include "lx_skb.h"
int skb_get_dsfield(const struct sk_buff *skb);
int INET_ECN_set_ce(struct sk_buff *skb);
#endif
