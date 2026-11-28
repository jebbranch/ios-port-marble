/* XNU control transport for the original cfg80211/mac80211 station operations.
 * Socket callbacks enqueue; this file executes only on the wireless worker. */
#include "lx_pci.h"
#include <net/cfg80211.h>
#include "cfg80211/core.h"
#include "cfg80211/rdev-ops.h"
#include "cfg80211/nl80211.h"
#include "cfg80211/trace.h"
#include "nw_control_endpoint.h"
#include "nw_cfg80211_control.h"

static struct net_device *primary;
static struct nw_cfg80211_observer observer;
static uint64_t owner, generation = 1;
static struct work_struct ready_work;
static bool sa_query_registered;
static bool radio_enabled = true;
struct bss_snapshot { struct bss_snapshot *next; size_t length; u8 bytes[]; };
static struct bss_snapshot *snapshot;

void nw_cfg80211_observe(const struct nw_cfg80211_observer *value) {
  if (value) observer = *value; else memset(&observer, 0, sizeof(observer));
}
struct net_device *nw_cfg80211_primary(void) { return primary; }
int nw_cfg80211_event_begin(struct nw_control_builder *b, void *bytes, size_t size, unsigned operation, int status) {
  return nw_control_begin(b, bytes, size, NW_CONTROL_EVENT, operation, 0, owner, generation, status);
}
int nw_cfg80211_event_send(struct nw_control_builder *builder) {
  struct nw_control_view view;
  if (nw_control_decode(builder->bytes, builder->length, &view)) return -EINVAL;
  if (view.header.session != owner || view.header.generation != generation) return NW_CONTROL_STALE;
  if (observer.event) observer.event(view.header.operation, view.header.status, &view);
  return owner ? nw_wifi_control_publish(owner, builder->bytes, builder->length) : NW_CONTROL_NOT_READY;
}
static void snapshot_free(void) {
  while (snapshot) { struct bss_snapshot *next = snapshot->next; kfree(snapshot); snapshot = next; }
}
static int common_error(int error) {
  switch (error) {
  case 0: return NW_CONTROL_OK;
  case -EINVAL: return NW_CONTROL_INVALID;
  case -EBUSY: case -EALREADY: return NW_CONTROL_BUSY;
  case -ENOENT: return NW_CONTROL_NOT_FOUND;
  case -ENOMEM: return NW_CONTROL_NO_MEMORY;
  case -ENETDOWN: case -ENODEV: case -ENOTCONN: case -ERFKILL: return NW_CONTROL_NOT_READY;
  case -EOPNOTSUPP: return NW_CONTROL_UNSUPPORTED;
  case -ETIMEDOUT: return NW_CONTROL_TIMED_OUT;
  default: return NW_CONTROL_DRIVER_FAILED;
  }
}
static const u8 *span(const struct nw_control_view *view, unsigned attribute, size_t exact) {
  size_t length; const u8 *value = nw_control_get(view, attribute, &length);
  return value && length == exact ? value : NULL;
}
static bool cipher_supported(struct wiphy *wiphy, u32 cipher) {
  for (int i = 0; i < wiphy->n_cipher_suites; i++) if (wiphy->cipher_suites[i] == cipher) return true;
  return false;
}
static int scan(struct cfg80211_registered_device *rdev, const struct nw_control_view *view) {
  struct wiphy *wiphy = &rdev->wiphy;
  size_t ssid_bytes, freq_bytes, ie_bytes;
  const u8 *ssids = nw_control_get(view, NW_CONTROL_ATTR_SCAN_SSIDS, &ssid_bytes);
  const u8 *freqs = nw_control_get(view, NW_CONTROL_ATTR_SCAN_FREQUENCIES, &freq_bytes);
  const u8 *ies = nw_control_get(view, NW_CONTROL_ATTR_IE, &ie_bytes);
  if (!rdev->ops->scan) return -EOPNOTSUPP;
  if (rdev->scan_req || rdev->scan_msg) return -EBUSY;
  if (ssid_bytes % 33 || ssid_bytes / 33 > wiphy->max_scan_ssids || freq_bytes % 4 ||
      freq_bytes / 4 > 256 || ie_bytes > wiphy->max_scan_ie_len) return -EINVAL;
  unsigned channels = freq_bytes ? freq_bytes / 4 : ieee80211_get_num_supported_channels(wiphy);
  if (!channels) return -EINVAL;
  size_t ssid_offset = sizeof(struct cfg80211_scan_request_int) + channels * sizeof(void *);
  size_t ie_offset = ssid_offset + (ssid_bytes / 33) * sizeof(struct cfg80211_ssid);
  struct cfg80211_scan_request_int *request = kzalloc(ie_offset + ie_bytes, GFP_KERNEL);
  if (!request) return -ENOMEM;
  request->req.ssids = (void *)((u8 *)request + ssid_offset);
  request->req.n_ssids = ssid_bytes / 33;
  for (unsigned i = 0; i < request->req.n_ssids; i++) {
    if (ssids[i * 33] > IEEE80211_MAX_SSID_LEN) { kfree(request); return -EINVAL; }
    request->req.ssids[i].ssid_len = ssids[i * 33];
    memcpy(request->req.ssids[i].ssid, ssids + i * 33 + 1, ssids[i * 33]);
  }
  for (unsigned i = 0; i < channels && freq_bytes; i++) {
    struct ieee80211_channel *channel = ieee80211_get_channel(wiphy, get_unaligned_le32(freqs + i * 4));
    if (!channel) { kfree(request); return -EINVAL; }
    if (!(channel->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_20MHZ)))
      request->req.channels[request->req.n_channels++] = channel;
  }
  for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
    struct ieee80211_supported_band *supported = wiphy->bands[band];
    if (!supported) continue;
    request->req.rates[band] = (1U << supported->n_bitrates) - 1;
    if (freq_bytes) continue;
    for (unsigned i = 0; i < supported->n_channels; i++)
      if (!(supported->channels[i].flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_20MHZ)))
        request->req.channels[request->req.n_channels++] = &supported->channels[i];
  }
  if (!request->req.n_channels) { kfree(request); return -EINVAL; }
  request->req.ie = (u8 *)request + ie_offset; request->req.ie_len = ie_bytes;
  if (ie_bytes) memcpy((u8 *)request->req.ie, ies, ie_bytes);
  eth_broadcast_addr(request->req.bssid);
  request->req.wdev = primary->ieee80211_ptr; request->req.wiphy = wiphy;
  request->req.scan_start = jiffies; request->req.tsf_report_link_id = -1;
  rdev->scan_req = request;
  int result = cfg80211_scan(rdev);
  if (result) { rdev->scan_req = NULL; kfree(request); return result; }
  dev_hold(primary); nl80211_send_scan_start(rdev, primary->ieee80211_ptr);
  return 0;
}
static int snapshot_build(struct cfg80211_registered_device *rdev) {
  snapshot_free(); struct bss_snapshot **tail = &snapshot;
  size_t memory = 0; unsigned count = 0;
  int result = 0;
  rcu_read_lock(); spin_lock_bh(&rdev->bss_lock);
  struct cfg80211_internal_bss *entry;
  list_for_each_entry(entry, &rdev->bss_list, list) {
    const struct cfg80211_bss_ies *ies = rcu_dereference(entry->pub.ies);
    if (!ies || !entry->pub.channel) continue;
    if (count == 256 || ies->len > 7800 || memory + sizeof(struct bss_snapshot) + ies->len + 192 > 512 * 1024) {
      result = -ENOBUFS; break;
    }
    struct bss_snapshot *item = kmalloc(sizeof(*item) + ies->len + 192, GFP_ATOMIC);
    if (!item) { result = -ENOMEM; break; }
    struct nw_control_builder b;
    nw_control_begin(&b, item->bytes, ies->len + 192, NW_CONTROL_REPLY, NW_CONTROL_GET_BSS, 1, owner, generation, 0);
    int error = nw_control_put(&b, NW_CONTROL_ATTR_BSSID, entry->pub.bssid, ETH_ALEN) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, entry->pub.channel->center_freq) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_SIGNAL, rdev->wiphy.signal_type == CFG80211_SIGNAL_TYPE_MBM ? entry->pub.signal / 100 : entry->pub.signal) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_CAPABILITY, entry->pub.capability) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_BEACON_INTERVAL, entry->pub.beacon_interval) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_AGE, jiffies_to_msecs(jiffies - entry->ts)) ||
      nw_control_put(&b, NW_CONTROL_ATTR_IE, ies->data, ies->len);
    if (error) { kfree(item); result = -ENOBUFS; break; }
    item->length = b.length; item->next = NULL; *tail = item; tail = &item->next;
    memory += sizeof(*item) + ies->len + 192; count++;
  }
  spin_unlock_bh(&rdev->bss_lock); rcu_read_unlock();
  if (result) snapshot_free();
  return result;
}
static int get_bss(struct cfg80211_registered_device *rdev, const struct nw_control_view *request, struct nw_control_builder *reply) {
  uint32_t index;
  if (nw_control_get_u32(request, NW_CONTROL_ATTR_BSS_INDEX, &index)) return NW_CONTROL_INVALID;
  if (!index) { int result = snapshot_build(rdev); if (result) return common_error(result); }
  struct bss_snapshot *item = snapshot;
  while (item && index--) item = item->next;
  if (!item) { snapshot_free(); return NW_CONTROL_NOT_FOUND; }
  struct nw_control_view view;
  if (nw_control_decode(item->bytes, item->length, &view)) return NW_CONTROL_DRIVER_FAILED;
  for (unsigned attr = 1; attr < NW_CONTROL_ATTR_COUNT; attr++) {
    size_t length; const void *bytes = nw_control_get(&view, attr, &length);
    if (bytes && nw_control_put(reply, attr, bytes, length)) return NW_CONTROL_NO_MEMORY;
  }
  return NW_CONTROL_OK;
}
static struct cfg80211_bss *find_bss(struct cfg80211_registered_device *rdev, const struct nw_control_view *request) {
  size_t ssid_length; const u8 *ssid = nw_control_get(request, NW_CONTROL_ATTR_SSID, &ssid_length);
  const u8 *bssid = span(request, NW_CONTROL_ATTR_BSSID, ETH_ALEN);
  uint32_t frequency;
  if (!bssid || !is_valid_ether_addr(bssid) || !ssid || !ssid_length || ssid_length > 32 ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_FREQUENCY, &frequency)) return NULL;
  struct ieee80211_channel *channel = ieee80211_get_channel(&rdev->wiphy, frequency);
  if (!channel || (channel->flags & IEEE80211_CHAN_DISABLED)) return NULL;
  return cfg80211_get_bss(&rdev->wiphy, channel, bssid, ssid, ssid_length, IEEE80211_BSS_TYPE_ESS, IEEE80211_PRIVACY_ANY);
}
static int authenticate(struct cfg80211_registered_device *rdev, const struct nw_control_view *request) {
  uint32_t auth, local = 0;
  if (!rdev->ops->auth) return -EOPNOTSUPP;
  if (nw_control_get_u32(request, NW_CONTROL_ATTR_AUTH_TYPE, &auth) || auth != NL80211_AUTHTYPE_OPEN_SYSTEM) return -EINVAL;
  nw_control_get_u32(request, NW_CONTROL_ATTR_LOCAL_ONLY, &local);
  if (local) return 0; /* cfg80211 no longer tracks local authentication state. */
  struct cfg80211_auth_request auth_request = { .auth_type = auth, .link_id = -1 };
  auth_request.bss = find_bss(rdev, request);
  if (!auth_request.bss) return -ENOENT;
  auth_request.ie = nw_control_get(request, NW_CONTROL_ATTR_IE, &auth_request.ie_len);
  int result = cfg80211_mlme_auth(rdev, primary, &auth_request);
  cfg80211_put_bss(&rdev->wiphy, auth_request.bss);
  return result;
}
static int associate(struct cfg80211_registered_device *rdev, const struct nw_control_view *request) {
  if (!rdev->ops->assoc) return -EOPNOTSUPP;
  uint32_t pairwise, group, akm, mfp;
  if (nw_control_get_u32(request, NW_CONTROL_ATTR_PAIRWISE_CIPHER, &pairwise) ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_GROUP_CIPHER, &group) ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_AKM, &akm) ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_MFP, &mfp) || mfp > 1) return -EINVAL;
  if ((pairwise && !cipher_supported(&rdev->wiphy, pairwise)) ||
      (group && !cipher_supported(&rdev->wiphy, group)) ||
      (akm && akm != WLAN_AKM_SUITE_PSK && akm != WLAN_AKM_SUITE_PSK_SHA256)) return -EOPNOTSUPP;
  struct cfg80211_assoc_request assoc_request = { .link_id = -1, .use_mfp = mfp };
  assoc_request.bss = find_bss(rdev, request);
  if (!assoc_request.bss) return -ENOENT;
  assoc_request.ie = nw_control_get(request, NW_CONTROL_ATTR_IE, &assoc_request.ie_len);
  assoc_request.prev_bssid = span(request, NW_CONTROL_ATTR_PREVIOUS_BSSID, ETH_ALEN);
  assoc_request.crypto.wpa_versions = pairwise ? NL80211_WPA_VERSION_2 : 0;
  assoc_request.crypto.cipher_group = group;
  if (pairwise) { assoc_request.crypto.n_ciphers_pairwise = 1; assoc_request.crypto.ciphers_pairwise[0] = pairwise; }
  if (akm) { assoc_request.crypto.n_akm_suites = 1; assoc_request.crypto.akm_suites[0] = akm; }
  assoc_request.crypto.control_port = true;
  assoc_request.crypto.control_port_ethertype = cpu_to_be16(ETH_P_PAE);
  assoc_request.crypto.control_port_no_encrypt = true;
  assoc_request.crypto.control_port_over_nl80211 = true;
  assoc_request.crypto.control_port_no_preauth = true;
  int result = cfg80211_mlme_assoc(rdev, primary, &assoc_request, NULL);
  if (!result) {
    /* Upstream uses a nonzero owner marker to route control-port frames.
     * The actual 64-bit owner is checked by the native broker above. */
    primary->ieee80211_ptr->conn_owner_nlportid = 1;
    memcpy(primary->ieee80211_ptr->disconnect_bssid, assoc_request.bss->bssid, ETH_ALEN);
  }
  cfg80211_put_bss(&rdev->wiphy, assoc_request.bss);
  return result;
}
static int set_key(struct cfg80211_registered_device *rdev, const struct nw_control_view *request) {
  uint32_t index, flags, cipher;
  if (nw_control_get_u32(request, NW_CONTROL_ATTR_KEY_INDEX, &index) ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_KEY_FLAGS, &flags) ||
      nw_control_get_u32(request, NW_CONTROL_ATTR_CIPHER, &cipher) || index > 7 || flags & ~15U) return -EINVAL;
  bool pairwise = flags & NW_CONTROL_KEY_PAIRWISE;
  if ((pairwise && (flags & NW_CONTROL_KEY_DEFAULT_MGMT)) ||
      ((flags & NW_CONTROL_KEY_DEFAULT_MGMT) && index < 4)) return -EINVAL;
  const u8 *address = pairwise ? span(request, NW_CONTROL_ATTR_BSSID, ETH_ALEN) : NULL;
  if (pairwise && (!address || !is_valid_ether_addr(address))) return -EINVAL;
  if (!cfg80211_valid_key_idx(rdev, index, pairwise)) return -EINVAL;
  if (flags & NW_CONTROL_KEY_DELETE) return rdev->ops->del_key ? rdev_del_key(rdev, primary, -1, index, pairwise, address) : -EOPNOTSUPP;
  if (!rdev->ops->add_key || !cipher_supported(&rdev->wiphy, cipher)) return -EOPNOTSUPP;
  size_t key_length, sequence_length;
  struct key_params key = { .cipher = cipher, .mode = NL80211_KEY_RX_TX };
  key.key = (void *)nw_control_get(request, NW_CONTROL_ATTR_KEY, &key_length);
  key.seq = (void *)nw_control_get(request, NW_CONTROL_ATTR_KEY_SEQUENCE, &sequence_length);
  if (!key.key || key_length > 32 || sequence_length > 16) return -EINVAL;
  key.key_len = key_length; key.seq_len = sequence_length;
  int result = cfg80211_validate_key_settings(rdev, &key, index, pairwise, address);
  if (!result) result = rdev_add_key(rdev, primary, -1, index, pairwise, address, &key);
  if (!result && !pairwise && (flags & NW_CONTROL_KEY_DEFAULT_TX))
    result = rdev->ops->set_default_key ? rdev_set_default_key(rdev, primary, -1, index, false, true) : -EOPNOTSUPP;
  if (!result && (flags & NW_CONTROL_KEY_DEFAULT_MGMT))
    result = rdev->ops->set_default_mgmt_key ? rdev_set_default_mgmt_key(rdev, primary, -1, index) : -EOPNOTSUPP;
  return result;
}
static int request_locked(struct cfg80211_registered_device *rdev, const struct nw_control_view *request, struct nw_control_builder *reply) {
  uint32_t value;
  switch (request->header.operation) {
  case NW_CONTROL_INFO: {
    if (!sa_query_registered && cipher_supported(&rdev->wiphy, WLAN_CIPHER_SUITE_AES_CMAC)) {
      static const u8 response_match[] = { WLAN_CATEGORY_SA_QUERY, WLAN_ACTION_SA_QUERY_RESPONSE };
      int result = cfg80211_mlme_register_mgmt(primary->ieee80211_ptr, 1,
        IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION, response_match, sizeof(response_match), false, NULL);
      if (result) return common_error(result);
      sa_query_registered = true;
    }
    u8 ciphers[128]; unsigned count = rdev->wiphy.n_cipher_suites;
    if (count > sizeof(ciphers) / 4) return NW_CONTROL_UNSUPPORTED;
    for (unsigned i = 0; i < count; i++) put_unaligned_le32(rdev->wiphy.cipher_suites[i], ciphers + i * 4);
    if (nw_control_put(reply, NW_CONTROL_ATTR_MAC, primary->dev_addr, ETH_ALEN) ||
        nw_control_put(reply, NW_CONTROL_ATTR_CIPHERS, ciphers, count * 4) ||
        nw_control_put_u32(reply, NW_CONTROL_ATTR_MAX_SCAN_SSIDS, rdev->wiphy.max_scan_ssids) ||
        nw_control_put_u32(reply, NW_CONTROL_ATTR_MAX_SCAN_IE, rdev->wiphy.max_scan_ie_len) ||
        nw_control_put_u32(reply, NW_CONTROL_ATTR_OPERSTATE, radio_enabled)) return NW_CONTROL_NO_MEMORY;
    return NW_CONTROL_OK;
  }
  case NW_CONTROL_GET_CHANNELS: {
    unsigned count = ieee80211_get_num_supported_channels(&rdev->wiphy);
    if (count > 512) return NW_CONTROL_UNSUPPORTED;
    u8 *records = kmalloc(max_t(size_t, count * 16, 1), GFP_KERNEL);
    if (!records) return NW_CONTROL_NO_MEMORY;
    unsigned at = 0;
    for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
      struct ieee80211_supported_band *supported = rdev->wiphy.bands[band];
      if (!supported) continue;
      for (unsigned i = 0; i < supported->n_channels; i++, at++) {
        struct ieee80211_channel *channel = &supported->channels[i];
        put_unaligned_le32(channel->center_freq, records + at * 16);
        put_unaligned_le32(channel->flags, records + at * 16 + 4);
        put_unaligned_le32(channel->max_power, records + at * 16 + 8);
        put_unaligned_le32(band, records + at * 16 + 12);
      }
    }
    int result = nw_control_put(reply, NW_CONTROL_ATTR_CHANNELS, records, at * 16);
    kfree(records); return result ? NW_CONTROL_NO_MEMORY : NW_CONTROL_OK;
  }
  case NW_CONTROL_SCAN: return common_error(scan(rdev, request));
  case NW_CONTROL_GET_BSS: return get_bss(rdev, request, reply);
  case NW_CONTROL_AUTHENTICATE: {
    int result = authenticate(rdev, request);
    if (!result && observer.event) observer.event(NW_CONTROL_AUTHENTICATE, 0, request);
    return common_error(result);
  }
  case NW_CONTROL_ASSOCIATE: return common_error(associate(rdev, request));
  case NW_CONTROL_SET_KEY: return common_error(set_key(rdev, request));
  case NW_CONTROL_DEAUTHENTICATE: {
    const u8 *address = span(request, NW_CONTROL_ATTR_BSSID, ETH_ALEN);
    if (!address || nw_control_get_u32(request, NW_CONTROL_ATTR_REASON, &value) || !value || value > 65535) return NW_CONTROL_INVALID;
    return common_error(rdev->ops->deauth ? cfg80211_mlme_deauth(rdev, primary, address, NULL, 0, value, false) : -EOPNOTSUPP);
  }
  case NW_CONTROL_SET_PORT: {
    if (nw_control_get_u32(request, NW_CONTROL_ATTR_AUTHORIZED, &value) || value > 1) return NW_CONTROL_INVALID;
    if (!primary->ieee80211_ptr->connected) return value ? NW_CONTROL_NOT_READY : NW_CONTROL_OK;
    if (!rdev->ops->change_station) return NW_CONTROL_UNSUPPORTED;
    struct station_parameters parameters = { .listen_interval = -1, .support_p2p_ps = -1,
      .sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED), .sta_flags_set = value ? BIT(NL80211_STA_FLAG_AUTHORIZED) : 0 };
    int result = rdev_change_station(rdev, primary, primary->ieee80211_ptr->u.client.connected_addr, &parameters);
    if (!result && observer.event) observer.event(NW_CONTROL_SET_PORT, value, request);
    return common_error(result);
  }
  case NW_CONTROL_TX_EAPOL: {
    uint32_t protocol, no_encrypt; size_t length; const void *payload = nw_control_get(request, NW_CONTROL_ATTR_EAPOL, &length);
    const u8 *destination = span(request, NW_CONTROL_ATTR_BSSID, ETH_ALEN);
    if (!destination || !payload || length < 4 || length > 4096 ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_ETHERTYPE, &protocol) || protocol != ETH_P_PAE ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_NO_ENCRYPT, &no_encrypt) || no_encrypt > 1) return NW_CONTROL_INVALID;
    if (!primary->ieee80211_ptr->connected) return NW_CONTROL_NOT_READY;
    if (!rdev->ops->tx_control_port) return NW_CONTROL_UNSUPPORTED;
    u64 cookie = 0;
    int result = rdev_tx_control_port(rdev, primary, payload, length, destination, cpu_to_be16(protocol), no_encrypt, -1, &cookie);
    if (!result && nw_control_put_u64(reply, NW_CONTROL_ATTR_COOKIE, cookie)) return NW_CONTROL_NO_MEMORY;
    return common_error(result);
  }
  case NW_CONTROL_SET_OPERSTATE:
    if (nw_control_get_u32(request, NW_CONTROL_ATTR_OPERSTATE, &value) || value > 1) return NW_CONTROL_INVALID;
    if (observer.event) observer.event(NW_CONTROL_SET_OPERSTATE, value, request);
    return NW_CONTROL_OK;
  case NW_CONTROL_SEND_MGMT: {
    uint32_t frequency, no_ack, wait, no_cck; size_t length;
    const u8 *frame = nw_control_get(request, NW_CONTROL_ATTR_FRAME, &length);
    if (!frame || length < 25 || length > 4096 ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_FREQUENCY, &frequency) ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_NO_ACK, &no_ack) || no_ack > 1 ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_NO_CCK, &no_cck) || no_cck > 1 ||
        nw_control_get_u32(request, NW_CONTROL_ATTR_WAIT, &wait) || wait > 5000) return NW_CONTROL_INVALID;
    struct cfg80211_mgmt_tx_params params = { .buf = frame, .len = length, .wait = wait,
      .dont_wait_for_ack = no_ack, .no_cck = no_cck, .link_id = -1 };
    params.chan = ieee80211_get_channel(&rdev->wiphy, frequency);
    if (!params.chan || (params.chan->flags & IEEE80211_CHAN_DISABLED)) return NW_CONTROL_INVALID;
    u64 cookie = 0;
    int result = cfg80211_mlme_mgmt_tx(rdev, primary->ieee80211_ptr, &params, &cookie);
    if (!result && nw_control_put_u64(reply, NW_CONTROL_ATTR_COOKIE, cookie)) return NW_CONTROL_NO_MEMORY;
    return common_error(result);
  }
  case NW_CONTROL_REPORT_RESULT:
    if (nw_control_get_u32(request, NW_CONTROL_ATTR_STATUS_CODE, &value) || value >= NW_CONTROL_STATUS_MAX) return NW_CONTROL_INVALID;
    if (observer.event) observer.event(NW_CONTROL_REPORT_RESULT, value, request);
    return NW_CONTROL_OK;
  default: return NW_CONTROL_UNSUPPORTED;
  }
}
static int native_request(void *unused, uint64_t session, const struct nw_control_view *request, struct nw_control_builder *reply) {
  (void)unused;
  nw_control_reply_state(reply, generation, 0);
  if (session != owner) return NW_CONTROL_STALE;
  if (!primary || !primary->registered) return NW_CONTROL_NOT_READY;
  if (request->header.operation != NW_CONTROL_INFO && request->header.generation != generation) return NW_CONTROL_STALE;
  if (!radio_enabled && request->header.operation != NW_CONTROL_INFO && request->header.operation != NW_CONTROL_GET_BSS &&
      request->header.operation != NW_CONTROL_GET_CHANNELS) {
    uint32_t value;
    if (request->header.operation == NW_CONTROL_SET_OPERSTATE &&
        !nw_control_get_u32(request, NW_CONTROL_ATTR_OPERSTATE, &value) && !value) return NW_CONTROL_OK;
    if (request->header.operation == NW_CONTROL_SET_PORT &&
        !nw_control_get_u32(request, NW_CONTROL_ATTR_AUTHORIZED, &value) && !value) return NW_CONTROL_OK;
    if (request->header.operation == NW_CONTROL_SET_KEY &&
        !nw_control_get_u32(request, NW_CONTROL_ATTR_KEY_FLAGS, &value) && (value & NW_CONTROL_KEY_DELETE)) return NW_CONTROL_OK;
    return NW_CONTROL_NOT_READY;
  }
  struct cfg80211_registered_device *rdev = wiphy_to_rdev(primary->ieee80211_ptr->wiphy);
  rtnl_lock();
  /* ndo_open and netdev notifiers take the wiphy mutex internally. */
  int result = radio_enabled ? dev_open(primary, NULL) : 0;
  if (!result) {
    wiphy_lock(&rdev->wiphy);
    result = request_locked(rdev, request, reply);
    wiphy_unlock(&rdev->wiphy);
  } else result = common_error(result);
  rtnl_unlock();
  return result;
}
static void opened(void *unused, uint64_t session) { (void)unused; owner = session; generation++; snapshot_free(); }
static void closed(void *unused, uint64_t session) {
  (void)unused;
  if (owner != session) return;
  owner = 0; generation++; snapshot_free();
  if (primary) {
    rtnl_lock();
    wiphy_lock(primary->ieee80211_ptr->wiphy);
    cfg80211_mlme_unregister_socket(primary->ieee80211_ptr, 1); sa_query_registered = false;
    wiphy_unlock(primary->ieee80211_ptr->wiphy);
    dev_close(primary); rtnl_unlock();
  }
}
int nw_cfg80211_begin_attempt(void) {
  if (!owner || !primary) return -ENOTCONN;
  if (!radio_enabled) return -ERFKILL;
  rtnl_lock(); dev_close(primary); generation++; snapshot_free();
  int result = dev_open(primary, NULL); rtnl_unlock();
  return result;
}
int nw_cfg80211_set_power(int enabled) {
  if (!primary) return -ENODEV;
  if (radio_enabled == !!enabled && (!enabled || netif_running(primary))) return 0;
  rtnl_lock();
  if (!enabled) { radio_enabled = false; dev_close(primary); }
  generation++; snapshot_free();
  int result = enabled ? dev_open(primary, NULL) : 0;
  if (enabled) radio_enabled = result == 0;
  rtnl_unlock(); return result;
}
static void ready(struct work_struct *work) {
  (void)work;
  if (primary && observer.ready) observer.ready(primary);
}
int nl80211_init(void) {
  radio_enabled = true; sa_query_registered = false;
  INIT_WORK(&ready_work, ready);
  const struct nw_control_dispatch dispatch = { .opened = opened, .closed = closed, .request = native_request };
  int result = nw_wifi_control_bind(&dispatch);
  return result == NW_CONTROL_OK ? 0 : -EBUSY;
}
void nl80211_exit(void) {
  nw_wifi_control_shutdown(); cancel_work_sync(&ready_work); snapshot_free();
  if (primary) { dev_put(primary); primary = NULL; }
  owner = 0;
}
void nl80211_notify_iface(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev, enum nl80211_commands command) {
  (void)rdev;
  if (command == NL80211_CMD_NEW_INTERFACE && !primary && wdev->iftype == NL80211_IFTYPE_STATION && wdev->netdev) {
    primary = wdev->netdev; dev_hold(primary); queue_work(system_wq, &ready_work);
  } else if (command == NL80211_CMD_DEL_INTERFACE && primary == wdev->netdev) {
    cancel_work_sync(&ready_work); dev_put(primary); primary = NULL; sa_query_registered = false; snapshot_free();
  }
}
static void simple_event(unsigned operation, int status) {
  u8 bytes[64]; struct nw_control_builder b;
  if (!nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), operation, status)) nw_cfg80211_event_send(&b);
}
void nl80211_send_scan_start(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev) {
  (void)rdev;
  if (primary && wdev == primary->ieee80211_ptr) simple_event(NW_CONTROL_SCAN_STARTED, 0);
}
struct sk_buff *nl80211_build_scan_msg(struct cfg80211_registered_device *rdev, struct wireless_dev *wdev, bool aborted) {
  (void)rdev;
  if (!primary || wdev != primary->ieee80211_ptr) return NULL;
  struct sk_buff *message = alloc_skb(NW_CONTROL_HEADER_SIZE, GFP_KERNEL);
  if (!message) return NULL;
  struct nw_control_builder b;
  nw_cfg80211_event_begin(&b, skb_put(message, NW_CONTROL_HEADER_SIZE), NW_CONTROL_HEADER_SIZE,
                          NW_CONTROL_SCAN_DONE, aborted ? NW_CONTROL_DRIVER_FAILED : 0);
  return message;
}
void nl80211_send_scan_msg(struct cfg80211_registered_device *rdev, struct sk_buff *message) {
  (void)rdev;
  if (!message) return;
  struct nw_control_builder b = { .bytes = message->data, .length = message->len, .capacity = message->len };
  /* A scan may finish after a client reconnect. Never retag its old owner. */
  struct nw_control_view view;
  if (!nw_control_decode(message->data, message->len, &view) && view.header.generation == generation && view.header.session == owner)
    nw_cfg80211_event_send(&b);
  kfree_skb(message);
}
void nl80211_send_rx_auth(struct cfg80211_registered_device *rdev, struct net_device *dev, const u8 *frame, size_t length, gfp_t gfp) {
  (void)rdev; (void)gfp;
  if (dev != primary || length < 30 || length > 4096) return;
  u8 *bytes = kmalloc(length + 128, gfp); struct nw_control_builder b;
  if (!bytes) return;
  if (!nw_cfg80211_event_begin(&b, bytes, length + 128, NW_CONTROL_AUTH_FRAME, 0) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_FRAME, frame, length)) nw_cfg80211_event_send(&b);
  memzero_explicit(bytes, length + 128); kfree(bytes);
}
void nl80211_send_rx_assoc(struct cfg80211_registered_device *rdev, struct net_device *dev, const struct cfg80211_rx_assoc_resp_data *data) {
  (void)rdev;
  if (dev != primary || data->len < 30 || data->len > 4096 || data->req_ies_len > 4096) return;
  size_t capacity = data->len + data->req_ies_len + 192;
  u8 *bytes = kmalloc(capacity, GFP_KERNEL); struct nw_control_builder b;
  if (!bytes) return;
  struct cfg80211_bss *bss = data->links[0].bss;
  if (!nw_cfg80211_event_begin(&b, bytes, capacity, NW_CONTROL_ASSOC_RESULT, 0) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_BSSID, data->buf + 16, ETH_ALEN) &&
      !nw_control_put_u32(&b, NW_CONTROL_ATTR_STATUS_CODE, get_unaligned_le16(data->buf + 26)) &&
      !nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, bss && bss->channel ? bss->channel->center_freq : 0) &&
      (!bss || !nw_control_put_u32(&b, NW_CONTROL_ATTR_SIGNAL,
          rdev->wiphy.signal_type == CFG80211_SIGNAL_TYPE_MBM ? bss->signal / 100 : bss->signal)) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_IE, data->buf + 30, data->len - 30) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_REQUEST_IE, data->req_ies, data->req_ies_len)) nw_cfg80211_event_send(&b);
  memzero_explicit(bytes, capacity); kfree(bytes);
}
static void timeout_event(struct net_device *dev, const u8 *address, unsigned operation) {
  if (dev != primary) return;
  u8 bytes[96]; struct nw_control_builder b;
  if (!nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), operation, NW_CONTROL_TIMED_OUT) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_BSSID, address, ETH_ALEN)) nw_cfg80211_event_send(&b);
}
void nl80211_send_auth_timeout(struct cfg80211_registered_device *rdev, struct net_device *dev, const u8 *address, gfp_t gfp) {
  (void)rdev; (void)gfp; timeout_event(dev, address, NW_CONTROL_AUTH_TIMEOUT);
}
void nl80211_send_assoc_timeout(struct cfg80211_registered_device *rdev, struct net_device *dev, const u8 *address, gfp_t gfp) {
  (void)rdev; (void)gfp; timeout_event(dev, address, NW_CONTROL_ASSOC_TIMEOUT);
}
static void disconnected(struct net_device *dev, const u8 *address, u16 reason, bool local) {
  if (dev != primary) return;
  u8 bytes[128]; struct nw_control_builder b;
  if (!nw_cfg80211_event_begin(&b, bytes, sizeof(bytes), NW_CONTROL_DISCONNECTED, 0) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_BSSID, address, ETH_ALEN) &&
      !nw_control_put_u32(&b, NW_CONTROL_ATTR_REASON, reason) &&
      !nw_control_put_u32(&b, NW_CONTROL_ATTR_LOCAL_ONLY, local)) nw_cfg80211_event_send(&b);
}
void nl80211_send_deauth(struct cfg80211_registered_device *rdev, struct net_device *dev, const u8 *frame, size_t length, bool reconnect, gfp_t gfp) {
  (void)rdev; (void)reconnect; (void)gfp;
  if (length >= 26) disconnected(dev, frame + 16, get_unaligned_le16(frame + 24), ether_addr_equal(frame + 10, dev->dev_addr));
}
void nl80211_send_disassoc(struct cfg80211_registered_device *rdev, struct net_device *dev, const u8 *frame, size_t length, bool reconnect, gfp_t gfp) {
  nl80211_send_deauth(rdev, dev, frame, length, reconnect, gfp);
}
void nl80211_send_disconnected(struct cfg80211_registered_device *rdev, struct net_device *dev, u16 reason, const u8 *ie, size_t length, bool from_ap) {
  (void)rdev; (void)ie; (void)length;
  disconnected(dev, dev->ieee80211_ptr->u.client.connected_addr, reason, !from_ap);
}
bool cfg80211_rx_control_port(struct net_device *dev, struct sk_buff *skb, bool unencrypted, int link_id) {
  if (dev != primary || !owner || link_id >= 0 || !skb_mac_header_was_set(skb) ||
      skb->protocol != cpu_to_be16(ETH_P_PAE) || skb->len < 4 || skb->len > 4096) return false;
  const struct ethhdr *ethernet = (const void *)skb_mac_header(skb);
  size_t capacity = skb->len + 192, allocation = capacity + skb->len;
  u8 *bytes = kmalloc(allocation, GFP_ATOMIC); struct nw_control_builder b;
  if (!bytes) return false;
  u8 *payload = bytes + capacity;
  if (skb_copy_bits(skb, 0, payload, skb->len)) { kfree(bytes); return false; }
  int result = nw_cfg80211_event_begin(&b, bytes, capacity, NW_CONTROL_RX_EAPOL, 0) ||
    nw_control_put(&b, NW_CONTROL_ATTR_BSSID, ethernet->h_source, ETH_ALEN) ||
    nw_control_put_u32(&b, NW_CONTROL_ATTR_NO_ENCRYPT, unencrypted) ||
    nw_control_put(&b, NW_CONTROL_ATTR_EAPOL, payload, skb->len);
  if (!result) result = nw_cfg80211_event_send(&b);
  memzero_explicit(bytes, allocation); kfree(bytes);
  return result == 0;
}
void cfg80211_control_port_tx_status(struct wireless_dev *wdev, u64 cookie, const u8 *frame, size_t length, bool ack, gfp_t gfp) {
  (void)gfp;
  if (!primary || wdev != primary->ieee80211_ptr || length < ETH_HLEN || length > 4110) return;
  size_t capacity = length + 128;
  u8 *bytes = kmalloc(capacity, gfp); struct nw_control_builder b;
  if (!bytes) return;
  if (!nw_cfg80211_event_begin(&b, bytes, capacity, NW_CONTROL_EAPOL_STATUS, 0) &&
      !nw_control_put(&b, NW_CONTROL_ATTR_FRAME, frame, length) &&
      !nw_control_put_u32(&b, NW_CONTROL_ATTR_ACKED, ack) &&
      !nw_control_put_u64(&b, NW_CONTROL_ATTR_COOKIE, cookie)) nw_cfg80211_event_send(&b);
  memzero_explicit(bytes, capacity); kfree(bytes);
}
