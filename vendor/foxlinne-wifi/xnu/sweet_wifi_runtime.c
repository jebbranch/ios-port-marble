/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Local SWEET diagnostic adapter for the FoXLinne ath11k board ABI. */
#include "nw_abi.h"
#include "sweet_wifi_dma_arena.h"
#include "sweet_wifi_firmware.h"
#include "sweet_wifi_smmu.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* XNU 8796 machine_routines, IOLib, pmap, clock and SWEETLOG interfaces.
 * Keep XNU's headers separate from the imported Linux compatibility headers. */
extern uintptr_t ml_io_map(uintptr_t, size_t);
extern uintptr_t ml_io_map_unmappable(uintptr_t, size_t, unsigned);
extern void ml_io_unmap(uintptr_t, size_t);
extern uint64_t kvtophys(uintptr_t);
extern void flush_dcache64(uint64_t, unsigned, int);
extern void *IOMallocContiguous(size_t, size_t, uintptr_t *);
extern void IOFreeContiguous(void *, size_t);
extern void *IOMallocData(size_t);
extern void IOFreeData(void *, size_t);
extern void IOSleep(unsigned);
extern void IODelay(unsigned);
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
extern void _disable_preemption(void);
extern void _enable_preemption(void);
extern void sweet_log_append(const char *, int);
extern void sweet_log_sync(void);
extern void panic(const char *, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define SMMU_PA 0x15000000u
#define SMMU_GR_BYTES 0x4000u
#define SMMU_CB_BASE 0x80000u
/* CB74 had repeated CBAR/CBA2R and SCTLR readback on our own RAM boot.
 * Other FoXLinne banks are not evidence of access on our handoff. */
#define SMMU_LOCAL_BANK 74u
#define SMMU_LOCAL_CB (SMMU_CB_BASE + 4096u * SMMU_LOCAL_BANK)
#define XNU_PAGE 0x4000u
#define XNU_VM_WIMG_WCOMB 0x6u
#define WIFI_MSI_LO 0x820u
#define WIFI_MSI_HI 0x824u
#define WIFI_MSI_EN 0x828u
#define WIFI_MSI_MASK 0x82cu
#define WIFI_MSI_STATUS 0x830u

static struct {
	uintptr_t cfg, dbi, bar, gr, cb, wc;
	void *tables, *backing;
	uint64_t tables_pa, backing_pa;
	size_t bar_bytes;
	unsigned msi, pcie, active, msi_seen, fw, ee, last_state, state_logs;
	unsigned bhi_initial, bhie_initial, transfer_seen;
	int cfg_error;
	struct sweet_wifi_dma_arena arena;
	struct sweet_wifi_dma_extent *extents;
} wifi;

static void
record(const char *stage, unsigned a, unsigned b)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "SWEET WIFI RUNTIME: %s a=%#x b=%#x\n",
	    stage, a, b);
	if (n > 0 && (size_t)n < sizeof(line)) sweet_log_append(line, n);
}

static void
mark(const char *stage, unsigned a, unsigned b)
{
	record(stage, a, b);
	sweet_log_sync();
}

static uint32_t
rd(uintptr_t va, unsigned off)
{
	uint32_t value = *(volatile uint32_t *)(va + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
wr(uintptr_t va, unsigned off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(va + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uintptr_t
smmu_reg(unsigned off, unsigned width)
{
	if (off < SMMU_GR_BYTES && width <= SMMU_GR_BYTES - off)
		return wifi.gr + off;
	if (wifi.cb && off >= SMMU_LOCAL_CB && off - SMMU_LOCAL_CB <= 4096u - width)
		return wifi.cb + off - SMMU_LOCAL_CB;
	return 0;
}

static int
smmu_read(void *arg, uint32_t off, uint32_t *value)
{
	(void)arg;
	uintptr_t addr = smmu_reg(off, 4);
	if (!addr || (off & 3)) return EINVAL;
	*value = rd(addr, 0);
	return 0;
}

static int
smmu_write(void *arg, uint32_t off, uint32_t value)
{
	(void)arg;
	uintptr_t addr = smmu_reg(off, 4);
	if (!addr || (off & 3)) return EINVAL;
	wr(addr, 0, value);
	return 0;
}

static int
smmu_write64(void *arg, uint32_t off, uint64_t value)
{
	(void)arg;
	uintptr_t addr = smmu_reg(off, 8);
	if (!addr || (off & 7)) return EINVAL;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint64_t *)addr = value;
	__asm__ volatile("dsb osh" ::: "memory");
	return 0;
}

static int
sync_tables(void *arg)
{
	(void)arg;
	flush_dcache64((uintptr_t)wifi.tables, 8192, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static void
smmu_delay(void *arg, unsigned us)
{
	(void)arg;
	IODelay(us);
}

static int
owns(void *arg, unsigned bank, unsigned slot)
{
	(void)arg;
	if (bank != SMMU_LOCAL_BANK || slot >= 113 || wifi.cb) return 0;
	/* Map only the context with local read/write precedent. This is not
	 * a grant for new handoff modes or other users of apps-SMMU. */
	wifi.cb = ml_io_map(SMMU_PA + SMMU_LOCAL_CB, 4096);
	return wifi.cb != 0;
}

static int
blocked(void *arg)
{
	(void)arg;
	uint32_t command = rd(wifi.cfg, 4);
	return command != UINT32_MAX && (command & 6u) == 2u;
}

static int
allows_diagnostic(void *arg)
{
	(void)arg;
	/* The caller has gated this mode and excluded other experimental
	 * apps-SMMU writers. This does not assert that DMA has worked. */
	return wifi.active;
}

static void
smmu_checkpoint(void *arg, unsigned step, unsigned bank, unsigned slot)
{
	(void)arg;
	mark("smmu", step, (bank << 16) | slot);
}

static uint64_t
now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000u;
}

static void *
heap_alloc(size_t size)
{
	if (size > SIZE_MAX - 16) return NULL;
	size_t *p = IOMallocData(size + 16);
	if (!p) return NULL;
	p[0] = size + 16;
	return (unsigned char *)p + 16;
}

static void
heap_free(void *value)
{
	if (value) {
		size_t *p = (void *)((unsigned char *)value - 16);
		IOFreeData(p, p[0]);
	}
}

static void *
dma_alloc(size_t size, uint64_t *bus)
{
	return sweet_wifi_dma_arena_alloc(&wifi.arena, size, bus);
}

static void
dma_free(size_t size, void *p, uint64_t bus)
{
	(void)size;
	/* Quarantine every DMA allocation during this short diagnostic.
	 * Driver error unwind must not make any range reusable while the
	 * endpoint's DMA ownership or quiescence is still uncertain. */
	(void)p;
	(void)bus;
}

static uint64_t
dma_map(void *p, size_t size, int direction)
{
	(void)direction;
	return sweet_wifi_dma_arena_map(&wifi.arena, p, size);
}

static void
dma_sync(uint64_t bus, size_t size, int direction)
{
	(void)bus;
	(void)size;
	(void)direction;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
firmware(const char *path, const uint8_t **data, size_t *size)
{
	int rc = sweet_wifi_firmware_get(path, data, size);
	if (!rc && wifi.fw++ < 12) record("firmware-borrow", wifi.fw, (unsigned)*size);
	return rc;
}

static int
cfg_offset(int offset, int width, unsigned *at)
{
	unsigned v = (unsigned)offset;
	if (v & 0x80000000u) {
		v &= 0x7fffffffu;
		if (v > 0x3fu || !wifi.pcie) return -EINVAL;
		v += wifi.pcie;
	}
	if ((width != 1 && width != 2 && width != 4) || v >= 256 ||
	    (v & ((unsigned)width - 1u))) return -EINVAL;
	*at = v;
	return 0;
}

static int
cfg_read(int offset, int width, uint32_t *value)
{
	unsigned at;
	if (!value) return -EINVAL;
	*value = 0;
	if (cfg_offset(offset, width, &at)) return -EINVAL;
	uint32_t word = rd(wifi.cfg, at & ~3u);
	if (word == UINT32_MAX) return -EIO;
	*value = width == 4 ? word :
	    (word >> ((at & 3u) * 8u)) & (width == 2 ? 0xffffu : 0xffu);
	return 0;
}

static int
cfg_write(int offset, int width, uint32_t value)
{
	unsigned at;
	uint32_t got;
	/* Allow only the MSI capability and PCIe Device/Link Control. In
	 * particular the imported stack may not change PCI_COMMAND or BAR. */
	if (cfg_offset(offset, width, &at)) goto fail;
	if (at == wifi.msi && width == 4) {
		at += 2;
		width = 2;
		value >>= 16;
	}
	if (!(width == 2 && (at == wifi.msi + 2 || at == wifi.pcie + 8 ||
	    at == wifi.pcie + 0x10)) &&
	    !(width == 4 && (at == wifi.msi + 4 || at == wifi.msi + 8 ||
	    at == wifi.msi + 12))) goto fail;
	if (width == 4) {
		wr(wifi.cfg, at, value);
	} else {
		/* ARM PCI config is accessed through the existing CFG ATU. A
		 * halfword write avoids echoing W1C Device Status bits. */
		value &= 0xffffu;
		__asm__ volatile("dmb oshst" ::: "memory");
		*(volatile uint16_t *)(wifi.cfg + at) = (uint16_t)value;
		__asm__ volatile("dsb osh" ::: "memory");
	}
	if (cfg_read((int)at, width, &got) || got != value) goto fail;
	return 0;
fail:
	wifi.cfg_error = EIO;
	return -EIO;
}

static int
msi_target(uint32_t *lo, uint32_t *hi, uint32_t *data)
{
	*lo = SWEET_WIFI_DMA_IOVA;
	*hi = *data = 0;
	return 0;
}

static void *
reject_phys(uint64_t address, size_t bytes)
{
	(void)address;
	(void)bytes;
	return NULL;
}

static unsigned
boot_window(unsigned off, unsigned last)
{
	uint32_t base = rd(wifi.bar, off);
	return base >= 0x100u && !(base & 3u) &&
	    base <= wifi.bar_bytes - last - 4u ? base : 0;
}

static unsigned
observe_mhi(void)
{
	unsigned bhi = boot_window(0x28u, 0x2cu);
	if (!bhi) return 0;
	unsigned ee = rd(wifi.bar, bhi + 0x28u);
	unsigned status = rd(wifi.bar, bhi + 0x2cu);
	if (ee > 7 || status == UINT32_MAX) return 0;
	if (wifi.ee != ee || wifi.last_state != status) {
		wifi.ee = ee;
		wifi.last_state = status;
		if (wifi.state_logs++ < 12) mark("bhi-state", ee, status);
	}
	return ee == 2u || ee == 4u;
}

static void
observe_transfer(void)
{
	/* BHI and BHIe have different register layouts. Only report a new
	 * nonzero doorbell and a completed transfer from this DMA arena. */
	static const struct {
		unsigned off, lo, hi, size, db, status;
	} windows[] = {
		{0x28, 0x08, 0x0c, 0x10, 0x18, 0x2c},
		{0x2c, 0x2c, 0x30, 0x34, 0x3c, 0x44},
	};
	if (!wifi.fw || wifi.transfer_seen) return;
	for (unsigned i = 0; i < 2; ++i) {
		unsigned base = boot_window(windows[i].off, windows[i].status);
		if (!base) continue;
		uint32_t done = rd(wifi.bar, base + windows[i].status);
		uint32_t seq = rd(wifi.bar, base + windows[i].db) & 0x3fffffffu;
		if ((done >> 30) != 2 || !seq ||
		    seq == (i ? wifi.bhie_initial : wifi.bhi_initial) ||
		    (i && (done & 0x3fffffffu) != seq)) continue;
		uint32_t bytes = rd(wifi.bar, base + windows[i].size);
		uint32_t lo = rd(wifi.bar, base + windows[i].lo);
		uint32_t hi = rd(wifi.bar, base + windows[i].hi);
		if (!bytes || hi || lo < SWEET_WIFI_DMA_IOVA + 4096u ||
		    lo - SWEET_WIFI_DMA_IOVA >= SWEET_WIFI_DMA_BYTES ||
		    bytes > SWEET_WIFI_DMA_BYTES - (lo - SWEET_WIFI_DMA_IOVA) ||
		    sweet_wifi_dma_arena_map(&wifi.arena,
		    (void *)(wifi.wc + (lo - SWEET_WIFI_DMA_IOVA)), bytes) != lo)
			continue;
		wifi.transfer_seen = 1;
		record(i ? "bhie-transfer" : "bhi-transfer", bytes, seq);
		break;
	}
}

static void
poll(void)
{
	observe_transfer();
	uint32_t bits = rd(wifi.dbi, WIFI_MSI_STATUS);
	if (bits && bits != UINT32_MAX) {
		wr(wifi.dbi, WIFI_MSI_STATUS, bits);
		if (!wifi.msi_seen++) record("msi-observed", bits, 0);
		nw_wifi_irq_pending(bits);
	}
	nw_wifi_poll();
}

static void
driver_log(int level, const char *line)
{
	/* No station credentials, packet content, or unbounded driver logs
	 * in the persisted ring. We only surface fixed diagnostic markers. */
	static const char message[] = "MHI did not load image";
	if (level <= 4) {
		for (; *line; ++line) {
			if (!strncmp(line, message, sizeof(message) - 1)) {
				record("mhi-image-error", (unsigned)level, 0);
				break;
			}
		}
	}
}

static void
halt(void)
{
	/* A shim fatal can occur while preemption is disabled. No storage
	 * sync or sleeping from this callback. */
	static const char message[] = "SWEET WIFI RUNTIME: driver-fatal\n";
	wr(wifi.dbi, WIFI_MSI_EN, 0);
	uint32_t command = rd(wifi.cfg, 4);
	if (command != UINT32_MAX) wr(wifi.cfg, 4, command & 0xfffbu);
	sweet_log_append(message, sizeof(message) - 1);
	panic("SWEET Wi-Fi shim reported a fatal error");
}

static int
capabilities(void)
{
	unsigned at = rd(wifi.cfg, 0x34) & 255u;
	uint64_t visited = 0;
	while (at) {
		if (at < 0x40u || at > 0xfcu || (at & 3) ||
		    (visited & (UINT64_C(1) << (at / 4)))) return EIO;
		visited |= UINT64_C(1) << (at / 4);
		uint32_t v = rd(wifi.cfg, at);
		if (v == UINT32_MAX) return EIO;
		if ((v & 255u) == 5u) {
			/* The pinned hw2.1 backend requests 28 vectors, for which
			 * the PCI MSI capability must support a 32-vector block. */
			if (wifi.msi || at > 0xf0u || !(v & (1u << 23)) ||
			    ((v >> 17) & 7u) < 5u) return ENOTSUP;
			wifi.msi = at;
		}
		if ((v & 255u) == 0x10u) {
			if (wifi.pcie || at > 0xecu) return EIO;
			wifi.pcie = at;
		}
		at = (v >> 8) & 255u;
	}
	return wifi.msi && wifi.pcie ? 0 : ENXIO;
}

static void
release_unexposed(void)
{
	if (wifi.wc) ml_io_unmap(wifi.wc, SWEET_WIFI_DMA_BYTES);
	if (wifi.backing) IOFreeContiguous(wifi.backing, SWEET_WIFI_DMA_BYTES);
	if (wifi.tables) IOFreeContiguous(wifi.tables, XNU_PAGE);
	if (wifi.extents) IOFreeData(wifi.extents, 16384u * sizeof(*wifi.extents));
}

int sweet_wifi_runtime_start(uint32_t, uint32_t, uint32_t, uint32_t);

int
sweet_wifi_runtime_start(uint32_t cfg_pa, uint32_t dbi_pa, uint32_t bar_pa,
    uint32_t bytes)
{
	static const unsigned candidates[] = { SMMU_LOCAL_BANK };
	static const struct sweet_wifi_smmu_io io = {
		.read32 = smmu_read, .write32 = smmu_write, .write64 = smmu_write64,
		.sync_tables = sync_tables, .delay_us = smmu_delay, .checkpoint = smmu_checkpoint,
	};
	static const struct sweet_wifi_smmu_grant grant = {
		.owns = owns, .endpoint_blocked = blocked, .allows_dma_diagnostic = allows_diagnostic,
		.candidates = candidates, .candidate_count = sizeof(candidates) / sizeof(*candidates),
	};
	static const struct lx_platform platform = {
		.now_ms = now_ms, .sleep_ms = IOSleep, .poll = poll, .log = driver_log,
		.halt = halt, .alloc = heap_alloc, .free = heap_free, .dma_alloc = dma_alloc,
		.dma_free = dma_free, .dma_map = dma_map, .dma_sync = dma_sync, .fw_get = firmware,
		.busy_delay_us = IODelay, .preempt_disable = _disable_preemption,
		.preempt_enable = _enable_preemption,
	};
	struct sweet_wifi_smmu_result result = {0};
	int error = EIO, started = 0;
	if (wifi.active) return EBUSY;
	wifi.active = 1;
	if (cfg_pa != 0x60100000u || dbi_pa != 0x60000000u ||
	    bar_pa != 0x60400000u || bytes != 0x200000u) return EINVAL;
	mark("map-pcie", bar_pa, bytes);
	wifi.cfg = ml_io_map(cfg_pa, 4096);
	wifi.dbi = ml_io_map(dbi_pa, 4096);
	wifi.bar = ml_io_map(bar_pa, bytes);
	wifi.bar_bytes = bytes;
	if (!wifi.cfg || !wifi.dbi || !wifi.bar) return ENOMEM;
	if (rd(wifi.cfg, 0) != 0x110317cbu || !blocked(NULL)) return EBUSY;
	error = capabilities();
	if (error) return error;
	mark("map-smmu", SMMU_PA, 0);
	wifi.gr = ml_io_map(SMMU_PA, SMMU_GR_BYTES);
	if (!wifi.gr) return ENOMEM;
	if (rd(wifi.gr, 0x20) != 0x4c017e71u || rd(wifi.gr, 0x24) != 0x6000004bu ||
	    rd(wifi.gr, 0x28) != 0x5111u) return ENOTSUP;

	mark("wired-alloc", SWEET_WIFI_DMA_BYTES, XNU_PAGE);
	wifi.tables = IOMallocContiguous(XNU_PAGE, XNU_PAGE, NULL);
	wifi.backing = IOMallocContiguous(SWEET_WIFI_DMA_BYTES, SWEET_WIFI_DMA_BLOCK, NULL);
	wifi.extents = IOMallocData(16384u * sizeof(*wifi.extents));
	error = ENOMEM;
	if (!wifi.tables || !wifi.backing || !wifi.extents) goto early;
	wifi.tables_pa = kvtophys((uintptr_t)wifi.tables);
	wifi.backing_pa = kvtophys((uintptr_t)wifi.backing);
	if (!wifi.tables_pa || (wifi.tables_pa & (XNU_PAGE - 1u)) ||
	    wifi.tables_pa > (UINT64_C(1) << 36) - XNU_PAGE ||
	    !wifi.backing_pa || (wifi.backing_pa & (SWEET_WIFI_DMA_BLOCK - 1u)) ||
	    wifi.backing_pa > (UINT64_C(1) << 36) - SWEET_WIFI_DMA_BYTES) goto early;
	/* Verify all CPU pages, including the last one, before making the
	 * second mapping or exposing the entire span to the endpoint. */
	for (size_t off = 0; off < SWEET_WIFI_DMA_BYTES; off += XNU_PAGE)
		if (kvtophys((uintptr_t)wifi.backing + off) != wifi.backing_pa + off) goto early;
	memset(wifi.backing, 0, SWEET_WIFI_DMA_BYTES);
	flush_dcache64((uintptr_t)wifi.backing, SWEET_WIFI_DMA_BYTES, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	mark("map-dma-wc", (unsigned)(wifi.backing_pa >> 32), (unsigned)wifi.backing_pa);
	wifi.wc = ml_io_map_unmappable((uintptr_t)wifi.backing_pa, SWEET_WIFI_DMA_BYTES,
	    XNU_VM_WIMG_WCOMB);
	if (!wifi.wc) goto early;
	error = sweet_wifi_dma_arena_init(&wifi.arena, (void *)wifi.wc, wifi.extents, 16384);
	if (error) goto early;
	error = sweet_wifi_smmu_commit(&io, &grant, wifi.tables,
	    (uint64_t *)wifi.tables + 512u, wifi.tables_pa, wifi.tables_pa + 4096u,
	    wifi.backing_pa, &result);
	if (error || !result.configured) {
		mark("smmu-rejected", (unsigned)error, result.last_step);
		if (!result.retain_backing) goto early;
		return error ? error : EIO;
	}

	mark("msi-poll-setup", wifi.msi, wifi.pcie);
	/* Capture DW MSI status, but mask delivery to the GIC: no native
	 * handler is installed in this XNU. The shim can poll MHI even on
	 * hardware where masking also suppresses DBI status. */
	wr(wifi.dbi, WIFI_MSI_EN, 0);
	wr(wifi.dbi, WIFI_MSI_MASK, UINT32_MAX);
	wr(wifi.dbi, WIFI_MSI_LO, SWEET_WIFI_DMA_IOVA);
	wr(wifi.dbi, WIFI_MSI_HI, 0);
	wr(wifi.dbi, WIFI_MSI_STATUS, UINT32_MAX);
	wr(wifi.dbi, WIFI_MSI_EN, UINT32_MAX);
	error = EIO;
	if (rd(wifi.dbi, WIFI_MSI_MASK) != UINT32_MAX ||
	    rd(wifi.dbi, WIFI_MSI_LO) != SWEET_WIFI_DMA_IOVA ||
	    rd(wifi.dbi, WIFI_MSI_HI) || rd(wifi.dbi, WIFI_MSI_EN) != UINT32_MAX ||
	    !blocked(NULL)) goto stop;

	static struct nw_board board;
	board = (struct nw_board){.bar = (void *)wifi.bar, .bar_len = wifi.bar_bytes,
	    .device_id = 0x1103, .msi_cap = (uint16_t)wifi.msi, .irq_base = 1000,
	    .cfg_read = cfg_read, .cfg_write = cfg_write, .msi_target = msi_target,
	    .map_phys = reject_phys};
	wifi.ee = UINT32_MAX;
	unsigned mission_at_entry = observe_mhi();
	if (wifi.ee != 0u || mission_at_entry) {
		mark("unexpected-boot-state", wifi.ee, wifi.last_state);
		goto stop;
	}
	unsigned bhi = boot_window(0x28u, 0x2cu);
	unsigned bhie = boot_window(0x2cu, 0x44u);
	if (!bhi) goto stop;
	wifi.bhi_initial = rd(wifi.bar, bhi + 0x18u) & 0x3fffffffu;
	wifi.bhie_initial = bhie ? rd(wifi.bar, bhie + 0x3cu) & 0x3fffffffu : 0;
	mark("bme-enable", result.route.bank, result.route.slot);
	uint32_t command = rd(wifi.cfg, 4);
	if (command == UINT32_MAX || (command & 6u) != 2u) goto stop;
	wr(wifi.cfg, 4, (command & 0xffffu) | 0x406u);
	command = rd(wifi.cfg, 4);
	if (command == UINT32_MAX || (command & 0x406u) != 0x406u) goto stop;
	mark("mhi-start", 0, 0);
	nw_wifi_irq_mode(0);
	int rc = nw_wifi_start(&platform, &board);
	if (rc) {
		observe_transfer();
		(void)observe_mhi();
		mark("driver-start-failed", (unsigned)rc, (unsigned)wifi.cfg_error);
		goto stop; /* nw_wifi_start handles its own software unwind */
	}
	started = 1;
	uint64_t begin = now_ms();
	error = ETIMEDOUT;
	do {
		if (wifi.cfg_error) { error = EIO; break; }
		(void)nw_wifi_step();
		if (wifi.cfg_error) { error = EIO; break; }
		observe_transfer();
		if (observe_mhi() && nw_wifi_stats.stage >= 4 && wifi.fw) {
			mark("firmware-and-stack-ready", nw_wifi_stats.stage, wifi.msi_seen);
			error = wifi.transfer_seen ? 0 : EIO;
			if (error) mark("transfer-witness-missing", wifi.fw, 0);
			break;
		}
		IOSleep(1);
	} while (now_ms() - begin < 30000u);
stop:
	mark("transport-stop", (unsigned)error, nw_wifi_stats.stage);
	if (started) nw_wifi_stop();
	wr(wifi.dbi, WIFI_MSI_EN, 0);
	command = rd(wifi.cfg, 4);
	if (command != UINT32_MAX) wr(wifi.cfg, 4, command & 0xfffbu);
	if (!blocked(NULL) || rd(wifi.dbi, WIFI_MSI_EN) != 0) error = EIO;
	mark("transport-retained", (unsigned)error, result.route.slot);
	return error;
early:
	release_unexposed();
	return error;
}
