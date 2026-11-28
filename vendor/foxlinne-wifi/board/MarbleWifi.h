// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// marble-wifi=1 surveys clock/pin handoff; =2 powers PCIe0 and identifies the endpoint.
void marble_wifi_start(void);
void marble_wifi_wake(void);
void marble_wifi_net_start(const uint8_t mac[6]);
unsigned marble_wifi_report(char *, unsigned);
// stage, errno, rail mask, TCS status, GDSC, halted branches, PHY, LTSSM,
// link, vendor/device, subsystem, BAR0, BAR bytes, SoC version, subversion,
// MHI version, MHI status, BHI offset, BHI environment, PCI command,
// WLAN pin, PERST pin, capability pointer, requested stage.
extern volatile uint32_t native_marble_wifi_stats[24];
#ifdef __cplusplus
}
#endif
