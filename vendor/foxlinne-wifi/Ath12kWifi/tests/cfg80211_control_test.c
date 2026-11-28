/* Native mapping test. Radio/MLME boundaries below are explicit test doubles;
 * production broker, queues, netdev lifecycle and key validator are real. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_plat.h"
#include "../src/nw_cfg80211_control.c"

static u64 ticks;
static unsigned allocations, frees, opens, stops, auths, assocs, keys, ports, eapols, observed;
static unsigned reply_sequence;
static u8 last_reply[8192], last_event[8192]; static size_t reply_length, event_length;
static u8 ap[ETH_ALEN] = {2,6,7,8,9,10}, mac[ETH_ALEN] = {2,1,2,3,4,5};
static struct cfg80211_internal_bss bss[2];
static unsigned bss_refs;
static unsigned registrations, unregistrations, management_frames;
static struct cfg80211_registered_device *rdev;
static struct wireless_dev wdev;
static void *allocate(size_t size) { void *p = malloc(size); if (p) allocations++; return p; }
static void release(void *p) { if (p) frees++; free(p); }
static void *dma_allocate(size_t size, u64 *bus) { void *p = calloc(1, size); *bus = (uintptr_t)p; return p; }
static void dma_release(size_t size, void *p, u64 bus) { (void)size; (void)bus; free(p); }
static u64 now(void) { return ticks; }
static void sleep_ms(unsigned ms) { ticks += ms; }
static void nothing(void) {}
static void halt(void) { abort(); }
static void log_line(int level, const char *text) { (void)level; (void)text; }
static const struct lx_platform platform = { .now_ms = now, .sleep_ms = sleep_ms, .poll = nothing,
  .log = log_line, .halt = halt, .alloc = allocate, .free = release,
  .dma_alloc = dma_allocate, .dma_free = dma_release };
static void locked(void) { assert(lx_rtnl_is_locked() && mutex_is_locked(&rdev->wiphy.mtx)); }
static int open_dev(struct net_device *dev) { (void)dev; assert(!mutex_is_locked(&rdev->wiphy.mtx)); opens++; return 0; }
static int stop_dev(struct net_device *dev) { (void)dev; assert(!mutex_is_locked(&rdev->wiphy.mtx)); stops++; wdev.connected = false; return 0; }
static const struct net_device_ops net_ops = { .ndo_open = open_dev, .ndo_stop = stop_dev };
static int fake_auth(struct wiphy *wiphy, struct net_device *dev, struct cfg80211_auth_request *request) {
  (void)wiphy; (void)dev; (void)request; assert(!"MLME boundary called through wrong API"); return -EINVAL;
}
int cfg80211_mlme_register_mgmt(struct wireless_dev *device, u32 portid, u16 type,
    const u8 *match, int match_length, bool multicast, struct netlink_ext_ack *extack) {
  (void)extack; locked(); assert(device == &wdev && portid == 1 && type == (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION));
  assert(match_length == 2 && match[0] == WLAN_CATEGORY_SA_QUERY && match[1] == WLAN_ACTION_SA_QUERY_RESPONSE && !multicast);
  registrations++; return 0;
}
void cfg80211_mlme_unregister_socket(struct wireless_dev *device, u32 portid) {
  locked(); assert(device == &wdev && portid == 1); unregistrations++;
}
int cfg80211_mlme_mgmt_tx(struct cfg80211_registered_device *device, struct wireless_dev *wireless,
    struct cfg80211_mgmt_tx_params *params, u64 *cookie) {
  locked(); assert(device == rdev && wireless == &wdev && params->len == 28 && params->chan->center_freq == 5180);
  assert(!params->offchan && params->link_id == -1); *cookie = 123; management_frames++; return 0;
}
/* These test doubles inspect the exact cfg80211 request, without replacing
 * production authentication code. Full upstream SME/WPA runs in test-supplicant. */
int cfg80211_mlme_auth(struct cfg80211_registered_device *device, struct net_device *dev, struct cfg80211_auth_request *request) {
  locked(); assert(device == rdev && dev == primary && request->bss == &bss[0].pub);
  assert(request->auth_type == NL80211_AUTHTYPE_OPEN_SYSTEM && request->link_id == -1); auths++; return 0;
}
int cfg80211_mlme_assoc(struct cfg80211_registered_device *device, struct net_device *dev,
  struct cfg80211_assoc_request *request, struct netlink_ext_ack *extack) {
  (void)extack; locked(); assert(device == rdev && dev == primary && request->bss == &bss[0].pub);
  assert(request->crypto.control_port && request->crypto.control_port_over_nl80211 && request->crypto.control_port_no_preauth);
  assert(request->crypto.control_port_ethertype == cpu_to_be16(ETH_P_PAE) && request->crypto.n_ciphers_pairwise == 1);
  assert(request->crypto.ciphers_pairwise[0] == WLAN_CIPHER_SUITE_CCMP && request->link_id == -1);
  assocs++; wdev.connected = true; memcpy(wdev.u.client.connected_addr, ap, ETH_ALEN); return 0;
}
int cfg80211_mlme_deauth(struct cfg80211_registered_device *device, struct net_device *dev,
  const u8 *address, const u8 *ie, int length, u16 reason, bool local) {
  (void)ie; (void)length; (void)local; locked(); assert(device == rdev && dev == primary && ether_addr_equal(address, ap) && reason);
  wdev.connected = false; return 0;
}
struct cfg80211_bss *__cfg80211_get_bss(struct wiphy *wiphy, struct ieee80211_channel *channel,
  const u8 *address, const u8 *ssid, size_t length, enum ieee80211_bss_type type,
  enum ieee80211_privacy privacy, u32 use_for) {
  (void)type; (void)privacy; (void)use_for; assert(wiphy == &rdev->wiphy && channel == bss[0].pub.channel);
  assert(ether_addr_equal(address, ap) && ssid && length == 3 && !memcmp(ssid, "test", 3)); bss_refs++; return &bss[0].pub;
}
void cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *value) {
  assert(wiphy == &rdev->wiphy && value == &bss[0].pub && bss_refs); bss_refs--;
}
int cfg80211_scan(struct cfg80211_registered_device *device) {
  locked(); assert(device == rdev && rdev->scan_req && rdev->scan_req->req.n_channels == 1);
  assert(rdev->scan_req->req.n_ssids == 1 && rdev->scan_req->req.ssids[0].ssid_len == 3); return 0;
}
static int add_key(struct wiphy *wiphy, struct net_device *dev, int link, u8 index, bool pairwise,
  const u8 *address, struct key_params *key) {
  locked(); assert(wiphy == &rdev->wiphy && dev == primary && link == -1 && index == 0 && pairwise);
  assert(ether_addr_equal(address, ap) && key->cipher == WLAN_CIPHER_SUITE_CCMP && key->key_len == 16); keys++; return 0;
}
static int change_station(struct wiphy *wiphy, struct net_device *dev, const u8 *address, struct station_parameters *parameters) {
  (void)wiphy; (void)dev; locked(); assert(ether_addr_equal(address, ap));
  assert(parameters->listen_interval == -1 && parameters->support_p2p_ps == -1 && parameters->sta_flags_set == BIT(NL80211_STA_FLAG_AUTHORIZED));
  ports++; return 0;
}
static int tx_port(struct wiphy *wiphy, struct net_device *dev, const void *frame, size_t length,
  const u8 *destination, __be16 protocol, bool no_encrypt, int link, u64 *cookie) {
  (void)wiphy; (void)dev; locked(); assert(length == 4 && frame && ether_addr_equal(destination, ap));
  assert(protocol == cpu_to_be16(ETH_P_PAE) && no_encrypt && link == -1); *cookie = 0x123456789abULL; eapols++; return 0;
}
static const struct cfg80211_ops operations = { .auth = fake_auth, .assoc = (void *)fake_auth, .deauth = (void *)fake_auth,
  .scan = (void *)fake_auth, .add_key = add_key, .change_station = change_station, .tx_control_port = tx_port };
static int send_message(void *context, uint64_t session, const void *bytes, size_t length) {
  (void)context; (void)session; struct nw_control_view view; assert(!nw_control_decode(bytes, length, &view));
  if (view.header.kind == NW_CONTROL_REPLY) { assert(length <= sizeof(last_reply)); memcpy(last_reply, bytes, length); reply_length = length; reply_sequence = view.header.sequence; }
  else { assert(length <= sizeof(last_event)); memcpy(last_event, bytes, length); event_length = length; }
  return NW_CONTROL_SEND_OK;
}
static void wake(void *context) { (void)context; }
static void observe(unsigned operation, int status, const struct nw_control_view *view) { (void)operation; (void)status; (void)view; observed++; }
static int copy(void *source, void *target, size_t length) { memcpy(target, source, length); return 0; }
static u8 request_bytes[8192]; static struct nw_control_builder request;
static uint64_t socket_owner; static unsigned sequence;
static void begin(unsigned operation) {
  assert(!nw_control_begin(&request, request_bytes, sizeof(request_bytes), NW_CONTROL_REQUEST, operation, ++sequence, socket_owner, generation, 0));
}
static int exchange(void) {
  assert(nw_wifi_control_receive(socket_owner, request.length, copy, request.bytes) == NW_CONTROL_OK);
  for (unsigned i = 0; reply_sequence != sequence && i < 100; i++) { nw_wifi_control_poll(); lx_pump(); }
  assert(reply_sequence == sequence); struct nw_control_view reply; assert(!nw_control_decode(last_reply, reply_length, &reply));
  return reply.header.status;
}
static void bss_attributes(void) {
  assert(!nw_control_put(&request, NW_CONTROL_ATTR_BSSID, ap, ETH_ALEN));
  assert(!nw_control_put(&request, NW_CONTROL_ATTR_SSID, "test", 3));
  assert(!nw_control_put_u32(&request, NW_CONTROL_ATTR_FREQUENCY, 5180));
}
int main(void) {
  lx_plat = &platform; lx_root_init();
  rdev = calloc(1, sizeof(*rdev)); assert(rdev); rdev->ops = &operations;
  mutex_init(&rdev->wiphy.mtx); INIT_LIST_HEAD(&rdev->bss_list);
  static const u32 ciphers[] = {WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_AES_CMAC};
  rdev->wiphy.cipher_suites = ciphers; rdev->wiphy.n_cipher_suites = ARRAY_SIZE(ciphers);
  rdev->wiphy.max_scan_ssids = 16; rdev->wiphy.max_scan_ie_len = 2048; rdev->wiphy.signal_type = CFG80211_SIGNAL_TYPE_MBM;
  struct ieee80211_channel channel = { .band = NL80211_BAND_5GHZ, .center_freq = 5180 };
  struct ieee80211_supported_band band = { .channels = &channel, .n_channels = 1, .n_bitrates = 8 };
  rdev->wiphy.bands[NL80211_BAND_5GHZ] = &band;
  for (unsigned i = 0; i < 2; i++) {
    INIT_LIST_HEAD(&bss[i].list); list_add_tail(&bss[i].list, &rdev->bss_list);
    memcpy(bss[i].pub.bssid, ap, ETH_ALEN); bss[i].pub.bssid[5] += i;
    bss[i].pub.channel = &channel; bss[i].pub.signal = -4200;
    struct cfg80211_bss_ies *ies = calloc(1, sizeof(*ies) + 5); assert(ies);
    ies->len = 5; memcpy(ies->data, "\0\3abc", 5); bss[i].pub.ies = ies;
  }
  struct net_device *dev = alloc_netdev_mqs(0, "wlan0", 0, ether_setup, 1, 1); assert(dev);
  dev->netdev_ops = &net_ops; eth_hw_addr_set(dev, mac); dev->ieee80211_ptr = &wdev;
  wdev.wiphy = &rdev->wiphy; wdev.netdev = dev; wdev.iftype = NL80211_IFTYPE_STATION;
  assert(!register_netdevice(dev));
  const struct nw_control_transport transport = { .send = send_message, .wake = wake };
  assert(!nw_wifi_control_transport_set(&transport)); assert(!nl80211_init());
  const struct nw_cfg80211_observer callbacks = { .event = observe }; nw_cfg80211_observe(&callbacks);
  nl80211_notify_iface(rdev, &wdev, NL80211_CMD_NEW_INTERFACE);
  assert(!nw_wifi_control_attach(&socket_owner)); begin(NW_CONTROL_INFO); assert(exchange() == NW_CONTROL_OK && opens == 1);
  assert(registrations == 1);
  begin(NW_CONTROL_GET_CHANNELS); assert(exchange() == NW_CONTROL_OK);
  struct nw_control_view channels; size_t channel_length;
  assert(!nw_control_decode(last_reply, reply_length, &channels));
  const u8 *records = nw_control_get(&channels, NW_CONTROL_ATTR_CHANNELS, &channel_length);
  assert(channel_length == 16 && get_unaligned_le32(records) == 5180);
  begin(NW_CONTROL_SEND_MGMT); u8 frame[28] = {0xd0};
  nw_control_put(&request, NW_CONTROL_ATTR_FRAME, frame, sizeof(frame));
  nw_control_put_u32(&request, NW_CONTROL_ATTR_FREQUENCY, 5180);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_NO_ACK, 0); nw_control_put_u32(&request, NW_CONTROL_ATTR_NO_CCK, 0);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_WAIT, 0);
  assert(exchange() == NW_CONTROL_OK && management_frames == 1);
  begin(NW_CONTROL_AUTHENTICATE); bss_attributes(); nw_control_put_u32(&request, NW_CONTROL_ATTR_AUTH_TYPE, 0);
  assert(exchange() == NW_CONTROL_OK && auths == 1 && !bss_refs);
  begin(NW_CONTROL_ASSOCIATE); bss_attributes(); nw_control_put_u32(&request, NW_CONTROL_ATTR_PAIRWISE_CIPHER, WLAN_CIPHER_SUITE_CCMP);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_GROUP_CIPHER, WLAN_CIPHER_SUITE_CCMP); nw_control_put_u32(&request, NW_CONTROL_ATTR_AKM, WLAN_AKM_SUITE_PSK);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_MFP, 0); assert(exchange() == NW_CONTROL_OK && assocs == 1 && !bss_refs);
  begin(NW_CONTROL_SET_KEY); u8 key[16] = {1}; nw_control_put_u32(&request, NW_CONTROL_ATTR_KEY_INDEX, 0);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_KEY_FLAGS, NW_CONTROL_KEY_PAIRWISE); nw_control_put_u32(&request, NW_CONTROL_ATTR_CIPHER, WLAN_CIPHER_SUITE_CCMP);
  nw_control_put(&request, NW_CONTROL_ATTR_BSSID, ap, ETH_ALEN); nw_control_put(&request, NW_CONTROL_ATTR_KEY, key, sizeof(key));
  assert(exchange() == NW_CONTROL_OK && keys == 1);
  begin(NW_CONTROL_SET_KEY); nw_control_put_u32(&request, NW_CONTROL_ATTR_KEY_INDEX, 0);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_KEY_FLAGS, NW_CONTROL_KEY_PAIRWISE); nw_control_put_u32(&request, NW_CONTROL_ATTR_CIPHER, WLAN_CIPHER_SUITE_CCMP);
  nw_control_put(&request, NW_CONTROL_ATTR_BSSID, ap, ETH_ALEN); nw_control_put(&request, NW_CONTROL_ATTR_KEY, key, 15);
  assert(exchange() == NW_CONTROL_INVALID && keys == 1);
  begin(NW_CONTROL_SET_PORT); nw_control_put_u32(&request, NW_CONTROL_ATTR_AUTHORIZED, 1); assert(exchange() == NW_CONTROL_OK && ports == 1);
  begin(NW_CONTROL_TX_EAPOL); nw_control_put(&request, NW_CONTROL_ATTR_BSSID, ap, ETH_ALEN);
  nw_control_put(&request, NW_CONTROL_ATTR_EAPOL, "\2\3\0\0", 4); nw_control_put_u32(&request, NW_CONTROL_ATTR_ETHERTYPE, ETH_P_PAE);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_NO_ENCRYPT, 1); assert(exchange() == NW_CONTROL_OK && eapols == 1);
  begin(NW_CONTROL_GET_BSS); nw_control_put_u32(&request, NW_CONTROL_ATTR_BSS_INDEX, 0); assert(exchange() == NW_CONTROL_OK);
  ((struct cfg80211_bss_ies *)bss[1].pub.ies)->data[2] = 'z';
  begin(NW_CONTROL_GET_BSS); nw_control_put_u32(&request, NW_CONTROL_ATTR_BSS_INDEX, 1); assert(exchange() == NW_CONTROL_OK);
  struct nw_control_view view; size_t length; assert(!nw_control_decode(last_reply, reply_length, &view));
  const u8 *ies = nw_control_get(&view, NW_CONTROL_ATTR_IE, &length); assert(ies && length == 5 && ies[2] == 'a');
  begin(NW_CONTROL_SCAN); u8 ssids[33] = {3,'a','b','c'}; nw_control_put(&request, NW_CONTROL_ATTR_SCAN_SSIDS, ssids, sizeof(ssids));
  assert(exchange() == NW_CONTROL_OK); assert(rdev->scan_req);
  struct sk_buff *done = nl80211_build_scan_msg(rdev, &wdev, false); assert(done); nl80211_send_scan_msg(rdev, done);
  kfree(rdev->scan_req); rdev->scan_req = NULL; dev_put(dev);
  u64 previous = generation; assert(!nw_cfg80211_begin_attempt() && generation > previous && opens == 2 && stops == 1);
  begin(NW_CONTROL_SET_PORT); nw_control_reply_state(&request, previous, 0);
  nw_control_put_u32(&request, NW_CONTROL_ATTR_AUTHORIZED, 1); assert(exchange() == NW_CONTROL_STALE && ports == 1);
  assert(!nw_cfg80211_set_power(0) && stops == 2 && !netif_running(dev));
  begin(NW_CONTROL_INFO); assert(exchange() == NW_CONTROL_OK && opens == 2 && registrations == 1);
  begin(NW_CONTROL_AUTHENTICATE); bss_attributes(); nw_control_put_u32(&request, NW_CONTROL_ATTR_AUTH_TYPE, 0);
  assert(exchange() == NW_CONTROL_NOT_READY && opens == 2 && auths == 1);
  begin(NW_CONTROL_SET_PORT); nw_control_put_u32(&request, NW_CONTROL_ATTR_AUTHORIZED, 0);
  assert(exchange() == NW_CONTROL_OK && !netif_running(dev));
  assert(!nw_cfg80211_set_power(1) && opens == 3 && netif_running(dev));
  nw_wifi_control_detach(socket_owner); nw_wifi_control_poll(); lx_pump(); assert(!owner && stops == 3 && unregistrations == 1);
  nl80211_exit(); unregister_netdevice(dev); free_netdev(dev); lx_pump();
  for (unsigned i = 0; i < 2; i++) free((void *)bss[i].pub.ies);
  free(rdev); assert(allocations == frees && observed);
  puts("cfg80211 control: production broker/lifecycle, scan/auth/assoc mapping, upstream key validation, ordered EAPOL, stable BSS dump, stale-generation rejection, owner-close cleanup passed (radio/MLME simulated)");
  return 0;
}
