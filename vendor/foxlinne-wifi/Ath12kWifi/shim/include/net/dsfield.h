#ifndef LX_DSFIELD_H
#define LX_DSFIELD_H
#include "lx_skb.h"
static inline u8 ipv4_get_dsfield(const void *header) { return ((const u8 *)header)[1]; }
static inline u8 ipv6_get_dsfield(const void *header) {
  const u8 *bytes = header; return (u8)((bytes[0] & 15) << 4 | bytes[1] >> 4);
}
#define ip_hdr(skb) ((void *)skb_network_header(skb))
#define ipv6_hdr(skb) ((void *)skb_network_header(skb))
#endif
