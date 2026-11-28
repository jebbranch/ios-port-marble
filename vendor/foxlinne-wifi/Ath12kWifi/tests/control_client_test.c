#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "lx_plat.h"
#include "nw_control_endpoint.h"
#include "nw_control_client.h"
static int server_fd, client_fd;
static uint64_t session, ticks;
static unsigned allocations, releases, dispatched, closed;
static bool client_done, injected;
static pthread_t worker;
static void *heap_alloc(size_t n) { void *p = malloc(n); if (p) allocations++; return p; }
static void heap_free(void *p) { releases++; free(p); }
static uint64_t now(void) { return ticks; }
static void sleep_ms(unsigned ms) { ticks += ms; }
static void nothing(void) {}
static void log_line(int level, const char *line) { (void)level; (void)line; }
static void halt(void) { abort(); }
static const struct lx_platform platform = { .now_ms = now, .sleep_ms = sleep_ms, .poll = nothing,
  .alloc = heap_alloc, .free = heap_free, .log = log_line, .halt = halt };
static void wake(void *context) { (void)context; }
static int copy_request(void *source, void *destination, size_t length) { memcpy(destination, source, length); return 0; }
static int transport_send(void *context, uint64_t id, const void *bytes, size_t length) {
  (void)context; assert(pthread_equal(pthread_self(), worker) && id == session);
  if (!injected) {
    uint8_t stale[128]; struct nw_control_builder builder;
    assert(!nw_control_begin(&builder, stale, sizeof(stale), NW_CONTROL_EVENT, NW_CONTROL_RX_EAPOL, 0, session + 1, 7, 0));
    assert(send(server_fd, stale, builder.length, 0) == (ssize_t)builder.length); injected = true;
  }
  ssize_t result = send(server_fd, bytes, length, 0);
  if (result == (ssize_t)length) return NW_CONTROL_SEND_OK;
  return errno == EAGAIN || errno == EWOULDBLOCK ? NW_CONTROL_SEND_AGAIN : NW_CONTROL_SEND_CLOSED;
}
static int dispatch(void *context, uint64_t id, const struct nw_control_view *request, struct nw_control_builder *reply) {
  (void)context; assert(pthread_equal(pthread_self(), worker) && id == session); dispatched++;
  if (request->header.operation == NW_CONTROL_SET_KEY) {
    size_t length; const uint8_t *key = nw_control_get(request, NW_CONTROL_ATTR_KEY, &length); assert(key && length == 32);
    for (unsigned i = 0; i < length; i++) assert(key[i] == 0xd7);
    uint8_t bytes[512], eapol[128]; for (unsigned i = 0; i < sizeof(eapol); i++) eapol[i] = (uint8_t)(i ^ 0x4d);
    struct nw_control_builder event;
    assert(!nw_control_begin(&event, bytes, sizeof(bytes), NW_CONTROL_EVENT, NW_CONTROL_RX_EAPOL, 0, id, 7, 0));
    assert(!nw_control_put(&event, NW_CONTROL_ATTR_EAPOL, eapol, sizeof(eapol)) && !nw_wifi_control_publish(id, event.bytes, event.length));
    return NW_CONTROL_OK;
  }
  if (request->header.operation == NW_CONTROL_AUTHENTICATE) { nw_control_reply_state(reply, 8, 0); return NW_CONTROL_STALE; }
  return nw_control_put_u32(reply, NW_CONTROL_ATTR_FEATURES, 0x11223344) ? NW_CONTROL_DRIVER_FAILED : NW_CONTROL_OK;
}
static void owner_closed(void *context, uint64_t id) { (void)context; assert(id == session); closed++; }
static void check_cleared(const uint8_t *bytes, size_t length) { for (size_t i = 0; i < length; i++) assert(!bytes[i]); }
static void *client_thread(void *context) {
  (void)context; struct nw_control_client *client = nw_control_client_from_fd(client_fd, session); assert(client);
  nw_control_client_generation(client, 7);
  uint8_t buffer[4096], key[32]; memset(key, 0xd7, sizeof(key));
  struct nw_control_builder request; struct nw_control_view reply;
  assert(!nw_control_client_begin(client, &request, buffer, sizeof(buffer), NW_CONTROL_SET_KEY));
  assert(!nw_control_put(&request, NW_CONTROL_ATTR_KEY, key, sizeof(key)));
  assert(nw_control_client_exchange(client, &request, &reply, 2000) == NW_CONTROL_OK);
  check_cleared(buffer, sizeof(buffer)); assert(nw_control_client_has_events(client));
  struct nw_control_packet *event; struct nw_control_view view;
  assert(nw_control_client_event(client, &event, &view) == 1 && view.header.session == session);
  size_t length; const uint8_t *eapol = nw_control_get(&view, NW_CONTROL_ATTR_EAPOL, &length); assert(eapol && length == 128);
  assert(!nw_control_client_begin(client, &request, buffer, sizeof(buffer), NW_CONTROL_INFO));
  assert(!nw_control_client_exchange(client, &request, &reply, 2000));
  uint32_t features; assert(!nw_control_get_u32(&reply, NW_CONTROL_ATTR_FEATURES, &features) && features == 0x11223344);
  for (unsigned i = 0; i < length; i++) assert(eapol[i] == (uint8_t)(i ^ 0x4d));
  assert(!nw_control_client_begin(client, &request, buffer, sizeof(buffer), NW_CONTROL_AUTHENTICATE));
  assert(nw_control_client_exchange(client, &request, &reply, 2000) == NW_CONTROL_STALE && reply.header.generation == 8);
  assert(!nw_control_client_failed(client));
  nw_control_client_close(client);
  for (unsigned i = 0; i < length; i++) assert(eapol[i] == (uint8_t)(i ^ 0x4d));
  nw_control_packet_free(event); __atomic_store_n(&client_done, true, __ATOMIC_RELEASE); return NULL;
}
int main(void) {
  lx_plat = &platform; lx_irq_polling(0); worker = pthread_self();
  int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets)); server_fd = sockets[0]; client_fd = sockets[1];
  int flags = fcntl(server_fd, F_GETFL); assert(flags >= 0 && !fcntl(server_fd, F_SETFL, flags | O_NONBLOCK));
  struct nw_control_transport transport = { .send = transport_send, .wake = wake };
  struct nw_control_dispatch handlers = { .request = dispatch, .closed = owner_closed };
  assert(!nw_wifi_control_transport_set(&transport) && !nw_wifi_control_bind(&handlers) && !nw_wifi_control_attach(&session));
  pthread_t thread; assert(!pthread_create(&thread, NULL, client_thread, NULL));
  static uint8_t bytes[NW_CONTROL_MAX_MESSAGE];
  while (!__atomic_load_n(&client_done, __ATOMIC_ACQUIRE) || nw_wifi_control_pending()) {
    ssize_t length = recv(server_fd, bytes, sizeof(bytes), 0);
    if (length > 0) assert(!nw_wifi_control_receive(session, (size_t)length, copy_request, bytes));
    nw_wifi_control_poll(); lx_pump(); sched_yield();
  }
  assert(!pthread_join(thread, NULL)); nw_wifi_control_detach(session);
  while (nw_wifi_control_pending()) { nw_wifi_control_poll(); lx_pump(); }
  nw_wifi_control_shutdown(); close(server_fd);
  assert(dispatched == 3 && closed == 1 && allocations == releases);
  assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets));
  struct nw_control_client *client = nw_control_client_from_fd(sockets[0], 17); assert(client);
  struct nw_control_builder request; struct nw_control_view reply; uint8_t encoded[128];
  assert(!nw_control_client_begin(client, &request, encoded, sizeof(encoded), NW_CONTROL_INFO));
  assert(nw_control_client_exchange(client, &request, &reply, 20) == -1 && errno == ETIMEDOUT && nw_control_client_failed(client));
  check_cleared(encoded, sizeof(encoded)); nw_control_client_close(client); close(sockets[1]);
  puts("PASS: userspace RPC through production worker handoff, interleaved EAPOL ownership across nested requests/close, stale sessions, reply status, key-buffer wiping and timeout cleanup");
}
