/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWEET_WIFI_DMA_ARENA_H
#define SWEET_WIFI_DMA_ARENA_H

#include <stddef.h>
#include <stdint.h>

struct sweet_wifi_dma_extent {
	uint32_t offset;
	uint32_t bytes;
	int used;
};

/* Calls must be serialized by the runtime owner. Memory must be wired,
 * exclusively CPU-accessed through a DMA-safe mapping, and contiguous in
 * IOVA starting at SWEET_WIFI_DMA_IOVA. First 4 KiB is reserved for MSI. */
struct sweet_wifi_dma_arena {
	unsigned char *base;
	struct sweet_wifi_dma_extent *extents;
	unsigned count, capacity;
};

int sweet_wifi_dma_arena_init(struct sweet_wifi_dma_arena *, void *,
    struct sweet_wifi_dma_extent *, unsigned);
void *sweet_wifi_dma_arena_alloc(struct sweet_wifi_dma_arena *, size_t,
    uint64_t *);
/* Free accepts only the base of a live allocation. A nonzero supplied DMA
 * address must match it. Call only after the endpoint has released it. */
int sweet_wifi_dma_arena_free(struct sweet_wifi_dma_arena *, void *, uint64_t);
uint64_t sweet_wifi_dma_arena_map(const struct sweet_wifi_dma_arena *,
    const void *, size_t);

#endif
