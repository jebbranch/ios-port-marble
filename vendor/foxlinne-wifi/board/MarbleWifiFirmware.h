/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MARBLE_WIFI_FIRMWARE_PREFIX "firmware/ath11k/WCN6855/hw2.1"
#define MARBLE_WIFI_FIRMWARE_ROOT "/private/var/ngpu/" MARBLE_WIFI_FIRMWARE_PREFIX
#ifdef __cplusplus
extern "C" {
#endif
int marble_wifi_firmware_read(const char *, uint8_t **, size_t *);
void marble_wifi_runtime(volatile uint32_t *, size_t, volatile uint32_t *, volatile uint32_t *, unsigned);
extern volatile uint32_t native_marble_wifi_runtime_stats[16];
#ifdef __cplusplus
}
#endif
