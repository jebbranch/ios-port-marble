/* Test the production notification decoder with fixed original-nl80211 frames. */
#include "utils/includes.h"
#include "utils/common.h"
#include "driver.h"
#include "nl80211_copy.h"
#include "nw_control.h"
#include <assert.h>
void nw_sheng_wireless_event(void *, const struct nw_control_view *);
static unsigned received;
static enum wpa_event_type expected;
static u8 message[512];
static size_t length;
void wpa_supplicant_event(void *context, enum wpa_event_type type, union wpa_event_data *event) {
  assert(context == message && type == expected); received++;
  if (type == EVENT_SIGNAL_CHANGE) assert(event->signal_change.data.signal == -55 && event->signal_change.above_threshold);
  if (type == EVENT_CH_SWITCH) assert(event->ch_switch.freq == 5180 && event->ch_switch.cf1 == 5190 &&
    event->ch_switch.ch_offset == 1 && event->ch_switch.ch_width == CHAN_WIDTH_40);
  if (type == EVENT_RX_MGMT) assert(event->rx_mgmt.frame_len == 28 && event->rx_mgmt.ssi_signal == -42);
  if (type == EVENT_UNPROT_DEAUTH) assert(event->unprot_deauth.reason_code == 6);
  if (type == EVENT_DRIVER_GTK_REKEY) assert(event->driver_gtk_rekey.replay_ctr[7] == 9 && event->driver_gtk_rekey.bssid[0] == 2);
}
static void begin(unsigned command) { memset(message, 0, sizeof(message)); length = 20; message[16] = command; message[17] = 1; }
static size_t attribute(unsigned type, const void *data, size_t size) {
  size_t at = length; assert(length + 4 + size + 3 <= sizeof(message));
  WPA_PUT_LE16(message + at, 4 + size); WPA_PUT_LE16(message + at + 2, type);
  if (size) memcpy(message + at + 4, data, size);
  length += (4 + size + 3) & ~(size_t)3; return at;
}
static void scalar(unsigned type, u32 value) { u8 data[4]; WPA_PUT_LE32(data, value); attribute(type, data, 4); }
static void finish_nested(size_t at) { WPA_PUT_LE16(message + at, length - at); }
static void deliver(void) {
  u8 bytes[600]; struct nw_control_builder b; struct nw_control_view view;
  WPA_PUT_LE32(message, length);
  assert(!nw_control_begin(&b, bytes, sizeof(bytes), NW_CONTROL_EVENT, NW_CONTROL_WIRELESS_EVENT, 0, 1, 1, 0));
  assert(!nw_control_put(&b, NW_CONTROL_ATTR_FRAME, message, length)); assert(!nw_control_decode(bytes, b.length, &view));
  nw_sheng_wireless_event(message, &view);
}
int main(void) {
  begin(NL80211_CMD_NOTIFY_CQM); size_t nest = attribute(NL80211_ATTR_CQM, NULL, 0);
  attribute(NL80211_ATTR_CQM_BEACON_LOSS_EVENT, NULL, 0); finish_nested(nest);
  expected = EVENT_BEACON_LOSS; deliver(); assert(received == 1);
  message[17] = 2; deliver(); assert(received == 1);
  begin(NL80211_CMD_NOTIFY_CQM); nest = attribute(NL80211_ATTR_CQM, NULL, 0);
  scalar(NL80211_ATTR_CQM_RSSI_LEVEL, (u32)-55); scalar(NL80211_ATTR_CQM_RSSI_THRESHOLD_EVENT, NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH);
  finish_nested(nest); expected = EVENT_SIGNAL_CHANGE; deliver(); assert(received == 2);
  /* Duplicate CQM containers and truncated nested attributes are rejected. */
  attribute(NL80211_ATTR_CQM, NULL, 0); deliver(); assert(received == 2);
  length -= 4; WPA_PUT_LE16(message + nest + 4, 1000); deliver(); assert(received == 2);
  begin(NL80211_CMD_CH_SWITCH_NOTIFY); scalar(NL80211_ATTR_WIPHY_FREQ, 5180);
  scalar(NL80211_ATTR_CENTER_FREQ1, 5190); scalar(NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_40);
  expected = EVENT_CH_SWITCH; deliver(); assert(received == 3);
  begin(NL80211_CMD_FRAME); u8 frame[28] = {0xd0}; attribute(NL80211_ATTR_FRAME, frame, sizeof(frame));
  scalar(NL80211_ATTR_WIPHY_FREQ, 5180); scalar(NL80211_ATTR_RX_SIGNAL_DBM, (u32)-42);
  expected = EVENT_RX_MGMT; deliver(); assert(received == 4);
  begin(NL80211_CMD_UNPROT_DEAUTHENTICATE); frame[24] = 6; attribute(NL80211_ATTR_FRAME, frame, sizeof(frame));
  expected = EVENT_UNPROT_DEAUTH; deliver(); assert(received == 5);
  begin(NL80211_CMD_SET_REKEY_OFFLOAD); u8 bssid[6] = {2,1,2,3,4,5}, replay[8] = {0,0,0,0,0,0,0,9};
  attribute(NL80211_ATTR_MAC, bssid, sizeof(bssid)); nest = attribute(NL80211_ATTR_REKEY_DATA, NULL, 0);
  attribute(NL80211_REKEY_DATA_REPLAY_CTR, replay, sizeof(replay)); finish_nested(nest);
  expected = EVENT_DRIVER_GTK_REKEY; deliver(); assert(received == 6);
  puts("wireless event decoder: CQM/beacon/RSSI, channel width, PMF management/unprotected deauth, rekey, malformed/duplicate notifications passed");
}
