#include "lx_os.h"
#ifndef LX_NETLINK_H
#define LX_NETLINK_H
/* Event encoding only. Requests use the native control API, not netlink. */
struct sk_buff;
struct nlattr { u16 nla_len, nla_type; };
struct nlmsghdr { u32 nlmsg_len; u16 nlmsg_type, nlmsg_flags; u32 nlmsg_seq, nlmsg_pid; };
#define NLA_ALIGNTO 4
#define NLA_ALIGN(n) ALIGN((n), NLA_ALIGNTO)
#define NLA_HDRLEN 4
#define NLA_F_NESTED (1U << 15)
#define NLA_F_NET_BYTEORDER (1U << 14)
#define NLA_TYPE_MASK (~(NLA_F_NESTED | NLA_F_NET_BYTEORDER))
#define NLMSG_HDRLEN 16
#define NLMSG_DEFAULT_SIZE 4096
#define NLM_F_MULTI 2
static inline void *nla_data(const struct nlattr *a) { return (u8 *)a + NLA_HDRLEN; }
static inline int nla_len(const struct nlattr *a) { return a->nla_len - NLA_HDRLEN; }
static inline unsigned nla_type(const struct nlattr *a) { return a->nla_type & NLA_TYPE_MASK; }
int nla_put(struct sk_buff *message, int type, int length, const void *bytes);
struct nlattr *nla_reserve(struct sk_buff *message, int type, int length);
struct nlattr *nla_nest_start_noflag(struct sk_buff *message, int type);
struct nlattr *nla_nest_start(struct sk_buff *message, int type);
int nla_nest_end(struct sk_buff *message, struct nlattr *start);
void nla_nest_cancel(struct sk_buff *message, struct nlattr *start);
struct sk_buff *nlmsg_new(size_t size, gfp_t flags);
void nlmsg_free(struct sk_buff *message);
static inline int nla_put_flag(struct sk_buff *m, int t) { return nla_put(m, t, 0, NULL); }
#define LX_NLA_SCALAR(name, type) \
static inline int nla_put_##name(struct sk_buff *m, int t, type v) { return nla_put(m, t, sizeof(v), &v); } \
static inline type nla_get_##name(const struct nlattr *a) { type v; memcpy(&v, nla_data(a), sizeof(v)); return v; }
LX_NLA_SCALAR(u8, u8)
LX_NLA_SCALAR(s8, s8)
LX_NLA_SCALAR(u16, u16)
LX_NLA_SCALAR(u32, u32)
LX_NLA_SCALAR(s32, s32)
LX_NLA_SCALAR(u64, u64)
#undef LX_NLA_SCALAR
int nla_put_u64_64bit(struct sk_buff *m, int type, u64 value, int padding);
static inline int nla_put_s64(struct sk_buff *m, int t, s64 v, int pad) { return nla_put_u64_64bit(m, t, (u64)v, pad); }
int nla_put_msecs(struct sk_buff *m, int t, unsigned long v, int pad);
static inline int nla_put_string(struct sk_buff *m, int t, const char *s) { return nla_put(m, t, strlen(s) + 1, s); }
static inline bool nla_get_flag(const struct nlattr *a) { return a != NULL; }
static inline size_t nla_total_size(size_t n) { return NLA_ALIGN(n + NLA_HDRLEN); }
static inline size_t nla_total_size_64bit(size_t n) { return nla_total_size(n) + nla_total_size(0); }
struct netlink_ext_ack { const char *_msg; const struct nlattr *bad_attr; u8 cookie[20], cookie_len; };
#define NL_SET_ERR_MSG(extack, message) do { struct netlink_ext_ack *_ack = (extack); if (_ack) _ack->_msg = (message); } while (0)
#endif
