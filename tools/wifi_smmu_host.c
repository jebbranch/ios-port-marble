/* Exercise the production transaction with fixed, independent register offsets. */
#include "sweet_wifi_smmu.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define BANK 72u
#define SLOT 1u
#define CONTEXT 0xc8000u
#define ARENA_PA UINT64_C(0x990000000)
#define L1_PA UINT64_C(0x998000000)
#define L2_PA (L1_PA + 4096)

enum failure {
	NONE, BUSY_ENDPOINT, NO_WITNESS, MISSING_ATS_HOOK, WRONG_OWNER, OWNED_SID,
	ACTIVE_CONTEXT, PAGE_SYNC_FAIL, CONTEXT_READBACK, TLB_TIMEOUT,
	NO_PAR, ZERO_PAR, WRONG_PA, BAD_GUARD, SLOT_CHANGED, ROUTE_WRITE_FAIL,
	ROUTE_CLEAR_FAIL, ATOSNS_DIAGNOSTIC, ATOSNS_WRONG_OWNER,
	OCCUPIED_FIRST, UNGRANTED_FIRST, ACTIVE_FIRST, UNREADABLE_FIRST,
	FIXED_READBACK, WRONG_PASIZE, FAULT_STALL
};

static uint32_t registers[0xcb000 / 4];
static _Alignas(4096) uint64_t l1[512], l2[512];
static struct model {
	enum failure failure;
	unsigned stage, writes, context_reads, polls, translates;
	unsigned syncs, slot_writes, guard, delays, ats_unavailable, first_reads;
} model;

static int
read32(void *arg, uint32_t off, uint32_t *value)
{
	struct model *m = arg;
	assert(!(off & 3) && off < sizeof(registers));
	if (off >= 0xca000u) {
		assert(m->stage == SW_WIFI_SMMU_CONTEXT_CHECK && off == 0xca000u);
		++m->first_reads;
		if (m->failure == UNREADABLE_FIRST) return EIO;
	}
	if (off >= CONTEXT) ++m->context_reads;
	if (off == 0xc87f4u) {
		++m->polls;
		*value = m->failure == TLB_TIMEOUT || m->polls == 1;
	} else {
		*value = registers[off / 4];
	}
	return 0;
}

static int
write32(void *arg, uint32_t off, uint32_t value)
{
	struct model *m = arg;
	assert(!(off & 3) && off < sizeof(registers));
	assert(m->syncs == 1);
	++m->writes;
	switch (off) {
	case 0x1920:
		assert(m->stage == SW_WIFI_SMMU_CONTEXT && value == 1);
		break;
	case 0x1120:
		assert(m->stage == SW_WIFI_SMMU_CONTEXT && value == 0x1f300);
		if (m->failure == CONTEXT_READBACK) return 0; /* silently dropped */
		break;
	case 0xc8010:
		assert(value == 0x38011);
		if (m->failure == FIXED_READBACK) value |= 0x60;
		if (m->failure == WRONG_PASIZE) value ^= 1;
		break;
	case 0xc8030: assert(value == 0x80802020); break;
	case 0xc8038: assert(value == 0xff); break;
	case 0xc803c: assert(value == 0); break;
	case 0xc8610: assert(value == BANK); break;
	case 0xc87f0: assert(value == 0); break;
	case 0xc8000:
		assert(value == 0x27 && m->polls == 2);
		if (m->failure == FIXED_READBACK) value |= 0xc00000;
		if (m->failure == FAULT_STALL) value |= 0x80;
		break;
	case 0xc04:
		assert(m->stage == SW_WIFI_SMMU_INSTALL && (m->guard || m->ats_unavailable));
		assert(registers[0x804 / 4] == 0);
		assert(value == BANK || value == 0x20000);
		++m->slot_writes;
		break;
	case 0x804:
		assert(m->stage == SW_WIFI_SMMU_INSTALL && (m->guard || m->ats_unavailable));
		assert(registers[0xc04 / 4] == BANK);
		assert(value == 0x80001c01 || value == 0);
		++m->slot_writes;
		if (!value && m->failure == ROUTE_CLEAR_FAIL) return EIO;
		break;
	default: assert(!"unexpected register write");
	}
	registers[off / 4] = value;
	/* A failed write callback does not prove the write had no effect. */
	if (off == 0x804 && value &&
	    (m->failure == ROUTE_WRITE_FAIL || m->failure == ROUTE_CLEAR_FAIL)) return EIO;
	return 0;
}

static int
write64(void *arg, uint32_t off, uint64_t value)
{
	struct model *m = arg;
	assert(m->stage == SW_WIFI_SMMU_CONTEXT);
	assert((off == 0xc8020 && value == (L1_PA | (UINT64_C(72) << 48))) ||
	    (off == 0xc8028 && value == 0));
	registers[off / 4] = (uint32_t)value;
	registers[off / 4 + 1] = (uint32_t)(value >> 32);
	++m->writes;
	return 0;
}

static int
sync_tables(void *arg)
{
	struct model *m = arg;
	assert(m->stage == SW_WIFI_SMMU_CONTEXT_CHECK && m->writes == 0);
	assert(l1[2] == (L2_PA | 3));
	assert(l2[256] == (ARENA_PA | 0xf41));
	assert(l2[319] == ((ARENA_PA + 63 * 0x200000) | 0xf41));
	assert(l2[320] == 0);
	++m->syncs;
	return m->failure == PAGE_SYNC_FAIL ? EIO : 0;
}

static void
delay_us(void *arg, unsigned us)
{
	struct model *m = arg;
	assert(us == 1);
	++m->delays;
}

static int
can_translate(void *arg, uint32_t id0)
{
	(void)arg;
	return !(id0 & 0x04000000u);
}

static int
translate(void *arg, unsigned bank, uint32_t iova, uint64_t *pa, int *fault)
{
	struct model *m = arg;
	assert(m->stage == SW_WIFI_SMMU_TRANSLATE && bank == BANK);
	assert(registers[CONTEXT / 4] ==
	    (m->failure == FIXED_READBACK ? 0xc00027u : 0x27u) && m->slot_writes == 0);
	uint32_t offset = m->translates / 2u * 0x200000u +
	    (m->translates % 2u ? 0x1ff000u : 0);
	assert(iova == 0xa0000000u + offset);
	++m->translates;
	if (m->failure == NO_PAR) return 0;
	if (m->failure == ZERO_PAR) {
		*pa = 0;
		*fault = 0;
		return 0;
	}
	*pa = ARENA_PA + offset + (m->failure == WRONG_PA ? 4096 : 0);
	*fault = iova == 0xa8000000u && m->failure != BAD_GUARD;
	m->guard = (unsigned)*fault;
	return 0;
}

static void
checkpoint(void *arg, unsigned step, unsigned bank, unsigned slot)
{
	struct model *m = arg;
	assert(bank == BANK || (bank == 74 &&
	    (step == SW_WIFI_SMMU_SNAPSHOT || step == SW_WIFI_SMMU_CONTEXT_CHECK)));
	assert(slot == (step == SW_WIFI_SMMU_SNAPSHOT ? 128u : SLOT));
	assert(step > m->stage || (step == m->stage && step == SW_WIFI_SMMU_CONTEXT_CHECK));
	assert(step < SW_WIFI_SMMU_CONFIGURED || m->stage >= SW_WIFI_SMMU_CONTEXT);
	m->stage = step;
	if (step == SW_WIFI_SMMU_ATS_UNAVAILABLE) ++m->ats_unavailable;
	if (step == SW_WIFI_SMMU_INSTALL && m->failure == SLOT_CHANGED)
		registers[0x804 / 4] = 0x80000061u;
}

static int
owns(void *arg, unsigned bank, unsigned slot)
{
	struct model *m = arg;
	assert((bank == BANK || bank == 74) && slot == SLOT);
	assert(m->context_reads == m->first_reads && m->writes == 0);
	if (bank == 74) {
		assert(m->failure >= UNGRANTED_FIRST);
		return m->failure != UNGRANTED_FIRST;
	}
	return m->failure != WRONG_OWNER && m->failure != ATOSNS_WRONG_OWNER;
}

static int
endpoint_blocked(void *arg)
{
	struct model *m = arg;
	return m->failure != BUSY_ENDPOINT;
}

static int
allows_dma_diagnostic(void *arg)
{
	struct model *m = arg;
	/* Also allow in failing ATS tests to prove they cannot fall back. */
	return m->failure != NO_WITNESS;
}

static void
test(enum failure failure, int expected, unsigned retain, unsigned translations)
{
	memset(registers, 0, sizeof(registers));
	model = (struct model){.failure = failure};
	int atosns = failure == NO_WITNESS || failure == ATOSNS_DIAGNOSTIC ||
	    failure == ATOSNS_WRONG_OWNER;
	registers[0x20 / 4] = atosns ? 0x4c017e71u : 0x48017e71u;
	registers[0x24 / 4] = 0x6000004bu;
	registers[0x28 / 4] = 0x5111u; /* observed Marble IDR2, 36-bit output */
	registers[0] = 0x200406u;
	registers[0x800 / 4] = 0x80000060u;
	registers[0xc00 / 4] = 2u;
	registers[0xc04 / 4] = 0x20000u;
	if (failure == OWNED_SID) registers[0x9c0 / 4] = 0x80011c00u;
	if (failure == ACTIVE_CONTEXT) registers[CONTEXT / 4] = 1;
	if (failure == OCCUPIED_FIRST) {
		registers[0x808 / 4] = 0x80000061u;
		registers[0xc08 / 4] = 74;
	}
	if (failure == ACTIVE_FIRST) registers[0xca000 / 4] = 1;
	static const unsigned candidates[] = {74, BANK};
	int multiple = failure >= OCCUPIED_FIRST && failure <= UNREADABLE_FIRST;
	const struct sweet_wifi_smmu_io io = {
		.arg = &model, .read32 = read32, .write32 = write32, .write64 = write64,
		.sync_tables = sync_tables, .delay_us = delay_us,
		.can_translate = can_translate,
		.translate = failure == MISSING_ATS_HOOK || atosns ? NULL : translate,
		.checkpoint = checkpoint,
	};
	const struct sweet_wifi_smmu_grant grant = {
		.bank = BANK, .owns = owns, .endpoint_blocked = endpoint_blocked,
		.allows_dma_diagnostic = allows_dma_diagnostic,
		.candidates = multiple ? candidates : NULL,
		.candidate_count = multiple ? 2 : 0,
	};
	struct sweet_wifi_smmu_result result;
	int rc = sweet_wifi_smmu_commit(&io, &grant, l1, l2, L1_PA, L2_PA, ARENA_PA, &result);
	assert(rc == expected && result.configured == (expected == 0));
	assert(result.retain_backing == (int)retain && result.translations == translations);
	assert(result.ats_verified == (translations == 129));
	assert(model.ats_unavailable == (failure == ATOSNS_DIAGNOSTIC));
	assert(result.route_removed == (failure == ROUTE_WRITE_FAIL));
	assert(model.first_reads == (failure == ACTIVE_FIRST || failure == UNREADABLE_FIRST));
	assert(result.last_step <= model.stage);
	if (!retain) assert(model.writes == 0);
	if ((failure >= BUSY_ENDPOINT && failure <= OWNED_SID) ||
	    failure == ATOSNS_WRONG_OWNER)
		assert(model.context_reads == 0);
	if (failure == TLB_TIMEOUT) assert(model.delays == 10000 && model.polls == 10001);
	if (!expected || failure == ROUTE_CLEAR_FAIL) {
		assert(registers[0x804 / 4] == 0x80001c01 && registers[0xc04 / 4] == BANK);
	} else {
		assert(registers[0x804 / 4] == (failure == SLOT_CHANGED ? 0x80000061u : 0));
		assert(registers[0xc04 / 4] == 0x20000);
	}
	assert(registers[0x800 / 4] == 0x80000060u && registers[0xc00 / 4] == 2);
}

int
main(void)
{
	test(NONE, 0, 1, 129);
	test(BUSY_ENDPOINT, EBUSY, 0, 0);
	test(NO_WITNESS, ENOTSUP, 0, 0);
	test(MISSING_ATS_HOOK, ENOTSUP, 0, 0);
	test(WRONG_OWNER, EACCES, 0, 0);
	test(OWNED_SID, EBUSY, 0, 0);
	test(ACTIVE_CONTEXT, EBUSY, 0, 0);
	test(PAGE_SYNC_FAIL, EIO, 0, 0);
	test(CONTEXT_READBACK, EIO, 1, 0);
	test(TLB_TIMEOUT, ETIMEDOUT, 1, 0);
	test(NO_PAR, EIO, 1, 0);
	test(ZERO_PAR, EIO, 1, 0);
	test(WRONG_PA, EIO, 1, 0);
	test(BAD_GUARD, EIO, 1, 128);
	test(SLOT_CHANGED, EIO, 1, 129);
	test(ROUTE_WRITE_FAIL, EIO, 1, 129);
	test(ROUTE_CLEAR_FAIL, EIO, 1, 129);
	test(ATOSNS_DIAGNOSTIC, 0, 1, 0);
	test(ATOSNS_WRONG_OWNER, EACCES, 0, 0);
	test(OCCUPIED_FIRST, 0, 1, 129);
	test(UNGRANTED_FIRST, 0, 1, 129);
	test(ACTIVE_FIRST, 0, 1, 129);
	test(UNREADABLE_FIRST, EIO, 0, 0);
	test(FIXED_READBACK, 0, 1, 129);
	test(WRONG_PASIZE, EIO, 1, 0);
	test(FAULT_STALL, EIO, 1, 0);
	puts("PASS: 26 SMMU scenarios, including bank selection and fixed-bit readback");
	return 0;
}
