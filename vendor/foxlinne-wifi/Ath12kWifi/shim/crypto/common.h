/* Kernel platform bindings for the unmodified hostap portable AES sources. */
#ifndef NW_PORTABLE_CRYPTO_COMMON_H
#define NW_PORTABLE_CRYPTO_COMMON_H
#include "lx.h"
#define os_malloc(n) lx_alloc(n, 0)
#define os_free(p) lx_free(p)
#define os_memcpy memcpy
#define os_memset memset
#define os_memcmp memcmp
static inline void *os_memdup(const void *source, size_t length) {
  void *copy = lx_alloc(length, 0); if (copy) memcpy(copy, source, length); return copy;
}
static inline void bin_clear_free(void *pointer, size_t length) {
  if (pointer) { memzero_explicit(pointer, length); lx_free(pointer); }
}
#define TEST_FAIL() 0
/* Crypto tracing is disabled, including all dumps of keys and plaintext. */
#define wpa_hexdump_key(...) ((void)0)
#define wpa_printf(...) ((void)0)
static inline int os_memcmp_const(const void *a, const void *b, size_t len) {
  const volatile u8 *x = a, *y = b; u8 difference = 0;
  while (len--) difference |= *x++ ^ *y++;
  return difference;
}
static inline u32 WPA_GET_BE32(const u8 *p) {
  return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static inline void WPA_PUT_BE16(u8 *p, u16 value) { p[0] = value >> 8; p[1] = value; }
static inline void WPA_PUT_BE32(u8 *p, u32 value) {
  p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static inline void WPA_PUT_BE64(u8 *p, u64 value) {
  WPA_PUT_BE32(p, value >> 32); WPA_PUT_BE32(p + 4, value);
}
#endif
