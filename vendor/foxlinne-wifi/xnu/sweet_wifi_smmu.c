/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "sweet_wifi_smmu.h"
#include <errno.h>
#include <stddef.h>

#define ID0      0x020u
#define ID1      0x024u
#define ID2      0x028u
#define SCR0     0x000u
#define SMR(n)   (0x800u + 4u * (n))
#define S2CR(n)  (0xc00u + 4u * (n))
#define CBAR(n)  (0x1000u + 4u * (n))
#define CBA2R(n) (0x1800u + 4u * (n))

static int
matches(const struct sweet_wifi_smmu_io *io, uint32_t off, uint32_t want)
{
	uint32_t got;
	return !io->read32(io->arg, off, &got) && got == want;
}

static void
checkpoint(const struct sweet_wifi_smmu_io *io,
    struct sweet_wifi_smmu_result *result, unsigned step)
{
	result->last_step = step;
	io->checkpoint(io->arg, step, result->route.bank, result->route.slot);
}

static int
set32_fixed(const struct sweet_wifi_smmu_io *io, uint32_t off, uint32_t value,
    uint32_t fixed)
{
	uint32_t got;
	return !io->write32(io->arg, off, value) &&
	    !io->read32(io->arg, off, &got) &&
	    (got == value || got == (value | fixed));
}

static int
set32(const struct sweet_wifi_smmu_io *io, uint32_t off, uint32_t value)
{
	return set32_fixed(io, off, value, 0);
}

static int
set64(const struct sweet_wifi_smmu_io *io, uint32_t off, uint64_t value)
{
	return !io->write64(io->arg, off, value) &&
	    matches(io, off, (uint32_t)value) &&
	    matches(io, off + 4u, (uint32_t)(value >> 32));
}

int
sweet_wifi_smmu_commit(const struct sweet_wifi_smmu_io *io,
    const struct sweet_wifi_smmu_grant *grant, uint64_t *l1, uint64_t *l2,
    uint64_t l1_pa, uint64_t l2_pa, uint64_t arena_pa,
    struct sweet_wifi_smmu_result *result)
{
	static const unsigned bits[] = {32, 36, 40, 42, 44, 48};
	struct sweet_wifi_smmu_group groups[128];
	uint32_t id0, id1, id2, scr0, cb, status;
	unsigned oas, count, slot, bank, candidate_count;
	int error, use_ats, route_touched = 0;

	if (!result) return EINVAL;
	*result = (struct sweet_wifi_smmu_result){0};
	if (!io || !grant || !io->read32 || !io->write32 || !io->write64 ||
	    !io->sync_tables || !io->delay_us || !io->checkpoint ||
	    !grant->owns || !grant->endpoint_blocked) return EINVAL;
	if ((grant->candidates == NULL) != (grant->candidate_count == 0) ||
	    grant->candidate_count > 8) return EINVAL;
	candidate_count = grant->candidate_count ? grant->candidate_count : 1;
	bank = grant->candidates ? grant->candidates[0] : grant->bank;
	result->route.bank = bank;
	result->route.slot = 128;
	checkpoint(io, result, SW_WIFI_SMMU_SNAPSHOT);
	if (!grant->endpoint_blocked(io->arg)) return EBUSY;
	if (io->read32(io->arg, ID0, &id0) || io->read32(io->arg, ID1, &id1) ||
	    io->read32(io->arg, ID2, &id2) || io->read32(io->arg, SCR0, &scr0)) {
		return EIO;
	}
	if (id0 == UINT32_MAX || id1 == UINT32_MAX || id2 == UINT32_MAX ||
	    scr0 == UINT32_MAX) return EIO;
	/* Only absence of the non-secure ATS capability permits skipping
	 * ATS. An available but broken witness is a hard failure. */
	use_ats = !(id0 & (1u << 26));
	if (use_ats ? (!io->can_translate || !io->translate ||
	    !io->can_translate(io->arg, id0)) :
	    (!grant->allows_dma_diagnostic || !grant->allows_dma_diagnostic(io->arg))) {
		return ENOTSUP;
	}
	count = id0 & 255u;
	oas = (id2 >> 4) & 15u;
	if (!count || count > 128 || !(id2 & (1u << 12)) ||
	    oas >= sizeof(bits) / sizeof(bits[0])) return ENOTSUP;
	for (unsigned i = 0; i < count; ++i) {
		if (io->read32(io->arg, SMR(i), &groups[i].smr) ||
		    io->read32(io->arg, S2CR(i), &groups[i].s2cr)) return EIO;
	}
	/* Like the reference UFS owner, skip occupied banks before trying
	 * another candidate. Unlike generic probing, only touch context pages
	 * expressly covered by the owner. A failed read is not "occupied". */
	for (unsigned i = 0; ; ++i) {
		bank = grant->candidates ? grant->candidates[i] : grant->bank;
		error = sweet_wifi_dma_plan_route(id0, id1, scr0, groups, count,
		    bank, 0, &result->route);
		if (!error) {
			slot = result->route.slot;
			cb = result->route.context_offset;
			if (!grant->owns(io->arg, bank, slot)) {
				error = EACCES;
			} else {
				checkpoint(io, result, SW_WIFI_SMMU_CONTEXT_CHECK);
				if (io->read32(io->arg, cb, &status) ||
				    status == UINT32_MAX) return EIO;
				if (status == 0) break;
				error = EBUSY;
			}
		}
		if (i + 1 == candidate_count || (error != EBUSY && error != EACCES))
			return error;
	}
	if (((uintptr_t)l1 | (uintptr_t)l2) & 4095u) return EINVAL;
	error = sweet_wifi_dma_build_tables(l1, l2, l1_pa, l2_pa, arena_pa,
	    bits[oas]);
	if (error) return error;
	if (io->sync_tables(io->arg)) return EIO;
	checkpoint(io, result, SW_WIFI_SMMU_CONTEXT);
	if (!matches(io, cb, 0) || !matches(io, SMR(slot), groups[slot].smr) ||
	    !matches(io, S2CR(slot), groups[slot].s2cr) ||
	    !grant->endpoint_blocked(io->arg)) return EBUSY;

	result->retain_backing = 1;
	error = EIO;
	if (!set32(io, CBA2R(bank), 1u) ||
	    !set32(io, CBAR(bank), 0x1f300u) ||
	    /* The reference Marble reads bits [6:5] as ones. Still check
	     * the requested PASIZE, ASID width, and SEP. */
	    !set32_fixed(io, cb + 0x10u, (7u << 15) | (1u << 4) | oas, 0x60u) ||
	    !set64(io, cb + 0x20u, l1_pa | ((uint64_t)bank << 48)) ||
	    !set64(io, cb + 0x28u, 0) ||
	    !set32(io, cb + 0x30u, (2u << 30) | (2u << 12) | (1u << 23) | 32u) ||
	    !set32(io, cb + 0x38u, 0xffu) || !set32(io, cb + 0x3cu, 0) ||
	    io->write32(io->arg, cb + 0x610u, bank) ||
	    io->write32(io->arg, cb + 0x7f0u, 0)) goto abort;
	for (unsigned us = 0;; ++us) {
		if (io->read32(io->arg, cb + 0x7f4u, &status) ||
		    status == UINT32_MAX) goto abort;
		if (!(status & 1u)) break;
		if (us == 10000u) {
			error = ETIMEDOUT;
			goto abort;
		}
		io->delay_us(io->arg, 1);
	}
	/* No fault IRQ or stall. ASIDPNE did not stick in local runs; leave
	 * it clear. CB74 may report the local NSH override in bits [23:22]. */
	if (!set32_fixed(io, cb, 0x27u, 0xc00000u)) goto abort;

	if (use_ats) {
		checkpoint(io, result, SW_WIFI_SMMU_TRANSLATE);
		for (unsigned i = 0; i <= SWEET_WIFI_DMA_BLOCKS * 2; ++i) {
			uint32_t offset = (i / 2u) * SWEET_WIFI_DMA_BLOCK +
			    ((i & 1u) ? SWEET_WIFI_DMA_BLOCK - 4096u : 0);
			int guard = offset == SWEET_WIFI_DMA_BYTES;
			uint64_t pa = 0;
			int fault = -1;
			if (io->translate(io->arg, bank, SWEET_WIFI_DMA_IOVA + offset,
			    &pa, &fault) || fault != guard ||
			    (!guard && pa != arena_pa + offset)) goto abort;
			++result->translations;
		}
		result->ats_verified = 1;
	} else {
		checkpoint(io, result, SW_WIFI_SMMU_ATS_UNAVAILABLE);
	}

	checkpoint(io, result, SW_WIFI_SMMU_INSTALL);
	if (!grant->endpoint_blocked(io->arg) ||
	    !matches(io, SMR(slot), groups[slot].smr) ||
	    !matches(io, S2CR(slot), groups[slot].s2cr)) goto abort;
	route_touched = 1;
	/* Set the context before making the stream match valid. */
	if (!set32(io, S2CR(slot), result->route.s2cr) ||
	    !set32(io, SMR(slot), result->route.smr) ||
	    !grant->endpoint_blocked(io->arg)) goto abort;
	checkpoint(io, result, SW_WIFI_SMMU_CONFIGURED);
	result->configured = 1;
	return 0;

abort:
	/* A write can fail after reaching the device. Clear/verify match first.
	 * Do not disable/reuse this context, or free memory, on an uncertain route. */
	if (route_touched && set32(io, SMR(slot), groups[slot].smr) &&
	    set32(io, S2CR(slot), groups[slot].s2cr)) {
		result->route_removed = 1;
	}
	io->checkpoint(io->arg, SW_WIFI_SMMU_ABORT, bank, slot);
	return error;
}
