/* Kernel facilities the ath12k sources use beyond the basics: hash tables, polling contexts, device tree and PCI stand-ins. */
#ifndef LX_ATH_H
#define LX_ATH_H
#include "lx_skb.h"
/* names the driver's message prototypes mention before its own headers declare them */
struct ath12k_base; struct ath12k; struct ath12k_hw; struct ath12k_pci;
typedef struct { u8 b[16]; } guid_t;
typedef guid_t uuid_t;
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, d0, d1, d2, d3, d4, d5, d6, d7 } }
struct lock_class_key { int unused; };
#define lockdep_register_key(k)
#define lockdep_unregister_key(k)
#define lockdep_set_class(l, k)
#define RCU_LOCKDEP_WARN(c, ...) ((void)0)
#define rcu_read_lock_held() (lx_rcu_readers() != 0)
#define rcu_read_lock_bh_held() rcu_read_lock_held()
#define rcu_read_lock_bh() rcu_read_lock()
#define rcu_read_unlock_bh() rcu_read_unlock()

/* rhashtable: chained buckets keyed by a byte range inside the element */
struct rhash_head { struct rhash_head *next; };
struct rhlist_head { struct rhash_head rhead; struct rhlist_head *next; };
struct rhashtable_params {
  u16 nelem_hint, key_len, key_offset, head_offset; u32 max_size; u16 min_size; bool automatic_shrinking;
  int (*obj_cmpfn)(void *, const void *); u32 (*hashfn)(const void *, u32, u32); void *obj_hashfn;
};
struct rhashtable { struct rhash_head *head; struct rhashtable_params p; unsigned int n; };
struct rhltable { struct rhashtable ht; };
int rhltable_init(struct rhltable *table, const struct rhashtable_params *params);
void rhltable_destroy(struct rhltable *table);
int rhltable_insert(struct rhltable *table, struct rhlist_head *object, struct rhashtable_params params);
int rhltable_remove(struct rhltable *table, struct rhlist_head *object, struct rhashtable_params params);
struct rhlist_head *rhltable_lookup(struct rhltable *table, const void *key, struct rhashtable_params params);
#define rhl_for_each_entry_rcu(pos, node, head, member) \
  for ((node) = (head); (node) && (((pos) = container_of((node), __typeof__(*(pos)), member)), 1); (node) = (node)->next)
struct hlist_node { struct hlist_node *next, **pprev; };
struct hlist_head { struct hlist_node *first; };
struct static_key_false { int enabled; };
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key_false name
#define DEFINE_STATIC_KEY_FALSE(name) struct static_key_false name = { 0 }
#define static_branch_unlikely(key) __atomic_load_n(&(key)->enabled, __ATOMIC_ACQUIRE)
#define static_branch_enable(key) __atomic_store_n(&(key)->enabled, 1, __ATOMIC_RELEASE)
#define static_branch_disable(key) __atomic_store_n(&(key)->enabled, 0, __ATOMIC_RELEASE)
int rhashtable_init(struct rhashtable *t, const struct rhashtable_params *p);
void rhashtable_destroy(struct rhashtable *t);
void *rhashtable_lookup_fast(struct rhashtable *t, const void *key, struct rhashtable_params p);
void *rhashtable_lookup_get_insert_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p);
int rhashtable_insert_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p);
int rhashtable_remove_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p);
#define rht_head_lx(x) (x)
#define IS_ERR_OR_NULL_LX(p) IS_ERR_OR_NULL(p)

/* polling context: the data path runs it from the dispatcher */
struct napi_struct {
  struct net_device *dev; int (*poll)(struct napi_struct *, int);
  int weight, scheduled, enabled, polling;
  struct list_head poll_list, rx_list; int state;
};
enum gro_result { GRO_MERGED, GRO_MERGED_FREE, GRO_HELD, GRO_NORMAL, GRO_CONSUMED };
typedef enum gro_result gro_result_t;
gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb);
void lx_napi_add(struct net_device *d, struct napi_struct *n, int (*poll)(struct napi_struct *, int), int weight);
void lx_napi_schedule(struct napi_struct *n);
bool lx_napi_complete_done(struct napi_struct *n, int done);
void lx_napi_del(struct napi_struct *n);
void lx_napi_enable(struct napi_struct *n);
void lx_napi_disable(struct napi_struct *n);
void lx_napi_synchronize(struct napi_struct *n);
bool lx_napi_schedule_prep(struct napi_struct *n);
#define netif_napi_add(d, n, p) lx_napi_add(d, n, p, 64)
#define netif_napi_add_weight(d, n, p, w) lx_napi_add(d, n, p, w)
#define netif_napi_del(n) lx_napi_del(n)
#define napi_enable(n) lx_napi_enable(n)
#define napi_disable(n) lx_napi_disable(n)
#define napi_schedule(n) lx_napi_schedule(n)
#define napi_complete_done(n, d) lx_napi_complete_done(n, d)
#define napi_synchronize(n) lx_napi_synchronize(n)
#define napi_schedule_prep(n) lx_napi_schedule_prep(n)
#define __napi_schedule(n) lx_napi_schedule(n)
#define init_dummy_netdev(d) do { memset(d, 0, sizeof *(d)); device_initialize(&(d)->dev); __hw_addr_init(&(d)->mc); } while (0)
#define alloc_netdev_dummy(n) alloc_netdev_mqs(n, "dummy%d", NET_NAME_ENUM, NULL, 1, 1)

struct dmi_header { u8 type, length; u16 handle; };
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); struct notifier_block *next; int priority; };
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 2)
static inline int notifier_from_errno(int error) { return error ? NOTIFY_STOP_MASK | (NOTIFY_OK - error) : NOTIFY_OK; }
static inline int notifier_to_errno(int result) { result &= ~NOTIFY_STOP_MASK; return result > NOTIFY_OK ? NOTIFY_OK - result : 0; }
struct atomic_notifier_head { int unused; };
#define atomic_notifier_chain_register(h, n) 0
#define atomic_notifier_chain_unregister(h, n) 0
#define panic_notifier_list lx_panic_list
extern struct atomic_notifier_head lx_panic_list;
int register_pm_notifier(struct notifier_block *n);
int unregister_pm_notifier(struct notifier_block *n);
int lx_pm_notify(unsigned long action);
struct device_node { int unused; };
#define of_property_read_string(np, n, out) (-ENOENT)
#define of_property_read_u32(np, n, out) (-ENOENT)
#define of_property_read_bool(np, n) 0
#define of_device_is_compatible(np, c) 0
#define of_machine_is_compatible(c) 0
#define of_reserved_mem_region_to_resource(np, index, resource) (-ENOENT)
#define of_find_property(np, n, l) ((void *)0)
#define of_property_present(np, n) (of_find_property(np, n, NULL) != NULL)
#define of_find_node_by_type(from, type) ((struct device_node *)NULL)
#define of_node_put(n) ((void)0)
#define of_get_child_by_name(n, c) ((struct device_node *)0)
#define of_get_property(np, n, l) ((const void *)0)
#define of_address_to_resource(np, i, r) (-ENOENT)
#define of_reserved_mem_lookup(n) ((void *)0)
#define of_parse_phandle(np, p, i) ((struct device_node *)0)
#define of_graph_get_remote_node(...) ((struct device_node *)0)
#define for_each_available_child_of_node_scoped(...) if (0)
#define of_match_node(m, n) ((void *)0)
#define of_match_device(m, d) ((void *)0)
#define device_property_read_string(d, n, o) (-ENOENT)
#define device_property_read_u32(d, n, o) (-ENOENT)
#ifndef prefetch
#define prefetch(p) __builtin_prefetch(p)
#endif
static inline void *memset32(u32 *p, u32 value, size_t count) { u32 *start=p; while (count--) *p++=value; return start; }
int device_get_mac_address(struct device *device, u8 *address);
#endif
