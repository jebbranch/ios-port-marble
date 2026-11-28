/* Offline tests for the exact software-only planner linked into the bundle. */
#include "sweet_wifi_dma_plan.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
test_routes(void)
{
	const uint32_t id0 = 0x4c017e71, id1 = 0x6000004b, scr0 = 0x200406;
	struct sweet_wifi_smmu_group groups[113];
	struct sweet_wifi_dma_route route;

	/* Representative existing translation and free faulting slots. */
	for (unsigned i = 0; i < 113; ++i) {
		groups[i] = (struct sweet_wifi_smmu_group){0, 0x20000};
	}
	groups[0] = (struct sweet_wifi_smmu_group){0x80000060u, 2};
	assert(!sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 0, &route));
	assert(route.slot == 1 && route.bank == 72);
	assert(route.context_offset == 0xc8000u);
	assert(route.smr == 0x80001c01u && route.s2cr == 72);
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 2, 0, &route) == EBUSY);
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 1, &route) == EBUSY);
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 75, 0, &route) == EINVAL);
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 114, 72, 0, &route) == EINVAL);
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0 & ~0x400u, groups, 113, 72, 0, &route) == EINVAL);
	assert(sweet_wifi_dma_plan_route(id0 | 0x100u, id1, scr0, groups, 113, 72, 0, &route) == EINVAL);

	/* A masked match (SID 0x1c00 with bit 0 ignored) still owns our SID. */
	groups[112] = (struct sweet_wifi_smmu_group){0x80011c00u, 0x20000};
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 0, &route) == EBUSY);
	groups[112].smr = 0x80001c01u;
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 0, &route) == EBUSY);
	groups[112].smr = UINT32_MAX;
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 0, &route) == EIO);

	/* Invalid does not mean free: skip a slot still configured for S1. */
	for (unsigned i = 1; i < 113; ++i) {
		groups[i] = (struct sweet_wifi_smmu_group){0, 0};
	}
	assert(sweet_wifi_dma_plan_route(id0, id1, scr0, groups, 113, 72, 0, &route) == ENOSPC);
}

static void
test_tables(void)
{
	const uint64_t l1_pa = UINT64_C(0x901000000);
	const uint64_t l2_pa = l1_pa + 4096;
	const uint64_t arena_pa = UINT64_C(0x900000000);
	const uint64_t disjoint_l1 = l1_pa + SWEET_WIFI_DMA_BYTES;
	const uint64_t disjoint_l2 = l2_pa + SWEET_WIFI_DMA_BYTES;
	uint64_t l1[512], l2[512];

	memset(l1, 0xa5, sizeof(l1));
	memset(l2, 0xa5, sizeof(l2));
	assert(sweet_wifi_dma_build_tables(l1, l2, l1_pa, l2_pa, arena_pa, 40) == EINVAL);
	assert(sweet_wifi_dma_build_tables(l1, l2, disjoint_l1, disjoint_l2, arena_pa, 32) == EINVAL);
	assert(sweet_wifi_dma_build_tables(l1, l2, disjoint_l1, disjoint_l2, arena_pa + 4096, 40) == EINVAL);
	assert(sweet_wifi_dma_build_tables(l1, l1, disjoint_l1, disjoint_l2, arena_pa, 40) == EINVAL);
	assert(sweet_wifi_dma_build_tables(l1, l2, disjoint_l1 + 1, disjoint_l2, arena_pa, 40) == EINVAL);
	assert(l1[2] == UINT64_C(0xa5a5a5a5a5a5a5a5));
	assert(!sweet_wifi_dma_build_tables(l1, l2, disjoint_l1, disjoint_l2, arena_pa, 40));
	for (unsigned i = 0; i < 512; ++i) {
		assert(l1[i] == (i == 2 ? (disjoint_l2 | 3u) : 0));
		assert(l2[i] == (i >= 256 && i < 320
		    ? ((arena_pa + (uint64_t)(i - 256) * 0x200000) | 0xf41u) : 0));
	}
}

int
main(void)
{
	test_routes();
	test_tables();
	puts("PASS: SID overlap and occupied-bank rejection; exact 128 MiB stage-1 map");
	return 0;
}
