/* Bounded socket-to-worker handoff. Linux control work never runs on a socket
 * callback thread; payloads are wiped when their queue owner releases them. */
#include "lx_plat.h"
#include "nw_control_endpoint.h"
enum item_kind { ITEM_OPEN, ITEM_CLOSE, ITEM_REQUEST, ITEM_SEND };
struct control_client;
struct control_item {
  struct control_item *next; struct control_client *owner;
  uint64_t session; size_t length, capacity; enum item_kind kind;
};
struct control_client { uint64_t session; bool activated; struct control_item open, close; };
static spinlock_t control_lock;
static struct control_item *rx_head, *rx_tail, *tx_head, *tx_tail;
static struct control_client *client;
static struct nw_control_transport transport;
static struct nw_control_dispatch dispatch;
static struct nw_control_stats stats;
static struct work_struct control_work;
static uint64_t next_session, writable_epoch;
static bool bound, tx_ready = true;
#define RX_LIMIT (256u * 1024u)
#define TX_LIMIT (512u * 1024u)
#define ITEM_LIMIT 128u
static void wake_worker(void) { if (transport.wake) transport.wake(transport.context); }
static uint8_t *item_bytes(struct control_item *item) { return (uint8_t *)(item + 1); }
static void push(struct control_item **head, struct control_item **tail, struct control_item *item) {
  item->next = NULL; if (*tail) (*tail)->next = item; else *head = item; *tail = item;
}
static struct control_item *pop(struct control_item **head, struct control_item **tail) {
  struct control_item *item = *head; if (!item) return NULL;
  *head = item->next; if (!*head) *tail = NULL; item->next = NULL; return item;
}
static void release_message(struct control_item *item) {
  nw_control_clear(item_bytes(item), item->capacity); lx_free(item);
}
static struct control_item *new_message(enum item_kind kind, uint64_t session, size_t capacity) {
  struct control_item *item = lx_alloc(sizeof(*item) + capacity, 0);
  if (item) *item = (struct control_item){ .kind = kind, .session = session, .capacity = capacity };
  return item;
}
static bool active(uint64_t session) {
  lx_shared_spin_lock(&control_lock); bool result = bound && client && client->session == session; lx_shared_spin_unlock(&control_lock); return result;
}
static void detach_locked(uint64_t session) {
  if (!client || client->session != session) return;
  struct control_client *closed = client; client = NULL;
  stats.disconnections++;
  push(&rx_head, &rx_tail, &closed->close); stats.rx_items++;
}
void nw_wifi_control_detach(uint64_t session) {
  lx_shared_spin_lock(&control_lock); detach_locked(session); lx_shared_spin_unlock(&control_lock); wake_worker();
}
static int enqueue_send(struct control_item *item) {
  lx_shared_spin_lock(&control_lock);
  if (!bound || !client || client->session != item->session) {
    lx_shared_spin_unlock(&control_lock); release_message(item); return NW_CONTROL_NOT_READY;
  }
  if (stats.tx_items >= ITEM_LIMIT || item->capacity > TX_LIMIT - stats.tx_bytes) {
    stats.tx_full++; detach_locked(item->session); lx_shared_spin_unlock(&control_lock);
    release_message(item); wake_worker(); return NW_CONTROL_NO_MEMORY;
  }
  push(&tx_head, &tx_tail, item); stats.tx_items++; stats.tx_bytes += item->capacity;
  lx_shared_spin_unlock(&control_lock); wake_worker(); return NW_CONTROL_OK;
}
static void flush_send(void) {
  for (unsigned budget = 0; budget < 64; budget++) {
    lx_shared_spin_lock(&control_lock);
    struct control_item *item = tx_head; uint64_t epoch = writable_epoch;
    bool send = item && bound && client && client->session == item->session;
    if (!item || (send && !tx_ready)) { lx_shared_spin_unlock(&control_lock); return; }
    lx_shared_spin_unlock(&control_lock);
    int result = send ? transport.send(transport.context, item->session, item_bytes(item), item->length) : NW_CONTROL_SEND_CLOSED;
    lx_shared_spin_lock(&control_lock);
    if (result == NW_CONTROL_SEND_AGAIN) {
      stats.send_retries++; if (epoch == writable_epoch) tx_ready = false;
      lx_shared_spin_unlock(&control_lock); return;
    }
    BUG_ON(pop(&tx_head, &tx_tail) != item); stats.tx_items--; stats.tx_bytes -= item->capacity;
    if (send && result != NW_CONTROL_SEND_OK) detach_locked(item->session);
    lx_shared_spin_unlock(&control_lock); release_message(item);
  }
}
static void run_request(struct control_item *item) {
  if (!active(item->session)) return;
  struct nw_control_view view;
  BUG_ON(nw_control_decode(item_bytes(item), item->length, &view));
  size_t capacity = view.header.operation == NW_CONTROL_INFO || view.header.operation == NW_CONTROL_GET_BSS ||
                    view.header.operation == NW_CONTROL_GET_CHANNELS ? 8192 : 512;
  struct control_item *reply = new_message(ITEM_SEND, item->session, capacity);
  if (!reply) { nw_wifi_control_detach(item->session); return; }
  struct nw_control_builder builder;
  BUG_ON(nw_control_begin(&builder, item_bytes(reply), capacity, NW_CONTROL_REPLY, view.header.operation,
                          view.header.sequence, item->session, view.header.generation, NW_CONTROL_OK));
  lx_shared_spin_lock(&control_lock); stats.last_operation = view.header.operation; stats.last_status = NW_CONTROL_BUSY; lx_shared_spin_unlock(&control_lock);
  int status = dispatch.request(dispatch.context, item->session, &view, &builder);
  if (status < 0 || status >= NW_CONTROL_STATUS_MAX) status = NW_CONTROL_DRIVER_FAILED;
  lx_shared_spin_lock(&control_lock); stats.last_status = status; lx_shared_spin_unlock(&control_lock);
  struct nw_control_view output;
  BUG_ON(nw_control_decode(builder.bytes, builder.length, &output));
  if (status) {
    BUG_ON(nw_control_begin(&builder, item_bytes(reply), capacity, NW_CONTROL_REPLY, view.header.operation,
                            view.header.sequence, item->session, output.header.generation, status));
  }
  reply->length = builder.length;
  lx_shared_spin_lock(&control_lock); stats.dispatched++; stats.replies++; lx_shared_spin_unlock(&control_lock);
  enqueue_send(reply);
}
static void control_run(struct work_struct *work) {
  (void)work; uint64_t start = lx_now_ns();
  for (unsigned budget = 0; budget < 16; budget++) {
    lx_shared_spin_lock(&control_lock); struct control_item *item = pop(&rx_head, &rx_tail);
    if (item) { stats.rx_items--; stats.rx_bytes -= item->capacity; }
    lx_shared_spin_unlock(&control_lock); if (!item) break;
    if (item->kind == ITEM_OPEN) {
      if (active(item->session) && dispatch.opened) dispatch.opened(dispatch.context, item->session);
    } else if (item->kind == ITEM_CLOSE) {
      if (dispatch.closed) dispatch.closed(dispatch.context, item->session);
      lx_free(item->owner);
    } else { run_request(item); release_message(item); }
    if (lx_now_ns() - start >= 2000000ULL) break;
  }
  flush_send(); nw_wifi_control_poll();
}
int nw_wifi_control_transport_set(const struct nw_control_transport *ops) {
  if (!ops || !ops->send || !ops->wake) return NW_CONTROL_INVALID;
  lx_shared_spin_lock(&control_lock);
  if (bound || client) { lx_shared_spin_unlock(&control_lock); return NW_CONTROL_BUSY; }
  transport = *ops; lx_shared_spin_unlock(&control_lock); return NW_CONTROL_OK;
}
int nw_wifi_control_bind(const struct nw_control_dispatch *ops) {
  if (!ops || !ops->request) return NW_CONTROL_INVALID;
  lx_shared_spin_lock(&control_lock);
  if (bound) { lx_shared_spin_unlock(&control_lock); return NW_CONTROL_BUSY; }
  if (!transport.send) { lx_shared_spin_unlock(&control_lock); return NW_CONTROL_NOT_READY; }
  BUG_ON(rx_head || tx_head); dispatch = *ops; tx_ready = true; INIT_WORK(&control_work, control_run); bound = true;
  lx_shared_spin_unlock(&control_lock); return NW_CONTROL_OK;
}
int nw_wifi_control_attach(uint64_t *session) {
  if (!session) return NW_CONTROL_INVALID;
  lx_shared_spin_lock(&control_lock); bool ready = bound; lx_shared_spin_unlock(&control_lock);
  if (!ready) return NW_CONTROL_NOT_READY;
  struct control_client *created = lx_alloc(sizeof(*created), 1); if (!created) return NW_CONTROL_NO_MEMORY;
  lx_shared_spin_lock(&control_lock);
  if (!bound || client || stats.rx_items >= ITEM_LIMIT - 1) {
    int result = bound ? NW_CONTROL_BUSY : NW_CONTROL_NOT_READY; lx_shared_spin_unlock(&control_lock); lx_free(created); return result;
  }
  if (!++next_session) next_session++;
  created->session = next_session;
  created->open = (struct control_item){ .kind = ITEM_OPEN, .session = next_session, .owner = created };
  created->close = (struct control_item){ .kind = ITEM_CLOSE, .session = next_session, .owner = created };
  client = created; stats.connections++; tx_ready = true; *session = next_session; lx_shared_spin_unlock(&control_lock); return NW_CONTROL_OK;
}
int nw_wifi_control_receive(uint64_t session, size_t length, int (*reader)(void *, void *, size_t), void *context) {
  if (!reader || length < NW_CONTROL_HEADER_SIZE || length > NW_CONTROL_MAX_MESSAGE) return NW_CONTROL_INVALID;
  if (!active(session)) return NW_CONTROL_NOT_READY;
  struct control_item *item = new_message(ITEM_REQUEST, session, length); if (!item) return NW_CONTROL_NO_MEMORY;
  item->length = length; struct nw_control_view view;
  if (reader(context, item_bytes(item), length) || nw_control_decode(item_bytes(item), length, &view) || view.header.kind != NW_CONTROL_REQUEST) {
    lx_shared_spin_lock(&control_lock); stats.malformed++; lx_shared_spin_unlock(&control_lock); release_message(item); return NW_CONTROL_INVALID;
  }
  if (view.header.session != session) {
    lx_shared_spin_lock(&control_lock); stats.stale_sessions++; lx_shared_spin_unlock(&control_lock); release_message(item); return NW_CONTROL_STALE;
  }
  lx_shared_spin_lock(&control_lock);
  if (!bound || !client || client->session != session) { lx_shared_spin_unlock(&control_lock); release_message(item); return NW_CONTROL_NOT_READY; }
  unsigned extra = client->activated ? 1 : 2;
  if (stats.rx_items + extra >= ITEM_LIMIT || length > RX_LIMIT - stats.rx_bytes) {
    stats.rx_full++; lx_shared_spin_unlock(&control_lock); release_message(item); return NW_CONTROL_BUSY;
  }
  if (!client->activated) { client->activated = true; push(&rx_head, &rx_tail, &client->open); stats.rx_items++; }
  push(&rx_head, &rx_tail, item); stats.rx_items++; stats.rx_bytes += length; stats.accepted++;
  lx_shared_spin_unlock(&control_lock); wake_worker(); return NW_CONTROL_OK;
}
void nw_wifi_control_writable(uint64_t session) {
  lx_shared_spin_lock(&control_lock);
  if (client && client->session == session) { writable_epoch++; tx_ready = true; }
  lx_shared_spin_unlock(&control_lock); wake_worker();
}
int nw_wifi_control_publish(uint64_t session, const void *bytes, size_t length) {
  struct nw_control_view view;
  if (nw_control_decode(bytes, length, &view) || view.header.kind != NW_CONTROL_EVENT || view.header.session != session) return NW_CONTROL_INVALID;
  struct control_item *item = new_message(ITEM_SEND, session, length); if (!item) return NW_CONTROL_NO_MEMORY;
  memcpy(item_bytes(item), bytes, length); item->length = length;
  lx_shared_spin_lock(&control_lock); stats.events++; lx_shared_spin_unlock(&control_lock); return enqueue_send(item);
}
int nw_wifi_control_pending(void) {
  lx_shared_spin_lock(&control_lock); int pending = rx_head != NULL || (tx_head && (tx_ready || !client || tx_head->session != client->session));
  lx_shared_spin_unlock(&control_lock); return pending;
}
void nw_wifi_control_poll(void) { if (nw_wifi_control_pending()) queue_work(system_wq, &control_work); }
void nw_wifi_control_stats_get(struct nw_control_stats *out) {
  lx_shared_spin_lock(&control_lock); *out = stats; out->active_session = client ? client->session : 0; lx_shared_spin_unlock(&control_lock);
}
void nw_wifi_control_shutdown(void) {
  lx_shared_spin_lock(&control_lock); bound = false; if (client) detach_locked(client->session); lx_shared_spin_unlock(&control_lock);
  cancel_work_sync(&control_work);
  while (nw_wifi_control_pending()) { control_run(&control_work); cancel_work_sync(&control_work); }
  memset(&dispatch, 0, sizeof(dispatch));
}
