/* Included after unchanged helpers extracted from the prepared ath12k source. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_plat.h"
static unsigned dma_allocations, dma_frees, heap_allocations, heap_frees;
static bool fail_dma;
static void *heap_alloc(size_t size) { void *p = malloc(size); if (p) heap_allocations++; return p; }
static void heap_free(void *pointer) { heap_frees++; free(pointer); }
static void *dma_alloc(size_t size, u64 *bus) {
  if (fail_dma) return NULL;
  void *p = calloc(1, size); if (p) { dma_allocations++; *bus = (uintptr_t)p; } return p;
}
static void dma_free(size_t size, void *pointer, u64 bus) {
  assert(size && (uintptr_t)pointer == bus); dma_frees++; free(pointer);
}
static void halt(void) { abort(); }
static void log_line(int level, const char *line) { (void)level; (void)line; }
static const struct lx_platform platform = { .alloc = heap_alloc, .free = heap_free,
  .dma_alloc = dma_alloc, .dma_free = dma_free, .halt = halt, .log = log_line };
static void cipher_layout(u32 cipher, unsigned iv_length, unsigned icv_length, bool qos) {
  u8 bytes[160]; unsigned mac_length = qos ? 26 : 24, length = mac_length + 73;
  for (unsigned i = 0; i < length; i++) bytes[i] = (u8)(i ^ 0xa5);
  put_unaligned_le16(IEEE80211_FTYPE_DATA | (qos ? IEEE80211_STYPE_QOS_DATA : 0) |
                     IEEE80211_FCTL_TODS | IEEE80211_FCTL_PROTECTED, bytes);
  const u8 snap[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
  memcpy(bytes + mac_length, snap, sizeof(snap));
  struct sk_buff *packet = alloc_skb(length, GFP_KERNEL); assert(packet);
  memcpy(skb_put(packet, length), bytes, length);
  struct sk_buff *original = skb_clone(packet, GFP_KERNEL); assert(original);
  struct ath12k_skb_cb cb = { .cipher = cipher };
  struct hal_tx_info info = { .flags0 = 0x123 };
  struct nw_buffer_stats before, after; nw_wifi_buffer_stats_get(&before);
  assert(!ath12k_wifi7_dp_tx_encap_eapol(packet, &info, &cb));
  nw_wifi_buffer_stats_get(&after);
  assert(after.dma_allocations == before.dma_allocations + 1 && after.copied_bytes == before.copied_bytes + length);
  assert(packet->len == length + iv_length + icv_length && packet->head != original->head);
  assert(!memcmp(packet->data, bytes, mac_length));
  for (unsigned i = 0; i < iv_length; i++) assert(packet->data[mac_length + i] == 0);
  assert(!memcmp(packet->data + mac_length + iv_length, bytes + mac_length, length - mac_length));
  for (unsigned i = 0; i < icv_length; i++) assert(packet->data[length + iv_length + i] == 0);
  assert(info.encap_type == HAL_TCL_ENCAP_TYPE_RAW && info.encrypt_type == ath12k_dp_tx_get_encrypt_type(cipher));
  assert(info.flags0 == (0x123 | u32_encode_bits(1, HAL_TCL_DATA_CMD_INFO2_TO_FW)));
  assert(original->len == length && !memcmp(original->data, bytes, length));
  kfree_skb(packet); kfree_skb(original);

  packet = alloc_skb(length, GFP_KERNEL); assert(packet); memcpy(skb_put(packet, length), bytes, length);
  info = (struct hal_tx_info){ .flags0 = 0x456, .encap_type = HAL_TCL_ENCAP_TYPE_NATIVE_WIFI };
  struct hal_tx_info unchanged = info; u8 *head = packet->head;
  fail_dma = true;
  assert(ath12k_wifi7_dp_tx_encap_eapol(packet, &info, &cb) == -ENOMEM);
  fail_dma = false;
  assert(packet->head == head && packet->len == length && !memcmp(packet->data, bytes, length));
  assert(!memcmp(&info, &unchanged, sizeof(info))); kfree_skb(packet);
}
int main(void) {
  lx_plat = &platform;
  for (unsigned qos = 0; qos < 2; qos++) {
    cipher_layout(WLAN_CIPHER_SUITE_CCMP, 8, 8, qos);
    cipher_layout(WLAN_CIPHER_SUITE_CCMP_256, 8, 16, qos);
    cipher_layout(WLAN_CIPHER_SUITE_GCMP, 8, 16, qos);
    cipher_layout(WLAN_CIPHER_SUITE_GCMP_256, 8, 16, qos);
    cipher_layout(WLAN_CIPHER_SUITE_TKIP, 8, 4, qos);
    cipher_layout(WLAN_CIPHER_SUITE_WEP40, 4, 4, qos);
    cipher_layout(WLAN_CIPHER_SUITE_WEP104, 4, 4, qos);
  }
  assert(dma_allocations == dma_frees && heap_allocations == heap_frees);
  puts("PASS: encrypted EAPOL RAW layouts for CCMP/GCMP/TKIP/WEP, QoS and non-QoS; one copy, clone isolation, allocation rollback and balanced owners");
}
