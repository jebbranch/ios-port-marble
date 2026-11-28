/* Firmware provider for the marble board adapter; no VFS or GPU dependency. */
#include "sweet_wifi_firmware.h"
#include <errno.h>
#include <string.h>

#define FW(name) \
	extern const uint8_t sweet_wifi_fw_##name[]; \
	extern const uint8_t sweet_wifi_fw_##name##_end[]

FW(amss);
FW(board2);
FW(m3);
FW(regdb);

static const struct {
	const char *name;
	const uint8_t *start;
	const uint8_t *end;
} files[] = {
	{"amss.bin", sweet_wifi_fw_amss, sweet_wifi_fw_amss_end},
	{"board-2.bin", sweet_wifi_fw_board2, sweet_wifi_fw_board2_end},
	{"m3.bin", sweet_wifi_fw_m3, sweet_wifi_fw_m3_end},
	{"regdb.bin", sweet_wifi_fw_regdb, sweet_wifi_fw_regdb_end},
};

/* Run locally before handing these bytes to the transport. */
size_t
sweet_wifi_firmware_inventory(void)
{
	size_t total = 0;
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
		uintptr_t start = (uintptr_t)files[i].start;
		uintptr_t end = (uintptr_t)files[i].end;
		if (end <= start || end - start > SIZE_MAX - total) {
			return 0;
		}
		total += end - start;
	}
	return total;
}

int
sweet_wifi_firmware_get(const char *path, const uint8_t **data, size_t *size)
{
	const char *name;

	if (data) {
		*data = NULL;
	}
	if (size) {
		*size = 0;
	}
	if (!path || !data || !size) {
		return -EINVAL;
	}
	name = path;
	for (const char *p = path; *p; ++p) {
		if (*p == '/') {
			name = p + 1;
		}
	}
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
		if (!strcmp(name, files[i].name)) {
			uintptr_t start = (uintptr_t)files[i].start;
			uintptr_t end = (uintptr_t)files[i].end;
			if (end <= start) {
				return -EIO;
			}
			*data = files[i].start;
			*size = end - start;
			return 0;
		}
	}
	/* No firmware-2.bin: ath11k intentionally falls back to API 1.
	 * The board worker, not this table, supplies wlan_mac.bin. */
	return -ENOENT;
}
