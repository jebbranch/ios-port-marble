/* Host test of the same provider and blobs that will be linked into XNU. */
#include "sweet_wifi_firmware.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int
main(void)
{
	static const struct {
		const char *path;
		size_t size;
	} expected[] = {
		{"ath11k/WCN6855/hw2.1/amss.bin", 4988928},
		{"ath11k/WCN6855/hw2.1/board-2.bin", 7237392},
		{"m3.bin", 266684},
		{"regdb.bin", 24310},
	};
	const uint8_t *bytes = NULL;
	size_t size = 0, total = 0;
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
		assert(sweet_wifi_firmware_get(expected[i].path, &bytes, &size) == 0);
		assert(bytes && size == expected[i].size);
		const uint8_t *again = NULL;
		size_t again_size = 0;
		assert(sweet_wifi_firmware_get(expected[i].path, &again, &again_size) == 0);
		assert(again == bytes && again_size == size);
		total += size;
	}
	assert(sweet_wifi_firmware_inventory() == total);
	assert(sweet_wifi_firmware_get("firmware-2.bin", &bytes, &size) == -ENOENT);
	assert(bytes == NULL && size == 0);
	assert(sweet_wifi_firmware_get(NULL, &bytes, &size) == -EINVAL);
	assert(sweet_wifi_firmware_get("m3.bin", NULL, &size) == -EINVAL);
	assert(sweet_wifi_firmware_get("m3.bin", &bytes, NULL) == -EINVAL);
	puts("PASS: four real firmware blobs, lengths, borrowed storage and API-1 fallback");
	return 0;
}
