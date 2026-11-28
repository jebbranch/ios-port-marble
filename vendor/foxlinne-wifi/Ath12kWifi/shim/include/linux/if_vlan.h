#ifndef LX_IF_VLAN_H
#define LX_IF_VLAN_H
#include "lx_skb.h"
#define VLAN_HLEN 4
#define VLAN_ETH_HLEN 18
#define VLAN_PRIO_MASK 0xe000
#define VLAN_PRIO_SHIFT 13
#define VLAN_VID_MASK 0x0fff
struct vlan_ethhdr {
  u8 h_dest[ETH_ALEN], h_source[ETH_ALEN];
  __be16 h_vlan_proto, h_vlan_TCI, h_vlan_encapsulated_proto;
} __packed;
static inline bool skb_vlan_tag_present(const struct sk_buff *skb) { return skb->vlan_present; }
static inline u16 skb_vlan_tag_get(const struct sk_buff *skb) { return skb->vlan_tci; }
static inline void __vlan_hwaccel_put_tag(struct sk_buff *skb, __be16 proto, u16 tci) {
  skb->vlan_proto = proto; skb->vlan_tci = tci; skb->vlan_present = true;
}
#endif
