/* XNU control-port backend for unchanged wpa_supplicant authentication code. */
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "driver.h"
#include "nw_control_client.h"
#include "nw_supplicant_frontend.h"

struct sheng_driver {
  void *context;
  struct nw_control_client *client;
  u8 address[ETH_ALEN], bssid[ETH_ALEN], ssid[SSID_MAX_LEN];
  size_t ssid_length;
  struct wpa_driver_capa capability;
  uint64_t generation;
  unsigned references;
  unsigned join_frequency;
  int closing, failed, drain_scheduled;
};
static void sheng_drain(void *context, void *unused);
static void sheng_read(int fd, void *context, void *unused);
void nw_sheng_wireless_event(void *context, const struct nw_control_view *view);
static void sheng_release(struct sheng_driver *driver)
{
  if (--driver->references) return;
  nw_control_client_close(driver->client);
  forced_memzero(driver, sizeof(*driver));
  os_free(driver);
}
static void sheng_schedule(struct sheng_driver *driver)
{
  if (driver->closing || driver->drain_scheduled) return;
  if (!eloop_register_timeout(0, 0, sheng_drain, driver, NULL))
    driver->drain_scheduled = 1;
  else
    eloop_terminate();
}
static int sheng_rpc(struct sheng_driver *driver,
                     struct nw_control_builder *request,
                     struct nw_control_view *reply)
{
  int result = nw_control_client_exchange(driver->client, request, reply, 3000);
  if (nw_control_client_failed(driver->client)) {
    if (!driver->failed) {
      driver->failed = 1;
      eloop_unregister_read_sock(nw_control_client_fd(driver->client));
      /* Let launchd restart and obtain a new ownership session. */
      eloop_terminate();
    }
  } else if (nw_control_client_has_events(driver->client)) {
    sheng_schedule(driver);
  }
  return result == NW_CONTROL_OK ? 0 : -1;
}
static int sheng_begin(struct sheng_driver *driver, struct nw_control_builder *b,
                        void *bytes, size_t capacity, unsigned operation)
{
  if (driver->closing || driver->failed) return -1;
  return nw_control_client_begin(driver->client, b, bytes, capacity, operation);
}
static int sheng_invalid(struct nw_control_builder *builder)
{
  nw_control_clear(builder->bytes, builder->capacity);
  return -1;
}
static const u8 *sheng_span(const struct nw_control_view *view, unsigned attr,
                           size_t exact)
{
  size_t length;
  const u8 *data = nw_control_get(view, attr, &length);
  return data && length == exact ? data : NULL;
}
static u32 sheng_value(const struct nw_control_view *view, unsigned attr, u32 fallback)
{
  uint32_t value;
  return nw_control_get_u32(view, attr, &value) ? fallback : value;
}
static int sheng_u32(void *priv, unsigned operation, unsigned attr, u32 value)
{
  struct sheng_driver *driver = priv;
  u8 bytes[96]; struct nw_control_builder b; struct nw_control_view reply;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), operation)) return -1;
  if (nw_control_put_u32(&b, attr, value)) return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static int sheng_get_bssid(void *priv, u8 *bssid)
{
  os_memcpy(bssid, ((struct sheng_driver *)priv)->bssid, ETH_ALEN);
  return is_zero_ether_addr(bssid) ? -1 : 0;
}
static int sheng_get_ssid(void *priv, u8 *ssid)
{
  struct sheng_driver *driver = priv;
  os_memcpy(ssid, driver->ssid, driver->ssid_length);
  return driver->ssid_length;
}
static const u8 *sheng_get_mac(void *priv)
{
  return ((struct sheng_driver *)priv)->address;
}
static int sheng_get_capa(void *priv, struct wpa_driver_capa *capability)
{
  *capability = ((struct sheng_driver *)priv)->capability;
  return 0;
}
static int sheng_scan(void *priv, struct wpa_driver_scan_params *parameters)
{
  struct sheng_driver *driver = priv;
  u8 bytes[8192], ssids[WPAS_MAX_SCAN_SSIDS * 33], frequencies[256 * 4];
  struct nw_control_builder b; struct nw_control_view reply;
  size_t count = 0;
  if (parameters->num_ssids > WPAS_MAX_SCAN_SSIDS ||
      parameters->num_ssids > (size_t)driver->capability.max_scan_ssids ||
      parameters->mac_addr_rand || parameters->p2p_probe) return -1;
  os_memset(ssids, 0, sizeof(ssids));
  for (size_t i = 0; i < parameters->num_ssids; i++) {
    size_t length = parameters->ssids[i].ssid_len;
    if (length > SSID_MAX_LEN) return -1;
    ssids[i * 33] = length;
    if (length) os_memcpy(ssids + i * 33 + 1, parameters->ssids[i].ssid, length);
  }
  if (parameters->freqs) {
    while (parameters->freqs[count]) {
      if (count == 256 || parameters->freqs[count] <= 0) return -1;
      WPA_PUT_LE32(frequencies + count * 4, parameters->freqs[count]); count++;
    }
  }
  if (!count && driver->join_frequency) {
    WPA_PUT_LE32(frequencies, driver->join_frequency); count = 1;
  }
  // The hint belongs to this first scan only; retries use upstream's search.
  driver->join_frequency = 0;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_SCAN)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_SCAN_SSIDS, ssids, parameters->num_ssids * 33) ||
      nw_control_put(&b, NW_CONTROL_ATTR_SCAN_FREQUENCIES, frequencies, count * 4) ||
      nw_control_put(&b, NW_CONTROL_ATTR_IE, parameters->extra_ies, parameters->extra_ies_len))
    return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static struct wpa_scan_results *sheng_scan_results(void *priv)
{
  struct sheng_driver *driver = priv;
  struct wpa_scan_results *results = os_zalloc(sizeof(*results));
  if (!results) return NULL;
  /* The kernel freezes a BSS snapshot until this indexed dump ends. */
  for (unsigned index = 0; index < 4096; index++) {
    u8 bytes[96]; struct nw_control_builder b; struct nw_control_view reply;
    if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_GET_BSS) ||
        nw_control_put_u32(&b, NW_CONTROL_ATTR_BSS_INDEX, index)) goto fail;
    int status = nw_control_client_exchange(driver->client, &b, &reply, 3000);
    if (nw_control_client_has_events(driver->client)) sheng_schedule(driver);
    if (status == NW_CONTROL_NOT_FOUND) return results;
    if (status != NW_CONTROL_OK) {
      if (nw_control_client_failed(driver->client)) eloop_terminate();
      goto fail;
    }
    const u8 *bssid = sheng_span(&reply, NW_CONTROL_ATTR_BSSID, ETH_ALEN);
    size_t ie_length; const void *ies = nw_control_get(&reply, NW_CONTROL_ATTR_IE, &ie_length);
    if (!bssid || !ies || ie_length > 8192) goto fail;
    struct wpa_scan_res *entry = os_zalloc(sizeof(*entry) + ie_length);
    if (!entry) goto fail;
    os_memcpy(entry->bssid, bssid, ETH_ALEN);
    entry->freq = sheng_value(&reply, NW_CONTROL_ATTR_FREQUENCY, 0);
    entry->level = (int32_t)sheng_value(&reply, NW_CONTROL_ATTR_SIGNAL, 0);
    entry->flags = WPA_SCAN_LEVEL_DBM | WPA_SCAN_NOISE_INVALID;
    entry->caps = sheng_value(&reply, NW_CONTROL_ATTR_CAPABILITY, 0);
    entry->beacon_int = sheng_value(&reply, NW_CONTROL_ATTR_BEACON_INTERVAL, 0);
    entry->age = sheng_value(&reply, NW_CONTROL_ATTR_AGE, 0);
    entry->ie_len = ie_length;
    os_memcpy(entry + 1, ies, ie_length);
    struct wpa_scan_res **updated = os_realloc_array(results->res, results->num + 1, sizeof(*updated));
    if (!updated) { os_free(entry); goto fail; }
    results->res = updated; results->res[results->num++] = entry;
  }
fail:
  wpa_scan_results_free(results);
  return NULL;
}
static int sheng_authenticate(void *priv, struct wpa_driver_auth_params *parameters)
{
  struct sheng_driver *driver = priv;
  /* Fast association can reuse fresh BSS results without calling scan2. */
  driver->join_frequency = 0;
  u8 bytes[8192]; struct nw_control_builder b; struct nw_control_view reply;
  /* This profile advertises open authentication plus WPA2-PSK, not WEP/SAE. */
  if (!parameters->bssid || parameters->auth_alg != WPA_AUTH_ALG_OPEN ||
      parameters->ssid_len > SSID_MAX_LEN || parameters->auth_data_len ||
      parameters->wep_key_len[0] || parameters->wep_key_len[1] ||
      parameters->wep_key_len[2] || parameters->wep_key_len[3]) return -1;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_AUTHENTICATE)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_BSSID, parameters->bssid, ETH_ALEN) ||
      nw_control_put(&b, NW_CONTROL_ATTR_SSID, parameters->ssid, parameters->ssid_len) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, parameters->freq) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_AUTH_TYPE, 0) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_LOCAL_ONLY, parameters->local_state_change) ||
      nw_control_put(&b, NW_CONTROL_ATTR_IE, parameters->ie, parameters->ie_len)) return sheng_invalid(&b);
  int result = sheng_rpc(driver, &b, &reply);
  if (!result) {
    os_memcpy(driver->ssid, parameters->ssid, parameters->ssid_len);
    driver->ssid_length = parameters->ssid_len;
  }
  return result;
}
static int sheng_associate(void *priv, struct wpa_driver_associate_params *parameters)
{
  struct sheng_driver *driver = priv;
  u8 bytes[8192]; struct nw_control_builder b; struct nw_control_view reply;
  if (!parameters->bssid || parameters->mode != IEEE80211_MODE_INFRA ||
      parameters->ssid_len > SSID_MAX_LEN) return -1;
  u32 pairwise = parameters->pairwise_suite == WPA_CIPHER_NONE ? 0 : wpa_cipher_to_suite(WPA_PROTO_RSN, parameters->pairwise_suite);
  u32 group = parameters->group_suite == WPA_CIPHER_NONE ? 0 : wpa_cipher_to_suite(WPA_PROTO_RSN, parameters->group_suite);
  u32 akm = wpa_akm_to_suite(parameters->key_mgmt_suite);
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_ASSOCIATE)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_BSSID, parameters->bssid, ETH_ALEN) ||
      nw_control_put(&b, NW_CONTROL_ATTR_SSID, parameters->ssid, parameters->ssid_len) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, parameters->freq.freq) ||
      nw_control_put(&b, NW_CONTROL_ATTR_IE, parameters->wpa_ie, parameters->wpa_ie_len) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_PAIRWISE_CIPHER, pairwise) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_GROUP_CIPHER, group) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_AKM, akm) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_MFP, parameters->mgmt_frame_protection == MGMT_FRAME_PROTECTION_REQUIRED) ||
      (parameters->prev_bssid && nw_control_put(&b, NW_CONTROL_ATTR_PREVIOUS_BSSID, parameters->prev_bssid, ETH_ALEN)))
    return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static int sheng_deauthenticate(void *priv, const u8 *address, u16 reason)
{
  struct sheng_driver *driver = priv;
  u8 bytes[128]; struct nw_control_builder b; struct nw_control_view reply;
  if (!address || sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_DEAUTHENTICATE)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_BSSID, address, ETH_ALEN) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_REASON, reason)) return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static int sheng_set_key(void *priv, struct wpa_driver_set_key_params *parameters)
{
  struct sheng_driver *driver = priv;
  u8 bytes[512]; struct nw_control_builder b; struct nw_control_view reply;
  u32 cipher = 0, flags = 0;
  if (parameters->vlan_id || parameters->link_id >= 0 ||
      (parameters->key_flag & (KEY_FLAG_PMK | KEY_FLAG_MODIFY | KEY_FLAG_NEXT))) return -1;
  switch (parameters->alg) {
  case WPA_ALG_NONE: flags |= NW_CONTROL_KEY_DELETE; break;
  case WPA_ALG_CCMP: cipher = RSN_CIPHER_SUITE_CCMP; break;
  case WPA_ALG_GCMP: cipher = RSN_CIPHER_SUITE_GCMP; break;
  case WPA_ALG_GCMP_256: cipher = RSN_CIPHER_SUITE_GCMP_256; break;
  case WPA_ALG_CCMP_256: cipher = RSN_CIPHER_SUITE_CCMP_256; break;
  case WPA_ALG_BIP_CMAC_128: cipher = RSN_CIPHER_SUITE_AES_128_CMAC; break;
  case WPA_ALG_BIP_GMAC_128: cipher = RSN_CIPHER_SUITE_BIP_GMAC_128; break;
  case WPA_ALG_BIP_GMAC_256: cipher = RSN_CIPHER_SUITE_BIP_GMAC_256; break;
  case WPA_ALG_BIP_CMAC_256: cipher = RSN_CIPHER_SUITE_BIP_CMAC_256; break;
  default: return -1;
  }
  if ((parameters->key_flag & KEY_FLAG_PAIRWISE) ||
      (parameters->addr && !is_broadcast_ether_addr(parameters->addr))) flags |= NW_CONTROL_KEY_PAIRWISE;
  if (parameters->set_tx) flags |= parameters->key_idx >= 4 ? NW_CONTROL_KEY_DEFAULT_MGMT : NW_CONTROL_KEY_DEFAULT_TX;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_SET_KEY)) return -1;
  if (nw_control_put_u32(&b, NW_CONTROL_ATTR_CIPHER, cipher) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_KEY_FLAGS, flags) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_KEY_INDEX, parameters->key_idx) ||
      (parameters->addr && nw_control_put(&b, NW_CONTROL_ATTR_BSSID, parameters->addr, ETH_ALEN)) ||
      nw_control_put(&b, NW_CONTROL_ATTR_KEY_SEQUENCE, parameters->seq, parameters->seq_len) ||
      nw_control_put(&b, NW_CONTROL_ATTR_KEY, parameters->key, parameters->key_len)) return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static int sheng_port(void *priv, int authorized)
{
  return sheng_u32(priv, NW_CONTROL_SET_PORT, NW_CONTROL_ATTR_AUTHORIZED, !!authorized);
}
static int sheng_operstate(void *priv, int state)
{
  return sheng_u32(priv, NW_CONTROL_SET_OPERSTATE, NW_CONTROL_ATTR_OPERSTATE, !!state);
}
static int sheng_eapol(void *priv, const u8 *destination, u16 protocol,
                       const u8 *payload, size_t length, int no_encrypt, int link_id)
{
  struct sheng_driver *driver = priv;
  u8 bytes[8192]; struct nw_control_builder b; struct nw_control_view reply;
  if (!destination || protocol != ETH_P_EAPOL || link_id >= 0 ||
      sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_TX_EAPOL)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_BSSID, destination, ETH_ALEN) ||
      nw_control_put(&b, NW_CONTROL_ATTR_EAPOL, payload, length) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_ETHERTYPE, protocol) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_NO_ENCRYPT, !!no_encrypt)) return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
void nw_sheng_report_auth_failure(void *priv)
{
  sheng_u32(priv, NW_CONTROL_REPORT_RESULT, NW_CONTROL_ATTR_STATUS_CODE, NW_CONTROL_AUTH_FAILED);
}
void nw_sheng_report_request_failure(void *priv, unsigned operation, int status)
{
  struct sheng_driver *driver = priv;
  u8 bytes[128]; struct nw_control_builder b; struct nw_control_view reply;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_REPORT_RESULT)) return;
  if (nw_control_put_u32(&b, NW_CONTROL_ATTR_STATUS_CODE, status) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_REASON, operation)) { sheng_invalid(&b); return; }
  sheng_rpc(driver, &b, &reply);
}
static int sheng_mgmt(void *priv, const u8 *frame, size_t length, int no_ack,
  unsigned frequency, unsigned wait, int no_cck, int link_id)
{
  struct sheng_driver *driver = priv;
  u8 bytes[4608]; struct nw_control_builder b; struct nw_control_view reply;
  if (length < 25 || length > 4096 || link_id >= 0 ||
      sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_SEND_MGMT)) return -1;
  if (nw_control_put(&b, NW_CONTROL_ATTR_FRAME, frame, length) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, frequency) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_NO_ACK, !!no_ack) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_NO_CCK, !!no_cck) ||
      nw_control_put_u32(&b, NW_CONTROL_ATTR_WAIT, wait)) return sheng_invalid(&b);
  return sheng_rpc(driver, &b, &reply);
}
static int sheng_send_mlme(void *priv, const u8 *frame, size_t length, int no_ack,
  unsigned frequency, const u16 *csa_offsets, size_t csa_length, int no_encrypt,
  unsigned wait, int link_id)
{
  (void)csa_offsets;
  if (csa_length || no_encrypt) return -1;
  return sheng_mgmt(priv, frame, length, no_ack, frequency, wait, 0, link_id);
}
static int sheng_send_action(void *priv, unsigned frequency, unsigned wait,
  const u8 *destination, const u8 *source, const u8 *bssid, const u8 *data,
  size_t length, int no_cck, int link_id)
{
  if (!destination || !source || !bssid || !data || length > 4096 - 24) return -1;
  u8 *frame = os_zalloc(length + 24);
  if (!frame) return -1;
  WPA_PUT_LE16(frame, WLAN_FC_TYPE_MGMT << 2 | WLAN_FC_STYPE_ACTION << 4);
  os_memcpy(frame + 4, destination, ETH_ALEN); os_memcpy(frame + 10, source, ETH_ALEN);
  os_memcpy(frame + 16, bssid, ETH_ALEN); os_memcpy(frame + 24, data, length);
  int result = sheng_mgmt(priv, frame, length + 24, 0, frequency, wait, no_cck, link_id);
  bin_clear_free(frame, length + 24); return result;
}
static void sheng_event(struct sheng_driver *driver, const struct nw_control_view *view)
{
  unsigned operation = view->header.operation;
  int frontend = operation == NW_CONTROL_JOIN_REQUEST || operation == NW_CONTROL_DISCONNECT_REQUEST ||
                 operation == NW_CONTROL_POWER_REQUEST || operation == NW_CONTROL_SCAN_REQUEST;
  if (frontend) {
    if (view->header.generation < driver->generation) return;
    driver->generation = view->header.generation;
    nw_control_client_generation(driver->client, driver->generation);
    if (operation != NW_CONTROL_SCAN_REQUEST) driver->join_frequency = 0;
    if (operation == NW_CONTROL_JOIN_REQUEST) {
      u32 frequency = sheng_value(view, NW_CONTROL_ATTR_FREQUENCY, 0);
      if (frequency >= 2412 && frequency <= 7125) driver->join_frequency = frequency;
    }
    nw_supplicant_request(driver->context, view);
    return;
  }
  if (view->header.generation != driver->generation) return;
  union wpa_event_data event;
  os_memset(&event, 0, sizeof(event));
  size_t length = 0;
  const u8 *frame = nw_control_get(view, NW_CONTROL_ATTR_FRAME, &length);
  const u8 *bssid = sheng_span(view, NW_CONTROL_ATTR_BSSID, ETH_ALEN);
  switch (operation) {
  case NW_CONTROL_WIRELESS_EVENT:
    nw_sheng_wireless_event(driver->context, view); break;
  case NW_CONTROL_SCAN_STARTED:
    wpa_supplicant_event(driver->context, EVENT_SCAN_STARTED, NULL); break;
  case NW_CONTROL_SCAN_DONE:
    event.scan_info.aborted = view->header.status != NW_CONTROL_OK;
    wpa_supplicant_event(driver->context, EVENT_SCAN_RESULTS, &event); break;
  case NW_CONTROL_AUTH_FRAME:
    if (!frame || length < 30) break;
    os_memcpy(event.auth.peer, frame + 10, ETH_ALEN);
    os_memcpy(event.auth.bssid, frame + 16, ETH_ALEN);
    event.auth.auth_type = WPA_GET_LE16(frame + 24);
    event.auth.auth_transaction = WPA_GET_LE16(frame + 26);
    event.auth.status_code = WPA_GET_LE16(frame + 28);
    event.auth.ies = frame + 30; event.auth.ies_len = length - 30;
    event.auth.frame_body = frame + 24; event.auth.frame_body_len = length - 24;
    wpa_supplicant_event(driver->context, EVENT_AUTH, &event); break;
  case NW_CONTROL_AUTH_TIMEOUT:
  case NW_CONTROL_ASSOC_TIMEOUT:
    if (!bssid) break;
    os_memcpy(event.timeout_event.addr, bssid, ETH_ALEN);
    wpa_supplicant_event(driver->context, operation == NW_CONTROL_AUTH_TIMEOUT ? EVENT_AUTH_TIMED_OUT : EVENT_ASSOC_TIMED_OUT, &event);
    break;
  case NW_CONTROL_ASSOC_RESULT: {
    u32 status = sheng_value(view, NW_CONTROL_ATTR_STATUS_CODE, 1);
    const u8 *ies = nw_control_get(view, NW_CONTROL_ATTR_IE, &length);
    if (!bssid) break;
    if (status) {
      event.assoc_reject.bssid = bssid; event.assoc_reject.status_code = status;
      event.assoc_reject.resp_ies = ies; event.assoc_reject.resp_ies_len = length;
      wpa_supplicant_event(driver->context, EVENT_ASSOC_REJECT, &event);
    } else {
      os_memcpy(driver->bssid, bssid, ETH_ALEN);
      event.assoc_info.addr = bssid;
      event.assoc_info.resp_ies = ies; event.assoc_info.resp_ies_len = length;
      event.assoc_info.req_ies = nw_control_get(view, NW_CONTROL_ATTR_REQUEST_IE, &event.assoc_info.req_ies_len);
      event.assoc_info.freq = sheng_value(view, NW_CONTROL_ATTR_FREQUENCY, 0);
      wpa_supplicant_event(driver->context, EVENT_ASSOC, &event);
    }
    break;
  }
  case NW_CONTROL_DISCONNECTED:
    os_memset(driver->bssid, 0, ETH_ALEN);
    event.deauth_info.addr = bssid;
    event.deauth_info.reason_code = sheng_value(view, NW_CONTROL_ATTR_REASON, WLAN_REASON_UNSPECIFIED);
    event.deauth_info.locally_generated = sheng_value(view, NW_CONTROL_ATTR_LOCAL_ONLY, 0);
    wpa_supplicant_event(driver->context, EVENT_DEAUTH, &event); break;
  case NW_CONTROL_RX_EAPOL:
    if (!bssid) break;
    event.eapol_rx.src = bssid;
    event.eapol_rx.data = nw_control_get(view, NW_CONTROL_ATTR_EAPOL, &event.eapol_rx.data_len);
    if (!event.eapol_rx.data || event.eapol_rx.data_len < 4) break;
    event.eapol_rx.encrypted = sheng_value(view, NW_CONTROL_ATTR_NO_ENCRYPT, 1) ? FRAME_NOT_ENCRYPTED : FRAME_ENCRYPTED;
    event.eapol_rx.link_id = -1;
    wpa_supplicant_event(driver->context, EVENT_EAPOL_RX, &event); break;
  case NW_CONTROL_EAPOL_STATUS:
    if (!frame || length < ETH_HLEN) break;
    event.eapol_tx_status.dst = frame;
    event.eapol_tx_status.data = frame + ETH_HLEN;
    event.eapol_tx_status.data_len = length - ETH_HLEN;
    event.eapol_tx_status.ack = sheng_value(view, NW_CONTROL_ATTR_ACKED, 0);
    event.eapol_tx_status.link_id = -1;
    wpa_supplicant_event(driver->context, EVENT_EAPOL_TX_STATUS, &event); break;
  }
}
static void sheng_drain(void *context, void *unused)
{
  struct sheng_driver *driver = context;
  (void)unused;
  driver->drain_scheduled = 0;
  driver->references++;
  for (unsigned budget = 0; budget < 32 && !driver->closing && !driver->failed; budget++) {
    struct nw_control_packet *packet; struct nw_control_view view;
    int result = nw_control_client_event(driver->client, &packet, &view);
    if (result < 0) {
      driver->failed = 1;
      eloop_unregister_read_sock(nw_control_client_fd(driver->client));
      eloop_terminate(); break;
    }
    if (!result) break;
    sheng_event(driver, &view);
    nw_control_packet_free(packet);
  }
  if (!driver->closing && !driver->failed && nw_control_client_has_events(driver->client)) sheng_schedule(driver);
  sheng_release(driver);
}
static void sheng_read(int fd, void *context, void *unused)
{
  (void)fd;
  /* Cancel an already queued drain; process this readiness only once. */
  eloop_cancel_timeout(sheng_drain, context, NULL);
  sheng_drain(context, unused);
}
static void sheng_deinit(void *priv)
{
  struct sheng_driver *driver = priv;
  driver->closing = 1;
  eloop_unregister_read_sock(nw_control_client_fd(driver->client));
  eloop_cancel_timeout(sheng_drain, driver, NULL);
  sheng_release(driver);
}
static void *sheng_init(void *context, const char *ifname)
{
  if (os_strcmp(ifname, "en0")) return NULL;
  struct sheng_driver *driver = os_zalloc(sizeof(*driver));
  if (!driver) return NULL;
  driver->context = context; driver->references = 1;
  driver->client = nw_control_client_open();
  if (!driver->client) { wpa_printf(MSG_ERROR, "sheng: native control open failed: %s", strerror(errno)); os_free(driver); return NULL; }
  u8 bytes[64]; struct nw_control_builder b; struct nw_control_view reply;
  if (sheng_begin(driver, &b, bytes, sizeof(bytes), NW_CONTROL_INFO) ||
      sheng_rpc(driver, &b, &reply)) {
    wpa_printf(MSG_ERROR, "sheng: native INFO failed: %s", strerror(errno)); goto fail;
  }
  const u8 *address = sheng_span(&reply, NW_CONTROL_ATTR_MAC, ETH_ALEN);
  if (!address) goto fail;
  os_memcpy(driver->address, address, ETH_ALEN);
  driver->generation = reply.header.generation;
  nw_control_client_generation(driver->client, driver->generation);
  driver->capability.flags = WPA_DRIVER_FLAGS_SME | WPA_DRIVER_FLAGS_CONTROL_PORT;
  driver->capability.flags2 = WPA_DRIVER_FLAGS2_CONTROL_PORT_RX | WPA_DRIVER_FLAGS2_CONTROL_PORT_TX_STATUS;
  driver->capability.key_mgmt = WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK | WPA_DRIVER_CAPA_KEY_MGMT_PSK_SHA256;
  driver->capability.auth = WPA_DRIVER_AUTH_OPEN;
  size_t cipher_length;
  const u8 *ciphers = nw_control_get(&reply, NW_CONTROL_ATTR_CIPHERS, &cipher_length);
  if (!ciphers || cipher_length % 4) goto fail;
  for (size_t i = 0; i < cipher_length; i += 4) {
    switch (WPA_GET_LE32(ciphers + i)) {
    case RSN_CIPHER_SUITE_CCMP: driver->capability.enc |= WPA_DRIVER_CAPA_ENC_CCMP; break;
    case RSN_CIPHER_SUITE_GCMP: driver->capability.enc |= WPA_DRIVER_CAPA_ENC_GCMP; break;
    case RSN_CIPHER_SUITE_GCMP_256: driver->capability.enc |= WPA_DRIVER_CAPA_ENC_GCMP_256; break;
    case RSN_CIPHER_SUITE_CCMP_256: driver->capability.enc |= WPA_DRIVER_CAPA_ENC_CCMP_256; break;
    case RSN_CIPHER_SUITE_AES_128_CMAC: driver->capability.enc |= WPA_DRIVER_CAPA_ENC_BIP; break;
    }
  }
  if (!(driver->capability.enc & WPA_DRIVER_CAPA_ENC_CCMP)) goto fail;
  driver->capability.max_scan_ssids = sheng_value(&reply, NW_CONTROL_ATTR_MAX_SCAN_SSIDS, 1);
  if (driver->capability.max_scan_ssids < 1 || driver->capability.max_scan_ssids > WPAS_MAX_SCAN_SSIDS) goto fail;
  if (eloop_register_read_sock(nw_control_client_fd(driver->client), sheng_read, driver, NULL)) goto fail;
  return driver;
fail:
  eloop_cancel_timeout(sheng_drain, driver, NULL);
  sheng_release(driver);
  return NULL;
}
const struct wpa_driver_ops wpa_driver_sheng_ops = {
  .name = "sheng", .desc = "UniXNU native cfg80211 control port",
  .init = sheng_init, .deinit = sheng_deinit,
  .get_bssid = sheng_get_bssid, .get_ssid = sheng_get_ssid,
  .get_mac_addr = sheng_get_mac, .get_capa = sheng_get_capa,
  .scan2 = sheng_scan, .get_scan_results2 = sheng_scan_results,
  .authenticate = sheng_authenticate, .associate = sheng_associate,
  .deauthenticate = sheng_deauthenticate, .set_key = sheng_set_key,
  .set_supp_port = sheng_port, .set_operstate = sheng_operstate,
  .tx_control_port = sheng_eapol,
  .send_mlme = sheng_send_mlme, .send_action = sheng_send_action
};
