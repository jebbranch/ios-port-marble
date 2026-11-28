/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LX_MATH_H
#define LX_MATH_H
#include "lx.h"
/* Same signed/unsigned rounding contract as the matching Linux math.h. */
#define DIV_ROUND_CLOSEST(x, divisor) ({ \
  __typeof__(x) _x = (x); __typeof__(divisor) _d = (divisor); \
  (((__typeof__(x))-1) > 0 || ((__typeof__(divisor))-1) > 0 || ((_x > 0) == (_d > 0))) ? \
    (_x + _d / 2) / _d : (_x - _d / 2) / _d; })
#endif
