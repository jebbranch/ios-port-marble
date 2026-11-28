/* User-side transport for the supplicant backend. No authentication algorithms. */
#ifndef NW_CONTROL_CLIENT_H
#define NW_CONTROL_CLIENT_H
#include "nw_control.h"
struct nw_control_client;
struct nw_control_packet;
struct nw_control_client *nw_control_client_open(void);
/* Takes ownership of a connected datagram fd; also used by host integration. */
struct nw_control_client *nw_control_client_from_fd(int fd, uint64_t session);
void nw_control_client_close(struct nw_control_client *client);
int nw_control_client_fd(const struct nw_control_client *client);
int nw_control_client_failed(const struct nw_control_client *client);
uint64_t nw_control_client_session(const struct nw_control_client *client);
void nw_control_client_generation(struct nw_control_client *client, uint64_t generation);
int nw_control_client_begin(struct nw_control_client *client, struct nw_control_builder *builder,
                            void *buffer, size_t capacity, unsigned operation);
/* Consumes and wipes the request buffer. Returns a common nonnegative status
 * for a reply, or -1/errno for transport failure. Reply spans last until the
 * next client read. Events received during an RPC keep their own storage. */
int nw_control_client_exchange(struct nw_control_client *client, struct nw_control_builder *request,
                               struct nw_control_view *reply, unsigned timeout_ms);
int nw_control_client_has_events(const struct nw_control_client *client);
/* Returns 1 with an owning packet, 0 when no event is ready, -1 on failure. */
int nw_control_client_event(struct nw_control_client *client, struct nw_control_packet **packet, struct nw_control_view *view);
void nw_control_packet_free(struct nw_control_packet *packet);
#endif
