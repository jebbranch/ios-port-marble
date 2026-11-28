/* Linux notification encoding, delivered over XNU's native control socket.
 * No Linux socket, request dispatcher, namespace or pretend netlink family. */
#include "lx_pci.h"
#include <net/genetlink.h>
#include <net/cfg80211.h>
#include "cfg80211/core.h"
#include "nw_cfg80211_control.h"

struct sk_buff *nlmsg_new(size_t payload, gfp_t flags) {
  if (payload > NW_CONTROL_MAX_MESSAGE - 128) return NULL;
  struct sk_buff *message = alloc_skb(payload + NLMSG_HDRLEN + GENL_HDRLEN, flags);
  if (!message) return NULL;
  u8 bytes[NW_CONTROL_HEADER_SIZE]; struct nw_control_builder b; struct nw_control_view view;
  nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), NW_CONTROL_WIRELESS_EVENT, 0);
  nw_control_decode(bytes, b.length, &view);
  /* Original CQM helpers clear all of cb before delivery. */
  message->control_session = view.header.session;
  message->control_generation = view.header.generation;
  return message;
}
void nlmsg_free(struct sk_buff *message) {
  if (!message) return;
  memzero_explicit(message->data, message->len); kfree_skb(message);
}
struct nlattr *nla_reserve(struct sk_buff *message, int type, int length) {
  if (length < 0 || length > 65535 - NLA_HDRLEN) return NULL;
  size_t size = NLA_ALIGN((size_t)length + NLA_HDRLEN);
  if (skb_tailroom(message) < size) return NULL;
  struct nlattr *attribute = (void *)skb_put(message, size);
  attribute->nla_type = type; attribute->nla_len = length + NLA_HDRLEN;
  memset((u8 *)attribute + NLA_HDRLEN + length, 0, size - NLA_HDRLEN - length);
  return attribute;
}
int nla_put(struct sk_buff *message, int type, int length, const void *bytes) {
  if (length && !bytes) return -EINVAL;
  struct nlattr *attribute = nla_reserve(message, type, length);
  if (!attribute) return -EMSGSIZE;
  if (length) memcpy(nla_data(attribute), bytes, length);
  return 0;
}
int nla_put_u64_64bit(struct sk_buff *message, int type, u64 value, int padding) {
  if (((uintptr_t)skb_tail_pointer(message) + NLA_HDRLEN) & 7)
    if (nla_put_flag(message, padding)) return -EMSGSIZE;
  return nla_put(message, type, sizeof(value), &value);
}
int nla_put_msecs(struct sk_buff *m, int type, unsigned long value, int padding) {
  return nla_put_u64_64bit(m, type, jiffies_to_msecs(value), padding);
}
struct nlattr *nla_nest_start_noflag(struct sk_buff *message, int type) { return nla_reserve(message, type, 0); }
struct nlattr *nla_nest_start(struct sk_buff *message, int type) { return nla_reserve(message, type | NLA_F_NESTED, 0); }
int nla_nest_end(struct sk_buff *message, struct nlattr *start) {
  start->nla_len = skb_tail_pointer(message) - (u8 *)start; return message->len;
}
void nla_nest_cancel(struct sk_buff *message, struct nlattr *start) { skb_trim(message, (u8 *)start - message->data); }
void *genlmsg_put(struct sk_buff *message, u32 portid, u32 sequence,
                  const struct genl_family *family, int flags, u8 command) {
  if (skb_tailroom(message) < NLMSG_HDRLEN + GENL_HDRLEN) return NULL;
  struct nlmsghdr *header = (void *)skb_put(message, NLMSG_HDRLEN);
  *header = (struct nlmsghdr){ .nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN, .nlmsg_type = family->id,
    .nlmsg_flags = flags, .nlmsg_seq = sequence, .nlmsg_pid = portid };
  struct genlmsghdr *generic = (void *)skb_put(message, GENL_HDRLEN);
  *generic = (struct genlmsghdr){ .cmd = command, .version = family->version };
  return generic + 1;
}
void genlmsg_end(struct sk_buff *message, void *payload) {
  struct nlmsghdr *header = (void *)((u8 *)payload - GENL_HDRLEN - NLMSG_HDRLEN);
  header->nlmsg_len = skb_tail_pointer(message) - (u8 *)header;
}
void genlmsg_cancel(struct sk_buff *message, void *payload) {
  skb_trim(message, (u8 *)payload - GENL_HDRLEN - NLMSG_HDRLEN - message->data);
}
static int deliver(struct sk_buff *message) {
  size_t capacity = message->len + NW_CONTROL_HEADER_SIZE + 8;
  u8 *bytes = kmalloc(capacity, GFP_ATOMIC);
  if (!bytes) { nlmsg_free(message); return -ENOMEM; }
  struct nw_control_builder b;
  int result = nw_control_begin(&b, bytes, capacity, NW_CONTROL_EVENT, NW_CONTROL_WIRELESS_EVENT, 0,
    message->control_session, message->control_generation, 0) ||
    nw_control_put(&b, NW_CONTROL_ATTR_FRAME, message->data, message->len);
  if (!result) result = nw_cfg80211_event_send(&b);
  memzero_explicit(bytes, capacity); kfree(bytes); nlmsg_free(message);
  return result == NW_CONTROL_OK ? 0 : result == NW_CONTROL_STALE ? -ESTALE : -ENOBUFS;
}
int genlmsg_multicast_netns(const struct genl_family *family, struct net *net, struct sk_buff *message,
                           u32 portid, unsigned group, gfp_t flags) {
  (void)family; (void)net; (void)portid; (void)group; (void)flags; return deliver(message);
}
int genlmsg_multicast_allns(const struct genl_family *family, struct sk_buff *message, u32 portid, unsigned group) {
  return genlmsg_multicast_netns(family, NULL, message, portid, group, GFP_KERNEL);
}
int genlmsg_unicast(struct net *net, struct sk_buff *message, u32 portid) {
  (void)net;
  if (!portid) { nlmsg_free(message); return -ENOTCONN; }
  return deliver(message);
}
/* Interface capabilities are queried through native INFO/GET_CHANNELS. This
 * lifecycle notification retains identity without serializing Linux dumps. */
void nl80211_notify_wiphy(struct cfg80211_registered_device *rdev, enum nl80211_commands command) {
  static const struct genl_family family = { .id = 0x13, .version = 1 };
  struct sk_buff *message = nlmsg_new(64, GFP_KERNEL);
  if (!message) return;
  void *header = genlmsg_put(message, 0, 0, &family, 0, command);
  if (!header || nla_put_u32(message, NL80211_ATTR_WIPHY, rdev->wiphy_idx)) { nlmsg_free(message); return; }
  genlmsg_end(message, header); genlmsg_multicast_netns(&family, NULL, message, 0, 0, GFP_KERNEL);
}
