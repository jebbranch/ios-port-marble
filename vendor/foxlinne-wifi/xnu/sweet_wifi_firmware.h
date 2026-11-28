#ifndef SWEET_WIFI_FIRMWARE_H
#define SWEET_WIFI_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

/* lx_platform.fw_get-compatible: borrowed, read-only kernel storage. */
int sweet_wifi_firmware_get(const char *, const uint8_t **, size_t *);
size_t sweet_wifi_firmware_inventory(void);

#endif
