/* Runtime for the Linux-derived sources: one dispatcher runs queued work, timers and interrupt handlers; waits run it. */
#include "lx_os.h"
#include <stdlib.h>
#include "lx_plat.h"

#ifndef LX_BOOT_SETTLE_MS
#define LX_BOOT_SETTLE_MS 8000
#endif
#ifndef LX_MAX_DEPTH
#define LX_MAX_DEPTH 16
#endif
const struct lx_platform *lx_plat;
void lx_spin_enter(void) { if (lx_plat && lx_plat->preempt_disable) lx_plat->preempt_disable(); }
void lx_spin_leave(void) { if (lx_plat && lx_plat->preempt_enable) lx_plat->preempt_enable(); }
volatile unsigned long jiffies;
static int module_parameter_lock;
void kernel_param_lock(struct module *module) {
  (void)module;
  while (__atomic_exchange_n(&module_parameter_lock, 1, __ATOMIC_ACQUIRE)) cpu_relax();
}
void kernel_param_unlock(struct module *module) {
  (void)module; __atomic_store_n(&module_parameter_lock, 0, __ATOMIC_RELEASE);
}
void (*lx_mmio_trace)(volatile void *a, u32 v);   /* host tests only */
static struct list_head g_work = LIST_HEAD_INIT(g_work);
static struct list_head g_timers = LIST_HEAD_INIT(g_timers);   /* delayed_work entries ordered by insertion */
static struct list_head g_hrtimers = LIST_HEAD_INIT(g_hrtimers);
static struct { int used; unsigned int irq; irq_handler_t h, t; void *id; int pending; int disabled, strict; } g_irq[64];
static u32 g_seed = 0x2545f491u;
static int g_depth;
int lx_dispatching(void) { return g_depth != 0; }
static int g_irq_polling = 1;
static unsigned g_rcu_readers;
static int g_rcu_lock, g_rcu_draining;
static struct rcu_head *g_rcu_head, *g_rcu_tail;
static void rcu_queue_lock(void) { while (__atomic_exchange_n(&g_rcu_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void rcu_queue_unlock(void) { __atomic_store_n(&g_rcu_lock, 0, __ATOMIC_RELEASE); }
unsigned lx_rcu_readers(void) { return __atomic_load_n(&g_rcu_readers, __ATOMIC_SEQ_CST); }
void lx_rcu_read_lock(void) { __atomic_fetch_add(&g_rcu_readers, 1, __ATOMIC_SEQ_CST); }
static void rcu_drain(void) {
  if (lx_rcu_readers()) return;
  rcu_queue_lock();
  if (lx_rcu_readers() || g_rcu_draining) { rcu_queue_unlock(); return; }
  struct rcu_head *head = g_rcu_head; g_rcu_head = g_rcu_tail = 0;
  g_rcu_draining = 1; rcu_queue_unlock();
  while (head) { struct rcu_head *next = head->next; head->next = 0; head->func(head); head = next; }
  rcu_queue_lock(); g_rcu_draining = 0; rcu_queue_unlock();
}
void lx_rcu_read_unlock(void) {
  unsigned before = __atomic_fetch_sub(&g_rcu_readers, 1, __ATOMIC_SEQ_CST);
  if (!before) lx_bug(__FILE__, __LINE__);
  if (before == 1) rcu_drain();
}
void lx_rcu_call(struct rcu_head *head, void (*callback)(struct rcu_head *)) {
  head->next = 0; head->func = callback;
  rcu_queue_lock();
  if (g_rcu_tail) g_rcu_tail->next = head; else g_rcu_head = head;
  g_rcu_tail = head; rcu_queue_unlock();
}
static void rcu_free_allocation(struct rcu_head *head) { lx_free(head->allocation); }
void lx_rcu_free(void *allocation, struct rcu_head *head) {
  head->allocation = allocation; lx_rcu_call(head, rcu_free_allocation);
}
void lx_synchronize_rcu(void) {
  while (lx_rcu_readers()) { lx_pump(); lx_idle(1); }
  rcu_drain();
}
void lx_rcu_barrier(void) {
  for (;;) {
    lx_synchronize_rcu(); rcu_queue_lock(); int pending = g_rcu_head != 0; rcu_queue_unlock();
    if (!pending) return;
  }
}
/* What runs right now, further out in the nesting: a wait inside one of them pumps again, and like in Linux a work item or an interrupt handler
 * does not run a second time inside itself (it stays queued or pending until the outer run returned). Kept by pointer: a work function may free
 * its own item. */
static const void *g_running[LX_MAX_DEPTH + 2]; static int g_nrunning;
static struct workqueue_struct *g_running_queue[LX_MAX_DEPTH + 2];
static u64 g_running_ticket[LX_MAX_DEPTH + 2], g_work_ticket;
static int is_running(const void *p) { for (int i = 0; i < g_nrunning; i++) if (g_running[i] == p) return 1; return 0; }
static struct workqueue_struct default_queue = { .max_active = LX_MAX_DEPTH, .name = "system" };
static struct workqueue_struct bottom_half_queue = { .max_active = LX_MAX_DEPTH, .flags = WQ_HIGHPRI | LX_WQ_IO_COMPLETION, .name = "bottom-half" };
static unsigned high_priority_runs;
static unsigned mutexes_held;
static struct mutex rtnl_mutex;
static struct { struct workqueue_struct *queue; const struct work_struct *target; u64 ticket; } flushing[LX_MAX_DEPTH + 2];
static unsigned flushing_depth;
struct workqueue_struct *system_wq = &default_queue, *system_dfl_wq = &default_queue;
struct workqueue_struct *system_unbound_wq = &default_queue, *system_freezable_wq = &default_queue;
struct workqueue_struct *system_power_efficient_wq = &default_queue;
struct workqueue_struct *system_bh_wq = &bottom_half_queue, *system_long_wq = &default_queue;
static bool work_runnable(const struct work_struct *work) {
  if (is_running(work)) return false;
  if (__atomic_load_n(&mutexes_held, __ATOMIC_ACQUIRE) && !(work->queue->flags & LX_WQ_IO_COMPLETION)) {
    bool allowed = false;
    for (unsigned i = 0; i < flushing_depth; i++) {
      if (flushing[i].queue != work->queue || work->ticket > flushing[i].ticket) continue;
      if (!flushing[i].target || flushing[i].target == work || work->queue->max_active == 1) { allowed = true; break; }
    }
    if (!allowed) return false;
  }
  unsigned active = 0;
  for (int i = 0; i < g_nrunning; i++) if (g_running_queue[i] == work->queue) active++;
  return active < work->queue->max_active;
}
bool lx_mutex_trylock(struct mutex *mutex) {
  int expected = 0;
  if (!__atomic_compare_exchange_n(&mutex->held, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
  __atomic_fetch_add(&mutexes_held, 1u, __ATOMIC_RELEASE); return true;
}
void lx_mutex_lock(struct mutex *mutex) {
  /* Linux callbacks have one executor. Locked control work is deferred at
   * dispatch; recursive acquisition signals an invalid callback/IO role. */
  BUG_ON(!lx_mutex_trylock(mutex));
}
void lx_mutex_unlock(struct mutex *mutex) {
  BUG_ON(!__atomic_load_n(&mutex->held, __ATOMIC_RELAXED));
  __atomic_store_n(&mutex->held, 0, __ATOMIC_RELEASE);
  BUG_ON(!__atomic_fetch_sub(&mutexes_held, 1u, __ATOMIC_ACQ_REL));
}
void lx_rtnl_lock(void) { mutex_lock(&rtnl_mutex); }
void lx_rtnl_unlock(void) { mutex_unlock(&rtnl_mutex); }
bool lx_rtnl_is_locked(void) { return mutex_is_locked(&rtnl_mutex); }
static char *g_stack_top; unsigned long lx_stack_peak, lx_depth_peak;

u64 lx_now_ns(void) { return lx_plat->now_ns ? lx_plat->now_ns() : lx_plat->now_ms() * 1000000ULL; }
u64 lx_boottime_ns(void) { return lx_plat->boottime_ns ? lx_plat->boottime_ns() : lx_now_ns(); }
u64 lx_now_ms(void) {
  u64 now = lx_plat->now_ns ? lx_plat->now_ns() / 1000000ULL : lx_plat->now_ms();
  jiffies = (unsigned long)now; return now;
}
static u64 hrtimer_now(const struct hrtimer *timer) { return timer->clockid == 7 ? lx_boottime_ns() : lx_now_ns(); }
void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*callback)(struct hrtimer *), int clockid, int mode) {
  (void)mode; INIT_LIST_HEAD(&timer->entry); timer->function = callback; timer->clockid = clockid; timer->armed = false;
}
int hrtimer_cancel(struct hrtimer *timer) {
  /* Callbacks execute on this dispatcher, so cancelling one's own callback is invalid. */
  BUG_ON(is_running(timer));
  int was = timer->armed;
  if (was) { timer->armed = false; list_del_init(&timer->entry); }
  return was;
}
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t time, u64 slack, int mode) {
  (void)slack;
  if (timer->armed) { list_del_init(&timer->entry); timer->armed = false; }
  u64 value = time > 0 ? (u64)time : 0;
  if (mode & HRTIMER_MODE_REL) {
    u64 now = hrtimer_now(timer); timer->expires = value > (u64)INT64_MAX - now ? (u64)INT64_MAX : now + value;
  } else timer->expires = value;
  timer->armed = true; list_add_tail(&timer->entry, &g_hrtimers);
}
u64 hrtimer_forward_now(struct hrtimer *timer, ktime_t interval) {
  u64 now = hrtimer_now(timer);
  if (interval <= 0 || timer->expires > now) return 0;
  u64 count = (now - timer->expires) / (u64)interval + 1;
  timer->expires = count > ((u64)INT64_MAX - timer->expires) / (u64)interval ? (u64)INT64_MAX : timer->expires + count * (u64)interval;
  return count;
}
/* Diagnostic bursts are bounded without adding a wakeup timer. */
int net_ratelimit(void) {
  static u64 window; static unsigned used;
  u64 now = lx_now_ms();
  if (now - window >= 1000) { window = now; used = 0; }
  return used < 10 ? (++used, 1) : 0;
}
void lx_delay_us(unsigned int us) {
  if (!us) return;
  if (lx_plat->busy_delay_us) { lx_plat->busy_delay_us(us); return; }
  if (lx_plat->now_ns) {
    u64 start = lx_now_ns(), interval = (u64)us * 1000;
    while (lx_now_ns() - start < interval) cpu_relax();
  } else {
    /* Millisecond-only host models advance time without running software. */
    lx_plat->sleep_ms(us / 1000 + (us % 1000 != 0));
  }
}
void lx_busy_ms(unsigned int ms) { while (ms--) lx_delay_us(1000); }
void lx_sleep_us(unsigned int us) {
  if (us < 1000) lx_delay_us(us);
  else lx_delay_ms(us / 1000 + (us % 1000 != 0));
}
void lx_warn(const char *f, int l) { lx_log(4, "warning at %s:%d", f, l); }
void lx_bug(const char *f, int l) { lx_log(3, "bug at %s:%d", f, l); lx_plat->halt(); for (;;) {} }
void (*lx_log_tap)(int level, const char *line);
void lx_log(int level, const char *fmt, ...) {
  char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); lx_plat->log(level, b);
  if (lx_log_tap) lx_log_tap(level, b); }
u32 lx_random_u32(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }

void *lx_alloc(size_t n, int zero) { void *p = lx_plat->alloc(n); if (p && zero) memset(p, 0, n); return p; }
void lx_free(const void *p) { if (p) lx_plat->free((void *)p); }
void *lx_memdup(const void *p, size_t n) { void *q = lx_alloc(n, 0); if (q && n) memcpy(q, p, n); return q; }
char *lx_strdup(const char *s) { return lx_memdup(s, strlen(s) + 1); }
int lx_kstrtoul(const char *s, int base, unsigned long *r) { char *e; *r = strtoul(s, &e, base); return e == s ? -EINVAL : 0; }
unsigned long find_first_bit(const unsigned long *a, unsigned long n) { for (unsigned long i = 0; i < n; i++) if (test_bit(i, a)) return i; return n; }
unsigned long find_last_bit(const unsigned long *a, unsigned long n) { for (unsigned long i = n; i; i--) if (test_bit(i - 1, a)) return i - 1; return n; }

/* work */
struct workqueue_struct *lx_alloc_workqueue(const char *name, unsigned flags, unsigned max_active, ...) {
  struct workqueue_struct *queue = lx_alloc(sizeof(*queue), 1);
  if (!queue) return NULL;
  queue->max_active = max_active ? max_active : LX_MAX_DEPTH; queue->flags = flags; queue->dynamic = true;
  va_list args; va_start(args, max_active); vsnprintf(queue->name, sizeof(queue->name), name, args); va_end(args);
  return queue;
}
int lx_queue_work_on(struct workqueue_struct *queue, struct work_struct *w) {
  if (w->queued) return 0;
  BUG_ON(!queue || !w->func);
  w->queue = queue; w->ticket = ++g_work_ticket; w->queued = 1;
  list_add_tail(&w->entry, &g_work); return 1;
}
int lx_queue_work(struct work_struct *w) { return lx_queue_work_on(system_wq, w); }
int lx_cancel_work(struct work_struct *w) { if (!w->queued) return 0; w->queued = 0; list_del_init(&w->entry); return 1; }
int lx_queue_delayed_on(struct workqueue_struct *queue, struct delayed_work *d, unsigned long ms) {
  if (d->armed || d->work.queued) return 0;
  BUG_ON(!queue || !d->work.func);
  if (!ms) return lx_queue_work_on(queue, &d->work);
  d->work.queue = queue;
  list_add_tail(&d->work.entry, &g_timers); d->armed = 1; d->due = lx_now_ms() + ms; return 1; }
int lx_queue_delayed(struct delayed_work *d, unsigned long ms) { return lx_queue_delayed_on(system_wq, d, ms); }
int lx_cancel_delayed(struct delayed_work *d) {
  int was = d->armed; if (was) { d->armed = 0; list_del_init(&d->work.entry); } return lx_cancel_work(&d->work) | was; }
int lx_mod_delayed_on(struct workqueue_struct *queue, struct delayed_work *d, unsigned long ms) {
  int was = lx_cancel_delayed(d); lx_queue_delayed_on(queue, d, ms); return was;
}
int lx_mod_delayed(struct delayed_work *d, unsigned long ms) { return lx_mod_delayed_on(system_wq, d, ms); }
static bool work_ticket_pending(const void *target, struct workqueue_struct *queue, u64 ticket) {
  for (int i = 0; i < g_nrunning; i++)
    if ((target ? g_running[i] == target : g_running_queue[i] == queue) &&
        g_running_ticket[i] && g_running_ticket[i] <= ticket) return true;
  for (struct list_head *p = g_work.next; p != &g_work; p = p->next) {
    struct work_struct *work = container_of(p, struct work_struct, entry);
    if ((target ? (const void *)work == target : work->queue == queue) && work->ticket <= ticket) return true;
  }
  return false;
}
bool lx_flush_work(struct work_struct *work) {
  /* All running callbacks are ancestors on this dispatcher. Waiting for an
   * ancestor cannot make progress, just as flushing one's own Linux work. */
  BUG_ON(is_running(work));
  if (!work->queued) return false;
  u64 ticket = work->ticket;
  BUG_ON(flushing_depth >= ARRAY_SIZE(flushing));
  flushing[flushing_depth].queue = work->queue; flushing[flushing_depth].target = work; flushing[flushing_depth++].ticket = ticket;
  while (work_ticket_pending(work, NULL, ticket)) if (!lx_pump()) lx_idle(1);
  flushing_depth--;
  return true;
}
bool lx_cancel_work_sync(struct work_struct *work) { BUG_ON(is_running(work)); return lx_cancel_work(work); }
bool lx_cancel_delayed_sync(struct delayed_work *work) { BUG_ON(is_running(&work->work)); return lx_cancel_delayed(work); }
bool lx_flush_delayed(struct delayed_work *work) {
  if (work->armed) {
    struct workqueue_struct *queue = work->work.queue;
    lx_cancel_delayed(work); lx_queue_work_on(queue, &work->work);
  }
  return lx_flush_work(&work->work);
}
void lx_flush_workqueue(struct workqueue_struct *queue) {
  BUG_ON(!queue);
  for (int i = 0; i < g_nrunning; i++) BUG_ON(g_running_queue[i] == queue);
  u64 ticket = g_work_ticket;
  BUG_ON(flushing_depth >= ARRAY_SIZE(flushing));
  flushing[flushing_depth].queue = queue; flushing[flushing_depth].target = NULL; flushing[flushing_depth++].ticket = ticket;
  while (work_ticket_pending(NULL, queue, ticket)) if (!lx_pump()) lx_idle(1);
  flushing_depth--;
}
void lx_destroy_workqueue(struct workqueue_struct *queue) {
  if (!queue) return;
  BUG_ON(!queue->dynamic);
  do { lx_flush_workqueue(queue); } while (work_ticket_pending(NULL, queue, g_work_ticket));
  /* Delayed work is the caller's lifetime obligation; catch stale users. */
  for (struct list_head *p = g_timers.next; p != &g_timers; p = p->next)
    BUG_ON(container_of(p, struct delayed_work, work.entry)->work.queue == queue);
  lx_free(queue);
}
static void timer_tramp(struct work_struct *w) {
  struct timer_list *t = container_of(w, struct timer_list, d.work); t->function(t); }
int lx_timer_mod(struct timer_list *t, unsigned long exp) {
  unsigned long now = (unsigned long)lx_now_ms(); int was = lx_cancel_delayed(&t->d);
  t->expires = exp;
  t->d.work.func = timer_tramp;
  lx_queue_delayed_on(system_bh_wq, &t->d, (long)(exp - now) > 0 ? exp - now : 0); return was; }
int lx_timer_del(struct timer_list *t) { return lx_cancel_delayed(&t->d); }
int lx_timer_del_sync(struct timer_list *t) { return lx_cancel_delayed_sync(&t->d); }
static void tasklet_tramp(struct work_struct *w) {
  struct tasklet_struct *t = container_of(w, struct tasklet_struct, w);
  if (t->disabled || !t->pending) return;
  t->pending = false;
  if (t->legacy) t->legacy(t->data); else if (t->func) t->func(t); }
void lx_tasklet_init(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *), void (*legacy)(unsigned long), unsigned long data) {
  INIT_WORK(&t->w, tasklet_tramp); t->func = callback; t->legacy = legacy; t->data = data; t->disabled = 0; t->pending = false;
}
void lx_tasklet_schedule(struct tasklet_struct *t) {
  t->pending = true;
  if (!t->disabled) lx_queue_work_on(system_bh_wq, &t->w);
}
void lx_tasklet_kill(struct tasklet_struct *t) { lx_cancel_work_sync(&t->w); t->pending = false; }
void lx_tasklet_disable(struct tasklet_struct *t) { t->disabled++; lx_cancel_work_sync(&t->w); }
void lx_tasklet_enable(struct tasklet_struct *t) {
  BUG_ON(!t->disabled);
  if (!--t->disabled && t->pending) lx_queue_work_on(system_bh_wq, &t->w);
}

/* interrupts */
int lx_request_irq(unsigned int irq, irq_handler_t h, irq_handler_t t, unsigned long fl, const char *n, void *id) {
  (void)fl; for (int i = 0; i < 64; i++) if (!g_irq[i].used) { g_irq[i].used = 1; g_irq[i].irq = irq; g_irq[i].h = h; g_irq[i].t = t; g_irq[i].id = id; g_irq[i].pending = 0; g_irq[i].disabled = 0; g_irq[i].strict = n && (!strcmp(n, "bhi") || !strcmp(n, "mhi")); return 0; }
  return -ENOMEM; }
void lx_free_irq(unsigned int irq, void *id) { for (int i = 0; i < 64; i++) if (g_irq[i].used && g_irq[i].irq == irq && g_irq[i].id == id) g_irq[i].used = 0; }
/* Disabling holds the bus controller's handlers ("bhi", "mhi"), whose disable and enable pair up around a power down: a disabled one keeps
 * what is pending and runs nothing until it is enabled again. The radio's copy engine and data groups share vectors with each other, so a
 * depth per vector would not pair up for them; their handlers check their own enable flags and keep running as before. */
void lx_disable_irq(unsigned int irq) { for (int i = 0; i < 64; i++) if (g_irq[i].used && g_irq[i].strict && g_irq[i].irq == irq) g_irq[i].disabled++; }
void lx_enable_irq(unsigned int irq) { for (int i = 0; i < 64; i++) if (g_irq[i].used && g_irq[i].strict && g_irq[i].irq == irq && g_irq[i].disabled) g_irq[i].disabled--; }
void lx_irq_raise(unsigned int irq) { for (int i = 0; i < 64; i++) if (g_irq[i].used && g_irq[i].irq == irq) g_irq[i].pending = 1; }
void lx_irq_polling(int enabled) { g_irq_polling = enabled != 0; }

unsigned lx_next_wait_ms(unsigned maximum) {
  u64 now = lx_now_ms();
  for (int i = 0; i < 64; i++)
    if (g_irq[i].used && g_irq[i].pending && !g_irq[i].disabled && !is_running(&g_irq[i])) return 0;
  for (struct list_head *p = g_work.next; p != &g_work; p = p->next)
    if (work_runnable(container_of(p, struct work_struct, entry))) return 0;
  for (struct list_head *p = g_timers.next; p != &g_timers; p = p->next) {
    struct delayed_work *d = container_of(p, struct delayed_work, work.entry);
    if (!d->armed || is_running(&d->work)) continue;
    if (d->due <= now) return 0;
    if (d->due - now < maximum) maximum = d->due - now;
  }
  for (struct list_head *p = g_hrtimers.next; p != &g_hrtimers; p = p->next) {
    struct hrtimer *timer = container_of(p, struct hrtimer, entry);
    if (!timer->armed || is_running(timer)) continue;
    u64 now_ns = hrtimer_now(timer);
    if (timer->expires <= now_ns) return 0;
    u64 delta = timer->expires - now_ns;
    u64 wait = delta / 1000000ULL + (delta % 1000000ULL != 0);
    if (wait < maximum) maximum = (unsigned)wait;
  }
  return g_irq_polling && maximum > 1 ? 1 : maximum;
}

void lx_irq_raise_all(void);
volatile uint64_t nw_wifi_where[4];
static void where(int kind, const void *fn) {
  nw_wifi_where[0] = kind; nw_wifi_where[1] = fn ? (uint64_t)((uintptr_t)fn - (uintptr_t)lx_now_ms) : 0;
  nw_wifi_where[2] = g_depth; nw_wifi_where[3] = lx_now_ms(); }
/* The dispatcher: interrupts first, then due timers, then one round of queued work. */
int lx_pump(void) {
  int ran = 0;
  if (g_depth > LX_MAX_DEPTH) return 0;
  { char here; if (!g_stack_top) g_stack_top = &here; unsigned long used = (unsigned long)(g_stack_top - &here); if (used > lx_stack_peak && used < (1UL << 24)) lx_stack_peak = used; if ((unsigned long)g_depth + 1 > lx_depth_peak) lx_depth_peak = g_depth + 1; }
  g_depth++;
  where(4, (const void *)lx_plat->poll);
  lx_plat->poll();
  /* Poll only until the board attaches real MSI delivery, or after its fallback. */
  { static u64 last_raise; u64 t = lx_now_ms(); if (g_irq_polling && t != last_raise) { last_raise = t; lx_irq_raise_all(); } }
  for (int i = 0; i < 64; i++) if (g_irq[i].used && g_irq[i].pending && !g_irq[i].disabled && !is_running(&g_irq[i])) {
    g_irq[i].pending = 0; ran = 1;
    where(1, (const void *)g_irq[i].h);
    g_running_queue[g_nrunning] = NULL; g_running_ticket[g_nrunning] = 0;
    g_running[g_nrunning++] = &g_irq[i];
    irqreturn_t r = g_irq[i].h ? g_irq[i].h(g_irq[i].irq, g_irq[i].id) : IRQ_WAKE_THREAD;
    if (r == IRQ_WAKE_THREAD && g_irq[i].t) g_irq[i].t(g_irq[i].irq, g_irq[i].id);
    g_nrunning--;
  }
  LIST_HEAD(ready_timers);
  for (unsigned budget = 0; budget < 64; budget++) {
    u64 now = lx_now_ms(); struct delayed_work *due = 0;
    /* A callback can cancel and free another timer: never retain the next
     * list entry across a callback (including callbacks in nested waits). */
    for (struct list_head *p = g_timers.next; p != &g_timers; p = p->next) {
      struct delayed_work *d = container_of(p, struct delayed_work, work.entry);
      if (d->armed && now >= d->due && !is_running(&d->work)) { due = d; break; }
    }
    if (!due) break;
    due->armed = 0; list_del_init(&due->work.entry); ran = 1;
    lx_queue_work_on(due->work.queue, &due->work);
    /* Due timers retain precedence over ordinary work. Ordered queues keep
     * their FIFO position, including work queued before the timer expired. */
    if (due->work.queue->max_active != 1) {
      list_del_init(&due->work.entry); list_add_tail(&due->work.entry, &ready_timers);
    }
  }
  if (!list_empty(&ready_timers)) {
    list_splice_tail_init(&g_work, &ready_timers);
    list_splice_tail_init(&ready_timers, &g_work);
  }
  /* one item at a time off the shared list, so a wait inside a work function still reaches work queued behind it */
  for (unsigned budget = 0; budget < 64; budget++) {
    struct hrtimer *due = NULL;
    for (struct list_head *p = g_hrtimers.next; p != &g_hrtimers; p = p->next) {
      struct hrtimer *timer = container_of(p, struct hrtimer, entry);
      if (timer->armed && timer->expires <= hrtimer_now(timer) && !is_running(timer)) { due = timer; break; }
    }
    if (!due) break;
    due->armed = false; list_del_init(&due->entry); ran = 1; where(2, (const void *)due->function);
    g_running_queue[g_nrunning] = NULL; g_running_ticket[g_nrunning] = 0;
    g_running[g_nrunning++] = due;
    enum hrtimer_restart restart = due->function(due); g_nrunning--;
    /* NORESTART callbacks may have freed their timer. */
    if (restart == HRTIMER_RESTART && !due->armed) { due->armed = true; list_add_tail(&due->entry, &g_hrtimers); }
  }
  const u64 work_started = lx_now_ms();
  for (int budget = 64; budget > 0; budget--) {
    struct work_struct *w = 0; struct list_head *e;
    for (e = g_work.next; e != &g_work; e = e->next) {
      struct work_struct *candidate = container_of(e, struct work_struct, entry);
      if (!work_runnable(candidate)) continue;
      if (!w) w = candidate;
      bool high = !!(candidate->queue->flags & WQ_HIGHPRI);
      if (high == (high_priority_runs < 8)) { w = candidate; break; }
    }
    if (!w) break;
    if (w->queue->flags & WQ_HIGHPRI) { if (high_priority_runs < 8) high_priority_runs++; }
    else high_priority_runs = 0;
    list_del_init(&w->entry); w->queued = 0; ran = 1;
    where(3, (const void *)w->func);
    g_running_queue[g_nrunning] = w->queue; g_running_ticket[g_nrunning] = w->ticket;
    g_running[g_nrunning++] = w; if (w->func) w->func(w); g_nrunning--;
    if (lx_now_ms() - work_started >= 2) break;
  }
  g_depth--;
  rcu_drain();
  where(0, 0);
  return ran;
}
void lx_idle(unsigned int ms) { lx_plat->sleep_ms(ms); }
void lx_delay_ms(unsigned int ms) {
  u64 end = lx_now_ms() + ms;
  while (lx_now_ms() < end) { if (!lx_pump()) lx_idle(1); }
}

/* device model */
#define MAXBUS 4
static const struct bus_type *g_bus[MAXBUS]; static struct device_driver *g_drv[16];
static int match_probe(struct device *d, struct device_driver *dr) {
  if (!d->bus || dr->bus != d->bus) return 0;
  if (d->bus->match && !d->bus->match(d, dr)) return 0;
  d->driver = dr; if (d->bus->probe) { int r = d->bus->probe(d); if (r) { d->driver = 0; return r; } }
  return 0; }
int bus_register(const struct bus_type *b) { for (int i = 0; i < MAXBUS; i++) if (!g_bus[i]) { g_bus[i] = b; return 0; } return -ENOMEM; }
void bus_unregister(const struct bus_type *b) { for (int i = 0; i < MAXBUS; i++) if (g_bus[i] == b) g_bus[i] = 0; }
static struct device g_root;
int driver_register(struct device_driver *dr) {
  for (int i = 0; i < 16; i++) if (!g_drv[i]) { g_drv[i] = dr; break; }
  for (int i = 0; i < 64; i++) (void)0;
  /* devices created before the driver are matched from the registry */
  extern void lx_probe_all(struct device_driver *);
  lx_probe_all(dr); return 0; }
void driver_unregister(struct device_driver *dr) { for (int i = 0; i < 16; i++) if (g_drv[i] == dr) g_drv[i] = 0; }
static struct device *g_devs[128];
static const struct class *g_classes[16];
static bool class_registered(const struct class *class) {
  for (unsigned i = 0; i < ARRAY_SIZE(g_classes); i++) if (g_classes[i] == class) return true;
  return false;
}
int class_register(const struct class *class) {
  if (!class || !class->name || !class->name[0]) return -EINVAL;
  unsigned slot = ARRAY_SIZE(g_classes);
  for (unsigned i = 0; i < ARRAY_SIZE(g_classes); i++) {
    if (g_classes[i] && !strcmp(g_classes[i]->name, class->name)) return -EEXIST;
    if (!g_classes[i] && slot == ARRAY_SIZE(g_classes)) slot = i;
  }
  if (slot == ARRAY_SIZE(g_classes)) return -ENOSPC;
  g_classes[slot] = class; return 0;
}
void class_unregister(const struct class *class) {
  for (unsigned i = 0; i < ARRAY_SIZE(g_devs); i++) BUG_ON(g_devs[i] && g_devs[i]->class == class);
  for (unsigned i = 0; i < ARRAY_SIZE(g_classes); i++) if (g_classes[i] == class) { g_classes[i] = NULL; return; }
}
void lx_probe_all(struct device_driver *dr) { for (int i = 0; i < 128; i++) if (g_devs[i] && !g_devs[i]->driver) match_probe(g_devs[i], dr); }
void device_initialize(struct device *d) {
  if (!d->references) __atomic_store_n(&d->references, 1u, __ATOMIC_RELEASE);
  if (!d->children.next) INIT_LIST_HEAD(&d->children);
  if (!d->sibling.next) INIT_LIST_HEAD(&d->sibling);
}
struct device *get_device(struct device *d) {
  if (d) { BUG_ON(!__atomic_load_n(&d->references, __ATOMIC_ACQUIRE)); __atomic_fetch_add(&d->references, 1u, __ATOMIC_RELAXED); }
  return d;
}
void put_device(struct device *d) {
  if (!d || __atomic_fetch_sub(&d->references, 1u, __ATOMIC_ACQ_REL) != 1) return;
  if (d->release) d->release(d); else if (d->type && d->type->release) d->type->release(d);
  else if (d->class && d->class->dev_release) d->class->dev_release(d);
}
int dev_set_name(struct device *d, const char *format, ...) {
  va_list args; va_start(args, format);
  int written = vsnprintf(d->namebuf, sizeof(d->namebuf), format, args); va_end(args);
  if (written < 0 || (size_t)written >= sizeof(d->namebuf)) return -ENAMETOOLONG;
  d->name = d->namebuf; return 0;
}
int device_rename(struct device *dev, const char *name) {
  char copy[sizeof(dev->namebuf)]; size_t length = 0;
  while (length < sizeof(copy) && name[length]) { copy[length] = name[length]; length++; }
  if (!length) return -EINVAL;
  if (length == sizeof(copy)) return -ENAMETOOLONG;
  copy[length] = 0; memcpy(dev->namebuf, copy, length + 1); dev->name = dev->namebuf; return 0;
}
int device_add(struct device *d) {
  if (d->registered) return -EBUSY;
  if (!d->references) return -EINVAL;
  if (d->class && !class_registered(d->class)) return -EINVAL;
  unsigned slot; for (slot = 0; slot < ARRAY_SIZE(g_devs) && g_devs[slot]; slot++) { }
  if (slot == ARRAY_SIZE(g_devs)) return -ENOSPC;
  if (d->parent) { get_device(d->parent); list_add_tail(&d->sibling, &d->parent->children); }
  g_devs[slot] = d;
  d->registered = 1;
  for (int i = 0; i < 16; i++) if (g_drv[i] && !d->driver) match_probe(d, g_drv[i]);
  return 0; }
int device_register(struct device *d) { device_initialize(d); return device_add(d); }
void device_del(struct device *d) {
  if (!d->registered) return; d->registered = 0;
  if (d->driver && d->bus && d->bus->remove) d->bus->remove(d);
  d->driver = 0;
  if (d->parent) { list_del_init(&d->sibling); put_device(d->parent); }
  for (int i = 0; i < 128; i++) if (g_devs[i] == d) g_devs[i] = 0;
}
void device_unregister(struct device *d) { if (d && d->registered) { device_del(d); put_device(d); } }
int device_for_each_child(struct device *d, void *data, int (*fn)(struct device *, void *)) {
  if (!d->children.next) return 0;
  struct list_head *p, *n; int r = 0;
  for (p = d->children.next; p != &d->children; p = n) { n = p->next; r = fn(container_of(p, struct device, sibling), data); if (r) break; }
  return r; }
void lx_root_init(void) { device_initialize(&g_root); }

/* ids */
int ida_alloc_range(struct ida *i, unsigned int lo, unsigned int hi, gfp_t f) {
  (void)f; for (unsigned int n = lo; n <= hi && n < 512; n++) if (!test_bit(n, i->bits)) { set_bit(n, i->bits); return n; } return -ENOSPC; }
void ida_free(struct ida *i, unsigned int id) { clear_bit(id, i->bits); }

/* DMA and firmware go to the platform */
void *lx_dma_alloc(struct device *d, size_t n, dma_addr_t *bus) { (void)d; return lx_plat->dma_alloc(n, bus); }
void *lx_dma_zalloc(size_t n) { u64 bus; return lx_plat->dma_alloc(n, &bus); }
void lx_dma_kfree(void *p) { if (p) lx_plat->dma_free(0, p, 0); }
void lx_dma_free(struct device *d, size_t n, void *p, dma_addr_t bus) { (void)d; lx_plat->dma_free(n, p, bus); }
dma_addr_t lx_dma_map(struct device *d, void *p, size_t n, int dir) { (void)d; return lx_plat->dma_map(p, n, dir); }
void lx_dma_sync(struct device *d, dma_addr_t bus, size_t n, int dir) { (void)d; lx_plat->dma_sync(bus, n, dir); }
static int has_amss(const char *n) { for (; *n; n++) if (n[0] == 'a' && n[1] == 'm' && n[2] == 's' && n[3] == 's') return 1; return 0; }
int lx_request_firmware(const struct firmware **fw, const char *name) {
  struct firmware *f = lx_alloc(sizeof *f, 1); if (!f) return -ENOMEM;
  if (lx_plat->fw_get(name, &f->data, &f->size)) { lx_free(f); return -ENOENT; }
  static int settled;
  if (!settled && has_amss(name) && f->size >= 4096) {
    u32 sum = 0; const u32 *w = (const u32 *)f->data; for (size_t i = 0; i < 0x80000 / 4 && i < f->size / 4; i++) sum += w[i];
    lx_log(6, "firmware %s: %zu bytes, sum of the first 512 KiB %08x, first word %08x", name, f->size, sum, w[0]);
    settled = 1; }
  *fw = f; return 0; }
void lx_release_firmware(const struct firmware *fw) { if (fw) lx_free(fw); }
struct firmware_job {
  struct work_struct work; char *name; struct device *device;
  void *context; void (*continuation)(const struct firmware *, void *);
};
static void firmware_job_run(struct work_struct *work) {
  struct firmware_job *job = container_of(work, struct firmware_job, work);
  const struct firmware *firmware = NULL;
  lx_request_firmware(&firmware, job->name);
  job->continuation(firmware, job->context);
  put_device(job->device); lx_free(job->name); lx_free(job);
}
int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device,
                            gfp_t flags, void *context, void (*continuation)(const struct firmware *, void *)) {
  (void)module; (void)uevent; (void)flags;
  if (!name || !continuation) return -EINVAL;
  struct firmware_job *job = lx_alloc(sizeof(*job), 1); if (!job) return -ENOMEM;
  job->name = lx_strdup(name); if (!job->name) { lx_free(job); return -ENOMEM; }
  job->device = get_device(device); job->context = context; job->continuation = continuation;
  INIT_WORK(&job->work, firmware_job_run); lx_queue_work(&job->work); return 0;
}

/* idr */
void idr_init(struct idr *i) { i->slot = 0; i->cap = 0; i->next = 0; }
void idr_destroy(struct idr *i) { lx_free(i->slot); idr_init(i); }
static int idr_grow(struct idr *i, unsigned int need) {
  if (need < i->cap) return 0;
  unsigned int nc = i->cap ? i->cap : 16; while (nc <= need) nc *= 2;
  void **n = lx_alloc(sizeof(void *) * nc, 1); if (!n) return -ENOMEM;
  if (i->slot) memcpy(n, i->slot, sizeof(void *) * i->cap);
  lx_free(i->slot); i->slot = n; i->cap = nc; return 0; }
int idr_alloc(struct idr *i, void *p, int start, int end, gfp_t f) {
  (void)f; unsigned int hi = end > 0 ? (unsigned int)end : 0x7fffffffu;
  for (unsigned int n = start; n < hi; n++) {
    if (n < i->cap && i->slot[n]) continue;
    if (idr_grow(i, n)) return -ENOMEM;
    i->slot[n] = p; return n; }
  return -ENOSPC; }
int idr_alloc_cyclic(struct idr *i, void *p, int start, int end, gfp_t f) {
  unsigned int from = i->next > (unsigned int)start ? i->next : (unsigned int)start;
  int r = idr_alloc(i, p, from, end, f); if (r < 0) r = idr_alloc(i, p, start, end, f);
  if (r >= 0) i->next = r + 1; return r; }
void *idr_find(const struct idr *i, unsigned long id) { return id < i->cap ? i->slot[id] : 0; }
void *idr_remove(struct idr *i, unsigned long id) { void *p = 0; if (id < i->cap) { p = i->slot[id]; i->slot[id] = 0; } return p; }
int idr_for_each(const struct idr *i, int (*fn)(int, void *, void *), void *data) {
  for (unsigned int n = 0; n < i->cap; n++) if (i->slot[n]) { int r = fn(n, i->slot[n], data); if (r) return r; }
  return 0; }

/* without interrupt delivery from the endpoint, every registered handler is given a turn each round; handlers check their own status */
void lx_irq_raise_all(void);
void lx_irq_raise_all(void) { for (int i = 0; i < 64; i++) if (g_irq[i].used) g_irq[i].pending = 1; }
