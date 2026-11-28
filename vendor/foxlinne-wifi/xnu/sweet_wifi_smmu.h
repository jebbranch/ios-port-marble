/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWEET_WIFI_SMMU_H
#define SWEET_WIFI_SMMU_H

#include "sweet_wifi_dma_plan.h"

/* Register offsets are relative to apps-SMMU GR0. Read/write callbacks
 * return 0 on success and must provide ordered device accesses. The caller
 * must exclude all other stream/context writers for the entire call. */
struct sweet_wifi_smmu_io {
	void *arg;
	int (*read32)(void *, uint32_t, uint32_t *);
	int (*write32)(void *, uint32_t, uint32_t);
	int (*write64)(void *, uint32_t, uint64_t);
	int (*sync_tables)(void *);
	void (*delay_us)(void *, unsigned);
	/* Nonzero only when the owner can obtain a hardware witness. Not
	 * called when the handoff reports that non-secure ATS is unavailable. */
	int (*can_translate)(void *, uint32_t id0);
	/* Translation witness from the owner, not a software table walk.
	 * This must not generate traffic from the endpoint. Return 0 only
	 * with an observed PA (fault=0) or an observed translation fault. */
	int (*translate)(void *, unsigned, uint32_t, uint64_t *, int *);
	/* Persist this checkpoint before returning, without reentering the owner. */
	void (*checkpoint)(void *, unsigned, unsigned, unsigned);
};

struct sweet_wifi_smmu_grant {
	unsigned bank;
	/* Confirm exclusive, accessible ownership of the exact bank, its ASID
	 * (equal to bank), and the candidate stream slot. Not a boot argument. */
	int (*owns)(void *, unsigned, unsigned);
	/* Check the endpoint command register with memory decode on and BME off. */
	int (*endpoint_blocked)(void *);
	/* Explicit owner approval for a bounded DMA diagnostic on ATOSNS
	 * hardware. No effect when ATS is available but fails its checks. */
	int (*allows_dma_diagnostic)(void *);
	/* Optional ordered candidate list supplied by the platform owner
	 * (highest priority first). Never derive access rights from IDR1. */
	const unsigned *candidates;
	unsigned candidate_count;
};

enum sweet_wifi_smmu_step {
	SW_WIFI_SMMU_SNAPSHOT = 1,
	SW_WIFI_SMMU_CONTEXT_CHECK,
	SW_WIFI_SMMU_CONTEXT,
	SW_WIFI_SMMU_TRANSLATE,
	SW_WIFI_SMMU_ATS_UNAVAILABLE,
	SW_WIFI_SMMU_INSTALL,
	SW_WIFI_SMMU_CONFIGURED,
	SW_WIFI_SMMU_ABORT,
};

struct sweet_wifi_smmu_result {
	struct sweet_wifi_dma_route route;
	unsigned last_step;
	unsigned translations;
	int configured;
	int ats_verified;
	int retain_backing;
	int route_removed;
};

/* No bus-master enable. Inputs are exclusive, wired memory, not arbitrary
 * addresses. The caller must zero the whole device-visible arena and make
 * that zeroing visible to DMA before exposing it. After the first MMIO write
 * attempt, keep tables/backing pinned and the grant reserved for this boot
 * even if the route was removed. */
int sweet_wifi_smmu_commit(const struct sweet_wifi_smmu_io *,
    const struct sweet_wifi_smmu_grant *, uint64_t *, uint64_t *, uint64_t,
    uint64_t, uint64_t, struct sweet_wifi_smmu_result *);

#endif
