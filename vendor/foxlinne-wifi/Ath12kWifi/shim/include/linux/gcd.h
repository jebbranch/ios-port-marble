#ifndef LX_GCD_H
#define LX_GCD_H
static inline unsigned long gcd(unsigned long a, unsigned long b) {
  while (b) { unsigned long remainder = a % b; a = b; b = remainder; }
  return a;
}
#endif
