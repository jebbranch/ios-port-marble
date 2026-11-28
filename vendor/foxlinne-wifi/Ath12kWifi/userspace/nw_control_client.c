/* Native control socket client; queues events instead of reentering the SME
 * while a driver operation waits for its reply. */
#include "nw_control_client.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include "nw_darwin_control.h"
#endif
struct nw_control_packet { struct nw_control_packet *next; size_t length; uint8_t bytes[]; };
struct nw_control_client {
  int fd; bool failed, exchanging;
  uint64_t session, generation; uint32_t sequence;
  uint8_t *receive; struct nw_control_packet *head, *tail;
  size_t queued_bytes; unsigned queued_packets;
};
static uint64_t now_ms(void) {
  struct timespec time; clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000 + (unsigned long)time.tv_nsec / 1000000;
}
struct nw_control_client *nw_control_client_from_fd(int fd, uint64_t session) {
  struct nw_control_client *client = calloc(1, sizeof(*client));
  if (!client) { close(fd); return NULL; }
  client->fd = fd; client->session = session; client->receive = malloc(NW_CONTROL_MAX_MESSAGE);
  int flags = fcntl(fd, F_GETFL);
  if (!client->receive || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    int error = errno; nw_control_client_close(client); errno = error; return NULL;
  }
#ifdef SO_NOSIGPIPE
  int enabled = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  return client;
}
struct nw_control_client *nw_control_client_open(void) {
#ifdef __APPLE__
  int fd = socket(PF_SYSTEM, SOCK_DGRAM, NW_SYSPROTO_CONTROL); if (fd < 0) return NULL;
  struct nw_ctl_info info = {0};
  memcpy(info.name, NW_CONTROL_NAME, sizeof(NW_CONTROL_NAME));
  if (ioctl(fd, NW_CTLIOCGINFO, &info)) { int error = errno; close(fd); errno = error; return NULL; }
  struct nw_sockaddr_ctl address = { .length = sizeof(address), .family = AF_SYSTEM,
    .sysaddr = NW_AF_SYS_CONTROL, .id = info.id };
  if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { int error = errno; close(fd); errno = error; return NULL; }
  uint64_t session; socklen_t size = sizeof(session);
  if (getsockopt(fd, NW_SYSPROTO_CONTROL, NW_CONTROL_GET_SESSION, &session, &size)) {
    int error = errno; close(fd); errno = error; return NULL;
  }
  if (size != sizeof(session)) { close(fd); errno = EPROTO; return NULL; }
  return nw_control_client_from_fd(fd, session);
#else
  errno = EAFNOSUPPORT; return NULL;
#endif
}
void nw_control_packet_free(struct nw_control_packet *packet) {
  if (!packet) return;
  nw_control_clear(packet->bytes, packet->length); free(packet);
}
void nw_control_client_close(struct nw_control_client *client) {
  if (!client) return;
  close(client->fd);
  while (client->head) { struct nw_control_packet *next = client->head->next; nw_control_packet_free(client->head); client->head = next; }
  if (client->receive) { nw_control_clear(client->receive, NW_CONTROL_MAX_MESSAGE); free(client->receive); }
  nw_control_clear(client, sizeof(*client)); free(client);
}
int nw_control_client_fd(const struct nw_control_client *client) { return client->fd; }
int nw_control_client_failed(const struct nw_control_client *client) { return client->failed; }
uint64_t nw_control_client_session(const struct nw_control_client *client) { return client->session; }
void nw_control_client_generation(struct nw_control_client *client, uint64_t generation) { client->generation = generation; }
int nw_control_client_begin(struct nw_control_client *client, struct nw_control_builder *builder,
                            void *buffer, size_t capacity, unsigned operation) {
  if (client->failed) { errno = ENOTCONN; return -1; }
  if (!++client->sequence) client->sequence++;
  if (nw_control_begin(builder, buffer, capacity, NW_CONTROL_REQUEST, operation, client->sequence,
                        client->session, client->generation, NW_CONTROL_OK)) { errno = EINVAL; return -1; }
  return 0;
}
static int wait_readable(struct nw_control_client *client, uint64_t deadline) {
  for (;;) {
    uint64_t now = now_ms(); if (now >= deadline) { client->failed = true; errno = ETIMEDOUT; return -1; }
    uint64_t remaining = deadline - now; if (remaining > INT32_MAX) remaining = INT32_MAX;
    struct pollfd descriptor = { .fd = client->fd, .events = POLLIN };
    int result = poll(&descriptor, 1, (int)remaining);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) { client->failed = true; if (!result) errno = ETIMEDOUT; return -1; }
    if (descriptor.revents & POLLIN) return 0;
    client->failed = true; errno = ENOTCONN; return -1;
  }
}
static int receive_packet(struct nw_control_client *client, struct nw_control_view *view) {
  struct iovec vector = { .iov_base = client->receive, .iov_len = NW_CONTROL_MAX_MESSAGE };
  struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1 };
  ssize_t length;
  do { length = recvmsg(client->fd, &message, 0); } while (length < 0 && errno == EINTR);
  if (length < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    client->failed = true; return -1;
  }
  if (!length || (message.msg_flags & MSG_TRUNC) || nw_control_decode(client->receive, (size_t)length, view)) {
    client->failed = true; errno = EPROTO; return -1;
  }
  if (view->header.session != client->session) { nw_control_clear(client->receive, (size_t)length); return 2; }
  return 1;
}
static int queue_event(struct nw_control_client *client, const struct nw_control_view *view) {
  size_t length = view->header.length;
  if (client->queued_packets >= 128 || length > 256u * 1024u - client->queued_bytes) {
    client->failed = true; errno = ENOBUFS; return -1;
  }
  struct nw_control_packet *packet = malloc(sizeof(*packet) + length);
  if (!packet) { client->failed = true; return -1; }
  packet->next = NULL; packet->length = length; memcpy(packet->bytes, view->bytes, length);
  if (client->tail) client->tail->next = packet; else client->head = packet;
  client->tail = packet; client->queued_packets++; client->queued_bytes += length;
  nw_control_clear(client->receive, length); return 0;
}
int nw_control_client_exchange(struct nw_control_client *client, struct nw_control_builder *request,
                               struct nw_control_view *reply, unsigned timeout_ms) {
  struct nw_control_view sent;
  if (client->failed || client->exchanging || nw_control_decode(request->bytes, request->length, &sent) ||
      sent.header.kind != NW_CONTROL_REQUEST || sent.header.session != client->session) {
    nw_control_clear(request->bytes, request->capacity); request->length = 0;
    errno = client->failed ? ENOTCONN : EINVAL; return -1;
  }
  client->exchanging = true;
  struct nw_control_header expected = sent.header;
  ssize_t length; do { length = send(client->fd, request->bytes, request->length, 0); } while (length < 0 && errno == EINTR);
  nw_control_clear(request->bytes, request->capacity); request->length = 0;
  if (length != (ssize_t)expected.length) {
    if (length >= 0) { errno = EPROTO; client->failed = true; }
    client->exchanging = false; return -1;
  }
  uint64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    int result = receive_packet(client, reply);
    if (result < 0) break;
    if (!result) { if (wait_readable(client, deadline)) break; continue; }
    if (result == 2) { if (now_ms() >= deadline) { client->failed = true; errno = ETIMEDOUT; break; } continue; }
    if (reply->header.kind == NW_CONTROL_EVENT) {
      if (queue_event(client, reply)) break;
    } else if (reply->header.kind == NW_CONTROL_REPLY) {
      if (reply->header.sequence == expected.sequence && reply->header.operation == expected.operation) {
        client->exchanging = false; return reply->header.status;
      }
      nw_control_clear(client->receive, reply->header.length);
    } else { client->failed = true; errno = EPROTO; break; }
    if (now_ms() >= deadline) { client->failed = true; errno = ETIMEDOUT; break; }
  }
  client->exchanging = false; return -1;
}
int nw_control_client_has_events(const struct nw_control_client *client) { return client->head != NULL; }
int nw_control_client_event(struct nw_control_client *client, struct nw_control_packet **packet, struct nw_control_view *view) {
  *packet = NULL;
  if (client->failed) { errno = ENOTCONN; return -1; }
  for (unsigned budget = 0; !client->head && budget < 64; budget++) {
    int result = receive_packet(client, view); if (result <= 0) return result;
    if (result == 2) continue;
    if (view->header.kind == NW_CONTROL_EVENT) { if (queue_event(client, view)) return -1; }
    else if (view->header.kind == NW_CONTROL_REPLY) nw_control_clear(client->receive, view->header.length);
    else { client->failed = true; errno = EPROTO; return -1; }
  }
  if (!client->head) return 0;
  struct nw_control_packet *ready = client->head; client->head = ready->next; if (!client->head) client->tail = NULL;
  client->queued_packets--; client->queued_bytes -= ready->length;
  if (nw_control_decode(ready->bytes, ready->length, view)) { nw_control_packet_free(ready); client->failed = true; errno = EPROTO; return -1; }
  *packet = ready; return 1;
}
