/* Published CCM/GCM/CMAC vectors through the production mac80211 adapters. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_pci.h"
#include "aead_api.h"
#include "aes_cmac.h"
#include "aes_gmac.h"
#include "aes_wrap.h"
#include "aes_siv.h"

static unsigned live;
static int fail_after = -1;
void *lx_alloc(size_t len, int zero) {
  if (fail_after == 0) return NULL;
  if (fail_after > 0) fail_after--;
  void *p = zero ? calloc(1, len) : malloc(len);
  if (p) live++;
  return p;
}
void lx_free(const void *p) { if (p) { assert(live); live--; free((void *)p); } }
static size_t unhex(const char *text, u8 *out) {
  size_t n = 0; unsigned value;
  while (*text) { assert(sscanf(text, "%2x", &value) == 1); out[n++] = value; text += 2; }
  return n;
}
static void equal_hex(const u8 *data, const char *text) {
  u8 expected[256]; size_t len = unhex(text, expected); assert(!memcmp(data, expected, len));
}
static void gcm_vector(size_t key_len, const char *cipher, const char *auth) {
  u8 key[32] = {0}, iv[16] = {0}, buffer[18] = {0}, tag_store[18] = {0};
  u8 *data = buffer + 1, *tag = tag_store + 1;
  struct crypto_aead *tfm = aead_key_setup_encrypt("gcm(aes)", key, key_len, 16);
  assert(!IS_ERR(tfm));
  assert(!aead_encrypt(tfm, iv, NULL, 0, data, 16, tag));
  equal_hex(data, cipher); equal_hex(tag, auth);
  u8 saved[16]; memcpy(saved, data, 16); tag[0] ^= 1;
  assert(aead_decrypt(tfm, iv, NULL, 0, data, 16, tag) == -EBADMSG);
  assert(!memcmp(data, saved, 16)); tag[0] ^= 1;
  assert(!aead_decrypt(tfm, iv, NULL, 0, data, 16, tag));
  for (unsigned i = 0; i < 16; i++) assert(data[i] == 0);
  aead_key_free(tfm);
}
static void ccm_vector(void) {
  u8 key[16], iv[16] = {1}, aad[9], buffer[25], tag[8];
  unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key);
  unhex("00000003020100a0a1a2a3a4a5", iv + 1);
  unhex("0001020304050607", aad + 1);
  unhex("08090a0b0c0d0e0f101112131415161718191a1b1c1d1e", buffer + 1);
  struct crypto_aead *tfm = aead_key_setup_encrypt("ccm(aes)", key, 16, 8);
  assert(!IS_ERR(tfm));
  assert(!aead_encrypt(tfm, iv, aad + 1, 8, buffer + 1, 23, tag));
  equal_hex(buffer + 1, "588c979a61c663d2f066d0c2c0f989806d5f6b61dac384");
  equal_hex(tag, "17e8d12cfdf926e0");
  u8 saved[23]; memcpy(saved, buffer + 1, 23); tag[1] ^= 1;
  assert(aead_decrypt(tfm, iv, aad + 1, 8, buffer + 1, 23, tag) == -EBADMSG);
  assert(!memcmp(saved, buffer + 1, 23)); tag[1] ^= 1;
  assert(!aead_decrypt(tfm, iv, aad + 1, 8, buffer + 1, 23, tag));
  equal_hex(buffer + 1, "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e");
  for (int failure = 0; failure < 2; failure++) {
    fail_after = failure;
    assert(aead_encrypt(tfm, iv, aad + 1, 8, buffer + 1, 23, tag) < 0);
    equal_hex(buffer + 1, "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e");
    fail_after = -1;
  }
  assert(aead_encrypt(tfm, iv, aad, 31, buffer, 1, tag) == -EINVAL);
  assert(aead_encrypt(tfm, iv, aad, 8, buffer, 65536, tag) == -EINVAL);
  aead_key_free(tfm);
}
static void management_mic(void) {
  u8 key[32], msg[64], aad[20] = {0}, data[40] = {0}, tag[16], expected[16];
  unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
  unhex("6bc1bee22e409f96e93d7e117393172a", msg);
  assert(!omac1_aes_128(key, msg, 16, tag));
  equal_hex(tag, "070a16b46b4d4144f79bdd9dd04a287c");
  /* BIP includes a zeroed MIC and beacon timestamp, never their wire values. */
  for (unsigned beacon = 0; beacon < 2; beacon++) {
    aad[0] = beacon ? 0x80 : 0xd0;
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = i + 1;
    u8 flattened[60]; memcpy(flattened, aad, 20); memcpy(flattened + 20, data, 40);
    if (beacon) memset(flattened + 20, 0, 8);
    memset(flattened + 52, 0, 8);
    assert(!omac1_aes_128(key, flattened, sizeof(flattened), expected));
    struct crypto_shash *cmac = ieee80211_aes_cmac_key_setup(key, 16);
    assert(!IS_ERR(cmac));
    assert(!ieee80211_aes_cmac(cmac, aad, data, 40, tag, 8));
    assert(!memcmp(tag, expected, 8));
    /* Empty body before the MIC is valid for a non-beacon. */
    if (!beacon) assert(!ieee80211_aes_cmac(cmac, aad, data, 8, tag, 8));
    ieee80211_aes_cmac_key_free(cmac);
    memset(flattened + 44, 0, 16);
    u8 nonce[12] = {0};
    assert(!aes_gmac(key, 16, nonce, 12, flattened, sizeof(flattened), expected));
    struct crypto_aead *gmac = ieee80211_aes_gmac_key_setup(key, 16);
    assert(!ieee80211_aes_gmac(gmac, aad, nonce, data, 40, tag));
    assert(!memcmp(tag, expected, 16)); ieee80211_aes_gmac_key_free(gmac);
  }
}
static void siv_vector(void) {
  u8 key[32], aad[24], plain[14], cipher[31], output[15];
  unhex("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", key);
  unhex("101112131415161718191a1b1c1d1e1f2021222324252627", aad);
  unhex("112233445566778899aabbccddee", plain);
  const u8 *addresses[] = {aad}; size_t lengths[] = {sizeof(aad)};
  assert(!aes_siv_encrypt(key, sizeof(key), plain, sizeof(plain), 1, addresses, lengths, cipher + 1));
  equal_hex(cipher + 1, "85632d07c6e8f37f950acd320a2ecc9340c02b9690c4dc04daef7f6afe5c");
  assert(!aes_siv_decrypt(key, sizeof(key), cipher + 1, 30, 1, addresses, lengths, output + 1));
  assert(!memcmp(output + 1, plain, sizeof(plain)));
  cipher[1] ^= 1;
  assert(aes_siv_decrypt(key, sizeof(key), cipher + 1, 30, 1, addresses, lengths, output + 1));
  fail_after = 0;
  assert(aes_siv_encrypt(key, sizeof(key), plain, sizeof(plain), 1, addresses, lengths, cipher + 1));
  fail_after = -1;
}
int main(void) {
  gcm_vector(16, "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf");
  gcm_vector(32, "cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919");
  ccm_vector(); management_mic(); siv_vector(); assert(live == 0);
  puts("PASS: CCM RFC3610, GCM NIST AES128/256, CMAC NIST, SIV RFC5297, BIP masks, unaligned buffers, tamper and allocation failure");
}
