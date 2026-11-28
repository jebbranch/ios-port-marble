/* Board requests/status and Ethernet ownership. Wireless state is exclusively
 * cfg80211/mac80211; password authentication is exclusively wpa_supplicant. */
#include "lx_pci.h"
#include <net/mac80211.h>
#include "cfg80211/core.h"
#include "nw_mainline.h"
#include "nw_cfg80211_control.h"
#include "nw_events.h"

#define UI_BSS_MAX 128
static spinlock_t boundary_lock;
static struct nw_wifi_status status;
static struct nw_bss_info *bsses;
static unsigned bss_count, rx_drops;
struct ui_request { unsigned operation, frequency; u8 ssid[32], ssid_length, bssid[6], has_bssid; char password[64]; u8 password_length, enabled; };
static struct ui_request ui;
static struct { bool pending, enabled; unsigned revision; } radio;
static u64 scan_deadline;
static bool scan_waiting;
static u64 join_deadline;
static bool join_waiting;
struct tx_packet { struct tx_packet *next; unsigned length, epoch; void *context; nw_packet_reader reader; void (*release)(void *); u8 bytes[]; };
static struct tx_packet *tx_head, *tx_tail;
static unsigned tx_count, tx_bytes, epoch;
static bool stopping;
static bool scan_ack_pending;
static struct work_struct transmit_work;
static void transmit(void);
static void transmit_run(struct work_struct *work) {
  (void)work; transmit(); nw_mainline_poll();
}

static int status_result(int result) {
  switch (result) {
  case NW_CONTROL_OK: return NW_RES_OK;
  case NW_CONTROL_AUTH_FAILED: return NW_RES_BAD_KEY;
  case NW_CONTROL_TIMED_OUT: return NW_RES_TIMEOUT;
  case NW_CONTROL_NOT_FOUND: return NW_RES_NOT_FOUND;
  case NW_CONTROL_UNSUPPORTED: return NW_RES_UNSUPPORTED;
  default: return NW_RES_FAILED;
  }
}
static void scan_snapshot(struct net_device *dev) {
  struct cfg80211_registered_device *rdev = wiphy_to_rdev(dev->ieee80211_ptr->wiphy);
  struct cfg80211_internal_bss *entry;
  rcu_read_lock(); spin_lock_bh(&rdev->bss_lock); lx_shared_spin_lock(&boundary_lock);
  bss_count = 0;
  list_for_each_entry(entry, &rdev->bss_list, list) {
    if (bss_count == UI_BSS_MAX) break;
    const struct cfg80211_bss_ies *ies = rcu_dereference(entry->pub.ies);
    if (!ies || !entry->pub.channel) continue;
    struct nw_bss_info *out = &bsses[bss_count++]; memset(out, 0, sizeof(*out));
    memcpy(out->bssid, entry->pub.bssid, ETH_ALEN);
    out->freq_mhz = entry->pub.channel->center_freq;
    out->signal_dbm = rdev->wiphy.signal_type == CFG80211_SIGNAL_TYPE_MBM ? entry->pub.signal / 100 : entry->pub.signal;
    out->caps = entry->pub.capability; out->beacon_int = entry->pub.beacon_interval;
    out->age_ms = jiffies_to_msecs(jiffies - entry->ts);
    out->ie_len = min_t(size_t, ies->len, sizeof(out->ie)); memcpy(out->ie, ies->data, out->ie_len);
    const struct element *ssid = cfg80211_find_elem(WLAN_EID_SSID, ies->data, ies->len);
    if (ssid && ssid->datalen <= 32) { out->ssid_len = ssid->datalen; memcpy(out->ssid, ssid->data, ssid->datalen); }
  }
  nw_wifi_stats.bss_seen = bss_count;
  lx_shared_spin_unlock(&boundary_lock); spin_unlock_bh(&rdev->bss_lock); rcu_read_unlock();
}
static int packet_read(void *context, size_t offset, void *destination, size_t length) {
  struct sk_buff *packet = context;
  if (offset > packet->len || length > packet->len - offset) return -EINVAL;
  return skb_copy_bits(packet, offset, destination, length);
}
static int input(struct net_device *dev, struct sk_buff *packet) {
  (void)dev;
  if (!skb_mac_header_was_set(packet) || skb_headroom(packet) < ETH_HLEN ||
      skb_mac_header(packet) != packet->data - ETH_HLEN) { rx_drops++; kfree_skb(packet); return -EINVAL; }
  skb_push(packet, ETH_HLEN);
  if (nw_events.rx_packet) {
    nw_wifi_stats.frames++; nw_events.rx_packet(packet, packet->len, packet_read);
    kfree_skb(packet); return 0;
  }
  if (skb_linearize(packet)) { rx_drops++; kfree_skb(packet); return -ENOMEM; }
  nw_wifi_stats.frames++;
  if (nw_events.rx) nw_events.rx(packet->data, packet->len); else rx_drops++;
  kfree_skb(packet); return 0;
}
static void input_flush(struct net_device *dev) { (void)dev; if (nw_events.rx_flush) nw_events.rx_flush(); }
static void ready(struct net_device *dev) {
  dev->input = input; dev->input_flush = input_flush;
  lx_shared_spin_lock(&boundary_lock);
  memcpy(status.mac, dev->dev_addr, ETH_ALEN); status.state = NW_STA_IDLE;
  status.channels = ieee80211_get_num_supported_channels(dev->ieee80211_ptr->wiphy);
  lx_shared_spin_unlock(&boundary_lock);
  nw_wifi_hw_ready(wiphy_to_ieee80211_hw(dev->ieee80211_ptr->wiphy));
  nw_wifi_stats.stage = 4;
}
static void clear_connection(unsigned state) {
  status.tx_rate_100kbps = 0;
  status.state = state; memset(status.bssid, 0, sizeof(status.bssid));
  status.ssid_len = 0; memset(status.ssid, 0, sizeof(status.ssid)); status.freq_mhz = 0; status.signal_dbm = 0;
}
static void event(unsigned operation, int result, const struct nw_control_view *view) {
  if (stopping) return;
  struct net_device *dev = nw_cfg80211_primary();
  uint32_t value; size_t length;
  if (operation == NW_CONTROL_SCAN_DONE) {
    if (dev) scan_snapshot(dev);
    lx_shared_spin_lock(&boundary_lock); scan_waiting = false; if (status.state == NW_STA_SCANNING) status.state = NW_STA_IDLE; lx_shared_spin_unlock(&boundary_lock);
    nw_wifi_stats.scans++; if (nw_events.scan_done) nw_events.scan_done(); return;
  }
  lx_shared_spin_lock(&boundary_lock);
  if (radio.pending || ui.operation == NW_CONTROL_JOIN_REQUEST || ui.operation == NW_CONTROL_DISCONNECT_REQUEST) {
    lx_shared_spin_unlock(&boundary_lock); return;
  }
  switch (operation) {
  case NW_CONTROL_AUTHENTICATE: {
    status.state = NW_STA_AUTH;
    const void *ssid = nw_control_get(view, NW_CONTROL_ATTR_SSID, &length);
    if (ssid && length <= 32) { status.ssid_len = length; memcpy(status.ssid, ssid, length); }
    break;
  }
  case NW_CONTROL_ASSOC_RESULT: {
    if (nw_control_get_u32(view, NW_CONTROL_ATTR_STATUS_CODE, &value) || value) {
      join_waiting = false;
      clear_connection(NW_STA_IDLE); status.result = NW_RES_REJECTED;
      lx_shared_spin_unlock(&boundary_lock); if (nw_events.connect_done) nw_events.connect_done(NW_RES_REJECTED); return;
    }
    status.state = NW_STA_KEYS;
    const void *address = nw_control_get(view, NW_CONTROL_ATTR_BSSID, &length);
    if (address && length == ETH_ALEN) memcpy(status.bssid, address, ETH_ALEN);
    if (!nw_control_get_u32(view, NW_CONTROL_ATTR_FREQUENCY, &value)) status.freq_mhz = value;
    for (unsigned i = 0; i < bss_count; i++) if (ether_addr_equal(bsses[i].bssid, status.bssid)) { status.signal_dbm = bsses[i].signal_dbm; break; }
    if (!nw_control_get_u32(view, NW_CONTROL_ATTR_SIGNAL, &value) && (s32)value >= -127 && (s32)value < 0)
      status.signal_dbm = (s32)value;
    break;
  }
  case NW_CONTROL_SET_PORT:
    if (result && status.state != NW_STA_UP) {
      join_waiting = false;
      status.state = NW_STA_UP; status.result = NW_RES_OK;
      lx_shared_spin_unlock(&boundary_lock); if (nw_events.connect_done) nw_events.connect_done(NW_RES_OK); return;
    }
    if (!result && status.state == NW_STA_UP) {
      clear_connection(NW_STA_IDLE); epoch++;
      lx_shared_spin_unlock(&boundary_lock);
      if (nw_events.link_lost) nw_events.link_lost(WLAN_REASON_UNSPECIFIED);
      return;
    }
    break;
  case NW_CONTROL_SET_OPERSTATE:
    if (!result && status.state == NW_STA_UP) {
      clear_connection(NW_STA_IDLE); epoch++;
      lx_shared_spin_unlock(&boundary_lock);
      if (nw_events.link_lost) nw_events.link_lost(WLAN_REASON_UNSPECIFIED);
      return;
    }
    break;
  case NW_CONTROL_DISCONNECTED: {
    bool notify = status.state == NW_STA_UP;
    bool failed_join = join_waiting; join_waiting = false;
    clear_connection(status.state == NW_STA_OFF ? NW_STA_OFF : NW_STA_IDLE); epoch++;
    lx_shared_spin_unlock(&boundary_lock);
    if (notify && nw_events.link_lost) {
      value = 0; nw_control_get_u32(view, NW_CONTROL_ATTR_REASON, &value); nw_events.link_lost(value);
    }
    if (failed_join && nw_events.connect_done) nw_events.connect_done(NW_RES_REJECTED);
    return;
  }
  case NW_CONTROL_REPORT_RESULT:
    if (!nw_control_get_u32(view, NW_CONTROL_ATTR_REASON, &value) && value == NW_CONTROL_SCAN_REQUEST) {
      scan_waiting = false; if (status.state == NW_STA_SCANNING) status.state = NW_STA_IDLE;
      lx_shared_spin_unlock(&boundary_lock); if (nw_events.scan_done) nw_events.scan_done(); return;
    }
    if (result) {
      join_waiting = false;
      clear_connection(NW_STA_IDLE); status.result = status_result(result);
      int completion = status.result; lx_shared_spin_unlock(&boundary_lock);
      if (nw_events.connect_done) nw_events.connect_done(completion); return;
    }
    break;
  }
  lx_shared_spin_unlock(&boundary_lock);
}
int nw_mainline_init(void) {
  spin_lock_init(&boundary_lock); memset(&status, 0, sizeof(status)); stopping = false;
  memzero_explicit(&ui, sizeof(ui)); bss_count = rx_drops = 0; scan_ack_pending = false;
  memset(&radio, 0, sizeof(radio)); radio.enabled = true; scan_waiting = false; scan_deadline = 0;
  join_waiting = false; join_deadline = 0;
  INIT_WORK(&transmit_work, transmit_run);
  /* Keep the UI snapshot outside the kernel collection's fixed data gap. */
  bsses = kcalloc(UI_BSS_MAX, sizeof(*bsses), GFP_KERNEL);
  if (!bsses) return -ENOMEM;
  const struct nw_cfg80211_observer callbacks = { .ready = ready, .event = event }; nw_cfg80211_observe(&callbacks);
  return 0;
}
int nw_wifi_scan(const u8 *ssid, size_t length) {
  if (length > 32 || (length && !ssid)) return -EINVAL;
  lx_shared_spin_lock(&boundary_lock);
  if (stopping || !radio.enabled) { lx_shared_spin_unlock(&boundary_lock); return -ENETDOWN; }
  if (scan_waiting) { lx_shared_spin_unlock(&boundary_lock); return 0; } /* Coalesce duplicate UI scan requests. */
  if (!ui.operation || ui.operation == NW_CONTROL_SCAN_REQUEST) {
    memzero_explicit(&ui, sizeof(ui)); ui.operation = NW_CONTROL_SCAN_REQUEST;
    ui.ssid_length = length; if (length) memcpy(ui.ssid, ssid, length);
    scan_waiting = true; scan_deadline = lx_now_ms() + 12000;
  } else scan_ack_pending = true;
  lx_shared_spin_unlock(&boundary_lock); return 0;
}
int nw_wifi_connect(const u8 *ssid, size_t length, const u8 *address, const char *password, size_t password_length) {
  if (!ssid || !length || length > 32 || password_length > 64 ||
      (password && password_length < 8) || (!password && password_length)) return -EINVAL;
  lx_shared_spin_lock(&boundary_lock);
  if (stopping || !radio.enabled) { lx_shared_spin_unlock(&boundary_lock); return -ENETDOWN; }
  if (scan_waiting) { scan_waiting = false; scan_ack_pending = true; }
  memzero_explicit(&ui, sizeof(ui)); ui.operation = NW_CONTROL_JOIN_REQUEST;
  memcpy(ui.ssid, ssid, length); ui.ssid_length = length;
  if (address) { ui.has_bssid = 1; memcpy(ui.bssid, address, ETH_ALEN); }
  /* The Settings list already identifies candidate channels. Limit only the
   * first scan, leaving upstream free to expand its search on a later retry. */
  int best_signal = -200;
  for (unsigned i = 0; i < bss_count; i++) {
    const struct nw_bss_info *bss = &bsses[i];
    if (bss->ssid_len != length || memcmp(bss->ssid, ssid, length) ||
        (address && !ether_addr_equal(address, bss->bssid)) || bss->signal_dbm <= best_signal) continue;
    best_signal = bss->signal_dbm; ui.frequency = bss->freq_mhz;
  }
  if (password) { ui.password_length = password_length; memcpy(ui.password, password, password_length); }
  clear_connection(NW_STA_AUTH); status.ssid_len = length; memcpy(status.ssid, ssid, length); epoch++;
  join_waiting = true; join_deadline = lx_now_ms() + 30000;
  lx_shared_spin_unlock(&boundary_lock); return 0;
}
void nw_wifi_disconnect(void) {
  lx_shared_spin_lock(&boundary_lock); memzero_explicit(&ui, sizeof(ui)); ui.operation = NW_CONTROL_DISCONNECT_REQUEST;
  join_waiting = false;
  if (scan_waiting) { scan_waiting = false; scan_ack_pending = true; }
  clear_connection(radio.enabled ? NW_STA_IDLE : NW_STA_OFF); epoch++; lx_shared_spin_unlock(&boundary_lock);
}
void nw_wifi_power(int enabled) {
  lx_shared_spin_lock(&boundary_lock); memzero_explicit(&ui, sizeof(ui));
  radio.pending = true; radio.enabled = !!enabled; radio.revision++;
  join_waiting = false;
  if (scan_waiting) { scan_waiting = false; scan_ack_pending = true; }
  clear_connection(enabled ? NW_STA_IDLE : NW_STA_OFF); epoch++; lx_shared_spin_unlock(&boundary_lock);
}
int nw_wifi_bss_count(void) { lx_shared_spin_lock(&boundary_lock); unsigned count = bss_count; lx_shared_spin_unlock(&boundary_lock); return count; }
int nw_wifi_bss_get(int index, struct nw_bss_info *out) {
  if (!out) return -EINVAL; lx_shared_spin_lock(&boundary_lock);
  int result = index < 0 || (unsigned)index >= bss_count ? -ENOENT : 0;
  if (!result) *out = bsses[index]; lx_shared_spin_unlock(&boundary_lock); return result;
}
void nw_wifi_status_get(struct nw_wifi_status *out) { lx_shared_spin_lock(&boundary_lock); *out = status; lx_shared_spin_unlock(&boundary_lock); }
void nw_mainline_rate_update(const u8 *bssid, unsigned rate) {
  lx_shared_spin_lock(&boundary_lock);
  if (status.state == NW_STA_UP && ether_addr_equal(status.bssid, bssid)) status.tx_rate_100kbps = rate;
  lx_shared_spin_unlock(&boundary_lock);
}
void nw_mainline_signal_update(const u8 *bssid, int signal) {
  if (signal < -127 || signal >= 0) return;
  struct nw_wifi_status snapshot; bool changed = false;
  lx_shared_spin_lock(&boundary_lock);
  if (status.state == NW_STA_UP && ether_addr_equal(status.bssid, bssid) &&
      (!status.signal_dbm || signal - status.signal_dbm >= 3 || status.signal_dbm - signal >= 3)) {
    status.signal_dbm = signal; snapshot = status; changed = true;
  }
  lx_shared_spin_unlock(&boundary_lock);
  if (changed && nw_events.link_update) nw_events.link_update(&snapshot);
}
static int queue_packet(struct tx_packet *packet) {
  unsigned length = packet->length; packet->next = NULL;
  lx_shared_spin_lock(&boundary_lock);
  if (stopping || status.state != NW_STA_UP || tx_count >= 256 || tx_bytes + length > 512 * 1024) {
    lx_shared_spin_unlock(&boundary_lock); lx_free(packet); return -ENOBUFS;
  }
  packet->epoch = epoch;
  if (tx_tail) tx_tail->next = packet; else tx_head = packet;
  tx_tail = packet; tx_count++; tx_bytes += length; lx_shared_spin_unlock(&boundary_lock); return 0;
}
int nw_wifi_tx(const u8 *bytes, size_t length) {
  if (!bytes || length < ETH_HLEN || length > 2000) return -EINVAL;
  struct tx_packet *packet = lx_alloc(sizeof(*packet) + length, 1);
  if (!packet) return -ENOMEM;
  packet->length = length; memcpy(packet->bytes, bytes, length); return queue_packet(packet);
}
int nw_wifi_tx_packet(void *context, size_t length, nw_packet_reader reader, void (*release)(void *)) {
  if (!context || !reader || !release || length < ETH_HLEN || length > 2000) return -EINVAL;
  struct tx_packet *packet = lx_alloc(sizeof(*packet), 1);
  if (!packet) return -ENOMEM;
  packet->length = length; packet->context = context; packet->reader = reader; packet->release = release;
  return queue_packet(packet);
}
static void transmit(void) {
  u64 started = lx_now_ns();
  for (unsigned budget = 0; budget < 64; budget++) {
    lx_shared_spin_lock(&boundary_lock); struct tx_packet *item = tx_head;
    bool allowed = !stopping && status.state == NW_STA_UP && item && item->epoch == epoch;
    if (item) { tx_head = item->next; if (!tx_head) tx_tail = NULL; tx_count--; tx_bytes -= item->length; }
    lx_shared_spin_unlock(&boundary_lock); if (!item) return;
    struct net_device *dev = nw_cfg80211_primary();
    if (allowed && dev && netif_running(dev)) {
      unsigned reserve = dev->needed_headroom + 64;
      struct sk_buff *packet = alloc_skb(reserve + item->length + dev->needed_tailroom, GFP_KERNEL);
      if (packet) {
        skb_reserve(packet, reserve); void *destination = skb_put(packet, item->length);
        int error = item->reader ? item->reader(item->context, 0, destination, item->length) : 0;
        if (!item->reader) memcpy(destination, item->bytes, item->length);
        if (error) { kfree_skb(packet); packet = NULL; }
      }
      if (packet) {
        packet->dev = dev; packet->protocol = get_unaligned((__be16 *)(packet->data + 12));
        skb_reset_mac_header(packet); skb_set_network_header(packet, ETH_HLEN); packet->ip_summed = CHECKSUM_NONE;
        dev_queue_xmit(packet);
      }
    }
    if (item->release) item->release(item->context);
    lx_free(item);
    if (lx_now_ns() - started >= 2000000ULL) return;
  }
}
void nw_mainline_run(void) {
  bool timed_out = false;
  lx_shared_spin_lock(&boundary_lock);
  if (join_waiting && lx_now_ms() >= join_deadline) {
    join_waiting = false; timed_out = true;
    memzero_explicit(&ui, sizeof(ui)); ui.operation = NW_CONTROL_DISCONNECT_REQUEST;
    clear_connection(NW_STA_IDLE); status.result = NW_RES_TIMEOUT; epoch++;
  }
  lx_shared_spin_unlock(&boundary_lock);
  if (timed_out && nw_events.connect_done) nw_events.connect_done(NW_RES_TIMEOUT);
  bool power_pending, power_enabled; unsigned power_revision;
  lx_shared_spin_lock(&boundary_lock); power_pending = radio.pending; power_enabled = radio.enabled; power_revision = radio.revision; lx_shared_spin_unlock(&boundary_lock);
  if (power_pending) {
    int result = nw_cfg80211_set_power(power_enabled);
    if (!result) {
      lx_shared_spin_lock(&boundary_lock); if (radio.revision == power_revision) radio.pending = false; lx_shared_spin_unlock(&boundary_lock);
      u8 bytes[64]; struct nw_control_builder b;
      if (!nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), NW_CONTROL_POWER_REQUEST, 0) &&
          !nw_control_put_u32(&b, NW_CONTROL_ATTR_OPERSTATE, power_enabled)) nw_cfg80211_event_send(&b);
      lx_log(6, "wifi: radio %s applied", power_enabled ? "on" : "off");
    } else {
      nw_wifi_stats.last_err = result;
      /* An unsuccessful enable remains off; a new UI request can retry it. */
      lx_shared_spin_lock(&boundary_lock); if (radio.revision == power_revision) { radio.pending = false; radio.enabled = false; clear_connection(NW_STA_OFF); }
      lx_shared_spin_unlock(&boundary_lock); lx_log(4, "wifi: radio request failed: %d", result);
    }
  }
  struct ui_request pending;
  lx_shared_spin_lock(&boundary_lock);
  if (scan_waiting && lx_now_ms() >= scan_deadline) {
    scan_waiting = false; scan_ack_pending = true;
    if (ui.operation == NW_CONTROL_SCAN_REQUEST) memzero_explicit(&ui, sizeof(ui));
    if (status.state == NW_STA_SCANNING) status.state = NW_STA_IDLE;
  }
  bool scan_ack = scan_ack_pending; scan_ack_pending = false;
  pending = ui; bool power_changing = radio.pending; lx_shared_spin_unlock(&boundary_lock);
  if (scan_ack && nw_events.scan_done) nw_events.scan_done();
  if (power_changing) { memzero_explicit(&pending, sizeof(pending)); nw_mainline_poll(); return; }
  if (pending.operation) {
    u8 bytes[256]; struct nw_control_builder b;
    int result = nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), pending.operation, 0);
    if (!result && pending.operation == NW_CONTROL_JOIN_REQUEST) {
      result = nw_cfg80211_begin_attempt();
      if (!result) {
        nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), pending.operation, 0);
        result = nw_control_put(&b, NW_CONTROL_ATTR_SSID, pending.ssid, pending.ssid_length) ||
          nw_control_put(&b, NW_CONTROL_ATTR_PASSWORD, pending.password, pending.password_length) ||
          (pending.frequency && nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, pending.frequency)) ||
          (pending.has_bssid && nw_control_put(&b, NW_CONTROL_ATTR_BSSID, pending.bssid, ETH_ALEN));
      }
    } else if (!result && pending.operation == NW_CONTROL_DISCONNECT_REQUEST) {
      result = nw_cfg80211_begin_attempt();
      if (!result) nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), pending.operation, 0);
    } else if (!result && pending.operation == NW_CONTROL_SCAN_REQUEST && pending.ssid_length) {
      result = nw_control_put(&b, NW_CONTROL_ATTR_SSID, pending.ssid, pending.ssid_length);
    }
    if (!result) result = nw_cfg80211_event_send(&b);
    if (!result) {
      lx_shared_spin_lock(&boundary_lock);
      if (!memcmp(&ui, &pending, sizeof(ui))) {
        memzero_explicit(&ui, sizeof(ui));
        if (pending.operation == NW_CONTROL_SCAN_REQUEST && status.state == NW_STA_IDLE) status.state = NW_STA_SCANNING;
        if (pending.operation == NW_CONTROL_JOIN_REQUEST) { status.state = NW_STA_AUTH; status.result = NW_RES_OK; epoch++; }
      }
      lx_shared_spin_unlock(&boundary_lock);
    }
    memzero_explicit(bytes, sizeof(bytes));
  }
  memzero_explicit(&pending, sizeof(pending)); nw_mainline_poll();
}
void nw_mainline_poll(void) {
  /* ndo_start_xmit is a nonblocking softirq entry in Linux. Let data move
   * through firmware/control waits; it never acquires the wiphy mutex. */
  if (nw_mainline_pending() && !stopping) queue_work(system_bh_wq, &transmit_work);
}
void nw_mainline_shutdown(void) {
  lx_shared_spin_lock(&boundary_lock); stopping = true; join_waiting = false; memzero_explicit(&ui, sizeof(ui)); clear_connection(NW_STA_OFF); epoch++;
  lx_shared_spin_unlock(&boundary_lock);
  cancel_work_sync(&transmit_work);
  while (nw_mainline_pending()) transmit();
  lx_shared_spin_lock(&boundary_lock); struct nw_bss_info *snapshot = bsses; bsses = NULL; bss_count = 0; lx_shared_spin_unlock(&boundary_lock);
  kfree(snapshot);
}
int nw_mainline_pending(void) { lx_shared_spin_lock(&boundary_lock); bool pending = tx_count != 0; lx_shared_spin_unlock(&boundary_lock); return pending; }
unsigned nw_rx_backlog(void) { return 0; } /* Input is delivered at the NAPI boundary, without another RX queue. */
unsigned nw_rx_dropped(void) { return rx_drops; }
