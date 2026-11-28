/* Marble routing and DMA geometry follow FoXLinne/UniXNU_marble.
 * This implementation only prepares software state; hardware ownership
 * must be established separately before committing a route. */
#include "sweet_wifi_dma_plan.h"
#include <errno.h>
#include <string.h>

int
sweet_wifi_dma_plan_route(uint32_t id0, uint32_t id1, uint32_t scr0,
    const struct sweet_wifi_smmu_group *groups, unsigned count,
    unsigned bank, uint32_t sctlr, struct sweet_wifi_dma_route *route)
{
	unsigned cb_count = id1 & 255u;
	unsigned stage2_count = (id1 >> 16) & 255u;
	unsigned pages = 2u << ((id1 >> 28) & 7u);
	unsigned sid_bits = (id0 >> 9) & 15u;
	unsigned slot = count;

	if (!groups || !route || id0 == UINT32_MAX || id1 == UINT32_MAX ||
	    scr0 == UINT32_MAX || !(id0 & (1u << 30)) || !(id0 & (1u << 27)) ||
	    (id0 & (1u << 8)) || (id1 & (1u << 31)) ||
	    (scr0 & 0x409u) != 0x400u || sid_bits < 13u ||
	    !count || count > 128u || count != (id0 & 255u) ||
	    cb_count > 128u || bank >= cb_count || bank < stage2_count) {
		return EINVAL;
	}
	if (sctlr == UINT32_MAX || (sctlr & 1u)) {
		return EBUSY;
	}

	uint32_t sid_mask = (1u << sid_bits) - 1u;
	for (unsigned i = 0; i < count; ++i) {
		uint32_t smr = groups[i].smr;
		uint32_t s2cr = groups[i].s2cr;
		if (smr == UINT32_MAX || s2cr == UINT32_MAX) {
			return EIO;
		}
		if (!(smr & (1u << 31))) {
			/* Never repurpose an invalid group whose translation type
			 * was left configured by a different driver. */
			if (s2cr == (2u << 16) && slot == count) {
				slot = i;
			}
			continue;
		}
		uint32_t mask = (smr >> 16) & 0x7fffu;
		if (!((smr ^ SWEET_WIFI_DMA_SID) & ~mask & sid_mask) ||
		    (!(s2cr & (3u << 16)) && (s2cr & 255u) == bank)) {
			return EBUSY;
		}
	}
	if (slot == count) {
		return ENOSPC;
	}
	*route = (struct sweet_wifi_dma_route){
		.slot = slot,
		.bank = bank,
		.context_offset = (pages + bank) * 4096u,
		.smr = (1u << 31) | SWEET_WIFI_DMA_SID,
		.s2cr = bank,
	};
	return 0;
}

static int
fits(uint64_t start, uint64_t size, uint64_t limit)
{
	return start && start < limit && size <= limit - start;
}

int
sweet_wifi_dma_build_tables(uint64_t l1[SWEET_WIFI_DMA_TABLE_WORDS],
    uint64_t l2[SWEET_WIFI_DMA_TABLE_WORDS], uint64_t l1_pa, uint64_t l2_pa,
    uint64_t arena_pa, unsigned output_bits)
{
	if (!l1 || !l2 || l1 == l2 || output_bits < 32 || output_bits > 48) {
		return EINVAL;
	}
	uint64_t limit = UINT64_C(1) << output_bits;
	if ((l1_pa & 4095u) || (l2_pa & 4095u) || l1_pa == l2_pa ||
	    (arena_pa & (SWEET_WIFI_DMA_BLOCK - 1u)) ||
	    !fits(l1_pa, 4096u, limit) || !fits(l2_pa, 4096u, limit) ||
	    !fits(arena_pa, SWEET_WIFI_DMA_BYTES, limit)) {
		return EINVAL;
	}
	uint64_t arena_end = arena_pa + SWEET_WIFI_DMA_BYTES;
	if ((l1_pa >= arena_pa && l1_pa < arena_end) ||
	    (l2_pa >= arena_pa && l2_pa < arena_end)) {
		return EINVAL;
	}
	memset(l1, 0, SWEET_WIFI_DMA_TABLE_WORDS * sizeof(*l1));
	memset(l2, 0, SWEET_WIFI_DMA_TABLE_WORDS * sizeof(*l2));
	l1[SWEET_WIFI_DMA_IOVA >> 30] = l2_pa | 3u;
	unsigned first = (SWEET_WIFI_DMA_IOVA >> 21) & 511u;
	/* Stage 1 block, AF, nG, inner-shareable, EL0 read/write, AttrIdx 0. */
	const uint64_t attrs = (1u << 11) | (1u << 10) | (3u << 8) | (1u << 6) | 1u;
	for (unsigned i = 0; i < SWEET_WIFI_DMA_BLOCKS; ++i) {
		l2[first + i] = (arena_pa + (uint64_t)i * SWEET_WIFI_DMA_BLOCK) | attrs;
	}
	return 0;
}
