#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_plat.h"
#include <linux/rfkill.h>
#include <linux/device/faux.h>

static u64 ticks;
static u64 fractional_ns, sleep_ns;
static unsigned irq_calls[2], work_calls, timer_calls, cancelled_calls, nested_calls;
static u64 timer_at;
static unsigned deferred_calls;
static u64 now(void) { return ticks; }
static u64 now_ns(void) { return ticks * 1000000ULL + fractional_ns; }
static u64 boot_ns(void) { return now_ns() + sleep_ns; }
static void sleep_ms(unsigned ms) { ticks += ms; }
static void busy_us(unsigned us) {
  u64 value = fractional_ns + (u64)us * 1000; ticks += value / 1000000; fractional_ns = value % 1000000;
}
static void poll(void) {}
static void log_line(int level, const char *line) { (void)level; (void)line; }
static void halt(void) { abort(); }
static bool firmware_available;
static unsigned runtime_allocations, runtime_frees;
static void *runtime_alloc(size_t length) { void *memory = malloc(length); if (memory) runtime_allocations++; return memory; }
static void runtime_free(void *memory) { runtime_frees++; free(memory); }
static int firmware_get(const char *name, const u8 **data, size_t *len) {
  static const u8 blob[] = {1, 2, 3, 4};
  if (!firmware_available || strcmp(name, "test.bin")) return -ENOENT;
  *data = blob; *len = sizeof(blob); return 0;
}
static const struct lx_platform platform = {
  .now_ms = now, .sleep_ms = sleep_ms, .poll = poll,
  .log = log_line, .halt = halt, .alloc = runtime_alloc, .free = runtime_free,
  .fw_get = firmware_get,
  .now_ns = now_ns, .boottime_ns = boot_ns,
  .busy_delay_us = busy_us,
};
static irqreturn_t irq(int number, void *id) {
  (void)number; irq_calls[(uintptr_t)id]++; return IRQ_HANDLED;
}
static struct work_struct work;
static struct delayed_work timer;
static void busy_work(struct work_struct *item) {
  work_calls++; ticks++;
  if (work_calls < 9) lx_queue_work(item);
}
static void timer_work(struct work_struct *item) { (void)item; timer_calls++; timer_at = ticks; }
static struct delayed_work canceller, *cancelled;
static void cancelled_work(struct work_struct *item) { (void)item; cancelled_calls++; }
static void cancel_other(struct work_struct *item) {
  (void)item; lx_cancel_delayed(cancelled); free(cancelled); cancelled = NULL;
}
static void nested_work(struct work_struct *item) {
  (void)item; nested_calls++; lx_delay_ms(3); assert(nested_calls == 1);
}
static void never_idle(struct work_struct *item) { ticks++; lx_queue_work(item); }
static void deferred(struct rcu_head *head) { deferred_calls++; free(head); }
static unsigned device_releases;
static void release_device(struct device *dev) { device_releases++; free(dev); }
static void device_lifetime(void) {
  struct device *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
  assert(parent && child); parent->release = release_device; child->release = release_device;
  device_initialize(parent); assert(!dev_set_name(parent, "wifi-parent-%d", 1));
  assert(!strcmp(dev_name(parent), "wifi-parent-1") && !device_add(parent));
  child->parent = parent; assert(!device_register(child) && parent->references == 2);
  get_device(child); device_unregister(child);
  assert(!device_releases && parent->references == 1 && child->references == 1);
  device_del(parent); assert(!device_releases && parent->references == 1);
  put_device(parent); assert(device_releases == 1);
  put_device(child); assert(device_releases == 2);
}
static unsigned firmware_callbacks, releases_before_callback;
static void firmware_complete(const struct firmware *fw, void *context) {
  assert(context == &firmware_callbacks && device_releases == releases_before_callback);
  assert(!!fw == firmware_available);
  if (fw) { assert(fw->size == 4 && fw->data[3] == 4); lx_release_firmware(fw); }
  firmware_callbacks++;
}
static unsigned modified_calls;
static void modified_callback(struct work_struct *work) { (void)work; modified_calls++; }
static void asynchronous_lifetimes(void) {
  for (unsigned available = 0; available < 2; available++) {
    struct device *dev = calloc(1, sizeof(*dev)); assert(dev); device_initialize(dev); dev->release = release_device;
    firmware_available = available; releases_before_callback = device_releases;
    assert(!request_firmware_nowait(NULL, false, "test.bin", dev, GFP_KERNEL, &firmware_callbacks, firmware_complete));
    put_device(dev); assert(device_releases == releases_before_callback);
    lx_pump(); assert(firmware_callbacks == available + 1 && device_releases == releases_before_callback + 1);
  }
  DECLARE_DELAYED_WORK(modified, modified_callback);
  u64 start = ticks;
  assert(lx_queue_delayed(&modified, 10) && !lx_queue_delayed(&modified, 1) && modified.due == start + 10);
  assert(lx_mod_delayed(&modified, 2) && modified.due == start + 2);
  ticks += 2; lx_pump(); assert(modified_calls == 1);
  assert(lx_queue_delayed(&modified, 10) && lx_mod_delayed(&modified, 0));
  lx_pump(); assert(modified_calls == 2 && !delayed_work_pending(&modified));
}
static unsigned hr_calls, hr_repeat_calls;
static enum hrtimer_restart hr_once(struct hrtimer *timer) { (void)timer; hr_calls++; return HRTIMER_NORESTART; }
static enum hrtimer_restart hr_repeat(struct hrtimer *timer) {
  hr_repeat_calls++;
  if (hr_repeat_calls == 1) { assert(hrtimer_forward_now(timer, 1000000) == 1); return HRTIMER_RESTART; }
  return HRTIMER_NORESTART;
}
static struct hrtimer *hr_cancelled;
static enum hrtimer_restart hr_cancel_other(struct hrtimer *timer) {
  (void)timer; assert(hrtimer_cancel(hr_cancelled) == 1); free(hr_cancelled); hr_cancelled = NULL; return HRTIMER_NORESTART;
}
static void precise_timers(void) {
  struct hrtimer timer, repeating, canceller;
  fractional_ns = 900000; assert(ktime_get() % 1000000 == 900000);
  hrtimer_setup(&timer, hr_once, 1, HRTIMER_MODE_REL);
  hrtimer_start(&timer, 200000, HRTIMER_MODE_REL); assert(lx_next_wait_ms(1000) == 1);
  lx_pump(); assert(!hr_calls);
  ticks++; fractional_ns = 100000; lx_pump(); assert(hr_calls == 1 && !hrtimer_is_queued(&timer));
  hrtimer_start(&timer, (ktime_t)now_ns() + 300000, HRTIMER_MODE_ABS);
  fractional_ns = 399999; lx_pump(); assert(hr_calls == 1);
  fractional_ns = 400000; lx_pump(); assert(hr_calls == 2);
  hrtimer_setup(&timer, hr_once, 7, HRTIMER_MODE_REL);
  hrtimer_start_range_ns(&timer, 1000000000, 1000000, HRTIMER_MODE_REL);
  assert(lx_next_wait_ms(2000) == 1000); sleep_ns += 2000000000;
  assert(lx_next_wait_ms(2000) == 0); lx_pump(); assert(hr_calls == 3);
  hrtimer_setup(&repeating, hr_repeat, 1, HRTIMER_MODE_REL);
  hrtimer_start(&repeating, 1000000, HRTIMER_MODE_REL);
  ticks++; lx_pump(); assert(hr_repeat_calls == 1 && hrtimer_is_queued(&repeating));
  ticks++; lx_pump(); assert(hr_repeat_calls == 2 && !hrtimer_is_queued(&repeating));
  hr_cancelled = calloc(1, sizeof(*hr_cancelled)); assert(hr_cancelled);
  hrtimer_setup(&canceller, hr_cancel_other, 1, HRTIMER_MODE_REL); hrtimer_setup(hr_cancelled, hr_once, 1, HRTIMER_MODE_REL);
  hrtimer_start(&canceller, 1000000, HRTIMER_MODE_REL); hrtimer_start(hr_cancelled, 1000000, HRTIMER_MODE_REL);
  ticks++; lx_pump(); assert(!hr_cancelled && hr_calls == 3);
}
static unsigned radio_calls, radio_polls; static bool radio_fail;
static int radio_block(void *context, bool blocked) { (void)blocked; assert(context == &radio_calls); radio_calls++; return radio_fail ? -EIO : 0; }
static void radio_poll(struct rfkill *radio, void *context) { (void)radio; (void)context; radio_polls++; }
static void radio_lifetime(void) {
  static const struct rfkill_ops ops = { .set_block = radio_block };
  struct device *dev = calloc(1, sizeof(*dev)); assert(dev); device_initialize(dev); dev->release = release_device;
  unsigned releases = device_releases;
  struct rfkill *radio = rfkill_alloc("test-radio", dev, RFKILL_TYPE_WLAN, &ops, &radio_calls); assert(radio);
  assert(!rfkill_register(radio) && !rfkill_blocked(radio)); put_device(dev);
  assert(!lx_rfkill_set_block(radio, true) && rfkill_blocked(radio) && radio_calls == 1);
  assert(!lx_rfkill_set_block(radio, true) && radio_calls == 1);
  radio_fail = true; assert(lx_rfkill_set_block(radio, false) == -EIO && rfkill_blocked(radio));
  radio_fail = false; assert(!lx_rfkill_set_block(radio, false) && !rfkill_blocked(radio));
  assert(rfkill_set_hw_state_reason(radio, true, RFKILL_HARD_BLOCK_SIGNAL));
  assert(rfkill_set_hw_state_reason(radio, true, RFKILL_HARD_BLOCK_NOT_OWNER));
  assert(rfkill_set_hw_state_reason(radio, false, RFKILL_HARD_BLOCK_SIGNAL));
  assert(!rfkill_set_hw_state_reason(radio, false, RFKILL_HARD_BLOCK_NOT_OWNER));
  rfkill_destroy(radio); assert(device_releases == releases + 1 && lx_next_wait_ms(10000) == 10000);
  static const struct rfkill_ops poll_ops = { .poll = radio_poll };
  radio = rfkill_alloc("polled-radio", NULL, RFKILL_TYPE_WLAN, &poll_ops, NULL); assert(radio && !rfkill_register(radio));
  lx_pump(); assert(radio_polls == 1 && lx_next_wait_ms(10000) == 5000);
  rfkill_pause_polling(radio); ticks += 5000; lx_pump(); assert(radio_polls == 1);
  rfkill_resume_polling(radio); lx_pump(); assert(radio_polls == 2);
  rfkill_destroy(radio); ticks += 5000; lx_pump(); assert(radio_polls == 2);
}
static unsigned ordered_sequence[8], ordered_used, flush_calls, freed_work_calls;
static struct workqueue_struct *ordered_queue;
static void ordered_first(struct work_struct *work) {
  (void)work; ordered_sequence[ordered_used++] = 1;
  lx_delay_ms(2);
  assert(ordered_used == 2 && ordered_sequence[1] == 2);
  ordered_sequence[ordered_used++] = 3;
}
static void independent_work(struct work_struct *work) { (void)work; ordered_sequence[ordered_used++] = 2; }
static void ordered_last(struct work_struct *work) { (void)work; ordered_sequence[ordered_used++] = 4; }
static void flushed_callback(struct work_struct *work) { (void)work; flush_calls++; }
static void freeing_callback(struct work_struct *work) { freed_work_calls++; free(work); }
static void workqueue_contracts(void) {
  ordered_queue = alloc_ordered_workqueue("radio-%u", WQ_MEM_RECLAIM, 3u);
  assert(ordered_queue && ordered_queue->max_active == 1 && !strcmp(ordered_queue->name, "radio-3"));
  DECLARE_WORK(first, ordered_first); DECLARE_WORK(last, ordered_last); DECLARE_WORK(other, independent_work);
  assert(queue_work(ordered_queue, &first) && queue_work(ordered_queue, &last));
  assert(queue_work(system_wq, &other) && !queue_work(system_wq, &first));
  flush_workqueue(ordered_queue);
  assert(ordered_used == 4);
  for (unsigned i = 0; i < 4; i++) assert(ordered_sequence[i] == i + 1);
  DECLARE_WORK(flushed, flushed_callback);
  assert(!flush_work(&flushed));
  assert(queue_work(ordered_queue, &flushed) && flush_work(&flushed) && flush_calls == 1);
  assert(queue_work(ordered_queue, &flushed) && cancel_work_sync(&flushed));
  assert(!flush_work(&flushed) && flush_calls == 1);
  DECLARE_DELAYED_WORK(later, flushed_callback);
  assert(queue_delayed_work(ordered_queue, &later, 1000));
  flush_workqueue(ordered_queue); assert(flush_calls == 1 && delayed_work_pending(&later));
  assert(flush_delayed_work(&later) && flush_calls == 2 && !delayed_work_pending(&later));
  assert(queue_delayed_work(ordered_queue, &later, 1000) && cancel_delayed_work_sync(&later));
  struct work_struct *freed = malloc(sizeof(*freed)); assert(freed); INIT_WORK(freed, freeing_callback);
  assert(queue_work(ordered_queue, freed) && flush_work(freed) && freed_work_calls == 1);
  assert(queue_work(ordered_queue, &flushed));
  destroy_workqueue(ordered_queue); ordered_queue = NULL; assert(flush_calls == 3);
}
static unsigned bottom_calls, ordinary_at, ordinary_calls, tasklet_calls, legacy_calls, normal_timer_calls;
static void priority_bottom(struct work_struct *work) { bottom_calls++; ticks++; if (bottom_calls < 20) queue_work(system_bh_wq, work); }
static void priority_ordinary(struct work_struct *work) { (void)work; ordinary_at = bottom_calls; ordinary_calls++; }
static void tasklet_callback(struct tasklet_struct *tasklet) { (void)tasklet; tasklet_calls++; }
static void legacy_tasklet(unsigned long data) { assert(data == 0x7850); legacy_calls++; }
static void normal_timer(struct timer_list *timer) { (void)timer; normal_timer_calls++; }
static void bottom_half_contracts(void) {
  DECLARE_WORK(ordinary, priority_ordinary); DECLARE_WORK(bottom, priority_bottom);
  queue_work(system_wq, &ordinary); queue_work(system_bh_wq, &bottom);
  for (unsigned i = 0; i < 20 && (bottom_calls < 20 || !ordinary_calls); i++) lx_pump();
  assert(bottom_calls == 20 && ordinary_calls == 1 && ordinary_at > 0 && ordinary_at <= 8);
  struct tasklet_struct tasklet, legacy;
  memset(&tasklet, 0xa5, sizeof(tasklet)); tasklet_setup(&tasklet, tasklet_callback);
  tasklet_schedule(&tasklet); tasklet_schedule(&tasklet); lx_pump(); assert(tasklet_calls == 1);
  tasklet_disable(&tasklet); tasklet_disable(&tasklet); tasklet_schedule(&tasklet); lx_pump(); assert(tasklet_calls == 1);
  tasklet_enable(&tasklet); lx_pump(); assert(tasklet_calls == 1);
  tasklet_enable(&tasklet); lx_pump(); assert(tasklet_calls == 2);
  tasklet_schedule(&tasklet); tasklet_disable(&tasklet); tasklet_kill(&tasklet); tasklet_enable(&tasklet); lx_pump(); assert(tasklet_calls == 2);
  memset(&legacy, 0xa5, sizeof(legacy)); tasklet_init(&legacy, legacy_tasklet, 0x7850);
  tasklet_schedule(&legacy); lx_pump(); assert(legacy_calls == 1); tasklet_kill(&legacy);
  struct timer_list timer; timer_setup(&timer, normal_timer, 0);
  assert(!mod_timer(&timer, jiffies + 5) && mod_timer(&timer, jiffies + 2));
  ticks += 2; lx_pump(); assert(normal_timer_calls == 1 && !timer_pending(&timer));
  assert(!mod_timer(&timer, jiffies + 3) && timer_delete_sync(&timer));
  ticks += 3; lx_pump(); assert(normal_timer_calls == 1);
}
static unsigned delay_callbacks;
static void delay_callback(struct work_struct *work) { (void)work; delay_callbacks++; }
static void atomic_delay_contracts(void) {
  DECLARE_WORK(work, delay_callback); schedule_work(&work);
  u64 start = now_ns(); udelay(7); assert(now_ns() - start == 7000 && !delay_callbacks);
  start = now_ns(); mdelay(2); assert(now_ns() - start == 2000000 && !delay_callbacks);
  usleep_range(2000, 2500); assert(delay_callbacks == 1);
}
static unsigned faux_probes, faux_removes;
static bool faux_fail;
static int faux_probe(struct faux_device *device) { assert(device->dev.registered); faux_probes++; return faux_fail ? -EIO : 0; }
static void faux_remove(struct faux_device *device) { assert(device->dev.registered); faux_removes++; }
static void native_device_contracts(void) {
  static const struct class class = { .name = "test-radio", .dev_release = release_device };
  struct device *device = calloc(1, sizeof(*device)); assert(device); device_initialize(device); device->class = &class;
  assert(device_add(device) == -EINVAL && !class_register(&class) && class_register(&class) == -EEXIST);
  assert(!device_add(device)); get_device(device); device_unregister(device); class_unregister(&class);
  unsigned before = device_releases; put_device(device); assert(device_releases == before + 1);
  struct device *parent = calloc(1, sizeof(*parent)); assert(parent); device_initialize(parent); parent->release = release_device;
  static const struct faux_device_ops ops = { .probe = faux_probe, .remove = faux_remove };
  struct faux_device *faux = faux_device_create("regulatory", parent, &ops);
  assert(!IS_ERR(faux) && faux_probes == 1 && parent->references == 2);
  get_device(&faux->dev); faux_device_destroy(faux);
  assert(faux_removes == 1 && faux->dev.references == 1 && parent->references == 1); put_device(&faux->dev);
  faux_fail = true; faux = faux_device_create("regulatory-failure", parent, &ops);
  assert(IS_ERR(faux) && PTR_ERR(faux) == -EIO && parent->references == 1 && faux_probes == 2 && faux_removes == 1);
  put_device(parent);
}
static struct mutex control_mutex;
static struct completion io_completion;
static unsigned locked_control_calls, io_calls, locked_flush_calls;
static void locked_control(struct work_struct *work) {
  (void)work; mutex_lock(&control_mutex); locked_control_calls++; mutex_unlock(&control_mutex);
}
static void completion_work(struct work_struct *work) { (void)work; io_calls++; complete(&io_completion); }
static void locked_flush(struct work_struct *work) { (void)work; assert(mutex_is_locked(&control_mutex)); locked_flush_calls++; }
static void completion_timer(struct timer_list *timer) { (void)timer; complete(&io_completion); }
static void mutex_execution_contracts(void) {
  mutex_init(&control_mutex); assert(mutex_trylock(&control_mutex) && !mutex_trylock(&control_mutex));
  DECLARE_WORK(control, locked_control); DECLARE_WORK(io, completion_work); DECLARE_WORK(flushed, locked_flush);
  struct workqueue_struct *queue = alloc_ordered_workqueue("completion-test", LX_WQ_IO_COMPLETION); assert(queue);
  init_completion(&io_completion); queue_work(system_wq, &control); queue_work(queue, &io);
  assert(wait_for_completion_timeout(&io_completion, 5) && io_calls == 1 && !locked_control_calls);
  assert(lx_next_wait_ms(1000) == 1000);
  queue_work(system_wq, &flushed); assert(flush_work(&flushed) && locked_flush_calls == 1 && !locked_control_calls);
  struct timer_list timer; timer_setup(&timer, completion_timer, 0); reinit_completion(&io_completion);
  mod_timer(&timer, (unsigned long)lx_now_ms() + 2);
  assert(wait_for_completion_timeout(&io_completion, 10) && !locked_control_calls);
  mutex_unlock(&control_mutex); lx_pump(); assert(locked_control_calls == 1);
  lx_rtnl_lock(); assert(lx_rtnl_is_locked());
  queue_work(system_wq, &control); lx_pump(); assert(locked_control_calls == 1);
  lx_rtnl_unlock(); lx_pump(); assert(!lx_rtnl_is_locked() && locked_control_calls == 2);
  destroy_workqueue(queue);
}
int main(void) {
  lx_plat = &platform; lx_root_init();
  device_lifetime();
  assert(!lx_request_irq(1000, irq, NULL, 0, "ce0", (void *)0));
  assert(!lx_request_irq(1001, irq, NULL, 0, "ce1", (void *)1));
  lx_irq_polling(0);
  assert(!lx_pump() && !irq_calls[0] && !irq_calls[1]);
  assert(lx_next_wait_ms(1000) == 1000);
  lx_irq_raise(1001); assert(lx_next_wait_ms(1000) == 0);
  assert(lx_pump() && !irq_calls[0] && irq_calls[1] == 1);
  lx_irq_polling(1); ticks++; lx_pump();
  assert(irq_calls[0] == 1 && irq_calls[1] == 2 && lx_next_wait_ms(1000) == 1);
  lx_irq_polling(0);

  INIT_WORK(&work, busy_work); INIT_DELAYED_WORK(&timer, timer_work);
  lx_queue_delayed(&timer, 3); assert(lx_next_wait_ms(1000) == 3);
  lx_queue_work(&work); assert(lx_next_wait_ms(1000) == 0);
  u64 start = ticks; lx_pump(); assert(work_calls == 2 && ticks - start == 2);
  assert(lx_next_wait_ms(1000) == 0);
  while (lx_next_wait_ms(1000) == 0) lx_pump();
  assert(work_calls == 9 && timer_calls == 1 && timer_at - start <= 4);
  assert(lx_next_wait_ms(1000) == 1000);

  cancelled = calloc(1, sizeof *cancelled); assert(cancelled);
  INIT_DELAYED_WORK(&canceller, cancel_other); INIT_DELAYED_WORK(cancelled, cancelled_work);
  lx_queue_delayed(&canceller, 1); lx_queue_delayed(cancelled, 1);
  ticks++; lx_pump(); assert(!cancelled && !cancelled_calls);
  struct work_struct nested; INIT_WORK(&nested, nested_work); lx_queue_work(&nested);
  lx_pump(); assert(nested_calls == 1 && lx_next_wait_ms(1000) == 1000);
  struct rcu_head *head = calloc(1, sizeof *head); assert(head);
  rcu_read_lock(); lx_rcu_call(head, deferred); lx_pump(); assert(!deferred_calls && lx_rcu_readers() == 1);
  rcu_read_unlock(); assert(deferred_calls == 1 && !lx_rcu_readers());
  head = lx_alloc(sizeof *head, 1); assert(head); lx_rcu_free(head, head); lx_rcu_barrier();
  struct work_struct load; INIT_WORK(&load, never_idle); lx_queue_work(&load);
  start = ticks; assert(wait_event_timeout(0, false, 5) == 0 && ticks - start <= 6);
  lx_cancel_work(&load);
  unsigned evaluations = 0;
  assert(wait_event_timeout_once(0, ++evaluations == 3, 20) > 0 && evaluations == 3);
  struct completion completion; init_completion(&completion); complete(&completion); complete(&completion);
  assert(wait_for_completion_timeout(&completion, 0) == 1);
  assert(wait_for_completion_timeout(&completion, 0) == 1);
  assert(wait_for_completion_timeout(&completion, 0) == 0);
  complete_all(&completion);
  for (unsigned i = 0; i < 3; i++) assert(wait_for_completion_timeout(&completion, 0) == 1);
  reinit_completion(&completion); assert(!completion_done(&completion));
  asynchronous_lifetimes();
  precise_timers(); radio_lifetime(); workqueue_contracts(); bottom_half_contracts(); atomic_delay_contracts(); native_device_contracts(); mutex_execution_contracts();
  assert(runtime_allocations == runtime_frees);
  puts("PASS: targeted MSI, deadline waits, ordered queues, flush/cancel lifetime, self-freeing work, nanosecond/boottime timers, async firmware, device references and rfkill");
  return 0;
}
