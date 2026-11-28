/* Execute original subsystem/device lifecycle with simulated radio operations. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "nw_abi.h"
#include "nw_mainline.h"
#include "nw_cfg80211_control.h"
#include "nw_control_endpoint.h"
#include <net/genetlink.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/nw_mainline.c"
int backtrace(void **, int);
void backtrace_symbols_fd(void *const *, int, int);
struct nw_wifi_stats nw_wifi_stats;
static u64 ticks;
static unsigned allocations, frees, ready_count, starts, stops, interfaces;
static unsigned preemption_depth, scan_completions;
static void disable_preemption(void) { preemption_depth++; }
static void enable_preemption(void) { assert(preemption_depth); preemption_depth--; }
static struct ieee80211_hw *expected_hw;
static unsigned reads, releases, receives, wireless_events;
static bool fail_allocation;
static u8 ethernet[1600], last_wireless[8192];
static size_t last_wireless_length;
static struct { void *allocation, *stack[8]; size_t length; int depth; } live[4096];
static void *allocate(size_t length) {
  if (fail_allocation) return NULL;
  void *p = malloc(length); if (!p) return NULL; allocations++;
  for (unsigned i = 0; i < ARRAY_SIZE(live); i++) if (!live[i].allocation) {
    live[i].allocation = p; live[i].length = length; live[i].depth = backtrace(live[i].stack, 8); return p;
  }
  abort();
}
static void release(void *p) {
  if (p) { frees++; for (unsigned i = 0; i < ARRAY_SIZE(live); i++) if (live[i].allocation == p) { live[i].allocation = NULL; break; } }
  free(p);
}
static void *dma_allocate(size_t length, u64 *bus) { void *p = calloc(1, length); *bus = (uintptr_t)p; return p; }
static void dma_release(size_t length, void *p, u64 bus) { (void)length; (void)bus; free(p); }
static u64 now(void) { return ticks; }
static void sleep_ms(unsigned ms) { assert(!preemption_depth); ticks += ms; }
static void poll(void) { nw_mainline_poll(); }
static void halt(void) { abort(); }
static void log_line(int level, const char *text) { if (level <= 4) fprintf(stderr, "%s\n", text); }
static int firmware(const char *name, const u8 **bytes, size_t *length) { (void)name; (void)bytes; (void)length; return -ENOENT; }
static const struct lx_platform platform = { .now_ms = now, .sleep_ms = sleep_ms, .poll = poll,
  .halt = halt, .log = log_line, .alloc = allocate, .free = release, .dma_alloc = dma_allocate,
  .dma_free = dma_release, .fw_get = firmware, .preempt_disable = disable_preemption, .preempt_enable = enable_preemption };
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { assert(hw == expected_hw); ready_count++; }
static int transport_send(void *context, uint64_t owner, const void *bytes, size_t length) {
  (void)context; (void)owner; struct nw_control_view view; assert(!nw_control_decode(bytes, length, &view));
  if (view.header.operation == NW_CONTROL_WIRELESS_EVENT) {
    assert(length <= sizeof(last_wireless)); memcpy(last_wireless, bytes, length); last_wireless_length = length; wireless_events++;
  }
  return NW_CONTROL_SEND_OK;
}
static void wake(void *context) { (void)context; }
static int start(struct ieee80211_hw *hw) { assert(hw == expected_hw); starts++; return 0; }
static void stop(struct ieee80211_hw *hw, bool suspend) { (void)suspend; assert(hw == expected_hw); stops++; }
static int add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { assert(hw == expected_hw && vif->type == NL80211_IFTYPE_STATION); interfaces++; return 0; }
static void remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { (void)hw; (void)vif; assert(interfaces); interfaces--; }
static void tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control, struct sk_buff *skb) { (void)hw; (void)control; ieee80211_free_txskb(hw, skb); }
static int config(struct ieee80211_hw *hw, u32 changed) { (void)hw; (void)changed; return 0; }
static void changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_bss_conf *conf, u64 flags) { (void)hw; (void)vif; (void)conf; (void)flags; }
static void filter(struct ieee80211_hw *hw, unsigned changed, unsigned *total, u64 multicast) { (void)hw; (void)changed; (void)multicast; *total = 0; }
static void wake_tx(struct ieee80211_hw *hw, struct ieee80211_txq *txq) { (void)hw; (void)txq; }
static int rts(struct ieee80211_hw *hw, u32 threshold) { (void)hw; (void)threshold; return 0; }
static const struct ieee80211_ops ops = { .start = start, .stop = stop, .add_interface = add_interface,
  .remove_interface = remove_interface, .tx = tx, .config = config, .bss_info_changed = changed, .configure_filter = filter,
  .wake_tx_queue = wake_tx, .set_rts_threshold = rts, .add_chanctx = ieee80211_emulate_add_chanctx,
  .remove_chanctx = ieee80211_emulate_remove_chanctx, .change_chanctx = ieee80211_emulate_change_chanctx };
int lx_init_cfg80211_init(void); int lx_init_ieee80211_init(void);
void lx_exit_cfg80211_exit(void); void lx_exit_ieee80211_exit(void);
static int read_packet(void *context, size_t offset, void *target, size_t length) {
  assert(context == ethernet && offset <= sizeof(ethernet) && length <= sizeof(ethernet) - offset);
  reads++; memcpy(target, ethernet + offset, length); return 0;
}
static void release_packet(void *context) { assert(context == ethernet); releases++; }
static void receive_packet(void *context, size_t length, nw_packet_reader reader) {
  u8 bytes[1600]; assert(length == sizeof(bytes) && !reader(context, 0, bytes, length));
  assert(!memcmp(bytes, ethernet, length));
  assert(!reader(context, 20, bytes, 100) && !memcmp(bytes, ethernet + 20, 100)); receives++;
}
static void authorized_fixture(void) {
  /* Simulate port authorization; the original MAC still has no radio peer. */
  memzero_explicit(&ui, sizeof(ui)); /* This fixture's new request has been consumed. */
  u8 bytes[64]; struct nw_control_builder b; struct nw_control_view view;
  assert(!nw_control_begin(&b, bytes, sizeof(bytes), NW_CONTROL_REQUEST, NW_CONTROL_SET_PORT, 1, 1, 1, 0));
  assert(!nw_control_decode(bytes, b.length, &view)); event(NW_CONTROL_SET_PORT, 1, &view);
}
static void data_boundary(struct net_device *dev) {
  for (unsigned i = 0; i < sizeof(ethernet); i++) ethernet[i] = (u8)i;
  ethernet[0] = 2; ethernet[6] = 2; ethernet[12] = 8; ethernet[13] = 0;
  authorized_fixture();
  assert(!nw_wifi_tx_packet(ethernet, 60, read_packet, release_packet));
  wiphy_lock(dev->ieee80211_ptr->wiphy); nw_mainline_poll(); lx_pump(); wiphy_unlock(dev->ieee80211_ptr->wiphy);
  assert(reads == 1 && releases == 1 && !nw_mainline_pending());
  assert(!nw_wifi_tx_packet(ethernet, 60, read_packet, release_packet));
  nw_wifi_disconnect(); nw_mainline_poll(); lx_pump();
  assert(reads == 1 && releases == 2); /* A previous connection's payload is never copied or sent. */
  assert(nw_wifi_tx_packet(ethernet, 60, read_packet, release_packet) == -ENOBUFS && releases == 2);
  authorized_fixture();
  for (unsigned i = 0; i < 256; i++) assert(!nw_wifi_tx_packet(ethernet, 60, read_packet, release_packet));
  assert(nw_wifi_tx_packet(ethernet, 60, read_packet, release_packet) == -ENOBUFS);
  nw_wifi_disconnect(); nw_mainline_poll(); for (unsigned i = 0; i < 8; i++) lx_pump();
  assert(!nw_mainline_pending() && reads == 1 && releases == 258);
  struct sk_buff *parent = alloc_skb(1632, GFP_KERNEL), *child = alloc_skb(64, GFP_KERNEL); assert(parent && child);
  skb_reserve(parent, 32); memcpy(skb_put(parent, 1600), ethernet, 1600);
  skb_reserve(child, 32); memcpy(skb_put(child, 32), ethernet, 32);
  struct page *page = virt_to_head_page(parent->head); get_page(page);
  skb_add_rx_frag(child, 0, page, (int)(parent->data + 32 - parent->head), 1568, 1568); kfree_skb(parent);
  struct nw_wifi_events events = { .rx_packet = receive_packet }; nw_wifi_set_events(&events);
  child->protocol = eth_type_trans(child, dev);
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  assert(!dev->input(dev, child) && receives == 1); nw_wifi_buffer_stats_get(&after);
  assert(before.dma_allocations == after.dma_allocations && after.live_dma + 2 == before.live_dma);
}
static int copy_message(void *source, void *target, size_t length) { memcpy(target, source, length); return 0; }
static void scan_completed(void) { scan_completions++; }
static unsigned join_completions, signal_updates;
static int join_result;
static void joined(int result) { join_completions++; join_result = result; }
static void signal_changed(const struct nw_wifi_status *copy) { assert(copy->state == NW_STA_UP && copy->signal_dbm < 0); signal_updates++; }
static void join_completion_and_signal(void) {
  const struct nw_wifi_events callbacks = {.connect_done = joined, .link_update = signal_changed}; nw_wifi_set_events(&callbacks);
  const u8 name[] = "timeout", address[] = {2,3,4,5,6,7}, other[] = {2,3,4,5,6,8};
  assert(!nw_wifi_connect(name, sizeof(name) - 1, NULL, NULL, 0));
  ticks += 29999; nw_mainline_run(); assert(join_waiting && !join_completions);
  ticks++; nw_mainline_run(); assert(!join_waiting && join_completions == 1 && join_result == NW_RES_TIMEOUT && status.state == NW_STA_IDLE);
  nw_mainline_run(); assert(join_completions == 1);
  assert(!nw_wifi_connect(name, sizeof(name) - 1, NULL, NULL, 0)); nw_wifi_disconnect();
  ticks += 30001; nw_mainline_run(); assert(!join_waiting && join_completions == 1);
  memzero_explicit(&ui, sizeof(ui)); status.state = NW_STA_AUTH;
  u8 bytes[256]; struct nw_control_builder builder; struct nw_control_view view;
  assert(!nw_control_begin(&builder, bytes, sizeof(bytes), NW_CONTROL_EVENT, NW_CONTROL_ASSOC_RESULT, 0, 1, 1, 0));
  assert(!nw_control_put_u32(&builder, NW_CONTROL_ATTR_STATUS_CODE, 0));
  assert(!nw_control_put(&builder, NW_CONTROL_ATTR_BSSID, address, sizeof(address)));
  assert(!nw_control_put_u32(&builder, NW_CONTROL_ATTR_FREQUENCY, 5180));
  assert(!nw_control_put_u32(&builder, NW_CONTROL_ATTR_SIGNAL, (u32)-46));
  assert(!nw_control_decode(bytes, builder.length, &view)); event(NW_CONTROL_ASSOC_RESULT, 0, &view);
  assert(status.state == NW_STA_KEYS && status.signal_dbm == -46 && !bss_count);
  authorized_fixture(); assert(join_completions == 2);
  nw_mainline_signal_update(address, -40); assert(signal_updates == 1 && status.signal_dbm == -40);
  nw_mainline_signal_update(address, -41); nw_mainline_signal_update(other, -60);
  nw_mainline_signal_update(address, 0); nw_mainline_signal_update(address, -200);
  assert(signal_updates == 1 && status.signal_dbm == -40);
  ticks += 30001; nw_mainline_run(); assert(join_completions == 2 && status.state == NW_STA_UP);
  nw_wifi_disconnect(); nw_mainline_run();
}
static void join_channel_hint(void) {
  const u8 name[] = "hint", first[] = {2,3,4,5,6,7}, second[] = {2,3,4,5,6,8};
  bss_count = 2;
  for (unsigned i = 0; i < bss_count; i++) {
    memset(&bsses[i], 0, sizeof(bsses[i])); bsses[i].ssid_len = sizeof(name) - 1;
    memcpy(bsses[i].ssid, name, sizeof(name) - 1);
  }
  memcpy(bsses[0].bssid, first, ETH_ALEN); bsses[0].signal_dbm = -60; bsses[0].freq_mhz = 2412;
  memcpy(bsses[1].bssid, second, ETH_ALEN); bsses[1].signal_dbm = -40; bsses[1].freq_mhz = 5180;
  assert(!nw_wifi_connect(name, sizeof(name) - 1, NULL, NULL, 0) && ui.frequency == 5180);
  u8 bytes[64]; struct nw_control_builder builder; struct nw_control_view view;
  assert(!nw_control_begin(&builder, bytes, sizeof(bytes), NW_CONTROL_REQUEST, NW_CONTROL_SET_PORT, 1, 1, 1, 0));
  assert(!nw_control_decode(bytes, builder.length, &view)); event(NW_CONTROL_SET_PORT, 1, &view);
  assert(status.state == NW_STA_AUTH); /* Old authorization cannot complete a queued new join. */
  assert(!nw_wifi_connect(name, sizeof(name) - 1, first, NULL, 0) && ui.frequency == 2412);
  assert(!nw_wifi_connect((const u8 *)"missing", 7, NULL, NULL, 0) && !ui.frequency);
  memcpy(status.bssid, first, ETH_ALEN); status.state = NW_STA_UP;
  nw_mainline_rate_update(first, 12010); nw_mainline_rate_update(second, 999);
  struct nw_wifi_status copy; nw_wifi_status_get(&copy); assert(copy.tx_rate_100kbps == 12010);
  nw_wifi_power(1); nw_mainline_run(); nw_wifi_status_get(&copy);
  assert(!copy.tx_rate_100kbps); bss_count = 0;
  authorized_fixture(); event(NW_CONTROL_SET_PORT, 0, &view); nw_wifi_status_get(&copy);
  assert(copy.state == NW_STA_IDLE && !copy.ssid_len && !copy.freq_mhz && !copy.tx_rate_100kbps);
  authorized_fixture(); event(NW_CONTROL_SET_OPERSTATE, 0, &view); nw_wifi_status_get(&copy);
  assert(copy.state == NW_STA_IDLE && !copy.ssid_len);
}
static void power_and_scan_ordering(struct net_device *dev) {
  struct nw_wifi_events callbacks = {.scan_done = scan_completed}; nw_wifi_set_events(&callbacks);
  nw_wifi_power(0); assert(nw_wifi_scan(NULL, 0) == -ENETDOWN); nw_mainline_run();
  assert(!netif_running(dev) && !radio.pending && !radio.enabled);
  nw_wifi_power(1); assert(!nw_wifi_scan(NULL, 0));
  assert(radio.pending && ui.operation == NW_CONTROL_SCAN_REQUEST); /* Scan cannot replace the power vote. */
  nw_mainline_run(); assert(netif_running(dev) && !radio.pending && radio.enabled);
  assert(scan_waiting && ui.operation == NW_CONTROL_SCAN_REQUEST); /* No user daemon has connected yet. */
  assert(!nw_wifi_scan(NULL, 0)); ticks += 12001; nw_mainline_run();
  assert(!scan_waiting && !ui.operation && scan_completions == 1);
  nw_wifi_disconnect(); assert(!nw_wifi_scan(NULL, 0) && ui.operation == NW_CONTROL_DISCONNECT_REQUEST);
  nw_mainline_run(); assert(scan_completions == 2); /* Scan cannot replace a queued disconnect either. */
  nw_wifi_power(1); nw_mainline_run();
}
static void notification_boundary(struct net_device *dev) {
  uint64_t session; assert(!nw_wifi_control_attach(&session));
  u8 bytes[64]; struct nw_control_builder info;
  assert(!nw_control_begin(&info, bytes, sizeof(bytes), NW_CONTROL_REQUEST, NW_CONTROL_INFO, 1, session, 0, 0));
  assert(!nw_wifi_control_receive(session, info.length, copy_message, bytes)); nw_wifi_control_poll(); lx_pump();
  cfg80211_cqm_beacon_loss_notify(dev, GFP_KERNEL); nw_wifi_control_poll(); lx_pump();
  assert(wireless_events == 1);
  struct nw_control_view view; size_t length;
  assert(!nw_control_decode(last_wireless, last_wireless_length, &view));
  const u8 *encoded = nw_control_get(&view, NW_CONTROL_ATTR_FRAME, &length);
  assert(encoded && length >= 20 && get_unaligned_le32(encoded) == length && encoded[16] == NL80211_CMD_NOTIFY_CQM);
  unsigned remaining = length - 20; const u8 *at = encoded + 20; bool found = false;
  while (remaining >= 4) {
    unsigned size = get_unaligned_le16(at), aligned = NLA_ALIGN(size);
    assert(size >= 4 && aligned <= remaining);
    if ((get_unaligned_le16(at + 2) & NLA_TYPE_MASK) == NL80211_ATTR_CQM) {
      assert(size == 8 && get_unaligned_le16(at + 4) == 4 &&
        get_unaligned_le16(at + 6) == NL80211_ATTR_CQM_BEACON_LOSS_EVENT); found = true;
    }
    at += aligned; remaining -= aligned;
  }
  assert(found && !remaining);
  struct sk_buff *old = nlmsg_new(32, GFP_KERNEL); assert(old);
  const struct genl_family family = {.id = 0x13, .version = 1};
  void *header = genlmsg_put(old, 0, 0, &family, 0, NL80211_CMD_NOTIFY_CQM); assert(header); genlmsg_end(old, header);
  nw_wifi_control_detach(session); nw_wifi_control_poll(); lx_pump();
  assert(genlmsg_unicast(NULL, old, 1) == -ESTALE && wireless_events == 1);
}
int main(void) {
  lx_plat = &platform; lx_root_init();
  fail_allocation = true; assert(nw_mainline_init() == -ENOMEM); nw_mainline_shutdown();
  fail_allocation = false; assert(!nw_mainline_init());
  const struct nw_control_transport transport = { .send = transport_send, .wake = wake };
  assert(!nw_wifi_control_transport_set(&transport));
  assert(!lx_init_cfg80211_init()); assert(!lx_init_ieee80211_init());
  expected_hw = ieee80211_alloc_hw(16, &ops); assert(expected_hw);
  expected_hw->queues = 4; ieee80211_hw_set(expected_hw, HAS_RATE_CONTROL);
  expected_hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
  static struct ieee80211_channel channels[] = {{ .band = NL80211_BAND_5GHZ, .center_freq = 5180, .hw_value = 36, .max_power = 20 }};
  static struct ieee80211_rate rates[] = {{ .bitrate = 60, .hw_value = 0 }, { .bitrate = 120, .hw_value = 1 }, { .bitrate = 240, .hw_value = 2 }};
  static struct ieee80211_supported_band band = { .band = NL80211_BAND_5GHZ, .channels = channels,
    .n_channels = ARRAY_SIZE(channels), .bitrates = rates, .n_bitrates = ARRAY_SIZE(rates) };
  expected_hw->wiphy->bands[NL80211_BAND_5GHZ] = &band;
  u8 address[] = {2,1,2,3,4,5}; SET_IEEE80211_PERM_ADDR(expected_hw, address);
  assert(!ieee80211_register_hw(expected_hw));
  for (unsigned i = 0; i < 10; i++) { lx_pump(); ticks++; }
  struct net_device *dev = nw_cfg80211_primary(); assert(dev && ready_count == 1 && nw_wifi_stats.stage == 4);
  struct nw_wifi_status state; nw_wifi_status_get(&state); assert(state.channels == 1 && !memcmp(state.mac, address, 6));
  rtnl_lock(); assert(!dev_open(dev, NULL)); rtnl_unlock();
  assert(starts == 1 && interfaces == 1);
  assert(!nw_cfg80211_set_power(0)); assert(!netif_running(dev) && !interfaces && stops == 1);
  assert(!nw_cfg80211_set_power(1)); assert(netif_running(dev) && interfaces == 1 && starts == 2);
  data_boundary(dev); power_and_scan_ordering(dev); join_channel_hint(); join_completion_and_signal(); notification_boundary(dev);
  nw_mainline_shutdown(); ieee80211_unregister_hw(expected_hw); ieee80211_free_hw(expected_hw);
  lx_exit_ieee80211_exit(); lx_exit_cfg80211_exit();
  for (unsigned i = 0; i < 10; i++) { lx_pump(); ticks++; }
  rcu_barrier(); nw_wifi_buffer_pool_configure(0);
  for (unsigned i = 0; i < ARRAY_SIZE(live); i++) if (live[i].allocation) {
    fprintf(stderr, "live allocation: %zu bytes\n", live[i].length); backtrace_symbols_fd(live[i].stack, live[i].depth, 2);
  }
  fprintf(stderr, "lifecycle: interfaces=%u stops=%u allocations=%u frees=%u\n", interfaces, stops, allocations, frees);
  assert(!interfaces && stops == starts && allocations == frees && !preemption_depth);
  puts("mainline runtime: original cfg80211/mac80211 lifecycle, radio off/on, retained TX ownership/bounds/epoch, data progress under wiphy lock, nonlinear RX without linearization, original CQM notification encoding, stale-event rejection and complete teardown passed (radio operations simulated)");
  return 0;
}
