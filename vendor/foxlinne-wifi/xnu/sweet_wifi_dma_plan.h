#ifndef SWEET_WIFI_DMA_PLAN_H
#define SWEET_WIFI_DMA_PLAN_H

#include <stdint.h>

#define SWEET_WIFI_DMA_SID 0x1c01u
#define SWEET_WIFI_DMA_IOVA 0xa0000000u
#define SWEET_WIFI_DMA_BLOCK 0x200000u
#define SWEET_WIFI_DMA_BLOCKS 64u
#define SWEET_WIFI_DMA_BYTES (SWEET_WIFI_DMA_BLOCK * SWEET_WIFI_DMA_BLOCKS)
#define SWEET_WIFI_DMA_TABLE_WORDS 512u

/* Pure planning over a snapshot: no reads of unknown context registers,
 * no permission changes, and no endpoint bus-master enable. */
struct sweet_wifi_smmu_group {
	uint32_t smr;
	uint32_t s2cr;
};

struct sweet_wifi_dma_route {
	unsigned slot;
	unsigned bank;
	uint32_t context_offset;
	uint32_t smr;
	uint32_t s2cr;
};

/* The caller supplies a bank it can actually access. An unused bank is
 * not necessarily writable from the Android bootloader's handoff state. */
int sweet_wifi_dma_plan_route(uint32_t id0, uint32_t id1, uint32_t scr0,
    const struct sweet_wifi_smmu_group *, unsigned group_count,
    unsigned bank, uint32_t sctlr, struct sweet_wifi_dma_route *);

/* Construct 4K-granule S1 tables for the fixed IOVA and 128 MiB arena.
 * The CPU and endpoint's cache attributes are a board-adapter decision. */
int sweet_wifi_dma_build_tables(uint64_t l1[SWEET_WIFI_DMA_TABLE_WORDS],
    uint64_t l2[SWEET_WIFI_DMA_TABLE_WORDS], uint64_t l1_pa, uint64_t l2_pa,
    uint64_t arena_pa, unsigned output_bits);

#endif
