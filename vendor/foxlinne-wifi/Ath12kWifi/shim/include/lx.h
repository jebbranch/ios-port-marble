/* Kernel programming interface the Linux-derived WiFi sources are built against, implemented by this module.
 * Execution model: everything runs from one dispatcher (lx_pump). Work items, timers and interrupt bottom halves are queued and run
 * from it; a "wait" runs the dispatcher until its condition holds or its time is up, so nothing blocks a thread. */
#ifndef LX_H
#define LX_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <linux/kconfig.h>
#if __STDC_HOSTED__
#include <stdlib.h>
#endif

typedef unsigned long kernel_ulong_t;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef u16 __le16; typedef u32 __le32; typedef u64 __le64; typedef u16 __be16; typedef u32 __be32;
typedef u64 dma_addr_t; typedef u64 phys_addr_t; typedef long ssize_t_lx;
typedef long ssize_t;
typedef unsigned int gfp_t; typedef long long loff_t;
#define __bitwise
#define __iomem
#define __user
#define __force
#define __init
#define __exit
#define __net_exit
#define __section(name) /* ELF subsection placement is not used in Mach-O. */
#define __must_check
#define __be64_unused
typedef u64 __be64;
#define __maybe_unused __attribute__((unused))
#define at_least static
static inline void memzero_explicit(void *memory, size_t length) {
  volatile u8 *bytes = memory;
  while (length--) *bytes++ = 0;
}
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __always_inline inline __attribute__((always_inline))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define BITS_PER_LONG 64
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define offsetof_lx(t, m) __builtin_offsetof(t, m)
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define SZ_4K 0x1000
#define SZ_512K 0x80000
#define SZ_1M 0x100000
#define SZ_2M 0x200000
#define HZ 1000
#define NSEC_PER_MSEC 1000000L
#define USEC_PER_MSEC 1000L
#define ERR_PTR(e) ((void *)(long)(e))
#define ERR_CAST(p) ((void *)(p))
#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define ENOTSUPP 524
#define EPROBE_DEFER 517
#ifndef S32_MIN
#define S32_MIN (-2147483647 - 1)
#endif
#ifndef ERFKILL
#define ERFKILL 132
#endif
#define ERESTARTSYS 512
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_DEVICE_TABLE(a, b)
#define MODULE_FIRMWARE(x)
#define MODULE_ALIAS(x)
#define WARN_ON(c) ({ int _w = !!(c); if (_w) lx_warn(__FILE__, __LINE__); _w; })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, ...) ({ int _w = !!(c); if (_w) lx_warn(__FILE__, __LINE__); _w; })
#define BUG_ON(c) do { if (c) lx_bug(__FILE__, __LINE__); } while (0)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "build bug")
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define smp_wmb() wmb()
#define smp_mb() mb()
#define smp_rmb() rmb()
/* The endpoint and the device mappings are outside the inner-shareable domain the C11 fences order: full system barriers. */
#if defined(__aarch64__)
#define mb() __asm__ volatile("dsb sy" ::: "memory")
#define wmb() __asm__ volatile("dsb sy" ::: "memory")
#define rmb() __asm__ volatile("dsb ld" ::: "memory")
#else
#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#define barrier() __asm__ volatile("" ::: "memory")
#define fallthrough __attribute__((fallthrough))
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, 64)

void lx_warn(const char *file, int line);
void lx_bug(const char *file, int line) __attribute__((noreturn));
void lx_log(int level, const char *fmt, ...) __printf(2, 3);

/* byte order: the target is little endian */
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#define lower_32_bits(n) ((u32)((u64)(n) & 0xffffffffu))
#define FIELD_PREP(mask, val) ((((u64)(val)) << __builtin_ctzll(mask)) & (mask))
#define FIELD_GET(mask, reg) ((((u64)(reg)) & (mask)) >> __builtin_ctzll(mask))
#define FIELD_MAX(mask) ((mask) >> __builtin_ctzll(mask))
#define FIELD_FIT(mask, value) (((u64)(value) & ~(u64)FIELD_MAX(mask)) == 0)
#define typecheck(type, x) ({ type _a; __typeof__(x) _b; (void)(&_a == &_b); 1; })
static inline u32 reciprocal_scale(u32 value, u32 range) { return (u32)(((u64)value * range) >> 32); }
static inline int fls64(u64 value) { return value ? 64 - __builtin_clzll(value) : 0; }
#define abs(x) ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })

/* lists */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define LIST_HEAD(n) struct list_head n = LIST_HEAD_INIT(n)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x) { x->prev = n; n->next = x; n->prev = p; p->next = n; }
static inline void list_add(struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void list_del(struct list_head *e) { e->prev->next = e->next; e->next->prev = e->prev; e->next = e->prev = 0; }
static inline void list_del_init(struct list_head *e) { e->prev->next = e->next; e->next->prev = e->prev; INIT_LIST_HEAD(e); }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
static inline void list_move_tail(struct list_head *e, struct list_head *h) { list_del(e); list_add_tail(e, h); }
static inline void list_splice_tail_init(struct list_head *l, struct list_head *h) {
  if (list_empty(l)) return;
  l->next->prev = h->prev; h->prev->next = l->next; l->prev->next = h; h->prev = l->prev; INIT_LIST_HEAD(l); }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_first_entry(p, t, m) list_entry((p)->next, t, m)
/* A list sentinel need not satisfy its containing type's stronger alignment.
 * Access the actual list_head, including at end-of-list, without dereferencing
 * a fictitious containing object. Preserve Linux's final iterator value. */
#define lx_list_member(p, m) ((struct list_head *)((char *)(p) + offsetof(__typeof__(*(p)), m)))
#define list_next_entry(p, m) list_entry(lx_list_member(p, m)->next, __typeof__(*(p)), m)
#define list_prev_entry(p, m) list_entry(lx_list_member(p, m)->prev, __typeof__(*(p)), m)
#define list_prepare_entry(p, h, m) ((p) ? (p) : list_entry(h, __typeof__(*(p)), m))
#define list_for_each_entry_continue(p, h, m) for (p = list_next_entry(p, m); lx_list_member(p, m) != (h); p = list_next_entry(p, m))
#define list_for_each_entry_continue_reverse(p, h, m) for (p = list_prev_entry(p, m); lx_list_member(p, m) != (h); p = list_prev_entry(p, m))
#define list_for_each(p, h) for (p = (h)->next; p != (h); p = p->next)
#define list_for_each_entry(p, h, m) for (p = list_entry((h)->next, __typeof__(*p), m); lx_list_member(p, m) != (h); p = list_next_entry(p, m))
#define list_for_each_entry_safe(p, n, h, m) for (p = list_entry((h)->next, __typeof__(*p), m), n = list_next_entry(p, m); lx_list_member(p, m) != (h); p = n, n = list_next_entry(n, m))

/* Atomic locks protect shared data. Hardware IRQ entry only records MSI bits;
 * the wireless dispatcher invokes handlers after these critical sections. */
typedef struct { int held; } spinlock_t;
typedef struct { int state; unsigned writers; } rwlock_t;
struct mutex { int held; };
static inline void lx_cpu_relax(void) {
#if defined(__aarch64__)
  __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__)
  __asm__ volatile("pause" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}
static inline void spin_lock_init(spinlock_t *lock) { __atomic_store_n(&lock->held, 0, __ATOMIC_RELAXED); }
void lx_spin_enter(void);
void lx_spin_leave(void);
static inline bool spin_trylock(spinlock_t *lock) {
  int expected = 0;
  return __atomic_compare_exchange_n(&lock->held, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static inline void spin_lock(spinlock_t *lock) {
  while (!spin_trylock(lock)) while (__atomic_load_n(&lock->held, __ATOMIC_RELAXED)) lx_cpu_relax();
}
static inline void spin_unlock(spinlock_t *lock) { __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE); }
/* Only the native control/status queues are shared with XNU threads. Internal
 * Linux locks belong to the wireless executor; their callbacks may allocate
 * through the platform and must not acquire native no-preemption semantics. */
static inline void lx_shared_spin_lock(spinlock_t *lock) { lx_spin_enter(); spin_lock(lock); }
static inline void lx_shared_spin_unlock(spinlock_t *lock) { spin_unlock(lock); lx_spin_leave(); }
static inline bool lx_shared_spin_trylock(spinlock_t *lock) {
  lx_spin_enter(); bool acquired = spin_trylock(lock); if (!acquired) lx_spin_leave(); return acquired;
}
#define spin_is_locked(l) (!!__atomic_load_n(&(l)->held, __ATOMIC_RELAXED))
#define spin_lock_bh(l) spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)
#define spin_lock_irqsave(l, f) do { (f) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); spin_unlock(l); } while (0)
#define DEFINE_SPINLOCK(n) spinlock_t n = { 0 }
static inline void rwlock_init(rwlock_t *lock) { lock->state = 0; lock->writers = 0; }
static inline void read_lock(rwlock_t *lock) {
  for (;;) {
    int readers = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    if (readers >= 0 && !__atomic_load_n(&lock->writers, __ATOMIC_RELAXED) &&
        __atomic_compare_exchange_n(&lock->state, &readers, readers + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    lx_cpu_relax();
  }
}
static inline void read_unlock(rwlock_t *lock) { __atomic_fetch_sub(&lock->state, 1, __ATOMIC_RELEASE); }
static inline void write_lock(rwlock_t *lock) {
  __atomic_fetch_add(&lock->writers, 1u, __ATOMIC_RELAXED);
  int expected;
  do { expected = 0; if (__atomic_compare_exchange_n(&lock->state, &expected, -1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break; lx_cpu_relax(); } while (true);
  __atomic_fetch_sub(&lock->writers, 1u, __ATOMIC_RELAXED);
}
static inline void write_unlock(rwlock_t *lock) { __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE); }
#define read_lock_bh(l) read_lock(l)
#define read_unlock_bh(l) read_unlock(l)
#define read_lock_irq(l) read_lock(l)
#define read_unlock_irq(l) read_unlock(l)
#define read_lock_irqsave(l, f) do { (f) = 0; read_lock(l); } while (0)
#define read_unlock_irqrestore(l, f) do { (void)(f); read_unlock(l); } while (0)
#define write_lock_irq(l) write_lock(l)
#define write_unlock_irq(l) write_unlock(l)
#define write_lock_irqsave(l, f) do { (f) = 0; write_lock(l); } while (0)
#define write_unlock_irqrestore(l, f) do { (void)(f); write_unlock(l); } while (0)
#define write_lock_bh(l) write_lock(l)
#define write_unlock_bh(l) write_unlock(l)
#define mutex_init(m) ((m)->held = 0)
#define mutex_destroy(m) WARN_ON((m)->held)
void lx_mutex_lock(struct mutex *mutex);
void lx_mutex_unlock(struct mutex *mutex);
bool lx_mutex_trylock(struct mutex *mutex);
void lx_rtnl_lock(void);
void lx_rtnl_unlock(void);
bool lx_rtnl_is_locked(void);
#define mutex_lock(m) lx_mutex_lock(m)
#define mutex_unlock(m) lx_mutex_unlock(m)
#define mutex_trylock(m) lx_mutex_trylock(m)
#define mutex_is_locked(m) (!!__atomic_load_n(&(m)->held, __ATOMIC_RELAXED))
#define local_bh_disable()
#define local_bh_enable()
#define might_sleep()
#define lockdep_assert_held(l)
#define lockdep_assert_held_write(l)

/* atomics */
typedef struct { int v; } atomic_t;
#define ATOMIC_INIT(i) { (i) }
#define atomic_read(a) __atomic_load_n(&(a)->v, __ATOMIC_SEQ_CST)
#define atomic_set(a, i) __atomic_store_n(&(a)->v, (i), __ATOMIC_SEQ_CST)
#define atomic_inc(a) __atomic_add_fetch(&(a)->v, 1, __ATOMIC_SEQ_CST)
#define atomic_dec(a) __atomic_sub_fetch(&(a)->v, 1, __ATOMIC_SEQ_CST)
#define atomic_inc_return(a) __atomic_add_fetch(&(a)->v, 1, __ATOMIC_SEQ_CST)
#define atomic_dec_return(a) __atomic_sub_fetch(&(a)->v, 1, __ATOMIC_SEQ_CST)
#define atomic_add(i, a) __atomic_add_fetch(&(a)->v, (i), __ATOMIC_SEQ_CST)
#define atomic_sub(i, a) __atomic_sub_fetch(&(a)->v, (i), __ATOMIC_SEQ_CST)
#define atomic_sub_return(i, a) atomic_sub(i, a)
#define atomic_cmpxchg(a, old, value) ({ int _expected = (old); \
  __atomic_compare_exchange_n(&(a)->v, &_expected, (value), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); _expected; })
#define atomic_dec_and_test(a) (atomic_dec_return(a) == 0)
static inline bool atomic_add_unless(atomic_t *a, int value, int unless) {
  int old = atomic_read(a);
  while (old != unless) {
    int next = (int)((unsigned)old + (unsigned)value);
    if (__atomic_compare_exchange_n(&a->v, &old, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
  }
  return false;
}
#define atomic_inc_not_zero(a) atomic_add_unless(a, 1, 0)
typedef struct { long v; } atomic_long_t;
static inline void set_bit(int n, volatile unsigned long *p) { __atomic_or_fetch(p + n / 64, 1UL << (n % 64), __ATOMIC_SEQ_CST); }
static inline void clear_bit(int n, volatile unsigned long *p) { __atomic_and_fetch(p + n / 64, ~(1UL << (n % 64)), __ATOMIC_SEQ_CST); }
static inline int test_bit(int n, const volatile unsigned long *p) {
  return (__atomic_load_n(p + n / 64, __ATOMIC_RELAXED) >> (n % 64)) & 1;
}
static inline int test_and_set_bit(int n, volatile unsigned long *p) {
  unsigned long bit = 1UL << (n % 64);
  return !!(__atomic_fetch_or(p + n / 64, bit, __ATOMIC_SEQ_CST) & bit);
}
static inline int test_and_clear_bit(int n, volatile unsigned long *p) {
  unsigned long bit = 1UL << (n % 64);
  return !!(__atomic_fetch_and(p + n / 64, ~bit, __ATOMIC_SEQ_CST) & bit);
}
#define DECLARE_BITMAP(n, b) unsigned long n[BITS_TO_LONGS(b)]

/* time */
typedef s64 ktime_t;
typedef s64 time64_t;
u64 lx_now_ms(void);
u64 lx_now_ns(void);
u64 lx_boottime_ns(void);
extern volatile unsigned long jiffies;
#define msecs_to_jiffies(m) ((unsigned long)(m))
#define secs_to_jiffies(s) ((unsigned long)(s) * HZ)
#define jiffies_to_msecs(j) ((unsigned int)(j))
static inline unsigned int jiffies_delta_to_msecs(long delta) { return delta > 0 ? (unsigned int)delta : 0; }
#define usecs_to_jiffies(us) ((unsigned long)(us) / 1000 + ((unsigned long)(us) % 1000 != 0))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_before_eq(a, b) ((long)((a) - (b)) <= 0)
#define time_after_eq(a, b) time_before_eq(b, a)
#define time_is_after_jiffies(a) time_after(a, jiffies)
#define time_is_before_jiffies(a) time_before(a, jiffies)
#define ktime_get() ((ktime_t)lx_now_ns())
#define ktime_get_boottime_ns() lx_boottime_ns()
#define ktime_get_seconds() (lx_now_ns() / 1000000000ULL)
#define ktime_to_ms(k) ((k) / 1000000LL)
#define ktime_to_ns(k) (k)
#define us_to_ktime(us) ((ktime_t)(us) * 1000)
#define ms_to_ktime(ms) ((ktime_t)(ms) * 1000000)
static inline unsigned long round_jiffies_up(unsigned long deadline) {
  unsigned long rounded = deadline / 1000 * 1000;
  return rounded == deadline ? deadline : rounded + 1000;
}
static inline unsigned long round_jiffies(unsigned long deadline) {
  unsigned long rounded = (deadline + 750) / 1000 * 1000;
  return time_after(rounded, jiffies) ? rounded : deadline;
}
static inline unsigned long round_jiffies_relative(unsigned long delay) {
  unsigned long now = jiffies; return round_jiffies(now + delay) - now;
}
#define ktime_sub(a, b) ((a) - (b))
#define ktime_set(sec, nsec) ((ktime_t)(sec) * 1000000000LL + (nsec))
void lx_delay_ms(unsigned int ms);   /* runs the dispatcher for that long */
void lx_delay_us(unsigned int us);
void lx_busy_ms(unsigned int ms);
void lx_sleep_us(unsigned int us);
#define mdelay(m) lx_busy_ms(m)
#define udelay(u) lx_delay_us(u)
#define msleep(m) lx_delay_ms(m)
#define usleep_range(a, b) lx_sleep_us(a)
#define cpu_relax() lx_cpu_relax()

/* memory */
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define GFP_NOWAIT 1
#define __GFP_ZERO 2
#define __GFP_NOWARN 4
#define GFP_DMA32 8
#define GFP_USER 0
void *lx_alloc(size_t n, int zero);
void lx_free(const void *p);
#define kmalloc(n, f) lx_alloc(n, 0)
#define kzalloc(n, f) lx_alloc(n, 1)
#define kcalloc(n, s, f) lx_alloc((n) * (s), 1)
#define kmalloc_array(n, s, f) lx_alloc((n) * (s), 0)
#define kzalloc_obj(p, ...) lx_alloc(sizeof(p), 1)
#define kzalloc_objs(p, n, ...) lx_alloc(sizeof(p) * (n), 1)
#define kvzalloc_objs(p, n, ...) lx_alloc(sizeof(p) * (n), 1)
#define kmalloc_obj(p, ...) lx_alloc(sizeof(p), 0)
#define kmalloc_objs(p, n, ...) lx_alloc(sizeof(p) * (n), 0)
#define kvzalloc(n, f) lx_alloc(n, 1)
#define vzalloc(n) lx_alloc(n, 1)
#define vmalloc(n) lx_alloc(n, 0)
#define kvmalloc(n, f) lx_alloc(n, 0)
#define kfree(p) lx_free(p)
#define kvfree(p) lx_free(p)
#define vfree(p) lx_free(p)
#define kfree_sensitive(p) lx_free(p)
void *lx_memdup(const void *p, size_t n);
#define kmemdup(p, n, f) lx_memdup(p, n)
static inline void *lx_memdup_array(const void *p, size_t count, size_t size) {
  if (size && count > (size_t)-1 / size) return NULL;
  return lx_memdup(p, count * size);
}
#define kmemdup_array(p, count, size, flags) lx_memdup_array(p, count, size)
char *lx_strdup(const char *s);
#define kstrdup(s, f) lx_strdup(s)
#define ZERO_OR_NULL_PTR(p) ((unsigned long)(p) <= 16)
#define __ro_after_init
#define __initdata
#define __initconst
#define __rcu
struct rcu_head { struct rcu_head *next; void (*func)(struct rcu_head *); void *allocation; };
void lx_rcu_read_lock(void);
void lx_rcu_read_unlock(void);
unsigned lx_rcu_readers(void);
void lx_rcu_call(struct rcu_head *head, void (*callback)(struct rcu_head *));
void lx_rcu_free(void *allocation, struct rcu_head *head);
void lx_synchronize_rcu(void);
void lx_rcu_barrier(void);
#define rcu_read_lock() lx_rcu_read_lock()
#define rcu_read_unlock() lx_rcu_read_unlock()
#define rcu_dereference(p) __atomic_load_n(&(p), __ATOMIC_SEQ_CST)
#define rcu_assign_pointer(p, v) __atomic_store_n(&(p), (v), __ATOMIC_SEQ_CST)
#define synchronize_rcu() lx_synchronize_rcu()
#define rcu_barrier() lx_rcu_barrier()
#define ffs(x) __builtin_ffs(x)
#define fls(x) ((x) ? 32 - __builtin_clz(x) : 0)
#define ilog2(x) (63 - __builtin_clzll(x))
#define is_power_of_2(x) ((x) && !((x) & ((x) - 1)))
#define swap(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define get_unaligned_le32(p) ({ u32 _v; memcpy(&_v, (p), 4); _v; })
#define get_unaligned(p) ({ __typeof__(*(p)) _value; memcpy(&_value, (p), sizeof _value); _value; })
#define put_unaligned(value, pointer) do { __typeof__(*(pointer)) _unaligned_value = (value); memcpy((pointer), &_unaligned_value, sizeof(_unaligned_value)); } while (0)
static inline u32 rol32(u32 v, unsigned n) { n &= 31; return (v << n) | (v >> ((32 - n) & 31)); }
static inline u32 ror32(u32 v, unsigned n) { n &= 31; return (v >> n) | (v << ((32 - n) & 31)); }
static inline u16 ror16(u16 v, unsigned n) { n &= 15; return (u16)((v >> n) | (v << ((16 - n) & 15))); }
#define get_unaligned_le16(p) ({ u16 _v; memcpy(&_v, (p), 2); _v; })
#define put_unaligned_le32(v, p) do { u32 _v = (v); memcpy((p), &_v, 4); } while (0)
#define put_unaligned_le64(v, p) do { u64 _v = (v); memcpy((p), &_v, 8); } while (0)
static inline void put_unaligned_be16(u16 value, void *memory) {
  u8 *p = memory; p[0] = (u8)(value >> 8); p[1] = (u8)value;
}
#define PAGE_SIZE 4096UL

/* io: BAR registers are volatile 32-bit words */
/* like the real accessors: a write is ordered after the memory writes before it, a read before the reads after it */
static inline u32 readl(const volatile void *a) { u32 v = *(const volatile u32 *)a; rmb(); return v; }
extern void (*lx_mmio_trace)(volatile void *a, u32 v);
static inline void writel(u32 v, volatile void *a) { wmb(); if (lx_mmio_trace) lx_mmio_trace(a, v); *(volatile u32 *)a = v; }
#define readl_relaxed readl
#define writel_relaxed writel
#define iowrite32 writel
#define ioread32 readl

/* random */
u32 lx_random_u32(void);
#define get_random_u32() lx_random_u32()
#define get_random_u16() ((u16)lx_random_u32())
#define get_random_u32_inclusive(lo, hi) ((lo) + lx_random_u32() % ((hi) - (lo) + 1))
#define prandom_u32_max(m) (lx_random_u32() % (m))
#ifndef ENOTUNIQ
#define ENOTUNIQ 76
#endif

#endif
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define for_each_set_bit(b, a, n) for ((b) = 0; (b) < (int)(n); (b)++) if (test_bit(b, a))
