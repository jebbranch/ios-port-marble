/* Test the actual runtime witness against independently placed BAR registers.
 * The runtime entry is deliberately never invoked in this hosted test. */
#define sweet_wifi_runtime_start wifi_runtime_not_called
#include "sweet_wifi_runtime.c"
#undef sweet_wifi_runtime_start

#include <assert.h>

static uint32_t bar[0x1000 / 4];
static _Alignas(4096) unsigned char arena_memory[8192];
static struct sweet_wifi_dma_extent extents[4];

int
main(void)
{
	wifi.bar = (uintptr_t)bar;
	wifi.bar_bytes = sizeof(bar);
	wifi.wc = (uintptr_t)arena_memory;
	assert(!sweet_wifi_dma_arena_init(&wifi.arena, arena_memory, extents, 4));
	uint64_t iova;
	assert(sweet_wifi_dma_arena_alloc(&wifi.arena, 4096, &iova) == arena_memory + 4096);
	assert(iova == 0xa0001000u);

	bar[0x28 / 4] = 0x100;
	bar[0x2c / 4] = 0x200;
	bar[0x108 / 4] = (uint32_t)iova;
	bar[0x110 / 4] = 4096;
	bar[0x118 / 4] = 7;
	bar[0x12c / 4] = 0x80000000u;
	wifi.bhi_initial = 7;
	observe_transfer();
	assert(!wifi.transfer_seen);
	wifi.fw = 1;
	observe_transfer();
	assert(!wifi.transfer_seen); /* stale doorbell */
	bar[0x118 / 4] = 8;
	bar[0x108 / 4] = 0xa0000000u;
	observe_transfer();
	assert(!wifi.transfer_seen); /* MSI page is not a firmware buffer */
	bar[0x108 / 4] = (uint32_t)iova;
	bar[0x110 / 4] = 4097;
	observe_transfer();
	assert(!wifi.transfer_seen); /* end falls outside the live allocation */
	bar[0x110 / 4] = 4096;
	bar[0x12c / 4] = 0xc0000000u;
	observe_transfer();
	assert(!wifi.transfer_seen);
	bar[0x12c / 4] = 0x80000000u;
	observe_transfer();
	assert(wifi.transfer_seen);

	wifi.transfer_seen = 0;
	bar[0x28 / 4] = 0xfffffffcu;
	bar[0x22c / 4] = (uint32_t)iova;
	bar[0x234 / 4] = 4096;
	bar[0x23c / 4] = 4;
	bar[0x244 / 4] = 0x80000003u;
	observe_transfer();
	assert(!wifi.transfer_seen); /* mismatched BHIe sequence */
	bar[0x244 / 4] = 0x80000004u;
	observe_transfer();
	assert(wifi.transfer_seen);
	puts("PASS: firmware DMA witness requires a fresh in-arena BHI/BHIe completion");
	return 0;
}
