#ifndef LX_LIBC_STDIO_H
#define LX_LIBC_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *, size_t, const char *, va_list);
int sscanf(const char *, const char *, ...) __attribute__((format(scanf, 2, 3)));
#endif
