#ifndef LX_GSO_H
#define LX_GSO_H
#include "lx_skb.h"
struct sk_buff *skb_gso_segment(struct sk_buff *skb, netdev_features_t features);
#endif
