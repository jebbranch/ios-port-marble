#include "lx_os.h"
#ifndef LX_CTYPE_H
#define LX_CTYPE_H
static inline int isalpha(int c) { return ((unsigned)c | 32u) - 'a' <= 'z' - 'a'; }
static inline int toupper(int c) { return (unsigned)c - 'a' <= 'z' - 'a' ? c - ('a' - 'A') : c; }
#endif
