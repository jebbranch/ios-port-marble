/* IEEE CRC-32 helpers used by mac80211's management-element cache. */
#include <linux/crc32.h>
u32 crc32_le(u32 crc, const void *data, size_t length) {
  const u8 *p = data;
  while (length--) {
    crc ^= *p++;
    for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}
u32 crc32_be(u32 crc, const void *data, size_t length) {
  const u8 *p = data;
  while (length--) {
    crc ^= (u32)*p++ << 24;
    for (unsigned bit = 0; bit < 8; bit++) crc = (crc << 1) ^ (0x04c11db7U & (0U - (crc >> 31)));
  }
  return crc;
}
