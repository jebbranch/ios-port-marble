#ifndef LX_NOSPEC_H
#define LX_NOSPEC_H
#include "lx.h"
static inline unsigned long lx_index_mask(unsigned long index, unsigned long size) {
  unsigned long mask;
#if defined(__aarch64__)
  __asm__ volatile("cmp %1, %2\n\tcsetm %0, lo\n\tcsdb" : "=r"(mask) : "r"(index), "r"(size) : "cc");
#else
  mask = index < size ? ~0UL : 0;
#if defined(__x86_64__)
  __asm__ volatile("lfence" ::: "memory");
#endif
#endif
  return mask;
}
#define array_index_nospec(index, size) ({ __typeof__(index) _i = (index); _i & (__typeof__(_i))lx_index_mask((unsigned long)_i, (unsigned long)(size)); })
#endif
