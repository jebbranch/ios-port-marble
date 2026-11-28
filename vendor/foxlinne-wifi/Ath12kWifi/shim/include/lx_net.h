/* Just enough of the socket interface for the QMI library: datagram sockets of the QRTR family, served by nw_qrtr.c. */
#ifndef LX_NET_H
#define LX_NET_H
#include "lx_os.h"
typedef unsigned short __kernel_sa_family_t;
typedef u32 __u32;
#define AF_QIPCRTR 42
#define PF_QIPCRTR 42
#define SOCK_DGRAM 2
#define MSG_DONTWAIT 0x40

struct sockaddr { __kernel_sa_family_t sa_family; char sa_data[14]; };
struct kvec { void *iov_base; size_t iov_len; };
struct msghdr { void *msg_name; int msg_namelen; unsigned int msg_flags; };
struct net { int unused; };
extern struct net init_net;
struct sock {
  void *sk_user_data; void (*sk_data_ready)(struct sock *); void (*sk_error_report)(struct sock *);
  struct list_head rx; u32 port; int reset; u32 peer_node, peer_port; int connected;
  /* Optional native transmit-status owner; QRTR leaves these unset. */
  bool wifi_status;
  int pacing_shift;
  bool (*try_hold)(struct sock *);
  void (*put)(struct sock *);
  void (*wifi_ack)(struct sock *, bool valid, bool acked);
};
static inline bool sk_requests_wifi_status(struct sock *sk) {
  return sk && sk->wifi_status && sk->try_hold && sk->put;
}
static inline void sk_pacing_shift_update(struct sock *sk, int shift) { if (sk) WRITE_ONCE(sk->pacing_shift, shift); }
struct socket { struct sock *sk; };
int sock_create_kern(struct net *net, int family, int type, int proto, struct socket **res);
void sock_release(struct socket *s);
int kernel_getsockname(struct socket *s, struct sockaddr *addr);
int kernel_connect(struct socket *s, struct sockaddr *addr, int len, int flags);
int kernel_bind(struct socket *s, struct sockaddr *addr, int len);
int kernel_sendmsg(struct socket *s, struct msghdr *m, struct kvec *v, size_t n, size_t len);
int kernel_recvmsg(struct socket *s, struct msghdr *m, struct kvec *v, size_t n, size_t len, int flags);
#define trace_sk_data_ready(sk) ((void)0)
#endif
