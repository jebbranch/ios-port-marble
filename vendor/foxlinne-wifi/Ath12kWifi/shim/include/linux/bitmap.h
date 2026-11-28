#ifndef LX_BITMAP_H
#define LX_BITMAP_H
#include "lx_os.h"
static inline void bitmap_zero(unsigned long *p, unsigned n) { memset(p, 0, BITS_TO_LONGS(n) * sizeof *p); }
static inline void bitmap_copy(unsigned long *to, const unsigned long *from, unsigned n) {
  memcpy(to, from, BITS_TO_LONGS(n) * sizeof *to);
}
static inline bool bitmap_empty(const unsigned long *p, unsigned n) {
  for (unsigned i = 0; i < n / 64; i++) if (p[i]) return false;
  return !(n % 64) || !(p[n / 64] & ((1UL << (n % 64)) - 1));
}
static inline bool bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned n) {
  for (unsigned i = 0; i < n / 64; i++) if (a[i] & ~b[i]) return false;
  return !(n % 64) || !((a[n / 64] & ~b[n / 64]) & ((1UL << (n % 64)) - 1));
}
static inline unsigned long *bitmap_zalloc(unsigned n, gfp_t flags) {
  (void)flags; return lx_alloc(BITS_TO_LONGS(n) * sizeof(unsigned long), 1);
}
static inline void bitmap_free(const unsigned long *p) { lx_free(p); }
#endif
