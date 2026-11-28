/* Network stack boundary: packet ownership, counters and per-queue backpressure. */
#include "lx_pci.h"
#include <net/net_namespace.h>
struct net init_net;
static LIST_HEAD(pernet_operations);
int register_pernet_subsys(struct pernet_operations *operations) {
  if (operations->registered) return -EEXIST;
  int result = operations->init ? operations->init(&init_net) : 0;
  if (result) return result;
  INIT_LIST_HEAD(&operations->list); list_add_tail(&operations->list, &pernet_operations);
  operations->registered = true; return 0;
}
void unregister_pernet_subsys(struct pernet_operations *operations) {
  if (!operations->registered) return;
  list_del_init(&operations->list); operations->registered = false;
  if (operations->exit) operations->exit(&init_net);
}

/* Control operations run on the wireless dispatcher under the wiphy lock. */
static struct net_device *devices[16];
/* All queue operations execute on the wireless dispatcher. Packet links are
 * reused, and queue storage is part of the net_device allocation. */
struct lx_netdev_tx {
  struct work_struct work;
  struct net_device *dev;
  struct lx_netdev_tx_stats stats;
  size_t memory;
  unsigned long active_queues;
  unsigned cursor;
  struct sk_buff_head queues[];
};
static void tx_worker(struct work_struct *work);
static void tx_purge(struct net_device *dev) {
  if (!dev->tx) return;
  for (unsigned i = 0; i < dev->num_tx_queues; i++) {
    struct sk_buff *packet;
    while ((packet = __skb_dequeue(&dev->tx->queues[i]))) {
      dev->tx_backlog--; dev->tx->memory -= packet->truesize;
      dev->tx->stats.purged++; dev->stats.tx_dropped++; kfree_skb(packet);
    }
  }
}
void lx_netdev_tx_kick(struct net_device *dev) {
  if (!dev->tx || !netif_running(dev) || !dev->carrier || !dev->tx_backlog) return;
  bool runnable = false;
  for (unsigned i = 0; i < dev->num_tx_queues; i++)
    if (!skb_queue_empty(&dev->tx->queues[i]) && !__netif_subqueue_stopped(dev, i) && !(dev->tx->active_queues & BIT(i))) { runnable = true; break; }
  if (!runnable) return;
  dev_hold(dev);
  if (!queue_work(system_bh_wq, &dev->tx->work)) dev_put(dev);
}
static int tx_next_queue(struct net_device *dev) {
  for (unsigned attempt = 0; attempt < dev->num_tx_queues; attempt++) {
    unsigned index = dev->tx->cursor++ % dev->num_tx_queues;
    if (!skb_queue_empty(&dev->tx->queues[index]) && !__netif_subqueue_stopped(dev, index) && !(dev->tx->active_queues & BIT(index))) return (int)index;
  }
  return -1;
}
static void tx_worker(struct work_struct *work) {
  struct lx_netdev_tx *tx = container_of(work, struct lx_netdev_tx, work);
  struct net_device *dev = tx->dev;
  u64 started = lx_now_ns();
  if (!netif_running(dev)) { tx_purge(dev); dev_put(dev); return; }
  for (unsigned budget = 0; dev->carrier && budget < 64; budget++) {
    int queue = tx_next_queue(dev); if (queue < 0) break;
    struct sk_buff *packet = __skb_dequeue(&tx->queues[queue]);
    unsigned cost = packet->truesize;
    tx->active_queues |= BIT(queue);
    int result = dev->netdev_ops->ndo_start_xmit(packet, dev);
    tx->active_queues &= ~BIT(queue);
    /* An OK callback consumes the packet and may free it immediately. */
    if (result == NETDEV_TX_BUSY) {
      __skb_queue_head(&tx->queues[queue], packet);
      tx->stats.busy++; netif_stop_subqueue(dev, (u16)queue);
    } else {
      BUG_ON(result != NETDEV_TX_OK); tx->stats.dispatched++;
      dev->tx_backlog--; tx->memory -= cost;
    }
    if (lx_now_ns() - started >= 2000000ULL) break;
  }
  if (dev->carrier && tx_next_queue(dev) >= 0) lx_netdev_tx_kick(dev);
  dev_put(dev);
}
int dev_queue_xmit(struct sk_buff *packet) {
  struct net_device *dev = packet->dev;
  if (!dev || !dev->tx || !dev->registered || !netif_running(dev) ||
      !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) {
    if (dev) dev->stats.tx_dropped++;
    kfree_skb(packet); return -ENETDOWN;
  }
  struct lx_netdev_tx *tx = dev->tx;
  unsigned queue = dev->netdev_ops->ndo_select_queue ?
      dev->netdev_ops->ndo_select_queue(dev, packet, NULL) : packet->queue_mapping;
  unsigned usable = dev->real_num_tx_queues;
  if (!usable || usable > dev->num_tx_queues) usable = dev->num_tx_queues;
  if (queue >= usable) queue = 0;
  packet->queue_mapping = queue;
  /* mac80211 owns its FQ/AQL queues. Honor IFF_NO_QUEUE so ordinary packets
   * reach that scheduler without a second software queue or work dispatch. */
  if ((dev->priv_flags & IFF_NO_QUEUE) && dev->carrier && skb_queue_empty(&tx->queues[queue]) &&
      !__netif_subqueue_stopped(dev, queue) && !(tx->active_queues & BIT(queue))) {
    tx->active_queues |= BIT(queue);
    int result = dev->netdev_ops->ndo_start_xmit(packet, dev);
    tx->active_queues &= ~BIT(queue);
    if (result == NETDEV_TX_OK) {
      tx->stats.accepted++; tx->stats.dispatched++; lx_netdev_tx_kick(dev); return 0;
    }
    BUG_ON(result != NETDEV_TX_BUSY);
    tx->stats.busy++; netif_stop_subqueue(dev, queue);
    /* BUSY preserves ownership; continue into the bounded fallback queue. */
  }
  if (dev->tx_backlog >= dev->tx_queue_len || packet->truesize > dev->tx_memory_limit ||
      tx->memory > dev->tx_memory_limit - packet->truesize) {
    tx->stats.queue_full++; dev->stats.tx_dropped++; kfree_skb(packet); return 1; /* NET_XMIT_DROP */
  }
  __skb_queue_tail(&tx->queues[queue], packet); dev->tx_backlog++; tx->memory += packet->truesize; tx->stats.accepted++;
  if (dev->tx_backlog > tx->stats.peak_packets) tx->stats.peak_packets = dev->tx_backlog;
  if (tx->memory > tx->stats.peak_bytes) tx->stats.peak_bytes = tx->memory;
  lx_netdev_tx_kick(dev); return 0;
}
void lx_netdev_tx_stats_get(const struct net_device *dev, struct lx_netdev_tx_stats *out) {
  if (dev->tx) *out = dev->tx->stats; else memset(out, 0, sizeof(*out));
}
static struct notifier_block *notifiers;
static int next_ifindex = 1;
static int notify_device(struct net_device *dev, unsigned event) {
  struct netdev_notifier_info info = { .dev = dev };
  int result = NOTIFY_DONE;
  for (struct notifier_block *n = notifiers; n; n = n->next) {
    result = n->notifier_call(n, event, &info);
    if (result & NOTIFY_STOP_MASK) break;
  }
  return result;
}
int register_netdevice_notifier(struct notifier_block *notifier) {
  struct notifier_block **entry = &notifiers;
  for (struct notifier_block *n = notifiers; n; n = n->next) if (n == notifier) return -EEXIST;
  while (*entry && (*entry)->priority >= notifier->priority) entry = &(*entry)->next;
  notifier->next = *entry; *entry = notifier;
  for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) if (devices[i]) {
    struct netdev_notifier_info info = { .dev = devices[i] };
    notifier->notifier_call(notifier, NETDEV_REGISTER, &info);
    if (netif_running(devices[i])) notifier->notifier_call(notifier, NETDEV_UP, &info);
  }
  return 0;
}
int unregister_netdevice_notifier(struct notifier_block *notifier) {
  struct notifier_block **entry = &notifiers;
  while (*entry && *entry != notifier) entry = &(*entry)->next;
  if (!*entry) return -ENOENT;
  *entry = notifier->next; notifier->next = NULL; return 0;
}
static bool name_used(const struct net_device *dev, const char *name) {
  for (unsigned i = 0; i < ARRAY_SIZE(devices); i++)
    if (devices[i] && devices[i] != dev && !strcmp(devices[i]->name, name)) return true;
  return false;
}
int dev_alloc_name(struct net_device *dev, const char *name) {
  char pattern[IFNAMSIZ], candidate[IFNAMSIZ]; unsigned len = 0;
  while (len < IFNAMSIZ && name[len]) {
    if (name[len] == '/' || name[len] == ':' || name[len] <= ' ') return -EINVAL;
    pattern[len] = name[len]; len++;
  }
  if (!len || len >= IFNAMSIZ) return -EINVAL;
  pattern[len] = 0;
  char *format = strchr(pattern, '%');
  if (format && (format[1] != 'd' || strchr(format + 2, '%'))) return -EINVAL;
  for (unsigned index = 0; index < 10000; index++) {
    int written = format ? snprintf(candidate, sizeof(candidate), pattern, (int)index) :
                           snprintf(candidate, sizeof(candidate), "%s", pattern);
    if (written < 0 || written >= IFNAMSIZ) return -ENAMETOOLONG;
    if (!name_used(dev, candidate)) { memcpy(dev->name, candidate, written + 1); return (int)index; }
    if (!format) return -EEXIST;
  }
  return -ENOSPC;
}
int dev_change_net_namespace(struct net_device *dev, struct net *net, const char *name_pattern) {
  (void)name_pattern;
  struct net *current = dev->nd_net ? dev->nd_net : &init_net;
  return net == current ? 0 : -EOPNOTSUPP;
}
void ether_setup(struct net_device *dev) {
  dev->type = 1; dev->addr_len = ETH_ALEN; dev->hard_header_len = ETH_HLEN;
  dev->mtu = 1500; dev->min_mtu = 68; dev->max_mtu = 65535;
  dev->flags = 2 | 0x1000; /* IFF_BROADCAST | IFF_MULTICAST */
}
static void netdev_device_release(struct device *device) {
  struct net_device *dev = container_of(device, struct net_device, dev);
  BUG_ON(!__atomic_load_n(&dev->free_requested, __ATOMIC_ACQUIRE));
  BUG_ON(dev->tx_backlog || (dev->tx && work_pending(&dev->tx->work)));
  if (dev->mc.list.next) __hw_addr_flush(&dev->mc);
  kfree(dev->allocation ? dev->allocation : dev);
}
static void netdev_drop_owner(struct net_device *dev) {
  if (!__atomic_exchange_n(&dev->free_requested, true, __ATOMIC_ACQ_REL)) dev_put(dev);
}
static void netdev_deferred_free(struct work_struct *work) {
  /* cfg80211 continues using wdev after unregister_netdevice. Release the
   * allocation only after the enclosing RTNL/wiphy control operation ends. */
  netdev_drop_owner(container_of(work, struct net_device, free_work));
}
struct net_device *alloc_netdev_mqs(int priv_size, const char *name, u8 assign_type,
                                   void (*setup)(struct net_device *), unsigned txqs, unsigned rxqs) {
  (void)assign_type;
  if (priv_size < 0 || !txqs || !rxqs || txqs > BITS_PER_LONG) return NULL;
  size_t offset = ALIGN(sizeof(struct net_device), NETDEV_ALIGN);
  size_t tx_offset = ALIGN(offset + (size_t)priv_size, __alignof__(struct lx_netdev_tx));
  void *allocation = kzalloc(tx_offset + sizeof(struct lx_netdev_tx) + txqs * sizeof(struct sk_buff_head) + NETDEV_ALIGN - 1, GFP_KERNEL);
  if (!allocation) return NULL;
  struct net_device *dev = (void *)ALIGN((uintptr_t)allocation, NETDEV_ALIGN);
  dev->allocation = allocation; dev->priv = (u8 *)dev + offset;
  dev->tx = (void *)((u8 *)dev + tx_offset); dev->tx->dev = dev;
  dev->num_tx_queues = dev->real_num_tx_queues = txqs; dev->tx_queue_len = 256; dev->tx_memory_limit = 512 * 1024;
  INIT_WORK(&dev->tx->work, tx_worker);
  INIT_WORK(&dev->free_work, netdev_deferred_free);
  for (unsigned i = 0; i < txqs; i++) skb_queue_head_init(&dev->tx->queues[i]);
  device_initialize(&dev->dev); dev->dev.release = netdev_device_release;
  __hw_addr_init(&dev->mc); spin_lock_init(&dev->addr_list_lock);
  if (setup) setup(dev);
  /* Keep the format until dev_alloc_name reserves the concrete interface name. */
  size_t length = 0; while (length < IFNAMSIZ && name[length]) length++;
  if (!length || length >= IFNAMSIZ) { kfree(allocation); return NULL; }
  memcpy(dev->name, name, length + 1); return dev;
}
int eth_mac_addr(struct net_device *dev, void *address) {
  const struct sockaddr *addr = address;
  if (!is_valid_ether_addr((const u8 *)addr->sa_data)) return -EADDRNOTAVAIL;
  if (netif_running(dev) && !(dev->priv_flags & IFF_LIVE_ADDR_CHANGE)) return -EBUSY;
  eth_hw_addr_set(dev, (const u8 *)addr->sa_data); return 0;
}
int register_netdevice(struct net_device *dev) {
  if (dev->registered) return -EBUSY;
  /* The XNU boundary supplies complete Ethernet packets, with no GSO/TSO
   * capability advertised to the host stack. Keep the Linux side consistent. */
  dev->features &= ~NETIF_F_GSO_MASK; dev->hw_features &= ~NETIF_F_GSO_MASK;
  if (name_used(dev, dev->name)) return -EEXIST;
  unsigned slot; for (slot = 0; slot < ARRAY_SIZE(devices) && devices[slot]; slot++) { }
  if (slot == ARRAY_SIZE(devices)) return -ENOSPC;
  int result = dev->netdev_ops && dev->netdev_ops->ndo_init ? dev->netdev_ops->ndo_init(dev) : 0;
  if (result) return result;
  result = notifier_to_errno(notify_device(dev, NETDEV_POST_INIT));
  if (result) {
    if (dev->netdev_ops && dev->netdev_ops->ndo_uninit) dev->netdev_ops->ndo_uninit(dev);
    return result;
  }
  dev->ifindex = next_ifindex++; dev->registered = true; devices[slot] = dev;
  notify_device(dev, NETDEV_REGISTER); return 0;
}
int dev_open(struct net_device *dev, struct netlink_ext_ack *extack) {
  (void)extack;
  if (!dev->registered) return -ENODEV;
  if (netif_running(dev)) return 0;
  int result = notifier_to_errno(notify_device(dev, NETDEV_PRE_UP));
  if (result) return result;
  dev->running = true;
  result = dev->netdev_ops && dev->netdev_ops->ndo_open ? dev->netdev_ops->ndo_open(dev) : 0;
  if (!result) { dev->flags |= IFF_UP; notify_device(dev, NETDEV_UP); lx_netdev_tx_kick(dev); }
  else dev->running = false;
  return result;
}
void unregister_netdevice(struct net_device *dev) {
  if (!dev->registered) return;
  dev_close(dev);
  for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) if (devices[i] == dev) devices[i] = NULL;
  dev->registered = false; notify_device(dev, NETDEV_UNREGISTER);
  if (dev->netdev_ops && dev->netdev_ops->ndo_uninit) dev->netdev_ops->ndo_uninit(dev);
  if (dev->needs_free_netdev) queue_work(system_wq, &dev->free_work);
}
void free_netdev(struct net_device *dev) {
  if (!dev) return;
  unregister_netdevice(dev);
  cancel_work_sync(&dev->free_work); netdev_drop_owner(dev);
}
void dev_hold(struct net_device *dev) {
  if (dev) get_device(&dev->dev);
}
void dev_put(struct net_device *dev) {
  if (dev) put_device(&dev->dev);
}

static struct netdev_hw_addr *find_address(struct netdev_hw_addr_list *list, const u8 *address, int len, u8 type) {
  struct netdev_hw_addr *entry;
  list_for_each_entry(entry, &list->list, list)
    if (entry->type == type && !memcmp(entry->addr, address, (size_t)len)) return entry;
  return NULL;
}
int __hw_addr_add(struct netdev_hw_addr_list *list, const u8 *addr, int len, u8 type) {
  if (len <= 0 || len > 32) return -EINVAL;
  struct netdev_hw_addr *entry = find_address(list, addr, len, type);
  if (entry) { entry->refcount++; return 0; }
  entry = kzalloc(sizeof(*entry), GFP_ATOMIC); if (!entry) return -ENOMEM;
  memcpy(entry->addr, addr, (size_t)len); entry->type = type; entry->refcount = 1;
  list_add_tail(&entry->list, &list->list); list->count++; return 0;
}
int __hw_addr_del(struct netdev_hw_addr_list *list, const u8 *addr, int len, u8 type) {
  if (len <= 0 || len > 32) return -EINVAL;
  struct netdev_hw_addr *entry = find_address(list, addr, len, type);
  if (!entry) return -ENOENT;
  if (--entry->refcount) return 0;
  list_del(&entry->list); list->count--; kfree(entry); return 0;
}
int __hw_addr_sync(struct netdev_hw_addr_list *to, struct netdev_hw_addr_list *from, int len) {
  if (len <= 0 || len > 32) return -EINVAL;
  struct netdev_hw_addr *entry, *next;
  list_for_each_entry_safe(entry, next, &from->list, list) {
    if (!entry->sync_cnt) {
      int result = __hw_addr_add(to, entry->addr, len, entry->type); if (result) return result;
      entry->sync_cnt++; entry->refcount++;
    } else if (entry->refcount == 1 && !__hw_addr_del(to, entry->addr, len, entry->type)) {
      entry->sync_cnt--; __hw_addr_del(from, entry->addr, len, entry->type);
    }
  }
  return 0;
}
void __hw_addr_unsync(struct netdev_hw_addr_list *to, struct netdev_hw_addr_list *from, int len) {
  if (len <= 0 || len > 32) return;
  struct netdev_hw_addr *entry, *next;
  list_for_each_entry_safe(entry, next, &from->list, list) {
    if (entry->sync_cnt && !__hw_addr_del(to, entry->addr, len, entry->type)) {
      entry->sync_cnt--; __hw_addr_del(from, entry->addr, len, entry->type);
    }
  }
}
void __hw_addr_flush(struct netdev_hw_addr_list *list) {
  while (!list_empty(&list->list)) {
    struct netdev_hw_addr *entry = list_first_entry(&list->list, struct netdev_hw_addr, list);
    list_del(&entry->list); kfree(entry);
  }
  list->count = 0;
}

void dev_sw_netstats_rx_add(struct net_device *dev, unsigned bytes) {
  __atomic_fetch_add(&dev->stats.rx_packets, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&dev->stats.rx_bytes, bytes, __ATOMIC_RELAXED);
}
void dev_sw_netstats_tx_add(struct net_device *dev, unsigned packets, unsigned bytes) {
  __atomic_fetch_add(&dev->stats.tx_packets, packets, __ATOMIC_RELAXED);
  __atomic_fetch_add(&dev->stats.tx_bytes, bytes, __ATOMIC_RELAXED);
}
int netif_receive_skb(struct sk_buff *skb) {
  struct net_device *dev = skb->dev;
  if (dev && dev->input) return dev->input(dev, skb);
  if (dev) __atomic_fetch_add(&dev->stats.rx_dropped, 1, __ATOMIC_RELAXED);
  kfree_skb(skb); return -ENODEV;
}
int netif_rx(struct sk_buff *skb) { return netif_receive_skb(skb); }
struct sk_buff *skb_gso_segment(struct sk_buff *skb, netdev_features_t features) {
  (void)features;
  return skb_is_gso(skb) ? ERR_PTR(-EOPNOTSUPP) : NULL;
}
void netif_receive_skb_list(struct list_head *list) {
  struct net_device *last = NULL;
  while (!list_empty(list)) {
    struct sk_buff *skb = list_first_entry(list, struct sk_buff, list);
    struct net_device *dev = skb->dev;
    if (last && last != dev && last->input_flush) last->input_flush(last);
    last = dev;
    skb_list_del_init(skb); netif_receive_skb(skb);
  }
  if (last && last->input_flush) last->input_flush(last);
}
void netif_stop_subqueue(struct net_device *dev, u16 queue) {
  if (WARN_ON(queue >= BITS_PER_LONG)) return;
  set_bit(queue, &dev->stopped_queues);
}
void netif_wake_subqueue(struct net_device *dev, u16 queue) {
  if (WARN_ON(queue >= BITS_PER_LONG)) return;
  if (test_and_clear_bit(queue, &dev->stopped_queues)) {
    lx_netdev_tx_kick(dev);
    if (dev->queue_wake) dev->queue_wake(dev);
  }
}
bool __netif_subqueue_stopped(const struct net_device *dev, u16 queue) {
  return queue >= BITS_PER_LONG || test_bit(queue, &dev->stopped_queues);
}
int dev_close(struct net_device *dev) {
  if (!netif_running(dev)) return 0;
  notify_device(dev, NETDEV_GOING_DOWN);
  dev->running = false;
  if (dev->tx) {
    BUG_ON(dev->tx->active_queues);
    if (cancel_work_sync(&dev->tx->work)) dev_put(dev);
    tx_purge(dev);
  }
  if (dev->netdev_ops && dev->netdev_ops->ndo_stop) dev->netdev_ops->ndo_stop(dev);
  dev->flags &= ~IFF_UP; notify_device(dev, NETDEV_DOWN); return 0;
}

__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev) {
  skb->dev = dev;
  if (!pskb_may_pull(skb, ETH_HLEN)) return 0;
  skb_reset_mac_header(skb);
  const u8 *ethernet = skb->data;
  skb->pkt_type = PACKET_HOST;
  if (is_multicast_ether_addr(ethernet))
    skb->pkt_type = is_broadcast_ether_addr(ethernet) ? PACKET_BROADCAST : PACKET_MULTICAST;
  else if (!ether_addr_equal(ethernet, dev->dev_addr)) skb->pkt_type = PACKET_OTHERHOST;
  __be16 protocol; memcpy(&protocol, ethernet + 12, 2);
  skb_pull(skb, ETH_HLEN); skb_reset_network_header(skb);
  if (get_unaligned_be16(ethernet + 12) >= 0x0600) return protocol;
  if (skb_headlen(skb) >= 2 && skb->data[0] == 0xff && skb->data[1] == 0xff)
    return cpu_to_be16(ETH_P_802_3);
  return cpu_to_be16(ETH_P_802_2);
}

int skb_get_dsfield(const struct sk_buff *skb) {
  size_t tail_offset = (size_t)(skb->tail - skb->head), network = skb->network_header;
  size_t header;
  if (skb->protocol == cpu_to_be16(ETH_P_IP)) header = 20;
  else if (skb->protocol == cpu_to_be16(ETH_P_IPV6)) header = 40;
  else return -1;
  if (network < skb_headroom(skb) || network > tail_offset || header > tail_offset - network) return -1;
  const u8 *ip = skb_network_header(skb);
  if (header == 20 && ip[0] >> 4 == 4)
    return ip[1];
  if (header == 40 && ip[0] >> 4 == 6)
    return (ip[0] & 15) << 4 | ip[1] >> 4;
  return -1;
}
int INET_ECN_set_ce(struct sk_buff *skb) {
  int dsfield = skb_get_dsfield(skb);
  if (dsfield < 0 || !(dsfield & 3)) return 0;
  if ((dsfield & 3) == 3) return 1;
  unsigned offset = skb_network_offset(skb);
  unsigned header = skb->protocol == cpu_to_be16(ETH_P_IP) ? 20 : 40;
  if (skb_ensure_writable(skb, offset + header)) return 0;
  u8 *ip = skb_network_header(skb);
  if (header == 20) {
    /* RFC 1624: updating the traffic-class byte also updates the IP checksum. */
    u16 old_word = get_unaligned_be16(ip), new_word = old_word | 3;
    u32 sum = (u16)~get_unaligned_be16(ip + 10) + (u16)~old_word + new_word;
    sum = (sum & 0xffff) + (sum >> 16); sum = (sum & 0xffff) + (sum >> 16);
    ip[1] |= 3; ip[10] = (u8)(~sum >> 8); ip[11] = (u8)~sum;
  } else ip[1] |= 0x30;
  return 1;
}

int skb_csum_hwoffload_help(struct sk_buff *skb, netdev_features_t features) {
  if (skb->ip_summed != CHECKSUM_PARTIAL) return 0;
  if (skb->csum_not_inet) return features & NETIF_F_SCTP_CRC ? 0 : -EOPNOTSUPP;
  if (features & NETIF_F_HW_CSUM) return 0;
  bool supported_ip = skb->protocol == cpu_to_be16(ETH_P_IP) && (features & NETIF_F_IP_CSUM);
  bool supported_ip6 = skb->protocol == cpu_to_be16(ETH_P_IPV6) && (features & NETIF_F_IPV6_CSUM) &&
                       (int)skb->transport_header - skb->network_header == 40;
  if ((supported_ip || supported_ip6) && (skb->csum_offset == 6 || skb->csum_offset == 16)) return 0;
  int start = skb_checksum_start_offset(skb);
  if (skb_is_gso(skb) || start < 0 || (unsigned)start >= skb->len ||
      skb->csum_offset + 2u > skb->len - (unsigned)start) return -EINVAL;
  int result = skb_ensure_writable(skb, skb->len);
  if (result) return result;
  const u8 *bytes = skb->data + start; unsigned len = skb->len - (unsigned)start;
  u32 sum = 0;
  while (len >= 2) {
    sum += (unsigned)bytes[0] * 256 + bytes[1]; sum = (sum & 65535) + (sum >> 16);
    bytes += 2; len -= 2;
  }
  if (len) sum += (unsigned)bytes[0] * 256;
  while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
  u16 check = (u16)~sum;
  put_unaligned_be16(check ? check : 0xffff, skb->data + start + skb->csum_offset);
  skb->ip_summed = CHECKSUM_NONE; return 0;
}
