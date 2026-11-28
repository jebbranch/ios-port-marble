/* Exercise production DMA-buffer ownership with a bounded host allocator. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/inet_ecn.h>
#include <linux/bitmap.h>
#include <linux/u64_stats_sync.h>
#include <linux/crc32.h>
#include <net/cfg80211.h>
#include <linux/if_vlan.h>
#include <net/sch_generic.h>
#include "core.h"
#include "nl80211.h"
DECLARE_EWMA(buffer_test, 10, 8)

static unsigned allocations, releases, fail_dma, fail_dma_once, fail_heap, heap_allocations, heap_releases;
static u64 clock_ms;
static u64 now(void) { return clock_ms; }
static void sleep_ms(unsigned ms) { clock_ms += ms; }
static void nothing(void) {}
static void log_line(int level, const char *line) { (void)level; (void)line; }
static void halt(void) { abort(); }
static void *heap_alloc(size_t n) { if (fail_heap) return NULL; void *p = malloc(n); if (p) heap_allocations++; return p; }
static void heap_free(void *p) { assert(p); heap_releases++; free(p); }
static void *dma_alloc(size_t n, uint64_t *bus) {
  if (fail_dma) return NULL;
  if (fail_dma_once) { fail_dma_once--; return NULL; }
  void *p = calloc(1, n); assert(p); allocations++; *bus = (uintptr_t)p; return p;
}
static void dma_free(size_t n, void *p, uint64_t bus) {
  (void)n; (void)bus; assert(p); releases++; free(p);
}
static uint64_t dma_map(void *p, size_t n, int dir) { (void)n; (void)dir; return (uintptr_t)p; }
static void dma_sync(uint64_t bus, size_t n, int dir) { (void)bus; (void)n; (void)dir; }
static int firmware(const char *name, const u8 **p, size_t *n) { (void)name; (void)p; (void)n; return -1; }
static const struct lx_platform platform = {
  now, sleep_ms, nothing, log_line, halt, heap_alloc, heap_free,
  dma_alloc, dma_free, dma_map, dma_sync, firmware
};

static struct sk_buff *packet(unsigned head, unsigned len, unsigned tail) {
  struct sk_buff *s = alloc_skb(head + len + tail, 0); assert(s);
  skb_reserve(s, head);
  u8 *p = skb_put(s, len); for (unsigned i = 0; i < len; i++) p[i] = (u8)(i ^ 0x5a);
  s->protocol = 0x1234; s->priority = 6; s->queue_mapping = 3; s->mark = 0x11223344;
  memset(s->cb, 0x27, sizeof s->cb);
  skb_set_network_header(s, 14); s->transport_header = s->network_header + 20;
  return s;
}
static void clone_and_cow(void) {
  struct sk_buff *s = packet(32, 1500, 16);
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  struct sk_buff *c = skb_clone(s, 0); assert(c && c->head == s->head && c->data == s->data);
  nw_wifi_buffer_stats_get(&after);
  assert(after.dma_allocations == before.dma_allocations && after.copied_bytes == before.copied_bytes);
  assert(skb_cloned(s) && skb_cloned(c));
  assert(c->protocol == s->protocol && c->priority == 6 && c->queue_mapping == 3 && c->mark == s->mark);
  assert(!memcmp(c->cb, s->cb, sizeof c->cb));
  assert(!skb_cow_head(c, 80));
  assert(c->head != s->head && skb_headroom(c) >= 80 && c->len == 1500);
  assert(skb_network_offset(c) == 14 && c->transport_header - c->network_header == 20);
  assert(!memcmp(c->data, s->data, s->len));
  c->data[0] ^= 0xff; assert(c->data[0] != s->data[0]);
  assert(!skb_cloned(s) && !skb_cloned(c));
  kfree_skb(s); assert(c->data[1] == (1 ^ 0x5a)); kfree_skb(c);

  s = packet(0, 17, 0); c = skb_clone(s, 0); assert(c);
  kfree_skb(s); assert(c->data[16] == (16 ^ 0x5a)); kfree_skb(c);
}
static void expansion_and_failures(void) {
  struct sk_buff *s = packet(16, 61, 0), *c = skb_clone(s, 0); assert(c);
  u8 *head = c->head; unsigned refs = skb_shinfo(c)->dataref;
  fail_dma = 1;
  assert(pskb_expand_head(c, 16, 16, 0) == -ENOMEM);
  assert(c->head == head && c->len == 61 && skb_shinfo(c)->dataref == refs);
  fail_dma = 0; fail_heap = 1; assert(!skb_clone(s, 0)); fail_heap = 0;
  assert(pskb_expand_head(c, -1, 0, 0) == -EINVAL);
  assert(skb_ensure_writable(c, 62) == -EINVAL);
  assert(!skb_ensure_writable(c, 61)); assert(c->head != s->head);
  struct sk_buff *trailer = NULL;
  assert(skb_cow_data(c, 35, &trailer) == 1 && trailer == c && skb_tailroom(c) >= 35);
  assert(skb_headroom(c) == 16 && !memcmp(c->data, s->data, s->len));
  struct sk_buff *copy = skb_copy_expand(c, 64, 24, 0); assert(copy);
  assert(skb_headroom(copy) == 64 && skb_tailroom(copy) >= 24 && skb_network_offset(copy) == 14);
  assert(!memcmp(copy->cb, c->cb, sizeof c->cb) && !memcmp(copy->data, c->data, c->len));
  assert(!skb_copy_expand(c, -1, 0, 0));
  kfree_skb(copy); kfree_skb(c); kfree_skb(s);
}
static void fragments(void) {
  struct sk_buff *s = packet(16, 24, 0), *f = packet(0, 31, 0);
  skb_shinfo(s)->frag_list = f; s->len += f->len; s->data_len = f->len;
  struct sk_buff *c = skb_clone(s, 0); assert(c);
  u8 middle[20]; assert(!skb_copy_bits(s, 20, middle, sizeof middle));
  assert(!memcmp(middle, s->data + 20, 4) && !memcmp(middle + 4, f->data, 16));
  assert(skb_copy_bits(s, 50, middle, 6) == -EFAULT);
  assert(!skb_linearize(c) && !c->data_len && c->len == 55);
  assert(!memcmp(c->data, s->data, 24) && !memcmp(c->data + 24, f->data, 31));
  assert(skb_shinfo(c)->frag_list == NULL && skb_shinfo(s)->frag_list == f);
  kfree_skb(c); assert(f->len == 31); kfree_skb(s);

  s = packet(0, 16, 0); s->data_len = 17;
  assert(pskb_expand_head(s, 0, 4, 0) == -EINVAL && s->len == 16);
  s->data_len = 0; kfree_skb(s);
}
static void flow_hash(void) {
  struct sk_buff *a = packet(32, 80, 0), *b = packet(0, 120, 0);
  memset(a->data, 0, 54); a->data[12] = 8; a->data[13] = 0;
  a->data[14] = 0x45; a->data[23] = 6;
  memcpy(a->data + 26, "\xc0\xa8\x01\x02\xc0\xa8\x01\x03", 8);
  memcpy(a->data + 34, "\x12\x34\x00\x50", 4);
  memcpy(b->data, a->data, 54);
  u32 h = skb_get_hash(a); assert(h && skb_get_hash(b) == h);
  b->hash = 0; b->data[35] ^= 1; assert(skb_get_hash(b) != h);
  u8 head[16]; memset(a->data - 8, 0xa5, 8);
  assert(!skb_copy_bits(a, -8, head, sizeof head));
  for (unsigned i = 0; i < 8; i++) assert(head[i] == 0xa5);
  assert(!memcmp(head + 8, a->data, 8));
  assert(skb_copy_bits(a, -33, head, 1) == -EFAULT);
  kfree_skb(a); kfree_skb(b);
}
static void support_semantics(void) {
  unsigned readers = lx_rcu_readers();
  { guard(rcu)(); assert(lx_rcu_readers() == readers + 1); }
  assert(lx_rcu_readers() == readers);
  struct mutex scoped; mutex_init(&scoped); unsigned entered = 0;
  scoped_guard(mutex, &scoped) { assert(scoped.held == 1); entered++; break; }
  assert(scoped.held == 0 && entered == 1);
  const char vector[] = "123456789";
  assert(~crc32_le(~0U, vector, 9) == 0xcbf43926U);
  assert(~crc32_be(~0U, vector, 9) == 0xfc891918U);
  assert(crc32_le(crc32_le(0, vector, 4), vector + 4, 5) == crc32_le(0, vector, 9));
  assert(crc32_be(crc32_be(0, vector, 4), vector + 4, 5) == crc32_be(0, vector, 9));
  struct ewma_buffer_test average; ewma_buffer_test_init(&average);
  ewma_buffer_test_add(&average, 80); ewma_buffer_test_add(&average, 0);
  assert(ewma_buffer_test_read(&average) == 70);
  unsigned long bits[2] = {0, ~0UL}, other[2] = {0, 1};
  assert(!bitmap_empty(bits, 65) && bitmap_subset(bits, other, 65));
  bits[1] = 2; assert(bitmap_empty(bits, 65));
  assert(usecs_to_jiffies(0) == 0 && usecs_to_jiffies(1) == 1 && usecs_to_jiffies(1001) == 2);
  struct u64_stats_sync sync; u64_stats_init(&sync);
  unsigned begin = u64_stats_fetch_begin(&sync); assert(!u64_stats_fetch_retry(&sync, begin));
  u64_stats_update_begin(&sync); u64_stats_update_end(&sync);
  assert(u64_stats_fetch_retry(&sync, begin));
  assert(jiffies_delta_to_msecs(-1) == 0 && jiffies_delta_to_msecs(120) == 120);
}
struct peer { unsigned key, index; struct rhlist_head node; };
static void peer_tables(void) {
  struct rhashtable_params params = { .key_len = sizeof(unsigned), .key_offset = offsetof(struct peer, key),
                                     .head_offset = offsetof(struct peer, node) };
  struct rhltable table; assert(!rhltable_init(&table, &params));
  struct peer a = { .key = 1, .index = 1 }, b = { .key = 2, .index = 2 },
              c = { .key = 1, .index = 3 }, d = { .key = 1, .index = 4 };
  assert(!rhltable_insert(&table, &a.node, params));
  assert(!rhltable_insert(&table, &b.node, params));
  assert(!rhltable_insert(&table, &c.node, params));
  assert(!rhltable_insert(&table, &d.node, params));
  assert(rhltable_insert(&table, &d.node, params) == -EEXIST && table.ht.n == 4);
  unsigned key = 1, count = 0; struct peer *peer; struct rhlist_head *node;
  rhl_for_each_entry_rcu(peer, node, rhltable_lookup(&table, &key, params), node) { assert(peer->key == 1); count++; }
  assert(count == 3);
  assert(!rhltable_remove(&table, &c.node, params) && table.ht.n == 3);
  assert(!rhltable_remove(&table, &d.node, params));
  assert(rhltable_lookup(&table, &key, params) == &a.node);
  key = 2; assert(rhltable_lookup(&table, &key, params) == &b.node);
  assert(!rhltable_remove(&table, &a.node, params) && !rhltable_remove(&table, &b.node, params));
  assert(table.ht.n == 0 && !table.ht.head && rhltable_remove(&table, &a.node, params) == -ENOENT);
  rhltable_destroy(&table);
}
static unsigned owner_releases;
static void owner_release(struct sk_buff *s) {
  assert(s->wifi_acked_valid && s->wifi_acked); assert(s->sk);
  owner_releases++;
}
static void metadata_ownership(void) {
  struct sk_buff *s = packet(32, 63, 8);
  skb_reset_mac_header(s); skb_hwtstamps(s)->hwtstamp = 1234567;
  s->sk = (struct sock *)&owner_releases; s->destructor = owner_release;
  struct sk_buff *c = skb_clone(s, 0), *copy = skb_copy(s, 0);
  assert(c && copy && !c->sk && !copy->sk && !c->destructor && !copy->destructor);
  assert(skb_hwtstamps(c)->hwtstamp == 1234567 && skb_hwtstamps(copy)->hwtstamp == 1234567);
  assert(!skb_cow_head(s, 80) && s->destructor == owner_release && s->sk);
  assert(skb_mac_header(s) == s->data && skb_hwtstamps(s)->hwtstamp == 1234567);
  kfree_skb(c); kfree_skb(copy); assert(owner_releases == 0);
  skb_get(s); assert(skb_shared(s)); kfree_skb(s);
  assert(!skb_shared(s) && owner_releases == 0);
  skb_complete_wifi_ack(s, true); assert(owner_releases == 1);
}
static unsigned received, flushed, wakes, stopped;
static int input_packet(struct net_device *dev, struct sk_buff *skb) {
  assert(dev == skb->dev && skb->len == 40 && skb_mac_header_was_set(skb));
  received++; kfree_skb(skb); return 0;
}
static void flush_packets(struct net_device *dev) { (void)dev; flushed++; }
static void wake_queues(struct net_device *dev) { (void)dev; wakes++; }
static int stop_interface(struct net_device *dev) { assert(!netif_running(dev)); stopped++; return 0; }
static u16 internet_checksum(const u8 *data, unsigned len) {
  u32 sum = 0;
  unsigned i;
  for (i = 0; i + 1 < len; i += 2) sum += (u16)((unsigned)data[i] * 256 + data[i + 1]);
  if (i < len) sum += (unsigned)data[i] * 256;
  while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
  return (u16)~sum;
}
static struct sk_buff *ip_packet(struct net_device *dev, bool ipv6) {
  struct sk_buff *skb = packet(32, 54, 8);
  memset(skb->data, 0, 54); memcpy(skb->data, dev->dev_addr, 6);
  put_unaligned_be16(ipv6 ? ETH_P_IPV6 : ETH_P_IP, skb->data + 12);
  u8 *ip = skb->data + 14;
  if (ipv6) { ip[0] = 0x6a; ip[1] = 0xa5; }
  else { ip[0] = 0x45; ip[1] = 0x2a; ip[2] = 0; ip[3] = 40; put_unaligned_be16(internet_checksum(ip, 20), ip + 10); }
  skb->protocol = eth_type_trans(skb, dev);
  assert(skb->len == 40 && skb_mac_header(skb) + 14 == skb->data && skb_network_header(skb) == skb->data);
  return skb;
}
static void network_boundary(void) {
  struct net_device dev = {0}; dev.dev_addr[0] = 2;
  for (unsigned ipv6 = 0; ipv6 < 2; ipv6++) {
    struct sk_buff *s = ip_packet(&dev, ipv6), *c = skb_clone(s, 0);
    int before = skb_get_dsfield(s); assert(before == (ipv6 ? 0xaa : 0x2a));
    assert(INET_ECN_set_ce(c) && skb_get_dsfield(c) == (before | 3));
    assert(skb_get_dsfield(s) == before && s->head != c->head);
    if (!ipv6) { assert(internet_checksum(c->data, 20) == 0 && internet_checksum(s->data, 20) == 0); }
    else assert(c->data[1] == 0xb5);
    kfree_skb(s); kfree_skb(c);
  }
  struct sk_buff *s = ip_packet(&dev, false); s->data[1] &= ~3;
  assert(!INET_ECN_set_ce(s)); kfree_skb(s);
  s = ip_packet(&dev, false); s->priority = 259;
  assert(cfg80211_classify8021d(s, NULL) == 3);
  s->priority = 0; assert(cfg80211_classify8021d(s, NULL) == 0); /* AF11 uses best effort, RFC8325. */
  s->data[1] = 0x20; assert(cfg80211_classify8021d(s, NULL) == 1); /* CS1 remains background. */
  __vlan_hwaccel_put_tag(s, cpu_to_be16(ETH_P_8021Q), 6 << VLAN_PRIO_SHIFT);
  assert(cfg80211_classify8021d(s, NULL) == 6); kfree_skb(s);
  LIST_HEAD(batch); dev.input = input_packet; dev.input_flush = flush_packets;
  for (unsigned i = 0; i < 3; i++) { s = ip_packet(&dev, false); list_add_tail(&s->list, &batch); }
  netif_receive_skb_list(&batch); assert(list_empty(&batch) && received == 3 && flushed == 1);
  dev.input = NULL; assert(netif_receive_skb(ip_packet(&dev, false)) == -ENODEV && dev.stats.rx_dropped == 1);
  dev.queue_wake = wake_queues;
  netif_stop_subqueue(&dev, 0); netif_stop_subqueue(&dev, 3);
  netif_wake_subqueue(&dev, 3); netif_wake_subqueue(&dev, 3);
  assert(wakes == 1 && __netif_subqueue_stopped(&dev, 0) && !__netif_subqueue_stopped(&dev, 3));
  static const struct net_device_ops ops = { .ndo_stop = stop_interface };
  dev.netdev_ops = &ops; dev.flags |= IFF_UP; dev.running = true;
  assert(!dev_close(&dev) && !netif_running(&dev)); assert(!dev_close(&dev) && stopped == 1);
  atomic_t count = ATOMIC_INIT(INT_MAX);
  assert(atomic_add_unless(&count, 1, 0) && atomic_read(&count) == INT_MIN);
}
struct receive_poll { struct napi_struct napi; unsigned left, calls; };
static int poll_packets(struct napi_struct *napi, int budget) {
  struct receive_poll *p = container_of(napi, struct receive_poll, napi);
  unsigned count = min(p->left, (unsigned)budget); p->left -= count; p->calls++;
  for (unsigned i = 0; i < count; i++) assert(napi_gro_receive(napi, ip_packet(napi->dev, false)) == GRO_NORMAL);
  if (count < (unsigned)budget) napi_complete_done(napi, count);
  return count;
}
static void napi_lifetime(void) {
  struct net_device dev = { .input = input_packet, .input_flush = flush_packets };
  dev.dev_addr[0] = 2;
  struct receive_poll p = { .left = 125 };
  unsigned before = received, batches = flushed;
  netif_napi_add(&dev, &p.napi, poll_packets);
  napi_schedule(&p.napi); lx_pump(); assert(p.calls == 0);
  napi_enable(&p.napi);
  assert(napi_schedule_prep(&p.napi) && !napi_schedule_prep(&p.napi));
  __napi_schedule(&p.napi);
  for (unsigned i = 0; i < 4 && p.left; i++) lx_pump();
  assert(!p.left && p.calls == 2 && received - before == 125 && flushed - batches == 2);
  napi_disable(&p.napi); napi_schedule(&p.napi); lx_pump(); assert(p.calls == 2);
  netif_napi_del(&p.napi); lx_pump();
  /* Reuse after delete must not retain a scheduled pointer to the old context. */
  memset(&p, 0, sizeof(p)); p.left = 1;
  netif_napi_add(&dev, &p.napi, poll_packets); napi_enable(&p.napi); napi_schedule(&p.napi);
  lx_pump(); assert(p.calls == 1 && !p.left); netif_napi_del(&p.napi);
}
static unsigned owner_refs = 1, ack_events;
static bool owner_valid, owner_acked;
static bool socket_hold(struct sock *sk) { (void)sk; if (!owner_refs) return false; owner_refs++; return true; }
static void socket_put(struct sock *sk) { (void)sk; assert(owner_refs); owner_refs--; }
static void socket_ack(struct sock *sk, bool valid, bool acked) { (void)sk; ack_events++; owner_valid = valid; owner_acked = acked; }
static void transmit_helpers(void) {
  struct sock socket = { .wifi_status = true, .try_hold = socket_hold, .put = socket_put, .wifi_ack = socket_ack };
  struct sk_buff *s = packet(32, 63, 8); s->sk = &socket;
  assert(sk_requests_wifi_status(&socket));
  struct sk_buff *c = skb_clone_sk(s); assert(c && c->sk == &socket && owner_refs == 2);
  skb_complete_wifi_ack(c, true); assert(owner_refs == 1 && ack_events == 1 && owner_valid && owner_acked);
  fail_heap = 1; assert(!skb_clone_sk(s) && owner_refs == 1); fail_heap = 0;
  c = skb_clone_sk(s); assert(c); kfree_skb(c); assert(owner_refs == 1 && ack_events == 2 && !owner_valid);
  s->sk = NULL;
  s->csum_start = (u16)(skb_headroom(s) + 14); s->csum_offset = 6; s->ip_summed = CHECKSUM_PARTIAL;
  s->data[20] = s->data[21] = 0;
  u16 expected = internet_checksum(s->data + 14, s->len - 14);
  c = skb_clone(s, 0); assert(!skb_cow_head(c, 80) && skb_checksum_start_offset(c) == 14);
  assert(!skb_csum_hwoffload_help(c, 0) && c->ip_summed == CHECKSUM_NONE);
  assert(get_unaligned_be16(c->data + 20) == (expected ? expected : 0xffff));
  assert(internet_checksum(c->data + 14, c->len - 14) == 0 && s->data[20] == 0 && s->ip_summed == CHECKSUM_PARTIAL);
  assert(!skb_csum_hwoffload_help(s, NETIF_F_HW_CSUM) && s->ip_summed == CHECKSUM_PARTIAL);
  s->csum_offset = 100; assert(skb_csum_hwoffload_help(s, 0) == -EINVAL);
  kfree_skb(c); kfree_skb(s);
  struct sk_buff_head first, second; skb_queue_head_init(&first); skb_queue_head_init(&second);
  struct sk_buff *a = packet(0, 14, 0), *b = packet(0, 14, 0);
  skb_queue_tail(&first, a); skb_queue_tail(&second, b);
  spin_lock(&first.lock); spin_lock(&second.lock); skb_queue_splice_init(&first, &second);
  assert(first.lock.held == 1 && second.lock.held == 1 && first.qlen == 0 && second.qlen == 2);
  spin_unlock(&first.lock); spin_unlock(&second.lock);
  assert(skb_dequeue(&second) == a && skb_dequeue(&second) == b && skb_queue_empty(&second));
  kfree_skb(a); kfree_skb(b);
}
static unsigned events[32], initialized, opened, uninitialized;
static bool reject_open;
static int device_event(struct notifier_block *block, unsigned long event, void *info) {
  (void)block; assert(netdev_notifier_info_to_dev(info)); assert(event < ARRAY_SIZE(events)); events[event]++;
  return event == NETDEV_PRE_UP && reject_open ? notifier_from_errno(-EPERM) : NOTIFY_OK;
}
static int init_device(struct net_device *dev) { assert(!dev->registered); initialized++; return 0; }
static int open_device(struct net_device *dev) { assert(netif_running(dev)); opened++; return 0; }
static void uninit_device(struct net_device *dev) { assert(!netif_running(dev) && !dev->registered); uninitialized++; }
static void interface_lifetime(void) {
  static const struct net_device_ops ops = { .ndo_init = init_device, .ndo_open = open_device, .ndo_stop = stop_interface, .ndo_uninit = uninit_device };
  struct notifier_block notifier = { .notifier_call = device_event };
  assert(!register_netdevice_notifier(&notifier));
  struct net_device *a = alloc_netdev_mqs(97, "wlan%d", NET_NAME_ENUM, ether_setup, 4, 1);
  assert(a && !((uintptr_t)a & 31) && !((uintptr_t)netdev_priv(a) & 31)); a->netdev_ops = &ops;
  assert(dev_alloc_name(a, a->name) == 0 && !strcmp(a->name, "wlan0") && !register_netdevice(a));
  assert(register_netdevice(a) == -EBUSY && initialized == 1 && events[NETDEV_REGISTER] == 1);
  struct net_device *b = alloc_netdev_mqs(1, "wlan%d", NET_NAME_ENUM, ether_setup, 1, 1); assert(b);
  assert(dev_alloc_name(b, b->name) == 1 && !strcmp(b->name, "wlan1"));
  assert(dev_alloc_name(b, "bad%p") == -EINVAL && dev_alloc_name(b, "bad/name") == -EINVAL);
  struct sockaddr address = { .sa_data = { 2, 1, 2, 3, 4, 5 } };
  reject_open = true; assert(dev_open(a, NULL) == -EPERM && !netif_running(a) && !opened); reject_open = false;
  assert(!eth_mac_addr(a, &address) && !dev_open(a, NULL) && !dev_open(a, NULL) && opened == 1 && events[NETDEV_UP] == 1);
  assert(eth_mac_addr(a, &address) == -EBUSY); a->priv_flags |= IFF_LIVE_ADDR_CHANGE; assert(!eth_mac_addr(a, &address));
  dev_hold(a); unsigned frees = heap_releases;
  free_netdev(a); assert(uninitialized == 1 && events[NETDEV_DOWN] == 1 && events[NETDEV_UNREGISTER] == 1);
  assert(a->free_requested && !a->registered && heap_releases == frees); dev_put(a);
  assert(heap_releases == frees + 1);
  assert(dev_alloc_name(b, "wlan%d") == 0); free_netdev(b);
  assert(!unregister_netdevice_notifier(&notifier));
  struct netdev_hw_addr_list all, one, two; __hw_addr_init(&all); __hw_addr_init(&one); __hw_addr_init(&two);
  const u8 group[6] = { 1, 0, 0x5e, 0, 0, 1 };
  assert(!__hw_addr_add(&one, group, 6, 2) && !__hw_addr_add(&two, group, 6, 2));
  assert(!__hw_addr_sync(&all, &one, 6) && !__hw_addr_sync(&all, &one, 6) && all.count == 1);
  assert(!__hw_addr_sync(&all, &two, 6) && all.count == 1);
  assert(!__hw_addr_del(&one, group, 6, 2) && !__hw_addr_sync(&all, &one, 6) && one.count == 0 && all.count == 1);
  __hw_addr_unsync(&all, &two, 6); assert(all.count == 0 && two.count == 1);
  assert(!__hw_addr_del(&two, group, 6, 2) && two.count == 0);
  __hw_addr_flush(&all); __hw_addr_flush(&one); __hw_addr_flush(&two);
}
static void dma_page_fragments(void) {
  struct sk_buff *parent = packet(32, 1600, 16), *child = packet(32, 32, 0);
  u8 expected[1600]; memcpy(expected, parent->data, sizeof(expected));
  memcpy(child->data, parent->data, 32);
  struct page *page = virt_to_head_page(parent->head); void *old_head = parent->head;
  assert(page && page == virt_to_head_page(parent->data + 1200) && page->references == 1);
  get_page(page); skb_add_rx_frag(child, 0, page, (int)(parent->data + 32 - parent->head), 1568, 1568);
  struct sk_buff *clone = skb_clone(child, 0); assert(clone && page->references == 2);
  kfree_skb(parent); assert(page->references == 1);
  u8 sample[100]; assert(!skb_copy_bits(clone, 20, sample, 100) && !memcmp(sample, expected + 20, 100));
  u8 scratch[100]; assert(skb_header_pointer(clone, 20, 100, scratch) == scratch && !memcmp(scratch, sample, 100));
  fail_dma = 1; assert(skb_linearize(clone) == -ENOMEM && page->references == 1 && clone->data_len == 1568); fail_dma = 0;
  kfree_skb(child); assert(!skb_linearize(clone) && !clone->data_len && !memcmp(clone->data, expected, 1600));
  assert(!virt_to_head_page(old_head)); kfree_skb(clone);
  struct sk_buff *packets[257];
  for (unsigned i = 0; i < ARRAY_SIZE(packets); i++) packets[i] = packet(0, 32, 0);
  for (unsigned i = 0; i < ARRAY_SIZE(packets); i++) {
    unsigned index = i * 73 % ARRAY_SIZE(packets); void *head = packets[index]->head;
    assert(virt_to_head_page((u8 *)head + 20)); kfree_skb(packets[index]); assert(!virt_to_head_page(head));
  }
}
static const u8 amsdu_own[6] = {2, 3, 4, 5, 6, 7}, amsdu_peer[6] = {2, 8, 9, 10, 11, 12};
static struct sk_buff *aggregate(bool malformed) {
  struct sk_buff *skb = alloc_skb(32 + 2846, 0); assert(skb); skb_reserve(skb, 32);
  const u8 snap[8] = {0xaa, 0xaa, 3, 0, 0, 0, 8, 0};
  for (unsigned frame = 0; frame < 2; frame++) {
    u8 *header = skb_put(skb, 14); memcpy(header, amsdu_own, 6); memcpy(header + 6, amsdu_peer, 6);
    put_unaligned_be16(malformed && frame == 1 ? 4000 : 1408, header + 12);
    skb_put_data(skb, snap, 8);
    u8 *body = skb_put(skb, 1400); for (unsigned i = 0; i < 1400; i++) body[i] = (u8)(i + frame);
    if (!frame) skb_put_zero(skb, 2);
  }
  return skb;
}
static void mainline_amsdu(void) {
  struct sk_buff *skb = aggregate(false); void *parent_head = skb->head;
  struct sk_buff_head frames; skb_queue_head_init(&frames);
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  ieee80211_amsdu_to_8023s(skb, &frames, amsdu_own, NL80211_IFTYPE_STATION, 0, amsdu_own, amsdu_peer, 0);
  nw_wifi_buffer_stats_get(&after);
  assert(frames.qlen == 2 && after.copied_bytes - before.copied_bytes == 92);
  assert(virt_to_head_page(parent_head));
  for (unsigned frame = 0; frame < 2; frame++) {
    struct sk_buff *output = skb_dequeue(&frames); assert(output && output->len == 1414 && output->data_len == 1376);
    assert(skb_shinfo(output)->nr_frags == 1 && skb_frag_page(&skb_shinfo(output)->frags[0])->address == parent_head);
    u8 bytes[1414]; assert(!skb_copy_bits(output, 0, bytes, sizeof(bytes)));
    assert(!memcmp(bytes, amsdu_own, 6) && !memcmp(bytes + 6, amsdu_peer, 6) && get_unaligned_be16(bytes + 12) == ETH_P_IP);
    for (unsigned i = 0; i < 1400; i++) assert(bytes[14 + i] == (u8)(i + frame));
    kfree_skb(output);
  }
  assert(!virt_to_head_page(parent_head));
  ieee80211_amsdu_to_8023s(aggregate(true), &frames, amsdu_own, NL80211_IFTYPE_STATION, 0, amsdu_own, amsdu_peer, 0);
  assert(skb_queue_empty(&frames));
  skb = aggregate(false); fail_dma = 1;
  ieee80211_amsdu_to_8023s(skb, &frames, amsdu_own, NL80211_IFTYPE_STATION, 0, amsdu_own, amsdu_peer, 0);
  fail_dma = 0; assert(skb_queue_empty(&frames));
  puts("mainline A-MSDU: 2 x 1400-byte payloads share their DMA parent; 92 bytes copied during splitting");
}
static unsigned tx_seen[16], tx_seen_count, tx_busy_count;
static netdev_tx_t transmit_packet(struct sk_buff *packet, struct net_device *dev) {
  if (packet->data[0] == 9 && !tx_busy_count++) return NETDEV_TX_BUSY;
  if (packet->data[0] == 15) {
    struct sk_buff *nested = alloc_skb(200, GFP_ATOMIC); assert(nested);
    memset(skb_put(nested, 150), 0, 150); nested->data[0] = 16; nested->dev = dev; nested->queue_mapping = packet->queue_mapping;
    unsigned before = tx_seen_count; assert(!dev_queue_xmit(nested)); lx_pump(); assert(tx_seen_count == before);
  }
  assert(tx_seen_count < ARRAY_SIZE(tx_seen)); tx_seen[tx_seen_count++] = packet->data[0];
  kfree_skb(packet); return NETDEV_TX_OK;
}
static struct sk_buff *queued_packet(struct net_device *dev, unsigned value, unsigned queue) {
  struct sk_buff *skb = packet(32, 150, 16); skb->dev = dev; skb->data[0] = value; skb->queue_mapping = queue; return skb;
}
static void transmit_backpressure(void) {
  lx_irq_polling(0);
  static const struct net_device_ops ops = { .ndo_start_xmit = transmit_packet };
  struct net_device *dev = alloc_netdev_mqs(0, "queue%d", NET_NAME_ENUM, ether_setup, 4, 1); assert(dev);
  dev->netdev_ops = &ops; dev->tx_queue_len = 3;
  assert(dev_alloc_name(dev, dev->name) == 0 && !register_netdevice(dev) && !dev_open(dev, NULL));
  netif_carrier_on(dev); netif_stop_subqueue(dev, 0);
  assert(!dev_queue_xmit(queued_packet(dev, 1, 0)) && !dev_queue_xmit(queued_packet(dev, 2, 1)));
  assert(!dev_queue_xmit(queued_packet(dev, 3, 0)) && dev_queue_xmit(queued_packet(dev, 4, 3)) == 1);
  lx_pump(); assert(tx_seen_count == 1 && tx_seen[0] == 2 && dev->tx_backlog == 2);
  assert(lx_next_wait_ms(1000) == 1000);
  netif_wake_subqueue(dev, 0); lx_pump();
  assert(tx_seen_count == 3 && tx_seen[1] == 1 && tx_seen[2] == 3 && qdisc_all_tx_empty(dev));
  assert(!dev_queue_xmit(queued_packet(dev, 9, 2))); lx_pump();
  assert(dev->tx_backlog == 1 && tx_seen_count == 3 && __netif_subqueue_stopped(dev, 2));
  assert(lx_next_wait_ms(1000) == 1000);
  assert(!dev_queue_xmit(queued_packet(dev, 10, 1)) && !dev_queue_xmit(queued_packet(dev, 11, 2)));
  lx_pump(); assert(tx_seen_count == 4 && tx_seen[3] == 10 && dev->tx_backlog == 2);
  netif_wake_subqueue(dev, 2); lx_pump();
  assert(tx_seen_count == 6 && tx_seen[4] == 9 && tx_seen[5] == 11 && !dev->tx_backlog);
  struct sk_buff *over_memory = queued_packet(dev, 12, 0); dev->tx_memory_limit = over_memory->truesize - 1;
  assert(dev_queue_xmit(over_memory) == 1 && !dev->tx_backlog); dev->tx_memory_limit = 512 * 1024;
  netif_carrier_off(dev); assert(!dev_queue_xmit(queued_packet(dev, 12, 0)) && dev->tx_backlog == 1);
  assert(lx_next_wait_ms(1000) == 1000); assert(!dev_close(dev) && !dev->tx_backlog);
  assert(!dev_open(dev, NULL)); netif_carrier_on(dev);
  assert(!dev_queue_xmit(queued_packet(dev, 13, 0)) && dev->dev.references == 2);
  assert(!dev_close(dev) && dev->dev.references == 1 && !dev->tx_backlog);
  struct lx_netdev_tx_stats stats; lx_netdev_tx_stats_get(dev, &stats);
  assert(stats.accepted == 8 && stats.queue_full == 2 && stats.busy == 1 && stats.dispatched == 6 && stats.purged == 2);
  assert(stats.peak_packets == 3 && stats.peak_bytes > 450);
  assert(dev_queue_xmit(queued_packet(dev, 14, 0)) == -ENETDOWN);
  assert(!dev_open(dev, NULL)); dev->priv_flags |= IFF_NO_QUEUE;
  assert(!dev_queue_xmit(queued_packet(dev, 14, 1)) && tx_seen_count == 7 && !dev->tx_backlog && dev->dev.references == 1);
  tx_busy_count = 0;
  assert(!dev_queue_xmit(queued_packet(dev, 9, 2)) && dev->tx_backlog == 1 && tx_seen_count == 7);
  assert(lx_next_wait_ms(1000) == 1000); netif_wake_subqueue(dev, 2); lx_pump(); assert(tx_seen_count == 8 && !dev->tx_backlog);
  assert(!dev_queue_xmit(queued_packet(dev, 15, 1)) && tx_seen_count == 9 && dev->tx_backlog == 1);
  lx_pump(); assert(tx_seen_count == 10 && tx_seen[8] == 15 && tx_seen[9] == 16 && !dev->tx_backlog);
  free_netdev(dev); lx_pump(); assert(tx_seen_count == 10);
}
static void dma_pool_reuse(void) {
  nw_wifi_buffer_pool_configure(16384);
  struct sk_buff *rx = __dev_alloc_skb(4096, GFP_ATOMIC); assert(rx);
  struct page *owner = &skb_shinfo(rx)->owner;
  assert(skb_tailroom(rx) >= 4096 && owner->allocation_size == 8192);
  /* The DMA allocation holds packet bytes only: its descriptor and the shared info are heap memory. */
  assert((u8 *)skb_shinfo(rx) + sizeof(struct skb_shared_info) <= owner->address || (u8 *)skb_shinfo(rx) >= owner->address + owner->allocation_size);
  assert(rx->end == owner->address + owner->allocation_size && owner->length == owner->allocation_size);
  void *rx_head = rx->head; kfree_skb(rx);
  rx = __dev_alloc_skb(4096, GFP_ATOMIC); assert(rx && rx->head == rx_head);
  kfree_skb(rx);
  /* The driver's receive buffer (2048 bytes, 128 of alignment room, 32 reserved) takes one 4096-byte page. */
  rx = __dev_alloc_skb(2048 + 128, GFP_ATOMIC); assert(rx && skb_shinfo(rx)->owner.allocation_size == 4096);
  kfree_skb(rx); nw_wifi_buffer_pool_configure(0);
  puts("DMA RX classes: 4096-byte receive buffer plus headroom fits 8192 bytes, a 2048-byte one fits 4096, metadata stays in heap memory and allocations are reused");
  nw_wifi_buffer_pool_configure(2048); /* room for one cached 1500-byte packet buffer */
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  void *first_head = NULL;
  for (unsigned i = 0; i < 4096; i++) {
    struct sk_buff *packet = alloc_skb(1500, GFP_ATOMIC); assert(packet);
    if (!i) first_head = packet->head;
    assert(packet->head == first_head && virt_to_head_page(first_head));
    assert(skb_shinfo(packet)->dataref == 1 && !skb_shinfo(packet)->nr_frags && !skb_shinfo(packet)->frag_list);
    kfree_skb(packet); assert(!virt_to_head_page(first_head));
  }
  nw_wifi_buffer_stats_get(&after);
  assert(after.dma_allocations - before.dma_allocations == 1 && after.pool_hits - before.pool_hits == 4095);
  assert(after.pool_cached_blocks == 1 && after.pool_cached_bytes == 2048);
  struct sk_buff *packet = alloc_skb(1500, GFP_ATOMIC), *clone = skb_clone(packet, GFP_ATOMIC); assert(packet && clone);
  kfree_skb(packet); nw_wifi_buffer_stats_get(&after); assert(!after.pool_cached_blocks);
  struct sk_buff *other = alloc_skb(1500, GFP_ATOMIC); assert(other && other->head != clone->head);
  kfree_skb(other); kfree_skb(clone);
  nw_wifi_buffer_stats_get(&after); assert(after.pool_cached_blocks == 1 && after.pool_cached_bytes == 2048);
  fail_dma = 1; assert(!alloc_skb(700, GFP_ATOMIC)); fail_dma = 0;
  nw_wifi_buffer_stats_get(&after); assert(!after.pool_cached_blocks && !after.pool_cached_bytes);
  nw_wifi_buffer_pool_configure(16384);
  packet = alloc_skb(100, GFP_ATOMIC); assert(packet); kfree_skb(packet);
  unsigned freed = releases; fail_dma_once = 1;
  packet = alloc_skb(5000, GFP_ATOMIC); assert(packet && releases == freed + 1); kfree_skb(packet);
  nw_wifi_buffer_pool_configure(0); nw_wifi_buffer_stats_get(&after);
  assert(!after.pool_cached_blocks && !after.pool_cached_bytes);
  puts("DMA reuse pool: 4096 sequential 1500-byte packets use 1 physical DMA allocation; clone lifetime, byte cap, pressure trim and shutdown drain pass");
}
static unsigned aborted_measurements;
static void abort_measurement(struct wiphy *wiphy, struct wireless_dev *wdev, struct cfg80211_pmsr_request *request) {
  assert(wdev->wiphy == wiphy && !request->nl_portid); aborted_measurements++;
}
static void mainline_control_lifetimes(void) {
  struct wiphy *wiphy = kzalloc(sizeof(*wiphy), GFP_KERNEL); assert(wiphy);
  struct station_parameters params = { .listen_interval = -1, .support_p2p_ps = -1,
    .sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED), .sta_flags_set = BIT(NL80211_STA_FLAG_AUTHORIZED) };
  assert(!cfg80211_check_station_change(wiphy, &params, CFG80211_STA_AP_STA));
  params.sta_flags_mask |= BIT(NL80211_STA_FLAG_WME);
  assert(cfg80211_check_station_change(wiphy, &params, CFG80211_STA_AP_STA) == -EINVAL);
  params.sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED); params.listen_interval = 3;
  assert(cfg80211_check_station_change(wiphy, &params, CFG80211_STA_AP_STA) == -EINVAL); kfree(wiphy);
  struct cfg80211_coalesce *coalesce = kzalloc(sizeof(*coalesce) + 2 * sizeof(coalesce->rules[0]), GFP_KERNEL); assert(coalesce);
  coalesce->n_rules = 2;
  for (unsigned i = 0; i < 2; i++) {
    coalesce->rules[i].n_patterns = 2;
    coalesce->rules[i].patterns = kzalloc(2 * sizeof(coalesce->rules[i].patterns[0]), GFP_KERNEL); assert(coalesce->rules[i].patterns);
    for (unsigned j = 0; j < 2; j++) { coalesce->rules[i].patterns[j].mask = kmalloc(8, GFP_KERNEL); assert(coalesce->rules[i].patterns[j].mask); }
  }
  cfg80211_free_coalesce(coalesce);
  struct cfg80211_registered_device *rdev = kzalloc(sizeof(*rdev), GFP_KERNEL); assert(rdev);
  static const struct cfg80211_ops ops = { .abort_pmsr = abort_measurement };
  rdev->ops = &ops; mutex_init(&rdev->wiphy.mtx);
  struct wireless_dev wdev = { .wiphy = &rdev->wiphy };
  INIT_LIST_HEAD(&wdev.pmsr_list); spin_lock_init(&wdev.pmsr_lock); INIT_WORK(&wdev.pmsr_free_wk, cfg80211_pmsr_free_wk);
  for (unsigned owner = 0; owner < 2; owner++) {
    struct cfg80211_pmsr_request *request = kzalloc(sizeof(*request), GFP_KERNEL); assert(request);
    request->nl_portid = owner ? 42 : 0; list_add_tail(&request->list, &wdev.pmsr_list);
  }
  schedule_work(&wdev.pmsr_free_wk); lx_pump(); assert(aborted_measurements == 1 && !list_empty(&wdev.pmsr_list));
  cfg80211_release_pmsr(&wdev, 42); lx_pump(); assert(aborted_measurements == 2 && list_empty(&wdev.pmsr_list));
  struct cfg80211_pmsr_request *request = kzalloc(sizeof(*request), GFP_KERNEL); assert(request);
  request->nl_portid = 17; list_add_tail(&request->list, &wdev.pmsr_list);
  wiphy_lock(wdev.wiphy); cfg80211_pmsr_wdev_down(&wdev); wiphy_unlock(wdev.wiphy);
  assert(aborted_measurements == 3 && list_empty(&wdev.pmsr_list)); kfree(rdev);
}
int main(void) {
  lx_plat = &platform;
  clone_and_cow(); expansion_and_failures(); fragments(); flow_hash(); support_semantics(); peer_tables(); metadata_ownership(); network_boundary(); napi_lifetime(); transmit_helpers(); interface_lifetime(); dma_page_fragments(); mainline_amsdu(); transmit_backpressure(); dma_pool_reuse(); mainline_control_lifetimes();
  struct sk_buff *s = packet(32, 1500, 32);
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  for (unsigned i = 0; i < 4096; i++) { struct sk_buff *c = skb_clone(s, 0); assert(c); kfree_skb(c); }
  nw_wifi_buffer_stats_get(&after);
  assert(after.dma_allocations == before.dma_allocations && after.copied_bytes == before.copied_bytes);
  kfree_skb(s); nw_wifi_buffer_stats_get(&after);
  assert(allocations == releases && after.live_dma == 0);
  assert(heap_allocations == heap_releases);
  assert(after.skb_allocations == after.skb_frees && after.dma_allocations == after.dma_frees);
  printf("PASS: DMA ownership, clones, COW, fragments, owner callbacks, batch input, ECN, queues and allocation failures\n");
  printf("4096 clones: 0 payload copies, 0 additional DMA allocations; live DMA: %llu\n", (unsigned long long)after.live_dma);
  return 0;
}
