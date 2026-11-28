/* A QRTR router for one remote node, with the name service the QMI library talks to. Sockets are datagram ports in a table; packets for
 * the remote node leave through the endpoint, packets from it are delivered to local ports or handled here when they are control packets. */
#include "lx_net.h"
#include "linux/qrtr.h"
#include "nw_qrtr.h"

#define LOCAL_NODE 1u
#define MAX_SOCK 16
#define MAX_SRV 32
#define MAX_LOOKUP 16
#define QRTR_PROTO_VER_1 1
#define QRTR_PROTO_VER_2 3

struct rx_pkt { struct list_head item; u32 node, port; size_t len; u8 data[]; };
struct srv { int used; u32 service, instance, node, port; };
struct lookup { int used; u32 service, instance, port; };

struct hdr_v1 { __le32 version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port; } __packed;
struct hdr_v2 { u8 version, type, flags, optlen; __le32 size; __le16 src_node, src_port, dst_node, dst_port; };

static struct sock *g_sock[MAX_SOCK];
static struct srv g_srv[MAX_SRV];
static struct lookup g_lookup[MAX_LOOKUP];
static nw_qrtr_xmit_fn g_xmit; static void *g_ctx;
static int g_hello_sent;
static u32 g_remote;      /* node id the remote announced, 0 until the first packet */
static u32 g_next_port = 0x4000;

void nw_qrtr_init(void) { memset(g_sock, 0, sizeof g_sock); memset(g_srv, 0, sizeof g_srv); memset(g_lookup, 0, sizeof g_lookup); g_remote = 0; g_next_port = 0x4000; }

static int send_remote(u32 type, u32 sn, u32 sp, u32 dn, u32 dp, const void *p, size_t len, int confirm) {
  if (!g_xmit) return -ENETDOWN;
  size_t padded = ALIGN(len, 4); u8 *b = lx_alloc(sizeof(struct hdr_v1) + padded, 1); if (!b) return -ENOMEM;
  struct hdr_v1 *h = (struct hdr_v1 *)b;
  h->version = cpu_to_le32(QRTR_PROTO_VER_1); h->type = cpu_to_le32(type);
  h->src_node = cpu_to_le32(sn); h->src_port = cpu_to_le32(sp); h->confirm_rx = cpu_to_le32(confirm);
  h->size = cpu_to_le32(len); h->dst_node = cpu_to_le32(dn); h->dst_port = cpu_to_le32(dp);
  if (len) memcpy(b + sizeof *h, p, len);
  int r = g_xmit(g_ctx, b, sizeof *h + padded);
  lx_free(b); return r; }

static void deliver(struct sock *s, u32 node, u32 port, const void *p, size_t len) {
  struct rx_pkt *r = lx_alloc(sizeof *r + len, 0); if (!r) return;
  r->node = node; r->port = port; r->len = len; memcpy(r->data, p, len);
  list_add_tail(&r->item, &s->rx);
  if (s->sk_data_ready) s->sk_data_ready(s); }
static struct sock *find_port(u32 port) { for (int i = 0; i < MAX_SOCK; i++) if (g_sock[i] && g_sock[i]->port == port) return g_sock[i]; return 0; }

static void ctrl_to(u32 port, u32 cmd, u32 svc, u32 inst, u32 node, u32 sp) {
  struct qrtr_ctrl_pkt pk; memset(&pk, 0, sizeof pk);
  pk.cmd = cpu_to_le32(cmd); pk.server.service = cpu_to_le32(svc); pk.server.instance = cpu_to_le32(inst);
  pk.server.node = cpu_to_le32(node); pk.server.port = cpu_to_le32(sp);
  struct sock *s = find_port(port); if (s) deliver(s, LOCAL_NODE, QRTR_PORT_CTRL, &pk, sizeof pk); }

static int lookup_matches(const struct lookup *l, const struct srv *v) {
  if (l->service && l->service != v->service) return 0;
  if (l->instance && l->instance != v->instance) return 0;
  return 1; }

static void ns_new_server(u32 svc, u32 inst, u32 node, u32 port) {
  struct srv *slot = 0;
  for (int i = 0; i < MAX_SRV; i++) { if (g_srv[i].used && g_srv[i].service == svc && g_srv[i].instance == inst && g_srv[i].node == node && g_srv[i].port == port) return; if (!g_srv[i].used && !slot) slot = &g_srv[i]; }
  if (!slot) return;
  slot->used = 1; slot->service = svc; slot->instance = inst; slot->node = node; slot->port = port;
  for (int i = 0; i < MAX_LOOKUP; i++) if (g_lookup[i].used && lookup_matches(&g_lookup[i], slot)) ctrl_to(g_lookup[i].port, QRTR_TYPE_NEW_SERVER, svc, inst, node, port); }
static void ns_del_server(u32 svc, u32 inst, u32 node, u32 port) {
  for (int i = 0; i < MAX_SRV; i++) if (g_srv[i].used && g_srv[i].service == svc && g_srv[i].node == node && g_srv[i].port == port) {
    g_srv[i].used = 0;
    for (int k = 0; k < MAX_LOOKUP; k++) if (g_lookup[k].used && lookup_matches(&g_lookup[k], &g_srv[i])) ctrl_to(g_lookup[k].port, QRTR_TYPE_DEL_SERVER, svc, inst, node, port); }
  (void)inst; }
static void ns_new_lookup(u32 from_port, u32 svc, u32 inst) {
  struct lookup *slot = 0;
  for (int i = 0; i < MAX_LOOKUP; i++) { if (!g_lookup[i].used) { if (!slot) slot = &g_lookup[i]; } else if (g_lookup[i].port == from_port && g_lookup[i].service == svc && g_lookup[i].instance == inst) return; }
  if (!slot) return;
  slot->used = 1; slot->service = svc; slot->instance = inst; slot->port = from_port;
  for (int i = 0; i < MAX_SRV; i++) if (g_srv[i].used && lookup_matches(slot, &g_srv[i])) ctrl_to(from_port, QRTR_TYPE_NEW_SERVER, g_srv[i].service, g_srv[i].instance, g_srv[i].node, g_srv[i].port); }

/* a control packet that reached the name service: from the remote node or from a local socket */
static void ns_handle(u32 src_node, u32 src_port, const struct qrtr_ctrl_pkt *p, size_t len) {
  if (len < sizeof *p) return;
  switch (le32_to_cpu(p->cmd)) {
  case QRTR_TYPE_HELLO:
    /* answered once per link: two name services that echo each other's hello would otherwise never stop */
    if (src_node != LOCAL_NODE && !g_hello_sent) { g_hello_sent = 1; send_remote(QRTR_TYPE_HELLO, LOCAL_NODE, QRTR_PORT_CTRL, src_node, QRTR_PORT_CTRL, &(struct qrtr_ctrl_pkt){ .cmd = cpu_to_le32(QRTR_TYPE_HELLO) }, sizeof(struct qrtr_ctrl_pkt), 0); }
    break;
  case QRTR_TYPE_NEW_SERVER: ns_new_server(le32_to_cpu(p->server.service), le32_to_cpu(p->server.instance), le32_to_cpu(p->server.node), le32_to_cpu(p->server.port)); break;
  case QRTR_TYPE_DEL_SERVER: ns_del_server(le32_to_cpu(p->server.service), le32_to_cpu(p->server.instance), le32_to_cpu(p->server.node), le32_to_cpu(p->server.port)); break;
  case QRTR_TYPE_NEW_LOOKUP: ns_new_lookup(src_port, le32_to_cpu(p->server.service), le32_to_cpu(p->server.instance)); break;
  case QRTR_TYPE_DEL_LOOKUP:
    for (int i = 0; i < MAX_LOOKUP; i++) if (g_lookup[i].used && g_lookup[i].port == src_port && g_lookup[i].service == le32_to_cpu(p->server.service)) g_lookup[i].used = 0;
    break;
  case QRTR_TYPE_BYE:
    for (int i = 0; i < MAX_SRV; i++) if (g_srv[i].used && g_srv[i].node == src_node) g_srv[i].used = 0;
    break;
  default: break;
  }
}

void nw_qrtr_set_endpoint(nw_qrtr_xmit_fn xmit, void *ctx) {
  g_xmit = xmit; g_ctx = ctx; g_hello_sent = xmit != 0;
  if (xmit) {   /* introduce ourselves to every node behind the endpoint */
    struct qrtr_ctrl_pkt hello = { .cmd = cpu_to_le32(QRTR_TYPE_HELLO) };
    send_remote(QRTR_TYPE_HELLO, LOCAL_NODE, QRTR_PORT_CTRL, QRTR_NODE_BCAST, QRTR_PORT_CTRL, &hello, sizeof hello, 0);
  }
}
void nw_qrtr_endpoint_down(void) {
  g_xmit = 0; g_hello_sent = 0;
  for (int i = 0; i < MAX_SRV; i++) if (g_srv[i].used && g_srv[i].node != LOCAL_NODE) { u32 sv = g_srv[i].service, in = g_srv[i].instance, nd = g_srv[i].node, po = g_srv[i].port; ns_del_server(sv, in, nd, po); }
}

void nw_qrtr_rx(const void *pkt, size_t len) {
  const u8 *b = pkt; u32 type, sn, sp, dn, dp, size, confirm = 0; size_t hl;
  if (len < 4) return;
  u32 ver = b[0];
  if (ver == QRTR_PROTO_VER_1 && len >= sizeof(struct hdr_v1)) {
    const struct hdr_v1 *h = pkt; hl = sizeof *h;
    type = le32_to_cpu(h->type); sn = le32_to_cpu(h->src_node); sp = le32_to_cpu(h->src_port);
    dn = le32_to_cpu(h->dst_node); dp = le32_to_cpu(h->dst_port); size = le32_to_cpu(h->size); confirm = le32_to_cpu(h->confirm_rx);
  } else if (ver == QRTR_PROTO_VER_2 && len >= sizeof(struct hdr_v2)) {
    const struct hdr_v2 *h = pkt; hl = sizeof *h + h->optlen;
    type = h->type; sn = le16_to_cpu(h->src_node); sp = le16_to_cpu(h->src_port); dn = le16_to_cpu(h->dst_node); dp = le16_to_cpu(h->dst_port);
    size = le32_to_cpu(h->size); confirm = h->flags & 1;
    if (sn == 0xffff) sn = QRTR_NODE_BCAST; if (sp == 0xffff) sp = QRTR_PORT_CTRL; if (dn == 0xffff) dn = QRTR_NODE_BCAST; if (dp == 0xffff) dp = QRTR_PORT_CTRL;
  } else { lx_log(4, "qrtr: bad packet version %u len %zu", ver, len); return; }
  if (hl + size > len) { lx_log(4, "qrtr: truncated packet"); return; }
  const u8 *pl = b + hl;
  if (type != QRTR_TYPE_DATA && type != QRTR_TYPE_RESUME_TX) {
    /* control traffic is addressed to the name service; the sender's node tells us which node the remote is */
  }
  if (!g_remote && sn != LOCAL_NODE && sn != QRTR_NODE_BCAST) g_remote = sn;
  if (confirm && type == QRTR_TYPE_DATA) {
    struct qrtr_ctrl_pkt r = { .cmd = cpu_to_le32(QRTR_TYPE_RESUME_TX) }; r.client.node = cpu_to_le32(dn); r.client.port = cpu_to_le32(dp);
    send_remote(QRTR_TYPE_RESUME_TX, dn, dp, sn, sp, &r, sizeof r, 0);
  }
  switch (type) {
  case QRTR_TYPE_DATA: { struct sock *s = find_port(dp); lx_log(7, "qrtr: data from %u:%x to port %x, %u bytes%s", sn, sp, dp, size, s ? "" : " (no socket)"); if (s) deliver(s, sn, sp, pl, size); else lx_log(6, "qrtr: no port %x", dp); break; }
  case QRTR_TYPE_RESUME_TX: case QRTR_TYPE_PING: case QRTR_TYPE_EXIT: break;
  default: ns_handle(sn, sp, (const struct qrtr_ctrl_pkt *)pl, size); break;
  }
}

/* sockets */
int sock_create_kern(struct net *net, int family, int type, int proto, struct socket **res) {
  (void)net; (void)type; (void)proto; if (family != AF_QIPCRTR) return -EAFNOSUPPORT;
  struct socket *so = lx_alloc(sizeof *so, 1); struct sock *sk = lx_alloc(sizeof *sk, 1); if (!so || !sk) { lx_free(so); lx_free(sk); return -ENOMEM; }
  INIT_LIST_HEAD(&sk->rx); sk->port = g_next_port++; so->sk = sk;
  for (int i = 0; i < MAX_SOCK; i++) if (!g_sock[i]) { g_sock[i] = sk; *res = so; return 0; }
  lx_free(so); lx_free(sk); return -EMFILE; }
void sock_release(struct socket *so) {
  if (!so) return; struct sock *sk = so->sk;
  for (int i = 0; i < MAX_SOCK; i++) if (g_sock[i] == sk) g_sock[i] = 0;
  for (int i = 0; i < MAX_LOOKUP; i++) if (g_lookup[i].used && g_lookup[i].port == sk->port) g_lookup[i].used = 0;
  while (!list_empty(&sk->rx)) { struct rx_pkt *r = list_first_entry(&sk->rx, struct rx_pkt, item); list_del(&r->item); lx_free(r); }
  lx_free(sk); lx_free(so); }
struct qsa { __kernel_sa_family_t family; u32 node, port; };
int kernel_getsockname(struct socket *so, struct sockaddr *addr) {
  struct qsa *a = (struct qsa *)addr; a->family = AF_QIPCRTR; a->node = LOCAL_NODE; a->port = so->sk->port; return 0; }
int kernel_bind(struct socket *so, struct sockaddr *addr, int len) { (void)len; so->sk->port = ((struct qsa *)addr)->port; return 0; }
int kernel_connect(struct socket *so, struct sockaddr *addr, int len, int flags) {
  (void)len; (void)flags; struct qsa *a = (struct qsa *)addr; so->sk->peer_node = a->node; so->sk->peer_port = a->port; so->sk->connected = 1; return 0; }
int kernel_sendmsg(struct socket *so, struct msghdr *m, struct kvec *v, size_t n, size_t len) {
  if (n != 1) return -EINVAL;
  struct qsa peer = { AF_QIPCRTR, so->sk->peer_node, so->sk->peer_port };
  struct qsa *to = m->msg_name ? m->msg_name : &peer; if (!m->msg_name && !so->sk->connected) return -EDESTADDRREQ;
  u32 dn = to->node, dp = to->port;
  const void *p = v->iov_base;
  if (dn == LOCAL_NODE || (dn == QRTR_NODE_BCAST && dp == QRTR_PORT_CTRL && 0)) {
    if (dp == QRTR_PORT_CTRL) { ns_handle(LOCAL_NODE, so->sk->port, p, len); return (int)len; }
    struct sock *s = find_port(dp); if (!s) return -ECONNREFUSED; deliver(s, LOCAL_NODE, so->sk->port, p, len); return (int)len; }
  /* the remote node: a lookup is answered from the table (done above for the local name service), data goes out */
  if (dp == QRTR_PORT_CTRL) { send_remote(QRTR_TYPE_DATA, LOCAL_NODE, so->sk->port, dn, dp, p, len, 0); return (int)len; }
  int r = send_remote(QRTR_TYPE_DATA, LOCAL_NODE, so->sk->port, dn, dp, p, len, 0);
  return r < 0 ? r : (int)len; }
int kernel_recvmsg(struct socket *so, struct msghdr *m, struct kvec *v, size_t n, size_t len, int flags) {
  (void)n; (void)flags; struct sock *sk = so->sk;
  if (list_empty(&sk->rx)) return -EAGAIN;
  struct rx_pkt *r = list_first_entry(&sk->rx, struct rx_pkt, item); list_del(&r->item);
  size_t c = r->len < len ? r->len : len; memcpy(v->iov_base, r->data, c);
  if (m->msg_name) { struct qsa *a = m->msg_name; a->family = AF_QIPCRTR; a->node = r->node; a->port = r->port; }
  lx_free(r); return (int)c; }
