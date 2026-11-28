/* Devices, waiting, deferred work, interrupts, DMA, firmware, packets. See lx.h for the execution model. */
#ifndef LX_OS_H
#define LX_OS_H
#include "lx.h"
#include <stdio.h>
typedef unsigned long ulong;

struct device; struct firmware; struct sk_buff; struct module; struct kobj_uevent_env; struct attribute_group;
void kernel_param_lock(struct module *module);
void kernel_param_unlock(struct module *module);
int lx_dispatching(void);
#define softirq_count() lx_dispatching()
struct kobject { int unused; };
struct device_node;
struct attribute { const char *name; };
struct device_attribute { struct attribute attr; };
struct bus_type;
struct device_driver { const char *name; const struct bus_type *bus; struct module *owner; int (*probe)(struct device *); const struct dev_pm_ops *pm; };
struct dev_pm_ops;
struct device_type { const char *name; void (*release)(struct device *); };
struct class { const char *name; void (*dev_release)(struct device *); };
int class_register(const struct class *class);
void class_unregister(const struct class *class);
struct device {
  const char *name; char namebuf[64]; struct device *parent; void *drvdata; u64 dma_mask; void *platform_data;
  struct device_driver *driver; const struct bus_type *bus; const struct device_type *type; void (*release)(struct device *);
  struct kobject kobj; struct list_head children, sibling; int registered; unsigned references; struct device_node *of_node; bool uevent_suppress, async_suspend;
  const struct class *class;
};
struct bus_type {
  const char *name; const char *dev_name; int (*match)(struct device *, const struct device_driver *); int (*probe)(struct device *);
  void (*remove)(struct device *); int (*uevent)(const struct device *, struct kobj_uevent_env *);
  const struct attribute_group **dev_groups;
};
#define THIS_MODULE ((struct module *)0)
#define DEVICE_ATTR_RO(n) static struct device_attribute dev_attr_##n = { { #n } }
#define DEVICE_ATTR_WO(n) static struct device_attribute dev_attr_##n = { { #n } }
#define ATTRIBUTE_GROUPS(n) static const struct attribute_group *n##_groups[] = { 0 }
#define sysfs_emit(buf, ...) snprintf(buf, 4096, __VA_ARGS__)
#define sysfs_emit_at(buf, at, ...) snprintf((buf) + (at), 4096 - (at), __VA_ARGS__)
#define sysfs_create_file(k, a) 0
#define sysfs_remove_file(k, a) ((void)0)
#define sysfs_create_link(...) 0
#define sysfs_remove_link(...) ((void)0)
#define KOBJ_CHANGE 0
#define kobject_uevent_env(k, a, e) 0
#define add_uevent_var(env, ...) 0
#define kasprintf(f, ...) lx_strdup("")
#define U32_MAX 0xffffffffu
#define dma_wmb() mb()
#define dma_rmb() mb()
#define fsleep(us) lx_sleep_us(us)
#define __ffs(x) __builtin_ctzl(x)
#define __fls(x) (63 - __builtin_clzl(x))
#define container_of_const(p, t, m) container_of(p, t, m)
#define vcalloc(n, s) lx_alloc((n) * (s), 1)
unsigned long find_last_bit(const unsigned long *a, unsigned long n);
unsigned long find_first_bit(const unsigned long *a, unsigned long n);
int device_register(struct device *d);
int device_add(struct device *d);
void device_del(struct device *d);
/* device memory takes only aligned accesses of a single width: never the string functions on a mapped BAR */
static inline void memset_io(volatile void *a, int v, unsigned long n) {
  volatile unsigned char *b = (volatile unsigned char *)a; unsigned int w = (unsigned char)v * 0x01010101u;
  while (n && ((unsigned long)b & 3)) { *b++ = (unsigned char)v; n--; }
  while (n >= 4) { *(volatile unsigned int *)b = w; b += 4; n -= 4; }
  while (n) { *b++ = (unsigned char)v; n--; } }
static inline void memcpy_fromio(void *d, const volatile void *s, unsigned long n) {
  unsigned char *o = d; const volatile unsigned char *i = (const volatile unsigned char *)s;
  while (n && (((unsigned long)i | (unsigned long)o) & 3)) { *o++ = *i++; n--; }
  while (n >= 4) { *(unsigned int *)o = *(const volatile unsigned int *)i; o += 4; i += 4; n -= 4; }
  while (n) { *o++ = *i++; n--; } }
static inline void memcpy_toio(volatile void *d, const void *s, unsigned long n) {
  volatile unsigned char *o = (volatile unsigned char *)d; const unsigned char *i = s;
  while (n && (((unsigned long)i | (unsigned long)o) & 3)) { *o++ = *i++; n--; }
  while (n >= 4) { *(volatile unsigned int *)o = *(const unsigned int *)i; o += 4; i += 4; n -= 4; }
  while (n) { *o++ = *i++; n--; } }
#define alloc_ordered_workqueue(name, flags, ...) lx_alloc_workqueue(name, flags, 1, ##__VA_ARGS__)
#define postcore_initcall(f)
#define subsys_initcall(f) int lx_init_##f(void) { return f(); }
#define late_initcall(f) int lx_init_##f(void) { return f(); }
#define fs_initcall(f) int lx_init_##f(void) { return f(); }
#define module_init(f)
#define module_exit(f) void lx_exit_##f(void) { f(); }
#define kstrtoul(s, b, r) lx_kstrtoul(s, b, r)
int lx_kstrtoul(const char *s, int base, unsigned long *r);

#define tasklet_init(t, f, d) lx_tasklet_init(t, NULL, f, d)
#define from_tasklet(var, callback_tasklet, field) container_of(callback_tasklet, __typeof__(*(var)), field)
void device_unregister(struct device *d);
int device_for_each_child(struct device *d, void *data, int (*fn)(struct device *, void *));
int driver_register(struct device_driver *d);
void driver_unregister(struct device_driver *d);
int bus_register(const struct bus_type *b);
void bus_unregister(const struct bus_type *b);
#define dev_name(d) ((d)->name ? (d)->name : "dev")
#define dev_get_drvdata(d) ((d)->drvdata)
#define dev_set_drvdata(d, p) ((d)->drvdata = (p))
#define dev_err(d, ...) lx_log(3, __VA_ARGS__)
#define netdev_err(d, ...) lx_log(3, __VA_ARGS__)
#define dev_warn(d, ...) lx_log(4, __VA_ARGS__)
#define dev_info(d, ...) lx_log(6, __VA_ARGS__)
#define KERN_DEBUG "7"
#define KERN_ERR "3"
#define KERN_WARNING "4"
#define KERN_INFO "6"
#define dev_printk(level, dev, ...) lx_log((level)[0] - '0', __VA_ARGS__)
#define dev_dbg(d, ...) lx_log(7, __VA_ARGS__)
#define dev_err_ratelimited(d, ...) lx_log(3, __VA_ARGS__)
#define dev_warn_ratelimited(d, ...) lx_log(4, __VA_ARGS__)
#define pr_err(...) lx_log(3, __VA_ARGS__)
#define pr_warn(...) lx_log(4, __VA_ARGS__)
#define pr_warn_ratelimited(...) do { if (net_ratelimit()) lx_log(4, __VA_ARGS__); } while (0)
#define pr_info(...) lx_log(6, __VA_ARGS__)
#define net_info_ratelimited(...) do { if (net_ratelimit()) lx_log(6, __VA_ARGS__); } while (0)
#define net_dbg_ratelimited(...) do { if (net_ratelimit()) lx_log(7, __VA_ARGS__); } while (0)
#define pr_debug(...) lx_log(7, __VA_ARGS__)
#define pr_fmt(x) x
#define dev_WARN(d, ...) lx_log(4, __VA_ARGS__)
struct device *get_device(struct device *d);
void put_device(struct device *d);
void device_initialize(struct device *d);
int dev_set_name(struct device *d, const char *format, ...) __printf(2, 3);
int device_rename(struct device *dev, const char *name);
static inline void device_enable_async_suspend(struct device *dev) { dev->async_suspend = true; }
static inline void dev_set_uevent_suppress(struct device *dev, bool suppress) { dev->uevent_suppress = suppress; }
static inline void device_init_wakeup(struct device *d, int e) { (void)d; (void)e; }
static inline int device_may_wakeup(struct device *d) { (void)d; return 0; }
static inline void pm_wakeup_event(struct device *d, int ms) { (void)d; (void)ms; }
static inline void pm_runtime_get_noresume(struct device *d) { (void)d; }
static inline void pm_runtime_put_noidle(struct device *d) { (void)d; }

/* waiting: wait_event runs the dispatcher (lx_pump) instead of sleeping */
typedef struct { int unused; } wait_queue_head_t;
#define init_waitqueue_head(w) ((void)(w))
#define wake_up(w) ((void)(w))
#define wake_up_all(w) ((void)(w))
#define wake_up_interruptible(w) ((void)(w))
extern void (*lx_log_tap)(int level, const char *line);   /* sees every line after it is logged */
int lx_pump(void);               /* runs one round of ready work; returns nonzero if anything ran */
void lx_irq_polling(int enabled);
unsigned lx_next_wait_ms(unsigned maximum);
int net_ratelimit(void);
void lx_idle(unsigned int ms);   /* nothing ready: let the clock advance and device events arrive */
#define wait_event_timeout(w, cond, tmo_j) ({ \
    unsigned long _end = (unsigned long)lx_now_ms() + (tmo_j); long _r; (void)(w); \
    for (;;) { \
      unsigned long _now = (unsigned long)lx_now_ms(); \
      if (cond) { _r = time_before(_now, _end) ? (long)(_end - _now) : 1; break; } \
      if (time_after_eq(_now, _end)) { _r = 0; break; } \
      if (!lx_pump()) lx_idle(1); \
    } _r; })
/* The condition may have side effects (a command is sent when it is evaluated): it is evaluated once per round, and never again after the wait ends. */
#define wait_event_timeout_once(w, cond, tmo_j) wait_event_timeout(w, cond, tmo_j)
#define wait_event(w, cond) ({ (void)(w); while (!(cond)) { if (!lx_pump()) lx_idle(1); } })
#define wait_event_interruptible(w, cond) ({ wait_event(w, cond); 0; })
#define wait_event_interruptible_timeout(w, cond, t) wait_event_timeout(w, cond, t)
struct completion { unsigned done; };
static inline void init_completion(struct completion *c) { __atomic_store_n(&c->done, 0, __ATOMIC_RELEASE); }
#define reinit_completion(c) init_completion(c)
static inline void complete(struct completion *c) {
  unsigned old = __atomic_load_n(&c->done, __ATOMIC_RELAXED);
  while (old != UINT_MAX && !__atomic_compare_exchange_n(&c->done, &old, old + 1, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
}
static inline void complete_all(struct completion *c) { __atomic_store_n(&c->done, UINT_MAX, __ATOMIC_RELEASE); }
static inline bool try_wait_for_completion(struct completion *c) {
  unsigned old = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
  while (old) {
    if (old == UINT_MAX) return true;
    if (__atomic_compare_exchange_n(&c->done, &old, old - 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
  }
  return false;
}
#define DECLARE_COMPLETION(n) struct completion n = { 0 }
#define wait_for_completion(c) wait_event(0, try_wait_for_completion(c))
#define wait_for_completion_timeout(c, t) wait_event_timeout(0, try_wait_for_completion(c), t)
#define completion_done(c) (__atomic_load_n(&(c)->done, __ATOMIC_ACQUIRE) != 0)

/* deferred work */
struct workqueue_struct;
struct work_struct {
  struct list_head entry; void (*func)(struct work_struct *); int queued;
  struct workqueue_struct *queue; u64 ticket;
};
struct delayed_work { struct work_struct work; u64 due; int armed; };
struct workqueue_struct { unsigned max_active, flags; bool dynamic; char name[64]; };
extern struct workqueue_struct *system_wq, *system_dfl_wq, *system_unbound_wq;
extern struct workqueue_struct *system_freezable_wq, *system_power_efficient_wq;
extern struct workqueue_struct *system_bh_wq, *system_long_wq;
struct workqueue_struct *lx_alloc_workqueue(const char *name, unsigned flags, unsigned max_active, ...);
void lx_flush_workqueue(struct workqueue_struct *queue);
void lx_destroy_workqueue(struct workqueue_struct *queue);
bool lx_flush_work(struct work_struct *work);
bool lx_cancel_work_sync(struct work_struct *work);
bool lx_cancel_delayed_sync(struct delayed_work *work);
bool lx_flush_delayed(struct delayed_work *work);
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
struct hrtimer {
  struct list_head entry; enum hrtimer_restart (*function)(struct hrtimer *);
  u64 expires; int clockid; bool armed;
};
#define HRTIMER_MODE_ABS 0
#define HRTIMER_MODE_REL 1
void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*callback)(struct hrtimer *), int clockid, int mode);
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t time, u64 slack, int mode);
int hrtimer_cancel(struct hrtimer *timer);
u64 hrtimer_forward_now(struct hrtimer *timer, ktime_t interval);
static inline bool hrtimer_is_queued(const struct hrtimer *timer) { return READ_ONCE(timer->armed); }
static inline void hrtimer_start(struct hrtimer *timer, ktime_t time, int mode) { hrtimer_start_range_ns(timer, time, 0, mode); }
#define DECLARE_WORK(name, callback) struct work_struct name = { .entry = LIST_HEAD_INIT(name.entry), .func = (callback) }
#define DECLARE_DELAYED_WORK(name, callback) struct delayed_work name = { .work = { .entry = LIST_HEAD_INIT(name.work.entry), .func = (callback) } }
#define work_pending(w) READ_ONCE((w)->queued)
#define delayed_work_pending(d) (READ_ONCE((d)->armed) || work_pending(&(d)->work))
#define INIT_WORK(w, f) do { (w)->func = (f); (w)->queued = 0; (w)->queue = NULL; (w)->ticket = 0; INIT_LIST_HEAD(&(w)->entry); } while (0)
#define INIT_DELAYED_WORK(d, f) do { INIT_WORK(&(d)->work, f); (d)->armed = 0; } while (0)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
int lx_queue_work(struct work_struct *w);
int lx_queue_work_on(struct workqueue_struct *queue, struct work_struct *w);
int lx_queue_delayed(struct delayed_work *d, unsigned long delay_ms);
int lx_queue_delayed_on(struct workqueue_struct *queue, struct delayed_work *d, unsigned long delay_ms);
int lx_mod_delayed(struct delayed_work *d, unsigned long delay_ms);
int lx_mod_delayed_on(struct workqueue_struct *queue, struct delayed_work *d, unsigned long delay_ms);
int lx_cancel_work(struct work_struct *w);
int lx_cancel_delayed(struct delayed_work *d);
#define schedule_work(w) lx_queue_work(w)
#define queue_work(q, w) lx_queue_work_on(q, w)
#define queue_delayed_work(q, d, t) lx_queue_delayed_on(q, d, t)
#define mod_delayed_work(q, d, t) lx_mod_delayed_on(q, d, t)
#define schedule_delayed_work(d, t) lx_queue_delayed(d, t)
#define cancel_work_sync(w) lx_cancel_work_sync(w)
#define cancel_delayed_work_sync(d) lx_cancel_delayed_sync(d)
#define cancel_delayed_work(d) lx_cancel_delayed(d)
#define flush_work(w) lx_flush_work(w)
#define flush_delayed_work(w) lx_flush_delayed(w)
#define flush_workqueue(q) lx_flush_workqueue(q)
#define drain_workqueue(q) lx_flush_workqueue(q)
#define destroy_workqueue(q) lx_destroy_workqueue(q)
#define alloc_workqueue(name, flags, max_active, ...) lx_alloc_workqueue(name, flags, max_active, ##__VA_ARGS__)
#define create_singlethread_workqueue(n) alloc_ordered_workqueue(n, 0)
#define WQ_UNBOUND (1u << 1)
#define WQ_HIGHPRI (1u << 4)
#define WQ_MEM_RECLAIM (1u << 3)
/* Native executor role: callbacks that deliver IRQ/timer/device completions
 * may run through a synchronous driver's wait; ordinary control work waits. */
#define LX_WQ_IO_COMPLETION (1u << 29)
struct timer_list { struct delayed_work d; void (*function)(struct timer_list *); unsigned long expires; };
#define timer_pending(t) ((t)->d.armed || (t)->d.work.queued)
#define timer_setup(t, f, fl) do { (t)->function = (f); INIT_DELAYED_WORK(&(t)->d, 0); } while (0)
int lx_timer_mod(struct timer_list *t, unsigned long expires_j);
int lx_timer_del(struct timer_list *t);
int lx_timer_del_sync(struct timer_list *t);
#define mod_timer(t, e) lx_timer_mod(t, e)
#define del_timer(t) lx_timer_del(t)
#define timer_delete(t) lx_timer_del(t)
#define del_timer_sync(t) lx_timer_del_sync(t)
#define timer_delete_sync(t) lx_timer_del_sync(t)
#define from_timer(v, t, f) container_of(t, __typeof__(*v), f)
struct tasklet_struct {
  struct work_struct w; void (*func)(struct tasklet_struct *); void (*legacy)(unsigned long); unsigned long data;
  unsigned disabled; bool pending;
};
void lx_tasklet_init(struct tasklet_struct *tasklet, void (*callback)(struct tasklet_struct *), void (*legacy)(unsigned long), unsigned long data);
#define tasklet_setup(t, f) lx_tasklet_init(t, f, NULL, 0)
void lx_tasklet_schedule(struct tasklet_struct *t);
void lx_tasklet_kill(struct tasklet_struct *t);
void lx_tasklet_disable(struct tasklet_struct *t);
void lx_tasklet_enable(struct tasklet_struct *t);
#define tasklet_schedule(t) lx_tasklet_schedule(t)
#define tasklet_kill(t) lx_tasklet_kill(t)
#define tasklet_disable(t) lx_tasklet_disable(t)
#define tasklet_enable(t) lx_tasklet_enable(t)

/* interrupts: the handler runs from the dispatcher after lx_irq_raise */
typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQ_WAKE_THREAD 2
#define IRQF_SHARED 0x80
#define IRQF_NOBALANCING 0x800
#define IRQF_NO_SUSPEND 0
typedef irqreturn_t (*irq_handler_t)(int, void *);
int lx_request_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread, unsigned long fl, const char *n, void *id);
void lx_free_irq(unsigned int irq, void *id);
void lx_irq_raise(unsigned int irq);
#define request_irq(i, h, f, n, d) lx_request_irq(i, h, 0, f, n, d)
#define request_threaded_irq(i, h, t, f, n, d) lx_request_irq(i, h, t, f, n, d)
#define free_irq(i, d) lx_free_irq(i, d)
void lx_disable_irq(unsigned int irq);
void lx_enable_irq(unsigned int irq);
#define enable_irq(i) lx_enable_irq(i)
#define disable_irq(i) lx_disable_irq(i)
#define disable_irq_nosync(i) lx_disable_irq(i)
#define synchronize_irq(i) ((void)(i))
#define irq_set_affinity_hint(i, m) 0
#define in_interrupt() 0
#define in_irq() 0

/* DMA: buffers come from a window the endpoint's IOMMU stream maps; the platform hook returns the bus address */
enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
void *lx_dma_alloc(struct device *d, size_t n, dma_addr_t *bus);
void *lx_dma_zalloc(size_t n);   /* zeroed memory of the DMA window, to be given back with lx_dma_kfree */
void lx_dma_kfree(void *p);
void lx_dma_free(struct device *d, size_t n, void *p, dma_addr_t bus);
dma_addr_t lx_dma_map(struct device *d, void *p, size_t n, int dir);
void lx_dma_sync(struct device *d, dma_addr_t bus, size_t n, int dir);
#define dma_alloc_coherent(d, n, h, f) lx_dma_alloc(d, n, h)
#define dma_free_coherent(d, n, p, h) lx_dma_free(d, n, p, h)
#define dma_alloc_noncoherent(d, n, h, dir, flags) lx_dma_alloc(d, n, h)
#define dma_free_noncoherent(d, n, p, h, dir) lx_dma_free(d, n, p, h)
#define dma_map_single(d, p, n, dir) lx_dma_map(d, p, n, dir)
#define dma_unmap_single(d, h, n, dir) lx_dma_sync(d, h, n, dir)
#define dma_mapping_error(d, h) ((h) == 0)
#define dma_sync_single_for_cpu(d, h, n, dir) lx_dma_sync(d, h, n, dir)
#define dma_sync_single_for_device(d, h, n, dir) lx_dma_sync(d, h, n, dir)
#define DMA_BIT_MASK(n) ((n) == 64 ? ~0ULL : ((1ULL << (n)) - 1))
#define dma_set_mask_and_coherent(d, m) ((d)->dma_mask = (m), 0)
#define dma_set_mask(d, m) ((d)->dma_mask = (m), 0)
#define dma_set_coherent_mask(d, m) 0

/* firmware: served from files the platform provides */
struct firmware { const u8 *data; size_t size; };
int lx_request_firmware(const struct firmware **fw, const char *name);
int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device,
                            gfp_t flags, void *context, void (*continuation)(const struct firmware *, void *));
void lx_release_firmware(const struct firmware *fw);
#define request_firmware(fw, n, d) lx_request_firmware(fw, n)
#define request_firmware_direct(fw, n, d) lx_request_firmware(fw, n)
#define release_firmware(fw) lx_release_firmware(fw)

/* id allocation */
struct ida { unsigned long bits[8]; };
#define DEFINE_IDA(n) struct ida n = { { 0 } }
int ida_alloc_range(struct ida *i, unsigned int lo, unsigned int hi, gfp_t f);
void ida_free(struct ida *i, unsigned int id);
#define ida_alloc(i, f) ida_alloc_range(i, 0, 511, f)
#define ida_simple_get(i, lo, hi, f) ida_alloc_range(i, lo, (hi) - 1, f)
#define ida_simple_remove(i, id) ida_free(i, id)





#define U8_MAX 0xffu
#define U16_MAX 0xffffu
#define S16_MAX 0x7fff
#define S32_MAX 0x7fffffff
#define ETOOSMALL 525
#define __cpu_to_le16(x) cpu_to_le16(x)
#define __cpu_to_le32(x) cpu_to_le32(x)
#define __cpu_to_le64(x) cpu_to_le64(x)
#define __le16_to_cpu(x) le16_to_cpu(x)
#define __le32_to_cpu(x) le32_to_cpu(x)
#define __le64_to_cpu(x) le64_to_cpu(x)
/* idr: ids index a growable pointer array */
struct idr { void **slot; unsigned int cap; unsigned int next; };
void idr_init(struct idr *i);
void idr_destroy(struct idr *i);
int idr_alloc(struct idr *i, void *p, int start, int end, gfp_t f);
int idr_alloc_cyclic(struct idr *i, void *p, int start, int end, gfp_t f);
void *idr_find(const struct idr *i, unsigned long id);
void *idr_remove(struct idr *i, unsigned long id);
int idr_for_each(const struct idr *i, int (*fn)(int id, void *p, void *data), void *data);
#define DEFINE_IDR(n) struct idr n = { 0, 0, 0 }
#define idr_preload(f)
#define idr_preload_end()
#endif
