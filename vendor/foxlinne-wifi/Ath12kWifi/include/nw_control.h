/* Native cfg80211/supplicant control channel. Ordinary data stays in-kernel.
 * Integers on the wire are little endian; packets contain no pointers.
 * Keys and EAPOL are control messages. Never log their payloads. */
#ifndef NW_CONTROL_H
#define NW_CONTROL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NW_CONTROL_NAME "org.unixnu.wifi.control"
#define NW_CONTROL_MAGIC 0x3143574eu
#define NW_CONTROL_VERSION 1
#define NW_CONTROL_MAX_MESSAGE 65536u
#define NW_CONTROL_HEADER_SIZE 40u
#define NW_CONTROL_GET_SESSION 1
enum nw_control_kind { NW_CONTROL_REQUEST = 1, NW_CONTROL_REPLY, NW_CONTROL_EVENT };
enum nw_control_status {
  NW_CONTROL_OK, NW_CONTROL_INVALID, NW_CONTROL_BUSY, NW_CONTROL_NOT_READY,
  NW_CONTROL_AUTH_FAILED, NW_CONTROL_TIMED_OUT, NW_CONTROL_UNSUPPORTED,
  NW_CONTROL_NO_MEMORY, NW_CONTROL_NOT_FOUND, NW_CONTROL_DRIVER_FAILED,
  NW_CONTROL_STALE, NW_CONTROL_STATUS_MAX
};
enum nw_control_operation {
  NW_CONTROL_INFO = 1, NW_CONTROL_SCAN, NW_CONTROL_GET_BSS,
  NW_CONTROL_AUTHENTICATE, NW_CONTROL_ASSOCIATE, NW_CONTROL_DEAUTHENTICATE,
  NW_CONTROL_SET_KEY, NW_CONTROL_SET_PORT, NW_CONTROL_TX_EAPOL,
  NW_CONTROL_SET_OPERSTATE, NW_CONTROL_GET_CHANNELS, NW_CONTROL_REPORT_RESULT,
  NW_CONTROL_SEND_MGMT,
  NW_CONTROL_COMMAND_MAX,
  NW_CONTROL_READY = 0x100, NW_CONTROL_SCAN_STARTED, NW_CONTROL_SCAN_DONE,
  NW_CONTROL_AUTH_FRAME, NW_CONTROL_ASSOC_RESULT, NW_CONTROL_DISCONNECTED,
  NW_CONTROL_RX_EAPOL, NW_CONTROL_EAPOL_STATUS, NW_CONTROL_JOIN_REQUEST,
  NW_CONTROL_DISCONNECT_REQUEST, NW_CONTROL_POWER_REQUEST, NW_CONTROL_SCAN_REQUEST,
  NW_CONTROL_AUTH_TIMEOUT, NW_CONTROL_ASSOC_TIMEOUT, NW_CONTROL_WIRELESS_EVENT,
  NW_CONTROL_EVENT_MAX
};
enum nw_control_attribute {
  NW_CONTROL_ATTR_IFNAME = 1, NW_CONTROL_ATTR_MAC, NW_CONTROL_ATTR_BSSID,
  NW_CONTROL_ATTR_SSID, NW_CONTROL_ATTR_FREQUENCY, NW_CONTROL_ATTR_IE,
  NW_CONTROL_ATTR_AUTH_TYPE, NW_CONTROL_ATTR_REASON, NW_CONTROL_ATTR_CIPHER,
  NW_CONTROL_ATTR_KEY_INDEX, NW_CONTROL_ATTR_KEY_FLAGS, NW_CONTROL_ATTR_KEY,
  NW_CONTROL_ATTR_KEY_SEQUENCE, NW_CONTROL_ATTR_AUTHORIZED, NW_CONTROL_ATTR_EAPOL,
  NW_CONTROL_ATTR_NO_ENCRYPT, NW_CONTROL_ATTR_COOKIE, NW_CONTROL_ATTR_STATUS_CODE,
  NW_CONTROL_ATTR_SCAN_SSIDS, NW_CONTROL_ATTR_SCAN_FREQUENCIES,
  NW_CONTROL_ATTR_BSS_INDEX, NW_CONTROL_ATTR_SIGNAL, NW_CONTROL_ATTR_CAPABILITY,
  NW_CONTROL_ATTR_BEACON_INTERVAL, NW_CONTROL_ATTR_AGE, NW_CONTROL_ATTR_FRAME,
  NW_CONTROL_ATTR_CIPHERS, NW_CONTROL_ATTR_FEATURES, NW_CONTROL_ATTR_MAX_SCAN_SSIDS,
  NW_CONTROL_ATTR_MAX_SCAN_IE, NW_CONTROL_ATTR_CHANNELS, NW_CONTROL_ATTR_OPERSTATE,
  NW_CONTROL_ATTR_PASSWORD, NW_CONTROL_ATTR_PAIRWISE_CIPHER,
  NW_CONTROL_ATTR_GROUP_CIPHER, NW_CONTROL_ATTR_AKM, NW_CONTROL_ATTR_MFP,
  NW_CONTROL_ATTR_PREVIOUS_BSSID, NW_CONTROL_ATTR_LOCAL_ONLY,
  NW_CONTROL_ATTR_ETHERTYPE, NW_CONTROL_ATTR_ACKED, NW_CONTROL_ATTR_REQUEST_IE,
  NW_CONTROL_ATTR_NO_ACK, NW_CONTROL_ATTR_WAIT, NW_CONTROL_ATTR_NO_CCK,
  NW_CONTROL_ATTR_COUNT
};
/* Cipher and AKM values are IEEE suite selectors, not wpa_supplicant enums.
 * Scan SSIDs use repeated 33-byte records (length, 32 padded SSID bytes).
 * Frequency lists contain little-endian u32 MHz values without a terminator.
 * CHANNELS contains 16-byte records: u32 MHz, Linux channel flags, signed
 * maximum dBm and nl80211 band; every field is little endian. */
enum nw_control_key_flags {
  NW_CONTROL_KEY_PAIRWISE = 1, NW_CONTROL_KEY_DEFAULT_TX = 2,
  NW_CONTROL_KEY_DEFAULT_MGMT = 4, NW_CONTROL_KEY_DELETE = 8
};
/* A generation identifies one connection attempt; replies/events retain it.
 * Handlers use it to reject old key/port/EAPOL operations before state changes.
 * Session identifies the control-socket owner, including after reconnect. */
struct nw_control_header {
  uint32_t magic; uint16_t version, kind, operation, reserved;
  uint32_t sequence, length; int32_t status; uint64_t session, generation;
};
struct nw_control_builder { uint8_t *bytes; size_t length, capacity; uint64_t attributes; };
struct nw_control_view { struct nw_control_header header; const uint8_t *bytes; uint64_t attributes; };
enum nw_control_codec_result { NW_CONTROL_CODEC_INVALID = -1, NW_CONTROL_CODEC_FULL = -2, NW_CONTROL_CODEC_VERSION = -3, NW_CONTROL_CODEC_DUPLICATE = -4 };
int nw_control_begin(struct nw_control_builder *builder, void *buffer, size_t capacity,
                     unsigned kind, unsigned operation, uint32_t sequence, uint64_t session, uint64_t generation, int status);
int nw_control_put(struct nw_control_builder *builder, unsigned attribute, const void *bytes, size_t length);
int nw_control_decode(const void *bytes, size_t length, struct nw_control_view *view);
/* Returned spans borrow the decoded message until its owner releases it. */
const void *nw_control_get(const struct nw_control_view *view, unsigned attribute, size_t *length);
void nw_control_clear(void *bytes, size_t length);
int nw_control_put_u32(struct nw_control_builder *builder, unsigned attribute, uint32_t value);
int nw_control_put_u64(struct nw_control_builder *builder, unsigned attribute, uint64_t value);
int nw_control_get_u32(const struct nw_control_view *view, unsigned attribute, uint32_t *value);
int nw_control_get_u64(const struct nw_control_view *view, unsigned attribute, uint64_t *value);
void nw_control_reply_state(struct nw_control_builder *builder, uint64_t generation, int status);
#ifdef __cplusplus
}
static_assert(sizeof(nw_control_header) == NW_CONTROL_HEADER_SIZE, "control header layout");
#else
_Static_assert(sizeof(struct nw_control_header) == NW_CONTROL_HEADER_SIZE, "control header layout");
#endif
#endif
