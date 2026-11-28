/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "sweet_wifi_dma_arena.h"
#include "sweet_wifi_dma_plan.h"
#include <errno.h>
#include <string.h>

#define ARENA_MSI_BYTES 4096u
#define ARENA_EXTENTS_MAX 16384u

int
sweet_wifi_dma_arena_init(struct sweet_wifi_dma_arena *arena, void *base,
    struct sweet_wifi_dma_extent *extents, unsigned capacity)
{
	if (!arena || !base || ((uintptr_t)base & (ARENA_MSI_BYTES - 1u)) ||
	    !extents || !capacity || capacity > ARENA_EXTENTS_MAX ||
	    (uintptr_t)base > UINTPTR_MAX - SWEET_WIFI_DMA_BYTES) return EINVAL;
	arena->base = base;
	arena->extents = extents;
	arena->capacity = capacity;
	arena->count = 1;
	extents[0] = (struct sweet_wifi_dma_extent){
		ARENA_MSI_BYTES, SWEET_WIFI_DMA_BYTES - ARENA_MSI_BYTES, 0
	};
	memset(base, 0, ARENA_MSI_BYTES);
	return 0;
}

void *
sweet_wifi_dma_arena_alloc(struct sweet_wifi_dma_arena *arena, size_t bytes,
    uint64_t *iova)
{
	if (iova) *iova = 0;
	if (!arena || !arena->count || !bytes ||
	    bytes > SWEET_WIFI_DMA_BYTES - ARENA_MSI_BYTES) return NULL;
	/* Match the reference BHI buffers' size-derived IOVA alignment.
	 * Round before computing it (including requests near a page boundary). */
	uint32_t rounded = ((uint32_t)bytes + 63u) & ~63u;
	uint32_t alignment = 64u;
	if (rounded >= 4096u) {
		rounded = (rounded + 4095u) & ~4095u;
		alignment = 4096u;
		while (alignment < rounded) alignment <<= 1;
	}
	struct sweet_wifi_dma_extent *extents = arena->extents;
	for (unsigned i = 0; i < arena->count; ++i) {
		struct sweet_wifi_dma_extent entry = extents[i];
		if (entry.used) continue;
		uint32_t start = (entry.offset + alignment - 1u) & ~(alignment - 1u);
		uint32_t pad = start - entry.offset;
		if (pad > entry.bytes || rounded > entry.bytes - pad) continue;
		uint32_t tail = entry.bytes - pad - rounded;
		unsigned extra = (pad != 0) + (tail != 0);
		if (extra > arena->capacity - arena->count) continue;
		if (extra) {
			memmove(extents + i + 1u + extra, extents + i + 1u,
			    (arena->count - i - 1u) * sizeof(*extents));
			arena->count += extra;
		}
		unsigned at = i;
		if (pad) extents[at++] = (struct sweet_wifi_dma_extent){entry.offset, pad, 0};
		extents[at++] = (struct sweet_wifi_dma_extent){start, rounded, 1};
		if (tail) extents[at] = (struct sweet_wifi_dma_extent){start + rounded, tail, 0};
		void *p = arena->base + start;
		memset(p, 0, rounded);
		if (iova) *iova = (uint64_t)SWEET_WIFI_DMA_IOVA + start;
		return p;
	}
	return NULL;
}

static int
arena_offset(const struct sweet_wifi_dma_arena *arena, const void *p,
    uint32_t *offset)
{
	if (!arena || !arena->count || !p) return 0;
	uintptr_t address = (uintptr_t)p, base = (uintptr_t)arena->base;
	if (address < base || address - base < ARENA_MSI_BYTES ||
	    address - base >= SWEET_WIFI_DMA_BYTES) return 0;
	*offset = (uint32_t)(address - base);
	return 1;
}

uint64_t
sweet_wifi_dma_arena_map(const struct sweet_wifi_dma_arena *arena,
    const void *p, size_t bytes)
{
	uint32_t offset;
	if (!bytes || !arena_offset(arena, p, &offset)) return 0;
	for (unsigned i = 0; i < arena->count; ++i) {
		struct sweet_wifi_dma_extent e = arena->extents[i];
		if (e.used && offset >= e.offset && offset - e.offset < e.bytes &&
		    bytes <= e.bytes - (offset - e.offset)) {
			return (uint64_t)SWEET_WIFI_DMA_IOVA + offset;
		}
	}
	return 0;
}

int
sweet_wifi_dma_arena_free(struct sweet_wifi_dma_arena *arena, void *p,
    uint64_t iova)
{
	uint32_t offset;
	if (!arena_offset(arena, p, &offset) ||
	    (iova && iova != (uint64_t)SWEET_WIFI_DMA_IOVA + offset)) return EINVAL;
	struct sweet_wifi_dma_extent *extents = arena->extents;
	for (unsigned i = 0; i < arena->count; ++i) {
		if (extents[i].offset != offset || !extents[i].used) continue;
		extents[i].used = 0;
		if (i + 1u < arena->count && !extents[i + 1u].used) {
			extents[i].bytes += extents[i + 1u].bytes;
			memmove(extents + i + 1u, extents + i + 2u,
			    (arena->count - i - 2u) * sizeof(*extents));
			--arena->count;
		}
		if (i && !extents[i - 1u].used) {
			extents[i - 1u].bytes += extents[i].bytes;
			memmove(extents + i, extents + i + 1u,
			    (arena->count - i - 1u) * sizeof(*extents));
			--arena->count;
		}
		return 0;
	}
	return EINVAL;
}
