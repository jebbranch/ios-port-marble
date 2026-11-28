/* Consume original Linux notification encodings, without a libnl dependency. */
#include "utils/includes.h"
#include "utils/common.h"
#include "common/ieee802_11_defs.h"
#include "driver.h"
#include "nl80211_copy.h"
#include "nw_control.h"

struct attribute { const u8 *data; size_t length; };
static int valid(const u8 *bytes, size_t length) {
  while (length) {
    if (length < 4) return 0;
    unsigned size = WPA_GET_LE16(bytes), aligned = (size + 3) & ~3U;
    if (size < 4 || aligned > length) return 0;
    for (unsigned i = size; i < aligned; i++) if (bytes[i]) return 0;
    bytes += aligned; length -= aligned;
  }
  return 1;
}
static struct attribute find(const u8 *bytes, size_t length, unsigned type) {
  struct attribute result = {0};
  while (length >= 4) {
    unsigned size = WPA_GET_LE16(bytes), attr = WPA_GET_LE16(bytes + 2) & 0x3fff;
    if (size < 4 || ((size + 3) & ~3U) > length) return (struct attribute){0};
    if (attr == type) {
      if (result.data) return (struct attribute){0};
      result = (struct attribute){bytes + 4, size - 4};
    }
    unsigned aligned = (size + 3) & ~3U; bytes += aligned; length -= aligned;
  }
  return result;
}
static u32 value(const u8 *bytes, size_t length, unsigned type, u32 fallback) {
  struct attribute attr = find(bytes, length, type);
  return attr.data && attr.length == 4 ? WPA_GET_LE32(attr.data) : fallback;
}
static enum chan_width width(unsigned nl_width) {
  switch (nl_width) {
  case NL80211_CHAN_WIDTH_20_NOHT: return CHAN_WIDTH_20_NOHT;
  case NL80211_CHAN_WIDTH_20: return CHAN_WIDTH_20;
  case NL80211_CHAN_WIDTH_40: return CHAN_WIDTH_40;
  case NL80211_CHAN_WIDTH_80: return CHAN_WIDTH_80;
  case NL80211_CHAN_WIDTH_80P80: return CHAN_WIDTH_80P80;
  case NL80211_CHAN_WIDTH_160: return CHAN_WIDTH_160;
  case NL80211_CHAN_WIDTH_320: return CHAN_WIDTH_320;
  default: return CHAN_WIDTH_UNKNOWN;
  }
}
void nw_sheng_wireless_event(void *context, const struct nw_control_view *view) {
  size_t length; const u8 *message = nw_control_get(view, NW_CONTROL_ATTR_FRAME, &length);
  if (!message || length < 20 || WPA_GET_LE32(message) != length || message[17] != 1 ||
      !valid(message + 20, length - 20)) return;
  const u8 *attrs = message + 20; size_t size = length - 20;
  struct attribute frame = find(attrs, size, NL80211_ATTR_FRAME);
  union wpa_event_data event; os_memset(&event, 0, sizeof(event));
  switch (message[16]) {
  case NL80211_CMD_NOTIFY_CQM: {
    struct attribute cqm = find(attrs, size, NL80211_ATTR_CQM);
    if (!cqm.data || !valid(cqm.data, cqm.length)) return;
    if (find(cqm.data, cqm.length, NL80211_ATTR_CQM_BEACON_LOSS_EVENT).data) {
      wpa_supplicant_event(context, EVENT_BEACON_LOSS, NULL); return;
    }
    struct attribute rssi = find(cqm.data, cqm.length, NL80211_ATTR_CQM_RSSI_LEVEL);
    if (!rssi.data || rssi.length != 4) return;
    event.signal_change.data.signal = (s32)WPA_GET_LE32(rssi.data);
    event.signal_change.above_threshold = value(cqm.data, cqm.length,
      NL80211_ATTR_CQM_RSSI_THRESHOLD_EVENT, 0) == NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH;
    wpa_supplicant_event(context, EVENT_SIGNAL_CHANGE, &event); return;
  }
  case NL80211_CMD_CH_SWITCH_NOTIFY:
  case NL80211_CMD_CH_SWITCH_STARTED_NOTIFY: {
    event.ch_switch.freq = value(attrs, size, NL80211_ATTR_WIPHY_FREQ, 0);
    unsigned raw = value(attrs, size, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_20_NOHT);
    event.ch_switch.ch_width = width(raw); event.ch_switch.ht_enabled = raw != NL80211_CHAN_WIDTH_20_NOHT;
    event.ch_switch.cf1 = value(attrs, size, NL80211_ATTR_CENTER_FREQ1, 0);
    event.ch_switch.cf2 = value(attrs, size, NL80211_ATTR_CENTER_FREQ2, 0);
    event.ch_switch.ch_offset = raw == NL80211_CHAN_WIDTH_40 ? (event.ch_switch.cf1 > event.ch_switch.freq ? 1 : -1) : 0;
    event.ch_switch.link_id = -1;
    if (event.ch_switch.freq) wpa_supplicant_event(context,
      message[16] == NL80211_CMD_CH_SWITCH_NOTIFY ? EVENT_CH_SWITCH : EVENT_CH_SWITCH_STARTED, &event);
    return;
  }
  case NL80211_CMD_FRAME:
    if (!frame.data || frame.length < 24) return;
    event.rx_mgmt.frame = frame.data; event.rx_mgmt.frame_len = frame.length;
    event.rx_mgmt.freq = value(attrs, size, NL80211_ATTR_WIPHY_FREQ, 0);
    event.rx_mgmt.ssi_signal = (s32)value(attrs, size, NL80211_ATTR_RX_SIGNAL_DBM, 0);
    wpa_supplicant_event(context, EVENT_RX_MGMT, &event); return;
  case NL80211_CMD_UNPROT_DEAUTHENTICATE:
  case NL80211_CMD_UNPROT_DISASSOCIATE:
    if (!frame.data || frame.length < 26) return;
    event.unprot_deauth.sa = frame.data + 10; event.unprot_deauth.da = frame.data + 4;
    event.unprot_deauth.reason_code = WPA_GET_LE16(frame.data + 24);
    wpa_supplicant_event(context, message[16] == NL80211_CMD_UNPROT_DEAUTHENTICATE ? EVENT_UNPROT_DEAUTH : EVENT_UNPROT_DISASSOC, &event);
    return;
  case NL80211_CMD_SET_REKEY_OFFLOAD: {
    struct attribute rekey = find(attrs, size, NL80211_ATTR_REKEY_DATA);
    struct attribute bssid = find(attrs, size, NL80211_ATTR_MAC);
    if (!rekey.data || !valid(rekey.data, rekey.length) || bssid.length != ETH_ALEN) return;
    struct attribute replay = find(rekey.data, rekey.length, NL80211_REKEY_DATA_REPLAY_CTR);
    if (replay.length != 8) return;
    event.driver_gtk_rekey.bssid = bssid.data; event.driver_gtk_rekey.replay_ctr = replay.data;
    wpa_supplicant_event(context, EVENT_DRIVER_GTK_REKEY, &event); return;
  }
  }
}
