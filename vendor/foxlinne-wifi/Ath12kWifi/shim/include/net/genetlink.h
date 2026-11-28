/* Original Linux event serializers on the native transport. No request family. */
#ifndef LX_GENETLINK_H
#define LX_GENETLINK_H
#include "lx_skb.h"
#include <net/netlink.h>
struct genl_info;
struct genl_family { u16 id; u8 version; };
struct genlmsghdr { u8 cmd, version; u16 reserved; };
#define GENL_HDRLEN 4
void *genlmsg_put(struct sk_buff *, u32 portid, u32 sequence, const struct genl_family *, int flags, u8 command);
void genlmsg_end(struct sk_buff *, void *header);
void genlmsg_cancel(struct sk_buff *, void *header);
int genlmsg_multicast_netns(const struct genl_family *, struct net *, struct sk_buff *, u32 portid, unsigned group, gfp_t flags);
int genlmsg_multicast_allns(const struct genl_family *, struct sk_buff *, u32 portid, unsigned group);
int genlmsg_unicast(struct net *, struct sk_buff *, u32 portid);
struct genl_multicast_group;
#define MODULE_ALIAS_GENL_FAMILY(name)
#endif
