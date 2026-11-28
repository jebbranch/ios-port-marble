/* Packet buffers and the network device bits the wireless headers refer to. */
#ifndef LX_SKB_H
#define LX_SKB_H
#include "lx_net.h"
#include <linux/netdev_features.h>
#include <linux/rbtree_types.h>
typedef u8 __u8; typedef u16 __u16; typedef u32 __u32x; typedef s8 __s8; typedef s16 __s16; typedef s32 __s32; typedef u64 __u64;
#include <uapi/linux/rfkill.h>
#include <linux/netdevice_enums.h>
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_P_TDLS 0x890d
#define ETH_P_PAE 0x888e
#define ETH_P_PREAUTH 0x88c7
#define ETH_P_IP 0x0800
#define ETH_P_IPV6 0x86dd
#define ETH_P_ARP 0x0806
#define ETH_P_AARP 0x80f3
#define ETH_P_IPX 0x8137
#define ETH_P_MPLS_UC 0x8847
#define ETH_P_MPLS_MC 0x8848
#define ETH_P_80221 0x8917
#define ETH_P_802_2 0x0004
#define ETH_P_802_3 0x0001
#define ETH_P_802_3_MIN 0x0600
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define NET_SKB_PAD 32
#define ETH_P_8021Q 0x8100
#define ETH_FCS_LEN 4
#define ETH_ZLEN 60
#define NSEC_PER_USEC 1000L
#define NSEC_PER_SEC 1000000000L
#define CLOCK_BOOTTIME 7
#define ETHTOOL_FWVERS_LEN 32
#define NETDEV_ALIGN 32
#define IFNAMSIZ 16
#define offsetofend(t, m) (__builtin_offsetof(t, m) + sizeof(((t *)0)->m))
#define hweight8(x) __builtin_popcount((u8)(x))
#define hweight16(x) __builtin_popcount((u16)(x))
#define hweight32(x) __builtin_popcount((u32)(x))
#define hweight64(x) __builtin_popcountll(x)
#define hweight_long(x) __builtin_popcountl(x)
static inline u8 u8_get_bits(u8 v, u8 m) { return (v & m) >> __builtin_ctz(m); }
static inline u16 u16_get_bits(u16 v, u16 m) { return (v & m) >> __builtin_ctz(m); }
static inline u32 u32_get_bits(u32 v, u32 m) { return (v & m) >> __builtin_ctz(m); }
static inline u64 u64_get_bits(u64 v, u64 m) { return (v & m) >> __builtin_ctzll(m); }
static inline u8 u8_encode_bits(u8 v, u8 m) { return (u8)(v << __builtin_ctz(m)) & m; }
static inline u16 u16_encode_bits(u16 v, u16 m) { return (u16)(v << __builtin_ctz(m)) & m; }
static inline u32 u32_encode_bits(u32 v, u32 m) { return (v << __builtin_ctz(m)) & m; }
static inline u64 u64_encode_bits(u64 v, u64 m) { return (v << __builtin_ctzll(m)) & m; }
static inline u32 u32_replace_bits(u32 old, u32 v, u32 m) { return (old & ~m) | u32_encode_bits(v, m); }
static inline u8 u8_replace_bits(u8 old, u8 v, u8 m) { return (old & ~m) | u8_encode_bits(v, m); }
typedef struct { long long v; } atomic64_t;
static inline s64 atomic64_read(const atomic64_t *p) { return __atomic_load_n(&p->v, __ATOMIC_SEQ_CST); }
static inline void atomic64_set(atomic64_t *p, s64 value) { __atomic_store_n(&p->v, value, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *p) { return __atomic_add_fetch(&p->v, 1, __ATOMIC_SEQ_CST); }
typedef struct { int unused; } possible_net_t;
#define write_pnet(p, n) ((void)0)
#define read_pnet(p) (&init_net)
struct netdev_hw_addr_list { struct list_head list; int count; };
struct netdev_hw_addr { struct list_head list; u8 addr[32], type; unsigned refcount, sync_cnt; };
struct net_device_stats { u64 rx_packets, rx_bytes, rx_dropped, tx_packets, tx_bytes, tx_dropped; };
struct lx_netdev_tx;
struct lx_netdev_tx_stats { u64 accepted, queue_full, busy, dispatched, purged, peak_packets, peak_bytes; };
struct net_device {
  char name[IFNAMSIZ]; u8 dev_addr[ETH_ALEN], perm_addr[ETH_ALEN];
  void *allocation; bool registered, running, netns_immutable;
  bool free_requested;
  struct work_struct free_work;
  struct device dev;
  u32 flags, priv_flags, mtu, min_mtu, max_mtu; void *priv;
  netdev_features_t features, hw_features;
  int ifindex; struct net *nd_net; u16 type, hard_header_len;
  u8 addr_len; bool carrier, needs_free_netdev;
  void (*carrier_changed)(struct net_device *, bool);
  unsigned tx_backlog; unsigned long stopped_queues;
  unsigned num_tx_queues, real_num_tx_queues, tx_queue_len, tx_memory_limit;
  struct lx_netdev_tx *tx;
  struct wireless_dev *ieee80211_ptr;
  const struct net_device_ops *netdev_ops;
  const struct ethtool_ops *ethtool_ops;
  struct netdev_hw_addr_list mc;
  spinlock_t addr_list_lock;
  struct net_device_stats stats;
  unsigned needed_headroom, needed_tailroom, pcpu_stat_type;
  /* The XNU boundary consumes skb ownership and delivers the Ethernet frame. */
  int (*input)(struct net_device *, struct sk_buff *);
  void (*input_flush)(struct net_device *);
  void (*queue_wake)(struct net_device *);
};
#define IFF_UP 0x1
#define IFF_ALLMULTI 0x200
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 0x10
static inline bool netif_carrier_ok(const struct net_device *dev) { return dev->carrier; }
void netif_carrier_on(struct net_device *dev);
void netif_carrier_off(struct net_device *dev);
#define netdev_info(dev, ...) lx_log(6, __VA_ARGS__)
typedef int netdev_tx_t;
enum tc_setup_type {
  TC_QUERY_CAPS, TC_SETUP_QDISC_MQPRIO, TC_SETUP_CLSU32, TC_SETUP_CLSFLOWER,
  TC_SETUP_CLSMATCHALL, TC_SETUP_CLSBPF, TC_SETUP_BLOCK,
  TC_SETUP_QDISC_CBS, TC_SETUP_QDISC_RED, TC_SETUP_QDISC_PRIO,
  TC_SETUP_QDISC_MQ, TC_SETUP_QDISC_ETF, TC_SETUP_ROOT_QDISC,
  TC_SETUP_QDISC_GRED, TC_SETUP_QDISC_TAPRIO, TC_SETUP_FT,
  TC_SETUP_QDISC_ETS, TC_SETUP_QDISC_TBF, TC_SETUP_QDISC_FIFO,
  TC_SETUP_QDISC_HTB, TC_SETUP_ACT
};
struct net_device_path_ctx { const struct net_device *dev; u8 daddr[ETH_ALEN]; };
struct net_device_path;
struct net_device_ops {
  int (*ndo_init)(struct net_device *);
  int (*ndo_open)(struct net_device *);
  int (*ndo_stop)(struct net_device *);
  void (*ndo_uninit)(struct net_device *);
  netdev_tx_t (*ndo_start_xmit)(struct sk_buff *, struct net_device *);
  void (*ndo_set_rx_mode)(struct net_device *);
  int (*ndo_set_mac_address)(struct net_device *, void *);
  int (*ndo_setup_tc)(struct net_device *, enum tc_setup_type, void *);
  u16 (*ndo_select_queue)(struct net_device *, struct sk_buff *, struct net_device *);
  int (*ndo_fill_forward_path)(struct net_device_path_ctx *, struct net_device_path *);
};
static inline void *netdev_priv(const struct net_device *dev) { return dev->priv; }
static inline void __hw_addr_init(struct netdev_hw_addr_list *list) { INIT_LIST_HEAD(&list->list); list->count = 0; }
#define NET_NAME_ENUM 1
#define SET_NETDEV_DEV(ndev, parent_device) ((ndev)->dev.parent = (parent_device))
#define SET_NETDEV_DEVTYPE(ndev, dev_type) ((ndev)->dev.type = (dev_type))
static inline void dev_net_set(struct net_device *dev, struct net *net) { dev->nd_net = net; }
static inline void eth_hw_addr_set(struct net_device *dev, const u8 *addr) { memcpy(dev->dev_addr, addr, ETH_ALEN); }
void ether_setup(struct net_device *dev);
int eth_mac_addr(struct net_device *dev, void *address);
struct net_device *alloc_netdev_mqs(int priv_size, const char *name, u8 name_assign_type,
                                   void (*setup)(struct net_device *), unsigned txqs, unsigned rxqs);
void free_netdev(struct net_device *dev);
void dev_hold(struct net_device *dev);
void dev_put(struct net_device *dev);
int dev_alloc_name(struct net_device *dev, const char *name);
int dev_change_net_namespace(struct net_device *dev, struct net *net, const char *name_pattern);
/* XNU exposes native statistics, without Linux SIOCETHTOOL or its ops table. */
static inline void netdev_set_default_ethtool_ops(struct net_device *dev, const struct ethtool_ops *ops) { (void)ops; dev->ethtool_ops = NULL; }
struct notifier_block;
struct netlink_ext_ack;
struct netdev_notifier_info { struct net_device *dev; };
static inline struct net_device *netdev_notifier_info_to_dev(void *info) { return ((struct netdev_notifier_info *)info)->dev; }
int register_netdevice_notifier(struct notifier_block *notifier);
int unregister_netdevice_notifier(struct notifier_block *notifier);
int register_netdevice(struct net_device *dev);
void unregister_netdevice(struct net_device *dev);
int dev_open(struct net_device *dev, struct netlink_ext_ack *extack);
int __hw_addr_add(struct netdev_hw_addr_list *list, const u8 *addr, int len, u8 type);
int __hw_addr_del(struct netdev_hw_addr_list *list, const u8 *addr, int len, u8 type);
int __hw_addr_sync(struct netdev_hw_addr_list *to, struct netdev_hw_addr_list *from, int len);
void __hw_addr_unsync(struct netdev_hw_addr_list *to, struct netdev_hw_addr_list *from, int len);
void __hw_addr_flush(struct netdev_hw_addr_list *list);
static inline bool netif_running(const struct net_device *dev) { return dev->running; }
static inline bool netif_is_bridge_port(const struct net_device *dev) { return !!(dev->priv_flags & IFF_BRIDGE_PORT); }
int netif_receive_skb(struct sk_buff *skb);
int netif_rx(struct sk_buff *skb);
void netif_receive_skb_list(struct list_head *list);
void dev_sw_netstats_rx_add(struct net_device *dev, unsigned bytes);
void dev_sw_netstats_tx_add(struct net_device *dev, unsigned packets, unsigned bytes);
void netif_stop_subqueue(struct net_device *dev, u16 queue);
void netif_wake_subqueue(struct net_device *dev, u16 queue);
bool __netif_subqueue_stopped(const struct net_device *dev, u16 queue);
#define netif_addr_lock_bh(d) spin_lock_bh(&(d)->addr_list_lock)
#define netif_addr_unlock_bh(d) spin_unlock_bh(&(d)->addr_list_lock)
int dev_close(struct net_device *dev);
__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev);
int dev_queue_xmit(struct sk_buff *skb);
void lx_netdev_tx_kick(struct net_device *dev);
void lx_netdev_tx_stats_get(const struct net_device *dev, struct lx_netdev_tx_stats *out);
struct rfkill;
struct skb_shared_info;
struct sk_buff {
  union { struct { struct sk_buff *next, *prev; }; struct list_head list; };
  u8 *head, *data, *tail, *end;
  struct skb_shared_info *shared; /* Heap metadata of the data buffer; clones share it. */
  u32 len, data_len, truesize;
  char cb[48] __attribute__((aligned(8)));
  struct sock *sk; struct net_device *dev;
  u16 protocol, queue_mapping; u8 ip_summed, pkt_type, cloned; u32 priority, mark, hash; u64 tstamp;
  u16 network_header, transport_header, mac_header, inner_transport_header;
  u16 csum_start, csum_offset;
  bool encapsulation, csum_not_inet, head_frag, vlan_present;
  u16 vlan_tci, vlan_proto;
  unsigned users;
  void (*destructor)(struct sk_buff *);
  bool wifi_acked_valid, wifi_acked;
  u64 control_session, control_generation; /* Notification lifetime; Linux owns all of cb. */
};
struct skb_shared_hwtstamps { ktime_t hwtstamp; };
/* A native DMA allocation is a refcounted page object. Its descriptor lives in
 * the heap object that also holds the shared info and survives while any skb
 * fragment references it. */
struct page {
  struct rb_node node; u8 *address; size_t length, allocation_size;
  dma_addr_t bus; unsigned references;
  struct page *pool_next;
};
typedef struct { struct page *page; unsigned offset, size; } skb_frag_t;
#define MAX_SKB_FRAGS 17
void get_page(struct page *page);
void put_page(struct page *page);
struct page *virt_to_head_page(const void *address);
static inline void *page_address(const struct page *page) { return page->address; }
static inline struct page *skb_frag_page(const skb_frag_t *frag) { return frag->page; }
static inline void *skb_frag_address(const skb_frag_t *frag) { return frag->page->address + frag->offset; }
static inline unsigned skb_frag_size(const skb_frag_t *frag) { return frag->size; }
/* Shared by clones; the DMA allocation is released by its last owner. Kept in
 * ordinary heap memory: the board may map the DMA window uncached for the CPU,
 * so the window holds packet bytes only and no reference counts, tree nodes or
 * fragment tables. */
struct skb_shared_info {
  u8 nr_frags; u16 gso_size; struct sk_buff *frag_list; unsigned int dataref;
  struct skb_shared_hwtstamps hwtstamps;
  struct page owner;
  skb_frag_t frags[MAX_SKB_FRAGS];
};
#define skb_shinfo(s) ((s)->shared)
#define skb_hwtstamps(s) (&skb_shinfo(s)->hwtstamps)
static inline bool skb_is_gso(const struct sk_buff *skb) { return skb_shinfo(skb)->gso_size != 0; }
static inline bool skb_has_frag_list(const struct sk_buff *skb) { return skb_shinfo(skb)->frag_list != NULL; }
static inline struct sk_buff *skb_get(struct sk_buff *skb) {
  __atomic_fetch_add(&skb->users, 1u, __ATOMIC_RELAXED); return skb;
}
static inline bool skb_shared(const struct sk_buff *skb) { return __atomic_load_n(&skb->users, __ATOMIC_ACQUIRE) > 1; }
struct sk_buff_head { struct sk_buff *next, *prev; u32 qlen; spinlock_t lock; };
struct sk_buff *alloc_skb(unsigned int size, gfp_t f);
struct sk_buff *__dev_alloc_skb(unsigned int size, gfp_t f);
#define dev_alloc_skb(n) __dev_alloc_skb(n, 0)
#define netdev_alloc_skb(d, n) __dev_alloc_skb(n, 0)
void kfree_skb(struct sk_buff *skb);
void kfree_skb_list(struct sk_buff *skb);
void skb_complete_wifi_ack(struct sk_buff *skb, bool acked);
static inline void skb_orphan(struct sk_buff *skb) {
  if (skb->destructor) { void (*release)(struct sk_buff *) = skb->destructor; skb->destructor = NULL; release(skb); }
  skb->sk = NULL;
}
static inline void skb_mark_not_on_list(struct sk_buff *skb) { skb->next = NULL; skb->prev = NULL; }
static inline void skb_list_del_init(struct sk_buff *skb) { list_del(&skb->list); skb_mark_not_on_list(skb); }
#define skb_list_walk_safe(first, skb, next_skb) for ((skb) = (first); (skb) && ((next_skb) = (skb)->next, 1); (skb) = (next_skb))
/* XNU verifies CHECKSUM_NONE in software; stale COMPLETE sums must not survive
 * an unaccounted header pull. UNNECESSARY remains valid for the payload. */
static inline void skb_postpull_rcsum(struct sk_buff *skb, const void *start, unsigned len) {
  (void)start; (void)len;
  if (skb->ip_summed == 2) skb->ip_summed = 0;
}
#define dev_kfree_skb(s) kfree_skb(s)
#define dev_kfree_skb_any(s) kfree_skb(s)
#define dev_kfree_skb_irq(s) kfree_skb(s)
#define consume_skb(s) kfree_skb(s)
#define kfree_skb_reason(s, r) kfree_skb(s)
static inline u32 skb_headroom(const struct sk_buff *s) { return s->data - s->head; }
static inline u32 skb_tailroom(const struct sk_buff *s) { return s->end - s->tail; }
static inline void skb_set_tail_pointer(struct sk_buff *s, int offset) { s->tail = s->data + offset; }
static inline void skb_reserve(struct sk_buff *s, int n) { s->data += n; s->tail += n; }
static inline void *skb_put(struct sk_buff *s, u32 n) { void *p = s->tail; s->tail += n; s->len += n; return p; }
static inline void *skb_put_zero(struct sk_buff *s, u32 n) { void *p = skb_put(s, n); memset(p, 0, n); return p; }
static inline void *skb_put_data(struct sk_buff *s, const void *d, u32 n) { void *p = skb_put(s, n); memcpy(p, d, n); return p; }
static inline void skb_put_u8(struct sk_buff *s, u8 value) { *(u8 *)skb_put(s, 1) = value; }
static inline void *skb_push(struct sk_buff *s, u32 n) { s->data -= n; s->len += n; return s->data; }
static inline void *skb_pull(struct sk_buff *s, u32 n) { if (n > s->len) return 0; s->len -= n; return s->data += n; }
static inline void skb_trim(struct sk_buff *s, u32 n) { if (s->len > n) { s->len = n; s->tail = s->data + n; } }
int skb_linearize(struct sk_buff *s);
#define __skb_linearize(s) skb_linearize(s)
static inline bool skb_needs_linearize(struct sk_buff *s, netdev_features_t features) {
  return s->data_len && ((skb_has_frag_list(s) && !(features & NETIF_F_FRAGLIST)) ||
                        (skb_shinfo(s)->nr_frags && !(features & NETIF_F_SG)));
}
static inline u32 skb_headlen(const struct sk_buff *s) { return s->len - s->data_len; }
bool pskb_may_pull(struct sk_buff *s, unsigned len);
void skb_add_rx_frag(struct sk_buff *s, int index, struct page *page, int offset, int size, unsigned truesize);
int __pskb_trim(struct sk_buff *s, unsigned len);
#define pskb_trim(s, len) __pskb_trim(s, len)
static inline void *pskb_pull(struct sk_buff *s, unsigned len) { return pskb_may_pull(s, len) ? skb_pull(s, len) : NULL; }
static inline int skb_is_nonlinear(const struct sk_buff *s) { return s->data_len != 0; }
static inline unsigned char *skb_network_header(const struct sk_buff *s) { return s->head + s->network_header; }
static inline unsigned char *skb_mac_header(const struct sk_buff *s) { return s->head + s->mac_header; }
static inline bool skb_mac_header_was_set(const struct sk_buff *s) { return s->mac_header != (u16)~0u; }
static inline void skb_reset_mac_header(struct sk_buff *s) { s->mac_header = (u16)(s->data - s->head); }
static inline void skb_set_mac_header(struct sk_buff *s, int off) { s->mac_header = (u16)(s->data - s->head + off); }
static inline int skb_network_offset(const struct sk_buff *s) { return skb_network_header(s) - s->data; }
static inline void skb_reset_network_header(struct sk_buff *s) { s->network_header = s->data - s->head; }
static inline void skb_set_network_header(struct sk_buff *s, int off) { s->network_header = s->data - s->head + off; }
static inline void skb_set_transport_header(struct sk_buff *s, int off) { s->transport_header = (u16)(s->data - s->head + off); }
static inline void skb_set_inner_transport_header(struct sk_buff *s, int off) { s->inner_transport_header = (u16)(s->data - s->head + off); }
static inline int skb_checksum_start_offset(const struct sk_buff *s) { return (int)s->csum_start - (int)skb_headroom(s); }
int skb_csum_hwoffload_help(struct sk_buff *s, netdev_features_t features);
static inline int skb_cloned(const struct sk_buff *s) {
  return s->cloned && __atomic_load_n(&skb_shinfo(s)->dataref, __ATOMIC_ACQUIRE) > 1;
}
int skb_cow_head(struct sk_buff *s, unsigned int h);
static inline int skb_clone_writable(const struct sk_buff *s, unsigned len) { return !skb_cloned(s) && len <= skb_headlen(s); }
int pskb_expand_head(struct sk_buff *s, int a, int b, gfp_t f);
int skb_ensure_writable(struct sk_buff *s, int n);
struct sk_buff *skb_copy(const struct sk_buff *s, gfp_t f);
struct sk_buff *skb_clone(struct sk_buff *s, gfp_t f);
struct sk_buff *skb_clone_sk(struct sk_buff *s);
struct sk_buff *skb_share_check(struct sk_buff *s, gfp_t flags);
struct sk_buff *skb_copy_expand(const struct sk_buff *s, int h, int t, gfp_t f);
u32 skb_get_hash(struct sk_buff *s);
int skb_copy_bits(const struct sk_buff *s, int offset, void *to, int length);
static inline void *skb_header_pointer(const struct sk_buff *s, int offset, int length, void *buffer) {
  if (length < 0 || (s64)offset < -(s64)skb_headroom(s)) return NULL;
  if ((s64)offset + length <= skb_headlen(s)) return s->data + offset;
  return skb_copy_bits(s, offset, buffer, length) ? NULL : buffer;
}
static inline void __skb_queue_head_init(struct sk_buff_head *q) { q->next = q->prev = (struct sk_buff *)q; q->qlen = 0; }
static inline void skb_queue_head_init(struct sk_buff_head *q) { __skb_queue_head_init(q); spin_lock_init(&q->lock); }
static inline int skb_queue_empty(const struct sk_buff_head *q) { return READ_ONCE(q->next) == (const struct sk_buff *)q; }
static inline u32 skb_queue_len(const struct sk_buff_head *q) { return READ_ONCE(q->qlen); }
static inline void __skb_queue_tail(struct sk_buff_head *q, struct sk_buff *s) { struct sk_buff *l = q->prev; s->next = (struct sk_buff *)q; s->prev = l; l->next = s; q->prev = s; q->qlen++; }
static inline void __skb_queue_head(struct sk_buff_head *q, struct sk_buff *s) { struct sk_buff *f = q->next; s->next = f; s->prev = (struct sk_buff *)q; f->prev = s; q->next = s; q->qlen++; }
static inline struct sk_buff *skb_peek(const struct sk_buff_head *q) { struct sk_buff *s = q->next; return s == (const struct sk_buff *)q ? 0 : s; }
#define __skb_peek(q) skb_peek(q)
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *q) { struct sk_buff *s = skb_peek(q); if (s) { q->next = s->next; s->next->prev = (struct sk_buff *)q; q->qlen--; s->next = s->prev = 0; } return s; }
static inline void __skb_unlink(struct sk_buff *s, struct sk_buff_head *q) { s->prev->next = s->next; s->next->prev = s->prev; q->qlen--; s->next = s->prev = 0; }
static inline void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *s) { unsigned long flags; spin_lock_irqsave(&q->lock, flags); __skb_queue_tail(q, s); spin_unlock_irqrestore(&q->lock, flags); }
static inline bool skb_queue_is_last(const struct sk_buff_head *q, const struct sk_buff *s) { return s->next == (const struct sk_buff *)q; }
static inline struct sk_buff *skb_queue_next(const struct sk_buff_head *q, const struct sk_buff *s) { return skb_queue_is_last(q, s) ? NULL : s->next; }
static inline void skb_copy_queue_mapping(struct sk_buff *to, const struct sk_buff *from) { to->queue_mapping = from->queue_mapping; }
static inline void skb_queue_head(struct sk_buff_head *q, struct sk_buff *s) { unsigned long flags; spin_lock_irqsave(&q->lock, flags); __skb_queue_head(q, s); spin_unlock_irqrestore(&q->lock, flags); }
static inline struct sk_buff *skb_dequeue(struct sk_buff_head *q) { unsigned long flags; spin_lock_irqsave(&q->lock, flags); struct sk_buff *s = __skb_dequeue(q); spin_unlock_irqrestore(&q->lock, flags); return s; }
static inline void skb_unlink(struct sk_buff *s, struct sk_buff_head *q) { unsigned long flags; spin_lock_irqsave(&q->lock, flags); __skb_unlink(s, q); spin_unlock_irqrestore(&q->lock, flags); }
static inline void skb_queue_purge(struct sk_buff_head *q) { struct sk_buff *s; while ((s = skb_dequeue(q))) kfree_skb(s); }
static inline void __skb_queue_purge(struct sk_buff_head *q) { struct sk_buff *s; while ((s = __skb_dequeue(q))) kfree_skb(s); }
static inline void skb_queue_splice_tail_init(struct sk_buff_head *a, struct sk_buff_head *b) { struct sk_buff *s; while ((s = __skb_dequeue(a))) __skb_queue_tail(b, s); }
static inline void skb_queue_splice_tail(struct sk_buff_head *a, struct sk_buff_head *b) {
  if (!a->qlen) return;
  struct sk_buff *first = a->next, *last = a->prev, *previous = b->prev;
  first->prev = previous; previous->next = first; last->next = (struct sk_buff *)b;
  b->prev = last; b->qlen += a->qlen;
}
static inline void skb_queue_splice_init(struct sk_buff_head *a, struct sk_buff_head *b) {
  if (!a->qlen) return;
  struct sk_buff *first = a->next, *last = a->prev, *next = b->next;
  first->prev = (struct sk_buff *)b; b->next = first; last->next = next; next->prev = last;
  b->qlen += a->qlen; __skb_queue_head_init(a);
}
#define skb_queue_walk(q, s) for (s = (q)->next; s != (struct sk_buff *)(q); s = s->next)
#define skb_queue_walk_safe(q, s, t) for (s = (q)->next, t = s->next; s != (struct sk_buff *)(q); s = t, t = s->next)
#define skb_cb_lx(s) ((s)->cb)

#define struct_group(name, ...) union { struct { __VA_ARGS__ }; struct { __VA_ARGS__ } name; }
#define struct_group_attr(name, attrs, ...) union { struct { __VA_ARGS__ }; struct attrs { __VA_ARGS__ } name; }
#define __struct_group(tag, name, attrs, ...) union { struct { __VA_ARGS__ } attrs; struct tag { __VA_ARGS__ } attrs name; }
#define DECLARE_FLEX_ARRAY(t, n) struct { struct { } __empty_##n; t n[]; }
#ifndef __counted_by
#define __counted_by(x)
#endif
#define __counted_by_le(x)
#define __must_be_array(x) 0
#define memset_after(p, v, m) memset((char *)(p) + offsetofend(__typeof__(*(p)), m), v, sizeof(*(p)) - offsetofend(__typeof__(*(p)), m))
#define lockdep_is_held(l) 1
#define lockdep_rtnl_is_held() lx_rtnl_is_locked()
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw(p) READ_ONCE(p)
#define rcu_dereference_check(p, c) (p)
#define rcu_access_pointer(p) (p)
#define RCU_INIT_POINTER(p, v) ((p) = (v))
#define rcu_pointer_handoff(p) (p)
#define kfree_rcu(p, m) do { __typeof__(p) _lx_rcu_pointer = (p); if (_lx_rcu_pointer) lx_rcu_free(_lx_rcu_pointer, &_lx_rcu_pointer->m); } while (0)
#define call_rcu(h, f) lx_rcu_call((h), (f))
#define list_for_each_entry_rcu(p, h, m, ...) list_for_each_entry(p, h, m)
static inline void list_add_rcu(struct list_head *node, struct list_head *head) {
  node->next = head->next; node->prev = head; head->next->prev = node;
  __atomic_store_n(&head->next, node, __ATOMIC_RELEASE);
}
static inline void list_add_tail_rcu(struct list_head *node, struct list_head *head) {
  struct list_head *previous = head->prev;
  node->next = head; node->prev = previous; head->prev = node;
  rcu_assign_pointer(previous->next, node);
}
static inline void list_del_rcu(struct list_head *node) {
  node->next->prev = node->prev;
  rcu_assign_pointer(node->prev->next, node->next);
  node->prev = 0;
}
#define ktime_get_ns() ((u64)ktime_get())
#define ktime_get_boottime() ktime_get()
#define ktime_get_real_ns() ((u64)ktime_get())
#define get_random_bytes(p, n) do { for (size_t _i = 0; _i < (n); _i++) ((u8 *)(p))[_i] = (u8)lx_random_u32(); } while (0)
#define do_div(n, b) ({ u32 _r = (u32)((n) % (b)); (n) /= (b); _r; })
static inline u16 get_unaligned_be16(const void *p) { const u8 *b = p; return (b[0] << 8) | b[1]; }
static inline u32 get_unaligned_be32(const void *p) { const u8 *b = p; return ((u32)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
static inline u16 get_unaligned_le16x(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u16 le16_get_bits(__le16 v, u16 m) { return (v & m) >> __builtin_ctz(m); }
static inline u32 le32_get_bits(__le32 v, u32 m) { return (v & m) >> __builtin_ctz(m); }
static inline __le16 le16_encode_bits(u16 v, u16 m) { return (u16)(v << __builtin_ctz(m)) & m; }
static inline __le32 le32_encode_bits(u32 v, u32 m) { return (v << __builtin_ctz(m)) & m; }
static inline __le16 cpu_to_be16x(u16 v) { return (v >> 8) | (v << 8); }
#define cpu_to_be16(x) ((u16)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#define be16_to_cpu(x) cpu_to_be16(x)
#define cpu_to_be32(x) __builtin_bswap32((u32)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(x))
#define cpu_to_be64(x) __builtin_bswap64((u64)(x))
#define be64_to_cpu(x) __builtin_bswap64((u64)(x))
#define swab16(x) cpu_to_be16(x)
#define swab32(x) __builtin_bswap32((u32)(x))
#ifndef htons
#define htons(x) cpu_to_be16(x)
#endif
#ifndef ntohs
#define ntohs(x) cpu_to_be16(x)
#endif
#ifndef htonl
#define htonl(x) __builtin_bswap32((u32)(x))
#endif
#ifndef ntohl
#define ntohl(x) __builtin_bswap32((u32)(x))
#endif
static inline int is_multicast_ether_addr(const u8 *a) { return 0x01 & a[0]; }
static inline int is_unicast_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a); }
static inline int is_broadcast_ether_addr(const u8 *a) { return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff; }
static inline int is_zero_ether_addr(const u8 *a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline int is_valid_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a) && !is_zero_ether_addr(a); }
static inline int ether_addr_equal(const u8 *a, const u8 *b) { return !memcmp(a, b, 6); }
static inline void ether_addr_copy(u8 *d, const u8 *s) { memcpy(d, s, 6); }
static inline u64 ether_addr_to_u64(const u8 *a) { u64 v = 0; for (int i = 0; i < 6; i++) v = (v << 8) | a[i]; return v; }
static inline void u64_to_ether_addr(u64 v, u8 *a) { for (int i = 5; i >= 0; i--) { a[i] = v & 0xff; v >>= 8; } }
static inline void eth_zero_addr(u8 *a) { memset(a, 0, 6); }
static inline void eth_random_addr(u8 *a) {
  for (unsigned i = 0; i < ETH_ALEN; i++) a[i] = (u8)lx_random_u32();
  a[0] = (a[0] & 0xfe) | 2;
}
static inline void eth_broadcast_addr(u8 *a) { memset(a, 0xff, 6); }
#define EXPORT_SYMBOL_NS_GPL(a, b)
static inline void __set_bit(int n, unsigned long *p) { p[n / 64] |= 1UL << (n % 64); }
static inline void __clear_bit(int n, unsigned long *p) { p[n / 64] &= ~(1UL << (n % 64)); }
static inline int __test_and_set_bit(int n, unsigned long *p) { int o = (p[n / 64] >> (n % 64)) & 1; __set_bit(n, p); return o; }
#define __acquires(x)
#define __releases(x)
#define __must_hold(x)
#define __acquire(x)
#define __release(x)
#define __attribute_const__ __attribute__((const))
#define __ro_after_init
#define __percpu
/* All Linux callbacks execute on the module dispatcher; its logical CPU is 0. */
#define alloc_percpu_gfp(type, flags) ((type *)lx_alloc(sizeof(type), 1))
#define free_percpu(p) lx_free(p)
#define per_cpu_ptr(p, cpu) (p)
#define this_cpu_ptr(p) (p)
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define static_assert(c, ...) _Static_assert(c, "")
#define DEFINE_GUARD(name, type, lock, unlock)
#define DEFINE_GUARD_COND(...)
#define DEFINE_FREE(...)
#define __bitwise
#define __rcu_lx
#endif
