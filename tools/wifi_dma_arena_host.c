/* Verify the exact DMA allocator in the relocatable bundle. */
#include "sweet_wifi_dma_arena.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) unsigned char memory[128u << 20];
static struct sweet_wifi_dma_arena arena;
static struct sweet_wifi_dma_extent extents[64];

static void
verify_extents(void)
{
	uint32_t next = 4096;
	for (unsigned i = 0; i < arena.count; ++i) {
		struct sweet_wifi_dma_extent e = arena.extents[i];
		assert(e.offset == next && e.bytes);
		assert(i == 0 || e.used || arena.extents[i - 1].used);
		next += e.bytes;
	}
	assert(next == sizeof(memory));
}

int
main(void)
{
	uint64_t first_iova, image_iova, small_iova, bad_iova;
	assert(sweet_wifi_dma_arena_init(&arena, memory + 1, extents, 64) == EINVAL);
	memset(memory, 0xff, 8192);
	assert(!sweet_wifi_dma_arena_init(&arena, memory, extents, 64));
	assert(memory[0] == 0 && memory[4095] == 0 && memory[4096] == 0xff);
	assert(!sweet_wifi_dma_arena_map(&arena, memory, 4));
	assert(!sweet_wifi_dma_arena_map(&arena, memory + 4096, 4));
	assert(sweet_wifi_dma_arena_free(&arena, memory, 0) == EINVAL);
	assert(!sweet_wifi_dma_arena_alloc(&arena, 0, &bad_iova) && !bad_iova);
	assert(!sweet_wifi_dma_arena_alloc(&arena, SIZE_MAX, &bad_iova) && !bad_iova);

	unsigned char *first = sweet_wifi_dma_arena_alloc(&arena, 65, &first_iova);
	assert(first == memory + 4096 && first_iova == 0xa0001000u);
	for (unsigned i = 0; i < 128; ++i) assert(first[i] == 0);
	/* The AMSS blob length rounds up past 4 MiB. BHI needs the next
	 * power-of-two address alignment, rather than merely a page. */
	unsigned char *image = sweet_wifi_dma_arena_alloc(&arena, 4988928, &image_iova);
	assert(image == memory + (8u << 20) && image_iova == 0xa0800000u);
	assert(sweet_wifi_dma_arena_map(&arena, image + 4096, 8192) == image_iova + 4096);
	unsigned char *small = sweet_wifi_dma_arena_alloc(&arena, 1, &small_iova);
	assert(small == first + 128 && small_iova == first_iova + 128);
	assert(sweet_wifi_dma_arena_map(&arena, first + 127, 1) == first_iova + 127);
	assert(!sweet_wifi_dma_arena_map(&arena, first + 127, 2));
	assert(!sweet_wifi_dma_arena_map(&arena, first + 1, SIZE_MAX));
	assert(!sweet_wifi_dma_arena_map(&arena, first, 0));
	assert(!sweet_wifi_dma_arena_map(&arena, memory + sizeof(memory), 1));
	verify_extents();

	assert(sweet_wifi_dma_arena_free(&arena, first + 1, 0) == EINVAL);
	assert(sweet_wifi_dma_arena_free(&arena, first, small_iova) == EINVAL);
	assert(!sweet_wifi_dma_arena_free(&arena, first, first_iova));
	assert(!sweet_wifi_dma_arena_map(&arena, first, 1));
	assert(sweet_wifi_dma_arena_free(&arena, first, first_iova) == EINVAL);
	assert(!sweet_wifi_dma_arena_free(&arena, image, image_iova));
	assert(!sweet_wifi_dma_arena_free(&arena, small, 0));
	assert(arena.count == 1);
	verify_extents();

	/* Refuse partial splits when metadata is full; do not lose free space. */
	assert(!sweet_wifi_dma_arena_init(&arena, memory, extents, 1));
	assert(!sweet_wifi_dma_arena_alloc(&arena, 64, &bad_iova) && !bad_iova);
	assert(arena.count == 1);
	verify_extents();

	/* Exercise repeated split/coalesce through reproducible fragmentation. */
	assert(!sweet_wifi_dma_arena_init(&arena, memory, extents, 64));
	void *slots[32] = {0};
	uint64_t bus[32];
	uint32_t rng = 1;
	for (unsigned i = 0; i < 1024; ++i) {
		rng = rng * 1664525u + 1013904223u;
		unsigned slot = (rng >> 16) & 31u;
		if (slots[slot]) {
			assert(!sweet_wifi_dma_arena_free(&arena, slots[slot], bus[slot]));
			slots[slot] = NULL;
		} else {
			size_t n = (rng & 32767u) + 1u;
			slots[slot] = sweet_wifi_dma_arena_alloc(&arena, n, &bus[slot]);
			if (slots[slot]) assert(sweet_wifi_dma_arena_map(&arena, slots[slot], n) == bus[slot]);
		}
		verify_extents();
	}
	for (unsigned i = 0; i < 32; ++i) {
		if (slots[i]) assert(!sweet_wifi_dma_arena_free(&arena, slots[i], bus[i]));
	}
	assert(arena.count == 1);
	verify_extents();
	puts("PASS: Wi-Fi DMA arena MSI reservation, BHI alignment, ownership and 1024 fragmentation iterations");
	return 0;
}
