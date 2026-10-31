/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"

#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem@146aa000 restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
/* marble: no direct PSHOLD; reset via PSCI */

#define GICD_BASE		0x17100000UL
#define GICR_BASE		0x17180000UL
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x40000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* marble: PPI 11 */
#define PTIMER_INTID		30	/* marble: PPI 14 */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
/* marble: 4x A510 0x000-0x300, 3x A710 0x400-0x600, 1x X2 0x700 */
static inline uint64_t cpu_mpidr(int i)
{
	static const uint64_t m[NR_CPUS] = { 0x000, 0x100, 0x200, 0x300,
					     0x400, 0x500, 0x600, 0x700 };
	return (i >= 0 && i < NR_CPUS) ? m[i] : 0;
}

static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

/*
 * ---- GPU SMMU / KGSL probe ----
 *
 * The GPU has its own SMMU-500 (kgsl-smmu@3da0000) and its own register block
 * (kgsl-3d0@3d00000); neither is used by XNU yet, so both are exactly as ABL
 * left them.  The question this answers is whether the non-secure AP can own
 * that SMMU at all: the audio SID on apps-smmu cannot be taken away from the
 * hypervisor, but kgsl-smmu is a different instance.
 *
 * Runs with the MMU on, before XNU.  Results go to low DRAM at GPU_PROBE_PA
 * (outside the ADT bank, so XNU never allocates it); bsd/dev/sweet_glink.c
 * maps that page and prints it into the normal kernel log.
 *
 * GPU_PROBE_LEVEL (build knob): 1 = SMMU reads, 2 = +S2CR write test,
 * 3 = +KGSL RBBM_STATUS read, 4 = both extras.
 */
#define GPU_PROBE_PA		0xa2e0f000UL
#define GPU_PROBE_MAGIC		0x47505550U	/* "GPUP" */
#define GPU_SMMU_BASE		0x3da0000UL
#define GPU_KGSL_BASE		0x3d00000UL
#define GPU_GPUCC_BASE		0x3d90000UL
#ifndef GPU_PROBE_LEVEL
#define GPU_PROBE_LEVEL		1
#endif
/* Optional register matrix; every one is a separate build. */
#ifndef GP_CBAR_BANK
#define GP_CBAR_BANK		-1	/* -1 = skip */
#endif
#ifndef GP_CBAR_WRITE
#define GP_CBAR_WRITE		0	/* 1 = write 0, 2 = write 0x10000 */
#endif
#ifndef GP_CBAR_COUNT
#define GP_CBAR_COUNT		0	/* read CBAR(0..n-1) */
#endif
#ifndef GP_SCTLR_BANK
#define GP_SCTLR_BANK		-1
#endif
#ifndef GP_SCTLR_WRITE
#define GP_SCTLR_WRITE		0
#endif
#ifndef GP_CB_REGS
#define GP_CB_REGS		0	/* read CB0 TTBR0/TCR/MAIR */
#endif
#ifndef GP_GR1_READ
#define GP_GR1_READ		0	/* read CBA2R/CBFRSYNRA */
#endif
#ifndef GP_KGSL_SCAN
#define GP_KGSL_SCAN		0	/* read four KGSL offsets */
#endif
#ifndef GP_SURVEY
#define GP_SURVEY		0	/* v8: CB bank at the real offset + GPUCC */
#endif
#ifndef GP_PWR
#define GP_PWR			0	/* v9: bring GPU_CC_CX up after the survey */
#endif
#ifndef GP_EARLY
#define GP_EARLY		0	/* v10: power CX (and GX) before the survey */
#endif
#ifndef GP_GX
#define GP_GX			0	/* v10: also power GPU_CC_GX */
#endif
#ifndef GP_CBW
#define GP_CBW			0	/* v11: program CB0 + SID 0 */
#endif
#ifndef GP_CBT
#define GP_CBT			0	/* v11a: is the CB register file writable? */
#endif
#ifndef GP_CMDB
#define GP_CMDB			0	/* v12: AOP cmd-db: find gfx.lvl and friends */
#endif
#ifndef GP_RAIL
#define GP_RAIL			0	/* v13: RPMh ARC vote for gfx.lvl/mx.lvl */
#endif
#ifndef GP_CLK
#define GP_CLK			0	/* v14: hub/gmu RCGs + gpu_cc_ahb */
#endif
#ifndef GP_GMU
#define GP_GMU			0	/* v15: GMU block reset + register liveness */
#endif
#ifndef GP_CLK_SEL
#define GP_CLK_SEL		6	/* hub RCG source: 0 tcxo, 6 gpll0/2 */
#endif
#ifndef GP_BR
#define GP_BR			0	/* v16: GPUCC branch writability scan */
#endif
#ifndef GP_GCC
#define GP_GCC			0	/* v17: GCC gate that feeds the GPUCC hub RCG */
#endif
#ifndef GP_VOTE
#define GP_VOTE			0	/* v18: scan the HLOS1 vote page at 0x7000 */
#endif
#ifndef GP_RSCC
#define GP_RSCC			0	/* v21: vendor PDC + RSCC ucode load */
#endif
#ifndef GP_GMUBOOT
#define GP_GMUBOOT		0	/* v22: load GMU firmware and start the CM3 */
#endif
#ifndef GP_TCM
#define GP_TCM			0	/* v23: are the ITCM/DTCM windows writable? */
#endif
#ifndef GP_STAGE
#define GP_STAGE		0	/* v28: stage GMU fw + buffers for XNU */
#endif
#ifndef GP_SMPREL
#define GP_SMPREL		0	/* v29: release CM3 from a secondary CPU */
#endif
#define GP_STR2(x) #x
#define GP_STR(x) GP_STR2(x)
static const char gp_level_tag[] __attribute__((used)) =
    "gpuprobe-" GP_STR(GPU_PROBE_LEVEL)
    "-c" GP_STR(GP_CBAR_BANK) "w" GP_STR(GP_CBAR_WRITE)
    "-s" GP_STR(GP_SCTLR_BANK) "w" GP_STR(GP_SCTLR_WRITE)
    "-r" GP_STR(GP_CB_REGS) GP_STR(GP_GR1_READ) GP_STR(GP_KGSL_SCAN)
    "-v" GP_STR(GP_SURVEY) "e" GP_STR(GP_EARLY) "g" GP_STR(GP_GX)
    "w" GP_STR(GP_CBW) "t" GP_STR(GP_CBT)
    "d" GP_STR(GP_CMDB) "r" GP_STR(GP_RAIL)
    "k" GP_STR(GP_CLK) "m" GP_STR(GP_GMU) "s" GP_STR(GP_CLK_SEL)
    "b" GP_STR(GP_BR) "g" GP_STR(GP_GCC) "o" GP_STR(GP_VOTE)
    "c" GP_STR(GP_RSCC) "u" GP_STR(GP_GMUBOOT) "t2" GP_STR(GP_TCM)
    "st" GP_STR(GP_STAGE) "sr" GP_STR(GP_SMPREL);

/* Keep this layout identical to the copy in bsd/dev/sweet_glink.c. */
struct gpu_probe_result {
	uint32_t magic;		/* 0x00 */
	uint32_t version;	/* 0x04 */
	uint32_t scr0;		/* 0x08 */
	uint32_t id0;		/* 0x0c */
	uint32_t id1;		/* 0x10 */
	uint32_t id2;		/* 0x14 */
	uint32_t num_smr;	/* 0x18 */
	uint32_t num_cb;	/* 0x1c */
	uint32_t valid_seen;	/* 0x20 */
	uint32_t sid0_slot;	/* 0x24 */
	uint32_t smr0;		/* 0x28 */
	uint32_t s2cr0;		/* 0x2c */
	uint32_t wsid;		/* 0x30 */
	uint32_t w2cr_old;	/* 0x34 */
	uint32_t wsmr;		/* 0x38 */
	uint32_t w2cr;		/* 0x3c */
	uint32_t w2cr_f;	/* 0x40 */
	uint32_t w2cr_r;	/* 0x44 */
	uint32_t rfault;	/* 0x48 */
	uint32_t wfault;	/* 0x4c */
	uint32_t kgsl_status;	/* 0x50 */
	uint32_t kgsl_status2;	/* 0x54 */
	uint32_t kfault;	/* 0x58 */
	uint32_t pad;		/* 0x5c */
	uint64_t far;		/* 0x60 */
	uint64_t esr;		/* 0x68 */
	uint32_t w2cr_t;	/* 0x70 readback after S2CR = TRANS|CB0 */
	uint32_t wcbar_old;	/* 0x74 CBAR(0) before */
	uint32_t wcbar;		/* 0x78 CBAR(0) after writing 0 */
	uint32_t wsctlr_old;	/* 0x7c CB0.SCTLR before */
	uint32_t wsctlr;	/* 0x80 CB0.SCTLR after writing 0 */
	uint32_t op_fault;	/* 0x84 bitmask for the optional matrix */
	uint64_t pad3;		/* 0x88 */
	uint32_t cbar_bank;	/* 0x90 bank selected by GP_CBAR_BANK */
	uint32_t cbar_r;	/* 0x94 */
	uint32_t cbar_w;	/* 0x98 readback after the write */
	uint32_t sctlr_bank;	/* 0x9c */
	uint32_t sctlr_r;	/* 0xa0 */
	uint32_t sctlr_w;	/* 0xa4 */
	uint32_t ttbr0;		/* 0xa8 CB0+0x20 */
	uint32_t tcr;		/* 0xac CB0+0x30 */
	uint32_t mair;		/* 0xb0 CB0+0x38 */
	uint32_t cba2r;		/* 0xb4 GR1+0x800 */
	uint32_t cbfrsynra;	/* 0xb8 GR1+0x400 */
	uint32_t cbar_count;	/* 0xbc */
	uint32_t cbar1;		/* 0xc0 */
	uint32_t cbar2;		/* 0xc4 */
	uint32_t cbar3;		/* 0xc8 */
	uint32_t last_step;	/* 0xcc written before every risky access */
	uint32_t prev_step;	/* 0xd0 last_step found at entry */
	uint32_t prev_magic;	/* 0xd4 */
	uint32_t kg0;		/* 0xd8 */
	uint32_t kg1;		/* 0xdc */
	uint32_t kg2;		/* 0xe0 */
	uint32_t kg3;		/* 0xe4 */
	uint32_t pad4;		/* 0xe8 */
	uint64_t pad5;		/* 0xf0 */

	/*
	 * v9 block (appended, so older kernels that only print up to pad5
	 * still work).  These are the named fields the GPU probes really want;
	 * the reused slots above stay as a fallback for an unpatched kernel.
	 */
	uint32_t p_cbbase;	/* 0xf8 computed context-bank base */
	uint32_t p_cb0_sctlr;	/* 0xfc */
	uint32_t p_cb0_ttbr0;	/* 0x100 */
	uint32_t p_cb0_ttbr1;	/* 0x104 */
	uint32_t p_cb0_tcr;	/* 0x108 */
	uint32_t p_cb0_tcr2;	/* 0x10c */
	uint32_t p_cb0_mair0;	/* 0x110 */
	uint32_t p_cb0_mair1;	/* 0x114 */
	uint32_t p_cb0_cbar;	/* 0x118 CBAR(0) */
	uint32_t p_cb0_cba2r;	/* 0x11c */
	uint32_t p_cb0_cbf;	/* 0x120 CBFRSYNRA(0) */
	uint32_t p_cb1_sctlr;	/* 0x124 */
	uint32_t p_cxgdr;	/* 0x128 GPU_CC_CX_GDSCR */
	uint32_t p_hwctrl;	/* 0x12c GPU_CC CX GDS_HW_CTRL */
	uint32_t p_gxgdr;	/* 0x130 GPU_CC_GX_GDSCR */
	uint32_t p_gxdom;	/* 0x134 GX domain-addr register */
	uint32_t p_clk_ahb;	/* 0x138 gpu_cc_ahb */
	uint32_t p_clk_cxgmu;	/* 0x13c gpu_cc_cx_gmu */
	uint32_t p_clk_hubcx;	/* 0x140 gpu_cc_hub_cx_int */
	uint32_t p_clk_smmuvote;/* 0x144 gpu_cc_hlos1_vote_gpu_smmu */
	uint32_t p_clk_memnoc;	/* 0x148 gpu_cc_memnoc_gfx */
	uint32_t p_kgsl_ident;	/* 0x14c */
	uint32_t p_kgsl_rbbm;	/* 0x150 */
	uint32_t p_rd_fault;	/* 0x154 bitmask of failed reads */
	uint32_t p_cxgdr2;	/* 0x158 after the power-on attempt */
	uint32_t p_clk_ahb2;	/* 0x15c */
	uint32_t p_clk_cxgmu2;	/* 0x160 */
	uint32_t p_clk_hubcx2;	/* 0x164 */
	uint32_t p_clk_smmuvote2;/* 0x168 */
	uint32_t p_clk_memnoc2;	/* 0x16c */
	uint32_t p_kgsl_rbbm2;	/* 0x170 */
	uint32_t p_cb0_sctlr2;	/* 0x174 after power / after CB write */
	uint32_t p_pwr_step;	/* 0x178 last power-on step reached */
	uint32_t p_pwr_flags;	/* 0x17c */
	uint32_t p_cbw_before;	/* 0x180 CB0.SCTLR before the CB write */
	uint32_t p_cbw_after;	/* 0x184 readback */
	uint32_t p_cbw_flags;	/* 0x188 */
	uint64_t p_pad6;	/* 0x190 */
};				/* 0x198 */

static volatile int gp_busy;
static volatile int gp_fault;
static volatile uint64_t gp_esr, gp_far;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	/*
	 * The GPU probe reads blocks that may be powered down.  A synchronous
	 * data abort there is an answer, not a panic: record ESR/FAR, skip the
	 * load/store, and let the probe carry on with the next register.
	 */
	if (gp_busy && (slot & 3) == 0 &&
	    ((rsr("esr_el1") >> 26) & 0x3f) == 0x25) {
		gp_fault = 1;
		gp_esr = rsr("esr_el1");
		gp_far = rsr("far_el1");
		wsr("elr_el1", rsr("elr_el1") + 4);
		isb();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* Probe-safe MMIO: the fault flag makes the caller ignore the value. */
static uint32_t
gp_rd32(uintptr_t a)
{
	uint32_t v = 0;

	gp_fault = 0;
	gp_busy = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	v = rd32(a);
	__asm__ volatile("dsb sy" ::: "memory");
	gp_busy = 0;
	if (gp_fault)
		return 0xffffffffu;
	return v;
}

static void
gp_wr32(uintptr_t a, uint32_t v)
{
	gp_fault = 0;
	gp_busy = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(a, v);
	__asm__ volatile("dsb sy" ::: "memory");
	gp_busy = 0;
}

/*
 * Where the context banks live.
 *
 * An SMMU-500 register file is two GR frames (GR0, GR1), each
 * 1 << (IDR1.NUMPAGENDXB + 1) pages of IDR1.PAGESIZE bytes, followed by the
 * context banks.  Our IDR1 = 0x40000016: PAGESIZE=0 (4K) and NUMPAGENDXB=4,
 * so GR0/GR1 are 32 pages each and CB0 = 0x3da0000 + 0x20000 = 0x3dc0000.
 *
 * The first GPU probe copied apps-smmu's CB offset (0x80000, correct there
 * because NUMPAGENDXB=6) and read 0x3e20000 -- 0x40000 bytes past the end of
 * this block, which is dead bus space.  Those boots hung; the CBs may have
 * been fine all along.
 */
static uintptr_t
gp_cb_base(uint32_t id1)
{
	unsigned npg = (id1 >> 28) & 7u;
	unsigned sh = (id1 & (1u << 31)) ? 16u : 12u;

	return GPU_SMMU_BASE + ((uintptr_t)(1u << (npg + 1u)) << sh);
}

/*
 * GPU_CC_CX / GPU_CC_GX power switches.
 *
 * Follows the vendor gdsc-regulator driver: GDSCR.PWR_ON is bit 31,
 * HW_CONTROL bit 1, SW_COLLAPSE bit 0 (1 = collapsed).  Under HW control the
 * AP has to take the switch back by clearing HW_CONTROL first.  The GX domain
 * additionally wants a BLK_ARES pulse, a GMEM_RESET pulse and the release of
 * GMEM_CLAMP_IO before the switch is closed.
 *
 * Every write goes through the fault-tolerant accessor, so a block that is
 * really gone records a flag instead of taking the boot with it.
 */
static void
gp_power_cx(volatile struct gpu_probe_result *r)
{
	uintptr_t g = GPU_GPUCC_BASE;
	uint32_t v;
	unsigned i;
	int ok = 1;

	r->p_pwr_step = 1;
	v = gp_rd32(g + 0x9108u);		/* GPU_CC_CX_GDSCR */
	con_hex("GPUS", 1);
	if (gp_fault) {
		r->p_pwr_flags |= 1u;
		return;
	}
	if (!(v & (1u << 31))) {
		uint32_t w = v & ~(1u << 0);

		if (v & (1u << 1)) {
			w &= ~(1u << 1);
			r->p_pwr_flags |= 0x100u;
		}
		r->p_pwr_step = 2;
		gp_wr32(g + 0x9108u, w);
		if (gp_fault) {
			r->p_pwr_flags |= 2u;
			ok = 0;
		}
		r->p_pwr_step = 3;
		for (i = 0; ok && i < 1500u; i++) {
			v = gp_rd32(g + 0x9108u);
			if (gp_fault) {
				r->p_pwr_flags |= 4u;
				ok = 0;
			} else if (v & (1u << 31)) {
				break;
			}
			delay_ms(1);
		}
		if (ok && !(v & (1u << 31)))
			r->p_pwr_flags |= 8u;
		else if (ok)
			r->p_pwr_flags |= 0x200u;	/* powered */
	} else {
		r->p_pwr_flags |= 0x400u;		/* was already on */
	}
	con_hex2("GPUS", "cx=", v, "fl=", r->p_pwr_flags);

	/*
	 * GPU_CC_CX_BCR (0x9104, bit 0 = BLK_ARES).  A powered block that is
	 * still held in reset reads back as per-boot garbage and drops writes,
	 * which is exactly what the CB page looked like.  Deassert it and keep
	 * both readbacks in the old fault/restore slots for the log.
	 */
	r->p_pwr_step = 5;
	v = gp_rd32(g + 0x9104u);
	r->w2cr_f = v;
	if (!gp_fault && (v & 1u)) {
		gp_wr32(g + 0x9104u, v & ~1u);
		delay_ms(2);
		r->w2cr_r = gp_rd32(g + 0x9104u);
		if (!gp_fault && (r->w2cr_r & 1u))
			r->p_pwr_flags |= 0x80000u;	/* still asserted */
		else
			r->p_pwr_flags |= 0x100000u;	/* released now */
	} else if (!gp_fault) {
		r->w2cr_r = v;
		r->p_pwr_flags |= 0x200000u;		/* was already released */
	}
	con_hex2("GPUS", "bcr=", r->w2cr_f, "bcr2=", r->w2cr_r);

	/* Ungate: smmu vote, ahb, hub_cx_int, cx_gmu. */
	{
		static const uint32_t offs[4] = {
			0x7000u, 0x911cu, 0x9148u, 0x913cu
		};

		for (i = 0; ok && i < 4u; i++) {
			r->p_pwr_step = 10u + i;
			v = gp_rd32(g + offs[i]);
			if (gp_fault) {
				r->p_pwr_flags |= 0x1000u << i;
				continue;
			}
			if (!(v & 1u)) {
				gp_wr32(g + offs[i], v | 1u);
				if (gp_fault)
					r->p_pwr_flags |= 0x1000u << i;
			}
		}
	}
	con_hex("GPUS", 2);
}

static void
gp_power_gx(volatile struct gpu_probe_result *r)
{
	uintptr_t g = GPU_GPUCC_BASE;
	const uint32_t srst[3] = { 0x9058u, 0x9358u, 0x958cu };
	uint32_t v, w;
	unsigned i;

	r->p_pwr_step = 40;
	v = gp_rd32(g + 0x905cu);		/* GPU_CC_GX_GDSCR */
	if (gp_fault) {
		r->p_pwr_flags |= 0x10000u;
		return;
	}
	if (v & (1u << 31))
		return;				/* already on */

	if (v & (1u << 1)) {
		r->p_pwr_flags |= 0x20000u;	/* took it from HW */
		gp_wr32(g + 0x905cu, v & ~(1u << 1));
	}

	/* BLK_ARES pulse, then GMEM_RESET pulse + release clamp. */
	r->p_pwr_step = 41;
	for (i = 0; i < 3u; i++) {
		w = gp_rd32(g + srst[i]);
		if (!gp_fault)
			gp_wr32(g + srst[i], w | 1u);
	}
	delay_ms(1);
	for (i = 0; i < 3u; i++) {
		w = gp_rd32(g + srst[i]);
		if (!gp_fault)
			gp_wr32(g + srst[i], w & ~1u);
	}
	r->p_pwr_step = 42;
	w = gp_rd32(g + 0x9504u);		/* GX domain register */
	if (!gp_fault) {
		gp_wr32(g + 0x9504u, w | (1u << 4));	/* GMEM_RESET */
		delay_ms(1);
		gp_wr32(g + 0x9504u, (w & ~(1u << 4)) & ~1u);
	}

	/* Close the switch and wait for PWR_ON. */
	r->p_pwr_step = 43;
	v = gp_rd32(g + 0x905cu);
	if (gp_fault)
		return;
	gp_wr32(g + 0x905cu, v & ~1u);
	for (i = 0; i < 1500u; i++) {
		v = gp_rd32(g + 0x905cu);
		if (gp_fault)
			break;
		if (v & (1u << 31)) {
			r->p_pwr_flags |= 0x40000u;
			break;
		}
		delay_ms(1);
	}

	/* GX clocks: gfx3d, gx_gmu, gx_ff, memnoc_gfx, gx_vsense. */
	{
		static const uint32_t offs[5] = {
			0x90a8u, 0x90bcu, 0x90c0u, 0x9150u, 0x90b0u
		};

		r->p_pwr_step = 44;
		for (i = 0; i < 5u; i++) {
			w = gp_rd32(g + offs[i]);
			if (gp_fault)
				continue;
			if (!(w & 1u))
				gp_wr32(g + offs[i], w | 1u);
		}
	}
	r->p_pwr_step = 45;
	r->p_kgsl_ident = gp_rd32(GPU_KGSL_BASE + 0x000u);
	r->p_kgsl_rbbm2 = gp_rd32(GPU_KGSL_BASE + 0x840u);
	con_hex2("GPUS", "gx=", v, "id=", r->p_kgsl_ident);
}

#if GP_RAIL
/*
 * Minimal RPMh ARC-level vote: one command in one idle ACTIVE TCS, the same
 * handshake sweet_rpmh.c uses for the PMIC LDOs on this phone, minus the
 * PMIC-specific mode/enable commands (an ARC resource only takes a level).
 * Returns 0 on a verified completion, a negative tag otherwise.
 */
#define RH_RSC			0x17a20000UL	/* apps_rsc drv-2 */
#define RH_TCS			0x17a20d00UL
#define RH_TCS_STRIDE		0x2a0u
#define RH_CMD_STRIDE		0x14u
#define RH_IRQ_EN		0x00u
#define RH_IRQ_STATUS		0x04u
#define RH_IRQ_CLEAR		0x08u
#define RH_WAIT			0x10u
#define RH_CONTROL		0x14u
#define RH_STATUS		0x18u
#define RH_ENABLE		0x1cu
#define RH_MSGID		0x30u
#define RH_ADDR			0x34u
#define RH_DATA			0x38u
#define RH_CMD_STATUS		0x3cu
#define RH_AMC_EN		(1u << 16)
#define RH_TRIGGER		(1u << 24)
#define RH_WRITE_ACK		(8u | (1u << 8) | (1u << 16))

static int
gp_rh_sync(uintptr_t a, uint32_t value)
{
	unsigned i;

	gp_wr32(a, value);
	for (i = 0; i < 100u; i++) {
		if (gp_rd32(a) == value)
			return 1;
		delay_ms(1);
	}
	return 0;
}

static int
gp_rh_vote(uint32_t addr, uint32_t level)
{
	uint32_t config = gp_rd32(RH_RSC + 0x0cu);
	unsigned ncpt = (config >> 27) & 0x1fu;
	uint32_t b, bit, irq_en;
	int t = -1, rc = -5;
	unsigned i, c;

	if (gp_fault || !ncpt || ncpt > 16u)
		return -1;

	for (i = 0; i < 2u; i++) {
		uint32_t s = RH_TCS + i * RH_TCS_STRIDE;

		if (gp_rd32(s + RH_STATUS) != 0 &&
		    gp_rd32(s + RH_STATUS) != 0xffffffffu &&
		    gp_rd32(s + RH_CONTROL) == 0 &&
		    gp_rd32(s + RH_ENABLE) == 0 &&
		    gp_rd32(s + RH_WAIT) == 0 &&
		    !(gp_rd32(RH_TCS + RH_IRQ_STATUS) & (1u << i))) {
			t = (int)i;
			break;
		}
	}
	if (t < 0)
		return -2;

	b = RH_TCS + (uint32_t)t * RH_TCS_STRIDE;
	bit = 1u << (uint32_t)t;
	irq_en = gp_rd32(RH_TCS + RH_IRQ_EN);
	if (irq_en == 0xffffffffu)
		return -3;
	if (!gp_rh_sync(RH_TCS + RH_IRQ_EN, irq_en & ~bit))
		return -4;
	if (!gp_rh_sync(b + RH_WAIT, 7u) || !gp_rh_sync(b + RH_ENABLE, 1u) ||
	    !gp_rh_sync(b + RH_CONTROL, 0u) || !gp_rh_sync(b + RH_CONTROL, RH_AMC_EN))
		return -4;

	gp_wr32(b + RH_MSGID, RH_WRITE_ACK);
	gp_wr32(b + RH_ADDR, addr);
	gp_wr32(b + RH_DATA, level);
	__asm__ volatile("dsb sy" ::: "memory");
	gp_wr32(b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);

	for (i = 0; i < 2000u; i++) {
		uint32_t st = gp_rd32(b + RH_CMD_STATUS);

		if (gp_fault)
			break;
		if ((st & ((1u << 8) | (1u << 16))) == ((1u << 8) | (1u << 16))) {
			rc = 0;
			break;
		}
		delay_ms(1);
	}
	(void)c;

	gp_rh_sync(b + RH_CONTROL, RH_AMC_EN);
	gp_rh_sync(b + RH_CONTROL, 0u);
	gp_rh_sync(b + RH_ENABLE, 0u);
	gp_rh_sync(b + RH_WAIT, 0u);
	gp_wr32(RH_TCS + RH_IRQ_CLEAR, bit);
	gp_wr32(RH_TCS + RH_IRQ_EN, irq_en);
	return rc;
}
#endif

#if GP_CLK
/*
 * The two RCGs the SMMU and the GMU sit behind.  RCG2 layout (clk-rcg2.c):
 * CMD_RCGR bit0 = UPDATE, bit1 = ROOT_EN, bit31 = ROOT_OFF; CFG_RCGR has
 * SRC_SEL in bits 10:8, using the parent_map value (not the index).
 * hub: parents {0 tcxo, 3 pll1, 5 gpll0, 6 gpll0/2}; gmu: add {1 pll0}.
 * GPLL0 feeds half the SoC and is always running, so 6 is the safe pick;
 * the GMU source is left on TCXO (0) for the first bring-up.
 */
static void
gp_clk_setup(volatile struct gpu_probe_result *r)
{
	static const struct { uint32_t cmd, cfg, sel; } rcg[2] = {
		{ 0x93ecu, 0x93f0u, GP_CLK_SEL },	/* hub_clk_src */
		{ 0x9318u, 0x931cu, 0u },		/* gmu_clk_src */
	};
	static const uint32_t br[4] = { 0x93e8u, 0x911cu, 0x9148u, 0x913cu };
	uint32_t cmd;
	unsigned i;

#if GP_GCC
	/*
	 * gcc_gpu_gpll0_div_clk_src is a GCC branch at 0x62000 bit 16 (and
	 * gcc_gpu_gpll0_clk_src is bit 15).  GPUCC's hub RCG is fed from here;
	 * with the GCC gate closed the RCG reads ROOT_EN=1 but outputs nothing,
	 * and every branch below it (hub_ahb_div, gpu_cc_ahb) stays dead.
	 */
	{
		uintptr_t a = 0x162000UL;
		uint32_t v = gp_rd32(a);

		if (!gp_fault) {
			gp_wr32(a, v | (1u << 15) | (1u << 16));
			__asm__ volatile("dsb sy" ::: "memory");
			delay_ms(2);
			r->p_cb0_sctlr = gp_rd32(a);	/* GCC 0x162000 readback */
		}
	}
#endif
	/*
	 * Bring the hub's always-on gate up first: the RCG/divider registers
	 * live behind it, and programming them while it is off leaves the
	 * whole hub branch dead (observed: hub_ahb_div and gpu_cc_ahb stuck
	 * at 0 no matter what the RCG said).
	 */
	{
		static const uint32_t pre[2] = { 0x93e8u, 0x9144u };

		for (i = 0; i < 2u; i++) {
			uintptr_t a = GPU_GPUCC_BASE + pre[i];
			uint32_t v = gp_rd32(a);

			if (!gp_fault && !(v & 1u)) {
				gp_wr32(a, v | 1u);
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(2);
	}

	for (i = 0; i < 2u; i++) {
		uintptr_t c = GPU_GPUCC_BASE + rcg[i].cmd;
		uintptr_t f = GPU_GPUCC_BASE + rcg[i].cfg;
		uint32_t v = gp_rd32(f);

		if (gp_fault)
			continue;
		v &= ~0x700u;
		v |= rcg[i].sel << 8;
		gp_wr32(f, v);
		/*
		 * The RCG output is parent/(2*M) in integer mode; leaving
		 * M=0 gives a dead clock even with ROOT_EN=1.  M=1 is the
		 * slowest running setting and is enough for a bring-up.
		 */
		gp_wr32(c + 0x8u, 1u);		/* M */
		gp_wr32(c + 0xcu, 0u);		/* N */
		gp_wr32(c + 0x10u, 0u);		/* D */
		cmd = gp_rd32(c);
		if (gp_fault)
			continue;
		gp_wr32(c, (cmd | 0x3u));	/* ROOT_EN + UPDATE */
		__asm__ volatile("dsb sy" ::: "memory");
	}

	for (i = 0; i < 4u; i++) {
		uintptr_t a = GPU_GPUCC_BASE + br[i];
		uint32_t v = gp_rd32(a);

		if (!gp_fault && !(v & 1u))
			gp_wr32(a, v | 1u);
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* Named v9 slots: the survey leaves these alone when it is off. */
	r->p_cb0_ttbr0 = gp_rd32(GPU_GPUCC_BASE + 0x93ecu);	/* hub CMD */
	r->p_cb0_ttbr1 = gp_rd32(GPU_GPUCC_BASE + 0x93f0u);	/* hub CFG */
	r->p_cb0_tcr = gp_rd32(GPU_GPUCC_BASE + 0x93f4u);	/* hub M */
	r->p_cb0_tcr2 = gp_rd32(GPU_GPUCC_BASE + 0x93f8u);	/* hub N */
	r->p_cb0_mair0 = gp_rd32(GPU_GPUCC_BASE + 0x93fcu);	/* hub D */
	r->p_cb0_mair1 = gp_rd32(GPU_GPUCC_BASE + 0x911cu);	/* ahb */
	r->p_cb0_cbar = gp_rd32(GPU_GPUCC_BASE + 0x9430u);	/* hub_ahb_div */
	r->p_cb0_cbf = gp_rd32(GPU_GPUCC_BASE + 0x9318u);	/* gmu CMD */
	con_hex2("GPUS", "hub=", r->p_cb0_ttbr0, "ahb=", r->p_cb0_mair1);
}
#endif

#if GP_GMU
/*
 * GMU liveness.  Before any firmware work: is the GMU register block
 * responding, and is GPU_CC_GMU_BCR (0x9314, bit 0 = BLK_ARES) released?
 * The CB page proved that an unclocked GPU block reads garbage instead of
 * faulting, so garbage here is an answer, not a crash.  Slots:
 *   slot= AO+0   cb-w= AO+4   cbregs-n= GMU+4   cb bank= GMU+8
 *   trans= GMU+0   fault= BCR before   restore= BCR after
 */
static void
gp_gmu_probe(volatile struct gpu_probe_result *r)
{
	uint32_t v;

	r->last_step = 300;
	r->w2cr_f = gp_rd32(GPU_GPUCC_BASE + 0x9314u);	/* GPU_CC_GMU_BCR */
	r->last_step = 301;
	if (!gp_fault && (r->w2cr_f & 1u)) {
		gp_wr32(GPU_GPUCC_BASE + 0x9314u, r->w2cr_f & ~1u);
		delay_ms(2);
	}
	r->w2cr_r = gp_rd32(GPU_GPUCC_BASE + 0x9314u);
#if GP_GMU == 4
	/*
	 * v20: the GPU's AHB path also has GCC-side gates that the vendor
	 * enables through the clock framework: gcc_qmip_gpu_ahb_clk
	 * (0x81008[0], hwcg bit1) and gcc_gpu_memnoc_gfx_clk (0x81010[0]).
	 * Open those, then look at gpu_cc_ahb and the GMU again.
	 */
	{
		static const uint32_t g[3] = { 0x181008u, 0x181010u, 0x162000u };
		unsigned i;

		for (i = 0; i < 3u; i++) {
			uint32_t v = gp_rd32(g[i]);

			if (!gp_fault) {
				gp_wr32(g[i], v | 1u);
				__asm__ volatile("dsb sy" ::: "memory");
			}
		}
		delay_ms(2);
	}
	r->p_cb0_sctlr = gp_rd32(GPU_GPUCC_BASE + 0x911cu);	/* ahb */
	r->last_step = 330;
	r->p_cb0_ttbr0 = gp_rd32(0x3d68000u + 0x1f800u);	/* CM3_SYSRESET */
	r->last_step = 331;
	r->p_cb0_ttbr1 = gp_rd32(0x3d68000u + 0x1f81cu);	/* FW_INIT */
	r->last_step = 332;
	r->p_cb0_tcr = gp_rd32(0x3d68000u + 0x1b400u);		/* ITCM[0] */
	con_hex2("GPUS", "ahb=", r->p_cb0_sctlr, "sysrst=", r->p_cb0_ttbr0);
#endif
#if GP_GMU == 3
	/*
	 * v19: raise the CX rail before touching the GMU.  Closing the CX GDSC
	 * is not the same as having a usable CX domain: the rail level comes
	 * from RPMh (cx.lvl), and Android's kgsl raises it while the GPU runs.
	 * Then read the GMU at the real gen7 offsets.
	 */
	r->p_cbw_before = (uint32_t)gp_rh_vote(0x30000u, 384u);	/* cx.lvl */
	delay_ms(10);
	r->last_step = 320;
	r->p_cb0_sctlr = gp_rd32(0x3d68000u + 0x1f800u);
	r->last_step = 321;
	r->p_cb0_ttbr0 = gp_rd32(0x3d68000u + 0x1f81cu);
	r->last_step = 322;
	r->p_cb0_ttbr1 = gp_rd32(0x3d68000u + 0x1f980u);
	r->last_step = 323;
	r->p_cb0_tcr = gp_rd32(0x3d68000u + 0x1f984u);
	r->p_cbw_flags |= 0x1000u;
	con_hex2("GPUS", "cxvote=", r->p_cbw_before, "sysrst=", r->p_cb0_sctlr);
#endif
#if GP_GMU == 2
	/*
	 * gmu_core_regwrite() goes through device->regmap, i.e. the kgsl
	 * register resource at 0x3d00000 -- NOT the gmu node (0x3d68000).
	 * gen7 offsets are word-relative to that base.
	 */
	r->last_step = 310;
	/*
	 * kgsl's regmap puts the gmu resource at word offset 0x1a000, so a
	 * gen7 offset O maps to 0x3d68000 + (O - 0x1a000) * 4.
	 */
	r->p_cb0_sctlr = gp_rd32(0x3d7e000u);	/* CM3_SYSRESET   0x1f800 */
	r->last_step = 311;
	r->p_cb0_ttbr0 = gp_rd32(0x3d7e070u);	/* FW_INIT_RESULT 0x1f81c */
	r->last_step = 312;
	r->p_cb0_ttbr1 = gp_rd32(0x3d7e600u);	/* HFI_CTRL_STATUS 0x1f980 */
	r->last_step = 313;
	r->p_cb0_tcr = gp_rd32(0x3d7e610u);	/* HFI ctrl word 0x1f984 */
	r->last_step = 314;
	r->p_cb0_tcr2 = gp_rd32(0x3d6d000u);	/* ITCM[0] 0x1b400 */
	r->last_step = 315;
	r->p_cb0_mair0 = gp_rd32(0x3d71000u);	/* DTCM[0] 0x1c400 */
	r->last_step = 316;
	r->p_cb0_mair1 = gp_rd32(0x3d00840u);	/* RBBM_STATUS */
	con_hex2("GPUS", "sysrst=", r->p_cb0_sctlr, "fwin=", r->p_cb0_ttbr0);
#endif
	r->last_step = 302;
	r->wsid = gp_rd32(0x3d40000u);			/* GMU AO block */
	r->last_step = 303;
	r->cbar_w = gp_rd32(0x3d40004u);
	r->last_step = 304;
	r->w2cr_t = gp_rd32(0x3d68000u);		/* GMU core */
	r->last_step = 305;
	r->cbar_count = gp_rd32(0x3d68004u);
	r->last_step = 306;
	r->cbar_bank = (int)gp_rd32(0x3d68008u);
	r->last_step = 307;
	r->cbfrsynra = gp_rd32(0xb290000u);		/* GMU PDC */
	r->last_step = 308;
	con_hex2("GPUS", "ao=", r->wsid, "gmu=", r->w2cr_t);
}
#endif

#if GP_BR
/*
 * Which GPUCC branch registers actually latch a write?  For each offset:
 * read v, write v|1, read back, restore v.  Bit n of the result means
 * offset[n] latched.  Slots: fault= latch mask  restore= 0x911c readback
 * after the attempt  trans= how many offsets were scanned.
 */
static void
gp_branch_scan(volatile struct gpu_probe_result *r)
{
	static const uint32_t offs[] = {
		0x911cu, 0x9120u, 0x9124u, 0x9128u, 0x912cu, 0x9130u,
		0x9134u, 0x9138u, 0x913cu, 0x9140u, 0x9144u, 0x9148u,
		0x914cu, 0x9150u, 0x9154u, 0x93e8u
	};
	uint32_t mask = 0, v, w;
	unsigned i;

	for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
		uintptr_t a = GPU_GPUCC_BASE + offs[i];

		r->last_step = 400 + i;
		v = gp_rd32(a);
		if (gp_fault)
			continue;
		if (v & 1u) {		/* already on: count it */
			mask |= 1u << i;
			continue;
		}
		gp_wr32(a, v | 1u);
		__asm__ volatile("dsb sy" ::: "memory");
		w = gp_rd32(a);
		if (!gp_fault && (w & 1u))
			mask |= 1u << i;
		gp_wr32(a, v);		/* restore */
	}
	__asm__ volatile("dsb sy" ::: "memory");
	r->w2cr_f = mask;
	r->w2cr_r = gp_rd32(GPU_GPUCC_BASE + 0x911cu);
	r->w2cr_t = (uint32_t)(sizeof(offs) / sizeof(offs[0]));
	con_hex2("GPUS", "branches=", mask, "ahb=", r->w2cr_r);
}
#endif

#if GP_VOTE
/*
 * The branches that refuse bit0 are the HALT_VOTED family.  Their enable is
 * a vote, and GPUCC keeps HLOS1 votes in the 0x7000 page.  Set bit0 on all
 * 64 words of that page (harmless: they are votes for GPU-internal clocks),
 * record which ones took, then re-read gpu_cc_ahb.
 */
static void
gp_vote_scan(volatile struct gpu_probe_result *r)
{
	uint32_t mask = 0;
	unsigned i;

	for (i = 0; i < 64u; i++) {
		uintptr_t a = GPU_GPUCC_BASE + 0x7000u + 4u * i;
		uint32_t v = gp_rd32(a);

		r->last_step = 500 + i;
		if (gp_fault)
			continue;
		if (v & 1u) {
			mask |= 1u << (i & 31u);
			continue;
		}
		gp_wr32(a, v | 1u);
		__asm__ volatile("dsb sy" ::: "memory");
		if (!gp_fault && (gp_rd32(a) & 1u))
			mask |= 1u << (i & 31u);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* also try the ahb branch once more after the votes */
	{
		uintptr_t a = GPU_GPUCC_BASE + 0x911cu;
		uint32_t v = gp_rd32(a);

		if (!gp_fault)
			gp_wr32(a, v | 1u);
	}
	r->p_cb0_sctlr = mask;
	r->p_cb0_ttbr0 = gp_rd32(GPU_GPUCC_BASE + 0x911cu);
	r->p_cb0_ttbr1 = gp_rd32(GPU_GPUCC_BASE + 0x9430u);
	con_hex2("GPUS", "votes=", mask, "ahb=", r->p_cb0_ttbr0);
}
#endif

#if GP_RSCC
/*
 * Replay gen7_load_rsc_ucode() + gen7_load_pdc_ucode() (adreno_gen7_gmu.c)
 * with the register numbers from gen7_reg.h.  RSCC register numbers are
 * word offsets; RSCC lives at 0x3d50000 and the PDC at 0xb290000.
 * These are the GPU's own sleep/wake sequencer and its PDC handshake; the
 * GMU is expected to become reachable once they are in place.
 */
static void
gp_rscc_load(volatile struct gpu_probe_result *r)
{
	uintptr_t rs = 0x3d50000UL;
	uintptr_t pd = 0xb290000UL;
	static const uint32_t seq[5] = {
		0xeaaae5a0u, 0xe1a1ebabu, 0xa2e0a581u, 0xecac82e2u, 0x0020edadu
	};
	unsigned i;

	r->last_step = 600;
	gp_wr32(pd + 0x4520u, 0u);		/* PDC GPU SEQ_START_ADDR */
	r->last_step = 601;
	gp_wr32(pd + 0x4500u, 0x80000001u);	/* PDC GPU ENABLE_PDC */

	r->last_step = 602;
	gp_wr32(rs + 0x10u, 1u << 24);		/* RSC_STATUS0: no SDE gating */
	r->last_step = 603;
	gp_wr32(rs + 0x2cu, 1u);		/* PDC slave id */
	gp_wr32(rs + 0x38u, 0u);
	gp_wr32(rs + 0x34u, 0u);
	gp_wr32(rs + 0x40u, 0u);
	gp_wr32(rs + 0x3cu, 0u);
	gp_wr32(rs + 0x48u, 0x80000000u);
	gp_wr32(rs + 0x44u, 0u);
	gp_wr32(rs + 0x400u, 0u);		/* override start */
	gp_wr32(rs + 0x20u, 0x4520u);		/* PDC seq start */
	gp_wr32(rs + 0x24u, 0x4510u);
	gp_wr32(rs + 0x28u, 0x4514u);
	r->last_step = 604;
	for (i = 0; i < 5u; i++)
		gp_wr32(rs + 0x600u + 4u * i, seq[i]);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(2);

	r->last_step = 605;
	r->p_cb0_sctlr = gp_rd32(rs + 0x20u);		/* RSCC seq start rb */
	r->p_cb0_ttbr0 = gp_rd32(pd + 0x4500u);		/* PDC enable rb */
	r->p_cb0_ttbr1 = gp_rd32(0x3d68000u + 0x1f800u);	/* CM3_SYSRESET */
	r->p_cb0_tcr = gp_rd32(GPU_GPUCC_BASE + 0x911cu);	/* ahb */
	con_hex2("GPUS", "rscc=", r->p_cb0_sctlr, "gmu=", r->p_cb0_ttbr1);
}
#endif

#if GP_GMUBOOT
/* GMU register addresses: kgsl base 0x3d00000, gmu region starts at word
 * 0x1a000, so byte = 0x3d68000 + (word - 0x1a000) * 4. */
#define GMU_ITCM_W	0x3d6d000UL	/* 0x1b400 */
#define GMU_DTCM_W	0x3d71000UL	/* 0x1c400 */
#define GMU_SYSRESET	0x3d7e000UL	/* 0x1f800 */
#define GMU_FW_INIT	0x3d7e070UL	/* 0x1f81c */
#define GMU_HFI_STATUS	0x3d7e600UL	/* 0x1f980 */
#define GMU_HFI_INIT	0x3d7e618UL	/* 0x1f986 */

/* GMU cache block lands here (64 KiB, page aligned, identity mapped). */
static uint8_t gmu_cache[0x10000] __attribute__((aligned(4096)));
static uint8_t gmu_hfi[0x10000] __attribute__((aligned(4096)));
static uint64_t gmu_l1[512] __attribute__((aligned(4096)));
static uint64_t gmu_l2[512] __attribute__((aligned(4096)));
static uint64_t gmu_l3[512] __attribute__((aligned(4096)));
static uint64_t gmu_h2[512] __attribute__((aligned(4096)));
static uint64_t gmu_h3[512] __attribute__((aligned(4096)));

/*
 * Map GMU VA 0x4000..0x14000 (the firmware's cache block) through context
 * bank 1 for stream 5.  VA 0..0x4000 and 0x14000.. are deliberately left
 * unmapped: the GMU fetches its reset vector from internal ITCM, which is
 * not behind this SMMU.
 */
static void
gp_gmu_iommu(volatile struct gpu_probe_result *r)
{
	uintptr_t s = GPU_SMMU_BASE;
	uintptr_t cb = GPU_SMMU_BASE + 0x21000u;	/* CB1 */
	uintptr_t l1 = (uintptr_t)gmu_l1;
	uintptr_t l2 = (uintptr_t)gmu_l2;
	uintptr_t l3 = (uintptr_t)gmu_l3;
	uintptr_t buf = (uintptr_t)gmu_cache;
	uintptr_t p;
	unsigned i;

	for (i = 0; i < 512u; i++) {
		gmu_l1[i] = 0;
		gmu_l2[i] = 0;
		gmu_l3[i] = 0;
	}
	gmu_l1[0] = (uint64_t)l2 | 3u;		/* L1[0] -> L2 table */
	gmu_l2[0] = (uint64_t)l3 | 3u;		/* L2[0] -> L3 table */
	for (i = 4; i < 20u; i++)		/* VA 0x4000..0x14000 */
		gmu_l3[i] = (uint64_t)(buf + (i - 4u) * 0x1000u) | 0x703u;

	/*
	 * HFI queue table lives in GMU_VA 0x60000000 (the vendor's
	 * "GMU_NONCACHED_KERNEL" window).  L1 index 1 covers 0x40000000..
	 * 0x7fffffff, L2 index 0 gives 0x60000000..0x601fffff.
	 */
	for (i = 0; i < 512u; i++) {
		gmu_h2[i] = 0;
		gmu_h3[i] = 0;
	}
	gmu_l1[1] = (uint64_t)(uintptr_t)gmu_h2 | 3u;
	gmu_h2[0] = (uint64_t)(uintptr_t)gmu_h3 | 3u;
	for (i = 0; i < 16u; i++)
		gmu_h3[i] = (uint64_t)((uintptr_t)gmu_hfi + i * 0x1000u) | 0x703u;

	for (p = l1; p < l1 + 0x5000u; p += 64u)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");

	r->last_step = 700;
	gp_wr32(cb + 0x00u, 0u);		/* disable CB1 */
	gp_wr32(s + 0x1004u, 0x1f000u);		/* CBAR(1) */
	gp_wr32(s + 0x1804u, 1u);		/* CBA2R(1) */
	gp_wr32(cb + 0x20u, (uint32_t)l1);
	gp_wr32(cb + 0x24u, (uint32_t)((uint64_t)l1 >> 32));
	gp_wr32(cb + 0x10u, 0u);		/* TCR2 */
	gp_wr32(cb + 0x30u, 0x3520u);		/* TCR */
	gp_wr32(cb + 0x38u, 0xffu);		/* MAIR0 */
	gp_wr32(cb + 0x00u, 0x1u);		/* enable */
	r->last_step = 701;
	gp_wr32(s + 0x804u, 0x80000005u);	/* SMR(1) = SID 5 */
	gp_wr32(s + 0xc04u, 0x1u);		/* S2CR(1) = TRANS, CB1 */
	__asm__ volatile("dsb sy" ::: "memory");

	/*
	 * kgsl flushes the TLB before every GMU boot: stale translations from
	 * a previous owner of this SMMU would send the CM3's first fetches to
	 * the wrong physical pages.
	 */
	r->last_step = 702;
	gp_wr32(s + 0x68u, 0u);			/* TLBIALLNSNH */
	gp_wr32(s + 0x6cu, 0u);			/* TLBIALLH */
	gp_wr32(s + 0x70u, 0u);			/* sTLBGSYNC */
	for (i = 0; i < 100u; i++) {
		if (!(gp_rd32(s + 0x74u) & 1u))
			break;
		delay_ms(1);
	}

#if GP_GMUBOOT == 4
	/* Readback of the whole SID-5 wiring, nothing else runs. */
	r->p_cb0_sctlr = gp_rd32(cb + 0x00u);	/* CB1.SCTLR */
	r->p_cb0_ttbr0 = gp_rd32(cb + 0x20u);	/* TTBR0 */
	r->p_cb0_ttbr1 = gp_rd32(cb + 0x30u);	/* TCR */
	r->p_cb0_tcr = gp_rd32(s + 0x804u);	/* SMR(1) */
	r->p_cb0_tcr2 = gp_rd32(s + 0xc04u);	/* S2CR(1) */
	r->p_cb0_mair0 = gp_rd32(s + 0x1004u);	/* CBAR(1) */
	r->p_cb0_mair1 = gp_rd32(s + 0x1804u);	/* CBA2R(1) */
#endif
}

static int
gp_fw_magic(const uint8_t *p)
{
	return p[0] == 0xde && p[1] == 0xad && p[2] == 0xbe && p[3] == 0xef &&
	    p[4] == 0xde && p[5] == 0xad && p[6] == 0xbe && p[7] == 0xef;
}

static uint32_t
gp_ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
gp_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	while (n--)
		*dst++ = *src++;
}

/*
 * HFI queue table (adreno_hfi.h / init_queues): a 24-byte table header, three
 * 48-byte queue headers, then 4 KiB per queue starting at ALIGN(168,16)=176.
 * The CM3 reads this table as soon as it starts.
 */
static void
gp_hfi_init(void)
{
	static const uint32_t qtype[3] = { 0x0A00u, 0x0A01u, 0x2802u };
	uint32_t *t = (uint32_t *)gmu_hfi;
	unsigned i;

	t[0] = 0;		/* version */
	t[1] = 42;		/* table size in dwords */
	t[2] = 6;		/* qhdr0 offset */
	t[3] = 12;		/* qhdr size in dwords */
	t[4] = 3;		/* num_q */
	t[5] = 3;		/* num_active_q */

	for (i = 0; i < 3u; i++) {
		uint32_t *h = &t[6u + i * 12u];

		h[0] = 1;				/* enabled */
		h[1] = 0x60000000u + 176u + i * 4096u;	/* GMU VA */
		h[2] = qtype[i];
		h[3] = 1024u;				/* dwords */
		h[4] = 0;
		h[10] = 0;				/* read index */
		h[11] = 0;				/* write index */
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < 16u; i++)
		__asm__ volatile("dc civac, %0" ::
		    "r"((uintptr_t)gmu_hfi + i * 0x1000u) : "memory");
}

static void
gp_gmu_boot(volatile struct gpu_probe_result *r)
{
	/*
	 * Firmware format: 16-byte file header, then blocks of
	 *   { u32 addr; u32 size; u32 0xdeadbeef; u32 0xdeadbeef; data[size] }
	 * The magic is stored big-endian.  The vendor's bulk writes confirm the
	 * three TCM blocks; the 0xdcac-byte block at 0x4000 goes to GMU memory
	 * through the SMMU, not through a register window.
	 */
	static const uint8_t magic[8] = {
		0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef
	};
	const uint8_t *base = gmu_fw_start;
	const uint8_t *end = gmu_fw_end;
	const uint8_t *p = base;
	uint32_t v;
	unsigned k;

	while (p + 16 <= end) {
		uint32_t addr, size;
		unsigned i;

		(void)magic;
		if (!gp_fw_magic(p + 8)) {
			p += 4;
			continue;
		}
		addr = gp_ld32(p);
		size = gp_ld32(p + 4);
		p += 16;
		if (size == 0 || (uint32_t)(end - p) < size)
			break;
		if (addr < 0x4000u) {			/* ITCM */
			for (i = 0; i < size; i += 4u) {
				uint32_t w;

				w = gp_ld32(p + i);
				gp_wr32(GMU_ITCM_W + addr + i, w);
			}
			r->last_step = 710;
		} else if (addr >= 0x10000000u) {	/* DTCM */
			uintptr_t base_pa = GMU_DTCM_W +
			    (addr - 0x10004000u);

			for (i = 0; i < size; i += 4u) {
				uint32_t w;

				w = gp_ld32(p + i);
				gp_wr32(base_pa + i, w);
			}
			r->last_step = 711;
		} else {				/* GMU cache */
			gp_copy(gmu_cache + (addr - 0x4000u), p, size);
			r->last_step = 712;
		}
		p += size;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	gp_gmu_iommu(r);

	/* Readbacks: did the TCM windows actually take the firmware? */
	r->p_cb0_sctlr = gp_rd32(GMU_ITCM_W);
	r->p_cb0_ttbr0 = gp_rd32(GMU_ITCM_W + 4u);
	r->p_cb0_ttbr1 = gp_rd32(GMU_DTCM_W + (0x10007f98u - 0x10004000u));

	/*
	 * Register configuration the vendor applies before releasing the CM3,
	 * in the exact order captured from kgsl_regmap_write.
	 */
	{
		static const struct { uint32_t w, v; } cfg[] = {
			{ 0x1f8f1u, 0x1u }, { 0x1f8f0u, 0x1u },
			{ 0x1f81cu, 0x0u }, { 0x1f801u, 0x2u },
			{ 0x1f985u, 0x60006000u }, { 0x1f984u, 0x1u },
			{ 0x1f82du, 0x4052u },
			{ 0x1f9ceu, 0x0u }, { 0x1f9cfu, 0x7030200u },
			{ 0x1f9cdu, 0x60002003u },
			{ 0x1f40fu, 0x1u }, { 0x1f400u, 0x1u },
			{ 0x1f401u, 0x1u },
			{ 0x1f991u, 0xffffffffu }, { 0x1f993u, 0xffff7ff9u },
		};
		unsigned k;

		r->last_step = 715;
		for (k = 0; k < sizeof(cfg) / sizeof(cfg[0]); k++)
			gp_wr32(0x3d68000UL +
			    ((uintptr_t)(cfg[k].w - 0x1a000u) << 2), cfg[k].v);
		/* TCM retention on, GBIF halt released (gen7_gmu_register_config) */
		gp_wr32(0x3d9e000u + 0x39u * 4u, 1u);
		gp_wr32(0x3d00000u + 0x3c45u * 4u, 0u);
		/* HFI queue table: our buffer at GMU_VA 0x60000000 */
		gp_hfi_init();
		gp_wr32(GMU_HFI_INIT - 4u, 0x60000000u);	/* QTBL_ADDR */
		gp_wr32(GMU_HFI_INIT - 8u, 1u);			/* QTBL_INFO */
		gp_wr32(0x3d68000UL + ((uintptr_t)(0x23b11u - 0x1a000u) << 2),
		    0x80c808a0u);				/* AHB_FENCE_RANGE_0 */
		__asm__ volatile("dsb sy" ::: "memory");
	}

	r->last_step = 720;
	r->p_kgsl_ident = gp_rd32(GMU_ITCM_W);		/* ITCM[0] echo */
#if GP_GMUBOOT == 1
	gp_wr32(GMU_SYSRESET, 0u);			/* release CM3 */
#endif
#if GP_GMUBOOT == 9
	/* Toggle the CM3 reset with no time to execute, then hand off. */
	gp_wr32(GMU_SYSRESET, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
	gp_wr32(GMU_SYSRESET, 1u);
	__asm__ volatile("dsb sy" ::: "memory");
#endif
#if GP_GMUBOOT == 8
	/* Release the CM3, let it run, then put it back in reset. */
	gp_wr32(GMU_SYSRESET, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(200);
	gp_wr32(GMU_SYSRESET, 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(10);
#endif
#if GP_GMUBOOT == 7
	/*
	 * Mask the GMU's two SPIs (DT irqs 0x130/0x131) and make them
	 * edge-triggered before handing off: a level-asserted GMU interrupt
	 * would otherwise storm XNU's GIC during early boot.
	 */
	gp_wr32(GICD_BASE + 0x180u + 9u * 4u, (1u << 16) | (1u << 17));
	gp_wr32(GICD_BASE + 0x280u + 9u * 4u, (1u << 16) | (1u << 17));
	gp_wr32(GICD_BASE + 0xc44u, (gp_rd32(GICD_BASE + 0xc44u) &
	    0xfffffff0u) | 0xau);		/* IRQ 304/305 -> edge */
	__asm__ volatile("dsb sy" ::: "memory");
	/* Release the CM3 and hand off to XNU without touching anything else. */
	gp_wr32(GMU_SYSRESET, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(200);
#endif
#if GP_GMUBOOT == 6
	/*
	 * Release the CM3 but never touch a GMU register afterwards: the AP
	 * loses the AHB once the CM3 runs, but the HFI queue table lives in
	 * our own DRAM.  If the GMU is alive it will write a message there.
	 */
	gp_wr32(GMU_SYSRESET, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(200);
	{
		volatile uint32_t *t = (volatile uint32_t *)gmu_hfi;

		r->p_cb0_sctlr = t[18 + 10];	/* msg queue read index */
		r->p_cb0_ttbr0 = t[18 + 11];	/* msg queue write index */
		r->p_cb0_ttbr1 = ((volatile uint32_t *)(gmu_hfi + 176 + 4096))[0];
		r->p_cb0_tcr = ((volatile uint32_t *)(gmu_hfi + 176 + 4096))[1];
		r->p_cb0_tcr2 = ((volatile uint32_t *)(gmu_hfi + 176 + 4096))[2];
		r->p_cb0_mair0 = ((volatile uint32_t *)(gmu_hfi + 176))[0];
	}
	con_hex2("GPUS", "msgr=", r->p_cb0_sctlr, "msgw=", r->p_cb0_ttbr0);
#endif
#if GP_GMUBOOT == 5
	/* Release, wait, then reset the SoC without touching GMU registers. */
	gp_wr32(GMU_SYSRESET, 0u);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(50);
	{
		register uint64_t x0 __asm("x0") = 0x84000009ull;

		__asm volatile("dsb sy" ::: "memory");
		__asm volatile("smc #0" : "+r"(x0) ::
		    "x1", "x2", "x3", "memory");
	}
	for (;;)
		delay_ms(1000);
#endif
	__asm__ volatile("dsb sy" ::: "memory");
	for (k = 0; k < 2000u; k++) {
		v = gp_rd32(GMU_FW_INIT);
		if (gp_fault)
			break;
		if (v & (1u << 8))
			break;
		delay_ms(1);
	}
	r->p_kgsl_rbbm = v;
	r->last_step = 721;
#if GP_GMUBOOT != 2
	if (v & (1u << 8)) {
		gp_wr32(GMU_HFI_INIT, 1u);
		__asm__ volatile("dsb sy" ::: "memory");
		for (k = 0; k < 200u; k++) {
			v = gp_rd32(GMU_HFI_STATUS);
			if (gp_fault || (v & 1u))
				break;
			delay_ms(1);
		}
		r->p_cbw_before = v;		/* HFI status */
	}
#endif
	r->p_cbw_flags |= 0x2000u;
	con_hex2("GPUS", "fwin=", r->p_kgsl_rbbm, "hfi=", r->p_cbw_before);
}
#endif

#if GP_TCM
/* Minimal test: write a pattern into TCM word 0 and read it back. */
static void
gp_tcm_test(volatile struct gpu_probe_result *r)
{
	uint32_t a0 = gp_rd32(0x3d6d000u);
	uint32_t b0 = gp_rd32(0x3d71000u);
	uint32_t a1, b1;

	r->p_cb0_sctlr = a0;	/* ITCM[0] before */
	r->p_cb0_ttbr0 = b0;	/* DTCM[0] before */
	r->last_step = 800;
	gp_wr32(0x3d6d000u, 0xdeadbeefu);
	gp_wr32(0x3d71000u, 0xcafebabeu);
	__asm__ volatile("dsb sy" ::: "memory");
	r->last_step = 801;
	a1 = gp_rd32(0x3d6d000u);
	b1 = gp_rd32(0x3d71000u);
	r->p_cb0_ttbr1 = a1;	/* ITCM[0] after */
	r->p_cb0_tcr = b1;	/* DTCM[0] after */
	con_hex2("GPUS", "itcm=", a1, "dtcm=", b1);
}
#endif

extern const uint8_t gmu_fw_start[], gmu_fw_end[];

#if GP_STAGE
/*
 * Stage everything the kernel-side GMU boot needs at fixed physical
 * addresses just past the result page (outside the ADT bank, so XNU will
 * not allocate it):
 *   0xa2e10000  firmware image (copy of gmu_gen70000.bin)
 *   0xa2e20000  GMU cache block   (64 KiB)
 *   0xa2e30000  HFI queue table   (64 KiB)
 *   0xa2e40000  page tables      (16 KiB)
 * A small header at 0xa2e0ff00 tells the kernel it is staged.
 */
static void
gp_stage_gmu(volatile struct gpu_probe_result *r)
{
	uint8_t *fw = (uint8_t *)0xa2e10000UL;
	uint8_t *bufs = (uint8_t *)0xa2e20000UL;
	uint32_t size = (uint32_t)(gmu_fw_end - gmu_fw_start);
	uint32_t i;

	for (i = 0; i < size; i++)
		fw[i] = gmu_fw_start[i];
	for (i = 0; i < 0x30000u; i++)	/* cache + hfi + tables */
		bufs[i] = 0;
	{
		volatile uint32_t *hdr = (volatile uint32_t *)0xa2e0ff00UL;

		hdr[0] = 0x474d5546u;	/* "GMUF" */
		hdr[1] = size;
		hdr[2] = 1;		/* version */
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < size; i += 64)
		__asm__ volatile("dc civac, %0" :: "r"((uintptr_t)(fw + i)) : "memory");
	r->p_cbw_flags |= 0x4000u;
	con_hex2("GPUS", "staged=", size, "at=", 0xa2e10000u);
}
#endif

/* v11: stage-1 tables, identity mapped (the shim is VA==PA). */
static uint64_t gp_cbw_l1[512] __attribute__((aligned(4096)));
static uint64_t gp_cbw_l2[512] __attribute__((aligned(4096)));

static void
gpu_probe_boot(void)
{
	volatile struct gpu_probe_result *r =
	    (volatile struct gpu_probe_result *)GPU_PROBE_PA;
	uintptr_t s = GPU_SMMU_BASE;
	uint32_t id0, id1, num_smr, num_cb, smr, s2cr;
	uint32_t rf = 0, wf = 0;
	uint32_t prev_magic = r->magic, prev_step = r->last_step;
	int slot0 = -1, free_slot = -1;
	unsigned i;

	for (i = 0; i < sizeof(*r); i++)
		((volatile uint8_t *)r)[i] = 0;
	r->prev_magic = prev_magic;
	r->prev_step = prev_step;
	r->version = GPU_PROBE_LEVEL;
#if GP_SURVEY
	r->version = 8;		/* read-only CB + GPUCC survey */
#endif
	r->sid0_slot = 0xffffffffu;
	r->wsid = 0xffffffffu;
	r->cbar_bank = 0xffffffffu;
	r->sctlr_bank = 0xffffffffu;

	/* gp_fault is cleared by every access, so latch it after each one. */
	r->scr0 = gp_rd32(s + 0x00);
	if (gp_fault && !rf) {
		rf = 1;
		r->far = gp_far;
		r->esr = gp_esr;
	}
	r->id0 = gp_rd32(s + 0x20);
	if (gp_fault && !rf) {
		rf = 1;
		r->far = gp_far;
		r->esr = gp_esr;
	}
	r->id1 = gp_rd32(s + 0x24);
	if (gp_fault && !rf) {
		rf = 1;
		r->far = gp_far;
		r->esr = gp_esr;
	}
	r->id2 = gp_rd32(s + 0x28);
	if (gp_fault && !rf) {
		rf = 1;
		r->far = gp_far;
		r->esr = gp_esr;
	}
	r->rfault = rf;
	con_hex2("GPU", "id0=", r->id0, "id1=", r->id1);
	con_hex2("GPU", "scr=", r->scr0, "rflt=", r->rfault);
	if (rf)
		goto done;

#if GP_EARLY
	/* Power first, then let the survey read the live block. */
	gp_power_cx(r);
#if GP_CLK
	gp_clk_setup(r);
#endif
#if GP_GMU
	gp_gmu_probe(r);
#endif
#if GP_BR
	gp_branch_scan(r);
#endif
#if GP_VOTE
	gp_vote_scan(r);
#endif
#if GP_RSCC
	gp_rscc_load(r);
#endif
#if GP_GMUBOOT
	gp_gmu_boot(r);
#endif
#if GP_TCM
	gp_tcm_test(r);
#endif
#if GP_GX
	gp_power_gx(r);
#endif
	con_hex("GPUS", 5);
#endif

#if GP_STAGE
	/* Runs regardless of GP_EARLY: the kernel does the hardware work. */
	gp_stage_gmu(r);
#endif
#if GP_SMPREL
	con_hex2("SMREL", "cpu7=", (uint32_t)cpu_mpidr(7),
	    "ret=", (uint32_t)smc(PSCI_CPU_ON, cpu_mpidr(7),
	    (uintptr_t)secondary_entry, 7));
#endif

	id0 = r->id0;
	id1 = r->id1;
	num_smr = id0 & 0xffu;		/* IDR0[7:0]  NUMSMRG   */
	num_cb = id1 & 0xffu;		/* IDR1[7:0]  NUMCB     */
	if (num_smr > 32)
		num_smr = 32;
	r->num_smr = num_smr;
	r->num_cb = num_cb;
	r->rfault = 0;
	r->far = 0;
	r->esr = 0;

	/* SMR/S2CR are at the same offsets on every SMMU-500 instance. */
	for (i = 0; i < num_smr; i++) {
		smr = gp_rd32(s + 0x800 + 4 * i);
		if (gp_fault)
			goto readfault;
		s2cr = gp_rd32(s + 0xc00 + 4 * i);
		if (gp_fault)
			goto readfault;
		if (!(smr & 0x80000000u)) {
			if (free_slot < 0)
				free_slot = (int)i;
			continue;
		}
		r->valid_seen++;
		if (((smr >> 16) & 0x7fffu) == 0 && slot0 < 0) {
			slot0 = (int)i;
			r->sid0_slot = (uint32_t)i;
			r->smr0 = smr;
			r->s2cr0 = s2cr;
		}
	}

	if (GPU_PROBE_LEVEL >= 2) {
		int w = (slot0 >= 0) ? slot0 : free_slot;

		if (w >= 0) {
			uint32_t old = gp_rd32(s + 0xc00 + 4 * w);

			r->wsid = (uint32_t)w;
			r->w2cr_old = old;
			wf |= (uint32_t)gp_fault;
			if (gp_fault)		/* cannot own what we cannot read */
				goto wdone;
			if (slot0 < 0) {
				gp_wr32(s + 0x800 + 4 * w, 0x80000000u);
				wf |= (uint32_t)gp_fault;
				r->wsmr = gp_rd32(s + 0x800 + 4 * w);
				wf |= (uint32_t)gp_fault;
			}
			gp_wr32(s + 0xc00 + 4 * w, 0x00010000u);	/* BYPASS */
			wf |= (uint32_t)gp_fault;
			r->w2cr = gp_rd32(s + 0xc00 + 4 * w);
			wf |= (uint32_t)gp_fault;
			gp_wr32(s + 0xc00 + 4 * w, 0x00020000u);	/* FAULT */
			wf |= (uint32_t)gp_fault;
			r->w2cr_f = gp_rd32(s + 0xc00 + 4 * w);
			wf |= (uint32_t)gp_fault;
			gp_wr32(s + 0xc00 + 4 * w, old);		/* restore */
			wf |= (uint32_t)gp_fault;
			r->w2cr_r = gp_rd32(s + 0xc00 + 4 * w);
			wf |= (uint32_t)gp_fault;

			/*
			 * Level 5: bypass is refused, but the GPU does not use
			 * bypass - it uses stage-1 translation.  Does a TRANS
			 * S2CR stick?  Only this one register: CBAR/SCTLR are
			 * a separate question and an SError there would take
			 * the whole boot with it.
			 */
			if (GPU_PROBE_LEVEL >= 5 && !wf) {
				gp_wr32(s + 0xc00 + 4 * w, 0x00000001u);  /* TRANS, CB0 */
				wf |= (uint32_t)gp_fault;
				r->w2cr_t = gp_rd32(s + 0xc00 + 4 * w);
				wf |= (uint32_t)gp_fault;
				gp_wr32(s + 0xc00 + 4 * w, old);	  /* restore */
				wf |= (uint32_t)gp_fault;
			}
			if (slot0 < 0) {
				/* Remove the SMR entry this probe created. */
				gp_wr32(s + 0x800 + 4 * w, 0);
				wf |= (uint32_t)gp_fault;
			}
wdone:
			r->wfault = wf;
		}
	}

	/*
	 * Optional register matrix.  Each block is a separate build so one
	 * fatal access does not hide the answer to the others.  last_step is
	 * written before every access, so a boot that dies leaves the step in
	 * DRAM for the next variant to report as prev_step (if DRAM survives).
	 */
	r->op_fault = 0;

#if GP_KGSL_SCAN
	r->last_step = 20;
	r->kg0 = gp_rd32(GPU_KGSL_BASE + 0x0000u);
	if (gp_fault)
		r->op_fault |= 1u;
	r->last_step = 21;
	r->kg1 = gp_rd32(GPU_KGSL_BASE + 0x0840u);	/* RBBM_STATUS */
	if (gp_fault)
		r->op_fault |= 2u;
	r->last_step = 22;
	r->kg2 = gp_rd32(GPU_KGSL_BASE + 0x1000u);
	if (gp_fault)
		r->op_fault |= 4u;
	r->last_step = 23;
	r->kg3 = gp_rd32(GPU_KGSL_BASE + 0x2000u);
	if (gp_fault)
		r->op_fault |= 8u;
#endif

#if GP_GR1_READ
	r->last_step = 30;
	r->cba2r = gp_rd32(s + 0x1800u);		/* GR1+0x800 */
	if (gp_fault)
		r->op_fault |= 0x10u;
	r->last_step = 31;
	r->cbfrsynra = gp_rd32(s + 0x1400u);		/* GR1+0x400 */
	if (gp_fault)
		r->op_fault |= 0x20u;
#endif

#if GP_CBAR_COUNT > 0
	r->last_step = 40;
	r->cbar_count = GP_CBAR_COUNT;
	r->cbar_r = gp_rd32(s + 0x1000u);
	if (gp_fault)
		r->op_fault |= 0x40u;
#if GP_CBAR_COUNT > 1
	r->last_step = 41;
	r->cbar1 = gp_rd32(s + 0x1004u);
	if (gp_fault)
		r->op_fault |= 0x40u;
#endif
#if GP_CBAR_COUNT > 2
	r->last_step = 42;
	r->cbar2 = gp_rd32(s + 0x1008u);
	if (gp_fault)
		r->op_fault |= 0x40u;
#endif
#if GP_CBAR_COUNT > 3
	r->last_step = 43;
	r->cbar3 = gp_rd32(s + 0x100cu);
	if (gp_fault)
		r->op_fault |= 0x40u;
#endif
#endif

#if GP_CBAR_BANK >= 0
	r->cbar_bank = GP_CBAR_BANK;
	r->last_step = 50;
	r->cbar_r = gp_rd32(s + 0x1000u + 4u * GP_CBAR_BANK);
	if (gp_fault)
		r->op_fault |= 0x80u;
#if GP_CBAR_WRITE
	r->last_step = 51;
	gp_wr32(s + 0x1000u + 4u * GP_CBAR_BANK,
	    (GP_CBAR_WRITE == 1) ? 0u : 0x10000u);
	if (gp_fault)
		r->op_fault |= 0x100u;
	r->last_step = 52;
	r->cbar_w = gp_rd32(s + 0x1000u + 4u * GP_CBAR_BANK);
	if (gp_fault)
		r->op_fault |= 0x200u;
#endif
#endif

#if GP_SCTLR_BANK >= 0
	r->sctlr_bank = GP_SCTLR_BANK;
	r->last_step = 60;
	r->sctlr_r = gp_rd32(s + 0x80000u + 0x1000u * GP_SCTLR_BANK);
	if (gp_fault)
		r->op_fault |= 0x400u;
#if GP_SCTLR_WRITE
	r->last_step = 61;
	gp_wr32(s + 0x80000u + 0x1000u * GP_SCTLR_BANK, 0u);
	if (gp_fault)
		r->op_fault |= 0x800u;
	r->last_step = 62;
	r->sctlr_w = gp_rd32(s + 0x80000u + 0x1000u * GP_SCTLR_BANK);
	if (gp_fault)
		r->op_fault |= 0x1000u;
#endif
#endif

#if GP_CB_REGS
	r->last_step = 70;
	r->ttbr0 = gp_rd32(s + 0x80020u);
	if (gp_fault)
		r->op_fault |= 0x2000u;
	r->last_step = 71;
	r->tcr = gp_rd32(s + 0x80030u);
	if (gp_fault)
		r->op_fault |= 0x4000u;
	r->last_step = 72;
	r->mair = gp_rd32(s + 0x80038u);
	if (gp_fault)
		r->op_fault |= 0x8000u;
#endif

#if GP_CMDB
	/*
	 * v12: the GX rail needs an RPMh vote, and a vote needs the resource
	 * address from the AOP cmd-db.  Parse it the way sweet_rpmh.c does:
	 * 8 slave descriptors at 0x8 + 16i (u16 slave, u16 entry offset,
	 * u16 aux offset, u16 count), entries of 24 bytes after the 144-byte
	 * header, resource address at +16.  Names are 8 bytes.
	 * Old slots: slot= cx.lvl  old= gfx.lvl  smr= mx.lvl  byp= mxc.lvl
	 *            fault= magic  restore= entries walked
	 */
	{
		uintptr_t db = 0x80860000UL;	/* aop_cmd_db_region */
		uint32_t magic = gp_rd32(db + 4u);
		uint32_t found[4] = { 0, 0, 0, 0 };
		unsigned h, j, walked = 0, n = 0;

		r->w2cr_f = magic;
		r->last_step = 240;
		if (magic == 0x0c0330dbu) {
			for (h = 0; h < 8u; h++) {
				uint32_t hd = db + 8u + 16u * h;
				uint32_t w0 = gp_rd32(hd);
				uint32_t w1 = gp_rd32(hd + 4u);
				uint32_t slave = w0 & 0xffffu;
				uint32_t eoff = w0 >> 16;
				uint32_t cnt = w1 >> 16;

				if (!slave || cnt > 512u)
					break;
				for (j = 0; j < cnt; j++) {
					uint32_t e = db + 144u + eoff + 24u * j;
					uint32_t k0 = gp_rd32(e);
					uint32_t k1 = gp_rd32(e + 4u);

					if (gp_fault)
						goto cmdb_done;
					walked++;
#if GP_CMDB == 2
					/*
					 * Look for clock-like RPMh resources: names are at
					 * most 8 bytes, so "gpu_ahb"/"gfx_ahb" fit but
					 * "gpu_cc_ahb" does not.  Keep up to four in
					 * named fields the rest of the probe never writes.
					 */
					if (((k0 & 0xffffffu) == 0x757067u ||
					     (k0 & 0xffffffu) == 0x786667u) && n < 4u) {
						uint32_t addr = gp_rd32(e + 16u);

						switch (n) {
						case 0:
							r->p_cb0_sctlr = k0;
							r->p_cb0_ttbr0 = addr;
							break;
						case 1:
							r->p_cb0_ttbr1 = k0;
							r->p_cb0_tcr = addr;
							break;
						case 2:
							r->p_cb0_tcr2 = k0;
							r->p_cb0_mair0 = addr;
							break;
						default:
							r->p_cb0_mair1 = k0;
							r->p_cb0_cbar = addr;
							break;
						}
						n++;
					}
#endif
					if (k0 == 0x6c2e7863u && k1 == 0x00006c76u)
						found[0] = gp_rd32(e + 16u); /* cx.lvl */
					else if (k0 == 0x2e786667u && k1 == 0x006c766cu)
						found[1] = gp_rd32(e + 16u); /* gfx.lvl */
					else if (k0 == 0x6c2e786du && k1 == 0x00006c76u)
						found[2] = gp_rd32(e + 16u); /* mx.lvl */
					else if (k0 == 0x2e63786du && k1 == 0x006c766cu)
						found[3] = gp_rd32(e + 16u); /* mxc.lvl */
				}
			}
		}
cmdb_done:
		/* Slots the survey never writes, so nothing can clobber them:
		 * slot= cx.lvl   l5-old= gfx.lvl   cb-w= mx.lvl
		 * cbregs-n= mxc.lvl   fault= magic   restore= walked */
#if GP_CMDB == 2
		r->p_cb0_cba2r = n;	/* matches */
#endif
		r->wsid = found[0];
		r->wcbar_old = found[1];
		r->cbar_w = found[2];
		r->cbar_count = found[3];
		r->w2cr_f = magic;
		r->w2cr_r = walked;
		con_hex2("GPUS", "cx=", found[0], "gfx=", found[1]);
		con_hex2("GPUS", "mx=", found[2], "mxc=", found[3]);
	}
#endif

#if GP_CBT
	/*
	 * v11a: is the CB register file writable now?  Two harmless config
	 * registers get a pattern and are left at the written value, so the
	 * survey below reports them (p_cb0_tcr2 / p_cb0_mair1).  SCTLR is not
	 * touched, so the bank cannot start translating anything.
	 */
	{
		uintptr_t cb = gp_cb_base(id1);

		r->p_cbw_flags = 0x200u;	/* marker: the CB test ran */
		r->last_step = 222;
		gp_wr32(cb + 0x10u, 0x5u);		/* TCR2 */
		__asm__ volatile("dsb sy" ::: "memory");
		r->last_step = 223;
		r->w2cr_t = gp_rd32(cb + 0x10u);
		r->last_step = 224;
		gp_wr32(cb + 0x3cu, 0xaabbccddu);	/* MAIR1 */
		__asm__ volatile("dsb sy" ::: "memory");
		r->last_step = 225;
		r->wsctlr = gp_rd32(cb + 0x3cu);
		con_hex2("GPUS", "tcr2=", r->w2cr_t, "mair1=", r->wsctlr);
	}
#endif

#if GP_CBW
	/*
	 * v11: make CB0 a stage-1 translator over a 1 GiB identity map and
	 * point SID 0 at it.  The tables sit in the shim's BSS (VA == PA), are
	 * cleaned to PoC, and the CB is disabled before its state changes.
	 * No GPU client exists yet, so an enabled CB with a fault policy of
	 * "terminate" cannot touch anything until the engine runs.
	 */
	{
		uintptr_t s = GPU_SMMU_BASE;
		uintptr_t cb = gp_cb_base(id1);
		uintptr_t l1 = (uintptr_t)gp_cbw_l1;
		uintptr_t l2 = (uintptr_t)gp_cbw_l2;
		uintptr_t p;
		unsigned k;
		uint32_t f = 0;

		gp_cbw_l1[0] = (uint64_t)l2 | 3u;
		for (k = 0; k < 512u; k++)
			gp_cbw_l2[k] = ((uint64_t)k << 21) | 0x701u;
		for (p = l1; p < l1 + 0x2000u; p += 64u)
			__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
		__asm__ volatile("dsb sy" ::: "memory");

		r->last_step = 200;
		gp_wr32(cb + 0x00u, 0u);	/* disable first */
		r->last_step = 201;
		gp_wr32(s + 0x1000u, 0x1f000u);	/* CBAR(0): S1 trans, S2 bypass */
		r->last_step = 202;
		gp_wr32(s + 0x1800u, 1u);	/* CBA2R(0): AArch64 */
		r->last_step = 203;
		gp_wr32(cb + 0x20u, (uint32_t)l1);
		gp_wr32(cb + 0x24u, (uint32_t)((uint64_t)l1 >> 32));
		r->last_step = 204;
		gp_wr32(cb + 0x10u, 0u);	/* TCR2 */
		r->last_step = 205;
		gp_wr32(cb + 0x30u, 0x3520u);	/* T0SZ=32, 4K, inner WBWA */
		r->last_step = 206;
		gp_wr32(cb + 0x38u, 0xffu);	/* MAIR0: attr0 = Normal WB */
		r->last_step = 207;
		gp_wr32(cb + 0x00u, 0x1u);	/* M=1 */
		r->last_step = 208;
		gp_wr32(s + 0x800u, 0x80000000u);	/* SMR(0): SID 0 */
		r->last_step = 209;
		gp_wr32(s + 0xc00u, 0x1u);	/* S2CR(0): TRANS -> CB0 */
		__asm__ volatile("dsb sy" ::: "memory");

		r->last_step = 210;
		r->smr0 = gp_rd32(s + 0x800u);
		if (gp_fault)
			f |= 1u;
		r->s2cr0 = gp_rd32(s + 0xc00u);
		if (gp_fault)
			f |= 2u;
		r->sid0_slot = 0;
		r->p_cbw_flags = 0x100u | f;
		r->p_cbw_before = 0;
		r->p_cbw_after = gp_rd32(cb + 0x00u);
		con_hex2("GPUS", "smr=", r->smr0, "s2cr=", r->s2cr0);
	}
#endif

#if GP_SURVEY
	/*
	 * v8 read-only survey: context banks at the *computed* base plus the
	 * GPUCC power/clock state.  Everything is a read, nothing is written.
	 *
	 * The result slots are reused so this runs on an unmodified XNU.  The
	 * printed fields mean:
	 *   cbar_r        CB base computed from IDR1 (expect 0x3dc0000)
	 *   kg0..kg3      CB0.TTBR1, CB0.TCR2, CB0.MAIR1, CB1.SCTLR
	 *   sctlr_r       CB0.SCTLR        ttbr0   CB0.TTBR0
	 *   tcr / mair    CB0.TCR / CB0.MAIR0
	 *   wcbar         CBAR(0)          cba2r / cbfrsynra  stream group 0
	 *   cbar1..3      GPUCC CX_GDSCR, GDS_HW_CTRL, GX_GDSCR
	 *   sctlr_w       GPUCC GX domain (0x3d99504)
	 *   wsctlr        gpu_cc_ahb (0x911c)
	 *   wsctlr_old    gpu_cc_cx_gmu (0x913c)
	 *   w2cr_old      gpu_cc_hub_cx_int (0x9148)
	 *   wsmr          gpu_cc_hlos1_vote_gpu_smmu (0x7000)
	 *   w2cr          gpu_cc_memnoc_gfx (0x9150)
	 *   kgsl_status   KGSL RBBM_STATUS (0x840)
	 *   kgsl_status2  KGSL RBBM_IDENT  (0x000)
	 *   op_fault      one bit per failed read, in access order below
	 */
	{
		uintptr_t g = GPU_GPUCC_BASE;
		uintptr_t cb = gp_cb_base(id1);
		uint32_t f = 0;
		unsigned n = 0;

#define GPR(step, dst, addr) do {			\
		r->last_step = (step);			\
		(dst) = gp_rd32(addr);			\
		if (gp_fault)				\
			f |= 1u << n;			\
		n++;					\
	} while (0)

		r->cbar_r = (uint32_t)cb;	/* where CB0 should be */
		r->cbar_bank = 0;

		/* GPUCC: is the CX domain already up? */
		GPR(100, r->cbar1, g + 0x9108u);	/* CX_GDSCR */
		GPR(101, r->cbar2, g + 0x953cu);	/* GDS_HW_CTRL */
		GPR(102, r->cbar3, g + 0x905cu);	/* GX_GDSCR */
		GPR(103, r->sctlr_w, g + 0x9504u);	/* GX domain addr */
		GPR(104, r->wsctlr, g + 0x911cu);	/* gpu_cc_ahb */
		GPR(105, r->wsctlr_old, g + 0x913cu);	/* gpu_cc_cx_gmu */
		GPR(106, r->w2cr_old, g + 0x9148u);	/* gpu_cc_hub_cx_int */
		GPR(107, r->wsmr, g + 0x7000u);		/* smmu clock vote */
		GPR(108, r->w2cr, g + 0x9150u);		/* gpu_cc_memnoc_gfx */

		/* Context bank 0 at the page the SMMU says it is. */
		GPR(110, r->sctlr_r, cb + 0x00u);	/* SCTLR */
		GPR(111, r->ttbr0, cb + 0x20u);		/* TTBR0 */
		GPR(112, r->tcr, cb + 0x30u);		/* TCR */
		GPR(113, r->mair, cb + 0x38u);		/* MAIR0 */
		GPR(114, r->wcbar, s + 0x1000u);	/* CBAR(0) */
		GPR(115, r->cba2r, s + 0x1800u);	/* CBA2R(0) */
		GPR(116, r->cbfrsynra, s + 0x1400u);	/* CBFRSYNRA(0) */
		GPR(117, r->kg0, cb + 0x28u);		/* TTBR1 */
		GPR(118, r->kg1, cb + 0x10u);		/* TCR2 */
		GPR(119, r->kg2, cb + 0x3cu);		/* MAIR1 */
		GPR(120, r->kg3, cb + 0x1000u);		/* CB1.SCTLR */

		/* Is the GPU engine itself alive behind the SMMU? */
		GPR(125, r->kgsl_status2, GPU_KGSL_BASE + 0x000u);
		GPR(126, r->kgsl_status, GPU_KGSL_BASE + 0x840u);

		r->op_fault = f;
		r->kfault = (f >> 20) & 3u;	/* KGSL IDENT / RBBM_STATUS */

		/* Named copies for kernels that know the v9 layout. */
		r->p_cbbase = (uint32_t)cb;
		r->p_cb0_sctlr = r->sctlr_r;
		r->p_cb0_ttbr0 = r->ttbr0;
		r->p_cb0_ttbr1 = r->kg0;
		r->p_cb0_tcr = r->tcr;
		r->p_cb0_tcr2 = r->kg1;
		r->p_cb0_mair0 = r->mair;
		r->p_cb0_mair1 = r->kg2;
		r->p_cb0_cbar = r->wcbar;
		r->p_cb0_cba2r = r->cba2r;
		r->p_cb0_cbf = r->cbfrsynra;
		r->p_cb1_sctlr = r->kg3;
		r->p_cxgdr = r->cbar1;
		r->p_hwctrl = r->cbar2;
		r->p_gxgdr = r->cbar3;
		r->p_gxdom = r->sctlr_w;
		r->p_clk_ahb = r->wsctlr;
		r->p_clk_cxgmu = r->wsctlr_old;
		r->p_clk_hubcx = r->w2cr_old;
		r->p_clk_smmuvote = r->wsmr;
		r->p_clk_memnoc = r->w2cr;
		r->p_kgsl_ident = r->kgsl_status2;
		r->p_kgsl_rbbm = r->kgsl_status;
		r->p_rd_fault = f;
#undef GPR
	}
#endif

#if GP_PWR || GP_EARLY
	{
		uintptr_t cb = (id1 == 0xffffffffu) ? 0 : gp_cb_base(id1);

		if (id1 == 0xffffffffu)
			r->p_pwr_flags |= 0x8000u;
#if !GP_EARLY
		gp_power_cx(r);		/* with GP_EARLY it already ran */
#endif
#if GP_GX
#if GP_RAIL
		/* Recorded in slots the survey never writes:
		 * fault= cx.lvl vote  restore= mxc.lvl vote  trans= gfx.lvl vote */
		r->w2cr_f = (uint32_t)gp_rh_vote(0x30000u, 384u); /* control */
		delay_ms(5);
		r->w2cr_r = (uint32_t)gp_rh_vote(0x30120u, 384u); /* mxc.lvl */
		delay_ms(5);
		r->w2cr_t = (uint32_t)gp_rh_vote(0x30050u, 192u); /* gfx.lvl */
		delay_ms(5);
		r->p_cbw_flags |= 0x1000u;
		con_hex2("GPUS", "cx=", r->w2cr_f, "mxc=", r->w2cr_r);
#endif
		gp_power_gx(r);
#if GP_RAIL
		/* GX clock gates after the attempt, in otherwise-unused slots. */
		r->wsid = gp_rd32(GPU_GPUCC_BASE + 0x90a8u);	/* gx_gfx3d */
		r->cbar_w = gp_rd32(GPU_GPUCC_BASE + 0x90bcu);	/* gx_gmu */
		r->cbar_count = gp_rd32(GPU_GPUCC_BASE + 0x90c0u); /* gx_ff */
		r->cbar_bank = (int)gp_rd32(GPU_GPUCC_BASE + 0x90b0u); /* vsense */
#endif
#endif
		r->p_pwr_step = 20;
		r->p_cxgdr2 = gp_rd32(GPU_GPUCC_BASE + 0x9108u);
		r->p_clk_ahb2 = gp_rd32(GPU_GPUCC_BASE + 0x911cu);
		r->p_clk_cxgmu2 = gp_rd32(GPU_GPUCC_BASE + 0x913cu);
		r->p_clk_hubcx2 = gp_rd32(GPU_GPUCC_BASE + 0x9148u);
		r->p_clk_smmuvote2 = gp_rd32(GPU_GPUCC_BASE + 0x7000u);
		r->p_clk_memnoc2 = gp_rd32(GPU_GPUCC_BASE + 0x9150u);
		r->p_kgsl_rbbm2 = gp_rd32(GPU_KGSL_BASE + 0x840u);
		r->last_step = 30;
		if (cb)
			r->p_cb0_sctlr2 = gp_rd32(cb + 0x00u);
		r->last_step = 31;
		r->cbar_bank = 1;	/* marker: the post-power block ran */
		con_hex2("GPU", "cx=", r->p_cxgdr2, "rbbm=", r->p_kgsl_rbbm2);
		con_hex2("GPU", "cb0=", r->p_cb0_sctlr2, "fl=", r->p_pwr_flags);
		con_hex("GPUS", 4);
	}
#endif

	if (GPU_PROBE_LEVEL >= 3) {
		/* kgsl register offsets are in dwords: RBBM_STATUS = 0x210. */
		r->kgsl_status = gp_rd32(GPU_KGSL_BASE + 0x840);
		r->kfault = (uint32_t)gp_fault;
		if (!gp_fault)
			r->kgsl_status2 = gp_rd32(GPU_KGSL_BASE + 0x804);
		if (gp_fault)
			r->kfault = 1;
	}
	goto done;

readfault:
	r->rfault = 1;
	r->far = gp_far;
	r->esr = gp_esr;
done:
	__asm__ volatile("dsb sy" ::: "memory");
	r->magic = GPU_PROBE_MAGIC;	/* publish last */
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

/*
 * topaz: xnu needs the 16K translation granule. ABL starts us on a Kryo 260
 * Silver core (Cortex-A53 class, TGran16 = 0); the Gold A73 cluster has it.
 * The boot core wakes gold CPU 0x100 with this DTB and parks itself; gold
 * runs the loader and xnu never sees the silver cluster.
 */
static volatile uint64_t gold_dtb;
static void test_gic(void);

/*
 * xnu starts the other gold CPUs itself (PSCI CPU_ON), without the shim, so
 * give their redistributors the same setup test_gic() gives the boot CPU:
 * awake, SGIs/PPIs in Group 1, default priority. The frames are MMIO, so the
 * boot CPU can do it for all of them.
 */
static void gold_gicr_prepare(void)
{
	for (uint32_t aff = 0x101; aff <= 0x103; aff++) {
		uintptr_t r = 0;
		for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
			uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
			if ((uint32_t)(typer >> 32) == aff) {
				r = GICR_BASE + off;
				break;
			}
			if (typer & (1 << 4))	/* Last */
				break;
		}
		if (!r) {
			con_hex("GICR miss", aff);
			continue;
		}
		uint32_t waker = rd32(r + GICR_WAKER);
		if (waker & 2) {
			wr32(r + GICR_WAKER, waker & ~2U);
			for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
				;
		}
		wr32(r + GICR_IGROUPR0, 0xffffffff);
		for (int id = 0; id < 32; id++)
			wr8(r + GICR_IPRIORITYR + id, 0xa0);
		/*
		 * Also enable the timers and SGI1, as test_gic() does on the boot
		 * CPU: without it a thread whose wakeup timer is armed on a
		 * secondary never wakes (backboardd stuck in usleep()).
		 */
		wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
		con_hex2("GICR+", "aff=", aff, "en=", rd32(r + GICR_ISENABLER0));
	}
}

/*
 * Core speed probe: ticks (19.2 MHz) for a fixed dependent-add loop with the
 * I-cache on. Silver runs at whatever ABL set; gold has no cpufreq driver
 * after PSCI CPU_ON, so a much larger count than silver means a low clock.
 */
static volatile uint64_t silver_ticks;

static uint64_t speed_ticks(void)
{
	uint64_t sctlr = rsr("sctlr_el1");
	wsr("sctlr_el1", sctlr | (1UL << 12));	/* I-cache on (fine with MMU off) */
	__asm__ volatile("isb");
	uint64_t t = now(), x = 0;
	for (uint64_t i = 0; i < 20000000; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(x));
	t = now() - t;
	wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb");
	return t;
}

/*
 * Gold cluster to its top cpufreq-hw level before xnu (which has no cpufreq
 * driver): freq-domain1 0xf523000, LUT +0x110 (32-byte rows), PERF_STATE +0x920.
 */
static void gold_cpufreq_max(void)
{
	uintptr_t b = 0x0f523000;
	uint32_t best = 0, best_khz = 0, prev = 0;
	for (uint32_t i = 0; i < 12; i++) {
		uint32_t v = rd32(b + 0x110 + i * 0x20);
		uint32_t khz = ((v >> 30) & 3) ? 19200 * (v & 0xff) : 300000;
		if (i && khz == prev)
			break;
		prev = khz;
		if (khz > best_khz) {
			best_khz = khz;
			best = i;
		}
	}
	uint32_t old = rd32(b + 0x920);
	wr32(b + 0x920, best);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("CPUFREQ", "old=", old, "mhz=", best_khz / 1000);
}

static void __attribute__((noreturn)) gold_main(uint64_t dtb)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	(void)dtb;

	con_hex2("X2", "midr=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2("MMFR0", "lo=", (uint32_t)mmfr0, "tg16=", (mmfr0 >> 20) & 0xf);
	con_skip();
	if (((mmfr0 >> 20) & 0xf) == 0) {
		con_str("X2", "no 16K granule either");
	} else {
		test_gic();
		gold_gicr_prepare();
		con_str("X2", "16K ok; diagnostics done");
	}
	delay_ms(30000);
	reset(20);
}

#if GP_SMPREL
/*
 * Releasing the CM3 stalls the core that does it forever, so do it from a
 * secondary CPU and leave the boot CPU free for XNU.
 */
static void
gp_gmu_release_from_secondary(void)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntpct_el0"), t;

	do {
		t = rsr("cntpct_el0");
	} while (t - t0 < f * 4u);
	wr32(0x3d7e000UL, 0u);		/* CM3_SYSRESET = 0 */
	for (;;) {
		__asm__ volatile("wfe");
	}
}
#endif

void secondary_main(uint64_t idx)
{
#if GP_SMPREL
	if (idx == 7u) {
		gp_gmu_release_from_secondary();
	}
#endif
	if (gold_dtb)
		gold_main(gold_dtb);
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + VTIMER_INTID, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, cpu_mpidr(i), (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (cpu_mpidr(i) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	/* marble: CNTP_CTL_EL0 traps at EL1; only touch the timer we used */
	if (src == VTIMER_INTID)
		wsr("cntv_ctl_el0", 0);
	else if (src == PTIMER_INTID)
		wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	con_hex2("IGROUP0", "was=", grp, "now=", rd32(r + GICR_IGROUPR0));
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

static void __attribute__((unused)) shim_main_p1(uint64_t dtb)
{
	struct mmu_info mi;

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "marble P1 diagnostics");
	/* topaz: xnu needs the 16K granule (TGran16 != 0); Cortex-A53 may lack it */
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_hex2("MMFR0", "lo=", (uint32_t)mmfr0, "tg16=", (mmfr0 >> 20) & 0xf);
	con_hex2("MIDR", "el1=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	if (((mmfr0 >> 20) & 0xf) == 0) {
		con_str("MMFR0", "NO 16K GRANULE ON BOOT CPU");
		delay_ms(30000);
		reset(20);
	}
	con_skip();
	con_str("SHIM", "GIC/timer test");
	test_gic();
	con_skip();
	con_str("SHIM", "marble diagnostics done; reset in 60s");
	delay_ms(60000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main_p1));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}

/* ---- X1: load the SWEETXNU payload and jump to xnu ---- */

extern const uint32_t splash_img[];

/*
 * XNU's phystokv_range() (arm_vm_init.c) refuses every PA outside the ADT
 * bank (0x840000000+), so ml_io_map() cannot touch low DRAM.  The DSP, on
 * the other hand, is behind apps-smmu SID 0x1801 which we leave bypassing,
 * so it reads plain physical addresses fine.  Fill the PCM tone here, while
 * the shim has no MMU (plain physical stores), and let sweet_glink.c map
 * exactly TONE_PA with offset mode and stream offsets 0/0x800.
 * Two 1152-byte buffers, 144 frames of 32-bit stereo at 96 kHz.
 */
#define TONE_PA		0xa2e00000UL
/* Bytes copied in front of the DSP: the kernel declares the same size as the
 * shared-memory region, so the offsets it streams are all inside this. */
#ifndef PCM_REGION
#define PCM_REGION	0x5000UL	/* overridden by build_xnu.sh (-DPCM_REGION) */
#endif

extern const uint8_t pcm_img[];
extern const uint8_t pcm_img_end[];

/* Copy the embedded raw PCM (48 kHz / 24-bit LE / stereo) into low DRAM,
 * looping if the clip is shorter than the region.  Falls back to the built-in
 * sine only if the image carries no PCM at all. */
static void
fill_pcm_low(void)
{
	const uint8_t *src = pcm_img;
	uint32_t n = (uint32_t)(pcm_img_end - pcm_img);
	volatile uint8_t *p = (volatile uint8_t *)TONE_PA;

	if (n == 0) {
		return;
	}
	for (uint32_t i = 0; i < PCM_REGION; i++) {
		p[i] = src[i % n];
	}
	__asm__ volatile("dsb sy" ::: "memory");
}
static const int16_t tone48[48] = {
	0, 1069, 2120, 3135, 4096, 4987, 5793, 6499,
	7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
	7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069,
	0, -1069, -2120, -3135, -4096, -4987, -5793, -6499,
	-7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568,
	-7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069,
};

static void
fill_tone_low(void)
{
	volatile uint32_t *p = (volatile uint32_t *)TONE_PA;

	for (int b = 0; b < 2; b++) {
		for (int i = 0; i < 144; i++) {
			int32_t v = (int32_t)tone48[i % 48] << 16;

			p[b * 0x200 + 2 * i] = (uint32_t)v;
			p[b * 0x200 + 2 * i + 1] = (uint32_t)v;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

#ifndef UFS_DIAG
void shim_main(uint64_t dtb)
{
	stage = "xnu";
	con_image(splash_img, FB_W, FB_H);
	con_str("SHIM", "marble XNU loader");
	if ((uint32_t)(pcm_img_end - pcm_img) != 0) {
		fill_pcm_low();
		con_hex2("PCM", "bytes=", (uint32_t)(pcm_img_end - pcm_img),
		    " at=", (uint32_t)TONE_PA);
	} else {
		fill_tone_low();
		con_str("TONE", "prefilled at 0xa2e00000");
	}

	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_hex2("MMFR0", "lo=", (uint32_t)mmfr0, "tg16=", (mmfr0 >> 20) & 0xf);
	con_hex2("MIDR", "el1=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	if (((mmfr0 >> 20) & 0xf) == 0) {
		con_str("MMFR0", "NO 16K GRANULE ON BOOT CPU");
		delay_ms(30000);
		reset(20);
	}
	con_skip();

#if GPU_PROBE_LEVEL > 0
	gpu_probe_boot();
#endif

	/* loader_boot never returns on success; on failure it returns a reason. */
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_str("XNU", "did not boot");
	delay_ms(120000);	/* keep the reason on screen long enough to read */
	reset(20);
}
#endif /* !UFS_DIAG */

#ifdef UFS_DIAG
void shim_main(uint64_t dtb)
{
	(void)dtb;
	stage = "ufs";
	con_clear(RED);
	con_str("SHIM", "marble UFS diagnostic");
	test_ufs();
	con_str("UFS", "done; no auto-reset (power-cycle to exit)");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
#endif
