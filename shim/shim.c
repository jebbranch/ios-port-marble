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
#define GP_STR2(x) #x
#define GP_STR(x) GP_STR2(x)
static const char gp_level_tag[] __attribute__((used)) =
    "gpuprobe-" GP_STR(GPU_PROBE_LEVEL)
    "-c" GP_STR(GP_CBAR_BANK) "w" GP_STR(GP_CBAR_WRITE)
    "-s" GP_STR(GP_SCTLR_BANK) "w" GP_STR(GP_SCTLR_WRITE)
    "-r" GP_STR(GP_CB_REGS) GP_STR(GP_GR1_READ) GP_STR(GP_KGSL_SCAN);

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
};				/* 0xf8 */

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

void secondary_main(uint64_t idx)
{
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
