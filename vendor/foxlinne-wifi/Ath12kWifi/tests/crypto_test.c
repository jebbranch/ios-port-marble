#include "nw_crypto.h"
#include <stdio.h>
#include <string.h>
static int fails;
static void hexd(const char *h, uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = v; } }
static void check(const char *name, const uint8_t *got, const char *want, size_t n) { uint8_t w[128]; hexd(want, w, n); if (memcmp(got, w, n)) { printf("FAIL %s\n", name); fails++; } else printf("ok   %s\n", name); }
int main(void) {
  uint8_t o[64];
  nw_sha1((const uint8_t *)"abc", 3, o); check("sha1 abc", o, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);
  nw_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, o); check("hmac rfc2202 #2", o, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20);
  nw_pbkdf2_sha1("password", 8, (const uint8_t *)"IEEE", 4, 4096, o, 32); check("pbkdf2 802.11i H.4.1", o, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);
  nw_pbkdf2_sha1("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, 4096, o, 32); check("pbkdf2 802.11i H.4.1 #2", o, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32);
  uint8_t k[16], pt[16]; uint32_t rk4[44]; hexd("000102030405060708090a0b0c0d0e0f", k, 16); hexd("00112233445566778899aabbccddeeff", pt, 16);
  nw_aes128_expand(k, rk4); nw_aes128_encrypt(rk4, pt, o); check("aes128 enc fips197", o, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
  nw_aes128_decrypt(rk4, o, o); check("aes128 dec fips197", o, "00112233445566778899aabbccddeeff", 16);
  uint8_t wrapped[24], key[16]; hexd("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", wrapped, 24); hexd("000102030405060708090a0b0c0d0e0f", k, 16);
  int r = nw_aes_unwrap(k, wrapped, 24, o); printf("%s unwrap rfc3394 4.1 rc=%d\n", r ? "FAIL" : "ok  ", r); if (r) fails++; else check("unwrap data", o, "00112233445566778899aabbccddeeff", 16);
  (void)key;
  printf("%s\n", fails ? "FAILED" : "all passed"); return fails ? 1 : 0; }
