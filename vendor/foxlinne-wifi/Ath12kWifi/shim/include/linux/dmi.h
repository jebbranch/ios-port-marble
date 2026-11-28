/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "lx_ath.h"
enum {DMI_BOARD_VENDOR, DMI_PRODUCT_NAME, DMI_SYS_VENDOR};
struct dmi_strmatch {int slot; const char *substr;};
struct dmi_system_id {int (*callback)(const struct dmi_system_id *); const char *ident; struct dmi_strmatch matches[4]; void *driver_data;};
#define DMI_MATCH(field, value) {field, value}
/* The board supplies DT identity; the x86 SMBIOS quirk table has no match. */
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *table) { (void)table; return NULL; }
