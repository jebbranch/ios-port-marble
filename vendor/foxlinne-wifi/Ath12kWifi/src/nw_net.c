/* Packet buffers, hash tables, polling contexts and the Michael MIC hash for the WiFi data path. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <linux/jhash.h>
#include <linux/rbtree.h>

static void carrier_set(struct net_device *dev, bool up) {
  if (dev->carrier == up) return;
  dev->carrier = up;
  if (up) lx_netdev_tx_kick(dev);
  if (dev->carrier_changed) dev->carrier_changed(dev, up);
}
void netif_carrier_on(struct net_device *dev) { carrier_set(dev, true); }
void netif_carrier_off(struct net_device *dev) { carrier_set(dev, false); }
bool qdisc_all_tx_empty(const struct net_device *dev) { return __atomic_load_n(&dev->tx_backlog, __ATOMIC_ACQUIRE) == 0; }

/* ---- packet buffers: reference-counted DMA storage and private skb metadata ---- */
static struct nw_buffer_stats buffer_stats;
#define BUFFER_ADD(field, n) __atomic_fetch_add(&buffer_stats.field, (u64)(n), __ATOMIC_RELAXED)
/* A DMA allocation holds packet bytes only (its metadata is a heap object), so
 * the classes are whole powers of two: a 2048-byte receive buffer with its
 * headroom takes one 4096-byte page of the window. */
static const unsigned pool_capacity[] = { 256, 512, 1024, 2048, 4096, 8192 };
static struct page *pool[ARRAY_SIZE(pool_capacity)];
static spinlock_t pool_lock;
static size_t pool_limit;
static int pool_bin(size_t length) {
  for (unsigned i = 0; i < ARRAY_SIZE(pool_capacity); i++) if (length == pool_capacity[i]) return i;
  return -1;
}
static void page_free_allocation(struct page *page) {
  lx_plat->dma_free(page->allocation_size, page->address, page->bus);
  BUFFER_ADD(dma_frees, 1); BUFFER_ADD(live_dma, -1);
  lx_free(container_of(page, struct skb_shared_info, owner));
}
void nw_wifi_buffer_pool_trim(void) {
  struct page *discard = NULL;
  spin_lock(&pool_lock);
  for (unsigned i = 0; i < ARRAY_SIZE(pool); i++) {
    while (pool[i]) {
      struct page *page = pool[i]; pool[i] = page->pool_next;
      BUFFER_ADD(pool_cached_bytes, -page->allocation_size); BUFFER_ADD(pool_cached_blocks, -1);
      page->pool_next = discard; discard = page;
    }
  }
  spin_unlock(&pool_lock);
  while (discard) { struct page *next = discard->pool_next; page_free_allocation(discard); discard = next; }
}
void nw_wifi_buffer_pool_configure(size_t maximum_bytes) {
  spin_lock(&pool_lock); pool_limit = maximum_bytes;
  bool trim = buffer_stats.pool_cached_bytes > maximum_bytes; spin_unlock(&pool_lock);
  if (trim) nw_wifi_buffer_pool_trim();
}
static bool page_cache(struct page *page) {
  int bin = pool_bin(page->length); if (bin < 0) return false;
  bool cached = false;
  spin_lock(&pool_lock);
  if (page->allocation_size <= pool_limit && buffer_stats.pool_cached_bytes <= pool_limit - page->allocation_size) {
    page->pool_next = pool[bin]; pool[bin] = page;
    BUFFER_ADD(pool_cached_bytes, page->allocation_size); BUFFER_ADD(pool_cached_blocks, 1); cached = true;
  }
  spin_unlock(&pool_lock); return cached;
}
static struct rb_root pages = RB_ROOT;
static int pages_lock;
static void page_tree_lock(void) { while (__atomic_exchange_n(&pages_lock, 1, __ATOMIC_ACQUIRE)) cpu_relax(); }
static void page_tree_unlock(void) { __atomic_store_n(&pages_lock, 0, __ATOMIC_RELEASE); }
static void page_register(struct page *page) {
  page_tree_lock();
  struct rb_node **slot = &pages.rb_node, *parent = NULL;
  while (*slot) {
    struct page *other = rb_entry(*slot, struct page, node); parent = *slot;
    slot = (uintptr_t)page->address < (uintptr_t)other->address ? &parent->rb_left : &parent->rb_right;
  }
  rb_link_node(&page->node, parent, slot); rb_insert_color(&page->node, &pages);
  page_tree_unlock();
}
struct page *virt_to_head_page(const void *address) {
  uintptr_t value = (uintptr_t)address; struct page *result = NULL;
  page_tree_lock();
  struct rb_node *node = pages.rb_node;
  while (node) {
    struct page *page = rb_entry(node, struct page, node); uintptr_t start = (uintptr_t)page->address;
    if (value < start) node = node->rb_left;
    else if (value - start >= page->allocation_size) node = node->rb_right;
    else { result = page; break; }
  }
  page_tree_unlock(); return result;
}
void get_page(struct page *page) { BUG_ON(!page); __atomic_fetch_add(&page->references, 1u, __ATOMIC_RELAXED); }
void put_page(struct page *page) {
  if (!page || __atomic_fetch_sub(&page->references, 1u, __ATOMIC_ACQ_REL) != 1) return;
  page_tree_lock(); rb_erase(&page->node, &pages); page_tree_unlock();
  if (!page_cache(page)) page_free_allocation(page);
}
void nw_wifi_buffer_stats_get(struct nw_buffer_stats *out) {
#define READ_COUNTER(field) out->field = __atomic_load_n(&buffer_stats.field, __ATOMIC_RELAXED)
  READ_COUNTER(skb_allocations); READ_COUNTER(skb_frees);
  READ_COUNTER(dma_allocations); READ_COUNTER(dma_frees);
  READ_COUNTER(live_dma); READ_COUNTER(peak_dma); READ_COUNTER(clones);
  READ_COUNTER(cow); READ_COUNTER(expansions); READ_COUNTER(copied_bytes);
  READ_COUNTER(allocation_failures);
  READ_COUNTER(pool_hits); READ_COUNTER(pool_misses);
  READ_COUNTER(pool_cached_bytes); READ_COUNTER(pool_cached_blocks);
#undef READ_COUNTER
}
/* Returns the buffer's shared info (heap) and its data range in the DMA window. */
static struct skb_shared_info *buffer_alloc(unsigned int size, u8 **head, u8 **end) {
  const unsigned int align = __alignof__(struct skb_shared_info);
  if (size > UINT_MAX - sizeof(struct skb_shared_info) - sizeof(struct sk_buff) - (align - 1)) { BUFFER_ADD(allocation_failures, 1); return 0; }
  size = ALIGN(size, align);
  u64 bus = 0; u8 *b = NULL; struct skb_shared_info *sh = NULL;
  spin_lock(&pool_lock);
  if (pool_limit) {
    for (unsigned i = 0; i < ARRAY_SIZE(pool_capacity); i++) if (size <= pool_capacity[i]) {
      size = pool_capacity[i];
      struct page *page = pool[i];
      if (page) {
        pool[i] = page->pool_next; b = page->address; bus = page->bus;
        sh = container_of(page, struct skb_shared_info, owner);
        BUFFER_ADD(pool_cached_bytes, -page->allocation_size); BUFFER_ADD(pool_cached_blocks, -1);
        BUFFER_ADD(pool_hits, 1);
      } else BUFFER_ADD(pool_misses, 1);
      break;
    }
  }
  spin_unlock(&pool_lock);
  bool reused = sh != NULL;
  if (!sh) {
    sh = lx_alloc(sizeof(*sh), 0);
    if (!sh) { BUFFER_ADD(allocation_failures, 1); return 0; }
    b = lx_plat->dma_alloc(size, &bus);
    if (!b) {
      /* Cached blocks in another size class must not prevent live allocations. */
      nw_wifi_buffer_pool_trim();
      b = lx_plat->dma_alloc(size, &bus);
    }
    if (!b) { lx_free(sh); BUFFER_ADD(allocation_failures, 1); return 0; }
  }
  memset(sh, 0, offsetof(struct skb_shared_info, frags)); sh->dataref = 1;
  sh->owner.address = b; sh->owner.length = size; sh->owner.allocation_size = size;
  sh->owner.bus = bus; sh->owner.references = 1; page_register(&sh->owner);
  if (!reused) {
    BUFFER_ADD(dma_allocations, 1);
    u64 live = BUFFER_ADD(live_dma, 1) + 1;
    u64 peak = __atomic_load_n(&buffer_stats.peak_dma, __ATOMIC_RELAXED);
    while (live > peak && !__atomic_compare_exchange_n(&buffer_stats.peak_dma, &peak, live, 1,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
  }
  *head = b; *end = b + size; return sh;
}
static void buffer_release(struct skb_shared_info *sh) {
  if (__atomic_fetch_sub(&sh->dataref, 1u, __ATOMIC_ACQ_REL) != 1) return;
  for (unsigned i = 0; i < sh->nr_frags; i++) put_page(sh->frags[i].page);
  struct sk_buff *frag = sh->frag_list;
  while (frag) { struct sk_buff *next = frag->next; kfree_skb(frag); frag = next; }
  put_page(&sh->owner);
}
struct sk_buff *alloc_skb(unsigned int size, gfp_t f) {
  (void)f; struct sk_buff *s = lx_alloc(sizeof *s, 1);
  if (!s) { BUFFER_ADD(allocation_failures, 1); return 0; }
  u8 *b; s->shared = buffer_alloc(size, &b, &s->end);
  if (!s->shared) { lx_free(s); return 0; }
  s->head = s->data = s->tail = b; s->truesize = (u32)(s->end - b + sizeof(*s) + sizeof(struct skb_shared_info));
  s->users = 1; s->mac_header = (u16)~0u; s->head_frag = true;
  BUFFER_ADD(skb_allocations, 1); return s;
}
struct sk_buff *__dev_alloc_skb(unsigned int size, gfp_t f) { struct sk_buff *s = alloc_skb(size + 32, f); if (s) skb_reserve(s, 32); return s; }
void kfree_skb(struct sk_buff *s) {
  if (!s) return;
  if (__atomic_fetch_sub(&s->users, 1u, __ATOMIC_ACQ_REL) != 1) return;
  skb_orphan(s);
  buffer_release(s->shared); lx_free(s); BUFFER_ADD(skb_frees, 1);
}
void kfree_skb_list(struct sk_buff *s) {
  while (s) { struct sk_buff *next = s->next; kfree_skb(s); s = next; }
}
void skb_complete_wifi_ack(struct sk_buff *s, bool acked) {
  s->wifi_acked_valid = true; s->wifi_acked = acked; kfree_skb(s);
}
static void buffer_copy(void *dst, const void *src, size_t n) {
  memcpy(dst, src, n); BUFFER_ADD(copied_bytes, n);
}
static bool valid_fragment(const skb_frag_t *frag) {
  return frag->page && frag->offset <= frag->page->length && frag->size <= frag->page->length - frag->offset;
}
void skb_add_rx_frag(struct sk_buff *skb, int index, struct page *page, int offset, int size, unsigned truesize) {
  BUG_ON(index < 0 || index >= MAX_SKB_FRAGS || index != skb_shinfo(skb)->nr_frags || offset < 0 || size < 0);
  skb_frag_t frag = { .page = page, .offset = (unsigned)offset, .size = (unsigned)size };
  BUG_ON(!valid_fragment(&frag) || (unsigned)size > UINT_MAX - skb->len);
  skb_shinfo(skb)->frags[index] = frag; skb_shinfo(skb)->nr_frags++;
  skb->len += (unsigned)size; skb->data_len += (unsigned)size; skb->truesize += truesize;
}
/* Both page fragments and chained skb payloads are flattened only for a write. */
static int copy_packet(const struct sk_buff *s, u8 *dst, unsigned int depth) {
  if (depth > 16 || skb_shinfo(s)->nr_frags > MAX_SKB_FRAGS || s->data_len > s->len) return -EINVAL;
  unsigned int linear = s->len - s->data_len;
  if ((size_t)(s->tail - s->data) != linear) return -EINVAL;
  buffer_copy(dst, s->data, linear);
  unsigned int done = linear;
  for (unsigned i = 0; i < skb_shinfo(s)->nr_frags; i++) {
    const skb_frag_t *frag = &skb_shinfo(s)->frags[i];
    if (!valid_fragment(frag) || frag->size > s->len - done) return -EINVAL;
    buffer_copy(dst + done, skb_frag_address(frag), frag->size); done += frag->size;
  }
  for (const struct sk_buff *frag = skb_shinfo(s)->frag_list; frag; frag = frag->next) {
    if (frag->len > s->len - done) return -EINVAL;
    int r = copy_packet(frag, dst + done, depth + 1); if (r) return r;
    done += frag->len;
  }
  return done == s->len ? 0 : -EINVAL;
}
static int copy_layout(const struct sk_buff *s, unsigned int h, unsigned int t, struct sk_buff *n) {
  if ((u64)h + s->len + t > (u32)~0u) return -ENOMEM;
  *n = *s;
  u8 *b; struct skb_shared_info *sh = buffer_alloc(h + s->len + t, &b, &n->end); if (!sh) return -ENOMEM;
  const unsigned int old_h = skb_headroom(s), keep = h < old_h ? h : old_h;
  buffer_copy(b + h - keep, s->data - keep, keep);
  int r = copy_packet(s, b + h, 0);
  if (r) { buffer_release(sh); return r; }
  sh->gso_size = skb_shinfo(s)->gso_size; sh->hwtstamps = skb_shinfo(s)->hwtstamps;
  n->shared = sh;
  n->head = b; n->data = b + h; n->tail = n->data + s->len;
  n->data_len = 0; n->truesize = (u32)(n->end - b + sizeof(*n) + sizeof(struct skb_shared_info)); n->cloned = 0; n->head_frag = true;
  n->network_header = s->network_header + h - old_h;
  n->transport_header = s->transport_header + h - old_h;
  if (s->encapsulation) n->inner_transport_header = s->inner_transport_header + h - old_h;
  if (s->ip_summed == CHECKSUM_PARTIAL) n->csum_start = s->csum_start + h - old_h;
  if (skb_mac_header_was_set(s)) n->mac_header = s->mac_header + h - old_h;
  return 0;
}
struct sk_buff *skb_copy(const struct sk_buff *s, gfp_t f) {
  return skb_copy_expand(s, skb_headroom(s), skb_tailroom(s), f);
}
struct sk_buff *skb_clone(struct sk_buff *s, gfp_t f) {
  (void)f; struct sk_buff *n = lx_alloc(sizeof *n, 0);
  if (!n) { BUFFER_ADD(allocation_failures, 1); return 0; }
  *n = *s; n->next = n->prev = 0; n->users = 1;
  n->sk = NULL; n->destructor = NULL; n->wifi_acked_valid = n->wifi_acked = false;
  __atomic_fetch_add(&skb_shinfo(s)->dataref, 1u, __ATOMIC_RELAXED);
  s->cloned = n->cloned = 1;
  BUFFER_ADD(skb_allocations, 1); BUFFER_ADD(clones, 1); return n;
}
struct sk_buff *skb_copy_expand(const struct sk_buff *s, int h, int t, gfp_t f) {
  (void)f; if (h < 0 || t < 0) return 0;
  struct sk_buff *n = lx_alloc(sizeof *n, 0);
  if (!n) { BUFFER_ADD(allocation_failures, 1); return 0; }
  if (copy_layout(s, h, t, n)) { lx_free(n); return 0; }
  n->next = n->prev = 0; n->users = 1; n->sk = NULL; n->destructor = NULL;
  n->wifi_acked_valid = n->wifi_acked = false;
  BUFFER_ADD(skb_allocations, 1); return n;
}
static void socket_clone_release(struct sk_buff *s) {
  struct sock *sk = s->sk;
  if (sk->wifi_ack) sk->wifi_ack(sk, s->wifi_acked_valid, s->wifi_acked);
  sk->put(sk);
}
struct sk_buff *skb_clone_sk(struct sk_buff *s) {
  struct sock *sk = s->sk;
  if (!sk || !sk->try_hold || !sk->put || !sk->try_hold(sk)) return NULL;
  struct sk_buff *clone = skb_clone(s, GFP_ATOMIC);
  if (!clone) { sk->put(sk); return NULL; }
  clone->sk = sk; clone->destructor = socket_clone_release; return clone;
}
struct sk_buff *skb_share_check(struct sk_buff *s, gfp_t flags) {
  if (!skb_shared(s)) return s;
  struct sk_buff *clone = skb_clone(s, flags); kfree_skb(s); return clone;
}
int pskb_expand_head(struct sk_buff *s, int a, int b, gfp_t f) {
  (void)f; if (a < 0 || b < 0) return -EINVAL;
  const u64 h = (u64)skb_headroom(s) + a, t = (u64)skb_tailroom(s) + b;
  if (h > (u32)~0u || t > (u32)~0u) return -ENOMEM;
  const int shared = skb_cloned(s);
  struct sk_buff next; int r = copy_layout(s, h, t, &next); if (r) return r;
  buffer_release(s->shared); *s = next;
  BUFFER_ADD(expansions, 1); if (shared) BUFFER_ADD(cow, 1); return 0;
}
int skb_linearize(struct sk_buff *s) { return s->data_len ? pskb_expand_head(s, 0, 0, 0) : 0; }
bool pskb_may_pull(struct sk_buff *s, unsigned len) {
  if (len <= skb_headlen(s)) return true;
  return len <= s->len && !skb_linearize(s);
}
int __pskb_trim(struct sk_buff *s, unsigned len) {
  if (len >= s->len) return 0;
  if (s->data_len) { int result = skb_linearize(s); if (result) return result; }
  skb_trim(s, len); return 0;
}
int skb_cow_head(struct sk_buff *s, unsigned int h) {
  unsigned int have = skb_headroom(s);
  if (h > 0x7fffffffU) return -ENOMEM;
  return skb_cloned(s) || h > have ? pskb_expand_head(s, h > have ? h - have : 0, 0, 0) : 0;
}
int skb_ensure_writable(struct sk_buff *s, int n) {
  if (n < 0 || (unsigned int)n > s->len) return -EINVAL;
  return skb_cloned(s) || s->data_len ? pskb_expand_head(s, 0, 0, 0) : 0;
}
struct sk_buff *skb_realloc_headroom(struct sk_buff *s, unsigned int h) { return skb_copy_expand(s, h, skb_tailroom(s), 0); }
int skb_cow_data(struct sk_buff *s, int tail, struct sk_buff **trailer) {
  if (tail < 0) return -EINVAL;
  unsigned int have = skb_tailroom(s);
  if (skb_cloned(s) || s->data_len || (unsigned int)tail > have) {
    int r = pskb_expand_head(s, 0, (unsigned int)tail > have ? (unsigned int)tail - have : 0, 0);
    if (r) return r;
  }
  *trailer = s; return 1;
}
void *pskb_put(struct sk_buff *s, struct sk_buff *tail, int n) { (void)tail; return skb_put(s, n); }

static int copy_bits(const struct sk_buff *s, unsigned offset, u8 *to, unsigned length, unsigned depth) {
  if (depth > 16 || s->data_len > s->len || offset > s->len || length > s->len - offset || skb_shinfo(s)->nr_frags > MAX_SKB_FRAGS)
    return -EFAULT;
  unsigned linear = s->len - s->data_len;
  if ((size_t)(s->tail - s->data) != linear) return -EFAULT;
  if (offset < linear) {
    unsigned n = min(length, linear - offset); memcpy(to, s->data + offset, n);
    to += n; length -= n; offset = 0;
  } else offset -= linear;
  for (unsigned i = 0; length && i < skb_shinfo(s)->nr_frags; i++) {
    const skb_frag_t *frag = &skb_shinfo(s)->frags[i];
    if (!valid_fragment(frag)) return -EFAULT;
    if (offset >= frag->size) { offset -= frag->size; continue; }
    unsigned n = min(length, frag->size - offset);
    memcpy(to, (u8 *)skb_frag_address(frag) + offset, n); to += n; length -= n; offset = 0;
  }
  for (const struct sk_buff *f = skb_shinfo(s)->frag_list; length && f; f = f->next) {
    if (offset >= f->len) { offset -= f->len; continue; }
    unsigned n = min(length, f->len - offset);
    int r = copy_bits(f, offset, to, n, depth + 1); if (r) return r;
    to += n; length -= n; offset = 0;
  }
  return length ? -EFAULT : 0;
}
int skb_copy_bits(const struct sk_buff *s, int offset, void *to, int length) {
  if (length < 0 || (s64)offset + length > s->len) return -EFAULT;
  unsigned requested = (unsigned)length;
  if (offset < 0) {
    unsigned before = (unsigned)(-(s64)offset);
    if (before > skb_headroom(s)) return -EFAULT;
    unsigned n = min((unsigned)length, before);
    memcpy(to, s->data + offset, n); to = (u8 *)to + n;
    length -= n; offset += n;
    if (!length) { BUFFER_ADD(copied_bytes, requested); return 0; }
  }
  int result = copy_bits(s, offset, to, length, 0);
  if (!result) BUFFER_ADD(copied_bytes, requested);
  return result;
}
/* Hash network addresses/protocol/ports, never payload bytes: packets of one
 * flow retain their queue as payload and packet length change. */
u32 skb_get_hash(struct sk_buff *s) {
  if (s->hash) return s->hash;
  u8 eth[18], ip[40], key[40]; unsigned count = 0, offset = 14;
  if (skb_copy_bits(s, 0, eth, 14)) return 0;
  u16 protocol = get_unaligned_be16(eth + 12);
  for (unsigned tags = 0; tags < 2 && (protocol == 0x8100 || protocol == 0x88a8); tags++) {
    u8 tag[4]; if (skb_copy_bits(s, offset, tag, 4)) return 0;
    protocol = get_unaligned_be16(tag + 2); offset += 4;
  }
  u8 transport = 0; unsigned ports = 0;
  if (protocol == ETH_P_IP) {
    if (skb_copy_bits(s, offset, ip, 20) || ip[0] >> 4 != 4 || (ip[0] & 15) < 5) return 0;
    memcpy(key, ip + 12, 8); count = 8; transport = ip[9];
    if (!(get_unaligned_be16(ip + 6) & 0x3fff)) ports = offset + (ip[0] & 15) * 4;
  } else if (protocol == ETH_P_IPV6) {
    if (skb_copy_bits(s, offset, ip, 40) || ip[0] >> 4 != 6) return 0;
    memcpy(key, ip + 8, 32); count = 32; transport = ip[6]; ports = offset + 40;
    /* Extension-header flows use address/protocol identity until the full
     * flow dissector is wired in; this preserves packet ordering. */
    if (transport != 6 && transport != 17 && transport != 132) ports = 0;
  } else { memcpy(key, eth, 14); count = 14; }
  key[count++] = protocol >> 8; key[count++] = protocol; key[count++] = transport;
  if (ports && (transport == 6 || transport == 17 || transport == 132)) {
    if (skb_copy_bits(s, ports, key + count, 4)) return 0;
    count += 4;
  }
  static u32 seed; if (!seed) seed = lx_random_u32() | 1;
  s->hash = jhash(key, count, seed); if (!s->hash) s->hash = 1;
  return s->hash;
}

/* ---- rhashtable: a single chain; peer tables are small ---- */
int rhashtable_init(struct rhashtable *t, const struct rhashtable_params *p) { t->head = 0; t->p = *p; t->n = 0; return 0; }
void rhashtable_destroy(struct rhashtable *t) { t->head = 0; t->n = 0; }
static void *key_of(const struct rhashtable *t, const struct rhash_head *h, struct rhashtable_params p) {
  (void)t; return (u8 *)h - p.head_offset + p.key_offset; }
void *rhashtable_lookup_fast(struct rhashtable *t, const void *key, struct rhashtable_params p) {
  for (struct rhash_head *h = t->head; h; h = h->next) if (!memcmp(key_of(t, h, p), key, p.key_len)) return (u8 *)h - p.head_offset;
  return 0; }
void *rhashtable_lookup_get_insert_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p) {
  void *o = rhashtable_lookup_fast(t, key_of(t, obj, p), p); if (o) return o;
  obj->next = t->head; t->head = obj; t->n++; return 0; }
int rhashtable_insert_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p) {
  return rhashtable_lookup_get_insert_fast(t, obj, p) ? -EEXIST : 0; }
int rhashtable_remove_fast(struct rhashtable *t, struct rhash_head *obj, struct rhashtable_params p) {
  (void)p; for (struct rhash_head **pp = &t->head; *pp; pp = &(*pp)->next) if (*pp == obj) { *pp = obj->next; t->n--; return 0; }
  return -ENOENT; }

/* Duplicate keys share one group; separate groups keep their independent
 * table chain. This is used for stations on distinct virtual interfaces. */
int rhltable_init(struct rhltable *table, const struct rhashtable_params *params) {
  return rhashtable_init(&table->ht, params);
}
void rhltable_destroy(struct rhltable *table) { rhashtable_destroy(&table->ht); }
struct rhlist_head *rhltable_lookup(struct rhltable *table, const void *key, struct rhashtable_params params) {
  for (struct rhash_head *head = table->ht.head; head; head = head->next)
    if (!memcmp(key_of(&table->ht, head, params), key, params.key_len)) return (struct rhlist_head *)head;
  return 0;
}
int rhltable_insert(struct rhltable *table, struct rhlist_head *object, struct rhashtable_params params) {
  const void *key = key_of(&table->ht, &object->rhead, params);
  for (struct rhash_head **link = &table->ht.head; *link; link = &(*link)->next) {
    struct rhlist_head *group = (struct rhlist_head *)*link;
    if (memcmp(key_of(&table->ht, *link, params), key, params.key_len)) continue;
    for (struct rhlist_head *item = group; item; item = item->next) if (item == object) return -EEXIST;
    object->rhead.next = group->rhead.next; object->next = group;
    *link = &object->rhead; table->ht.n++; return 0;
  }
  object->next = 0; object->rhead.next = table->ht.head;
  table->ht.head = &object->rhead; table->ht.n++; return 0;
}
int rhltable_remove(struct rhltable *table, struct rhlist_head *object, struct rhashtable_params params) {
  (void)params;
  for (struct rhash_head **link = &table->ht.head; *link; link = &(*link)->next) {
    struct rhlist_head *group = (struct rhlist_head *)*link;
    if (group == object) {
      if (group->next) { group->next->rhead.next = group->rhead.next; *link = &group->next->rhead; }
      else *link = group->rhead.next;
      table->ht.n--; return 0;
    }
    for (struct rhlist_head *previous = group; previous->next; previous = previous->next) {
      if (previous->next != object) continue;
      previous->next = object->next;
      table->ht.n--; return 0;
    }
  }
  return -ENOENT;
}

/* ---- polling contexts run from the dispatcher ---- */
static struct napi_struct *g_napi[16];
static void napi_run(struct work_struct *w);
static struct work_struct g_napi_work;
static unsigned g_napi_cursor;
void lx_napi_add(struct net_device *d, struct napi_struct *n, int (*poll)(struct napi_struct *, int), int weight) {
  n->dev = d; n->poll = poll; n->weight = max(weight, 1);
  n->scheduled = n->enabled = n->polling = 0; INIT_LIST_HEAD(&n->rx_list);
  for (int i = 0; i < 16; i++) if (!g_napi[i] || g_napi[i] == n) {
    g_napi[i] = n;
    if (!g_napi_work.func) INIT_WORK(&g_napi_work, napi_run);
    return;
  }
  lx_bug(__FILE__, __LINE__);
}
bool lx_napi_schedule_prep(struct napi_struct *n) {
  return __atomic_load_n(&n->enabled, __ATOMIC_ACQUIRE) &&
         !__atomic_exchange_n(&n->scheduled, 1, __ATOMIC_ACQ_REL);
}
void lx_napi_schedule(struct napi_struct *n) {
  if (!__atomic_load_n(&n->enabled, __ATOMIC_ACQUIRE)) return;
  __atomic_store_n(&n->scheduled, 1, __ATOMIC_RELEASE); queue_work(system_bh_wq, &g_napi_work);
}
bool lx_napi_complete_done(struct napi_struct *n, int done) {
  (void)done; __atomic_store_n(&n->scheduled, 0, __ATOMIC_RELEASE);
  return __atomic_load_n(&n->enabled, __ATOMIC_ACQUIRE) != 0;
}
void lx_napi_enable(struct napi_struct *n) { __atomic_store_n(&n->enabled, 1, __ATOMIC_RELEASE); }
void lx_napi_synchronize(struct napi_struct *n) {
  while (__atomic_load_n(&n->polling, __ATOMIC_ACQUIRE)) lx_idle(1);
}
void lx_napi_disable(struct napi_struct *n) {
  __atomic_store_n(&n->enabled, 0, __ATOMIC_RELEASE);
  lx_napi_synchronize(n); __atomic_store_n(&n->scheduled, 0, __ATOMIC_RELEASE);
}
void lx_napi_del(struct napi_struct *n) {
  lx_napi_disable(n);
  for (int i = 0; i < 16; i++) if (g_napi[i] == n) g_napi[i] = NULL;
  while (!list_empty(&n->rx_list)) {
    struct sk_buff *skb = list_first_entry(&n->rx_list, struct sk_buff, list);
    skb_list_del_init(skb); kfree_skb(skb);
  }
}
/* XNU receives normal packets in batches; no Linux GRO merge is advertised. */
gro_result_t napi_gro_receive(struct napi_struct *n, struct sk_buff *skb) {
  list_add_tail(&skb->list, &n->rx_list); return GRO_NORMAL;
}
static void napi_run(struct work_struct *w) {
  (void)w; unsigned budget = 256, start = g_napi_cursor;
  u64 deadline = lx_now_ms() + 2;
  for (unsigned offset = 0; offset < 16 && budget && lx_now_ms() < deadline; offset++) {
    unsigned i = (start + offset) % 16; struct napi_struct *n = g_napi[i];
    g_napi_cursor = (i + 1) % 16;
    if (!n || !__atomic_load_n(&n->enabled, __ATOMIC_ACQUIRE) || !__atomic_load_n(&n->scheduled, __ATOMIC_ACQUIRE)) continue;
    __atomic_store_n(&n->polling, 1, __ATOMIC_RELEASE);
    int weight = min((unsigned)n->weight, budget), done = n->poll(n, weight);
    netif_receive_skb_list(&n->rx_list);
    __atomic_store_n(&n->polling, 0, __ATOMIC_RELEASE);
    if (done > 0) budget -= min((unsigned)done, (unsigned)weight);
  }
  for (unsigned i = 0; i < 16; i++) if (g_napi[i] && g_napi[i]->enabled && g_napi[i]->scheduled) {
    queue_work(system_bh_wq, &g_napi_work); break;
  }
}

/* ---- Michael MIC (the TKIP integrity hash) ---- */
struct crypto_shash *crypto_alloc_shash(const char *alg, u32 type, u32 mask) {
  (void)type; (void)mask; if (strcmp(alg, "michael_mic")) return (struct crypto_shash *)ERR_PTR(-ENOENT);
  return lx_alloc(sizeof(struct crypto_shash) + 16, 1); }
void crypto_free_shash(struct crypto_shash *t) { lx_free(t); }
struct mm { u32 l, r; u32 key[2]; u32 pend, pendlen; };
static u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
static u32 ror(u32 v, int n) { return (v >> n) | (v << (32 - n)); }
static u32 xswap(u32 v) { return ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); }
static void mix(struct mm *m) {
  m->r ^= rol(m->l, 17); m->l += m->r; m->r ^= xswap(m->l); m->l += m->r; m->r ^= rol(m->l, 3); m->l += m->r; m->r ^= ror(m->l, 2); m->l += m->r; }
int crypto_shash_setkey(struct crypto_shash *t, const u8 *key, unsigned int len) {
  if (len != 8) return -EINVAL; memcpy((u8 *)t + sizeof *t, key, 8); return 0; }
int crypto_shash_init(struct shash_desc *d) {
  struct mm *m = (struct mm *)d->ctx; const u8 *k = (u8 *)d->tfm + sizeof *d->tfm;
  memcpy(m->key, k, 8); m->l = m->key[0]; m->r = m->key[1]; m->pend = 0; m->pendlen = 0; return 0; }
int crypto_shash_update(struct shash_desc *d, const u8 *data, unsigned int len) {
  struct mm *m = (struct mm *)d->ctx;
  while (len--) { m->pend |= (u32)*data++ << (8 * m->pendlen); if (++m->pendlen == 4) { m->l ^= m->pend; mix(m); m->pend = 0; m->pendlen = 0; } }
  return 0; }
int crypto_shash_final(struct shash_desc *d, u8 *out) {
  struct mm *m = (struct mm *)d->ctx; static const u8 pad[5] = { 0x5a, 0, 0, 0, 0 };
  crypto_shash_update(d, pad, 1); while (m->pendlen) crypto_shash_update(d, pad + 1, 1);
  for (int i = 0; i < 4; i++) crypto_shash_update(d, pad + 1, 1); memcpy(out, &m->l, 4); memcpy(out + 4, &m->r, 4); return 0; }
