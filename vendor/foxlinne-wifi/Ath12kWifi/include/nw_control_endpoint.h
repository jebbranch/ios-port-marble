/* Native socket boundary. Callbacks and pointers here never cross into userspace. */
#ifndef NW_CONTROL_ENDPOINT_H
#define NW_CONTROL_ENDPOINT_H
#include "nw_control.h"
#ifdef __cplusplus
extern "C" {
#endif
enum nw_control_send_result { NW_CONTROL_SEND_OK, NW_CONTROL_SEND_AGAIN, NW_CONTROL_SEND_CLOSED };
struct nw_control_transport {
  void *context;
  int (*send)(void *context, uint64_t session, const void *bytes, size_t length);
  void (*wake)(void *context);
};
struct nw_control_dispatch {
  void *context;
  void (*opened)(void *context, uint64_t session);
  void (*closed)(void *context, uint64_t session);
  int (*request)(void *context, uint64_t session, const struct nw_control_view *request, struct nw_control_builder *reply);
};
struct nw_control_stats {
  uint64_t accepted, dispatched, replies, events, malformed, stale_sessions, rx_full, tx_full, send_retries;
  size_t rx_bytes, tx_bytes; unsigned rx_items, tx_items;
  uint64_t connections, disconnections, active_session;
  unsigned last_operation, last_status;
};
/* Transport installation/binding and shutdown run on the wireless worker. */
int nw_wifi_control_transport_set(const struct nw_control_transport *transport);
int nw_wifi_control_bind(const struct nw_control_dispatch *dispatch);
void nw_wifi_control_shutdown(void);
/* Native socket callbacks may run on any thread. receive copies once from its
 * owning mbuf through reader; activation waits for the first valid request. */
int nw_wifi_control_attach(uint64_t *session);
void nw_wifi_control_detach(uint64_t session);
int nw_wifi_control_receive(uint64_t session, size_t length, int (*reader)(void *, void *, size_t), void *context);
void nw_wifi_control_writable(uint64_t session);
/* Worker-side event publication copies a complete encoded message. */
int nw_wifi_control_publish(uint64_t session, const void *bytes, size_t length);
void nw_wifi_control_poll(void);
int nw_wifi_control_pending(void);
void nw_wifi_control_stats_get(struct nw_control_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
