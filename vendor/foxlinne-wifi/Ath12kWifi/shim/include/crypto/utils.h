#ifndef LX_CRYPTO_UTILS_H
#define LX_CRYPTO_UTILS_H
#include "lx.h"
static inline int crypto_memneq(const void *a, const void *b, size_t n) {
  const volatile u8 *x = a, *y = b; u8 difference = 0;
  for (size_t i = 0; i < n; i++) difference |= x[i] ^ y[i];
  return difference != 0;
}
static inline void crypto_xor(u8 *to, const u8 *from, unsigned n) {
  for (unsigned i = 0; i < n; i++) to[i] ^= from[i];
}
#endif
