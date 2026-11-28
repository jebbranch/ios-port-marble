#ifndef LX_CRC32_H
#define LX_CRC32_H
#include "lx.h"
u32 crc32_le(u32 crc, const void *data, size_t length);
u32 crc32_be(u32 crc, const void *data, size_t length);
static inline u32 crc32(u32 crc, const void *data, size_t length) { return crc32_le(crc, data, length); }
#endif
