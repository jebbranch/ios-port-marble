/* Settings selects networks; upstream wpa_supplicant owns the connection SME. */
#include "utils/includes.h"
#include "utils/common.h"
#include <limits.h>
#include "config.h"
#include "wpa_supplicant_i.h"
#include "ctrl_iface.h"
#include "nw_supplicant_frontend.h"

extern void nw_sheng_report_auth_failure(void *priv);
extern void nw_sheng_report_request_failure(void *priv, unsigned operation, int status);

static int command(struct wpa_supplicant *supplicant, char *text, int network_id)
{
  size_t length = 0;
  char *reply = wpa_supplicant_ctrl_iface_process(supplicant, text, &length);
  int result = -1;
  if (reply) {
    if (network_id) {
      char *end;
      long id = strtol(reply, &end, 10);
      if (end != reply && end <= reply + length && id >= 0 && id <= INT_MAX) result = id;
    } else if (length >= 2 && os_memcmp(reply, "OK", 2) == 0) {
      result = 0;
    }
    os_free(reply);
  } else if (!network_id && length == 2) {
    result = 0;
  }
  return result;
}
static int fixed_command(struct wpa_supplicant *supplicant, const char *text)
{
  char buffer[64];
  if (os_strlcpy(buffer, text, sizeof(buffer)) >= sizeof(buffer)) return -1;
  return command(supplicant, buffer, 0);
}
static int join(struct wpa_supplicant *supplicant, const struct nw_control_view *event)
{
  size_t ssid_length, password_length;
  const u8 *ssid = nw_control_get(event, NW_CONTROL_ATTR_SSID, &ssid_length);
  const u8 *password = nw_control_get(event, NW_CONTROL_ATTR_PASSWORD, &password_length);
  size_t bssid_length;
  const u8 *bssid = nw_control_get(event, NW_CONTROL_ATTR_BSSID, &bssid_length);
  if (!ssid || !ssid_length || ssid_length > 32 || password_length > 64 ||
      (password_length && password_length < 8) ||
      (bssid && (bssid_length != ETH_ALEN || is_zero_ether_addr(bssid) || is_multicast_ether_addr(bssid)))) return -1;
  if (password_length == 64) {
    u8 psk[32];
    int invalid = hexstr2bin((const char *)password, psk, sizeof(psk));
    forced_memzero(psk, sizeof(psk));
    if (invalid) return -1;
  }
  /* wifid retains saved networks. The supplicant holds only its active profile
   * in RAM; REMOVE_NETWORK also clears old SME, PMK and handshake ownership. */
  if (fixed_command(supplicant, "REMOVE_NETWORK all")) return -1;
  char text[512] = "ADD_NETWORK";
  int id = command(supplicant, text, 1);
  if (id < 0) return -1;
  char ssid_hex[65]; wpa_snprintf_hex(ssid_hex, sizeof(ssid_hex), ssid, ssid_length);
  os_snprintf(text, sizeof(text), "SET_NETWORK %d ssid %s", id, ssid_hex);
  if (command(supplicant, text, 0)) goto fail;
  if (bssid) {
    os_snprintf(text, sizeof(text), "SET_NETWORK %d bssid " MACSTR, id, MAC2STR(bssid));
    if (command(supplicant, text, 0)) goto fail;
  }
  os_snprintf(text, sizeof(text), "SET_NETWORK %d key_mgmt %s", id,
              password_length ? "WPA-PSK WPA-PSK-SHA256" : "NONE");
  if (command(supplicant, text, 0)) goto fail;
  if (password_length) {
    char encoded[4 * 64 + 1];
    if (password_length == 64) {
      os_memcpy(encoded, password, 64); encoded[64] = 0;
      os_snprintf(text, sizeof(text), "SET_NETWORK %d psk %s", id, encoded);
    } else {
      printf_encode(encoded, sizeof(encoded), password, password_length);
      os_snprintf(text, sizeof(text), "SET_NETWORK %d psk \"%s\"", id, encoded);
    }
    int result = command(supplicant, text, 0);
    forced_memzero(encoded, sizeof(encoded)); forced_memzero(text, sizeof(text));
    if (result) goto fail;
    os_snprintf(text, sizeof(text), "SET_NETWORK %d proto RSN", id);
    if (command(supplicant, text, 0)) goto fail;
    os_snprintf(text, sizeof(text), "SET_NETWORK %d ieee80211w 1", id);
    if (command(supplicant, text, 0)) goto fail;
  }
  os_snprintf(text, sizeof(text), "SET_NETWORK %d scan_ssid 1", id);
  if (command(supplicant, text, 0)) goto fail;
  os_snprintf(text, sizeof(text), "SELECT_NETWORK %d", id);
  if (!command(supplicant, text, 0)) return 0;
fail:
  forced_memzero(text, sizeof(text));
  os_snprintf(text, sizeof(text), "REMOVE_NETWORK %d", id);
  command(supplicant, text, 0);
  return -1;
}
int nw_supplicant_request(void *context, const struct nw_control_view *event)
{
  struct wpa_supplicant *supplicant = context;
  int result = -1;
  switch (event->header.operation) {
  case NW_CONTROL_JOIN_REQUEST: result = join(supplicant, event); break;
  case NW_CONTROL_DISCONNECT_REQUEST: result = fixed_command(supplicant, "DISCONNECT"); break;
  case NW_CONTROL_SCAN_REQUEST: {
    size_t length; const u8 *ssid = nw_control_get(event, NW_CONTROL_ATTR_SSID, &length);
    if (ssid && length && length <= 32) {
      char hex[65], text[80]; wpa_snprintf_hex(hex, sizeof(hex), ssid, length);
      os_snprintf(text, sizeof(text), "SCAN ssid %s", hex); result = command(supplicant, text, 0);
    } else result = fixed_command(supplicant, "SCAN");
    break;
  }
  case NW_CONTROL_POWER_REQUEST: {
    uint32_t state;
    if (!nw_control_get_u32(event, NW_CONTROL_ATTR_OPERSTATE, &state))
      // Radio power does not select a network. Settings/wifid owns selection.
      result = fixed_command(supplicant, "DISCONNECT");
    break;
  }
  }
  if (result && event->header.operation == NW_CONTROL_JOIN_REQUEST)
    nw_sheng_report_request_failure(supplicant->drv_priv, event->header.operation, NW_CONTROL_INVALID);
  if (result && event->header.operation == NW_CONTROL_SCAN_REQUEST)
    nw_sheng_report_request_failure(supplicant->drv_priv, event->header.operation, NW_CONTROL_BUSY);
  return result;
}
void nw_supplicant_auth_failed(void *context, const char *reason)
{
  struct wpa_supplicant *supplicant = context;
  if (supplicant->driver && !os_strcmp(supplicant->driver->name, "sheng") &&
      (!os_strcmp(reason, "WRONG_KEY") || !os_strcmp(reason, "NO_PSK_AVAILABLE")))
    nw_sheng_report_auth_failure(supplicant->drv_priv);
}
