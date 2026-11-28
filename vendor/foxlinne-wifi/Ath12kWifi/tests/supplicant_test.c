/* Real upstream SME/WPA state machine against an offline AP over datagrams. */
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "common/wpa_common.h"
#include "common/eapol_common.h"
#include "crypto/sha1.h"
#include "crypto/aes_wrap.h"
#include "drivers/driver.h"
#include "wpa_supplicant_i.h"
#include "config_ssid.h"
#include "nw_control_client.h"
#include "nw_supplicant_frontend.h"
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>

static const u8 station[6] = {2,1,2,3,4,5}, ap[6] = {2,6,7,8,9,10};
static const u8 ssid[] = "offline-wpa";
static const u8 rsn[] = {0x30,0x14,1,0,0,0x0f,0xac,4,1,0,0,0x0f,0xac,4,1,0,0,0x0f,0xac,2,0,0};
static const u8 password[] = "offline-passphrase";
static int user_fd, kernel_fd, stopping, finished;
static _Atomic unsigned ptk_installs, gtk_installs, m2_seen, m4_seen, rekey_seen, authorized, wrong_key_reports;
static uint64_t session = 0x123456789abULL, generation = 30;
static struct wpa_ptk ptk;
static u8 anonce[32], gtk[16], group2[16], message3[512];
static size_t message3_length;
static struct wpa_supplicant *supplicant;
static int passed, open_network, wrong_password;

/* The test replaces only socket opening, retaining the production client. */
struct nw_control_client *nw_control_client_open(void)
{
  int fd = user_fd; user_fd = -1;
  return nw_control_client_from_fd(fd, session);
}
static void send_packet(struct nw_control_builder *b)
{
  assert(send(kernel_fd, b->bytes, b->length, 0) == (ssize_t)b->length);
}
static void event_begin(struct nw_control_builder *b, void *bytes, size_t size, unsigned operation)
{
  assert(!nw_control_begin(b, bytes, size, NW_CONTROL_EVENT, operation, 0, session, generation, NW_CONTROL_OK));
}
static void eapol(const u8 *frame, size_t length)
{
  u8 bytes[1024]; struct nw_control_builder b;
  event_begin(&b, bytes, sizeof(bytes), NW_CONTROL_RX_EAPOL);
  assert(!nw_control_put(&b, NW_CONTROL_ATTR_BSSID, ap, sizeof(ap)));
  assert(!nw_control_put(&b, NW_CONTROL_ATTR_EAPOL, frame, length));
  assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_NO_ENCRYPT, 1));
  send_packet(&b);
}
static size_t key_message(u8 *frame, unsigned info, unsigned replay, const u8 *plain, size_t size)
{
  const size_t base = sizeof(struct ieee802_1x_hdr) + sizeof(struct wpa_eapol_key);
  os_memset(frame, 0, 512);
  struct ieee802_1x_hdr *header = (void *)frame;
  struct wpa_eapol_key *key = (void *)(header + 1);
  header->version = 2; header->type = IEEE802_1X_TYPE_EAPOL_KEY;
  key->type = EAPOL_KEY_TYPE_RSN;
  WPA_PUT_BE16(key->key_info, info);
  WPA_PUT_BE16(key->key_length, 16);
  WPA_PUT_BE64(key->replay_counter, replay);
  os_memcpy(key->key_nonce, anonce, sizeof(anonce));
  size_t encrypted = size ? size + 8 : 0;
  if (size) assert(!aes_wrap(ptk.kek, ptk.kek_len, size / 8, plain, frame + base + 18));
  WPA_PUT_BE16(frame + base + 16, encrypted);
  size_t length = base + 18 + encrypted;
  WPA_PUT_BE16((u8 *)&header->length, length - sizeof(*header));
  if (info & WPA_KEY_INFO_MIC)
    assert(!wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_PSK,
                              RSN_HASH_SHA256, 2, frame, length, frame + base));
  return length;
}
static void message1(void)
{
  u8 frame[512];
  size_t length = key_message(frame, 2 | WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_ACK, 1, NULL, 0);
  eapol(frame, length);
}
static void message3_send(void)
{
  u8 plain[48] = {0};
  os_memcpy(plain, rsn, sizeof(rsn));
  u8 *kde = plain + sizeof(rsn);
  kde[0] = 0xdd; kde[1] = 22; kde[2] = 0; kde[3] = 0x0f; kde[4] = 0xac; kde[5] = 1;
  kde[6] = 1; os_memcpy(kde + 8, gtk, sizeof(gtk));
  plain[46] = 0xdd;
  message3_length = key_message(message3, 2 | WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_ACK |
    WPA_KEY_INFO_MIC | WPA_KEY_INFO_INSTALL | WPA_KEY_INFO_SECURE | WPA_KEY_INFO_ENCR_KEY_DATA, 2, plain, sizeof(plain));
  /* A corrupt MIC must not install a key or open the port. */
  u8 corrupt[512]; os_memcpy(corrupt, message3, message3_length); corrupt[81] ^= 1;
  eapol(corrupt, message3_length);
  /* An otherwise valid frame from an earlier attempt must also be ignored. */
  generation--; eapol(message3, message3_length); generation++;
  eapol(message3, message3_length);
}
static void group_rekey(void)
{
  u8 plain[24] = {0xdd,22,0,0x0f,0xac,1,2,0}, frame[512];
  os_memcpy(plain + 8, group2, sizeof(group2));
  size_t length = key_message(frame, 2 | WPA_KEY_INFO_ACK | WPA_KEY_INFO_MIC |
    WPA_KEY_INFO_SECURE | WPA_KEY_INFO_ENCR_KEY_DATA, 4, plain, sizeof(plain));
  eapol(frame, length);
}
static void transmitted(const struct nw_control_view *request)
{
  size_t length; const u8 *data = nw_control_get(request, NW_CONTROL_ATTR_EAPOL, &length);
  assert(data && length >= 99 && length <= 512);
  const struct wpa_eapol_key *key = (const void *)(data + 4);
  unsigned info = WPA_GET_BE16(key->key_info);
  assert(!(info & WPA_KEY_INFO_ACK));
  if ((info & WPA_KEY_INFO_KEY_TYPE) && !(info & WPA_KEY_INFO_SECURE)) {
    assert(++m2_seen == 1);
    u8 pmk[32];
    assert(!pbkdf2_sha1((const char *)password, ssid, sizeof(ssid) - 1, 4096, pmk, sizeof(pmk)));
    assert(!wpa_pmk_to_ptk(pmk, sizeof(pmk), "Pairwise key expansion", ap, station,
      anonce, key->key_nonce, &ptk, WPA_KEY_MGMT_PSK, WPA_CIPHER_CCMP, NULL, 0, 0));
    forced_memzero(pmk, sizeof(pmk));
  }
  u8 check[512], mic[16]; os_memcpy(check, data, length); os_memset(check + 81, 0, 16);
  assert(!wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_PSK,
    RSN_HASH_SHA256, 2, check, length, mic));
  if (wrong_password) {
    assert(os_memcmp(mic, data + 81, 16) && m2_seen == 1);
    u8 bytes[128]; struct nw_control_builder b;
    event_begin(&b, bytes, sizeof(bytes), NW_CONTROL_DISCONNECTED);
    assert(!nw_control_put(&b, NW_CONTROL_ATTR_BSSID, ap, sizeof(ap)));
    assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_REASON, WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT));
    assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_LOCAL_ONLY, 0));
    send_packet(&b); return;
  }
  assert(!os_memcmp(mic, data + 81, 16));
  if ((info & WPA_KEY_INFO_KEY_TYPE) && !(info & WPA_KEY_INFO_SECURE)) {
    message3_send();
  } else if (info & WPA_KEY_INFO_KEY_TYPE) {
    assert(++m4_seen <= 2);
    if (m4_seen == 2) group_rekey();
  } else {
    assert(++rekey_seen == 1);
    __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
  }
}
static void install(const struct nw_control_view *request)
{
  uint32_t flags, cipher, index;
  assert(!nw_control_get_u32(request, NW_CONTROL_ATTR_KEY_FLAGS, &flags));
  if (flags & NW_CONTROL_KEY_DELETE) return;
  assert(!nw_control_get_u32(request, NW_CONTROL_ATTR_CIPHER, &cipher));
  assert(!nw_control_get_u32(request, NW_CONTROL_ATTR_KEY_INDEX, &index));
  size_t length; const u8 *key = nw_control_get(request, NW_CONTROL_ATTR_KEY, &length);
  assert(cipher == RSN_CIPHER_SUITE_CCMP && key && length == 16);
  if (flags & NW_CONTROL_KEY_PAIRWISE) {
    assert(index == 0 && ++ptk_installs == 1 && !os_memcmp(key, ptk.tk, 16));
  } else {
    gtk_installs++;
    if (index == 1) assert(gtk_installs == 1 && !os_memcmp(key, gtk, 16));
    else assert(index == 2 && gtk_installs == 2 && !os_memcmp(key, group2, 16));
  }
}
static void *kernel(void *unused)
{
  (void)unused;
  for (;;) {
    u8 bytes[8192], reply_bytes[8192]; struct nw_control_view request; struct nw_control_builder reply;
    ssize_t length = recv(kernel_fd, bytes, sizeof(bytes), 0);
    if (__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) break;
    assert(length > 0 && !nw_control_decode(bytes, length, &request));
    assert(request.header.session == session);
    assert(!nw_control_begin(&reply, reply_bytes, sizeof(reply_bytes), NW_CONTROL_REPLY,
      request.header.operation, request.header.sequence, session, generation, NW_CONTROL_OK));
    unsigned operation = request.header.operation;
    switch (operation) {
    case NW_CONTROL_INFO: {
      u8 ciphers[8]; WPA_PUT_LE32(ciphers, RSN_CIPHER_SUITE_CCMP);
      WPA_PUT_LE32(ciphers + 4, RSN_CIPHER_SUITE_AES_128_CMAC);
      assert(!nw_control_put(&reply, NW_CONTROL_ATTR_MAC, station, sizeof(station)));
      assert(!nw_control_put(&reply, NW_CONTROL_ATTR_CIPHERS, ciphers, sizeof(ciphers)));
      assert(!nw_control_put_u32(&reply, NW_CONTROL_ATTR_MAX_SCAN_SSIDS, 16));
      break;
    }
    case NW_CONTROL_SCAN: break;
    case NW_CONTROL_GET_BSS: {
      uint32_t index; assert(!nw_control_get_u32(&request, NW_CONTROL_ATTR_BSS_INDEX, &index));
      if (index) { nw_control_reply_state(&reply, generation, NW_CONTROL_NOT_FOUND); break; }
      u8 ies[128] = {0, sizeof(ssid) - 1}; size_t count = 2;
      os_memcpy(ies + count, ssid, sizeof(ssid) - 1); count += sizeof(ssid) - 1;
      u8 rates[] = {1,8,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c};
      os_memcpy(ies + count, rates, sizeof(rates)); count += sizeof(rates);
      if (!open_network) { os_memcpy(ies + count, rsn, sizeof(rsn)); count += sizeof(rsn); }
      assert(!nw_control_put(&reply, NW_CONTROL_ATTR_BSSID, ap, sizeof(ap)));
      assert(!nw_control_put(&reply, NW_CONTROL_ATTR_IE, ies, count));
      assert(!nw_control_put_u32(&reply, NW_CONTROL_ATTR_FREQUENCY, 5180));
      assert(!nw_control_put_u32(&reply, NW_CONTROL_ATTR_SIGNAL, (u32)-40));
      assert(!nw_control_put_u32(&reply, NW_CONTROL_ATTR_CAPABILITY, open_network ? 0x01 : 0x11));
      assert(!nw_control_put_u32(&reply, NW_CONTROL_ATTR_BEACON_INTERVAL, 100)); break;
    }
    case NW_CONTROL_AUTHENTICATE: break;
    case NW_CONTROL_ASSOCIATE: {
      uint32_t cipher, akm;
      assert(!nw_control_get_u32(&request, NW_CONTROL_ATTR_PAIRWISE_CIPHER, &cipher));
      assert(!nw_control_get_u32(&request, NW_CONTROL_ATTR_AKM, &akm));
      assert(open_network ? (cipher == 0 && akm == 0) :
        (cipher == RSN_CIPHER_SUITE_CCMP && akm == RSN_AUTH_KEY_MGMT_PSK_OVER_802_1X)); break;
    }
    case NW_CONTROL_SET_KEY: install(&request); break;
    case NW_CONTROL_SET_PORT: {
      uint32_t value; assert(!nw_control_get_u32(&request, NW_CONTROL_ATTR_AUTHORIZED, &value));
      if (value) { assert(open_network ? (!ptk_installs && !gtk_installs) : (ptk_installs == 1 && gtk_installs >= 1)); authorized++; } break;
    }
    case NW_CONTROL_SET_OPERSTATE: break;
    case NW_CONTROL_DEAUTHENTICATE: break;
    case NW_CONTROL_TX_EAPOL: transmitted(&request); break;
    case NW_CONTROL_REPORT_RESULT: {
      uint32_t status;
      assert(wrong_password && !nw_control_get_u32(&request, NW_CONTROL_ATTR_STATUS_CODE, &status) && status == NW_CONTROL_AUTH_FAILED);
      wrong_key_reports++; __atomic_store_n(&finished, 1, __ATOMIC_RELEASE); break;
    }
    default: assert(!"unexpected driver request");
    }
    send_packet(&reply);
    struct nw_control_builder b; u8 event_bytes[1024];
    switch (operation) {
    case NW_CONTROL_INFO:
      generation++;
      event_begin(&b, event_bytes, sizeof(event_bytes), NW_CONTROL_JOIN_REQUEST);
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_SSID, ssid, sizeof(ssid) - 1));
      if (!open_network) assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, 5180));
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_BSSID, ap, sizeof(ap)));
      if (!open_network) {
        const char *pass = wrong_password ? "wrong-passphrase" : (const char *)password;
        assert(!nw_control_put(&b, NW_CONTROL_ATTR_PASSWORD, pass, strlen(pass)));
      }
      send_packet(&b); break;
    case NW_CONTROL_SCAN:
      {
        size_t bytes; const u8 *frequencies = nw_control_get(&request, NW_CONTROL_ATTR_SCAN_FREQUENCIES, &bytes);
        assert(open_network ? bytes == 0 : (frequencies && bytes == 4 && WPA_GET_LE32(frequencies) == 5180));
      }
      event_begin(&b, event_bytes, sizeof(event_bytes), NW_CONTROL_SCAN_DONE); send_packet(&b); break;
    case NW_CONTROL_AUTHENTICATE: {
      u8 frame[30] = {0xb0};
      os_memcpy(frame + 4, station, 6); os_memcpy(frame + 10, ap, 6); os_memcpy(frame + 16, ap, 6); frame[26] = 2;
      event_begin(&b, event_bytes, sizeof(event_bytes), NW_CONTROL_AUTH_FRAME);
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_FRAME, frame, sizeof(frame))); send_packet(&b); break;
    }
    case NW_CONTROL_ASSOCIATE: {
      event_begin(&b, event_bytes, sizeof(event_bytes), NW_CONTROL_ASSOC_RESULT);
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_BSSID, ap, 6));
      assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_STATUS_CODE, 0));
      assert(!nw_control_put_u32(&b, NW_CONTROL_ATTR_FREQUENCY, 5180));
      size_t size; const void *ies = nw_control_get(&request, NW_CONTROL_ATTR_IE, &size);
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_REQUEST_IE, ies, size));
      assert(!nw_control_put(&b, NW_CONTROL_ATTR_IE, rsn, open_network ? 0 : sizeof(rsn))); send_packet(&b);
      if (!open_network) message1(); break;
    }
    case NW_CONTROL_SET_OPERSTATE: {
      uint32_t state; assert(!nw_control_get_u32(&request, NW_CONTROL_ATTR_OPERSTATE, &state));
      if (state && m4_seen == 1) {
        eapol(message3, message3_length); /* Exact replay must be dropped. */
        u8 retry[512]; os_memcpy(retry, message3, message3_length);
        WPA_PUT_BE64(retry + 9, 3); os_memset(retry + 81, 0, 16);
        assert(!wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_PSK,
          RSN_HASH_SHA256, 2, retry, message3_length, retry + 81));
        eapol(retry, message3_length); /* Legitimate AP retransmission. */
      }
      break;
    }
    }
    nw_control_clear(bytes, sizeof(bytes));
  }
  return NULL;
}
static void power_waits_for_selection(void)
{
  u8 bytes[96]; struct nw_control_builder builder; struct nw_control_view view;
  event_begin(&builder, bytes, sizeof(bytes), NW_CONTROL_POWER_REQUEST);
  assert(!nw_control_put_u32(&builder, NW_CONTROL_ATTR_OPERSTATE, 1));
  assert(!nw_control_decode(bytes, builder.length, &view));
  assert(!nw_supplicant_request(supplicant, &view));
  assert(supplicant->disconnected && supplicant->wpa_state == WPA_DISCONNECTED);
}
static void check(void *unused, void *unused2)
{
  (void)unused; (void)unused2;
  if ((open_network && supplicant->wpa_state == WPA_COMPLETED) || __atomic_load_n(&finished, __ATOMIC_ACQUIRE)) {
    if (wrong_password) {
      assert(supplicant->wpa_state != WPA_COMPLETED && wrong_key_reports == 1 && m2_seen == 1);
      assert(!ptk_installs && !gtk_installs && !m4_seen && !authorized);
      passed = 1; eloop_terminate(); return;
    }
    assert(supplicant->wpa_state == WPA_COMPLETED);
    assert(supplicant->current_ssid && supplicant->current_ssid->bssid_set &&
           !os_memcmp(supplicant->current_ssid->bssid, ap, ETH_ALEN));
    if (open_network) {
      assert(!ptk_installs && !gtk_installs && !m2_seen && !m4_seen && !rekey_seen && authorized);
      power_waits_for_selection();
      passed = 1; eloop_terminate(); return;
    }
    assert(ptk_installs == 1 && gtk_installs == 2 && m2_seen == 1 && m4_seen == 2 && rekey_seen == 1);
    assert(!wrong_key_reports);
    power_waits_for_selection();
    passed = 1; eloop_terminate(); return;
  }
  eloop_register_timeout(0, 1000, check, NULL, NULL);
}
static void timeout(void *unused, void *unused2)
{
  (void)unused; (void)unused2;
  fprintf(stderr, "timeout: state=%d m2=%u m4=%u ptk=%u gtk=%u rekey=%u\n",
    supplicant->wpa_state, m2_seen, m4_seen, ptk_installs, gtk_installs, rekey_seen);
  eloop_terminate();
}
int main(int argc, char **argv)
{
  assert(argc == 3); open_network = !strcmp(argv[2], "open"); wrong_password = !strcmp(argv[2], "wrong");
  int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets));
  user_fd = sockets[0]; kernel_fd = sockets[1];
  int wake_fd = dup(user_fd); assert(wake_fd >= 0);
  for (unsigned i = 0; i < 32; i++) anonce[i] = i + 1;
  for (unsigned i = 0; i < 16; i++) { gtk[i] = i + 40; group2[i] = i + 80; }
  pthread_t thread; assert(!pthread_create(&thread, NULL, kernel, NULL));
  struct wpa_params params; os_memset(&params, 0, sizeof(params)); params.wpa_debug_level = MSG_WARNING;
  struct wpa_global *global = wpa_supplicant_init(&params); assert(global);
  struct wpa_interface interface; os_memset(&interface, 0, sizeof(interface));
  interface.ifname = "en0"; interface.driver = "sheng"; interface.confname = argv[1];
  supplicant = wpa_supplicant_add_iface(global, &interface, NULL); assert(supplicant);
  eloop_register_timeout(0, 1000, check, NULL, NULL);
  eloop_register_timeout(12, 0, timeout, NULL, NULL);
  wpa_supplicant_run(global);
  eloop_cancel_timeout(check, NULL, NULL); eloop_cancel_timeout(timeout, NULL, NULL);
  wpa_supplicant_deinit(global);
  __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
  assert(send(wake_fd, "x", 1, 0) == 1);
  pthread_join(thread, NULL); close(kernel_fd); close(wake_fd);
  assert(passed);
  if (wrong_password) puts("supplicant: actual wrong passphrase produces an invalid M2 MIC; AP rejection reaches upstream WRONG_KEY decision and native failure report; no keys installed or port authorized passed");
  else if (open_network) puts("supplicant: original open-network scan/auth/assoc and authorized port, pinned BSSID, no key/EAPOL installation passed");
  else puts("supplicant: original scan/auth/assoc, pinned BSSID, passphrase PBKDF2, four-way handshake, PTK/GTK, group rekey, MIC rejection, stale-generation rejection, M3 replay without reinstall, wrong-key notification passed");
  return 0;
}
