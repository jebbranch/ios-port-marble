#ifndef NW_CRYPTO_H
#define NW_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
/* The primitives of WPA2-Personal: SHA-1 (FIPS 180), HMAC (RFC 2104), PBKDF2 (RFC 2898), AES-128 (FIPS 197), AES key wrap (RFC 3394) and
 * the 802.11 pseudo-random function. Plain C with no allocation, so the host tests and the kernel build use the same file. */
void nw_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void nw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *data, size_t len, uint8_t out[20]);
void nw_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned iterations, uint8_t *out, size_t olen);
void nw_aes128_expand(const uint8_t key[16], uint32_t rk[44]);
void nw_aes128_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
void nw_aes128_decrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
/* in: n 64-bit blocks of wrapped data (n >= 2 including the check block); out gets (n - 1) * 8 bytes. Returns 0 when the integrity check holds. */
int nw_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out);
/* IEEE 802.11-2016 12.7.1.2: PRF-n over key, label, data; bits is a multiple of 8, out holds bits / 8 bytes. */
void nw_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t bits);
#endif
