#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_plat.h"
#include "nw_control_endpoint.h"
struct allocation { void *pointer; size_t length; };
static struct allocation ledger[1024];
static pthread_mutex_t ledger_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned allocated, freed, wakes;
static u64 ticks;
static void *heap_alloc(size_t length) {
  void *p = malloc(length); if (!p) return NULL;
  pthread_mutex_lock(&ledger_lock);
  unsigned i; for (i = 0; i < ARRAY_SIZE(ledger) && ledger[i].pointer; i++) { }
  assert(i < ARRAY_SIZE(ledger)); ledger[i] = (struct allocation){ p, length }; allocated++;
  pthread_mutex_unlock(&ledger_lock); return p;
}
static void heap_free(void *p) {
  pthread_mutex_lock(&ledger_lock);
  unsigned i; for (i = 0; i < ARRAY_SIZE(ledger) && ledger[i].pointer != p; i++) { }
  assert(i < ARRAY_SIZE(ledger)); unsigned repeated = 0;
  for (size_t j = 0; j < ledger[i].length; j++) {
    repeated = ((u8 *)p)[j] == 0xd7 ? repeated + 1 : 0;
    assert(repeated < 32); /* No released queue owner retains the synthetic key. */
  }
  ledger[i].pointer = NULL; freed++; pthread_mutex_unlock(&ledger_lock); free(p);
}
static u64 now(void) { return ticks; }
static void sleep_ms(unsigned ms) { ticks += ms; }
static void nothing(void) {}
static void halt(void) { abort(); }
static void log_line(int level, const char *line) { (void)level; (void)line; }
static const struct lx_platform platform = { .now_ms = now, .sleep_ms = sleep_ms, .poll = nothing,
  .alloc = heap_alloc, .free = heap_free, .halt = halt, .log = log_line };
static uint64_t session;
static unsigned opened, closed, dispatched, sent, send_attempts;
static bool blocked, wake_in_send, in_dispatch;
static _Thread_local bool socket_callback;
static uint8_t last_reply[8192]; static size_t last_length;
static int send_message(void *context, uint64_t id, const void *bytes, size_t length) {
  (void)context; assert(id == session && length <= sizeof(last_reply)); send_attempts++;
  if (wake_in_send) { wake_in_send = false; nw_wifi_control_writable(id); return NW_CONTROL_SEND_AGAIN; }
  if (blocked) return NW_CONTROL_SEND_AGAIN;
  struct nw_control_view view; assert(!nw_control_decode(bytes, length, &view) && view.header.session == id);
  memcpy(last_reply, bytes, length); last_length = length; sent++; return NW_CONTROL_SEND_OK;
}
static void wake(void *context) { (void)context; __atomic_fetch_add(&wakes, 1u, __ATOMIC_RELAXED); }
static void owner_opened(void *context, uint64_t id) { (void)context; assert(!socket_callback && id == session); opened++; }
static void owner_closed(void *context, uint64_t id) { (void)context; assert(!socket_callback && id); closed++; }
static int request(void *context, uint64_t id, const struct nw_control_view *view, struct nw_control_builder *reply) {
  (void)context; assert(!socket_callback && !in_dispatch && id == session); in_dispatch = true; dispatched++;
  lx_delay_ms(1); /* A wait must not reenter the control dispatcher. */
  nw_control_reply_state(reply, 7, NW_CONTROL_OK);
  int status = NW_CONTROL_OK;
  if (view->header.operation == NW_CONTROL_SET_KEY) {
    size_t length; const u8 *key = nw_control_get(view, NW_CONTROL_ATTR_KEY, &length); assert(key && length == 32);
    for (unsigned i = 0; i < length; i++) assert(key[i] == 0xd7);
    if (view->header.generation != 7) {
      assert(!nw_control_put(reply, NW_CONTROL_ATTR_KEY, key, length)); status = NW_CONTROL_STALE;
    }
  } else assert(!nw_control_put_u32(reply, NW_CONTROL_ATTR_FEATURES, 0x11223344));
  in_dispatch = false; return status;
}
static int copy_message(void *source, void *destination, size_t length) { memcpy(destination, source, length); return 0; }
static size_t packet(uint8_t *bytes, unsigned kind, unsigned operation, uint64_t id, uint64_t generation) {
  struct nw_control_builder builder;
  assert(!nw_control_begin(&builder, bytes, 4096, kind, operation, 1, id, generation, NW_CONTROL_OK));
  if (operation == NW_CONTROL_SET_KEY) { u8 key[32]; memset(key, 0xd7, sizeof(key)); assert(!nw_control_put(&builder, NW_CONTROL_ATTR_KEY, key, sizeof(key))); }
  return builder.length;
}
static void drain(void) { for (unsigned i = 0; i < 100 && nw_wifi_control_pending(); i++) { nw_wifi_control_poll(); lx_pump(); } assert(!nw_wifi_control_pending()); }
static bool producer_done;
static void *producer(void *context) {
  (void)context; uint8_t bytes[4096]; size_t length = packet(bytes, NW_CONTROL_REQUEST, NW_CONTROL_SET_KEY, session, 7);
  for (unsigned i = 0; i < 12; i++) {
    socket_callback = true; assert(nw_wifi_control_receive(session, length, copy_message, bytes) == NW_CONTROL_OK); socket_callback = false;
  }
  nw_control_clear(bytes, sizeof(bytes)); __atomic_store_n(&producer_done, true, __ATOMIC_RELEASE); return NULL;
}
int main(void) {
  lx_plat = &platform; lx_irq_polling(0);
  struct nw_control_transport transport = { .send = send_message, .wake = wake };
  struct nw_control_dispatch dispatch = { .opened = owner_opened, .closed = owner_closed, .request = request };
  assert(nw_wifi_control_attach(&session) == NW_CONTROL_NOT_READY);
  assert(!nw_wifi_control_transport_set(&transport) && !nw_wifi_control_bind(&dispatch));
  assert(!nw_wifi_control_attach(&session) && session); uint64_t other;
  assert(nw_wifi_control_attach(&other) == NW_CONTROL_BUSY);
  uint8_t bytes[4096]; size_t length = packet(bytes, NW_CONTROL_REQUEST, NW_CONTROL_SET_KEY, session + 1, 7);
  assert(nw_wifi_control_receive(session, length, copy_message, bytes) == NW_CONTROL_STALE && !opened);
  length = packet(bytes, NW_CONTROL_REQUEST, NW_CONTROL_SET_KEY, session, 0);
  assert(!nw_wifi_control_receive(session, length, copy_message, bytes)); nw_control_clear(bytes, sizeof(bytes));
  struct mutex mutex; mutex_init(&mutex); mutex_lock(&mutex); nw_wifi_control_poll(); lx_pump(); assert(!opened && !dispatched);
  mutex_unlock(&mutex); drain(); assert(opened == 1 && dispatched == 1 && sent == 1);
  struct nw_control_view reply; assert(!nw_control_decode(last_reply, last_length, &reply));
  assert(reply.header.status == NW_CONTROL_STALE && reply.header.generation == 7 && !reply.attributes);
  pthread_t thread; assert(!pthread_create(&thread, NULL, producer, NULL));
  while (!__atomic_load_n(&producer_done, __ATOMIC_ACQUIRE) || nw_wifi_control_pending()) { nw_wifi_control_poll(); lx_pump(); sched_yield(); }
  assert(!pthread_join(thread, NULL)); drain(); assert(dispatched == 13 && sent == 13);
  length = packet(bytes, NW_CONTROL_REQUEST, NW_CONTROL_INFO, session, 7); blocked = true;
  assert(!nw_wifi_control_receive(session, length, copy_message, bytes)); drain();
  struct nw_control_stats stats; nw_wifi_control_stats_get(&stats); assert(stats.tx_items == 1 && stats.send_retries == 1);
  unsigned before = sent; blocked = false; nw_wifi_control_writable(session); drain(); assert(sent == before + 1);
  wake_in_send = true; assert(!nw_wifi_control_receive(session, length, copy_message, bytes)); drain(); assert(sent == before + 2);
  nw_wifi_control_detach(session); drain(); assert(closed == 1);
  uint64_t previous = session; assert(!nw_wifi_control_attach(&session) && session != previous);
  assert(nw_wifi_control_receive(previous, length, copy_message, bytes) == NW_CONTROL_NOT_READY);
  length = packet(bytes, NW_CONTROL_REQUEST, NW_CONTROL_INFO, session, 7);
  struct nw_control_builder builder; assert(!nw_control_begin(&builder, bytes, sizeof(bytes), NW_CONTROL_REQUEST, NW_CONTROL_INFO, 1, session, 7, 0));
  static const uint8_t scan_ies[3000];
  assert(!nw_control_put(&builder, NW_CONTROL_ATTR_IE, scan_ies, sizeof(scan_ies))); length = builder.length;
  unsigned accepted = 0; while (nw_wifi_control_receive(session, length, copy_message, bytes) == NW_CONTROL_OK) accepted++;
  assert(accepted && accepted < 128); nw_wifi_control_detach(session); drain(); assert(closed == 2);
  assert(!nw_wifi_control_attach(&session));
  static uint8_t large[65536], payload[60000];
  assert(!nw_control_begin(&builder, large, sizeof(large), NW_CONTROL_EVENT, NW_CONTROL_RX_EAPOL, 0, session, 7, 0));
  assert(!nw_control_put(&builder, NW_CONTROL_ATTR_EAPOL, payload, sizeof(payload)));
  unsigned published = 0; while (nw_wifi_control_publish(session, large, builder.length) == NW_CONTROL_OK) published++;
  assert(published > 0 && published < 16); drain(); assert(closed == 3);
  nw_wifi_control_shutdown(); nw_wifi_control_stats_get(&stats);
  assert(!stats.rx_items && !stats.tx_items && !stats.rx_bytes && !stats.tx_bytes && stats.stale_sessions == 1 && stats.rx_full && stats.tx_full);
  assert(allocated == freed && wakes && send_attempts > sent);
  puts("PASS: real-thread ingress, exclusive worker dispatch, session isolation, reply ownership/wiping, bounded RX/TX queues, writable wake race and client cleanup");
}
