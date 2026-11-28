/* The PCI endpoint as the WiFi driver sees it. Config space, the register window and the message interrupts come from the board code. */
#ifndef LX_PCI_H
#define LX_PCI_H
#include "lx_ath.h"
#define PCI_VENDOR_ID_QCOM 0x17cb
#define PCI_ANY_ID (~0)
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_EXP_LNKCTL 0x10
#define PCI_EXP_LNKCTL_ASPMC 0x0003
#define PCI_EXP_LNKCTL_ASPM_L0S 0x0001
#define PCI_EXP_LNKCTL_ASPM_L1 0x0002
#define PCI_MSI_FLAGS 0x02
#define PCI_MSI_FLAGS_ENABLE 0x0001
#define PCI_MSI_ADDRESS_LO 0x04
#define PCI_MSI_ADDRESS_HI 0x08
#define PCI_MSI_DATA_32 0x08
#define PCI_MSI_DATA_64 0x0c
#define PCI_IRQ_MSI 0x2
#define PCI_IRQ_INTX 0x1
#define PCI_IRQ_ALL_TYPES 0x3
#define PCI_IRQ_MSIX 0x4
#define PCI_EXP_LNKSTA 0x12
#define PCI_EXP_LNKSTA_CLS 0x000f
#define PCI_EXP_LNKSTA_NLW 0x03f0
struct pci_device_id { u32 vendor, device, subvendor, subdevice, class, class_mask; unsigned long driver_data; };
#define PCI_DEVICE(v, d) .vendor = (v), .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
struct msi_msg { u32 address_lo, address_hi, data; };
struct msi_desc { struct msi_msg msg; struct { struct { u8 is_64; } msi_attrib; } pci; };
struct pci_bus { int number; };
struct pci_ops { int unused; };
struct pci_dev {
  u16 subsystem_vendor, subsystem_device;
  struct device dev; u16 vendor, device; u32 irq; struct pci_bus *bus; u8 revision;
  u32 msi_cap_off; int msi_enabled; unsigned int nvec; u16 msi_cap;
};
struct pci_driver {
  const char *name; const struct pci_device_id *id_table;
  int (*probe)(struct pci_dev *, const struct pci_device_id *); void (*remove)(struct pci_dev *);
  struct device_driver driver; void (*shutdown)(struct pci_dev *); const struct dev_pm_ops *pm;
};
struct dev_pm_ops { int unused; };
#define SIMPLE_DEV_PM_OPS(n, s, r) static const struct dev_pm_ops n = { 0 }
#define DEFINE_SIMPLE_DEV_PM_OPS(n, s, r) static const struct dev_pm_ops n = { 0 }
#define pm_sleep_ptr(p) (p)
#define pci_get_drvdata(p) dev_get_drvdata(&(p)->dev)
#define pci_set_drvdata(p, d) dev_set_drvdata(&(p)->dev, d)
int lx_pci_read_config(struct pci_dev *d, int off, int size, u32 *v);
int lx_pci_write_config(struct pci_dev *d, int off, int size, u32 v);
#define pci_read_config_dword(d, o, v) lx_pci_read_config(d, o, 4, v)
#define pci_write_config_dword(d, o, v) lx_pci_write_config(d, o, 4, v)
static inline int pci_read_config_word(struct pci_dev *d, int o, u16 *v) { u32 t = 0; int r = lx_pci_read_config(d, o, 2, &t); *v = t; return r; }
#define pci_write_config_word(d, o, v) lx_pci_write_config(d, o, 2, v)
static inline int pcie_capability_read_word(struct pci_dev *d, int o, u16 *v) { u32 t = 0; int r = lx_pci_read_config(d, 0x80000000 | o, 2, &t); *v = t; return r; }
int pcie_capability_clear_and_set_word(struct pci_dev *d, int o, u16 clear, u16 set);
#define pcie_capability_clear_word(d, o, c) pcie_capability_clear_and_set_word(d, o, c, 0)
int pci_enable_device(struct pci_dev *d);
void pci_disable_device(struct pci_dev *d);
int pci_is_enabled(struct pci_dev *d);
void pci_set_master(struct pci_dev *d);
int pci_request_region(struct pci_dev *d, int bar, const char *n);
void pci_release_region(struct pci_dev *d, int bar);
void *pci_iomap(struct pci_dev *d, int bar, unsigned long max);
void pci_iounmap(struct pci_dev *d, void *p);
unsigned long lx_pci_resource_len(struct pci_dev *d, int bar);
#define pci_resource_len(d, b) lx_pci_resource_len(d, b)
int pci_assign_resource(struct pci_dev *d, int bar);
int pci_alloc_irq_vectors(struct pci_dev *d, unsigned int min, unsigned int max, unsigned int flags);
void pci_free_irq_vectors(struct pci_dev *d);
int pci_irq_vector(struct pci_dev *d, unsigned int nr);
int pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);
#define pci_domain_nr(b) 0
#define pci_alloc_irq_vectors_affinity(...) (-ENOSYS)
#define pcie_aspm_enabled(d) 0
#define pci_clear_master(d) ((void)0)
#define pci_save_state(d) 0
#define pci_restore_state(d) ((void)0)
#define pci_name(d) "0000:01:00.0"
#define pm_runtime_get_sync(d) 0
#define pm_runtime_put_sync(d) 0
/* platform devices are not used on this board; the type is only referred to */
struct platform_device { struct device dev; };
struct platform_driver { int (*probe)(struct platform_device *); void (*remove)(struct platform_device *); struct device_driver driver; };
struct of_device_id { char compatible[128]; const void *data; };
#define MODULE_DEVICE_TABLE_LX(...)
struct qcom_smem_state; struct clk; struct regulator; struct rproc;
#define IRQ_DISABLE_UNLAZY 1
#define irq_set_status_flags(i, f) ((void)0)
#define irq_clear_status_flags(i, f) ((void)0)
#define irq_set_affinity_and_hint(i, m) 0
struct msi_desc *irq_get_msi_desc(unsigned int irq);
#define from_work(var, w, member) container_of(w, __typeof__(*(var)), member)
#define module_param_named(...)
#define module_param(...)
#define module_param_array(...)
#define MODULE_PARM_DESC(...)
#define MODULE_VERSION(...)
#define DECLARE_EWMA(name, p, w) \
  struct ewma_##name { unsigned long internal; }; \
  static inline void ewma_##name##_init(struct ewma_##name *e) { e->internal = 0; } \
  static inline unsigned long ewma_##name##_read(struct ewma_##name *e) { return e->internal >> (p); } \
  static inline void ewma_##name##_add(struct ewma_##name *e, unsigned long v) { \
    unsigned long c = e->internal, shift = __builtin_ctzl((unsigned long)(w)); \
    e->internal = c ? (((c << shift) - c) + (v << (p))) >> shift : (v << (p)); }

#define sizeof_field(t, m) sizeof(((t *)0)->m)
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((unsigned long)(p), a))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define roundup_pow_of_two(x) ({ unsigned long _v = (x), _r = 1; while (_r < _v) _r <<= 1; _r; })
#define noinline_for_stack
#define noinline __attribute__((noinline))
#define MSEC_PER_SEC 1000L
#define USEC_PER_SEC 1000000L
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define struct_size(p, m, n) (sizeof(*(p)) + sizeof((p)->m[0]) * (n))
#define DEFINE_RAW_FLEX(type, name, member, count) \
  _Static_assert(__builtin_constant_p(count), "onstack flex array requires constant count"); \
  union { u8 bytes[sizeof(type) + sizeof(((type *)0)->member[0]) * (count)]; type obj; } name##_u = { }; \
  type *name = (type *)&name##_u
#define size_add(a, b) ((a) + (b))
#define size_mul(a, b) ((a) * (b))
#define memset_startat(p, v, m) memset((char *)(p) + __builtin_offsetof(__typeof__(*(p)), m), v, sizeof(*(p)) - __builtin_offsetof(__typeof__(*(p)), m))
#define list_first_entry_or_null(p, t, m) (list_empty(p) ? (t *)0 : list_first_entry(p, t, m))
#define list_for_each_safe(p, n, h) for (p = (h)->next, n = p->next; p != (h); p = n, n = p->next)
#define list_for_each_entry_continue(p, h, m) for (p = list_next_entry(p, m); &p->m != (h); p = list_next_entry(p, m))
#define list_last_entry(p, t, m) list_entry((p)->prev, t, m)
#define list_is_last(e, h) ((e)->next == (h))
#define list_replace(o, n) do { (n)->next = (o)->next; (n)->next->prev = (n); (n)->prev = (o)->prev; (n)->prev->next = (n); } while (0)
#define le64_get_bits(v, m) (((u64)(v) & (m)) >> __builtin_ctzll(m))
#define u32p_replace_bits(p, v, m) (*(p) = ((*(p)) & ~(m)) | ((((u32)(v)) << __builtin_ctz(m)) & (m)))
#define skb_get_queue_mapping(s) ((s)->queue_mapping)
#define skb_set_queue_mapping(s, q) ((s)->queue_mapping = (q))
#define SKB_DATA_ALIGN(x) ALIGN(x, 64)
#define scnprintf(b, n, ...) ({ int _r = snprintf(b, n, __VA_ARGS__); _r >= (int)(n) ? (int)(n) - 1 : _r; })
#define strscpy(d, s, n) ({ size_t _n = (n); strncpy(d, s, _n); if (_n) (d)[_n - 1] = 0; strlen(d); })
#define strscpy_pad(d, s, n) strscpy(d, s, n)
#define isascii(c) ((unsigned)(c) < 128)
#define firmware_request_nowarn(fw, n, d) lx_request_firmware(fw, n)
#define to_pci_dev(d) container_of(d, struct pci_dev, dev)
#define SET_SYSTEM_SLEEP_PM_OPS(a, b)
#define SET_RUNTIME_PM_OPS(a, b, c)
#define cpumask_of(c) ((const struct cpumask *)0)
struct cpumask { int unused; };
#define timer_container_of(v, t, m) container_of(t, __typeof__(*(v)), m)
#define ECOMM 70
struct va_format { const char *fmt; va_list *va; };
struct sysinfo { unsigned long totalram, freeram, mem_unit; };
#define si_meminfo(i) memset(i, 0, sizeof *(i))
#define dmi_walk(f, d) (-ENODEV)
#define dmi_get_system_info(x) ((const char *)0)
#define dmi_get_bios_year() 0
#define __free(f) __attribute__((cleanup(lx_cleanup_##f)))
static inline void lx_cleanup_kfree(void *p) { lx_free(*(void **)p); }
#define no_free_ptr(p) ({ __typeof__(p) _p = (p); (p) = 0; _p; })
#define return_ptr(p) return no_free_ptr(p)
/* scope-based guards: released when the variable leaves its block */
#define LX_CAT2(a, b) a##b
#define LX_CAT(a, b) LX_CAT2(a, b)
static inline void lx_g_mutex_exit(struct mutex **m) { mutex_unlock(*m); }
static inline struct mutex *lx_g_mutex_enter(struct mutex *m) { mutex_lock(m); return m; }
static inline void lx_g_spinlock_bh_exit(spinlock_t **l) { spin_unlock_bh(*l); }
static inline spinlock_t *lx_g_spinlock_bh_enter(spinlock_t *l) { spin_lock_bh(l); return l; }
static inline void lx_g_rcu_exit(int *x) { (void)x; lx_rcu_read_unlock(); }
static inline int lx_g_rcu_enter(void) { lx_rcu_read_lock(); return 0; }
#define guard(kind) for (int LX_CAT(lx_once_, __LINE__) = 0; 0;) {} LX_GUARD_##kind
#define scoped_guard(kind, ...) \
  for (bool LX_CAT(lx_scoped_, __LINE__) = false; !LX_CAT(lx_scoped_, __LINE__);) \
    for (LX_GUARD_##kind(__VA_ARGS__); !LX_CAT(lx_scoped_, __LINE__) && (LX_CAT(lx_scoped_, __LINE__) = true);)
#define LX_GUARD_mutex(m) struct mutex *LX_CAT(lx_g_, __LINE__) __attribute__((cleanup(lx_g_mutex_exit))) = lx_g_mutex_enter(m)
#define LX_GUARD_spinlock_bh(l) spinlock_t *LX_CAT(lx_g_, __LINE__) __attribute__((cleanup(lx_g_spinlock_bh_exit))) = lx_g_spinlock_bh_enter(l)
#define LX_GUARD_rcu() int LX_CAT(lx_g_, __LINE__) __attribute__((cleanup(lx_g_rcu_exit))) = lx_g_rcu_enter()
struct wiphy;
void lx_g_wiphy_exit(struct wiphy **w);
struct wiphy *lx_g_wiphy_enter(struct wiphy *w);
#define LX_GUARD_wiphy(w) struct wiphy *LX_CAT(lx_g_, __LINE__) __attribute__((cleanup(lx_g_wiphy_exit))) = lx_g_wiphy_enter(w)
#define lockdep_assert_wiphy(w)
#define rtnl_lock() lx_rtnl_lock()
#define rtnl_unlock() lx_rtnl_unlock()
#define ASSERT_RTNL()

#define SZ_512M 0x20000000
#define SZ_256M 0x10000000
#define SZ_16M 0x1000000
#define SZ_32M 0x2000000
#define SZ_1G 0x40000000
#define put_unaligned_le16(v, p) do { u16 _v = (v); memcpy((p), &_v, 2); } while (0)
#define put_unaligned_le32x(v, p) put_unaligned_le32(v, p)
#define list_move(e, h) do { list_del(e); list_add(e, h); } while (0)
#define kzalloc_flex(p, m, n, ...) lx_alloc(sizeof(p) + sizeof(((__typeof__(p) *)0)->m[0]) * (n), 1)
#define kmalloc_flex(p, m, n, ...) lx_alloc(sizeof(p) + sizeof(((__typeof__(p) *)0)->m[0]) * (n), 0)
#define iounmap(p) ((void)(p))
#define idr_for_each_entry(i, p, id) for (id = 0; id < (int)(i)->cap; id++) if (((p) = (i)->slot[id]) != 0)
#define div_u64(a, b) ((u64)(a) / (u32)(b))
#define div64_u64(a, b) ((u64)(a) / (u64)(b))
#define atomic_dec_if_positive(a) ({ int _v = atomic_read(a) - 1; if (_v >= 0) atomic_set(a, _v); _v; })
#define rcu_read_lock_any_held() 1
struct ethhdr { u8 h_dest[6]; u8 h_source[6]; __be16 h_proto; } __packed;
struct reserved_mem { phys_addr_t base, size; };
struct resource { phys_addr_t start, end; };
#define resource_size(r) ((r)->end - (r)->start + 1)
#define IPV6_ADDR_SCOPE_LINKLOCAL 2
#define IPV6_ADDR_SCOPE_GLOBAL 14
struct inet6_ifaddr { int unused; }; struct inet6_dev { int unused; }; struct ifacaddr6 { int unused; };
#define ipv6_addr_src_scope(a) 0
#define QCOM_SCM_LX 1
#define DEFINE_MUTEX(n) struct mutex n = { 0 }
#define isprint(c) ((unsigned)(c) - 32u < 95u)
#define isdigit_lx(c) ((c) >= '0' && (c) <= '9')
#define dma_mb() mb()
#define le64_encode_bits(v, m) ((__le64)((((u64)(v)) << __builtin_ctzll(m)) & (m)))
#define of_graph_get_endpoint_by_regs(...) ((struct device_node *)0)
#define of_graph_get_remote_endpoint(...) ((struct device_node *)0)
#define of_graph_get_port_parent(...) ((struct device_node *)0)
#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define CHECKSUM_PARTIAL 3
#define IFA_F_DADFAILED 0x08
void *lx_ioremap(phys_addr_t pa, size_t n);
#define ioremap(pa, n) lx_ioremap(pa, n)
#define devm_ioremap(dev, pa, n) lx_ioremap(pa, n)
#define ioremap_wc(pa, n) lx_ioremap(pa, n)
#define memremap(pa, n, f) lx_ioremap(pa, n)
#define memunmap(p) ((void)(p))
#define MEMREMAP_WC 0

#define SET_LATE_SYSTEM_SLEEP_PM_OPS(a, b)
#define memcpy_and_pad(d, dl, s, sl, p) do { size_t _l = (sl) < (dl) ? (sl) : (dl); memcpy(d, s, _l); if ((dl) > _l) memset((char *)(d) + _l, p, (dl) - _l); } while (0)
static inline int list_count_nodes(struct list_head *h) { int n = 0; struct list_head *p; for (p = h->next; p != h; p = p->next) n++; return n; }
#define get_unaligned_be64(p) ({ u64 _v; memcpy(&_v, (p), 8); __builtin_bswap64(_v); })
#define get_unaligned_le64(p) ({ u64 _v; memcpy(&_v, (p), 8); _v; })
#define list_for_each_entry_safe_reverse(p, n, h, m) for (p = list_entry((h)->prev, __typeof__(*p), m), n = list_entry(p->m.prev, __typeof__(*p), m); &p->m != (h); p = n, n = list_entry(n->m.prev, __typeof__(*n), m))
static inline unsigned char *skb_tail_pointer(const struct sk_buff *s) { return s->tail; }
#define WARN_ONCE(c, ...) WARN(c)
#define u8p_replace_bits(p, v, m) (*(p) = ((*(p)) & ~(m)) | ((((u8)(v)) << __builtin_ctz(m)) & (m)))
#define u16p_replace_bits(p, v, m) (*(p) = ((*(p)) & ~(m)) | ((((u16)(v)) << __builtin_ctz(m)) & (m)))
#define skb_queue_len_lockless(q) ((q)->qlen)
#define synchronize_net() synchronize_rcu()
#define __ffs64(x) __builtin_ctzll(x)
#define list_cut_before(a, b, e) lx_list_cut_before(a, b, e)
static inline void lx_list_cut_before(struct list_head *dst, struct list_head *src, struct list_head *entry) {
  if (src->next == entry) { INIT_LIST_HEAD(dst); return; }
  dst->next = src->next; dst->next->prev = dst; dst->prev = entry->prev; dst->prev->next = dst; src->next = entry; entry->prev = src; }
static inline void list_splice_tail(struct list_head *l, struct list_head *h) { if (list_empty(l)) return; l->next->prev = h->prev; h->prev->next = l->next; l->prev->next = h; h->prev = l->prev; }
static inline void list_splice(struct list_head *l, struct list_head *h) { if (list_empty(l)) return; l->prev->next = h->next; h->next->prev = l->prev; l->next->prev = h; h->next = l->next; }
static inline void list_splice_init(struct list_head *l, struct list_head *h) { list_splice(l, h); INIT_LIST_HEAD(l); }
#define rcu_dereference_rtnl(p) (p)
#define le32_to_cpup(p) (*(p))
#define le16_to_cpup(p) (*(p))
static inline u16 be16_to_cpup(const __be16 *p) { return get_unaligned_be16(p); }
struct crypto_shash { int unused; };
struct shash_desc { struct crypto_shash *tfm; u8 ctx[64]; };
struct crypto_shash *crypto_alloc_shash(const char *alg, u32 type, u32 mask);
void crypto_free_shash(struct crypto_shash *t);
int crypto_shash_setkey(struct crypto_shash *t, const u8 *key, unsigned int len);
int crypto_shash_init(struct shash_desc *d);
int crypto_shash_update(struct shash_desc *d, const u8 *data, unsigned int len);
int crypto_shash_final(struct shash_desc *d, u8 *out);
#define SHASH_DESC_ON_STACK(d, t) struct shash_desc d##_store; struct shash_desc *d = &d##_store; d->tfm = (t)
#define dev_coredumpv(d, p, n, f) lx_free(p)
#define in6_dev_get(d) ((struct inet6_dev *)0)
int skb_cow_data(struct sk_buff *s, int tail, struct sk_buff **trailer);
void *pskb_put(struct sk_buff *s, struct sk_buff *tail, int n);
struct sk_buff *skb_realloc_headroom(struct sk_buff *s, unsigned int h);
#define shash_desc_zero(d) memset(d, 0, sizeof *(d))
static inline void __skb_queue_before(struct sk_buff_head *q, struct sk_buff *next, struct sk_buff *s) { s->next = next; s->prev = next->prev; next->prev->next = s; next->prev = s; q->qlen++; }
#define skb_copy_from_linear_data(s, to, n) memcpy(to, (s)->data, n)
static inline struct sk_buff *skb_peek_tail(const struct sk_buff_head *q) { struct sk_buff *s = q->prev; return s == (const struct sk_buff *)q ? 0 : s; }
#define smp_processor_id() 0
#define PCI_VDEVICE(v, d) .vendor = PCI_VENDOR_ID_##v, .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define ffz(x) __builtin_ctzl(~(x))
#include <linux/bitmap.h>
#endif
