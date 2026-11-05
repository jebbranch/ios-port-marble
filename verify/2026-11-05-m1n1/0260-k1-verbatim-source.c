/*
 * SWEET: minimal GLINK-over-SMEM client for the ADSP GPR router.
 *
 * sweet_adsp.c releases the Q6 through TZ PAS.  This file adds just enough of
 * the GLINK wire protocol to open the "adsp_apps" channel and send one GPR
 * packet to the q6prm service, asking it to vote for the LPASS audio hardware
 * core.  LPASS register blocks (RX macro 0x03200000, CDC, TDM) only answer the
 * AP after that vote, so this is the gate in front of any sound output.
 *
 * Facts from the live DTS and the Linux drivers:
 *   SMEM           0x80900000 + 2 MiB; partition table in the last 4 KiB ($TOC)
 *   GLINK items    478 descriptor, 479 AP->ADSP FIFO, 480 ADSP->AP FIFO
 *   IPCC           0xed18000, SEND_ID at +0x0c, LPASS client 3, signal 0
 *   GPR domains    ADSP 2, APPS 3; q6prm service 7, module IID 2
 *   q6prm vote     opcode 0x0100100F, PARAM_ID_RSC_HW_CORE 0x08001032
 *
 * Every access to shared memory is a 32-bit volatile access: the region is
 * mapped Device, and XNU's memcpy does unaligned ldp/stp which faults there.
 *
 * Gated by sweetglink=1.  Register reads after the vote additionally need
 * sweetglinkread=1, because a fault there panics.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include <vm/vm_kern.h>

extern vm_map_address_t pmap_map(vm_map_address_t virt, vm_offset_t start,
    vm_offset_t end, vm_prot_t prot, unsigned int flags);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_spmi_init(void);
extern int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);
extern int sweet_spmi_read_any(unsigned sid, unsigned addr, uint8_t *out);
extern int sweet_spmi_read_block(unsigned sid, unsigned addr, uint8_t *buf,
    unsigned len);
extern int sweet_spmi_write(unsigned sid, unsigned addr, const uint8_t *buf,
    unsigned len);
void sweet_glink_probe(void);

#define G_SMEM_PA       0x80900000u
#define G_SMEM_SZ       0x00200000u
#define G_TOC_OFF       (G_SMEM_SZ - 0x1000u)
#define G_PFN_MASK      0x3fffu

#define G_IPCC_PA       0xed18000u
#define G_IPCC_SEND_ID  0x0cu
#define G_IPCC_ADSP     3u

#define G_LPASS_RX_PA   0x03200000u     /* RX macro */
#define G_LPASS_VA_PA   0x033f0000u     /* VA macro */

/* Force the console ring out to disk before a step that may panic. */
extern void sweet_log_sync(void);       /* bsd/dev/sweet_ufs.c */
extern void sweet_i2c_probe_amps(void);  /* bsd/dev/sweet_i2c.c */
extern void sweet_i2c_amp_readback(const char *tag);

/* GLINK commands */
#define CMD_VERSION             0
#define CMD_VERSION_ACK         1
#define CMD_OPEN                2
#define CMD_CLOSE               3
#define CMD_OPEN_ACK            4
#define CMD_INTENT              5
#define CMD_RX_DONE             6
#define CMD_RX_INTENT_REQ       7
#define CMD_RX_INTENT_REQ_ACK   8
#define CMD_TX_DATA             9
#define CMD_CLOSE_ACK           11
#define CMD_TX_DATA_CONT        12
#define CMD_READ_NOTIF          13
#define CMD_RX_DONE_W_REUSE     14

#define GLINK_VERSION_1         1u
#define GLINK_FEATURE_REUSE     1u      /* GLINK_FEATURE_INTENT_REUSE */

#define G_LINK_NAME             "adsp_apps"
#define G_LINK_NAME_LEN         10u     /* "adsp_apps" + NUL */

#define G_NINTENTS              8
#define G_INTENT_SIZE           1024u
#define G_FIFO_RESERVE          16u     /* FIFO_FULL_RESERVE + TX_BLOCKED */

/* GPR / AudioReach */
#define GPR_DOMAIN_ADSP         2u
#define GPR_DOMAIN_APPS         3u
#define GPR_PRM_SERVICE         7u
#define GPR_PRM_MODULE          2u

#define PRM_CMD_REQUEST_HW_RSC      0x0100100Fu
#define PARAM_ID_RSC_HW_CORE        0x08001032u
#define Q6PRM_HW_CORE_LPASS         1u

static vm_offset_t g_gr_win, g_cb_win;
static vm_offset_t g_smem;
static vm_offset_t g_ipcc;
static int        g_chan;

/* SMEM item locations (offsets from the SMEM base). */
static uint32_t g_desc_off, g_desc_len;
static uint32_t g_tx_off, g_tx_len;
static uint32_t g_rx_off, g_rx_len;

/* Channel state. */
static uint32_t g_lcid = 1;
static uint32_t g_rcid;
static int      g_ver_acked;
static int      g_open_acked;
static int      g_remote_open;

struct g_riid {
	uint32_t id;
	uint32_t size;
	uint32_t chan;
	uint32_t in_use;
};

struct g_liid {
	uint32_t id;
	uint32_t size;
	uint32_t filled;
	uint8_t  buf[G_INTENT_SIZE];
};

/* Intents handed to the peer on CMD_RX_INTENT_REQ.  The ADSP asks for one
 * when it wants to push a notification (we saw "size=16" right after boot);
 * ACKing without advertising a buffer leaves its TX side without a slot. */
#define G_NDYN 8
struct g_dyn_liid {
	uint32_t id;
	uint32_t size;
	uint32_t filled;
	uint32_t lcid;
	uint8_t  buf[256];
};
static struct g_dyn_liid g_dyn_liids[G_NDYN];
static uint32_t g_dyn_next = 0x200;

#define G_NRIIDS 256           /* the ADSP advertises ~120 of them */
static struct g_riid g_riids[G_NRIIDS];
static int           g_nriids;
static struct g_liid g_liids[G_NINTENTS];
static int           g_nliids;

/* Last assembled GPR packet. */
static uint8_t  g_rsp[G_INTENT_SIZE];
static uint32_t g_rsp_len;
static int      g_rsp_ready;

/* Optional pmic_glink client (sweetbatt=1).  The ADSP opens
 * PMIC_RTR_ADSP_APPS itself; we ack it, open our own side and send
 * 24-byte battmgr requests. */
#define G_BATT_LCID	2u
#define G_BATT_LIID	0x100u
#define G_BATT_NAME	"PMIC_RTR_ADSP_APPS"
#define G_BATT_NAME_LEN	19u	/* 18 chars + NUL */

static uint32_t g_batt_rcid;
static int      g_batt_acked, g_batt_remote;
static uint8_t  g_batt_rsp[64];
static uint32_t g_batt_rsp_len, g_batt_fill;
static int      g_batt_ready;
/* Last sample, consumed by SweetBattery.  -1 means "not read yet". */
static int g_batt_s_status = -1, g_batt_s_cap = -1, g_batt_s_mv = -1;
static int g_batt_s_ma, g_batt_s_usb = -1;
static int g_batt_chan;

static void
g_wait(unsigned ms)
{
	uint64_t deadline;

	if (ms == 0) {
		return;
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&g_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static vm_offset_t
g_win_alloc(vm_size_t size)
{
	vm_offset_t va = 0;

	if (kmem_alloc(kernel_map, &va, size, KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	return va;
}

static vm_offset_t
g_win_map(vm_offset_t win, uint64_t pa, vm_size_t size)
{
	uint64_t base = pa & ~(uint64_t)G_PFN_MASK;
	uint32_t off = (uint32_t)(pa - base);
	vm_size_t span = (off + size + G_PFN_MASK) & ~G_PFN_MASK;

	pmap_remove(kernel_pmap, win, win + span);
	pmap_map(win, (vm_offset_t)base, (vm_offset_t)(base + span),
	    VM_PROT_READ | VM_PROT_WRITE, VM_WIMG_IO);
	return win + off;
}

/* ml_io_map() draws from a permanent, system-wide 8 MiB io_submap that
 * SPMI already half fills, and an exhausted submap panics with
 * KERN_NO_SPACE (run76, run83).  Use a kernel_map window instead: allocate
 * the range once and point it at the physical page. */
static vm_offset_t
g_map_win(uint64_t pa, vm_size_t size)
{
	vm_offset_t w = g_win_alloc(size);

	if (w == 0) {
		return 0;
	}
	return g_win_map(w, pa, size);
}

/* Same, with an explicit WIMG attribute (e.g. VM_WIMG_POSTED). */
static vm_offset_t
g_map_win_wimg(uint64_t pa, vm_size_t size, unsigned int wimg)
{
	uint64_t base = pa & ~(uint64_t)G_PFN_MASK;
	uint32_t off = (uint32_t)(pa - base);
	vm_size_t span = (off + size + G_PFN_MASK) & ~G_PFN_MASK;
	vm_offset_t w = g_win_alloc(span);

	if (w == 0) {
		return 0;
	}
	pmap_remove(kernel_pmap, w, w + span);
	pmap_map(w, (vm_offset_t)base, (vm_offset_t)(base + span),
	    VM_PROT_READ | VM_PROT_WRITE, wimg);
	return w + off;
}

static void
g_win_free(vm_offset_t win, vm_size_t size)
{
	pmap_remove(kernel_pmap, win, win + size);
	kmem_free(kernel_map, win, size);
}

/* ---- Device-memory accessors (32-bit) ---- */

static uint32_t
g_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(g_smem + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
g_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(g_smem + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint16_t
g_rd16(uint32_t off)
{
	uint32_t v = g_rd(off & ~3u);

	return (uint16_t)(v >> (8u * (off & 3u)));
}

static void
g_fifo_read(uint32_t base, uint32_t len, uint32_t pos,
    uint8_t *dst, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = g_rd(base + ((pos + i) % len));
		uint32_t k = (n - i) < 4 ? (n - i) : 4;

		for (uint32_t b = 0; b < k; b++) {
			dst[i + b] = (uint8_t)(w >> (8u * b));
		}
	}
}

static void
g_fifo_write(uint32_t base, uint32_t len, uint32_t pos,
    const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		uint32_t k = (n - i) < 4 ? (n - i) : 4;

		for (uint32_t b = 0; b < k; b++) {
			w |= (uint32_t)src[i + b] << (8u * b);
		}
		/* head and lengths are 8-byte aligned, words never straddle */
		g_wr(base + ((pos + i) % len), w);
	}
}

static uint32_t
g_rx_avail(void)
{
	uint32_t head = g_rd(g_desc_off + 12);
	uint32_t tail = g_rd(g_desc_off + 8);

	if (head < tail) {
		return g_rx_len - tail + head;
	}
	return head - tail;
}

static void
g_rx_advance(uint32_t n)
{
	uint32_t tail = g_rd(g_desc_off + 8);

	tail = (tail + n) % g_rx_len;
	g_wr(g_desc_off + 8, tail);
}

static void
g_kick(void)
{
	if (g_ipcc != 0) {
		*(volatile uint32_t *)(g_ipcc + G_IPCC_SEND_ID) =
		    (G_IPCC_ADSP << 16) | 0u;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

/* Send one small GLINK control message (8-byte header + up to 64 bytes). */
static void
g_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, uint32_t dlen)
{
	uint8_t msg[8 + 64];
	uint32_t head, msglen;

	if (dlen > 64) {
		dlen = 64;
	}
	msg[0] = (uint8_t)(cmd & 0xff);
	msg[1] = (uint8_t)(cmd >> 8);
	msg[2] = (uint8_t)(p1 & 0xff);
	msg[3] = (uint8_t)(p1 >> 8);
	msg[4] = (uint8_t)(p2 & 0xff);
	msg[5] = (uint8_t)(p2 >> 8);
	msg[6] = (uint8_t)(p2 >> 16);
	msg[7] = (uint8_t)(p2 >> 24);
	if (data != NULL && dlen != 0) {
		for (uint32_t i = 0; i < dlen; i++) {
			msg[8 + i] = ((const uint8_t *)data)[i];
		}
	}
	msglen = 8 + dlen;
	head = g_rd(g_desc_off + 4);
	g_fifo_write(g_tx_off, g_tx_len, head, msg, msglen);
	head = (head + msglen + 7u) & ~7u;
	if (head >= g_tx_len) {
		head -= g_tx_len;
	}
	g_wr(g_desc_off + 4, head);
	g_kick();
}

/* Send a GLINK data message (16-byte header + payload). */
static void
g_send_tx_data_lcid(uint32_t lcid, uint32_t iid, const uint8_t *pkt,
    uint32_t len)
{
	/* One probe thread only, so a static buffer is fine; GLINK messages
	 * are 16-byte header + payload and must not be truncated (a 360-byte
	 * APM graph open used to go out as 128 bytes and the DSP ignored it). */
	static uint8_t msg[16 + 4096];
	uint32_t head;

	if (len > 4096) {
		len = 4096;
	}
	msg[0] = CMD_TX_DATA;
	msg[1] = 0;
	msg[2] = (uint8_t)(lcid & 0xff);
	msg[3] = (uint8_t)(lcid >> 8);
	msg[4] = (uint8_t)(iid & 0xff);
	msg[5] = (uint8_t)(iid >> 8);
	msg[6] = (uint8_t)(iid >> 16);
	msg[7] = (uint8_t)(iid >> 24);
	msg[8] = (uint8_t)(len & 0xff);
	msg[9] = (uint8_t)(len >> 8);
	msg[10] = (uint8_t)(len >> 16);
	msg[11] = (uint8_t)(len >> 24);
	msg[12] = 0;
	msg[13] = 0;
	msg[14] = 0;
	msg[15] = 0;
	for (uint32_t i = 0; i < len; i++) {
		msg[16 + i] = pkt[i];
	}
	head = g_rd(g_desc_off + 4);
	g_fifo_write(g_tx_off, g_tx_len, head, msg, 16 + len);
	head = (head + 16 + len + 7u) & ~7u;
	if (head >= g_tx_len) {
		head -= g_tx_len;
	}
	g_wr(g_desc_off + 4, head);
	g_kick();
}

static void
g_send_tx_data(uint32_t iid, const uint8_t *pkt, uint32_t len)
{
	g_send_tx_data_lcid(g_lcid, iid, pkt, len);
}

static void
g_send_version(void)
{
	g_send(CMD_VERSION, (uint16_t)GLINK_VERSION_1, GLINK_FEATURE_REUSE,
	    NULL, 0);
}

static void
g_send_open(void)
{
	uint8_t name[16];

	for (uint32_t i = 0; i < sizeof(name); i++) {
		name[i] = 0;
	}
	for (uint32_t i = 0; i < G_LINK_NAME_LEN; i++) {
		name[i] = (uint8_t)G_LINK_NAME[i];
	}
	g_send(CMD_OPEN, (uint16_t)g_lcid, G_LINK_NAME_LEN, name,
	    G_LINK_NAME_LEN);
}

static void
g_send_open_ack(uint32_t rcid)
{
	g_send(CMD_OPEN_ACK, (uint16_t)rcid, 0, NULL, 0);
}

static void
g_advertise_intent_lcid(uint32_t lcid, uint32_t id, uint32_t size)
{
	uint8_t buf[8];

	buf[0] = (uint8_t)(size & 0xff);
	buf[1] = (uint8_t)(size >> 8);
	buf[2] = (uint8_t)(size >> 16);
	buf[3] = (uint8_t)(size >> 24);
	buf[4] = (uint8_t)(id & 0xff);
	buf[5] = (uint8_t)(id >> 8);
	buf[6] = (uint8_t)(id >> 16);
	buf[7] = (uint8_t)(id >> 24);
	g_send(CMD_INTENT, (uint16_t)lcid, 1, buf, 8);
}

static void
g_advertise_intent(uint32_t id, uint32_t size)
{
	g_advertise_intent_lcid(g_lcid, id, size);
}

static struct g_riid *
g_pick_riid(uint32_t chan, uint32_t len)
{
	struct g_riid *best = NULL;

	for (int i = 0; i < g_nriids; i++) {
		struct g_riid *r = &g_riids[i];

		if (r->in_use || r->size < len || r->chan != chan) {
			continue;
		}
		if (best == NULL || r->size > best->size) {
			best = r;
		}
	}
	return best;
}

/* Older builds picked any intent.  Keep that as a fallback so a wrong
 * channel tag can never stop the GPR/audio path from transmitting. */
static struct g_riid *
g_pick_riid_any(uint32_t len)
{
	struct g_riid *best = NULL;

	for (int i = 0; i < g_nriids; i++) {
		struct g_riid *r = &g_riids[i];

		if (r->in_use || r->size < len) {
			continue;
		}
		if (best == NULL || r->size > best->size) {
			best = r;
		}
	}
	return best;
}

static struct g_riid *
g_gpr_riid(uint32_t len)
{
	struct g_riid *r = g_pick_riid(g_rcid, len);

	if (r == NULL) {
		r = g_pick_riid_any(len);
	}
	return r;
}

static int
g_rx_one(void)
{
	uint8_t hdr[16];
	uint8_t tmp[64];
	uint32_t avail = g_rx_avail();
	uint32_t tail = g_rd(g_desc_off + 8);
	uint16_t cmd, p1;
	uint32_t p2;

	if (avail < 8) {
		return -1;
	}
	g_fifo_read(g_rx_off, g_rx_len, tail, hdr, 8);
	cmd = (uint16_t)(hdr[0] | (hdr[1] << 8));
	p1 = (uint16_t)(hdr[2] | (hdr[3] << 8));
	p2 = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
	    ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

	switch (cmd) {
	case CMD_VERSION:
		printf("SWEET GLINK: remote VERSION %u features %#x\n", p1, p2);
		g_send(CMD_VERSION_ACK, GLINK_VERSION_1, GLINK_FEATURE_REUSE,
		    NULL, 0);
		g_ver_acked = 1;
		g_rx_advance(8);
		break;

	case CMD_VERSION_ACK:
		printf("SWEET GLINK: VERSION_ACK %u features %#x\n", p1, p2);
		g_ver_acked = 1;
		g_rx_advance(8);
		break;

	case CMD_OPEN: {
		uint32_t n = p2;

		if (n > 56) {
			printf("SWEET GLINK: bad OPEN name len %u\n", n);
			g_rx_advance(8);
			break;
		}
		if (avail < 8 + n) {
			return -1;
		}
		g_fifo_read(g_rx_off, g_rx_len, tail + 8, tmp, n);
		tmp[n] = 0;
		printf("SWEET GLINK: remote OPEN rcid=%u name=%s\n", p1, tmp);
		if (n >= 9 &&
		    tmp[0] == 'a' && tmp[1] == 'd' && tmp[2] == 's' &&
		    tmp[3] == 'p' && tmp[4] == '_' && tmp[5] == 'a' &&
		    tmp[6] == 'p' && tmp[7] == 'p' && tmp[8] == 's') {
			/* The rcid of *our* channel, not of the last OPEN. */
			g_rcid = p1;
			g_remote_open = 1;
		} else if (n >= 18) {
			int pm = 1;

			for (int k = 0; k < 18; k++) {
				if (tmp[k] != (uint8_t)G_BATT_NAME[k]) {
					pm = 0;
					break;
				}
			}
			if (pm) {
				g_batt_rcid = p1;
				g_batt_remote = 1;
			}
		}
		g_rx_advance((8 + p2 + 7u) & ~7u);
		break;
	}

	case CMD_OPEN_ACK:
		printf("SWEET GLINK: OPEN_ACK lcid=%u\n", p1);
		if (p1 == G_BATT_LCID) {
			g_batt_acked = 1;
		} else {
			g_open_acked = 1;
		}
		g_rx_advance(8);
		break;

	case CMD_INTENT: {
		uint32_t count = p2;

		if (count > 8) {
			count = 8;
		}
		if (avail < 8 + count * 8) {
			return -1;
		}
		g_fifo_read(g_rx_off, g_rx_len, tail + 8, tmp, count * 8);
		for (uint32_t i = 0; i < count; i++) {
			uint32_t size = (uint32_t)tmp[i * 8] |
			    ((uint32_t)tmp[i * 8 + 1] << 8) |
			    ((uint32_t)tmp[i * 8 + 2] << 16) |
			    ((uint32_t)tmp[i * 8 + 3] << 24);
			uint32_t id = (uint32_t)tmp[i * 8 + 4] |
			    ((uint32_t)tmp[i * 8 + 5] << 8) |
			    ((uint32_t)tmp[i * 8 + 6] << 16) |
			    ((uint32_t)tmp[i * 8 + 7] << 24);

			printf("SWEET GLINK: remote intent id=%u size=%u\n",
			    id, size);
			{
				int found = 0;

				/* A re-advertised id means the remote finished
				 * with it: reset it instead of filling the
				 * table with duplicates. */
				for (int k = 0; k < g_nriids; k++) {
					if (g_riids[k].id == id) {
						g_riids[k].size = size;
						g_riids[k].chan = p1;
						g_riids[k].in_use = 0;
						found = 1;
						break;
					}
				}
				if (!found && g_nriids < G_NRIIDS) {
					g_riids[g_nriids].id = id;
					g_riids[g_nriids].size = size;
					g_riids[g_nriids].chan = p1;
					g_riids[g_nriids].in_use = 0;
					g_nriids++;
				}
			}
		}
		g_rx_advance((8 + count * 8 + 7u) & ~7u);
		break;
	}

	case CMD_RX_DONE:
	case CMD_RX_DONE_W_REUSE:
		for (int i = 0; i < g_nliids; i++) {
			if (g_liids[i].id == p2) {
				g_liids[i].filled = 0;
			}
		}
		/* The peer returns the RX intent we used for a TX_DATA.
		 * Without this the battmgr channel's intent pool stayed
		 * in_use after the boot probe, so every periodic refresh
		 * found "no intent" and the cache never updated. */
		{
			int match = -1, fallback = -1;

			for (int i = 0; i < g_nriids; i++) {
				if (!g_riids[i].in_use ||
				    g_riids[i].id != p2) {
					continue;
				}
				if (g_riids[i].chan == p1) {
					match = i;
					break;
				}
				if (fallback < 0) {
					fallback = i;
				}
			}
			if (match < 0) {
				match = fallback;
			}
			if (match >= 0) {
				g_riids[match].in_use = 0;
			}
		}
		g_rx_advance(8);
		break;

	case CMD_RX_INTENT_REQ: {
		uint32_t sz = p2;
		uint32_t lcid = (p1 == g_batt_rcid) ? G_BATT_LCID : g_lcid;

		printf("SWEET GLINK: remote wants intent chan=%u size=%u\n",
		    p1, sz);
		g_send(CMD_RX_INTENT_REQ_ACK, (uint16_t)lcid, 1, NULL, 0);
		for (int i = 0; i < G_NDYN; i++) {
			if (g_dyn_liids[i].id != 0) {
				continue;
			}
			if (sz == 0 || sz > sizeof(g_dyn_liids[i].buf)) {
				sz = sizeof(g_dyn_liids[i].buf);
			}
			g_dyn_liids[i].id = g_dyn_next++;
			g_dyn_liids[i].size = sz;
			g_dyn_liids[i].filled = 0;
			g_dyn_liids[i].lcid = lcid;
			g_advertise_intent_lcid(lcid, g_dyn_liids[i].id, sz);
			printf("SWEET GLINK: provided intent id=%u size=%u "
			    "chan=%u\n", g_dyn_liids[i].id, sz, lcid);
			break;
		}
		g_rx_advance(8);
		break;
	}

	case CMD_RX_INTENT_REQ_ACK:
		printf("SWEET GLINK: intent req ack granted=%u\n", p2);
		g_rx_advance(8);
		break;

	case CMD_TX_DATA:
	case CMD_TX_DATA_CONT: {
		uint32_t chunk, left, liid;

		if (avail < 16) {
			return -1;
		}
		g_fifo_read(g_rx_off, g_rx_len, tail, hdr, 16);
		chunk = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
		    ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
		left = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
		    ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
		liid = p2;
		/* A bogus or desynced header must not spin the kernel thread. */
		if (chunk > g_rx_len || left > g_rx_len) {
			printf("SWEET GLINK: bad TX_DATA chunk %u left %u\n",
			    chunk, left);
			g_rx_advance(8);
			break;
		}
		if (avail < 16 + chunk) {
			return -1;
		}
		if (g_batt_rcid != 0 && p1 == g_batt_rcid) {
			if (g_batt_fill + chunk <= sizeof(g_batt_rsp)) {
				g_fifo_read(g_rx_off, g_rx_len, tail + 16,
				    g_batt_rsp + g_batt_fill, chunk);
				g_batt_fill += chunk;
			}
			if (left == 0) {
				g_batt_rsp_len = g_batt_fill;
				if (g_batt_rsp_len > sizeof(g_batt_rsp)) {
					g_batt_rsp_len = sizeof(g_batt_rsp);
				}
				g_batt_ready = 1;
				g_batt_fill = 0;
				g_send(CMD_RX_DONE_W_REUSE,
				    (uint16_t)G_BATT_LCID, liid, NULL, 0);
			}
			g_rx_advance((16 + chunk + 7u) & ~7u);
			break;
		}
		{
			int dyn = -1;

			for (int i = 0; i < G_NDYN; i++) {
				if (g_dyn_liids[i].id != 0 &&
				    g_dyn_liids[i].id == liid) {
					dyn = i;
					break;
				}
			}
			if (dyn >= 0) {
				if (g_dyn_liids[dyn].filled + chunk <=
				    g_dyn_liids[dyn].size) {
					g_fifo_read(g_rx_off, g_rx_len,
					    tail + 16,
					    g_dyn_liids[dyn].buf +
					    g_dyn_liids[dyn].filled, chunk);
					g_dyn_liids[dyn].filled += chunk;
				}
				if (left == 0) {
					g_dyn_liids[dyn].filled = 0;
					g_send(CMD_RX_DONE_W_REUSE,
					    (uint16_t)g_dyn_liids[dyn].lcid,
					    liid, NULL, 0);
				}
				g_rx_advance((16 + chunk + 7u) & ~7u);
				break;
			}
		}
		for (int i = 0; i < g_nliids; i++) {
			if (g_liids[i].id != liid) {
				continue;
			}
			if (g_liids[i].filled + chunk <= g_liids[i].size) {
				g_fifo_read(g_rx_off, g_rx_len, tail + 16,
				    g_liids[i].buf + g_liids[i].filled, chunk);
				g_liids[i].filled += chunk;
			}
			if (left == 0) {
				g_rsp_len = g_liids[i].filled;
				if (g_rsp_len > sizeof(g_rsp)) {
					g_rsp_len = sizeof(g_rsp);
				}
				for (uint32_t k = 0; k < g_rsp_len; k++) {
					g_rsp[k] = g_liids[i].buf[k];
				}
				g_rsp_ready = 1;
				g_liids[i].filled = 0;
				/* RX_DONE carries the *local* channel id. */
				g_send(CMD_RX_DONE_W_REUSE,
				    (uint16_t)g_lcid, liid, NULL, 0);
			}
			break;
		}
		g_rx_advance((16 + chunk + 7u) & ~7u);
		break;
	}

	case CMD_READ_NOTIF:
		g_kick();
		g_rx_advance(8);
		break;

	case CMD_CLOSE:
	case CMD_CLOSE_ACK:
		printf("SWEET GLINK: close cmd %u p1=%u\n", cmd, p1);
		g_rx_advance(8);
		break;

	default:
		printf("SWEET GLINK: unknown cmd %u p1=%u p2=%#x\n", cmd, p1, p2);
		g_rx_advance(8);
		break;
	}
	return 0;
}

/* Drain incoming messages for up to budget_ms; returns messages seen. */
static int
g_drain(unsigned budget_ms)
{
	int seen = 0;
	unsigned waited = 0;

	for (;;) {
		int got = 0;

		while (got < 256 && g_rx_one() == 0) {
			seen++;
			got++;
		}
		if (waited >= budget_ms || got >= 256) {
			break;
		}
		if (!got) {
			g_wait(5);
			waited += 5;
		}
	}
	return seen;
}

static int
g_wait_flag(const int *flag, unsigned ms)
{
	unsigned i = 0;

	while (i < ms) {
		g_drain(0);
		if (*flag) {
			return 1;
		}
		g_wait(5);
		i += 5;
	}
	g_drain(0);
	return *flag;
}

static int
g_wait_riid(uint32_t len, unsigned ms)
{
	unsigned i = 0;

	while (i < ms) {
		if (g_gpr_riid(len) != NULL) {
			return 1;
		}
		g_drain(0);
		g_wait(5);
		i += 5;
	}
	g_drain(0);
	return g_gpr_riid(len) != NULL;
}

static void
g_send_gpr(const uint8_t *pkt, uint32_t len)
{
	struct g_riid *r;

	if (len > 4096) {
		len = 4096;
	}
	if (g_gpr_riid(len) == NULL) {
		printf("SWEET GLINK: requesting %u-byte intent\n", len);
		g_send(CMD_RX_INTENT_REQ, (uint16_t)g_lcid, len, NULL, 0);
		if (!g_wait_riid(len, 5000)) {
			printf("SWEET GLINK: no remote intent for %u bytes "
			    "(rx_avail=%u)\n", len, g_rx_avail());
			return;
		}
	}
	r = g_gpr_riid(len);
	if (r == NULL) {
		return;
	}
	r->in_use = 1;
	printf("SWEET GLINK: TX_DATA iid=%u len=%u\n", r->id, len);
	g_send_tx_data(r->id, pkt, len);
}

static void
g_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* PRM "vote for the LPASS hardware core" request. */
static void
g_prm_vote_lpass(void)
{
	uint8_t pkt[80];
	uint32_t pkt_size = 24 + 16 + 20;   /* GPR + APM cmd + param */

	for (uint32_t i = 0; i < sizeof(pkt); i++) {
		pkt[i] = 0;
	}
	/* GPR header: version 0, hdr_size 6 words, pkt_size */
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, GPR_PRM_SERVICE);      /* src_port */
	g_put32(pkt + 12, GPR_PRM_MODULE);      /* dest_port */
	g_put32(pkt + 16, 1);                   /* token */
	g_put32(pkt + 20, PRM_CMD_REQUEST_HW_RSC);
	/* APM command header */
	g_put32(pkt + 24 + 0, 0);               /* payload_address_lsw */
	g_put32(pkt + 24 + 4, 0);               /* payload_address_msw */
	g_put32(pkt + 24 + 8, 0);               /* mem_map_handle */
	g_put32(pkt + 24 + 12, 20);             /* payload_size */
	/* apm_module_param_data + hw_clk_id */
	g_put32(pkt + 40 + 0, GPR_PRM_MODULE);  /* module_instance_id */
	g_put32(pkt + 40 + 4, PARAM_ID_RSC_HW_CORE);
	g_put32(pkt + 40 + 8, 4);               /* param_size */
	g_put32(pkt + 40 + 12, 0);              /* error_code */
	g_put32(pkt + 40 + 16, Q6PRM_HW_CORE_LPASS);

	printf("SWEET GLINK: PRM vote lpass core (%u bytes)\n", pkt_size);
	g_send_gpr(pkt, pkt_size);
}

static uint32_t g_le32(const uint8_t *p);
static void g_smmu_faults(const char *tag);

#define PARAM_ID_RSC_AUDIO_HW_CLK 0x0800102Cu

/* Ask PRM for one LPASS clock (q6prm_set_lpass_clock equivalent).
 * Returns 0 when the ADSP answered with status 0. */
static int
g_prm_vote_clock_ex(uint32_t clk_id, uint32_t freq, uint32_t attr, uint32_t root)
{
	uint8_t pkt[96];
	uint32_t pkt_size = 24 + 16 + 36;

	for (uint32_t i = 0; i < sizeof(pkt); i++) {
		pkt[i] = 0;
	}
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, GPR_PRM_SERVICE);
	g_put32(pkt + 12, GPR_PRM_MODULE);
	g_put32(pkt + 16, 3);
	g_put32(pkt + 20, PRM_CMD_REQUEST_HW_RSC);
	g_put32(pkt + 24 + 0, 0);
	g_put32(pkt + 24 + 4, 0);
	g_put32(pkt + 24 + 8, 0);
	g_put32(pkt + 24 + 12, 36);             /* payload_size */
	g_put32(pkt + 40 + 0, GPR_PRM_MODULE);
	g_put32(pkt + 40 + 4, PARAM_ID_RSC_AUDIO_HW_CLK);
	g_put32(pkt + 40 + 8, 20);              /* param_size */
	g_put32(pkt + 40 + 12, 0);
	g_put32(pkt + 56 + 0, 1);               /* num_clk_id */
	g_put32(pkt + 56 + 4, clk_id);
	g_put32(pkt + 56 + 8, freq);
	g_put32(pkt + 56 + 12, attr);           /* clock_attri */
	g_put32(pkt + 56 + 16, root);           /* clock_root (DT ext-clk-src) */
	printf("SWEET GLINK: PRM clk %#x @ %u attr=%u root=%u\n",
	    clk_id, freq, attr, root);

	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 1500; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (!g_rsp_ready || g_rsp_len < 32) {
		printf("SWEET GLINK: clk %#x no response\n", clk_id);
		return -1;
	}
	if (g_le32(g_rsp + 28) != 0) {
		printf("SWEET GLINK: clk %#x attr=%u root=%u status %#x\n", clk_id,
		    attr, root, g_le32(g_rsp + 28));
		return -1;
	}
	printf("SWEET GLINK: clk %#x attr=%u root=%u ok\n", clk_id, attr, root);
	return 0;
}

/*
 * The vendor gets clock_attri from a driver constant and clock_root from the
 * DT, so try the plausible attributes first and only then fall back to a
 * root scan.  (va_core_clk: id 775/0x307 src 2; rx_core_clk: id 782/0x30e
 * src 5 - see logs/dmesg-after-4320.txt.)
 */
static int
g_prm_vote_clock(uint32_t clk_id, uint32_t freq, uint32_t root)
{
	for (uint32_t a = 0; a < 4; a++) {
		if (g_prm_vote_clock_ex(clk_id, freq, a, root) == 0) {
			return 0;
		}
	}
	for (uint32_t r = 0; r < 8; r++) {
		if (r == root) {
			continue;
		}
		if (g_prm_vote_clock_ex(clk_id, freq, 0, r) == 0) {
			return 0;
		}
	}
	return -1;
}

/* APM_CMD_GET_SPF_STATE on the spf_core service: proves APM is alive even
 * if the PRM vote is refused.  src_port and dest_port are both 1. */
static void
g_apm_state_query(void)
{
	uint8_t pkt[32];
	uint32_t n = 24;

	for (uint32_t i = 0; i < sizeof(pkt); i++) {
		pkt[i] = 0;
	}
	g_put32(pkt + 0, (6u << 4) | (n << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, 1);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, 2);
	g_put32(pkt + 20, 0x01001021u);         /* APM_CMD_GET_SPF_STATE */
	printf("SWEET GLINK: APM get_spf_state\n");
	g_send_gpr(pkt, n);
}

/* Same query, but wait for the answer so a dead/restarted APM is visible. */
static void
g_apm_state_query_sync(const char *tag)
{
	uint8_t pkt[32];
	uint32_t n = 24;
	static const uint32_t words[] = { 16, 20, 24, 28, 32, 36 };

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (n << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, 1);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, 2);
	g_put32(pkt + 20, 0x01001021u);
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, n);
	for (unsigned i = 0; i < 2000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (!g_rsp_ready) {
		printf("SWEET GLINK: spf_state %s: no rsp\n", tag);
	} else {
		printf("SWEET GLINK: spf_state %s len=%u", tag, g_rsp_len);
		for (unsigned k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
			if (words[k] + 4 <= g_rsp_len) {
				printf(" w%u=%#x", words[k],
				    g_le32(g_rsp + words[k]));
			}
		}
		printf("\n");
	}
	sweet_log_sync();
}

/* SPMI reads only: RTC on PMK8350 (SID 0, 0x61xx) and the PM8350B
 * haptics engine (SID 3, 0xf0xx/0xf1xx). */
static void
g_spmi_dump(const char *tag, unsigned sid, unsigned base, unsigned n)
{
	uint8_t v;
	int any = 0;

	if (sweet_spmi_init() != 0) {
		printf("SWEET SPMI: %s: not ready\n", tag);
		return;
	}
	printf("SWEET SPMI: %s sid=%u base=%#x:", tag, sid, base);
	for (unsigned i = 0; i < n; i++) {
		if (sweet_spmi_read(sid, base + i, &v) == 0) {
			printf(" %02x=%02x", base + i, v);
			any++;
		}
	}
	if (!any) {
		printf(" (no access)");
	}
	printf("\n");
	sweet_log_sync();
}

/* MMIO window dump (SMMU/TBU state).  Reads only. */
static void
g_mmio_dump(const char *tag, uint64_t pa, unsigned n)
{
	vm_offset_t v = g_map_win((uint64_t)pa, 0x1000);

	if (!v) {
		printf("SWEET MMIO: %s: map failed\n", tag);
		return;
	}
	printf("SWEET MMIO: %s @%#llx:", tag, pa);
	for (unsigned i = 0; i < n; i++) {
		printf(" +%02x=%08x", i * 4,
		    *(volatile uint32_t *)(v + i * 4));
	}
	printf("\n");
	sweet_log_sync();
}

/*
 * Set the wall clock from sweettime=<unix seconds> in the boot args.  The
 * image is built on the host, so the value is accurate to boot latency and
 * does not depend on the PMIC RTC's epoch or access rights.
 */
static void
g_clock_from_bootarg(void)
{
	int secs = 0;

	if (PE_parse_boot_argn("sweettime", &secs, sizeof(secs)) &&
	    secs > 1577836800) {
		clock_set_calendar_microtime((clock_sec_t)secs, 0);
		printf("SWEET RTC: clock set from sweettime=%d\n", secs);
		sweet_log_sync();
	}
}

/*
 * PMK8350 RTC (SID 0).  Mainline rtc-pm8xxx.c pmk8350_regs:
 *   ctrl 0x6146 (bit7 enable), write 0x6140..0x6143,
 *   read 0x6148..0x614b, alarm_ctrl 0x6246 (bit7 enable).
 * The counter is plain Unix seconds (TWRP reads the same register at 0x6148).
 */
static uint32_t
g_rtc_read_pmic(void)
{
	uint8_t b[4], lsb;

	if (sweet_spmi_read_block(0, 0x6148u, b, sizeof(b)) != 0) {
		return 0;
	}
	/* Re-read the LSB: a carry between the two reads means the value
	 * straddled a second boundary. */
	if (sweet_spmi_read_block(0, 0x6148u, &lsb, 1) == 0 && lsb < b[0]) {
		if (sweet_spmi_read_block(0, 0x6148u, b, sizeof(b)) != 0) {
			return 0;
		}
	}
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
g_rtc_set_from_pmic(void)
{
	uint32_t secs = g_rtc_read_pmic();

	if (secs == 0) {
		printf("SWEET RTC: block read failed\n");
	} else if (secs < 1577836800u || secs > 2524608000u) {
		/* APID 51 (PMK8350 RTC) is owned by the PMIC firmware (EE4),
		 * so AP writes are refused and Android/TWRP cannot set it
		 * either.  The calendar above already comes from sweettime,
		 * which mkimage.sh stamps into the cmdline at build time. */
		printf("SWEET RTC: pmic=%u raw (EE4-owned; AP cannot set it,"
		    " calendar from sweettime)\n", secs);
	} else {
		printf("SWEET RTC: pmic=%u (plausible)\n", secs);
		clock_set_calendar_microtime((clock_sec_t)secs, 0);
		printf("SWEET RTC: kernel calendar set\n");
	}
	sweet_log_sync();
}

static void
g_hap_wr(unsigned addr, uint8_t val, const char *what)
{
	printf("SWEET HAP: %s %#x<-%02x rc=%d\n", what, addr, val,
	    sweet_spmi_write(3, addr, &val, 1));
}

/*
 * PM8350B (HAP520) direct play: sine drive at the DT LRA period, 3.6 V,
 * full amplitude, 300 ms.  The register values follow qcom-hv-haptics.c
 * from the marble-s-oss 5.10 tree.  hBoost is left as found unless the
 * VREG_EN bit is clear, in which case it is turned on explicitly.
 */
static void
g_haptics_try(void)
{
	uint8_t rev1 = 0, rev = 0, ptn = 0, v = 0;

	if (sweet_spmi_init() != 0) {
		return;
	}
	/* REVISION1 (0x00) reads 0 on this HAP520; the hardware type is in
	 * REVISION2 (0x01), where 2 means HAP520. */
	(void)sweet_spmi_read(3, 0xf000, &rev1);
	for (unsigned i = 0; i < 3; i++) {
		uint8_t r = 0;

		if (sweet_spmi_read(3, 0xf001, &r) == 0 && r >= 1 && r <= 5) {
			rev = r;
			break;
		}
		printf("SWEET HAP: rev2 try %u -> %#x\n", i, r);
		delay(2000);
	}
	if (rev == 0) {
		printf("SWEET HAP: no cfg revision (rev1=%#x)\n", rev1);
		return;
	}
	(void)sweet_spmi_read(3, 0xf101, &ptn);
	printf("SWEET HAP: cfg=%#x/%#x ptn=%#x boost-vreg=%#x ", rev1, rev, ptn,
	    sweet_spmi_read(3, 0xf246, &v) == 0 ? v : 0xffu);
	printf("hboost-follow=%#x\n",
	    sweet_spmi_read(3, 0xf241, &v) == 0 ? v : 0xffu);

	/* The DT caps this LRA at qcom,vmax-mv = 1450; the write is in
	 * 50 mV steps (1450/50 = 29).  Driving 3600 mV tripped the HAP520
	 * fault output (0xf00c = 0x2) before the LRA could move. */
	g_hap_wr(0xf048, 29, "vmax");
	g_hap_wr(0xf049, 0x01, "wave");
	/* Open-loop LRA period: 5882 us / 5 us per step = 0x0498. */
	{
		uint8_t t[2] = { 0x98, 0x04 };
		printf("SWEET HAP: tlra %#x rc=%d\n", 0x498,
		    sweet_spmi_write(3, 0xf05c, t, sizeof(t)));
	}
	/* Keep the direct-play amplitude within the DT effect range (max
	 * ~0x7f).  0xff tripped the HAP fault bit 0x2 during play. */
	g_hap_wr(0xf126, 0x7f, "direct amplitude");
	if (sweet_spmi_read(3, 0xf063, &v) == 0) {
		g_hap_wr(0xf063, v & ~1u, "autores off");
	}
	/*
	 * hBoost lives on the PM8350B's hap_cfg SDAM (sdam@9d00 in the live
	 * DT, phandle 0x5e2).  The charger firmware asks the PBS for the rail
	 * by writing PBS_ARG 0x42 = 1 and then starting it with PBS_TRIG
	 * 0xe5.  The owner guard refuses the store when another execution
	 * environment owns this APID, so this cannot disturb the charger.
	 */
	if (sweet_spmi_read(3, 0xf246, &v) == 0) {
		printf("SWEET HAP: boost vreg=%#x\n", v);
		if ((v & 1u) == 0) {
			uint8_t on = 1;
			int r1 = sweet_spmi_write(0, 0x9d42, &on, 1);
			int r2 = 0;

			if (r1 == 0) {
				r2 = sweet_spmi_write(0, 0x9de5, &on, 1);
			}
			printf("SWEET HAP: hboost trigger rc=%d/%d\n", r1, r2);
			delay(20000);
			if (sweet_spmi_read(3, 0xf246, &v) == 0) {
				printf("SWEET HAP: boost vreg now=%#x\n", v);
			}
		}
	}
	if (sweet_spmi_read(3, 0xf068, &v) == 0) {
		g_hap_wr(0xf068, v | 1u, "vreg ready");
	}
	/* The vendor's enable path clears the latched SC / auto-res / hBoost
	 * ready faults before every play; without it PLAY can be ignored. */
	g_hap_wr(0xf066, 0x07, "fault clear");
	{
		/* Read the fault register immediately after the clear, before
		 * enable and play: a non-zero here is a fault that the clear did
		 * not remove (still latched / owned elsewhere), while a zero that
		 * later turns non-zero is a fault raised by this very pulse.
		 * Read-only, so the drive sequence is unchanged. */
		uint8_t f0 = 0;

		if (sweet_spmi_read(3, 0xf00c, &f0) == 0) {
			printf("SWEET HAP: fault after clear=%#x\n", f0);
		}
	}
	g_hap_wr(0xf046, 0x80, "enable");
	delay(2000);
	g_hap_wr(0xf04c, 0x81, "play");
	delay(300000);
	{
		uint8_t f = 0;
		if (sweet_spmi_read(3, 0xf00c, &f) == 0) {
			printf("SWEET HAP: fault during play=%#x\n", f);
		}
	}
	g_hap_wr(0xf04c, 0x00, "stop");
	g_hap_wr(0xf046, 0x00, "disable");
	{
		uint8_t fault = 0, hi = 0, lo = 0, irt = 0;
		(void)sweet_spmi_read(3, 0xf00c, &fault);
		(void)sweet_spmi_read(3, 0xf009, &hi);
		(void)sweet_spmi_read(3, 0xf00a, &lo);
		(void)sweet_spmi_read(3, 0xf010, &irt);
		printf("SWEET HAP: fault=%#x auto_res=%#x/%#x irt=%#x\n",
		    fault, hi, lo, irt);
	}
	if (sweet_spmi_read(3, 0xf046, &v) == 0) {
		printf("SWEET HAP: final en=%#x\n", v);
	}
	sweet_log_sync();
}

/*
 * User-space trigger for the haptics engine.  iOS's own haptic stack is not
 * present in this kernel, so this gives bring-up a deterministic way to fire
 * the hardware once the system is running:
 *
 *     sysctl -w kern.sweetvibe=1
 */
static int
sweet_vibe_sysctl SYSCTL_HANDLER_ARGS
{
	int v = 0;
	int err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL) {
		return err;
	}
	if (v != 0) {
		g_haptics_try();
	}
	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, sweetvibe,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, 0, 0,
    sweet_vibe_sysctl, "I", "trigger the PM8350B haptics engine");

/*
 * Called from the key driver so a physical key press produces haptic
 * feedback.  The haptics sequence sleeps ~300 ms; sweet_keys polls from its
 * own kernel thread, so that delay is acceptable here.  Re-entry is
 * dropped rather than queued.
 */
void
sweet_haptics_kick(void)
{
	static volatile int busy;

	if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL) != 0) {
		return;
	}
	g_haptics_try();
	__atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/*
 * PMK8350 PON (HLOS block at 0x1300) power-off.  This mirrors the vendor
 * msm-poweroff.c: program PS_HOLD_RST_CTL for PON_POWER_OFF_SHUTDOWN
 * (0x4), then the platform expert deasserts PS_HOLD via SCM.  The HLOS PON
 * block (ppid 0x13) is the AP's copy; the PMIC firmware keeps the real
 * PON/RTC APIDs.
 */
int
sweet_pon_power_off(void)
{
	uint8_t sub = 0, v = 0;
	unsigned ctl, ctl2;
	int rc = 0;

	/* Log every step and force it to the raw log ring: if the PMIC
	 * arbitration is gone, this path hangs and the 30 s halt watchdog
	 * panics with no other trace. */
	printf("SWEET PON: begin\n");
	sweet_log_sync();
	if (sweet_spmi_init() != 0) {
		printf("SWEET PON: spmi init failed\n");
		sweet_log_sync();
		return -1;
	}
	if (sweet_spmi_read(0, 0x1305u, &sub) != 0) {
		sub = 0x09u;		/* PMK8350 PON_GEN3_HLOS */
	}
	ctl = (sub == 0x08u || sub == 0x09u) ? 0x1352u : 0x135Au;
	ctl2 = ctl + 1;
	printf("SWEET PON: subtype=%#x ctl=%#x\n", sub, ctl);
	printf("SWEET PON: step 1 read ctl2=%#x\n", ctl2);
	if (sweet_spmi_read(0, ctl2, &v) == 0) {
		v &= (uint8_t)~0x80u;
		rc |= sweet_spmi_write(0, ctl2, &v, 1);
	}
	printf("SWEET PON: step 1 done rc=%d\n", rc);
	delay(500);
	printf("SWEET PON: step 2 set shutdown ctl=%#x\n", ctl);
	if (sweet_spmi_read(0, ctl, &v) == 0) {
		v = (uint8_t)((v & ~0x0Fu) | 0x04u);	/* SHUTDOWN */
		rc |= sweet_spmi_write(0, ctl, &v, 1);
	}
	printf("SWEET PON: step 2 done rc=%d\n", rc);
	printf("SWEET PON: step 3 enable ctl2=%#x\n", ctl2);
	if (sweet_spmi_read(0, ctl2, &v) == 0) {
		v |= 0x80u;
		rc |= sweet_spmi_write(0, ctl2, &v, 1);
	}
	printf("SWEET PON: off rc=%d, deassert PS_HOLD\n", rc);
	sweet_log_sync();
	return rc;
}

static void
g_dump(const char *tag, const uint8_t *p, uint32_t len)
{
	char line[80];
	uint32_t n = len > 64 ? 64 : len;

	printf("SWEET GLINK: %s (%u bytes)\n", tag, len);
	for (uint32_t off = 0; off < n; off += 16) {
		static const char hex[] = "0123456789abcdef";
		int pos = 0;
		uint32_t k = (n - off) > 16 ? 16 : (n - off);

		for (uint32_t i = 0; i < k; i++) {
			line[pos++] = hex[p[off + i] >> 4];
			line[pos++] = hex[p[off + i] & 0xf];
			line[pos++] = ' ';
		}
		line[pos] = 0;
		printf("SWEET GLINK:   +%02x %s\n", off, line);
	}
}

static uint32_t
g_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- SMEM parsing ---- */

struct g_part {
	uint32_t off;
	uint32_t size;
	uint16_t host0;
	uint16_t host1;
	int      ok;
	int      global;
};

static int
g_smem_partition(struct g_part *out)
{
	uint32_t num;

	printf("SWEET GLINK: smem sbl version %#x initialized %u\n",
	    g_rd(0x5c), g_rd(0xc0));
	if (g_rd(G_TOC_OFF + 0) != 0x434f5424u /* "$TOC" */) {
		printf("SWEET GLINK: no $TOC at %#x (got %#x)\n",
		    G_TOC_OFF, g_rd(G_TOC_OFF + 0));
		return 0;
	}
	num = g_rd(G_TOC_OFF + 8);
	printf("SWEET GLINK: ptable version %u entries %u\n",
	    g_rd(G_TOC_OFF + 4), num);
	if (num > 32) {
		num = 32;
	}
	for (uint32_t i = 0; i < num; i++) {
		uint32_t e = G_TOC_OFF + 32 + i * 48;
		uint32_t off = g_rd(e + 0);
		uint32_t size = g_rd(e + 4);
		uint16_t h0 = g_rd16(e + 12);
		uint16_t h1 = g_rd16(e + 14);

		if (off == 0 || size == 0) {
			continue;
		}
		printf("SWEET GLINK: part[%u] off=%#x size=%#x hosts=%u,%u\n",
		    i, off, size, h0, h1);
		if (out->ok == 0 && h0 == 0 && h1 == 2) {
			out->off = off;
			out->size = size;
			out->host0 = h0;
			out->host1 = h1;
			out->ok = 1;
			out->global = 0;
		} else if (out->ok == 0 && h0 == 0xfffe && h1 == 0xfffe) {
			out->off = off;
			out->size = size;
			out->host0 = h0;
			out->host1 = h1;
			out->ok = 1;
			out->global = 1;
		}
	}
	if (!out->ok) {
		printf("SWEET GLINK: no AP/ADSP or global partition\n");
		return 0;
	}
	if (g_rd(out->off + 0) != 0x54525024u /* "$PRT" */) {
		printf("SWEET GLINK: bad partition magic %#x\n",
		    g_rd(out->off + 0));
		return 0;
	}
	printf("SWEET GLINK: partition %s free_uncached=%#x\n",
	    out->global ? "global" : "apps/adsp", g_rd(out->off + 12));
	return 1;
}

static int
g_find_item(const struct g_part *p, uint16_t want, uint32_t *data_off,
    uint32_t *data_len)
{
	uint32_t e = p->off + 0x20;
	uint32_t end = p->off + g_rd(p->off + 12);

	while (e < end) {
		uint16_t canary = g_rd16(e + 0);
		uint16_t item = g_rd16(e + 2);
		uint32_t size = g_rd(e + 4);
		uint16_t pad_data = g_rd16(e + 8);
		uint16_t pad_hdr = g_rd16(e + 10);

		if (canary != 0xa5a5) {
			printf("SWEET GLINK: bad item canary %#x at %#x\n",
			    canary, e);
			return 0;
		}
		if (item == want) {
			*data_off = e + 16 + pad_hdr;
			*data_len = size - pad_data;
			return 1;
		}
		e = e + 16 + pad_hdr + size;
	}
	return 0;
}

static int
g_smem_init(void)
{
	struct g_part part;
	uint32_t off, len;

	for (uint32_t i = 0; i < sizeof(part); i++) {
		((uint8_t *)&part)[i] = 0;
	}
	g_smem = g_win_alloc(G_SMEM_SZ + PAGE_SIZE);
	if (g_smem == 0) {
		printf("SWEET GLINK: no VA for smem\n");
		return 0;
	}
	g_smem = g_win_map(g_smem, G_SMEM_PA, G_SMEM_SZ);

	if (!g_smem_partition(&part)) {
		return 0;
	}
	if (!g_find_item(&part, 478, &off, &len) || len < 32) {
		printf("SWEET GLINK: item 478 (descriptor) missing\n");
		return 0;
	}
	g_desc_off = off;
	g_desc_len = len;
	printf("SWEET GLINK: descriptor %#x (%u bytes): %u %u %u %u\n",
	    off, len, g_rd(off + 0), g_rd(off + 4), g_rd(off + 8),
	    g_rd(off + 12));

	if (!g_find_item(&part, 479, &off, &len)) {
		printf("SWEET GLINK: item 479 (tx fifo) missing\n");
		return 0;
	}
	g_tx_off = off;
	g_tx_len = len;
	if (!g_find_item(&part, 480, &off, &len)) {
		printf("SWEET GLINK: item 480 (rx fifo) missing\n");
		return 0;
	}
	g_rx_off = off;
	g_rx_len = len;
	printf("SWEET GLINK: tx %#x (%u) rx %#x (%u)\n",
	    g_tx_off, g_tx_len, g_rx_off, g_rx_len);

	/* Print the item table so the next run has ground truth. */
	{
		uint32_t e = part.off + 0x20;
		uint32_t end = part.off + g_rd(part.off + 12);
		int n = 0;

		while (e < end && n < 300) {
			uint16_t item = g_rd16(e + 2);
			uint32_t size = g_rd(e + 4);

			if (item > 400) {
				printf("SWEET GLINK: item %u size %u\n",
				    item, size);
			}
			e = e + 16 + g_rd16(e + 10) + size;
			n++;
		}
		printf("SWEET GLINK: %d items scanned\n", n);
	}
	return 1;
}

static void
g_lpass_probe(int clk_va_ok, int clk_rx_ok)
{
	int both = 0, do_rx = 0, do_va = 0;

	(void)PE_parse_boot_argn("sweetglinkread", &both, sizeof(both));
	(void)PE_parse_boot_argn("sweetreadrx", &do_rx, sizeof(do_rx));
	(void)PE_parse_boot_argn("sweetreadva", &do_va, sizeof(do_va));
	if (both) {
		do_rx = 1;
		do_va = 1;
	}
	if (!do_rx && !do_va) {
		printf("SWEET GLINK: lpass read skipped (sweetglinkread=1)\n");
		return;
	}
	if (do_rx && clk_rx_ok) {
		vm_offset_t win = g_win_alloc(0x8000);

		if (win != 0) {
			vm_offset_t rx;

			printf("SWEET GLINK: reading rx_macro %#x\n",
			    G_LPASS_RX_PA);
			sweet_log_sync();   /* a panic must not eat these lines */
			rx = g_win_map(win, G_LPASS_RX_PA, 16);
			printf("SWEET GLINK: rx_macro %08x %08x %08x %08x\n",
			    *(volatile uint32_t *)(rx + 0x0),
			    *(volatile uint32_t *)(rx + 0x4),
			    *(volatile uint32_t *)(rx + 0x8),
			    *(volatile uint32_t *)(rx + 0xc));
			g_win_free(win, 0x8000);
		}
	} else if (do_rx) {
		printf("SWEET GLINK: rx read skipped (clock vote)\n");
	}
	if (do_va && clk_va_ok) {
		vm_offset_t win = g_win_alloc(0x8000);

		if (win != 0) {
			vm_offset_t va;

			printf("SWEET GLINK: reading va_macro %#x\n",
			    G_LPASS_VA_PA);
			sweet_log_sync();   /* a panic must not eat these lines */
			va = g_win_map(win, G_LPASS_VA_PA, 16);
			printf("SWEET GLINK: va_macro %08x %08x %08x %08x\n",
			    *(volatile uint32_t *)(va + 0x0),
			    *(volatile uint32_t *)(va + 0x4),
			    *(volatile uint32_t *)(va + 0x8),
			    *(volatile uint32_t *)(va + 0xc));
			g_win_free(win, 0x8000);
		}
	} else if (do_va) {
		printf("SWEET GLINK: va read skipped (clock vote)\n");
	}
}

/*
 * 0237: paired RX-macro snapshot.  A single read of the macro window cannot be
 * interpreted at all - all-zero is equally consistent with "the macro is off",
 * "these twelve bytes are reserved", and "the read never reached the device".
 * Reading the same 64 bytes before the graph is opened and again after the
 * stream is the cheapest thing that can turn it into evidence: a pre/post
 * difference is attributable to the stream, and two identical dumps tell us to
 * stop spending boots on this window.
 *
 * Gated by the same sweetreadrx=1 that arms g_lpass_probe(), and it keeps
 * g_lpass_probe's own four-word line running as a cross-check of the first
 * words against a code path that already worked.
 */
static void
g_rx_snapshot(const char *tag)
{
	int do_rx = 0;
	vm_offset_t win;
	vm_offset_t rx;

	(void)PE_parse_boot_argn("sweetreadrx", &do_rx, sizeof(do_rx));
	if (!do_rx) {
		return;
	}
	win = g_win_alloc(0x8000);
	if (win == 0) {
		printf("SWEET MARK: rxsnap %s no window\n", tag);
		sweet_log_sync();
		return;
	}
	printf("SWEET MARK: rxsnap %s reading %#x\n", tag, G_LPASS_RX_PA);
	sweet_log_sync();
	rx = g_win_map(win, G_LPASS_RX_PA, 0x800);
	/*
	 * 0239: turn the RX window into a known-answer test.  0237 read
	 * base+0x00..0x3c twice in one boot and both dumps were zero
	 * (logs/ring-rxsnap.txt:2291 pre, :2612 post), and the Android regmap dump
	 * logs/android-lpass-cdc-regmap.txt shows those same offsets as zero on a
	 * *working* system - so that range carries no information at all, and
	 * repeating it would only re-measure our own ignorance.
	 *
	 * The nine offsets below are instead the ones the vendor regmap shows as
	 * non-zero while Android's audio stack is up and silent (0x088=0x60,
	 * 0x0a4=0x0c, 0x0c0/0x0c4=0x0e, 0x400=0x04, 0x404=0x10, 0x418=0x01), plus
	 * the two it shows as zero (0x000, 0x004) kept as an alignment control:
	 * those catch a window based somewhere other than where we think.
	 *
	 * What this can decide, and nothing more: if the non-zero anchors come back
	 * non-zero, our AP-side read of the macro is real and a later pre/post
	 * difference at those offsets becomes evidence.  If they come back zero
	 * while the controls also read zero, the read is not reaching what Android
	 * reads - a dead instrument, and this window closes for good.  Either way
	 * it measures our read path, not whether anything made a sound.
	 */
	{
		static const uint32_t aoff[9] = {
		    0x000u, 0x004u, 0x088u, 0x0a4u, 0x0c0u,
		    0x0c4u, 0x400u, 0x404u, 0x418u
		};
		static const uint32_t aexp[9] = {
		    0x00u, 0x00u, 0x60u, 0x0cu, 0x0eu,
		    0x0eu, 0x04u, 0x10u, 0x01u
		};

		for (int i = 0; i < 9; i++) {
			uint32_t v = *(volatile uint32_t *)(rx + aoff[i]);

			printf("SWEET MARK: rxsnap %s @%#05x = %08x exp=%08x\n",
			    tag, aoff[i], v, aexp[i]);
			sweet_log_sync();
		}
	}
	g_win_free(win, 0x8000);
}

/*
 * 0242: read the clock-mode muxsel registers that the vendor's own live device
 * tree names outright - logs/fdt/marble-live.dts:21272, node
 * lpass-cdc-clk-rsc-mngr: rx_mclk_mode_muxsel 0x033a40d8,
 * wsa_mclk_mode_muxsel 0x033a20e0, va_mclk_mode_muxsel 0x03420000.  These are
 * absolute addresses the DT hands us, so nothing here is a datasheet guess.  The
 * obvious target ("read the mi2s2 port's status register") is NOT available: the
 * AP device tree has no TDM controller node with a reg at all, only pinctrl
 * group names - the muxsel is the clock register the vendor actually points the
 * AP at.
 *
 * Two deliberate safety choices, because this region has never been read from
 * this kernel:
 *  - each read prints its address and syncs the log BEFORE touching memory, so a
 *    fault or hang names the address that killed it instead of losing the boot;
 *  - one 0x8000 window is allocated and reused (g_win_map does its own
 *    pmap_remove), which matters here because G_PFN_MASK is 0x3fff: 0x033a40d8
 *    sits at offset 0x40d8 inside its 16 KB page, so a PAGE_SIZE window would be
 *    mapped past its end.
 */
static void
g_clk_witness(const char *tag)
{
	int wit = 0;
	vm_offset_t win;
	vm_offset_t reg;
	static const uint32_t addr[3] = { 0x033a40d8u, 0x033a20e0u, 0x03420000u };
	static const char *const name[3] = { "rx", "wsa", "va" };

	(void)PE_parse_boot_argn("sweetclkwit", &wit, sizeof(wit));
	if (!wit) {
		return;
	}
	win = g_win_alloc(0x8000);
	if (win == 0) {
		printf("SWEET MARK: clkwit %s no window\n", tag);
		sweet_log_sync();
		return;
	}
	for (int i = 0; i < 3; i++) {
		printf("SWEET MARK: clkwit %s %s @%#x =", tag, name[i], addr[i]);
		sweet_log_sync();
		reg = g_win_map(win, (uint64_t)addr[i], 4);
		printf(" %#010x\n", *(volatile uint32_t *)reg);
		sweet_log_sync();
	}
	g_win_free(win, 0x8000);
}


/* ---- AudioReach playback graph ------------------------------------------
 * Two definitions are used.  g_graph_open_payload is the smallest playback
 * graph in the AGM service's table (subgraph 0xb000013d, container
 * 0xe00001b8, SAL(0x07001010, iid 0x483f) -> CODEC_DMA_SINK(0x07001023,
 * iid 0x4840), connection 0x483f:1 -> 0x4840:2).  g_graph_vendor_payload is
 * the graph the vendor HAL actually opened during the kprobe capture
 * (subgraph 0xb00000d5, container 0xe0000122, four modules); it is the
 * fallback when the hand-built one is rejected.
 *
 * The table in the AGM service is serialised for storage (12-byte param
 * header); the wire format needs the 16-byte apm_module_param_data with
 * error_code and 8-byte aligned sections, so the sections are rebuilt here
 * from the same fields.
 */
static const uint8_t g_graph_open_payload[344] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3d, 0x01, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xb8, 0x01, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x3d, 0x01, 0x00, 0xb0, 0xb8, 0x01, 0x00, 0xe0, 0x02, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x3f, 0x48, 0x00, 0x00, 0x23, 0x10, 0x00, 0x07,
	0x40, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x3f, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x40, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08,
	0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x3f, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x40, 0x48, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t g_graph_vendor_payload[440] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xd5, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x22, 0x01, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xd5, 0x00, 0x00, 0xb0, 0x22, 0x01, 0x00, 0xe0, 0x04, 0x00, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0x87, 0x45, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07,
	0x63, 0x47, 0x00, 0x00, 0x00, 0x3d, 0x01, 0x10, 0xd4, 0x42, 0x00, 0x00,
	0x0f, 0x10, 0x00, 0x07, 0x08, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x87, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x63, 0x47, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0xd4, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x08, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x10, 0x00, 0x08, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x87, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x63, 0x47, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x63, 0x47, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xd4, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x08, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x87, 0x45, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t g_graph_vp[712] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x07, 0x8f, 0x42, 0x00, 0x00, 0x98, 0x10, 0x00, 0x07,
	0x06, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x3d, 0x01, 0x10, 0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x1c, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x10, 0x00, 0x08, 0xe4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0e, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x8f, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0xe2, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
	0x7e, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x25, 0x00, 0x00, 0x00, 0x36, 0x44, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x99, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00,
	0x8d, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xd3, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x2f, 0x00, 0x00, 0x00, 0x2c, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x48, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x33, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

/* Vendor device graph 0xb0000002 with a WR_SHARED_MEM_EP (0x07001000,
 * iid 0x40bd) prepended and wired to SAL input port 2.  The vendor does
 * this with a second (stream) subgraph and a cross-graph connection; a
 * single merged subgraph avoids having to open and bind two graphs.  The
 * module/port/connection entries come from the vendor stream graph
 * 0xb00000d6 dump (WR has 0 inputs, 1 output). */
static const uint8_t g_graph_vp_wr[760] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x02, 0x00, 0x00, 0xe0, 0x08, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x07, 0x8f, 0x42, 0x00, 0x00, 0x98, 0x10, 0x00, 0x07, 0x06, 0x40, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x07, 0xba, 0x48, 0x00, 0x00, 0x01, 0x3d, 0x01, 0x10, 0xd3, 0x42, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x07, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0xc4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x1c, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0xe2, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x7e, 0x46, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x36, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x99, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x8d, 0x46, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x2c, 0x42, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x48, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x33, 0x43, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* The vendor's real 4-subgraph graph, verbatim from the AGM service's
 * heap during a live agmplay stream (2026-10-27 kprobe capture):
 * subgraphs 0xb0000002 (device/VP), 0xb0000081 (stream), 0xb0000024 and
 * 0xb0000023, modules 0x07001000 WR iid 0x40bd, TDM iid 0x428f, SAL iid
 * 0x4012, AW iid 0x42d3, MFC/CONVERTER instances 0x40c8/0x40bf/0x40c0.
 * Both ends use the same instance ids our hand-built graph does, so the
 * existing SET_CFG sequence applies unchanged.
 *
 * The GRAPH_OPEN packet carries only a descriptor {offset, 0, handle,
 * size}; the DSP reads the definition from the mapped region.  The host
 * struct below is 2724 bytes while the vendor declared 2712, so the
 * wire payload starts 12 bytes in - sweetvgoff/sweetvglen make that
 * assumption testable without another build. */
static const uint8_t g_graph_vendor4[2724] = {
	0xa4, 0x0a, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x5c, 0x03, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00,
	0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x10, 0x00, 0x08, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x07, 0x8f, 0x42, 0x00, 0x00, 0x98, 0x10, 0x00, 0x07,
	0x06, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x3d, 0x01, 0x10, 0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0xac, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
	0x8f, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08,
	0xe4, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
	0xe2, 0x41, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x1d, 0x00, 0x00, 0x00, 0x7e, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x36, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x99, 0x41, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x2d, 0x00, 0x00, 0x00, 0x8d, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x06, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x2c, 0x42, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
	0x48, 0x44, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x35, 0x00, 0x00, 0x00, 0x33, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x61, 0x10, 0x00, 0x08, 0x70, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0xd3, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xd4, 0x42, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x12, 0x00, 0x08,
	0xd7, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xd3, 0x42, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x12, 0x00, 0x08,
	0x48, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc0, 0x8f, 0x42, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x00, 0x08,
	0x81, 0x00, 0x00, 0xb0, 0x30, 0x02, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x81, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x04, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08,
	0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x6e, 0x00, 0x00, 0xe0,
	0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08, 0x38, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0xb0, 0x6e, 0x00, 0x00, 0xe0,
	0x05, 0x00, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07, 0xa1, 0x42, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xa4, 0x42, 0x00, 0x00, 0x38, 0x10, 0x00, 0x07,
	0xa6, 0x42, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xa7, 0x42, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0xad, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0x7c, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0xa1, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xa4, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xa6, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0xa7, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xad, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08,
	0x74, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xa4, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xa7, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xa1, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x42, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xa6, 0x42, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0xad, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xa6, 0x42, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00, 0xad, 0x42, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0xa6, 0x42, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xad, 0x42, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0xa6, 0x42, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
	0xad, 0x42, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xad, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xa1, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0, 0x2c, 0x01, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x24, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0,
	0x30, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07,
	0xc8, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc8, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x23, 0x00, 0x00, 0xb0, 0xc8, 0x03, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x23, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x9c, 0x03, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08,
	0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x24, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0xe0,
	0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0xe0,
	0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2e, 0x00, 0x00, 0xe0,
	0x07, 0x00, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08, 0x78, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0, 0x2c, 0x00, 0x00, 0xe0,
	0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07, 0xbd, 0x40, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xbe, 0x40, 0x00, 0x00, 0x05, 0x10, 0x00, 0x07,
	0xbf, 0x40, 0x00, 0x00, 0x03, 0x10, 0x00, 0x07, 0xc0, 0x40, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xc1, 0x40, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0,
	0x2d, 0x00, 0x00, 0xe0, 0x04, 0x00, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0xc2, 0x40, 0x00, 0x00, 0x19, 0x10, 0x00, 0x07, 0xc4, 0x40, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xc6, 0x40, 0x00, 0x00, 0x1b, 0x10, 0x00, 0x07,
	0x08, 0x40, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0, 0x2e, 0x00, 0x00, 0xe0,
	0x01, 0x00, 0x00, 0x00, 0x32, 0x10, 0x00, 0x07, 0x65, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08, 0xf4, 0x00, 0x00, 0x00,
	0x0a, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbe, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc1, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc2, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc4, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x65, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x08, 0x00, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x10, 0x00, 0x08, 0x94, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
	0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xbe, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xbf, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc1, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xc1, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc2, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x65, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xc4, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc4, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc2, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

/* Subgraph ids the vendor starts for that graph (PREPARE/START list). */
static const uint32_t g_vendor4_sgs[4] = {
	0xb0000002u, 0xb0000081u, 0xb0000024u, 0xb0000023u,
};

/* The vendor graph of THIS calibration generation, rebuilt from the ACDB rather than
 * captured: records copied verbatim from the group whose subgraph-connection table in
 * /vendor/etc/acdbdata/ukee_mtp/MTP_ukee_acdb_cal.acdb names exactly
 * {0xb0000002, 0xb0000025, 0xb0000024, 0xb0000023} - the list the vendor's own
 * GRAPH_START carries today.  Each record is a 52-byte preamble plus a blob whose
 * length is the word 4 bytes before its props header; that rule holds on every
 * interior boundary of both the ACDB and g_graph_vendor4.
 *
 * It is self-consistent in a way g_graph_vendor4 is not: that array is 4 bytes short
 * of its own last record (declares 924, holds 920), and the vendor packet declared
 * 2712 = 2728 - 16, so the host header is 16 bytes, not 12.  Nothing here has been
 * verified on hardware - sweetvgraph=2 is the experiment that decides it. */
static const uint8_t g_graph_ukee_current[2960] = {
	0x90, 0x0b, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x54, 0x03, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x28, 0x03, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00,
	0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x10, 0x00, 0x08, 0x38, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0xe0, 0x05, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x23, 0x10, 0x00, 0x07, 0xb1, 0x43, 0x00, 0x00, 0x9d, 0x10, 0x00, 0x07,
	0x86, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0x7c, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0xb1, 0x43, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86, 0x45, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08, 0x94, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x07, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xb1, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x86, 0x45, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x86, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0xe2, 0x41, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x23, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00,
	0x36, 0x44, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x19, 0x00, 0x00, 0x00, 0x7e, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x48, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x61, 0x10, 0x00, 0x08, 0xf4, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x8a, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x86, 0x45, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x12, 0x00, 0x08,
	0x6f, 0x13, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xb1, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0xe2, 0x41, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xc0, 0x02, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x00, 0x08,
	0x6f, 0x13, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xb1, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x23, 0x44, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xc0, 0x02, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x00, 0x08,
	0x6f, 0x13, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x36, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc0, 0xb1, 0x43, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x00, 0x08,
	0x6f, 0x13, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x7e, 0x46, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc0, 0xb1, 0x43, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x62, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x00, 0x08,
	0x6f, 0x13, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0xb0, 0x20, 0x03, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x00, 0x00, 0xf4, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x25, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x30, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x60, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0xb0,
	0x30, 0x00, 0x00, 0xe0, 0x0a, 0x00, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0xc9, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xca, 0x40, 0x00, 0x00,
	0x17, 0x10, 0x00, 0x07, 0xcc, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0xcd, 0x40, 0x00, 0x00, 0x10, 0x10, 0x00, 0x07, 0xcf, 0x40, 0x00, 0x00,
	0x38, 0x10, 0x00, 0x07, 0xd0, 0x40, 0x00, 0x00, 0x14, 0x10, 0x00, 0x07,
	0xe6, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0x76, 0x41, 0x00, 0x00,
	0x5b, 0x10, 0x00, 0x07, 0xb4, 0x45, 0x00, 0x00, 0x1b, 0x10, 0x00, 0x07,
	0x36, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0xf4, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xc9, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xca, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcc, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcd, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xcf, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xd0, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xe6, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x76, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xb4, 0x45, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x36, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08, 0xc4, 0x00, 0x00, 0x00,
	0x0c, 0x00, 0x00, 0x00, 0xc9, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xca, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xcc, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xcd, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xd0, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0xcf, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xd0, 0x40, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0xcf, 0x40, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xd0, 0x40, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0xcf, 0x40, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
	0xcf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc9, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xe6, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcc, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xca, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xb4, 0x45, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xb4, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe6, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xd0, 0x40, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
	0xcf, 0x40, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xcd, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x36, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x36, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x76, 0x41, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0,
	0x2c, 0x01, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00,
	0x11, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x0b, 0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x20, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x10, 0x00, 0x08, 0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x24, 0x00, 0x00, 0xb0, 0x30, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x07, 0xc8, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc8, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0, 0xc8, 0x03, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x9c, 0x03, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x23, 0x00, 0x00, 0xb0, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x08, 0x24, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x2c, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x2d, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x2e, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x14, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x13, 0x10, 0x00, 0x08,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x78, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0,
	0x2c, 0x00, 0x00, 0xe0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07,
	0xbd, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07, 0xbe, 0x40, 0x00, 0x00,
	0x05, 0x10, 0x00, 0x07, 0xbf, 0x40, 0x00, 0x00, 0x03, 0x10, 0x00, 0x07,
	0xc0, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07, 0xc1, 0x40, 0x00, 0x00,
	0x23, 0x00, 0x00, 0xb0, 0x2d, 0x00, 0x00, 0xe0, 0x04, 0x00, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xc2, 0x40, 0x00, 0x00, 0x1b, 0x10, 0x00, 0x07,
	0xc3, 0x40, 0x00, 0x00, 0x19, 0x10, 0x00, 0x07, 0xc4, 0x40, 0x00, 0x00,
	0x1a, 0x10, 0x00, 0x07, 0xc6, 0x40, 0x00, 0x00, 0x23, 0x00, 0x00, 0xb0,
	0x2e, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x32, 0x10, 0x00, 0x07,
	0x65, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08,
	0xf4, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc1, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc2, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc4, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x65, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08, 0x94, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xbe, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbe, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xbf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc1, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc1, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc2, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xc2, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xc3, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc4, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x65, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xc4, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* The same four subgraphs, in the order the vendor's GRAPH_START sends them. */
static const uint32_t g_ukee_current_sgs[4] = {
	0xb0000002u, 0xb0000025u, 0xb0000024u, 0xb0000023u,
};

#define APM_CMD_GRAPH_OPEN                  0x01001000u
#define APM_CMD_GRAPH_PREPARE               0x01001001u
#define APM_CMD_GRAPH_START                 0x01001002u
#define APM_CMD_SET_CFG                     0x01001006u
#define APM_CMD_SHARED_MEM_MAP_REGIONS      0x0100100Cu
#define APM_PARAM_ID_SUB_GRAPH_LIST         0x08001005u
#define APM_GRAPH_SG_ID                     0xb000013du
#define G_SAL_IID                           0x483fu
#define G_CDMA_IID                          0x4840u
#define G_WR_MODULE_ID                      0x07001000u
#define G_TDM_MODULE_ID                     0x0700100eu
#define G_INPUT_MODULE_OFF                  216u
#define APM_VP_SG_ID                        0xb0000002u
/* The hypervisor owns context banks 0..51; the Android driver's first
 * free stage-2 CB is 0x34 (kprobe: __arm_smmu_alloc_cb returned 0x34 for
 * the audio SID 0x4d).  Writing CBAR of a secured bank (e.g. 6) aborts. */
#define G_SMMU_CB                           52u
#define G_SMMU_CBAR(n)                      (0x1000u + 4u * (n))
#define G_SMMU_CBA2R(n)                     (0x1800u + 4u * (n))
#define G_SMMU_CB_PA(n)                     (0x15080000u + 0x1000u * (n))
#define G_S2_L1_PA                          0xa3000000u
#define G_S2_L2_PA                          0xa3001000u
#define G_IPA_BASE                          0xa2e00000u
#define G_VP_SAL_IID                        0x4012u
#define G_VP_WR_IID                         0x40bdu
#define G_VP_TDM_IID                        0x428fu
#define G_VP_AW_IID                         0x42d3u
#define G_CDMA_MODULE_OFF                   224u
#define APM_VENDOR_SG_ID                    0xb00000d5u
#define APM_SHM_POOL                        3u   /* SHMEM8_4K */
#define G_SHM_SIZE                          0x5000u  /* vendor MAP size */
/* sweetshmsize=<bytes> grows the region so a real audio clip fits (the S1
 * block mapping covers 2 MiB, but the IOVA must be block-aligned to use it). */
static uint32_t    g_shm_size;

static uint32_t
g_shm_region_size(void)
{
	static int done;

	if (!done) {
		int v = 0;

		done = 1;
		g_shm_size = 0x5000u;		/* vendor MAP size */
		(void)PE_parse_boot_argn("sweetshmsize", &v, sizeof(v));
		if (v >= 0x1000 && v <= 0x200000) {
			g_shm_size = (uint32_t)v;
		}
	}
	return g_shm_size;
}
#define G_GRAPH_OFF                         0x2000u  /* graph payload inside the region */
#define G_VENDOR_IOVA                       0x11ffe8000ull
/*
 * The DSP can only reach memory within its own reserved region
 * (adsp_region@9fd00000, 49 MiB).  XNU's physical allocator manages only the
 * 33 GiB window the ADT describes, so kmem_alloc_contig cannot return a page
 * the DSP can address.  Place the PCM buffer near the end of the reserved
 * region and map it uncached instead.
 */
#define G_SHM_FIXED_PA                      0xa2d00000u

/* Send a fully built packet and decode its reply. */
static int
g_apm_xfer(uint8_t *pkt, uint32_t pkt_size, const char *what)
{
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 5000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (!g_rsp_ready) {
		printf("SWEET GLINK: %s: no response\n", what);
		return -1;
	}
	if (g_rsp_len >= 28) {
		uint32_t hop = g_le32(g_rsp + 20);      /* packet opcode */
		uint32_t op = g_le32(g_rsp + 24);
		uint32_t status;

		/* APM_CMD_RSP_SHARED_MEM_MAP_REGIONS is short: 24-byte header
		 * plus a bare 4-byte mem_map_handle.  It is a success reply,
		 * not a GPR basic response. */
		if (g_rsp_len == 28 || hop == 0x02001001u) {
			op = APM_CMD_SHARED_MEM_MAP_REGIONS;
			status = 0;
		} else if (g_rsp_len >= 32) {
			status = g_le32(g_rsp + 28);
		} else {
			printf("SWEET GLINK: %s rsp %u bytes hop=%#x w24=%#x\n",
			    what, g_rsp_len, hop, op);
			sweet_log_sync();
			return -1;
		}
		printf("SWEET GLINK: %s rsp op=%#x status=%#x hop=%#x len=%u\n",
		    what, op, status, hop, g_rsp_len);
		sweet_log_sync();
		return status == 0 ? 0 : -1;
	}
	printf("SWEET GLINK: %s rsp %u bytes\n", what, g_rsp_len);
	return -1;
}

/* Send one APM command with the payload inlined behind the 16-byte payload
 * descriptor (graph and data commands use this form). */
static int
g_apm_cmd(uint32_t src_port, uint32_t dst_port, const uint8_t *payload,
    uint32_t plen, uint32_t opcode, const char *what)
{
	uint8_t pkt[24 + 16 + 1024];
	uint32_t pkt_size = 24 + 16 + plen;

	if (plen > sizeof(pkt) - 40) {
		return -1;
	}
	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	{
		/* sweetdbsrc=<port>: push the buffer from another client so the
		 * sink graph and the buffers can be aligned either way. */
		static uint32_t dbsrc;

		if (dbsrc == 0) {
			int v = 0;

			(void)PE_parse_boot_argn("sweetdbsrc", &v, sizeof(v));
			dbsrc = (v > 0) ? (uint32_t)v : 0xffffffffu;
		}
		g_put32(pkt + 8, (dbsrc != 0xffffffffu) ? dbsrc : src_port);
	}     /* GPR client port */
	g_put32(pkt + 12, dst_port);    /* 1 = APM, else module iid */
	g_put32(pkt + 16, 0x100u + opcode);
	g_put32(pkt + 20, opcode);
	g_put32(pkt + 24 + 0, 0);       /* payload_address_lsw  (inline) */
	g_put32(pkt + 24 + 4, 0);       /* payload_address_msw */
	g_put32(pkt + 24 + 8, 0);       /* mem_map_handle */
	g_put32(pkt + 24 + 12, plen);   /* payload_size */
	memcpy(pkt + 40, payload, plen);

	printf("SWEET GLINK: APM %s src=%#x dst=%#x (%u bytes)\n", what,
	    src_port, dst_port, pkt_size);
	sweet_log_sync();
	return g_apm_xfer(pkt, pkt_size, what);
}

/* APM_CMD_SHARED_MEM_MAP_REGIONS is the exception: the vendor kprobe trace
 * shows a 44-byte packet whose body starts at +24 (pool|count, flag, addr
 * lsw/msw, size) with no payload descriptor, and bit 16 set in the domain
 * word.  The descriptor form makes the DSP read pool 0 / count 0. */
/* sweetiova=1 registers the region at the vendor IOVA while the buffer
 * itself stays at the physical page we fill. */
static uint64_t    g_map_addr;		/* MAP region address override */
static uint64_t    g_map_pa;		/* buffer PA override (iova mode) */
static uint32_t    g_shm_hi;		/* sweetshmhi: high word of MAP addr */
static uint64_t    g_s1_tbl_pa;		/* S1 table base */

static int
g_apm_shm_map_flat(uint32_t src_port, uint64_t pa, uint32_t flag,
    const char *what)
{
	uint8_t pkt[24 + 20];
	uint32_t pkt_size = sizeof(pkt);
	/* sweetiova=1 registers the region at the vendor IOVA while the
	 * buffer itself stays at the physical page we fill. */
	uint64_t mapa = g_map_addr ? g_map_addr : pa;

	/*
	 * The vendor's raw SHM_MAP carries {addr_lsw=0x1ffd0000,
	 * addr_msw=1}.  A 33-bit IOVA makes the DSP's translation fault,
	 * so the high word is not addressing; sheng's audio driver says the
	 * upper word is the stream index (0x1001 & 0xf = 1).  sweetmhi=1
	 * reproduces the vendor packet byte for byte.  Default 0 keeps every
	 * earlier run identical.
	 */
	mapa = (mapa & 0xffffffffull) | ((uint64_t)g_shm_hi << 32);

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8) | 0x10000u);
	g_put32(pkt + 8, src_port);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, 0x2du);
	g_put32(pkt + 20, APM_CMD_SHARED_MEM_MAP_REGIONS);
	g_put32(pkt + 24, 3u | (1u << 16));
	g_put32(pkt + 28, flag);
	g_put32(pkt + 32, (uint32_t)mapa);
	g_put32(pkt + 36, (uint32_t)(mapa >> 32));
	g_put32(pkt + 40, g_shm_region_size());
	printf("SWEET GLINK: shm_flat pa=%#llx%s flag=%u size=%u\n", mapa,
	    mapa != pa ? " (region addr, buffer PA differs)" : "", flag,
	    g_shm_region_size());
	sweet_log_sync();
	return g_apm_xfer(pkt, pkt_size, what);
}

/* The vendor HAL never inlines a graph payload: it maps one region once and
 * afterwards points APM commands at it.  Mirror that with a physically
 * contiguous kernel buffer.  SHM_MAP carries no apm_cmd_header: the payload
 * is {u16 mem_pool_id; u16 num_regions; u32 property_flag; region[]}, and
 * the response payload is the 32-bit mem_map_handle (mainline audioreach.h
 * and the vendor's kprobe trace agree byte for byte). */
static vm_offset_t g_shm_va;
static uint64_t    g_shm_pa;
static uint64_t    g_self_pa;		/* page proven real by g_phys_selftest */
static int         g_shm_offset_mode = 1;
static int         g_skipgfsr;		/* sweetskipgfsr=1 */
static int         g_s1_cbn = -1;	/* context bank the S1 probe configured */
static uint32_t    g_dbdst;		/* sweetdbdst=<iid>: override DATA_BUFFER dst */
/* Client port that opened the graph.  DATA_BUFFER_V2 must come from the
 * SAME port: in the vendor kprobe trace GRAPH_OPEN/SET_CFG/PREPARE/START and
 * every DATA_BUFFER all carry src=0x2010 (and SHM_MAP comes from 0x2002).
 * We used to open from 0x2010 but push buffers from 0x2011, i.e. the sink
 * belonged to a graph its buffers did not come from. */
static uint32_t    g_stream_port = 0x2010u;
static vm_offset_t g_s1_win;		/* its context-bank register page */
static int         g_nolpass;		/* sweetnolpass=1 */
static uint32_t    g_shm_handle;
static int         g_shm_mapped;

/* GPR reserves 16 bytes after the header for the payload descriptor
 * (address lsw/msw, mem_map_handle, payload_size); the SHM body follows it.
 * The hand-built packet below put the body at offset 24, so the DSP read the
 * pool id as the payload size and answered AR_EBADPARAM.  Build the body in
 * a standalone array and let g_apm_cmd place it correctly. */
/*
 * ATS1PR: make the SMMU walk its own stage-1 tables for one IOVA and report
 * the result.  No device DMA is involved, so this separates "is the AP-side
 * mapping correct" from "can the DSP read the buffer".  Mirrors the vendor's
 * arm_smmu_iova_to_phys_hard(): 64-bit ATS1PR write (AArch64 table format),
 * wait for ATSR.ACTIVE to clear, then read the 64-bit PAR at 0x50 where
 * BIT(0) means fault and [39:12] is the output PA.
 */
static uint64_t
g_ats1pr(vm_offset_t cbw, uint64_t iova)
{
	uint64_t par;

	*(volatile uint64_t *)(cbw + 0x800u) = iova & ~0xfffull;
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; i < 100000; i++) {
		if ((*(volatile uint32_t *)(cbw + 0x8f0u) & 1u) == 0) {
			break;
		}
		__asm__ volatile("yield");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	par = *(volatile uint64_t *)(cbw + 0x50u);
	return par;
}

/*
 * ---- GPU (kgsl-smmu) probe ----
 *
 * The shim runs this before XNU, with a fault-tolerant accessor, and leaves
 * the result in low DRAM at 0xa2e0f000.  This side only prints it: the answer
 * has to reach the UFS log, and the shim has no filesystem writer.
 *
 * kgsl-smmu@3da0000 is a separate SMMU-500 instance from apps-smmu, so the
 * hypervisor restriction that owns the audio SID need not apply to it.  The
 * shim's write test is the decisive part: if S2CR Bypass (0x10000) sticks,
 * the AP can own GPU stream mappings.
 */
#define GPU_PROBE_PA		0xa2e0f000ull
#define GPU_PROBE_MAGIC		0x47505550u

/* Keep this layout identical to shim/shim.c. */
struct gpu_probe_result {
	uint32_t magic;
	uint32_t version;
	uint32_t scr0;
	uint32_t id0;
	uint32_t id1;
	uint32_t id2;
	uint32_t num_smr;
	uint32_t num_cb;
	uint32_t valid_seen;
	uint32_t sid0_slot;
	uint32_t smr0;
	uint32_t s2cr0;
	uint32_t wsid;
	uint32_t w2cr_old;
	uint32_t wsmr;
	uint32_t w2cr;
	uint32_t w2cr_f;
	uint32_t w2cr_r;
	uint32_t rfault;
	uint32_t wfault;
	uint32_t kgsl_status;
	uint32_t kgsl_status2;
	uint32_t kfault;
	uint32_t pad;
	uint64_t far;
	uint64_t esr;
	uint32_t w2cr_t;
	uint32_t wcbar_old;
	uint32_t wcbar;
	uint32_t wsctlr_old;
	uint32_t wsctlr;
	uint32_t op_fault;
	uint64_t pad3;
	uint32_t cbar_bank;
	uint32_t cbar_r;
	uint32_t cbar_w;
	uint32_t sctlr_bank;
	uint32_t sctlr_r;
	uint32_t sctlr_w;
	uint32_t ttbr0;
	uint32_t tcr;
	uint32_t mair;
	uint32_t cba2r;
	uint32_t cbfrsynra;
	uint32_t cbar_count;
	uint32_t cbar1;
	uint32_t cbar2;
	uint32_t cbar3;
	uint32_t last_step;
	uint32_t prev_step;
	uint32_t prev_magic;
	uint32_t kg0;
	uint32_t kg1;
	uint32_t kg2;
	uint32_t kg3;
	uint32_t pad4;
	uint64_t pad5;
	/* v9: named fields, appended so the old prints keep working. */
	uint32_t p_cbbase;
	uint32_t p_cb0_sctlr;
	uint32_t p_cb0_ttbr0;
	uint32_t p_cb0_ttbr1;
	uint32_t p_cb0_tcr;
	uint32_t p_cb0_tcr2;
	uint32_t p_cb0_mair0;
	uint32_t p_cb0_mair1;
	uint32_t p_cb0_cbar;
	uint32_t p_cb0_cba2r;
	uint32_t p_cb0_cbf;
	uint32_t p_cb1_sctlr;
	uint32_t p_cxgdr;
	uint32_t p_hwctrl;
	uint32_t p_gxgdr;
	uint32_t p_gxdom;
	uint32_t p_clk_ahb;
	uint32_t p_clk_cxgmu;
	uint32_t p_clk_hubcx;
	uint32_t p_clk_smmuvote;
	uint32_t p_clk_memnoc;
	uint32_t p_kgsl_ident;
	uint32_t p_kgsl_rbbm;
	uint32_t p_rd_fault;
	uint32_t p_cxgdr2;
	uint32_t p_clk_ahb2;
	uint32_t p_clk_cxgmu2;
	uint32_t p_clk_hubcx2;
	uint32_t p_clk_smmuvote2;
	uint32_t p_clk_memnoc2;
	uint32_t p_kgsl_rbbm2;
	uint32_t p_cb0_sctlr2;
	uint32_t p_pwr_step;
	uint32_t p_pwr_flags;
	uint32_t p_cbw_before;
	uint32_t p_cbw_after;
	uint32_t p_cbw_flags;
	uint64_t p_pad6;
};

/*
 * A GPU run uses sweetglink=0 so the audio path never starts, but the
 * auto-reset lives inside the glink probe -- without this the phone would sit
 * in iOS until somebody pressed power.  Reset only when glink is off, so an
 * audio run still resets at its own end.
 */
static void
g_gpu_maybe_reset(void)
{
	int rb = 0, gl = 1;

	int gm = 0;

	(void)PE_parse_boot_argn("sweetreboot", &rb, sizeof(rb));
	(void)PE_parse_boot_argn("sweetglink", &gl, sizeof(gl));
	(void)PE_parse_boot_argn("sweetgmuboot", &gm, sizeof(gm));
	if (gm) {
		return;	/* the gmu boot resets when it is done */
	}
	if (rb && !gl) {
		printf("SWEET GPU: PSCI SYSTEM_RESET\n");
		sweet_log_sync();
		{
			register uint64_t x0 __asm("x0") = 0x84000009ull;

			__asm volatile("dsb sy" ::: "memory");
			__asm volatile("smc #0" : "+r"(x0) ::
			    "x1", "x2", "x3", "memory");
		}
		for (;;) {
			delay(1000);
		}
	}
}

static void
g_gpu_probe(void)
{
	int en = 0;
	vm_offset_t v;
	struct gpu_probe_result *r;

	(void)PE_parse_boot_argn("sweetgpu", &en, sizeof(en));
	if (!en) {
		return;
	}
	v = g_map_win(GPU_PROBE_PA, 0x1000u);
	if (v == 0) {
		printf("SWEET GPU: cannot map probe page\n");
		sweet_log_sync();
		g_gpu_maybe_reset();
		return;
	}
	r = (struct gpu_probe_result *)v;
	if (r->magic != GPU_PROBE_MAGIC) {
		printf("SWEET GPU: no shim result (magic %#x)\n", r->magic);
		sweet_log_sync();
		g_gpu_maybe_reset();
		return;
	}
	printf("SWEET GPU: shim v%u scr0=%#x id0=%#x id1=%#x id2=%#x\n",
	    r->version, r->scr0, r->id0, r->id1, r->id2);
	printf("SWEET GPU: nsmr=%u ncb=%u valid=%u sid0slot=%d smr0=%#x "
	    "s2cr0=%#x\n", r->num_smr, r->num_cb, r->valid_seen,
	    (int)r->sid0_slot, r->smr0, r->s2cr0);
	printf("SWEET GPU: wr slot=%d old=%#x smr=%#x byp=%#x fault=%#x "
	    "restore=%#x wf=%u\n", (int)r->wsid, r->w2cr_old, r->wsmr,
	    r->w2cr, r->w2cr_f, r->w2cr_r, r->wfault);
	printf("SWEET GPU: kgsl st=%#x st2=%#x kf=%u rf=%u far=%#llx "
	    "esr=%#llx\n", r->kgsl_status, r->kgsl_status2, r->kfault,
	    r->rfault, r->far, r->esr);
	if (r->version >= 5) {
		printf("SWEET GPU: l5 trans=%#x cbar=%#x old=%#x sctlr=%#x "
		    "old=%#x\n", r->w2cr_t, r->wcbar, r->wcbar_old,
		    r->wsctlr, r->wsctlr_old);
	}
	printf("SWEET GPU: cb bank=%d r=%#x w=%#x sctlr bank=%d r=%#x "
	    "w=%#x of=%#x\n", (int)r->cbar_bank, r->cbar_r, r->cbar_w,
	    (int)r->sctlr_bank, r->sctlr_r, r->sctlr_w, r->op_fault);
	printf("SWEET GPU: cbregs ttbr0=%#x tcr=%#x mair=%#x cba2r=%#x "
	    "cbf=%#x n=%u/%#x %#x %#x\n", r->ttbr0, r->tcr, r->mair,
	    r->cba2r, r->cbfrsynra, r->cbar_count, r->cbar1, r->cbar2,
	    r->cbar3);
	printf("SWEET GPU: kgsl %#x %#x %#x %#x last=%u prev=%u magic=%#x\n",
	    r->kg0, r->kg1, r->kg2, r->kg3, r->last_step, r->prev_step,
	    r->prev_magic);
	/* v9: everything the GPU probes actually need, by name. */
	printf("SWEET GPU: v9 cbbase=%#x cb0 sctlr=%#x ttbr0=%#x ttbr1=%#x "
	    "tcr=%#x tcr2=%#x mair0=%#x mair1=%#x\n", r->p_cbbase,
	    r->p_cb0_sctlr, r->p_cb0_ttbr0, r->p_cb0_ttbr1, r->p_cb0_tcr,
	    r->p_cb0_tcr2, r->p_cb0_mair0, r->p_cb0_mair1);
	printf("SWEET GPU: v9 cb0 cbar=%#x cba2r=%#x cbf=%#x cb1 sctlr=%#x "
	    "kgsl ident=%#x rbbm=%#x rdf=%#x\n", r->p_cb0_cbar,
	    r->p_cb0_cba2r, r->p_cb0_cbf, r->p_cb1_sctlr, r->p_kgsl_ident,
	    r->p_kgsl_rbbm, r->p_rd_fault);
	printf("SWEET GPU: v9 gpucc cxgdr=%#x hwctrl=%#x gxgdr=%#x gxdom=%#x "
	    "ahb=%#x cxgmu=%#x hubcx=%#x vote=%#x memnoc=%#x\n",
	    r->p_cxgdr, r->p_hwctrl, r->p_gxgdr, r->p_gxdom, r->p_clk_ahb,
	    r->p_clk_cxgmu, r->p_clk_hubcx, r->p_clk_smmuvote,
	    r->p_clk_memnoc);
	if (r->p_pwr_step != 0) {
		printf("SWEET GPU: v9 after pwr step=%u fl=%#x cxgdr=%#x "
		    "ahb=%#x cxgmu=%#x hubcx=%#x vote=%#x memnoc=%#x "
		    "rbbm=%#x cb0sctlr=%#x\n", r->p_pwr_step, r->p_pwr_flags,
		    r->p_cxgdr2, r->p_clk_ahb2, r->p_clk_cxgmu2,
		    r->p_clk_hubcx2, r->p_clk_smmuvote2, r->p_clk_memnoc2,
		    r->p_kgsl_rbbm2, r->p_cb0_sctlr2);
	}
	if (r->p_cbw_flags != 0) {
		printf("SWEET GPU: v9 cbwrite before=%#x after=%#x fl=%#x\n",
		    r->p_cbw_before, r->p_cbw_after, r->p_cbw_flags);
	}
	sweet_log_sync();
	g_gpu_maybe_reset();
}

/*
 * ---- RPMh cmd-db dump (sweetdb=1) ----
 *
 * Plain DDR, safe at any time.  Every entry is {name[8], prio[2x u32],
 * address}, grouped under per-slave headers at table+8.  Print names and
 * addresses so the GPU BCM/ARC resources can be voted by name instead of
 * by guessing offsets.
 */
static uint32_t gm_rd(vm_offset_t b, uint32_t o);

static void
g_cmddb_dump(void)
{
	int en = 0;
	vm_offset_t db;
	unsigned i, j, shown = 0;

	(void)PE_parse_boot_argn("sweetdb", &en, sizeof(en));
	if (!en) {
		return;
	}
	db = g_map_win(0x80860000ull, 0x20000u);
	if (!db) {
		printf("SWEET DB: map failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET DB: ver=%#x magic=%#x\n", gm_rd(db, 0u), gm_rd(db, 4u));
	for (i = 0; i < 8u; i++) {
		uint32_t h = 8u + 16u * i;
		uint32_t hw = gm_rd(db, h), hw2 = gm_rd(db, h + 4u);
		uint32_t ents = 144u + ((hw >> 16) & 0xffffu);
		unsigned slave = hw & 0xffffu;
		unsigned count = (hw2 >> 16) & 0xffffu;

		if (!slave || !count || count > 256u) {
			continue;
		}
		printf("SWEET DB: slave%u count=%u\n", slave, count);
		sweet_log_sync();
		for (j = 0; j < count && shown < 120u; j++) {
			uint32_t e = ents + 24u * j;
			uint32_t a = gm_rd(db, e + 16u);
			char n[9];
			unsigned k;

			for (k = 0; k < 8u; k++) {
				uint32_t w = gm_rd(db, e + 4u * (k / 4u));
				n[k] = (char)((w >> (8u * (k % 4u))) & 0xffu);
			}
			n[8] = 0;
			printf("SWEET DB:  %s = %#x\n", n, a);
			++shown;
		}
		sweet_log_sync();
	}
	printf("SWEET DB: done entries=%u\n", shown);
	sweet_log_sync();
}

/*
 * ---- GPU state snapshot (sweetgsnap=1) ----
 *
 * Read-only dump of everything we are about to touch, taken before any GPU
 * write.  Every later experiment is compared against these ABL-handover
 * values; without it a changed bit cannot be attributed to a step we took.
 */
static uint32_t gm_rd(vm_offset_t b, uint32_t o);

static void
g_gpu_snapshot(void)
{
	int en = 0;
	vm_offset_t gc, kg, sm, gm;

	(void)PE_parse_boot_argn("sweetgsnap", &en, sizeof(en));
	if (!en) {
		return;
	}
	printf("SWEET SNAP: start level=%d\n", en);
	sweet_log_sync();
	sm = g_map_win(0x3da0000ull, 0x20000u);
	if (!sm) {
		printf("SWEET SNAP: smmu window alloc failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET SNAP: smmu id0=%#x id1=%#x s2cr0=%#x s2cr1=%#x "
	    "s2cr5=%#x\n", gm_rd(sm, 0x0u), gm_rd(sm, 0x4u),
	    gm_rd(sm, 0xc00u), gm_rd(sm, 0xc04u), gm_rd(sm, 0xc14u));
	sweet_log_sync();
	kg = g_map_win(0x3d00000ull, 0x40000u);
	if (!kg) {
		printf("SWEET SNAP: kgsl window alloc failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET SNAP: rbbm_ident=%#x rbbm_status=%#x\n",
	    gm_rd(kg, 0x0u), gm_rd(kg, 0x840u));
	sweet_log_sync();
	gc = g_map_win(0x3d90000ull, 0x10000u);
	if (!gc) {
		printf("SWEET SNAP: gpucc window alloc failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET SNAP: cx_gdscr=%#x cfg=%#x gx_gdscr=%#x gx_cfg=%#x\n",
	    gm_rd(gc, 0x9108u), gm_rd(gc, 0x910cu), gm_rd(gc, 0x905cu),
	    gm_rd(gc, 0x9060u));
	sweet_log_sync();
	printf("SWEET SNAP: ahb=%#x cxgmu=%#x hub=%#x smmuv=%#x "
	    "memnoc=%#x bcr=%#x\n", gm_rd(gc, 0x911cu), gm_rd(gc, 0x913cu),
	    gm_rd(gc, 0x9148u), gm_rd(gc, 0x7000u), gm_rd(gc, 0x9150u),
	    gm_rd(gc, 0x9314u));
	printf("SWEET SNAP: gmu_rcg=%#x/%#x hub_rcg=%#x/%#x hub_div=%#x\n",
	    gm_rd(gc, 0x9318u), gm_rd(gc, 0x931cu), gm_rd(gc, 0x93ecu),
	    gm_rd(gc, 0x93f0u), gm_rd(gc, 0x9430u));
	sweet_log_sync();
	if (en < 2) {
		printf("SWEET SNAP: gmu skipped (level 1)\n");
		sweet_log_sync();
		return;
	}
	gm = g_map_win(0x3d68000ull, 0x10000u);
	if (!gm) {
		printf("SWEET SNAP: gmu window alloc failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET SNAP: gmu (last group)\n");
	printf("SWEET SNAP: cm3_reset=%#x fw_init=%#x hfi_cs=%#x hfi_ci=%#x "
	    "itcm0=%#x dtcm0=%#x\n", gm_rd(gm, 0x16000u), gm_rd(gm, 0x16070u),
	    gm_rd(gm, 0x16600u), gm_rd(gm, 0x16618u), gm_rd(gm, 0x5000u),
	    gm_rd(gm, 0x9000u));
	sweet_log_sync();
}

/*
 * ---- SPMI rail probe (sweetrail=1) ----
 *
 * The GPU rails are PM8350C S6 (CX, expected SID 2 base 0x1900) and PM8350
 * S5 (gfx, SID 1 base 0x1800), but a wrong SID or block base looks exactly
 * like a dead rail.  So sweep every PMIC/block and print only the blocks
 * that answer: a live SMPS has a 0..7 mode at +0x45 and EN bit 7 at +0x46,
 * with the voltage setpoint at +0x40.
 */
extern int sweet_spmi_init(void);
extern int sweet_spmi_read_any(unsigned sid, unsigned addr, uint8_t *out);

static void
g_rail_probe(void)
{
	int en = 0, mode;
	unsigned sid, base, seen = 0;

	(void)PE_parse_boot_argn("sweetrail", &en, sizeof(en));
	if (!en) {
		return;
	}
	mode = en;	/* 1 = the two suspected rails only, 2 = full sweep */
	if (sweet_spmi_init() != 0) {
		printf("SWEET RAIL: spmi init failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET RAIL: start mode=%d\n", mode);
	sweet_log_sync();
	for (sid = 0; sid <= 3u; sid++) {
		if (mode < 2 && sid != 1u && sid != 2u) {
			continue;
		}
		printf("SWEET RAIL: sid%u scanning\n", sid);
		sweet_log_sync();
		for (base = 0x1400u; base <= 0x1f00u; base += 0x100u) {
			uint8_t vo = 0xff, md = 0xff, eb = 0xff, v2 = 0xff;
			int r0, r1, r2, r3;

			if (mode < 2 && base != 0x1900u && base != 0x1800u) {
				continue;
			}
			r0 = sweet_spmi_read_any(sid, base + 0x40u, &vo);
			r1 = sweet_spmi_read_any(sid, base + 0x45u, &md);
			r2 = sweet_spmi_read_any(sid, base + 0x46u, &eb);
			r3 = sweet_spmi_read_any(sid, base + 0x47u, &v2);
			if (r0 && r1 && r2) {
				continue;	/* nothing backs this block */
			}
			++seen;
			printf("SWEET RAIL: sid%u b%04x vo=%02x md=%02x "
			    "en=%02x v2=%02x r=%d%d%d%d\n",
			    sid, base, vo, md, eb, v2, r0, r1, r2, r3);
			sweet_log_sync();
		}
	}
	/*
	 * G1 read these four cmd-db blocks on SID 4 alone and only one of
	 * them answered: vrm_gfx@0x0600 returned eight registers (00 03 00
	 * 00 0b 05 00 01) while vrm_cx@0x2900, vrm_lcx@0x2000 and
	 * vrm_mxc@0x2700 came back all-0xff, which is what SPMI gives when
	 * no slave lives there.  The Android regulator names explain it: the
	 * rails sit on two different PMICs (pm8350 S5 for gfx, pm8350c S2/S6
	 * for the gfx/mmcx voters), so the high nibble of the cmd-db value
	 * cannot be the SID for all four.  Walk every SID with the same four
	 * offsets and print only the blocks that answer.  Still read-only:
	 * eight registers per hit, no enable or voltage bit touched.
	 */
	{
		static const unsigned int vr[4] = {
			0x2900u, 0x0600u, 0x2000u, 0x2700u
		};
		static const char *vn[4] = { "cx", "gfx", "lcx", "mxc" };
		unsigned int s, i, j, hits = 0;

		for (s = 0; s <= 15u; s++) {
			for (i = 0; i < 4u; i++) {
				uint8_t b[8];
				int ok = 0;

				for (j = 0; j < 8u; j++) {
					b[j] = 0xff;
					if (sweet_spmi_read_any(s, vr[i] + j,
						    &b[j]) == 0) {
							ok++;
						}
				}
				if (ok == 0) {
					continue;
				}
				hits++;
				printf("SWEET RAIL: vrm_%s@%u:%04x ok=%d %02x %02x "
					"%02x %02x %02x %02x %02x %02x\n", vn[i], s, vr[i],
					ok, b[0], b[1], b[2], b[3], b[4], b[5], b[6],
					b[7]);
				sweet_log_sync();
			}
		}
		printf("SWEET RAIL: vrm hits=%u\n", hits);
		sweet_log_sync();
	}

	printf("SWEET RAIL: done blocks=%u\n", seen);
	sweet_log_sync();
}

/*
 * ---- kernel-side GMU bring-up (sweetgmuboot=1) ----
 *
 * The shim stages the firmware and its buffers at fixed physical addresses
 * just past the probe page:
 *   0xa2e10000 firmware   0xa2e20000 gmu cache   0xa2e30000 hfi queues
 *   0xa2e40000 page tables
 * Doing the boot here instead of in the shim means XNU is already running
 * when the CM3 starts, so every step lands in the SWEETLOG ring.
 */
#define GMU_FW_PA	0xa2e10000ull
#define GMU_CACHE_PA	0xa2e20000ull
#define GMU_HFI_PA	0xa2e30000ull
#define GMU_TBL_PA	0xa2e40000ull
#define GMU_HDR_PA	0xa2e0f000ull
#define GMU_STAGE_MAGIC	0x474d5546u	/* "GMUF" */

#define GPUCC_PA	0x3d90000ull
#define GMUREG_PA	0x3d68000ull
#define KGSMMU_PA	0x3da0000ull
#define KGSL_PA		0x3d00000ull

/* gen7 register word offset -> byte offset in the gmu block */
#define GMU_OFF(w)	((uint32_t)(((w) - 0x1a000u) << 2))

static uint32_t
gm_rd(vm_offset_t b, uint32_t o)
{
	uint32_t v = *(volatile uint32_t *)(b + o);

	__asm__ volatile("dsb sy" ::: "memory");
	return v;
}

static void
gm_wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t
gm_fw_ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * SWEET: these two live in osfmk/kern/processor.c (patch 0188) and are
 * also declared again further down next to the SMP sampler.  The canary
 * below runs earlier in the file, and XNU builds with ISO C99 implicit
 * function declarations as an error, so the prototypes have to be here
 * rather than only at the sampler site.
 */
extern int sweet_cpu_busy(int cpu);
extern uint64_t sweet_cpu_tid(int cpu);

/*
 * Fabric canary for the CM3 release window (sweetgcanary=1).
 *
 * Every GMU boot so far has answered the same way: the core that writes
 * CM3_SYSRESET disappears, the ring stops, and the watchdog picks the box
 * up.  From that one thread two completely different faults look identical
 * -- either the core is pinned by a Device read that never completes, or
 * the whole interconnect goes down and nothing can reach DRAM or the log
 * any more.  Those need opposite fixes, so a run has to say which it was.
 *
 * This thread is started immediately before the release write and runs on
 * whatever core the scheduler hands it, i.e. not the releasing core.  Each
 * 100 ms tick prints:
 *
 *   n=...      the tick itself: this core, DRAM and the log path work;
 *   self=...   a pure-DRAM write/compare self-test, so a heartbeat is not
 *              merely the scheduler being alive;
 *   busy=0x..  which CPUs currently have a non-idle active thread;
 *   t0..t3     the active thread id on those CPUs.  A core stuck inside a
 *              Device read keeps its tid forever while the others change;
 *              a dead fabric freezes every tid at once;
 *   probe=     one target per tick, printed as arm/done around the access.
 *              An arm with no done names the block that pins the core.
 *              hfi is our own DRAM and acts as the control.
 *
 * Deliberately excluded: GPUCC and the GMU window (0216 already knows they
 * are fatal once the CM3 runs) and the SMMU, because reading GFSR has
 * read-restore-lock semantics and the probe would disturb the very state
 * being observed.
 *
 * Known limitation: if the releasing core dies while holding the log lock,
 * this heartbeat stops for the same reason a fabric death would.  The tick
 * counter and the tid columns are what separate the two cases -- a lock
 * wait leaves the other CPUs' tids changing in the ring lines we do get.
 */
static volatile int g_canary_go;
static vm_offset_t g_canary_ao;
static vm_offset_t g_canary_rs;
static vm_offset_t g_canary_hfi;
static uint32_t g_canary_scratch[64];

#define G_CANARY_SCRATCH_N (sizeof(g_canary_scratch) / sizeof(g_canary_scratch[0]))
#define G_CANARY_TICK_US	   20000u	/* 20 ms; 100 ticks = 2 s window */

static unsigned int
g_canary_selftest(void)
{
	unsigned int bad = 0;
	unsigned int i;
	const uint32_t seed = 0x5a5a0000u;

	for (i = 0; i < G_CANARY_SCRATCH_N; i++) {
		g_canary_scratch[i] = seed + i;
	}
	for (i = 0; i < G_CANARY_SCRATCH_N; i++) {
		if (g_canary_scratch[i] != seed + i) {
			bad++;
		}
	}
	return bad;
}

static void
g_canary_probe(unsigned int which)
{
	static const char *nm[3] = { "hfi", "ao", "rscc" };

	if (which > 2) {
		return;
	}
	printf("SWEET CANARY: probe %s arm\n", nm[which]);
	sweet_log_sync();
	switch (which) {
	case 0:
		printf("SWEET CANARY: probe hfi done=%u/%u\n",
		    *(volatile uint32_t *)(g_canary_hfi + 0x40u),
		    *(volatile uint32_t *)(g_canary_hfi + 0x44u));
		break;
	case 1:
		printf("SWEET CANARY: probe ao done=%#x %#x\n",
		    gm_rd(g_canary_ao, 0x0u), gm_rd(g_canary_ao, 0x4u));
		break;
	default:
		printf("SWEET CANARY: probe rscc done=%#x %#x\n",
		    gm_rd(g_canary_rs, 0xd18u), gm_rd(g_canary_rs, 0xfb8u));
		break;
	}
	sweet_log_sync();
}

static void
g_canary_thread(void *arg, wait_result_t wr)
{
	unsigned int n;
	unsigned int nc = ml_get_cpu_count();

	(void)arg;
	(void)wr;
	if (nc > 8u) {
		nc = 8u;
	}
	g_canary_ao = g_map_win_wimg(0x3d40000ull, 0x1000u, VM_WIMG_IO);
	g_canary_rs = g_map_win_wimg(0x3d50000ull, 0x2000u, VM_WIMG_IO);
	g_canary_hfi = g_map_win(GMU_HFI_PA, 0x1000u);
	printf("SWEET CANARY: start v2 nc=%u ao=%#llx rs=%#llx hfi=%#llx\n", nc,
	    (unsigned long long)g_canary_ao, (unsigned long long)g_canary_rs,
	    (unsigned long long)g_canary_hfi);
	sweet_log_sync();

	for (n = 0; n < 100u; n++) {
		unsigned int mask = 0;
		unsigned int i;
		unsigned int bad;
		uint64_t tv[4] = { 0, 0, 0, 0 };

		/*
		 * v2: busy-wait instead of blocking on a deadline.  The v1 thread
		 * slept 100 ms before its first tick and printed nothing, while the
		 * log lock was provably free (the releasing core had just finished a
		 * printf+sync pair).  That leaves "never scheduled" and "timer
		 * interrupt never arrived" and v1 cannot tell them apart.  delay()
		 * polls, so a heartbeat here proves scheduling without depending on
		 * interrupt delivery at all.
		 */
		if (n > 0u) {
			delay(G_CANARY_TICK_US);
		}
		if (!g_canary_go) {
			continue;
		}
		for (i = 0; i < nc; i++) {
			if (sweet_cpu_busy((int)i) == 1) {
				mask |= 1u << i;
			}
			tv[i & 3u] = sweet_cpu_tid((int)i);
		}
		bad = g_canary_selftest();
		printf("SWEET CANARY: n=%u self=%u busy=0x%x tid=%llu "
		    "t0=%llu t1=%llu t2=%llu t3=%llu\n", n, bad, mask,
		    (unsigned long long)thread_tid(current_thread()),
		    (unsigned long long)tv[0], (unsigned long long)tv[1],
		    (unsigned long long)tv[2], (unsigned long long)tv[3]);
		sweet_log_sync();
		/*
		 * Two clean heartbeat ticks first: prove the fabric is alive
		 * after the release before spending a probe on a target that
		 * may pin this core as well.
		 */
		if (n >= 3u) {
			g_canary_probe((n - 3u) % 3u);
		}
	}
	printf("SWEET CANARY: done\n");
	sweet_log_sync();
}

static void
g_canary_arm(void)
{
	int cy = 0;
	thread_t th;

	(void)PE_parse_boot_argn("sweetgcanary", &cy, sizeof(cy));
	if (!cy) {
		return;
	}
	g_canary_go = 1;
	if (kernel_thread_start((thread_continue_t)g_canary_thread, NULL,
	    &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		printf("SWEET CANARY: armed\n");
	} else {
		printf("SWEET CANARY: arm failed\n");
	}
	sweet_log_sync();
}


/*
 * The releasing core and the observing core have to be different threads.
 * Runs D and E both lost the log at the release because the thread that
 * wrote CM3_SYSRESET was also the one that tried to look at the result, so
 * "no output" could not be told apart from "no machine".  This thread only
 * performs the release store; the caller keeps running and reads nothing but
 * our own DRAM.  If the release pins a core, that is this thread, and the
 * observer's lines still reach the ring.
 */
static void
g_rel_thread(void *arg, wait_result_t wr)
{
	vm_offset_t gm;

	(void)arg;
	(void)wr;
	delay(60000);
	gm = g_map_win_wimg(GMUREG_PA, 0x40000, VM_WIMG_POSTED);
	if (!gm) {
		printf("SWEET REL2: gm map failed\n");
		sweet_log_sync();
		return;
	}
	printf("SWEET REL2: releaser storing\n");
	sweet_log_sync();
	gm_wr(gm, GMU_OFF(0x1f800u), 0u);
	printf("SWEET REL2: releaser returned\n");
	sweet_log_sync();
	for (;;) {
		delay(100000);
		printf("SWEET REL2: releaser alive\n");
		sweet_log_sync();
	}
}

static void
g_hfi_dump(vm_offset_t hfi, unsigned int t)
{
	printf("SWEET REL2: t=%u hfi=%u/%u msg=%u/%u\n", t,
	    *(volatile uint32_t *)(hfi + 0x40u),
	    *(volatile uint32_t *)(hfi + 0x44u),
	    *(volatile uint32_t *)(hfi + 0x70u),
	    *(volatile uint32_t *)(hfi + 0x74u));
	sweet_log_sync();
}

static void
g_gmu_boot(void)
{
	int en = 0;
	vm_offset_t hdr, gc, gm, sm, fwp, cache, hfi, tbl, kg;
	const uint8_t *fw;
	uint32_t size, v, w;
	unsigned i, k;
	uint64_t fw_end, p;

	(void)PE_parse_boot_argn("sweetgmuboot", &en, sizeof(en));
	if (!en) {
		return;
	}
	hdr = g_map_win(GMU_HDR_PA, 0x1000);
	gc = g_map_win(GPUCC_PA, 0x10000);
	gm = g_map_win_wimg(GMUREG_PA, 0x40000, VM_WIMG_POSTED);
	sm = g_map_win(KGSMMU_PA, 0x40000);
	kg = g_map_win(KGSL_PA, 0x40000);
	fwp = g_map_win(GMU_FW_PA, 0x10000);
	cache = g_map_win(GMU_CACHE_PA, 0x10000);
	hfi = g_map_win(GMU_HFI_PA, 0x10000);
	tbl = g_map_win(GMU_TBL_PA, 0x10000);
	if (!hdr || !gc || !gm || !sm || !kg || !fwp || !cache || !hfi ||
	    !tbl) {
		printf("SWEET GMU: window alloc failed\n");
		sweet_log_sync();
		return;
	}
	if (*(volatile uint32_t *)(hdr + 0xf00) != GMU_STAGE_MAGIC) {
		printf("SWEET GMU: no staged firmware\n");
		sweet_log_sync();
		return;
	}
	size = *(volatile uint32_t *)(hdr + 0xf04);
	printf("SWEET GMU: staged fw %u bytes\n", size);
	sweet_log_sync();

	/*
	 * 1. CX power.  Match the value the vendor writes (0x228800): besides
	 * clearing SW_COLLAPSE it sets CLK_DIS_WAIT=8 and RETAIN_FF_ENABLE
	 * (bit 11).  Without retention the CX-domain flops -- including the
	 * branch enables we write next -- do not survive.
	 */
	v = gm_rd(gc, 0x9108);
	{
		uint32_t base = v & ~0xf000u;

		base |= 0x8000u;		/* CLK_DIS_WAIT = 8 */
		base |= 1u << 11;		/* RETAIN_FF_ENABLE */
		base |= (1u << 21) | (1u << 17);

		/*
		 * No collapse/power cycle: even with the rail votes moved to
		 * the WAKE TCS group (state=2, as the vendor's rpmh_write
		 * shows) the domain does not come back.  Stay on the state
		 * ABL left until the power path is really understood.
		 */
		gm_wr(gc, 0x9108, base);
		g_wait(2);
	}
	printf("SWEET GMU: cx gdscr=%#x\n", gm_rd(gc, 0x9108));
	sweet_log_sync();

	/*
	 * 1b. GX power, before the clocks.  The vendor's power-on enables
	 * three regulators here (its gdsc-regulator path), and its GX switch
	 * only closes once the mxc rail has been voted -- which the shim does.
	 * Sequence mirrors gdsc-regulator: BLK_ARES pulse, GMEM_RESET pulse,
	 * release GMEM_CLAMP_IO, then clear SW_COLLAPSE and poll PWR_ON.
	 */
	{
		static const uint32_t srst[3] = { 0x9058u, 0x9358u, 0x958cu };
		uint32_t gx = 0x905cu, dom = 0x9504u;

		v = gm_rd(gc, gx);
		if (!(v & (1u << 31))) {
			if (v & 2u) {
				gm_wr(gc, gx, v & ~2u);
			}
			for (i = 0; i < 3u; i++) {
				gm_wr(gc, srst[i], gm_rd(gc, srst[i]) | 1u);
			}
			g_wait(1);
			for (i = 0; i < 3u; i++) {
				gm_wr(gc, srst[i], gm_rd(gc, srst[i]) & ~1u);
			}
			v = gm_rd(gc, dom);
			gm_wr(gc, dom, v | (1u << 4));	/* GMEM_RESET */
			g_wait(1);
			gm_wr(gc, dom, (v & ~(1u << 4)) & ~1u);	/* clamp off */
			v = gm_rd(gc, gx);
			gm_wr(gc, gx, v & ~1u);		/* SW_COLLAPSE off */
			g_wait(3);
		}
		printf("SWEET GMU: gx gdscr=%#x dom=%#x\n", gm_rd(gc, gx),
		    gm_rd(gc, dom));
		sweet_log_sync();
	}

	/*
	 * 2. clocks, exactly the sequence the vendor kernel performs (captured
	 * with kprobes on regmap_update_bits_base):
	 *   hub RCG: CMD.ROOT_EN=1, CFG = HW_CLK_CTRL|div=1|src=tcxo,
	 *            CMD.UPDATE=1, CMD.ROOT_EN=0
	 *   smmu vote, GCC memnoc, then gpu_cc_ahb = 1
	 * The HF_CLK_CTRL bit and the divider are what we were missing: a
	 * branch cannot latch while its parent RCG is not running that way.
	 */
	{
		static const uint32_t rcg[2] = { 0x93ecu, 0x9318u };
		uintptr_t gg = g_map_win(0x100000ull, 0x100000u);

		for (i = 0; i < 2u; i++) {
			uint32_t c = rcg[i], f = c + 4u;

			gm_wr(gc, c, gm_rd(gc, c) | 0x2u);	/* ROOT_EN */
			gm_wr(gc, f, (gm_rd(gc, f) & ~0x10371fu) |
			    0x100001u);				/* HW_CLK_CTRL|div 1 */
			gm_wr(gc, c, (gm_rd(gc, c) & ~1u) | 1u); /* UPDATE */
			/*
			 * Leave ROOT_EN on: clearing it (as an earlier version
			 * did) gates the root back off, and every branch below
			 * -- hub_ahb_div, gpu_cc_ahb -- stays dead.
			 */
		}
		gm_wr(gc, 0x93e8u, gm_rd(gc, 0x93e8u) | 1u);	/* hub_aon */
		gm_wr(gc, 0x7000u, gm_rd(gc, 0x7000u) | 1u);	/* smmu vote */
		if (gg) {
			gm_wr(gg, 0x81010u, gm_rd(gg, 0x81010u) | 1u);
			gm_wr(gg, 0x81018u, gm_rd(gg, 0x81018u) | 1u);
		}
		gm_wr(gc, 0x9148u, gm_rd(gc, 0x9148u) | 1u);	/* hub_cx_int */
		gm_wr(gc, 0x913cu, gm_rd(gc, 0x913cu) | 1u);	/* cx_gmu */
		gm_wr(gc, 0x9144u, gm_rd(gc, 0x9144u) | 1u);	/* cxo */
		/*
		 * hub_ahb_div(0x9430) sits between the hub RCG and
		 * gpu_cc_ahb, and every survey has it reading 0: we have only
		 * ever read it, never enabled it.  Try it *before* the ahb
		 * branch.  If 0x9430 is a plain divider CFG and not a branch
		 * CFG, bit 0 carries no enable meaning and the div/neigh
		 * readbacks below show that instead.  This is the last
		 * untried item on the hub clock chain.
		 */
		gm_wr(gc, 0x9430u, gm_rd(gc, 0x9430u) | 1u);	/* hub_ahb_div */
		/*
		 * Witness: the 23:25 run read hub cmd=0x2 / cfg=0x100001 with no
		 * hub_ahb_div write, and this run read cmd=0 cfg=0 / neigh=0 0 with
		 * it.  Re-read the registers that were known good *before* the ahb
		 * branch write so one boot says whether the 0x9430 write alone
		 * darkened GPUCC, or whether the ahb write that follows did.
		 */
		printf("SWEET GMU: ahbdiv witness gdscr=%#x cxgmu=%#x hubcx=%#x "
		    "cmd=%#x cfg=%#x\n",
		    gm_rd(gc, 0x9108u), gm_rd(gc, 0x913cu), gm_rd(gc, 0x9148u),
		    gm_rd(gc, 0x93ecu), gm_rd(gc, 0x93f0u));
		sweet_log_sync();
		gm_wr(gc, 0x911cu, gm_rd(gc, 0x911cu) | 1u);	/* gpu_cc_ahb */
		/*
		 * Gated-read test for this branch.  The page self-gates when idle
		 * (the release path already notes gpu_cc_ahb closes itself without
		 * traffic), and Run D read hub CMD/CFG as 0x2/0x100001 in one print
		 * and all-zero two lines later in the same boot with no write in
		 * between.  A single readback after a write may therefore be sampling
		 * a clock that is off rather than a bit that did not take.  Burst-read
		 * the branch just written and cx_gmu(0x913c) as the control that is
		 * known to latch, then read both again after a gap.
		 */
		{
			uint32_t bv[4];
			uint32_t cv[4];
			unsigned int s;

			for (s = 0; s < 4u; s++) {
				bv[s] = gm_rd(gc, 0x911cu);
				cv[s] = gm_rd(gc, 0x913cu);
			}
			printf("SWEET GMU: ahb burst ahb=%x %x %x %x cxgmu=%x %x %x %x\n",
			    bv[0], bv[1], bv[2], bv[3], cv[0], cv[1], cv[2], cv[3]);
			sweet_log_sync();
			g_wait(20);
			printf("SWEET GMU: ahb burst after20ms ahb=%x cxgmu=%x\n",
			    gm_rd(gc, 0x911cu), gm_rd(gc, 0x913cu));
			sweet_log_sync();
		/*
		 * Run F showed that back-to-back reads of this page return the
		 * previous transfer rather than the addressed register: cx_gmu,
		 * which read 0x2221 one line earlier in the witness, came back
		 * 0x100001 four times in a row inside the burst -- 0x100001 being
		 * the hub CFG value transferred just before it -- while the pair
		 * taken after a 20 ms gap (ahb=0, cxgmu=2221) is self-consistent.
		 * So a trustworthy gpu_cc_ahb sample is one read per gap.  Take
		 * three gapped reads, then one immediately-following pair as the
		 * in-run control that the artifact is still there.
		 */
		{
			uint32_t ga, gcm, gh, b1, b2;

			g_wait(20);
			ga = gm_rd(gc, 0x911cu);
			g_wait(20);
			gcm = gm_rd(gc, 0x913cu);
			g_wait(20);
			gh = gm_rd(gc, 0x93ecu);
			b1 = gm_rd(gc, 0x911cu);
			b2 = gm_rd(gc, 0x911cu);
			printf("SWEET GMU: gap ahb=%x cxgmu=%x cmd=%x backtoback=%x %x\n",
			    ga, gcm, gh, b1, b2);
			sweet_log_sync();
		}
		}
		/*
		 * Sandwiched calibration, replacing the 0227 loop.  Reading the same
		 * register back to back cannot detect a stale readback at all: a
		 * repeated gpu_cc_ahb matches its own previous transfer whether or
		 * not the bridge is feeding us pipeline data, so 0227 would print
		 * valid=8/8 either way and the A/B decision would be unreadable.
		 * Each sample now takes CX_GDSCR before, between and after two
		 * gpu_cc_ahb reads; all three calibration words must be exactly
		 * 0xf8228800 (this function wrote 0x228800 with PWR_ON, and six
		 * boots of witnesses agree) and the two target reads must match.
		 * Only pairs passing both checks count, and only counted pairs may
		 * decide A or B.
		 */
		{
			uint32_t a1, a2, c1, c2, c3;
			uint32_t va[8];
			unsigned int t, valid = 0, bad = 0;

			for (t = 0; t < 8u; t++) {
				c1 = gm_rd(gc, 0x9108u);
				a1 = gm_rd(gc, 0x911cu);
				c2 = gm_rd(gc, 0x9108u);
				a2 = gm_rd(gc, 0x911cu);
				c3 = gm_rd(gc, 0x9108u);
				if (c1 == 0xf8228800u && c2 == 0xf8228800u &&
				    c3 == 0xf8228800u && a1 == a2) {
					va[valid++] = a1;
				} else {
					bad++;
				}
				g_wait(2);
			}
			printf("SWEET GMU: cal2 valid=%u bad=%u ahb=%x %x %x %x "
			    "%x %x %x %x\n", valid, bad, va[0], va[1], va[2], va[3],
			    va[4], va[5], va[6], va[7]);
			sweet_log_sync();
		}
		printf("SWEET GMU: rootkeep ahbdiv hub cmd=%#x cfg=%#x "
		    "div=%#x ahb=%#x gmu=%#x\n",
		    gm_rd(gc, 0x93ecu), gm_rd(gc, 0x93f0u),
		    gm_rd(gc, 0x9430u), gm_rd(gc, 0x911cu),
		    gm_rd(gc, 0x9318u));
		sweet_log_sync();
		printf("SWEET GMU: ahbdiv neigh=%#x %#x\n",
		    gm_rd(gc, 0x9434u), gm_rd(gc, 0x9438u));
		sweet_log_sync();
	}

	/* 3. firmware: blocks are {addr, size, 0xdeadbeef, 0xdeadbeef} */
	fw = (const uint8_t *)fwp;
	fw_end = (uint64_t)fw + size;
	for (p = (uint64_t)fw; p + 16 <= fw_end; ) {
		const uint8_t *q = (const uint8_t *)p;
		uint32_t addr, bsize;

		if (!(q[8] == 0xde && q[9] == 0xad && q[10] == 0xbe &&
		    q[11] == 0xef && q[12] == 0xde && q[13] == 0xad &&
		    q[14] == 0xbe && q[15] == 0xef)) {
			p += 4;
			continue;
		}
		addr = gm_fw_ld32(q);
		bsize = gm_fw_ld32(q + 4);
		p += 16;
		if (bsize == 0 || p + bsize > fw_end) {
			break;
		}
		if (addr < 0x4000u) {
			for (i = 0; i < bsize; i += 4u) {
				gm_wr(gm, GMU_OFF(0x1b400u) + addr + i,
				    gm_fw_ld32((const uint8_t *)(p + i)));
			}
		} else if (addr >= 0x10000000u) {
			for (i = 0; i < bsize; i += 4u) {
				gm_wr(gm, GMU_OFF(0x1c400u) +
				    (addr - 0x10004000u) + i,
				    gm_fw_ld32((const uint8_t *)(p + i)));
			}
		} else {
			for (i = 0; i + 4u <= bsize; i += 4u) {
				gm_wr(cache, (addr - 0x4000u) + i,
				    gm_fw_ld32((const uint8_t *)(p + i)));
			}
		}
		p += bsize;
	}
	printf("SWEET GMU: fw loaded, itcm0=%#x\n",
	    gm_rd(gm, GMU_OFF(0x1b400u)));
	sweet_log_sync();

	/* 4. HFI queue table (adreno_hfi.h init_queues) */
	{
		static const uint32_t qt[3] = { 0x0a00u, 0x0a01u, 0x2802u };

		/* Device memory: 32-bit stores only, no NEON striding. */
		gm_wr(hfi, 0u, 0u);
		gm_wr(hfi, 4u, 42u);
		gm_wr(hfi, 8u, 6u);
		gm_wr(hfi, 12u, 12u);
		gm_wr(hfi, 16u, 3u);
		gm_wr(hfi, 20u, 3u);
		for (i = 0; i < 3u; i++) {
			uint32_t o = (6u + i * 12u) * 4u;

			gm_wr(hfi, o + 0u, 1u);
			gm_wr(hfi, o + 4u,
			    (uint32_t)GMU_HFI_PA + 176u + i * 4096u);
			gm_wr(hfi, o + 8u, qt[i]);
			gm_wr(hfi, o + 12u, 1024u);
		}
	}

	/* 5. SID-5 context bank 1: cache at 0x4000, hfi at 0x60000000 */
	{
		uint32_t tpa = (uint32_t)GMU_TBL_PA;
		vm_offset_t cb = sm + 0x21000u;	/* CB1 */

		/* Tables live in Device memory too: 32-bit stores only. */
		for (i = 0; i < 512u * 5u; i++) {
			gm_wr(tbl, i * 4u, 0u);
		}
		gm_wr(tbl, 0x0000u, (tpa + 0x1000u) | 3u);	/* L1[0] */
		gm_wr(tbl, 0x0004u, 0u);
		gm_wr(tbl, 0x1000u, (tpa + 0x2000u) | 3u);	/* L2[0] */
		gm_wr(tbl, 0x1004u, 0u);
		for (i = 4; i < 20u; i++) {			/* L3: cache */
			gm_wr(tbl, 0x2000u + i * 8u,
			    ((uint32_t)GMU_CACHE_PA + (i - 4u) * 0x1000u) |
			    0x703u);
			gm_wr(tbl, 0x2004u + i * 8u, 0u);
		}
		gm_wr(tbl, 0x0008u, (tpa + 0x3000u) | 3u);	/* L1[1] */
		gm_wr(tbl, 0x000cu, 0u);
		gm_wr(tbl, 0x3000u, (tpa + 0x4000u) | 3u);	/* H2[0] */
		gm_wr(tbl, 0x3004u, 0u);
		for (i = 0; i < 16u; i++) {			/* H3: hfi */
			gm_wr(tbl, 0x4000u + i * 8u,
			    ((uint32_t)GMU_HFI_PA + i * 0x1000u) | 0x703u);
			gm_wr(tbl, 0x4004u + i * 8u, 0u);
		}
		__asm__ volatile("dsb sy" ::: "memory");

		gm_wr(cb, 0x00u, 0u);
		gm_wr(sm, 0x1004u, 0x1f000u);		/* CBAR(1) */
		gm_wr(sm, 0x1804u, 1u);			/* CBA2R(1) */
		gm_wr(cb, 0x20u, (uint32_t)(GMU_TBL_PA));
		gm_wr(cb, 0x24u, (uint32_t)(GMU_TBL_PA >> 32));
		gm_wr(cb, 0x10u, 0u);			/* TCR2 */
		gm_wr(cb, 0x30u, 0x3520u);		/* TCR */
		gm_wr(cb, 0x38u, 0xffu);		/* MAIR0 */
		gm_wr(cb, 0x00u, 0x1u);			/* enable */
		gm_wr(sm, 0x804u, 0x80000005u);		/* SMR(1) SID 5 */
		gm_wr(sm, 0xc04u, 0x1u);		/* S2CR(1) */
		gm_wr(sm, 0x68u, 0u);			/* TLBIALLNSNH */
		gm_wr(sm, 0x70u, 0u);			/* sTLBGSYNC */
		printf("SWEET GMU: smmu smr1=%#x s2cr1=%#x cb1=%#x\n",
		    gm_rd(sm, 0x804u), gm_rd(sm, 0xc04u), gm_rd(cb, 0x00u));
		sweet_log_sync();
	}

	/* 6. vendor pre-boot register configuration */
	{
		static const struct { uint32_t w, v; } cfg[] = {
			{ 0x1f8f1u, 0x1u }, { 0x1f8f0u, 0x1u },
			{ 0x1f81cu, 0x0u }, { 0x1f801u, 0x2u },
			{ 0x1f985u, (uint32_t)GMU_HFI_PA },
			{ 0x1f984u, 0x1u },
			{ 0x1f82du, 0x4052u },
			{ 0x1f9ceu, 0x0u }, { 0x1f9cfu, 0x7030200u },
			{ 0x1f9cdu, 0x60002003u },
			{ 0x1f40fu, 0x1u }, { 0x1f400u, 0x1u },
			{ 0x1f401u, 0x1u },
			{ 0x1f991u, 0xffffffffu }, { 0x1f993u, 0xffff7ff9u },
			{ 0x23b11u, 0x80c808a0u },
		};

		for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
			gm_wr(gm, GMU_OFF(cfg[i].w), cfg[i].v);
		}
		gm_wr(gc, 0xe000u + 0x39u * 4u, 1u);	/* TCM retention */
		gm_wr(kg, 0x3c45u * 4u, 0u);		/* GBIF halt off */
	}
	printf("SWEET GMU: pre-boot cfg done\n");
	sweet_log_sync();

	/*
	 * 7. Release the CM3.  From here on never touch a GMU register: once
	 * the CM3 runs, the AP's access to the block stalls (the AHB is the
	 * GMU's now).  The HFI queues live in our own DRAM, so poll those.
	 */
	{
		int rel = 0;

		(void)PE_parse_boot_argn("sweetgmurel", &rel, sizeof(rel));
		if (rel == 2) {
			/*
			 * sweetgmurel=2: release from the thread above and watch from
			 * here.  Sixty DDR-only samples at 20 ms is 1.2 s of proof
			 * that the machine still runs after the store; moving write
			 * index is the first piece of evidence that the CM3 consumes
			 * the queues, without reading a single GPU register.
			 */
			unsigned int t;
			thread_t th;

			g_hfi_dump(hfi, 60000u);
			if (kernel_thread_start((thread_continue_t)g_rel_thread,
			    NULL, &th) == KERN_SUCCESS) {
				thread_deallocate(th);
			} else {
				printf("SWEET REL2: no releaser thread\n");
				sweet_log_sync();
			}
			for (t = 0; t < 60u; t++) {
				delay(20000);
				g_hfi_dump(hfi, t);
			}
			goto gmu_done;
		}

		if (rel) {
			/*
			 * The shim's secondary CPU releases the CM3 (the core
			 * that does it is lost).  GPUCC stays reachable, so
			 * watch GX_GDSCR: the GMU powers GX as soon as its
			 * firmware runs.
			 */
			printf("SWEET GMU: release delegated to shim cpu\n");
			sweet_log_sync();
			uint32_t r0, w0, r1, w1;

			/*
			 * Hand-off: the shim core releases CM3 when it sees this.
			 * The page must be uncached -- the shim polls it with the
			 * MMU off, so a cached store would never become visible.
			 */
			vm_offset_t ho = g_map_win_wimg(0xa2e0f000ull, 0x1000u,
			    VM_WIMG_IO);

			if (ho) {
				*(volatile uint32_t *)(ho + 0xf08) = 0x47505552u;
				__asm__ volatile("dsb sy" ::: "memory");
			}
			g_wait(2000);
			printf("SWEET GMU: shim marks pre=%#x post=%#x beat=%u\n",
			    ho ? *(volatile uint32_t *)(ho + 0xf08) : 0xffffffffu,
			    ho ? *(volatile uint32_t *)(ho + 0xf0c) : 0xffffffffu,
			    ho ? *(volatile uint32_t *)(ho + 0xf10) : 0xffffffffu);
			sweet_log_sync();
			v = gm_rd(gc, 0x905cu);
			r0 = *(volatile uint32_t *)(hfi + (6u + 10u) * 4u);
			w0 = *(volatile uint32_t *)(hfi + (6u + 11u) * 4u);
			r1 = *(volatile uint32_t *)(hfi + (18u + 10u) * 4u);
			w1 = *(volatile uint32_t *)(hfi + (18u + 11u) * 4u);
			printf("SWEET GMU: gx gdscr=%#x pwr_on=%u cmd r/w=%u/%u "
			    "msg r/w=%u/%u\n", v, (v >> 31) & 1u, r0, w0, r1, w1);
			printf("SWEET GMU: smmu gfsr=%#x fsynr0=%#x fsynr1=%#x "
			    "fsynr2=%#x\n", gm_rd(sm, 0x48u), gm_rd(sm, 0x50u),
			    gm_rd(sm, 0x54u), gm_rd(sm, 0x58u));
			{
				vm_offset_t ao = g_map_win(0x3d40000ull, 0x1000u);

				if (ao) {
					printf("SWEET GMU: ao %#x %#x %#x %#x\n",
					    gm_rd(ao, 0x0u), gm_rd(ao, 0x4u),
					    gm_rd(ao, 0x8u), gm_rd(ao, 0x10u));
				}
			}
			sweet_log_sync();
			goto gmu_done;
		}
	}
	/*
	 * 7a. A clock/rail-only run must not release the CM3.  Without
	 * this switch rel==0 always fell through to the release below,
	 * so every GMU boot ended with the core wedged and the ring cut
	 * short (no PSCI SYSTEM_RESET, watchdog picked it up).  Exit to
	 * gmu_done instead: the PSCI reset fires and the ring is pulled
	 * whole, which is what makes the ahbdiv readback trustworthy.
	 */
	{
		int norel = 0;

		(void)PE_parse_boot_argn("sweetgmunorel", &norel,
		    sizeof(norel));
		if (norel) {
			printf("SWEET GMU: release suppressed (sweetgmunorel=%d)\n",
			    norel);
			sweet_log_sync();
			goto gmu_done;
		}
	}
	printf("SWEET GMU: releasing cm3 (posted window)\n");
	sweet_log_sync();
	/*
	 * The CM3 raises its GMU/HFI SPIs (304/305 in the live DT) the
	 * moment it runs.  They are level-sensitive and nothing in XNU has
	 * masked them, so the releasing CPU drowns in interrupts and every
	 * probe print after the release disappears.  Mask and clear them.
	 */
	{
		vm_offset_t gd = g_map_win_wimg(0x17100000ull, 0x2000u,
		    VM_WIMG_IO);

		if (gd) {
			uint32_t b = (1u << 16) | (1u << 17);

			gm_wr(gd, 0x1a4u, b);	/* GICD_ICENABLER9 */
			gm_wr(gd, 0x2a4u, b);	/* GICD_ICPENDR9 */
			__asm__ volatile("dsb sy" ::: "memory");
			printf("SWEET GMU: gic masked en=%#x pend=%#x\n",
			    gm_rd(gd, 0x1a4u), gm_rd(gd, 0x2a4u));
			sweet_log_sync();
		}
	}
	/*
	 * Put the GMU AO AHB fence in ALLOW mode and clear any latched
	 * fence status before the CM3 runs.  gen7_gmu_pwrctrl_suspend does
	 * exactly these two writes; without them the firmware can leave the
	 * fence armed, and from that point *every* AP access to GPUCC, the
	 * GMU, the SMMU and RSCC wedges the core.
	 */
	gm_wr(gm, GMU_OFF(0x23b14u), 0x7u);	/* AHB_FENCE_STATUS_CLR */
	gm_wr(gm, GMU_OFF(0x23b10u), 0u);	/* AO_AHB_FENCE_CTRL: allow */
	printf("SWEET GMU: fence ctrl=%#x status=%#x\n",
	    gm_rd(gm, GMU_OFF(0x23b10u)), gm_rd(gm, GMU_OFF(0x23b14u)));
	sweet_log_sync();
	/*
	 * Vendor order: gpu_cc_ahb is enabled immediately *before* the
	 * release, not seconds earlier.  It self-gates when idle, so doing
	 * it with the clocks and then staging firmware leaves it closed by
	 * the time the CM3 starts.
	 */
	printf("SWEET GMU: ahb pre-release=%#x\n", gm_rd(gc, 0x911cu));
	gm_wr(gc, 0x911cu, gm_rd(gc, 0x911cu) | 1u);
	printf("SWEET GMU: ahb armed=%#x\n", gm_rd(gc, 0x911cu));
	sweet_log_sync();
	g_canary_arm();
	gm_wr(gm, GMU_OFF(0x1f800u), 0u);
	/*
	 * Do not touch the GMU window after the release: once the CM3 owns
	 * the AHB, an AP read there is what wedges this core.  Everything we
	 * still want (GPUCC, the AO block, the HFI queues in DDR) lives
	 * outside that window.
	 */
	printf("SWEET GMU: release written, gmu read skipped\n");
	sweet_log_sync();
	g_wait(50);
	/*
	 * RSCC sits outside the GPU AHB: its TCS DRV0 status words are what
	 * the vendor polls to decide whether the GMU is healthy.  Word
	 * offsets from gen7_reg.h, 32-bit registers (offset << 2).
	 */
	{
		vm_offset_t rs = g_map_win_wimg(0x3d50000ull, 0x2000u,
		    VM_WIMG_IO);

		if (rs) {
			printf("SWEET GMU: rscc tcs=%#x %#x %#x %#x\n",
			    gm_rd(rs, 0xd18u), gm_rd(rs, 0xfb8u),
			    gm_rd(rs, 0x1258u), gm_rd(rs, 0x14f8u));
			sweet_log_sync();
		} else {
			printf("SWEET GMU: rscc map failed\n");
			sweet_log_sync();
		}
	}
	g_wait(250);
	{
		vm_offset_t ao = g_map_win_wimg(0x3d40000ull, 0x1000u,
		    VM_WIMG_IO);

		/*
		 * GPUCC is behind the GMU's AHB once the CM3 runs: reading it
		 * after the release is what wedges the core.  Only the SMMU
		 * (separate block), DDR (HFI queues) and the AO block stay
		 * reachable from here, so those are all we touch.
		 */
		printf("SWEET GMU: after rel hfi=%u/%u msg=%u/%u\n",
		    *(volatile uint32_t *)(hfi + 0x40u),
		    *(volatile uint32_t *)(hfi + 0x44u),
		    *(volatile uint32_t *)(hfi + 0x70u),
		    *(volatile uint32_t *)(hfi + 0x74u));
		sweet_log_sync();
		g_wait(200);
		printf("SWEET GMU: hfi later=%u/%u msg=%u/%u\n",
		    *(volatile uint32_t *)(hfi + 0x40u),
		    *(volatile uint32_t *)(hfi + 0x44u),
		    *(volatile uint32_t *)(hfi + 0x70u),
		    *(volatile uint32_t *)(hfi + 0x74u));
		sweet_log_sync();
		if (ao) {
			printf("SWEET GMU: ao=%#x %#x %#x\n", gm_rd(ao, 0x0u),
			    gm_rd(ao, 0x4u), gm_rd(ao, 0x8u));
			sweet_log_sync();
		}
		/* The SMMU is the last block we dare touch. */
		printf("SWEET GMU: smmu gfsr=%#x fsynr1=%#x\n",
		    gm_rd(sm, 0x48u), gm_rd(sm, 0x54u));
		sweet_log_sync();
	}
	v = 0;
	if (0) {
		gm_wr(gm, GMU_OFF(0x1f986u), 1u);
		for (k = 0; k < 200u; k++) {
			v = gm_rd(gm, GMU_OFF(0x1f980u));
			if (v & 1u) {
				break;
			}
			g_wait(1);
		}
		printf("SWEET GMU: hfi status=%#x after %u ms\n", v, k);
		sweet_log_sync();
	}

gmu_done:
	/* Done: reset so the ring can be pulled from Android/TWRP. */
	{
		int rb = 0, gl = 1;

		(void)PE_parse_boot_argn("sweetreboot", &rb, sizeof(rb));
		(void)PE_parse_boot_argn("sweetglink", &gl, sizeof(gl));
		if (rb && !gl) {
			printf("SWEET GMU: PSCI SYSTEM_RESET\n");
			sweet_log_sync();
			{
				register uint64_t x0 __asm("x0") = 0x84000009ull;

				__asm volatile("dsb sy" ::: "memory");
				__asm volatile("smc #0" : "+r"(x0) ::
				    "x1", "x2", "x3", "memory");
			}
			for (;;) {
				delay(1000);
			}
		}
	}
}

/* The ADSP reaches DDR through the platform's apps-smmu (SMMU-500 at
 * 0x15000000, lpass_tbu at 0x151e9000, SID range 0x180000..0x1bffff).
 * Read-only probe: model, stream-match entries, and any existing mapping
 * that covers the ADSP's SID. */
static void
g_smmu_probe(void)
{
	static bool done;
	vm_offset_t v;
	uint32_t id0, id1, cr0;
	unsigned num_smr;
	int valid = 0, free_slot = -1, sid18 = -1, hi_free = -1;

	if (done) {
		return;
	}
	done = true;
	/* io_map() allocates from the fixed 8 MiB io_submap and run76
	 * panicked trying to take 1 MiB of what was left.  Use kernel_map
	 * windows instead (same helper the SHM/IPCC paths use). */
	v = g_win_alloc(0x2000u);
	if (v != 0) {
		v = g_win_map(v, 0x15000000u, 0x2000u);
	}
	if (!v) {
		printf("SWEET SMMU: map failed\n");
		return;
	}
	/* SMMU-500: sCR0 is +0x00 (bit0 CLIENTPD, bit10 USFCFG), ID0/ID1 are
	 * +0x20/+0x24, SMRs at +0x800, S2CRs at +0xc00.  VALID is bit 31. */
	cr0 = *(volatile uint32_t *)(v + 0x00);
	id0 = *(volatile uint32_t *)(v + 0x20);
	id1 = *(volatile uint32_t *)(v + 0x24);
	printf("SWEET SMMU: id0=%#x id1=%#x scr0=%#x\n", id0, id1, cr0);
	sweet_log_sync();
	/* ID0[7:0] is the number of SMR groups (113 here, matching Android's
	 * "stream matching with 113 register groups").  Reading past the last
	 * implemented SMR returns an external abort: run65 read SMR[113] at
	 * offset 0x9c4 and panicked.  Never hardcode 120. */
	num_smr = id0 & 0xffu;
	if (num_smr == 0 || num_smr > 128) {
		num_smr = 113;
	}
	for (unsigned n = 0; n < num_smr; n++) {
		uint32_t smr = *(volatile uint32_t *)(v + 0x800 + 4 * n);
		uint32_t s2cr = *(volatile uint32_t *)(v + 0xc00 + 4 * n);

		if (!(smr & 0x80000000u)) {
			if (free_slot < 0) {
				free_slot = (int)n;
			}
			if (n == 0x4du) {
				hi_free = (int)n;
			}
			continue;
		}
		valid++;
		printf("SWEET SMMU: smr%d sid=%#x mask=%#x s2cr=%#x\n",
		    n, smr & 0xffffu, (smr >> 16) & 0x7fffu, s2cr);
		sweet_log_sync();
		{
			/* A masked SMR also matches 0x1801: compare only
			 * the bits the mask leaves significant. */
			uint32_t mask = (smr >> 16) & 0x7fffu;

			if ((((smr & 0xffffu) ^ 0x1801u) & ~mask) == 0) {
				sid18 = (int)n;
			}
		}
	}
	printf("SWEET SMMU: %d valid stream entries\n", valid);

	/*
	 * One decisive test (sweetcbar=1): is CBAR writable at all from this
	 * execution environment?  CBAR(6) and CBAR(52) abort, but banks 0..5
	 * are the ones the bootloader visibly configured, so CBAR(0) is the
	 * best candidate for "a bank that belongs to us".  Writing 0 makes it
	 * CBAR_TYPE_S2_TRANS with VMID 0.  Either the store takes (and we can
	 * build a real stage-2 context on it) or it aborts (CBAR is closed).
	 */
	{
		int cbar_try = 0;

		(void)PE_parse_boot_argn("sweetcbar", &cbar_try,
		    sizeof(cbar_try));
		if (cbar_try) {
			uint32_t before = *(volatile uint32_t *)(v + 0x1000u);
			uint32_t after;

			*(volatile uint32_t *)(v + 0x1000u) = 0;
			__asm__ volatile("dsb sy" ::: "memory");
			after = *(volatile uint32_t *)(v + 0x1000u);
			printf("SWEET SMMU: CBAR0 %#x -> %#x (writable=%d)\n",
			    before, after, after == 0);
			sweet_log_sync();
			return;
		}
	}

	/* The fault snapshot needs these windows on every path, including the
	 * ones that return early. */
	if (g_gr_win == 0) {
		g_gr_win = v;
		if (g_cb_win == 0) {
			g_cb_win = g_map_win(0x15080000u, 0x10000u);
		}
	}
	/*
	 * sweetnostall=1: clear CFCFG on every context bank the bootloader
	 * left configured.  The vendor's arm-smmu-qcom.c points out that this
	 * SMMU rewrites a BYPASS S2CR into FAULT, and its bypass bank is
	 * programmed with SCTLR=0 rather than the bootloader's 0xe0.  A 0xe0
	 * SCTLR has CFCFG=1, i.e. a context fault STALLS the transaction
	 * instead of terminating it - and a stalled transaction on the LPASS
	 * TBU is exactly the "whole SoC freezes, log stops mid-line" shape we
	 * keep hitting.  Only bit 7 changes; M and the enabled banks stay.
	 */
	{
		int nostall = 0;

		(void)PE_parse_boot_argn("sweetnostall", &nostall,
		    sizeof(nostall));
		if (nostall && g_cb_win != 0) {
			for (unsigned n = 0; n < 6; n++) {
				volatile uint32_t *s =
				    (volatile uint32_t *)(g_cb_win +
				    n * 0x1000u);
				uint32_t b = *s;

				*s = b & ~0x80u;	/* CFCFG = 0 */
				__asm__ volatile("dsb sy" ::: "memory");
				printf("SWEET SMMU: cb%u sctlr %#x -> %#x\n",
				    n, b, *s);
			}
			sweet_log_sync();
		}
	}
	if (g_cb_win == 0) {
		g_cb_win = g_map_win(0x15080000u, 0x10000u);
	}
	/*
	 * Stage-1 path (sweets1=1).  The vendor header for this SoC says
	 * ID0.S2TS=0: this SMMU does NOT do stage-2 translation at all
	 * (ID0=0x4c017e71, ID1=0x6000004b with NUMS2CB=0).  So the only
	 * workable translation is stage-1 on a CBAR.Type=1 bank
	 * (S1_TRANS_S2_BYPASS), and banks 0..5 already read Type=1 while
	 * CBAR itself is not writable.  Take one of those banks, build an
	 * S1 identity map for the PCM page, enable it, and route the audio
	 * SID at it.  CFCFG stays 0 (terminate, not stall) so a bad access
	 * reports instead of hanging the interconnect.
	 */
	{
		int s1 = 0;

		(void)PE_parse_boot_argn("sweets1", &s1, sizeof(s1));
		if (s1) {
			int cbn = 5, iova_mode = 0;
			int s1_ns = 0, s1_ttbr1 = 0, s1_tcr2 = 0, wide = 0;
			int s1_attr = 0, s1_cbar = 0, s1_off = 0;
			uint32_t s1_tcr = 0, s1_sctlr = 0;
			uint64_t s1_out = 0, s1_out_pa = 0;
			uint64_t s1_tbl = 0, s1_va = 0, s1_pa = 0;
			uint64_t s1_leaf = 0x701ull;
			uint32_t s1_mair = 0xffu, cbar_val = 0x10000u;
			vm_offset_t cbw, tb1;
			uint32_t cbar, sc, re_sctlr, re_ttbr, re_tcr, re_mair;

			(void)PE_parse_boot_argn("sweets1cb", &cbn,
			    sizeof(cbn));
			(void)PE_parse_boot_argn("sweetiova", &iova_mode,
			    sizeof(iova_mode));
			/* Empirical knobs for the walk itself; each is a
			 * candidate explanation for the persistent TF. */
			(void)PE_parse_boot_argn("sweets1ns", &s1_ns,
			    sizeof(s1_ns));
			(void)PE_parse_boot_argn("sweets1tcr2", &s1_tcr2,
			    sizeof(s1_tcr2));
			(void)PE_parse_boot_argn("sweets1ttbr1", &s1_ttbr1,
			    sizeof(s1_ttbr1));
			(void)PE_parse_boot_argn("sweets1tbl", &s1_tbl,
			    sizeof(s1_tbl));
			(void)PE_parse_boot_argn("sweets1va", &s1_va,
			    sizeof(s1_va));
			(void)PE_parse_boot_argn("sweets1pa", &s1_pa,
			    sizeof(s1_pa));
			/*
			 * sweets1attr selects the *leaf* memory attributes, i.e.
			 * how the SMMU presents the buffer to the DSP on the AXI
			 * side.  The ADSP is not in the AP coherency domain, so an
			 * Inner-Shareable Normal-WB transaction makes the
			 * interconnect snoop AP caches for a master that cannot
			 * participate in coherency; a stall there looks exactly
			 * like the ~200 ms hang we see.  Sweep this first.
			 *   0  Attr0 = Normal WB  + SH inner   (current, 0x701)
			 *   1  Attr1 = Normal NC  + SH inner
			 *   2  Attr1 = Normal NC  + SH outer
			 *   3  Attr1 = Normal NC  + non-shareable
			 *   4  Attr1 = Device nGnRE + non-shareable
			 *   5  Attr1 = Device nGnRnE + non-shareable
			 * MAIR0 always keeps Attr0 = 0xff (Normal WB) so the entry
			 * stays valid for any index we pick.
			 */
			(void)PE_parse_boot_argn("sweets1attr", &s1_attr,
			    sizeof(s1_attr));
			(void)PE_parse_boot_argn("sweets1cbar", &s1_cbar,
			    sizeof(s1_cbar));
			/*
			 * sweets1off=1 leaves the bank's CBAR at type 1 but writes
			 * SCTLR=0, i.e. stage-1 translation disabled.  That is
			 * exactly what the vendor driver does for its emulated
			 * bypass bank (CBAR 0x1f300, SCTLR 0), so the DSP's access
			 * passes straight through to the physical address with no
			 * page table involved.  Use it with sweets1cb=74 so the
			 * shared SID 0x584 bank keeps translating as before.
			 */
			(void)PE_parse_boot_argn("sweets1off", &s1_off,
			    sizeof(s1_off));
			(void)PE_parse_boot_argn("sweets1tcr", &s1_tcr,
			    sizeof(s1_tcr));
			(void)PE_parse_boot_argn("sweets1sctlr", &s1_sctlr,
			    sizeof(s1_sctlr));
			/*
			 * sweets1out=<block base> moves ONLY the translation
			 * output, leaving g_map_pa (which is also where the tone
			 * is written and what SHM_MAP registers) untouched.
			 * sweets1pa moves both - and that is how c2 wrote 20 KiB
			 * of tone straight into the ADSP's live carve-out and
			 * panicked the DSP.  Use this one to target foreign
			 * memory.
			 */
			(void)PE_parse_boot_argn("sweets1out", &s1_out,
			    sizeof(s1_out));
			switch (s1_attr) {
			case 1:		/* Normal NC, inner shareable */
				s1_mair = 0xff44u;
				s1_leaf = 0x705ull;
				break;
			case 2:		/* Normal NC, outer shareable */
				s1_mair = 0xff44u;
				s1_leaf = 0x505ull;
				break;
			case 3:		/* Normal NC, non-shareable */
				s1_mair = 0xff44u;
				s1_leaf = 0x305ull;
				break;
			case 4:		/* Device nGnRE, non-shareable */
				s1_mair = 0xff04u;
				s1_leaf = 0x305ull;
				break;
			case 5:		/* Device nGnRnE, non-shareable */
				s1_mair = 0xff00u;
				s1_leaf = 0x305ull;
				break;
			default:	/* 0 (and anything else) = as before */
				s1_attr = 0;
				break;
			}
			/*
			 * sweets1ap=<1..3> rewrites ONLY the leaf descriptor's Access
			 * Permission field ([7:6]); absent or 0 leaves the descriptor exactly
			 * as built.  Motivation: the cb5 fault at FAR=our IOVA only ever
			 * shows up in the offset-mode (property_flag=4) boot - the one boot
			 * where the DSP really walks this table - and our leaf carries
			 * AP=00, privileged-only.  ats1pr cannot see that: it probes at EL1.
			 *
			 * This replaces the sweets1ap11 hunk of 0230, which ORed
			 * (3ull << 6) | 0x2.  bit1 is part of the TYPE field: at L2
			 * [1:0] = 0b01 means "2 MiB block" and 0b11 means "pointer to an L3
			 * table", so that boot measured a broken walk, not a permission
			 * result - post cb5 went FSR 0x408 -> 0x402 with FAR unchanged and
			 * FSYNR0 0x40002 -> 0x40003 (logs/ring-s1ap11.txt:2428 against
			 * logs/ring-audio-coldamp.txt:2371).  The identical trap is
			 * documented above for 0x703.  AF is bit10 and 0x701 already sets
			 * it, so only AP is missing - and it must be MASKED, not ORed.
			 *
			 * Placed after the sweets1attr switch on purpose: that switch
			 * overwrites s1_leaf wholesale, so an earlier edit would be lost
			 * as soon as the two knobs are combined.
			 */
			{
				int s1_ap = 0;

				(void)PE_parse_boot_argn("sweets1ap", &s1_ap, sizeof(s1_ap));
				if (s1_ap >= 1 && s1_ap <= 3) {
					s1_leaf &= ~(3ull << 6);
					s1_leaf |= ((uint64_t)s1_ap) << 6;
					printf("SWEET MARK: s1ap leaf=%#llx req=%d\n",
					    (unsigned long long)s1_leaf, s1_ap);
					sweet_log_sync();
				}
			}
			/* sweets1cbar=1 uses the exact value the vendor arm-smmu
			 * driver writes for a stage-1 bank:
			 * TYPE=1 (bits 17:16) | S1_MEMATTR=0xf (bits 15:12) |
			 * BPSHCFG=3 (bits 9:8) = 0x1f300.  We used to write a
			 * bare 0x10000, which leaves S1_MEMATTR=0. */
			if (s1_cbar) {
				cbar_val = 0x1f300u;
			}
			/* Banks 6..74 exist but CBAR reads 0 for them; they are
			 * only usable once CBAR says Type=1.  SmmuBypass.h in the
			 * UniXNU touch work shows that write succeeding at the
			 * last bank, so allow sweets1cb up to 74 and program
			 * CBAR ourselves for the high banks. */
			if (cbn < 0 || cbn > 74) {
				cbn = 5;
			}
			cbw = g_map_win(0x15080000u + 0x1000u * cbn,
			    0x1000u);
			g_s1_tbl_pa = s1_tbl ? (s1_tbl & ~0xfffull) :
			    (uint64_t)G_S2_L1_PA;
			tb1 = g_map_win(g_s1_tbl_pa, 0x3000u);
			if (cbw == 0 || tb1 == 0) {
				printf("SWEET SMMU: s1 windows failed\n");
				sweet_log_sync();
				return;
			}
			/* S1 page tables: L1[2] -> L2, L2 block identity.  S1
			 * block = PA | AttrIndx 0 | SH inner (3<<8) | AF |
			 * valid|block = PA | 0x703. */
			{
				uint64_t *l1 = (uint64_t *)tb1;
				uint64_t *l2 = (uint64_t *)(tb1 + 0x1000);
				uint64_t *l3t = (uint64_t *)(tb1 + 0x2000);
				uint64_t va;
				unsigned i1, i2, i3;
				int s1empty = 0;

				(void)PE_parse_boot_argn("sweets1empty", &s1empty,
				    sizeof(s1empty));
				/*
				 * sweetiova=1 replays the vendor's address
				 * exactly: the region is registered at the
				 * vendor IOVA 0x1_1FFE8000 (33-bit) and that
				 * IOVA translates to our tone page.  The old
				 * runs used a self-chosen 32-bit PA instead,
				 * so this configuration had never been tried.
				 * 33-bit input space needs T0SZ=31 and three
				 * levels (the IOVA's 2 MiB-block offset
				 * 0x1E8000 differs from our PA's, so a block
				 * descriptor cannot express it - use a 4 KiB
				 * page at L3).
				 */
				va = iova_mode ? (s1_va ? s1_va :
				    G_VENDOR_IOVA) : G_IPA_BASE;
				/* The DSP truncates the region IOVA to its
				 * own address width: the vendor's 0x1_1FFE8000
				 * arrives as 0x1FFE8000.  Pick the input width
				 * from the address we are actually given. */
				wide = (va >> 32) != 0;
				if (iova_mode) {
					/*
					 * One 2 MiB block is enough and avoids
					 * a third level entirely: it maps
					 * IOVA [0x1_1FE00000, 0x1_20000000)
					 * to PA [0xa2e00000, 0xa3000000), so
					 * the buffer must sit at the same
					 * in-block offset as the IOVA.
					 */
					g_map_addr = va;
					/* Output block base: default is the tone
					 * page's 2 MiB block; sweets1pa moves it,
					 * e.g. into the audio CMA range Android
					 * uses (0xfbc00000) to test whether the
					 * target side lets the DSP read there. */
					if (s1_pa != 0) {
						g_map_pa = (s1_pa & ~0x1fffffull) |
						    (va & 0x1fffffull);
					} else {
						g_map_pa =
						    (G_IPA_BASE & ~0x1fffffull) |
						    (va & 0x1fffffull);
					}
				}
				i1 = (unsigned)((va >> 30) & (wide ? 0x7u : 0x3u));
				i2 = (unsigned)((va >> 21) & 0x1ffu);
				i3 = (unsigned)((va >> 12) & 0x1ffu);
				/*
				 * Where the leaf descriptor points.  Default: the
				 * real buffer PA.  sweets1out moves ONLY this, so
				 * the tone stays in host RAM while the DSP is sent
				 * somewhere else - the safe form of the c2 test.
				 */
				s1_out_pa = s1_out ?
				    ((s1_out & ~0x1fffffull) | (va & 0x1fffffull)) :
				    g_map_pa;
				for (unsigned i = 0; i < 512; i++) {
					l1[i] = 0;
					l2[i] = 0;
					l3t[i] = 0;
				}
				/* sweets1empty=1 installs a deliberately empty
				 * table: every access must then be terminated
				 * at the SMMU (CFCFG=0) instead of reaching
				 * DDR.  If the machine still dies, the death is
				 * not on the apps-smmu path at all. */
				if (!s1empty) {
					l1[i1] = (uint64_t)G_S2_L2_PA | 0x3ull;
					/* AArch64 long descriptors: a level-2
					 * BLOCK is bits[1:0] = 0b01, while
					 * 0b11 means "level-3 page table".
					 * The old value 0x703 therefore made
					 * the SMMU treat 0xa2e00000 as an L3
					 * table (we never built one), so the
					 * walk always failed - which is why
					 * the identity map behaved exactly
					 * like the empty table (v46).
					 * 0x701 = valid|block | AF |
					 *         SH inner shareable |
					 *         AttrIndx 0 (MAIR0=0xff). */
					if (iova_mode) {
						/* 2 MiB block: valid|block
						 * (0b01) + AF + SH inner
						 * + AttrIndx 0 = | 0x701 */
						l2[i2] = (s1_out_pa &
						    ~0x1fffffull) | s1_leaf | (s1_ns ? 0x20ull : 0ull);
					} else {
						l2[i2] = ((uint64_t)G_IPA_BASE &
						    ~0x1fffffull) | s1_leaf | (s1_ns ? 0x20ull : 0ull);
					}
				}
				__asm__ volatile("dsb sy" ::: "memory");
				printf("SWEET SMMU: s1 tables%s va=%#llx "
				    "l1[%u]=%#llx l2[%u]=%#llx l3[%u]=%#llx buf_pa=%#llx\n",
				    s1empty ? " (EMPTY)" : "",
				    (unsigned long long)va, i1,
				    (unsigned long long)l1[i1], i2,
				    (unsigned long long)l2[i2], i3,
				    (unsigned long long)l3t[i3],
				    (unsigned long long)g_map_pa);
			}
			cbar = *(volatile uint32_t *)(v + 0x1000u + 4u * cbn);
			if (cbn >= 6) {
				/* SmmuBypass.h order: SCTLR 0, then CBAR. */
				*(volatile uint32_t *)(cbw + 0x00u) = 0;
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(v + 0x1000u + 4u * cbn) =
				    cbar_val;
				__asm__ volatile("dsb sy" ::: "memory");
				cbar = *(volatile uint32_t *)
				    (v + 0x1000u + 4u * cbn);
				printf("SWEET SMMU: cb%u cbar set -> %#x%s\n",
				    cbn, cbar,
				    cbar == cbar_val ? "" : " (READBACK BAD)");
				sweet_log_sync();
			}
			/* AArch64 long-descriptor tables need CBA2R.VA64=1
			 * whatever bank we use; for the bootloader's CB0..5
			 * we keep its CBAR (type 1 = S1 translate) but still
			 * have to force the table format.
			 *
			 * CBA2R(n) = GR1 + 0x800 + 4n, and ID1.PAGESIZE=0
			 * makes GR1 = base + 0x1000, so the absolute address
			 * is base + 0x1800 + 4n.  v79/v80 briefly moved this
			 * to base+0x8000 (ARM's *AArch64 SMMUv3* offset); on
			 * this v2 part that address is unmapped and writing
			 * it panics.  The vendor driver's base+0x8d0 for
			 * cbndx 0x34 is base+0x18d0 - the original place. */
			*(volatile uint32_t *)(v + 0x1800u + 4u * cbn) = 1u;
			__asm__ volatile("dsb sy" ::: "memory");
			printf("SWEET SMMU: cb%u cba2r@%#x = %#x (want 1)\n",
			    cbn, (unsigned)(v + 0x1800u + 4u * cbn),
			    *(volatile uint32_t *)(v + 0x1800u + 4u * cbn));
			sweet_log_sync();
			*(volatile uint32_t *)(cbw + 0x20u) =
			    (uint32_t)g_s1_tbl_pa;		/* TTBR0 */
			*(volatile uint32_t *)(cbw + 0x24u) = 0;
			if (s1_tcr2 != 0) {
				*(volatile uint32_t *)(cbw + 0x10u) =
				    (uint32_t)s1_tcr2;
			}
			if (s1_ttbr1) {
				*(volatile uint32_t *)(cbw + 0x28u) =
				    (uint32_t)g_s1_tbl_pa;
				*(volatile uint32_t *)(cbw + 0x2cu) = 0;
			}
			/* TCR, AArch64 long-descriptor format (CBA2R.VA64=1):
			 * T0SZ = 64 - input_size, so a 32-bit input space is
			 * T0SZ=0x20.  That matches the table layout below
			 * (L1 index VA[31:30] with 4 entries, L2 index
			 * VA[29:21] with 2 MiB blocks).  0x3560 =
			 * T0SZ 32 | TG0 4K | SH0 inner | ORGN0 WBWA |
			 * IRGN0 WBWA.  (In the AArch32 format the same width
			 * would be T0SZ=0 + EAE=1 instead - do not mix.) */
			/* T0SZ = 64 - input width: 0x20 for 32 bit, 0x1f for
			 * the vendor's 33-bit IOVA space. */
			*(volatile uint32_t *)(cbw + 0x30u) = s1_tcr ?
			    s1_tcr :
			    ((iova_mode && wide) ? 0x351fu : 0x3560u);	/* TCR */
			*(volatile uint32_t *)(cbw + 0x38u) = s1_mair;	/* MAIR0 */
			__asm__ volatile("dsb sy" ::: "memory");
			/* M | TRE | AFE | S1_ASIDPNE | CFRE | SHCFG=NSH;
			 * CFCFG=0 and CFIE=0 on purpose.
			 * The vendor arm-smmu driver's stage-1 value for an
			 * io-incoherent client is 0x00c010e7 (same, plus
			 * CFCFG=1 = stall); sweets1sctlr overrides. */
			*(volatile uint32_t *)(cbw + 0x00u) = s1_sctlr ?
			    s1_sctlr : (s1_off ? 0u : 0x00c01027u);
			__asm__ volatile("dsb sy" ::: "memory");
			re_sctlr = *(volatile uint32_t *)(cbw + 0x00u);
			re_ttbr = *(volatile uint32_t *)(cbw + 0x20u);
			re_tcr = *(volatile uint32_t *)(cbw + 0x30u);
			re_mair = *(volatile uint32_t *)(cbw + 0x38u);
			/*
			 * Exactly one entry, SID 0x1801, at the first free
			 * slot (slot 6).  Verified writable and readable as
			 * 0x80001801 in run97..101.
			 *
			 * ★ Do NOT try to cover 0x1800..0x180f.  Writing SID
			 * 0x1800 aborts the store (run70, run103), and run105
			 * showed that at least one of 0x1802..0x180f aborts
			 * too - each reserved SID costs a whole boot, and the
			 * reserved set is not documented anywhere.
			 */
			if (free_slot >= 0) {
				*(volatile uint32_t *)(v + 0x800 +
				    4 * free_slot) = 0x80001801u;
				__asm__ volatile("dsb sy" ::: "memory");
				*(volatile uint32_t *)(v + 0xc00 +
				    4 * free_slot) = (uint32_t)cbn & 0xffu;
				__asm__ volatile("dsb sy" ::: "memory");
			}
			*(volatile uint32_t *)(v + 0x68) = 0xffffffffu;
			*(volatile uint32_t *)(v + 0x70) = 0;
			for (int i = 0; i < 1000 &&
			    (*(volatile uint32_t *)(v + 0x74) & 1u); i++) {
				__asm__ volatile("yield");
			}
			/*
			 * ATS1PR: ask the SMMU itself to translate an IOVA
			 * through this context bank.  This is a page-table
			 * walk initiated by the SMMU, not a DSP DMA, so it
			 * proves whether the AP-side S1 mapping is correct
			 * without risking the interconnect.  The result
			 * lands in PAR (SMMU-500: PA in [31:12], bit 0 set
			 * means a fault was reported).
			 */
			{
				uint64_t par_ok, par_bad;

				/* Vendor arm-smmu.c iova_to_phys_hard():
				 * AArch64 format writes ATS1PR with a 64-bit
				 * access, waits for ATSR.ACTIVE to clear, and
				 * reads the 64-bit PAR at 0x50 (fault = BIT0,
				 * PA = PAR[39:12] | iova[11:0]). */
				uint32_t va32 = (uint32_t)(g_map_addr ?
				    g_map_addr : G_IPA_BASE);
				par_ok = g_ats1pr(cbw, va32);
				par_bad = g_ats1pr(cbw, va32 + 0x200000u);
				/* PAR bit0 set = fault.  4 KiB-aligned PA
				 * in [47:12].  The old probe used the tone
				 * PA and 0x10000000 as *IOVAs* - neither is
				 * in the table, so its par=0 was correct and
				 * told us nothing. */
				printf("SWEET SMMU: ats1pr iova=%#x -> par=%#llx%s "
				    "| +2M -> par=%#llx%s  mair=%#x cba2r=%#x "
				    "idr2=%#x tcr2=%#x\n", va32,
				    (unsigned long long)par_ok,
				    (par_ok & 1ull) ? " FAULT" : " OK",
				    (unsigned long long)par_bad,
				    (par_bad & 1ull) ? " FAULT" : " OK",
				    *(volatile uint32_t *)(cbw + 0x38u),
				    *(volatile uint32_t *)(v + 0x1800u +
				    4u * cbn),
				    *(volatile uint32_t *)(v + 0x28u),
				    *(volatile uint32_t *)(cbw + 0x10u));
				sweet_log_sync();
			}
			sc = *(volatile uint32_t *)(v + 0xc00 + 4 * free_slot);
			/* Remember the bank so the data-plane probe can read its
			 * FSR/FAR after DATA_BUFFER.  That read has never
			 * succeeded (the machine dies first), so "translation
			 * succeeded" has been an inference, not a measurement. */
			g_s1_cbn = cbn;
			g_s1_win = cbw;
			printf("SWEET SMMU: s1 cb%u cbar=%#x sctlr=%#x ttbr0=%#x "
			    "tcr=%#x mair=%#x leaf=%#llx attr=%d off=%d smr=%#x s2cr=%#x\n",
			    cbn, cbar, re_sctlr, re_ttbr, re_tcr, re_mair,
			    (unsigned long long)s1_leaf, s1_attr, s1_off,
			    *(volatile uint32_t *)(v + 0x800 + 4 * free_slot),
			    sc);
			sweet_log_sync();
			return;
		}
	}

	/* Targeted read-only map.  A blind sweep is not safe: 0x15004000
	 * aborts on read (run86), so the block has holes.  Only the pages we
	 * have already read successfully are touched:
	 *   0x15000000 GR0/GR1 (+0x1000..0x3FFF readable)
	 *   0x15080000 CB0..15 as the vendor math predicts
	 * CBAR tells us the bank's type and VMID; SCTLR/VTCR/VTTBR tell us
	 * whether the bootloader left it live. */
	{
		vm_offset_t gr = g_map_win(0x15000000u, 0x2000u);
		vm_offset_t cb = g_map_win(0x15080000u, 0x10000u);

		g_gr_win = gr;
		g_cb_win = cb;

		if (gr == 0 || cb == 0) {
			printf("SWEET SMMU: cb map failed gr=%d cb=%d\n",
			    gr != 0, cb != 0);
		} else {
			for (unsigned n = 0; n < 8; n++) {
				uint32_t cbar = *(volatile uint32_t *)
				    (gr + 0x1000u + 4u * n);
				uint32_t sc = *(volatile uint32_t *)
				    (cb + n * 0x1000u);
				uint32_t vt = *(volatile uint32_t *)
				    (cb + n * 0x1000u + 0x30u);
				uint32_t tb = *(volatile uint32_t *)
				    (cb + n * 0x1000u + 0x20u);

				printf("SWEET SMMU: cbm%u cbar=%#x(type=%u vmid=%u) "
				    "sctlr=%#x vtcr=%#x ttbr0=%#x\n", n, cbar,
				    (cbar >> 16) & 3u, cbar & 0xffu, sc, vt, tb);
			}
			printf("SWEET SMMU: cb map done\n");
			sweet_log_sync();
		}
	}
	/*
	 * sweetcb=N: route the audio SID at an existing context bank instead
	 * of programming our own.  run87 read CBAR = 0x1f000 for banks 0..5,
	 * i.e. Type=1 (stage-1 translation with stage-2 BYPASS); a stream
	 * sent to such a bank passes physical addresses straight through, so
	 * the DSP can read the SHM page with no page table at all.  Only
	 * SMR[0x4d] and S2CR[0x4d] are written in this mode.
	 */
	{
		int cbarg = -1;

		(void)PE_parse_boot_argn("sweetcb", &cbarg, sizeof(cbarg));
		if (cbarg >= 0 && cbarg < 128 && free_slot >= 0) {
			*(volatile uint32_t *)(v + 0x800 + 4 * free_slot) =
			    0x80000000u | 0x1801u;
			__asm__ volatile("dsb sy" ::: "memory");
			*(volatile uint32_t *)(v + 0xc00 + 4 * free_slot) =
			    (uint32_t)cbarg & 0xffu;
			__asm__ volatile("dsb sy" ::: "memory");
			*(volatile uint32_t *)(v + 0x68) = 0xffffffffu;
			*(volatile uint32_t *)(v + 0x70) = 0;
			for (int i = 0; i < 1000 &&
			    (*(volatile uint32_t *)(v + 0x74) & 1u); i++) {
				__asm__ volatile("yield");
			}
			printf("SWEET SMMU: audio -> cb%u cbar=%#x smr=%#x "
			    "s2cr=%#x\n", cbarg,
			    *(volatile uint32_t *)(v + 0x1000u + 4u * cbarg),
			    *(volatile uint32_t *)(v + 0x800 + 4 * free_slot),
			    *(volatile uint32_t *)(v + 0xc00 + 4 * free_slot));
			sweet_log_sync();
			return;
		}
	}
	printf("SWEET SMMU: stage-2 programming skipped (layout unknown)\n");
	sweet_log_sync();
	return;
	/* The ADSP audio SID (msm-audio-ion: iommus = <&apps_smmu 0x1801>)
	 * has no SMR in a fastboot-booted XNU.  Give it a stage-2 identity
	 * context so the DSP can reach the PCM page.  If the bootloader
	 * already gave it an SMR, leave that translation alone (the firmware
	 * may need it) and report it instead. */
	if (sid18 >= 0) {
		printf("SWEET SMMU: sid 0x1801 already at smr%d\n", sid18);
	} else if (free_slot >= 0) {
		int pcm = 0;
		int slot = (hi_free >= 0) ? hi_free : free_slot;

		(void)PE_parse_boot_argn("sweetpcm", &pcm, sizeof(pcm));
		if (!pcm) {
			printf("SWEET SMMU: S2 writes skipped (sweetpcm=1)\n");
			sweet_log_sync();
			return;
		}
		/* Bypass is refused from NS (run73) and the bootloader's ADSP
		 * context does not cover our PA (run75).  Build a stage-2
		 * identity block for the PCM page and point SID 0x1801 at CB6.
		 *
		 * Layout and values taken from the vendor tree, branch
		 * marble-s-oss (same kernel as this phone):
		 *   drivers/iommu/arm/arm-smmu/arm-smmu.c: ID1.NUMPAGENDXB=6
		 *     -> numpage = 1 << 7 = 128 pages of 4 KiB, so
		 *        ARM_SMMU_CB(n) = numpage + n => CB0 at +0x80000.
		 *   CBAR stores abort from NS for every bank tried (run77
		 *     CB6, run79/80 CB52); an untouched CBAR reads 0, which
		 *     is CBAR_TYPE_S2_TRANS with VMID 0, so we leave it alone
		 *     and only program the CB window + S2CR.
		 *   arm_smmu_write_context_bank(): CBA2R, CBAR, TCR, TTBR0,
		 *     SCTLR; S2 domains use fmt AARCH64 => CBA2R.VA64=1,
		 *     8-bit VMID lives in CBAR[7:0].
		 *   io-pgtable-arm.c arm_64_lpae_alloc_pgtable_s2(): for
		 *     IAS=32, OAS=32, 4K granule, coherent walk:
		 *     PS=0, TG0=0, SH=IS(3), RGN=WBWA(1), SL0=1, T0SZ=32
		 *     => VTCR = 0x80003560.
		 *   2 MiB block: valid|AF|HAP_RW|MEMATTR_OIWB = +0x4fd.
		 * CFIE is deliberately left clear: XNU has no SMMU fault
		 * handler and we do not want an unhandled IRQ storm. */
		uint32_t cbar6, cba2r6, sctlr6, vtcr6, pre0, pre1;
		uint32_t sacr;
		vm_offset_t tb, cb;

		/* sACR.CACHE_LOCK (bit 26) locks the per-CB config registers
		 * in the non-secure copy.  The vendor clears it in
		 * qsmmuv500_cfg_probe() before programming any CB; without
		 * this, a CBAR store aborts (run77/run79).  Only continue if
		 * the clear actually stuck. */
		sacr = *(volatile uint32_t *)(v + 0x10u);
		*(volatile uint32_t *)(v + 0x10u) = sacr & ~(1u << 26);
		__asm__ volatile("dsb sy" ::: "memory");
		printf("SWEET SMMU: sACR %#x -> %#x\n", sacr,
		    *(volatile uint32_t *)(v + 0x10u));
		sweet_log_sync();
		if (*(volatile uint32_t *)(v + 0x10u) & (1u << 26)) {
			printf("SWEET SMMU: CACHE_LOCK denied, CBAR untouched\n");
			sweet_log_sync();
			return;
		}

		cb = g_win_alloc(0x1000u);
		if (cb != 0) {
			cb = g_win_map(cb, G_SMMU_CB_PA(G_SMMU_CB), 0x1000u);
		}
		if (cb == 0) {
			printf("SWEET SMMU: CB%u page map failed\n", G_SMMU_CB);
			sweet_log_sync();
			return;
		}
		cbar6  = *(volatile uint32_t *)(v + G_SMMU_CBAR(G_SMMU_CB));
		cba2r6 = *(volatile uint32_t *)(v + G_SMMU_CBA2R(G_SMMU_CB));
		sctlr6 = *(volatile uint32_t *)(cb + 0x00u);
		vtcr6  = *(volatile uint32_t *)(cb + 0x30u);
		pre0   = *(volatile uint32_t *)(cb + 0x20u);
		pre1   = *(volatile uint32_t *)(cb + 0x24u);
		printf("SWEET SMMU: CB%u pre cbar=%#x cba2r=%#x sctlr=%#x "
		    "vtcr=%#x ttbr0=%#x%08x\n", G_SMMU_CB, cbar6, cba2r6,
		    sctlr6, vtcr6, pre1, pre0);
		sweet_log_sync();

		tb = g_map_win(G_S2_L1_PA, 0x2000);
		if (tb) {
			uint64_t *l1 = (uint64_t *)tb;
			uint64_t *l2 = (uint64_t *)(tb + 0x1000);
			unsigned i1 = (G_IPA_BASE >> 30) & 0x3u;
			unsigned i2 = (G_IPA_BASE >> 21) & 0x1ffu;

			for (unsigned i = 0; i < 512; i++) {
				l1[i] = 0;
				l2[i] = 0;
			}
			l1[i1] = (uint64_t)G_S2_L2_PA | 0x3ull;
			l2[i2] = ((uint64_t)G_IPA_BASE & ~0x1fffffull) |
			    0x4fdull;
			__asm__ volatile("dsb sy" ::: "memory");
			printf("SWEET SMMU: S2 l1[%u]=%#llx l2[%u]=%#llx\n",
			    i1, (unsigned long long)l1[i1], i2,
			    (unsigned long long)l2[i2]);
		} else {
			printf("SWEET SMMU: S2 tables map failed\n");
		}
		/* Vendor order minus CBAR: CBA2R, TCR, TTBR0, SCTLR. */
		*(volatile uint32_t *)(v + G_SMMU_CBA2R(G_SMMU_CB)) = 0x1u;
		/*
		 * Do NOT write CBAR.  A CBAR store aborts from NS even with
		 * sACR.CACHE_LOCK clear (run80: sACR was already 0x1c), while
		 * CBA2R stores succeed.  We do not need it: CBAR reads back 0
		 * for an untouched bank, and CBAR_TYPE_S2_TRANS is 0, so the
		 * bank is already a stage-2 context with VMID 0.  Bail out if
		 * that ever stops being true.
		 */
		if ((*(volatile uint32_t *)(v + G_SMMU_CBAR(G_SMMU_CB)) &
		    0x30000u) != 0) {
			printf("SWEET SMMU: CB%u CBAR=%#x not S2, skip\n",
			    G_SMMU_CB,
			    *(volatile uint32_t *)(v + G_SMMU_CBAR(G_SMMU_CB)));
			sweet_log_sync();
			return;
		}
		*(volatile uint32_t *)(cb + 0x30u) = 0x80003560u; /* VTCR */
		*(volatile uint32_t *)(cb + 0x20u) = (uint32_t)G_S2_L1_PA;
		*(volatile uint32_t *)(cb + 0x24u) = 0;
		*(volatile uint32_t *)(cb + 0x00u) = 0x0a5f0027u; /* SCTLR */
		__asm__ volatile("dsb sy" ::: "memory");
		/* Route the audio stream to CB52.  SMR first so no stream
		 * matches until the context is fully programmed.
		 *
		 * MASK must stay 0: an SMR with any MASK bit set aborts the
		 * store on this SMMU (run69 wrote 0x800f1800, run82 wrote
		 * 0x800f1801; both faulted at the store itself).  So the
		 * qcom,smmu-sid-mask=0xf from the audio-ion DT cannot be
		 * expressed here; the stream must present exactly 0x1801. */
		*(volatile uint32_t *)(v + 0x800 + 4 * slot) =
		    0x80000000u | 0x1801u;
		__asm__ volatile("dsb sy" ::: "memory");
		*(volatile uint32_t *)(v + 0xc00 + 4 * slot) =
		    G_SMMU_CB & 0xffu;
		__asm__ volatile("dsb sy" ::: "memory");
		*(volatile uint32_t *)(v + 0x68) = 0xffffffffu;	/* TLBIALLNSNH */
		*(volatile uint32_t *)(v + 0x70) = 0;		/* TLBGSYNC */
		for (int i = 0; i < 1000 &&
		    (*(volatile uint32_t *)(v + 0x74) & 1u); i++) {
			__asm__ volatile("yield");
		}
		printf("SWEET SMMU: S2 CB%u smr=%#x s2cr=%#x cbar=%#x cba2r=%#x "
		    "sctlr=%#x ttbr0=%#x%08x vtcr=%#x\n", G_SMMU_CB,
		    *(volatile uint32_t *)(v + 0x800 + 4 * slot),
		    *(volatile uint32_t *)(v + 0xc00 + 4 * slot),
		    *(volatile uint32_t *)(v + G_SMMU_CBAR(G_SMMU_CB)),
		    *(volatile uint32_t *)(v + G_SMMU_CBA2R(G_SMMU_CB)),
		    *(volatile uint32_t *)(cb + 0x00u),
		    *(volatile uint32_t *)(cb + 0x24u),
		    *(volatile uint32_t *)(cb + 0x20u),
		    *(volatile uint32_t *)(cb + 0x30u));
	}
	/* USFCFG=1 aborts every unmatched stream; turning it off is a
	 * belt-and-braces measure for streams other than 0x1801. */
	if (cr0 & 0x400u) {
		*(volatile uint32_t *)(v + 0x00) = cr0 & ~0x400u;
		__asm__ volatile("dsb sy" ::: "memory");
		printf("SWEET SMMU: USFCFG cleared %#x -> %#x\n", cr0,
		    *(volatile uint32_t *)(v + 0x00));
	}
	sweet_log_sync();
}

/* Prove whether the page pmap_find_phys reports is a real DRAM address:
 * write a magic word through the kernel mapping, then map that physical
 * address back and read it.  A match means the SHM path can hand this PA to
 * the DSP; a mismatch means the SHIM virtualises physical addresses and the
 * whole SHM_MAP approach is dead. */
static void
g_phys_selftest(void)
{
	static bool done;
	vm_offset_t va = 0, mirror;
	uint64_t pa;
	uint32_t pn;
	int on = 0;

	if (done) {
		return;
	}
	done = true;
	/* Diagnostic only (sweetphys=1).  It maps a kernel-managed RAM page
	 * with pmap_map and deliberately leaks it.  That page used to double
	 * as a high-bank SHM candidate, but the SHM region is 0x5000 while
	 * the allocation was 0x4000: mapping it rounded up to 0x8000 and
	 * created a spurious kernel mapping of the *next* page.  When that
	 * page was later freed, pmap_assert_free panicked on exactly
	 * g_self_pa+0x4000 (seen at 0x91dea0000 and 0x92144c000). */
	(void)PE_parse_boot_argn("sweetphys", &on, sizeof(on));
	if (!on) {
		return;
	}
	/* Fixed 16 KiB: this only checks whether a kernel page can be mirrored.
	 * It used to follow the region size, so growing the region to 1 MiB also
	 * grew a pointless allocation + ml_io_map - and a 1 MiB ml_io_map is
	 * implicated in the pmap_assert_free panic seen in j2. */
	const vm_size_t probe_size = 0x4000;

	if (kmem_alloc(kernel_map, &va, probe_size, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET PHYS: alloc failed\n");
		return;
	}
	pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
	pa = ptoa_64(pn);
	printf("SWEET PHYS: va=%#llx pa=%#llx\n", (uint64_t)va, pa);
	*(volatile uint32_t *)va = 0x5a5a1234u;
	__asm__ volatile("dsb sy" ::: "memory");
	mirror = g_map_win((uint64_t)pa, probe_size);
	if (!mirror) {
		printf("SWEET PHYS: mirror map failed\n");
	} else {
		uint32_t v = *(volatile uint32_t *)mirror;

		printf("SWEET PHYS: mirror=%#x %s\n", v,
		    v == 0x5a5a1234u ? "MATCH" : "MISMATCH");
	}
	/* Never free this page: ml_io_map's kernel PTEs cannot be removed, so
	 * kmem_free here trips pmap_assert_free (run63).  Keep it as the
	 * high-bank SHM candidate instead. */
	g_self_pa = pa;
	sweet_log_sync();
}

static int
g_apm_shm_map_gpr(uint32_t src_port)
{
	/* XNU's phystokv_range() rejects any PA outside the ADT bank, so
	 * ml_io_map(0xa2e00000) panics.  The SHIM pre-fills the PCM tone
	 * there before XNU starts; for that candidate va stays 0 and this
	 * code never touches the buffer.  The self-test page is inside the
	 * ADT bank and is mapped normally; the vendor IOVA is a status-only
	 * probe so the DSP is never asked to read an untranslated address.
	 * flag 4 = offset mode (vendor GSL), 0 = PA mode. */
	/* Vendor MAP (kprobe trace, iid 0x2002): flag=4 with address
	 * 0x1_1FFE8000 = an IOVA, and DATA_BUFFER then carries bare offsets.
	 * flag 4 therefore means "region base is a DSP-side IOVA".  Upstream
	 * instead uses property_flag=0 with a physical address and absolute
	 * buffer addresses.  sweetshmflag picks which convention to try. */
	static uint32_t flags[] = { 4u, 0u };
	{
		int f = -1;

		(void)PE_parse_boot_argn("sweetshmflag", &f, sizeof(f));
		if (f == 0 || f == 2 || f == 4) {
			flags[0] = (uint32_t)f;
			flags[1] = (f == 4u) ? 0u : 4u;
			printf("SWEET GLINK: shm flag order %u,%u\n",
			    flags[0], flags[1]);
		}
	}
	uint64_t pa[3];
	vm_offset_t va[3];
	int playable[3];
	unsigned n = 0;

	if (g_shm_mapped) {
		return 0;
	}
	g_smmu_probe();
	g_phys_selftest();
	pa[n] = g_map_pa ? g_map_pa : 0xa2e00000u;		/* SHIM-prefilled tone, low DRAM */
	{
		/* sweetpa=0x...: put the buffer somewhere else.  The ADSP
		 * carve-out (adsp_region@9fd00000, 49 MiB from the live DT) is
		 * memory the DSP can reach with its own stream, so it is the
		 * test for "can the DSP read host memory at all". */
		uint64_t ovr = 0;

		(void)PE_parse_boot_argn("sweetpa", &ovr, sizeof(ovr));
		if (ovr != 0) {
			printf("SWEET GLINK: shm pa override %#llx\n",
			    ovr);
			pa[n] = ovr;
		}
	}
	/* ml_io_map() itself works for this PA (run64 mapped it and the APM
	 * accepted it); only flush_dcache64()->phystokv_range() panics on
	 * PAs outside the ADT bank, so this mapping is never flushed. */
	va[n] = g_map_win((uint64_t)pa[n], g_shm_region_size());
	playable[n++] = 1;
	/* Do NOT add g_self_pa here: it is a single 0x4000 page, while a SHM
	 * region is 0x5000.  g_map_win() rounds the span up to 0x8000, so the
	 * second page (g_self_pa+0x4000) got a kernel pmap mapping although
	 * it belongs to an unrelated allocation; freeing it later tripped
	 * pmap_assert_free.  The low-DRAM tone candidate above is the one
	 * the vendor uses and the one that works. */
	pa[n] = 0x11ffe8000ull;		/* vendor IOVA, status only */
	va[n] = 0;
	playable[n++] = 0;

	for (unsigned i = 0; i < n; i++) {
		for (unsigned f = 0; f < 2; f++) {
			printf("SWEET GLINK: shm_map pool=3 pa=%#llx flag=%u "
			    "play=%d\n", pa[i], flags[f], playable[i]);
			sweet_log_sync();
			if (g_apm_shm_map_flat(src_port, pa[i], flags[f],
			    "shm_map") != 0) {
				continue;
			}
			g_shm_handle = g_le32(g_rsp + 24);
			printf("SWEET GLINK: shm handle=%#x len=%u\n",
			    g_shm_handle, g_rsp_len);
			sweet_log_sync();
			if (g_shm_handle == 0 || g_shm_handle == 0xffffffffu) {
				continue;
			}
			if (!playable[i]) {
				printf("SWEET GLINK: shm iova accepted "
				    "(apps-smmu required)\n");
				sweet_log_sync();
				continue;
			}
			g_shm_va = va[i];
			g_shm_pa = pa[i];
			g_shm_offset_mode = (flags[f] & 4u) != 0;
			g_shm_mapped = 1;
			printf("SWEET GLINK: shm mapped pa=%#llx mode=%s "
			    "va=%#llx\n", g_shm_pa,
			    g_shm_offset_mode ? "offset" : "pa",
			    (uint64_t)g_shm_va);
			sweet_log_sync();
			return 0;
		}
	}
	return -1;
}

static int
g_apm_shm_map(uint32_t src_port)
{
	return g_apm_shm_map_gpr(src_port);

	uint8_t pkt[24 + 20];
	uint32_t pkt_size = sizeof(pkt);
	ppnum_t pn;

	if (g_shm_mapped) {
		return 0;
	}
	if (!g_shm_va) {
		g_shm_va = g_map_win(G_SHM_FIXED_PA, g_shm_region_size());
		if (g_shm_va) {
			g_shm_pa = G_SHM_FIXED_PA;
			printf("SWEET GLINK: shm: fixed pa=%#llx size=%#x\n",
			    g_shm_pa, g_shm_region_size());
			sweet_log_sync();
		}
	}
	if (!g_shm_va) {
		/* Fallback: the old allocator path.  It only ever returned high
		 * memory, which the DSP cannot reach, so this should not run. */
		static const ppnum_t lim[] = {
			0x40000u, 0x80000u, 0x100000u, 0x180000u,
			0x200000u, 0x300000u, 0u
		};

		for (unsigned i = 0; i < sizeof(lim) / sizeof(lim[0]); i++) {
			vm_offset_t va = 0;

			if (kmem_alloc_contig(kernel_map, &va, g_shm_region_size(),
			    PAGE_MASK, lim[i], 0,
			    KMA_DATA | KMA_ZERO | KMA_PERMANENT,
			    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				printf("SWEET GLINK: shm: limit=%#x failed\n", lim[i]);
				sweet_log_sync();
				continue;
			}
			pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
			if (!pn) {
				kmem_free(kernel_map, va, g_shm_region_size());
				continue;
			}
			g_shm_va = va;
			g_shm_pa = ptoa_64(pn);
			printf("SWEET GLINK: shm: limit=%#x pa=%#llx\n",
			    lim[i], g_shm_pa);
			sweet_log_sync();
			break;
		}
		if (!g_shm_va) {
			printf("SWEET GLINK: shm: no contiguous memory\n");
			return -1;
		}
	}
	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	/* The vendor sets src_port_valid (bit 16) on SHM_MAP only: the APM
	 * needs the client port to bind the region's handle to it. */
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8) |
	    0x10000u);
	g_put32(pkt + 8, src_port);
	g_put32(pkt + 12, 1);
	/* Linux' q6apm tags SHM_MAP with the graph id: use the same token as
	 * GRAPH_OPEN here instead of the old arbitrary 0x2000. */
	g_put32(pkt + 16, 0x100u + APM_CMD_GRAPH_OPEN);
	g_put32(pkt + 20, APM_CMD_SHARED_MEM_MAP_REGIONS);
	g_put32(pkt + 24, APM_SHM_POOL | (1u << 16));
	g_put32(pkt + 28, 4u);          /* property_flag, as the vendor sends */
	g_put32(pkt + 32, (uint32_t)g_shm_pa);
	g_put32(pkt + 36, (uint32_t)(g_shm_pa >> 32));
	g_put32(pkt + 40, g_shm_region_size());
	printf("SWEET GLINK: shm_map src=%#x pa=%#llx size=%#x "
	    "valid=1\n", src_port, g_shm_pa, g_shm_region_size());
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 5000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (!g_rsp_ready || g_rsp_len < 28) {
		printf("SWEET GLINK: shm_map: no response (%u)\n", g_rsp_len);
		return -1;
	}
	/* The response payload is {echoed opcode, mem_map_handle}: the
	 * handle is the word after the opcode, not the opcode itself. */
	g_shm_handle = g_le32(g_rsp + 28);
	printf("SWEET GLINK: shm_map rsp w24=%#x handle=%#x w32=%#x\n",
	    g_le32(g_rsp + 24), g_shm_handle, g_le32(g_rsp + 32));
	if (g_shm_handle == 0 || g_shm_handle == 0xffffffffu ||
	    g_le32(g_rsp + 24) != APM_CMD_SHARED_MEM_MAP_REGIONS ||
	    g_le32(g_rsp + 28) != 0) {
		printf("SWEET GLINK: shm_map: failed (status=%#x)\n",
		    g_le32(g_rsp + 28));
		return -1;
	}
	g_shm_mapped = 1;
	return 0;
}

/* One-shot SHM_MAP probe for an arbitrary physical address.  Used to find
 * out which addresses the ADSP is willing to map. */
static int
g_apm_map_probe(uint32_t src_port, uint32_t pa_lsw, uint32_t pa_msw,
    uint32_t size, uint32_t flag, uint32_t token, const char *what)
{
	uint8_t pkt[24 + 20];
	uint32_t pkt_size = sizeof(pkt);

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8) | 0x10000u);
	g_put32(pkt + 8, src_port);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, token);
	g_put32(pkt + 20, APM_CMD_SHARED_MEM_MAP_REGIONS);
	g_put32(pkt + 24, APM_SHM_POOL | (1u << 16));
	g_put32(pkt + 28, flag);
	g_put32(pkt + 32, pa_lsw);
	g_put32(pkt + 36, pa_msw);
	g_put32(pkt + 40, size);
	printf("SWEET GLINK: map_probe %s src=%#x flag=%u pa=%#x:%#x "
	    "size=%#x\n", what, src_port, flag, pa_msw, pa_lsw, size);
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 4000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (!g_rsp_ready || g_rsp_len < 32) {
		printf("SWEET GLINK: map_probe %s: no rsp\n", what);
		sweet_log_sync();
		return -1;
	}
	printf("SWEET GLINK: map_probe %s rsp len=%u w24=%#x w28=%#x "
	    "w32=%#x\n", what, g_rsp_len, g_le32(g_rsp + 24),
	    g_le32(g_rsp + 28), g_le32(g_rsp + 32));
	sweet_log_sync();
	return g_le32(g_rsp + 28) == 0 ? 0 : -1;
}

/* GRAPH_OPEN with the payload in the mapped region, as the vendor does. */
static int
g_apm_graph_open_shm(uint32_t src_port, const uint8_t *payload, uint32_t len,
    uint32_t off, const char *what)
{
	uint8_t pkt[24 + 16];
	uint32_t pkt_size = sizeof(pkt);

	if (!g_shm_mapped || !g_shm_va || off + len > g_shm_region_size()) {
		return -1;
	}
	memcpy((void *)(g_shm_va + off), payload, len);
	/* Never flush_dcache64() a low physical address: phystokv_range()
	 * only knows the ADT banks (0x840000000+) and panics otherwise.
	 * The window is Device-mapped, so the stores are already in DRAM. */
	__asm__ volatile("dsb sy" ::: "memory");

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, src_port);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, 0x400u);
	g_put32(pkt + 20, APM_CMD_GRAPH_OPEN);
	g_put32(pkt + 24 + 0, off);   /* payload offset inside the region */
	g_put32(pkt + 24 + 4, 0);
	g_put32(pkt + 24 + 8, g_shm_handle);
	g_put32(pkt + 24 + 12, len);
	printf("SWEET GLINK: APM %s src=%#x (shm handle=%#x len=%u)\n",
	    what, src_port, g_shm_handle, len);
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 5000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
		/* Beat once a second.  Three vendor4 boots left the ring at the graph_open
		 * line with neither a response nor the timeout print, and those two readings
		 * cannot be told apart without something that keeps writing while we wait. */
		if ((i % 200) == 195) {
			printf("SWEET GLINK: poll beat %u rsp=%d\n", i, g_rsp_ready);
			sweet_log_sync();
		}
	}
	if (!g_rsp_ready) {
		printf("SWEET GLINK: %s: no response\n", what);
		return -1;
	}
	if (g_rsp_len >= 32) {
		uint32_t status = g_le32(g_rsp + 28);

		printf("SWEET GLINK: %s rsp op=%#x status=%#x\n", what,
		    g_le32(g_rsp + 24), status);
		sweet_log_sync();
		return status == 0 ? 0 : -1;
	}
	printf("SWEET GLINK: %s rsp %u bytes\n", what, g_rsp_len);
	return -1;
}

/* One SET_CFG parameter block: {iid, param_id, size, 0} + data, padded to 8.
 * The vendor sends these to the module instance port itself (dst=0x4308 etc.),
 * which is what the APM expects for module-targeted configuration. */
static int
g_apm_set_cfg(uint32_t src_port, uint32_t mod_iid, uint32_t pid,
    const void *data, uint32_t dlen, const char *what)
{
	uint8_t pl[16 + 64];
	uint32_t plen;

	if (dlen > 64) {
		return -1;
	}
	memset(pl, 0, sizeof(pl));
	g_put32(pl + 0, mod_iid);
	g_put32(pl + 4, pid);
	g_put32(pl + 8, dlen);
	memcpy(pl + 16, data, dlen);
	plen = (16 + dlen + 7u) & ~7u;
	return g_apm_cmd(src_port, mod_iid, pl, plen, APM_CMD_SET_CFG, what);
}

/*
 * One SET_CFG carried in shared memory: the OOB form.
 *
 * Same 16-byte apm_module_param_data_t records as the in-band list, but the
 * packet carries an apm_cmd_header_t {offset, 0, handle, len} instead of the
 * body, and the body is read out of the region we already mapped.  That is how
 * the vendor's 18 KB configuration reaches the APM, and APM writes each record's
 * error_code back into the region, so one packet answers "which of these module
 * instance ids does this graph have".
 *
 * Every record here carries the same param id and the same payload, so the only
 * variable between records is the module instance id; the first three are
 * instances this graph definitely contains and act as the positive control.
 */
/*
 * G round: exactly one probe, and its advertised payload address is deliberately
 * INSIDE the region we declared (0x5000) - the one thing F could not control for.
 * F's single out-of-range send came back status 0x1 exactly like the five
 * in-range sends before it, so the address axis is closed and APM does not appear
 * to range-check.  What F also showed is that the refusal *count* was never why
 * the data plane died: E's dose reading is refuted.  What tracks instead is
 * whether the advertised address is serviceable - D sent one in-range probe and
 * the plane ran 31 buffers with four done replies, while E (two probes at
 * base+off, addresses the DSP cannot service) and F (one probe past the region)
 * both died at the first buffer.  This boot repeats F with a serviceable address
 * on the same code lineage: plane alive => the wedge is the bogus address, and
 * the dereference story gains its second leg; plane still dead => 0254/0255
 * themselves wedge it, and that inference is void.
 *
 * In-use layout: PCM at 0x0 and 0x800 (1152 B each, used to 0xc80), graph
 * payload at 0x2000 (used to 0x2b90), region size 0x5000.  0x1000 is free.
 */
static const uint32_t g_oob_offs[] = { 0x1000u };
#define SWEET_OOB_FORMS 1		/* 0 = bare offset, 1 = base + offset */
#define SWEET_OOB_RECS  4
#define SWEET_OOB_STRIDE 72		/* 16-byte wire header + align8(52) */

/*
 * K1 round: J answered 0x3 with the payload zeroed, so APM does not look at the
 * bytes.  Three fields are still bundled inside H: the per-record size, the
 * record count (and with it the stride), and the iid - H and J both carry
 * iid=1 while G and I carry real module instance ids.  K1 moves only the iid:
 * four records, iid = the first four modules MTP registers for subgraph
 * 0xb0000023 (0x40bd 0x40be 0x40bf 0x40c0), param 0x08001004, size 52, stride 72,
 * total 288, address 0x1000, dst=1, same handle, payload back to H's bytes.
 * Still 0x3 => iid does not participate and the boundary is size or count.
 * Any other code => the iid is the boundary, which is APM's fan-out key.
 */
static const uint32_t g_oob_miids[SWEET_OOB_RECS] = {
	0x40bdu, 0x40beu, 0x40bfu, 0x40c0u
};

/* H's four payload words, restored so the iid is the only field that moved. */
static const uint32_t g_h_payload[SWEET_OOB_RECS][13] = {
	/* CDP_ukee_acdb_cal.acdb @0x239e6 */
	{0x00000003u, 0x00004147u, 0x00000001u, 0x00004167u, 0x00000002u, 0x000043afu, 0x00000001u, 0x00004147u, 0x00000002u, 0x00004167u, 0x0000000bu, 0x00004021u, 0x00000002u},
	/* CDP_ukee_acdb_cal.acdb @0x23bde */
	{0x00000003u, 0x00004589u, 0x00000001u, 0x00004587u, 0x00000002u, 0x00004587u, 0x00000001u, 0x00004763u, 0x00000002u, 0x00004763u, 0x00000001u, 0x0000458au, 0x00000002u},
	/* CDP_ukee_acdb_cal.acdb @0x311ae */
	{0x00000003u, 0x0000434au, 0x00000001u, 0x0000434cu, 0x00000002u, 0x0000434cu, 0x00000001u, 0x0000434bu, 0x00000002u, 0x0000434bu, 0x00000001u, 0x0000434du, 0x00000002u},
	/* CDP_ukee_acdb_cal.acdb @0x31c7e */
	{0x00000003u, 0x000045adu, 0x00000001u, 0x000045afu, 0x00000002u, 0x000045afu, 0x00000001u, 0x000045aeu, 0x00000002u, 0x000045aeu, 0x00000001u, 0x0000459cu, 0x00000002u},
};

static int
g_apm_set_cfg_oob(uint32_t src_port, const char *what)
{
	uint8_t body[SWEET_OOB_RECS * SWEET_OOB_STRIDE];
	uint8_t pkt[24 + 16];
	uint32_t off = 0;
	uint32_t i;
	uint32_t j;
	uint32_t t;

	if (!g_shm_mapped || !g_shm_va) {
		printf("SWEET MARK: oobcfg v8 no mapping\n");
		return (-1);
	}
	memset(body, 0, sizeof(body));
	memset(pkt, 0, sizeof(pkt));
	g_smmu_faults("oobcfg-pre");
	for (i = 0; i < SWEET_OOB_RECS; i++) {
		g_put32(body + off + 0, g_oob_miids[i]);
		g_put32(body + off + 4, 0x08001004u);
		g_put32(body + off + 8, 52);
		g_put32(body + off + 12, 0);	/* error_code, written by the DSP */
		for (j = 0; j < 13; j++) {
			g_put32(body + off + 16 + j * 4, g_h_payload[i][j]);
		}
		off += SWEET_OOB_STRIDE;
	}
	for (t = 0; t < sizeof(g_oob_offs) / sizeof(g_oob_offs[0]); t++) {
		const uint32_t oob_off = g_oob_offs[t];
		const int in_bounds = (oob_off + off <= g_shm_region_size());
		uint32_t f;

		if (!in_bounds) {
			printf("SWEET MARK: oobcfg v8 off=%#x is past the declared region %#x, body not written\n",
			    oob_off, g_shm_region_size());
		} else {
			memcpy((void *)(g_shm_va + oob_off), body, off);
		}
		__asm__ volatile("dsb sy" ::: "memory");

		for (f = 0; f < SWEET_OOB_FORMS; f++) {
			uint64_t addr = (f == 0) ? (uint64_t)oob_off :
			    g_map_pa + (uint64_t)oob_off;

			if (f == 1 && g_map_pa == 0) {
				printf("SWEET MARK: oobcfg v8 try %u no region base, abs form skipped\n",
				    t * SWEET_OOB_FORMS + f);
				continue;
			}
			g_put32(pkt + 0, (6u << 4) | (sizeof(pkt) << 8));
			g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
			g_put32(pkt + 8, src_port);
			g_put32(pkt + 12, 1);
			g_put32(pkt + 16, 0x400u);
			g_put32(pkt + 20, APM_CMD_SET_CFG);
			g_put32(pkt + 24 + 0, (uint32_t)addr);
			g_put32(pkt + 24 + 4, (uint32_t)(addr >> 32));
			g_put32(pkt + 24 + 8, g_shm_handle);
			g_put32(pkt + 24 + 12, off);
			printf("SWEET MARK: oobcfg v8 try %u form=%s off=%#x addr=%#llx len=%u recs=%u handle=%#x\n",
			    t * SWEET_OOB_FORMS + f, f == 0 ? "bare" : "abs",
			    oob_off, addr, off, SWEET_OOB_RECS, g_shm_handle);
			sweet_log_sync();
			g_rsp_ready = 0;
			g_rsp_len = 0;
			g_send_gpr(pkt, sizeof(pkt));
			for (unsigned n = 0; n < 5000; n += 5) {
				g_drain(0);
				if (g_rsp_ready) {
					break;
				}
				g_wait(5);
			}
			if (!g_rsp_ready) {
				printf("SWEET GLINK: %s addr=%#llx: no response\n",
				    what, addr);
				g_smmu_faults("oobcfg-norsp");
				continue;
			}
			printf("SWEET GLINK: %s addr=%#llx rsp op=%#x status=%#x hop=%#x len=%u\n",
			    what, addr, g_le32(g_rsp + 24), g_le32(g_rsp + 28),
			    g_le32(g_rsp + 20), g_rsp_len);
			/* Kept because it is one MMIO read pass, but E measured that this
			 * witness cannot currently speak: all six banks hold FSR=0x400 that
			 * survives our W1C attempt and the post snapshots were byte-identical
			 * to pre.  So "no new fault bits" is not evidence of no fault. */
			g_smmu_faults("oobcfg-post");
			__asm__ volatile("dsb sy" ::: "memory");
			if (!in_bounds) {
				printf("SWEET MARK: oobcfg v8 recs not read back (past the mapping)\n");
				sweet_log_sync();
				continue;
			}
			for (i = 0; i < SWEET_OOB_RECS; i++) {
				const uint8_t *rec = (const uint8_t *)g_shm_va +
				    oob_off + i * SWEET_OOB_STRIDE;

				printf("SWEET MARK: oobcfg v8 rec %u iid=%#x param=%#x size=%u err=%#x\n",
				    i, g_le32(rec + 0), g_le32(rec + 4),
				    g_le32(rec + 8), g_le32(rec + 12));
			}
			sweet_log_sync();
		}
	}
	return (0);
}

/*
 * A SET_CFG carrying more than one parameter record.
 *
 * This is the container we have never used: every SET_CFG sent so far is a
 * single record aimed at that module's own port.  GSL's rule (verified in
 * libar-gsl.so against gsl_graph_set_custom_config) is that a multi-record
 * list goes to dst=1, the APM, which then fans it out, and that the list stays
 * in-band while it is at most 256 bytes - only longer lists get SHM_MAP'd.
 */
#define SWEET_CC_MAX 256

static int
g_apm_set_cfg_batch(uint32_t src_port, uint8_t *body, uint32_t blen,
    uint32_t nrec, const char *what)
{
	if (blen == 0 || blen > SWEET_CC_MAX) {
		printf("SWEET MARK: cccfg v1 reject recs=%u len=%u\n",
		    nrec, blen);
		return (-1);
	}
	printf("SWEET MARK: cccfg v1 dst=1 inband recs=%u len=%u\n",
	    nrec, blen);
	{
		/* Walk the list the way the APM does, so the ring itself carries
		 * the container shape instead of relying on a host-side calc. */
		uint32_t off = 0;
		uint32_t i = 0;

		while (off + 16 <= blen) {
			uint32_t psz = g_le32(body + off + 8);
			uint32_t step = 16 + ((psz + 7u) & ~7u);

			if (step <= 16) {
				printf("SWEET MARK: cccfg v1 bad size at %u\n",
				    off);
				return (-1);
			}
			printf("SWEET MARK: cccfg v1 rec %u iid=%#x param=%#x size=%u\n",
			    i, g_le32(body + off), g_le32(body + off + 4), psz);
			off += step;
			i++;
		}
		if (off != blen || i != nrec) {
			printf("SWEET MARK: cccfg v1 walk %u != len %u (recs %u/%u)\n",
			    off, blen, i, nrec);
			return (-1);
		}
	}
	return (g_apm_cmd(src_port, 1, body, blen, APM_CMD_SET_CFG, what));
}

/* ---- PCM feed: 1 kHz stereo tone ---------------------------------------
 * The vendor HAL puts PCM in the same mapped region and sends DATA_BUFFER_V2
 * (0x0400100A, no APM header, 44-byte payload) to the input module; buf_addr
 * is an offset inside the region and the DSP paces the AP with
 * DATA_BUFFER_DONE_V2 (0x05001004).  One buffer is 288 frames, i.e. 6 ms at
 * 48 kHz stereo 16-bit.
 */
#define APM_CMD_DATA_BUFFER_V2      0x0400100Au
#define APM_RSP_DATA_BUFFER_DONE_V2 0x05001004u
#define G_PCM_OFF_A                 0x0000u
#define G_PCM_OFF_B                 0x0800u
#define G_PCM_BYTES                 1152u

static const int16_t g_sine_48[48] = {
	0, 1069, 2120, 3135, 4096, 4987, 5793, 6499,
	7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
	7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069,
	0, -1069, -2120, -3135, -4096, -4987, -5793, -6499,
	-7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568,
	-7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069,
};

static int g_vol_printed = 0;

static void
g_fill_tone(uint32_t off)
{
	int16_t *p = (int16_t *)(g_shm_va + off);

	/* The WR endpoint is 48 kHz / 16-bit / stereo (vendor SET_CFG), so
	 * 1152 bytes is 288 frames (6 ms).  The 48-entry table is one
	 * period: a 1 kHz tone.  Amplitude is deliberately low - the tone
	 * only has to prove the pipe, not be loud. */
	int g = 1;

	(void)PE_parse_boot_argn("sweetvol", &g, sizeof(g));
	if (g < 1 || g > 31) {
		g = 1;		/* the shipped 1/8 of 1/8, i.e. peak 1024 */
	}
	if (!g_vol_printed) {
		g_vol_printed = 1;
		printf("SWEET MARK: tone gain g=%d peak=%d\n", g, 1024 * g);
		sweet_log_sync();
	}
	for (uint32_t i = 0; i < G_PCM_BYTES / 4; i++) {
		int16_t v = (int16_t)((g_sine_48[i % 48] * g) >> 3);

		p[2 * i] = v;
		p[2 * i + 1] = v;
	}
}

/*
 * SMMU fault snapshot.  Read BEFORE touching the GLINK FIFO after a
 * DATA_BUFFER, because a stalled DSP can hang that read and we would lose
 * the only diagnostic window.  GFSR.USF means no SMR matched and
 * GFSYNR1[14:0] holds the offending Stream ID (ARM SMMU-500).
 */
static void
g_smmu_faults(const char *tag)
{
	if (g_gr_win == 0) {
		printf("SWEET SMMU: %s no gr window\n", tag);
		sweet_log_sync();
		return;
	}
	printf("SWEET SMMU: %s GFSR=%#x FSYNR0=%#x FSYNR1=%#x FSYNR2=%#x\n",
	    tag, *(volatile uint32_t *)(g_gr_win + 0x48u),
	    *(volatile uint32_t *)(g_gr_win + 0x50u),
	    *(volatile uint32_t *)(g_gr_win + 0x54u),
	    *(volatile uint32_t *)(g_gr_win + 0x58u));
	if (g_cb_win != 0) {
		for (unsigned n = 0; n < 6; n++) {
			uint32_t fsr = *(volatile uint32_t *)
			    (g_cb_win + n * 0x1000u + 0x58u);
			uint32_t far = *(volatile uint32_t *)
			    (g_cb_win + n * 0x1000u + 0x60u);
			uint32_t fsyn = *(volatile uint32_t *)
			    (g_cb_win + n * 0x1000u + 0x68u);

			if (fsr != 0 || far != 0) {
				printf("SWEET SMMU: %s cb%u FSR=%#x FAR=%#x "
				    "FSYNR0=%#x farhi=%#x\n", tag, n, fsr, far, fsyn,
			    *(volatile uint32_t *)(g_cb_win + n * 0x1000u +
			    0x64u));
			}
		}
	}
	sweet_log_sync();
}

static int
g_apm_data_buffer(uint32_t src_port, uint32_t dst_port, uint32_t off,
    uint32_t len, uint32_t token)
{
	uint8_t pkt[24 + 44];
	uint32_t pkt_size = sizeof(pkt);

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, src_port);
	{
		/*
		 * sweetdbdst=<iid> retargets DATA_BUFFER without touching the
		 * graph.  Every memory path (translated, faulting, bypassed,
		 * even the ADSP's own carve-out) killed the machine identically,
		 * so the next question is whether the WR data path is what
		 * dies or whether ANY DATA_BUFFER frame is fatal.  Pointing the
		 * frame at a module that cannot be a sink (0x0bad) or at the
		 * TDM itself (0x4308) separates the two.
		 */
		static int dbdst_init = -1;

		if (dbdst_init < 0) {
			dbdst_init = 0;
			(void)PE_parse_boot_argn("sweetdbdst", &g_dbdst,
			    sizeof(g_dbdst));
		}
		g_put32(pkt + 12, g_dbdst ? g_dbdst : dst_port);
	}
	{
		/* The vendor's own wire trace uses a bare 0/1 here, so replay
		 * that by default; sweettok=1 switches to the upstream Linux
		 * composite form dsp_buf | (len << 16). */
		static int up = -1;
		uint32_t tok;

		if (up < 0) {
			up = 0;
			(void)PE_parse_boot_argn("sweettok", &up, sizeof(up));
		}
		tok = up ? ((token & 0xffffu) | (len << 16)) : (token & 0xffffu);
		g_put32(pkt + 16, tok);
	}
	g_put32(pkt + 20, APM_CMD_DATA_BUFFER_V2);
	{
		/* Offset mode: buf_addr is an offset inside the mapped region.
		 * PA mode: the DSP wants the physical address itself. */
		uint64_t a = g_shm_offset_mode ? off : (g_shm_pa + off);

		g_put32(pkt + 24 + 0, (uint32_t)a);
		g_put32(pkt + 24 + 4, (uint32_t)(a >> 32));
	}
	g_put32(pkt + 24 + 8, g_shm_handle);
	g_put32(pkt + 24 + 12, len);
	g_rsp_ready = 0;
	g_rsp_len = 0;
	{
		/* Upstream AudioReach calls the first two payload words
		 * buf_addr_lsw/msw, not "offset".  If the DSP expects an
		 * address, an offset of 0 or 0x800 points at nothing. */
		static int pamode = -1;

		if (pamode < 0) {
			pamode = 0;
			(void)PE_parse_boot_argn("sweetpamode", &pamode,
			    sizeof(pamode));
		}
		if (pamode && g_shm_offset_mode) {
			uint64_t a = g_shm_pa + off;

			g_put32(pkt + 24 + 0, (uint32_t)a);
			g_put32(pkt + 24 + 4, (uint32_t)(a >> 32));
			printf("SWEET GLINK: data_buffer addr=%#llx (pamode)\n",
			    a);
		}
	}
	if (dst_port == G_VP_WR_IID) {
		static int first = 1;

		if (first) {
			g_smmu_faults("pre ");
			/*
			 * All six bootloader banks already carry stale faults, so
			 * a post-send read would otherwise be indistinguishable
			 * from what was there before.  FSR is W1C: write back what
			 * we read and verify it actually cleared.
			 */
			if (g_s1_cbn >= 0 && g_s1_win != 0) {
				/* Use the bank's OWN mapping.  g_cb_win is only
				 * 0x10000 (banks 0..15); indexing it with a high
				 * bank number (c3 used 74) hit unmapped kernel VA
				 * and panicked. */
				volatile uint32_t *cbr =
				    (volatile uint32_t *)g_s1_win;
				uint32_t old_fsr = cbr[0x58u / 4u];

				cbr[0x58u / 4u] = old_fsr;
				__asm__ volatile("dsb sy" ::: "memory");
				printf("SWEET SMMU: s1 cb%d fsr clear %#x -> %#x\n",
				    g_s1_cbn, old_fsr, cbr[0x58u / 4u]);
				sweet_log_sync();
			}
			/* GFSR is W1C: clear any pre-existing global fault so
			 * a later reading must come from this transaction. */
			*(volatile uint32_t *)(g_gr_win + 0x48u) = 0xffffffffu;
			__asm__ volatile("dsb sy" ::: "memory");
			printf("SWEET SMMU: gfsr cleared -> %#x\n",
			    *(volatile uint32_t *)(g_gr_win + 0x48u));
			sweet_log_sync();
		}
		printf("SWEET GLINK: data_buffer send\n");
		sweet_log_sync();
		g_send_gpr(pkt, pkt_size);
		/*
		 * The single most valuable reading in this project: the S1
		 * context bank's own FSR/FAR for the DSP's first buffer access.
		 * FSR==0 means the access really did translate and the DSP got
		 * data (so the hang is downstream of the SMMU); a fault code
		 * means the access never reached DDR and every "translation
		 * succeeded" conclusion so far was inference.
		 * Print BEFORE the reads so a hang is not mistaken for "we
		 * never got there".
		 */
		if (first && g_s1_cbn >= 0 && g_s1_win != 0) {
			volatile uint32_t *cbr = (volatile uint32_t *)g_s1_win;
			uint32_t p_fsr, p_far, p_farhi, p_fsyn;

			printf("SWEET SMMU: post-send probe cb%d reading...\n",
			    g_s1_cbn);
			sweet_log_sync();
			p_fsr = cbr[0x58u / 4u];
			p_far = cbr[0x60u / 4u];
			p_farhi = cbr[0x64u / 4u];
			p_fsyn = cbr[0x68u / 4u];
			printf("SWEET SMMU: post-send cb%d FSR=%#x FAR=%#x:%#x "
			    "FSYNR0=%#x\n", g_s1_cbn, p_fsr, p_farhi, p_far,
			    p_fsyn);
			sweet_log_sync();
			/*
			 * sweets1fsrpoll=1 keeps reading FSR for ~200 ms.
			 * Calibrate against sweets1empty=1 first: that run must
			 * show TF (bit1) for the DSP's access, which proves the
			 * poll captures real faults AND tells us how long the
			 * DSP takes to touch the buffer.  Polling costs SMMU
			 * MMIO reads, which is exactly what kills some runs.
			 */
			{
				int fsrpoll = 0;

				(void)PE_parse_boot_argn("sweets1fsrpoll",
				    &fsrpoll, sizeof(fsrpoll));
				if (fsrpoll) {
				uint32_t base = p_fsr;

				for (unsigned i = 0; i < 40; i++) {
					uint32_t v = cbr[0x58u / 4u];

					if (v != base) {
						printf("SWEET SMMU: fsrpoll %u"
						    " FSR=%#x FAR=%#x:%#x\n", i, v,
						    cbr[0x64u / 4u], cbr[0x60u / 4u]);
						sweet_log_sync();
						base = v;
					}
					delay(5);
				}
				printf("SWEET SMMU: fsrpoll done, FSR=%#x\n",
				    cbr[0x58u / 4u]);
				sweet_log_sync();
				}
			}
		}
		if (first) {
			uint32_t gf_now = 0, gf_syn = 0;
			int reached = -1;

			first = 0;
			/*
			 * Read GFSR IMMEDIATELY, before the disk sync.  The
			 * previous round proved that the sync after the send
			 * completes (the "polling GFSR" line reaches eMMC),
			 * so CPU, interrupts and the eMMC path are all alive
			 * at that point; the death happens on the first SMMU
			 * register read that follows.  Reading first gives us
			 * one chance to sample the fault before the TCU stops
			 * answering.  `reached` tells us how far we got even
			 * if the machine dies later.
			 */
			gf_now = *(volatile uint32_t *)(g_gr_win + 0x48u);
			gf_syn = *(volatile uint32_t *)(g_gr_win + 0x54u);
			reached = 0;
			printf("SWEET GLINK: data_buffer sent; gfsr=%#x fsynr1=%#x\n",
			    gf_now, gf_syn);
			reached = 1;
			sweet_log_sync();
			reached = 2;
			/*
			 * sweetskipgfsr=1: do NOT touch the SMMU after the send.
			 *
			 * Every legal DATA_BUFFER run died on the next SMMU
			 * register read, which meant we never once waited for
			 * the DSP's own reply.  A read that never returns is
			 * also the only reason the whole ring stops there.  So
			 * poll the GLINK FIFO instead: if the DSP answers (even
			 * with an error) the machine is fine and the SMMU MMIO
			 * path is what dies; if nothing ever arrives while the
			 * CPU keeps printing, the DSP itself is wedged.
			 */
			/*
			 * sweetnolpass=1: after the send, touch nothing on the
			 * LPASS side at all - only RAM and the eMMC log flush.
			 * If these markers keep appearing, the AP is healthy and
			 * only the LPASS MMIO stopped answering.
			 */
			{
				static int nolpass = -1;

				if (nolpass < 0) {
					nolpass = 0;
					(void)PE_parse_boot_argn("sweetnolpass",
					    &nolpass, sizeof(nolpass));
					g_nolpass = nolpass;
				}
				if (nolpass) {
					for (unsigned i = 0; i < 20; i++) {
						printf("SWEET GLINK: nolpass alive"
						    " %u\n", i);
						sweet_log_sync();
						delay(100);
					}
				}
			}
			{
				static int skipgfsr = -1;

				if (skipgfsr < 0) {
					skipgfsr = 0;
					(void)PE_parse_boot_argn("sweetskipgfsr",
					    &skipgfsr, sizeof(skipgfsr));
					g_skipgfsr = skipgfsr;
				}
				if (skipgfsr) {
					g_rsp_ready = 0;
					g_rsp_len = 0;
					for (unsigned i = 0; i < 400; i++) {
						g_drain(0);
						if (g_rsp_ready) {
							break;
						}
						if ((i % 40) == 39) {
							printf("SWEET GLINK: alive %u "
							    "(no reply yet)\n", i + 1);
							sweet_log_sync();
						}
						g_wait(5);
					}
					printf("SWEET GLINK: post-send ready=%d "
					    "len=%u\n", g_rsp_ready, g_rsp_len);
					sweet_log_sync();
				}
			}
			/* Poll the SMMU (not the GLINK FIFO) for a global fault
			 * so an unmatched stream is recorded before the DSP can
			 * wedge the bus.  GFSYNR1[14:0] would then be the real
			 * Stream ID.  Skipped entirely under sweetskipgfsr=1:
			 * this read is what kills every legal-buffer run. */
			uint32_t gfsr = gf_now;

			for (unsigned i = 0; !g_skipgfsr && i < 2000 && gfsr == 0;
			    i++) {
				gfsr = *(volatile uint32_t *)(g_gr_win + 0x48u);
				__asm__ volatile("yield");
			}
			reached = 3;
			if (g_skipgfsr) {
				printf("SWEET SMMU: post skipped (sweetskipgfsr)"
				    "\n");
				sweet_log_sync();
			} else {
				printf("SWEET SMMU: poll GFSR=%#x FSYNR1=%#x "
				    "reached=%d\n", gfsr,
				    *(volatile uint32_t *)(g_gr_win + 0x54u),
				    reached);
				sweet_log_sync();
				g_smmu_faults("post");
			}
		}
	} else {
		g_send_gpr(pkt, pkt_size);
	}
	{
		static int nw_db = -1;

		if (nw_db < 0) {
			nw_db = 0;
			(void)PE_parse_boot_argn("sweetnowait", &nw_db,
			    sizeof(nw_db));
		}
		/* Fire-and-forget: the DSP answers nothing even when it is
		 * healthy, so a 100 ms wait per buffer would stall the tone
		 * loop for minutes.  Return success immediately. */
		if (nw_db) {
			g_drain(0);
			return 0;
		}
	}
	{
		/* 100 ms was a guess.  If the sink only starts pulling after the
		 * TDM comes up, the DONE reply can be late - sweetrspwait=<ms>
		 * widens the window for the single-buffer probe. */
		static int rspwait = -1;
		unsigned limit;

		if (rspwait < 0) {
			rspwait = 100;
			(void)PE_parse_boot_argn("sweetrspwait", &rspwait,
			    sizeof(rspwait));
			if (rspwait < 100 || rspwait > 5000) {
				rspwait = 100;
			}
		}
		limit = (unsigned)rspwait;
		for (unsigned i = 0; i < limit; i += 5) {
			g_drain(0);
			if (g_rsp_ready) {
				break;
			}
			g_wait(5);
		}
	}
	static int dbg;
	if (!g_rsp_ready || g_rsp_len < 40) {
		if (dbg++ < 4) {
			/* `len < 40` also swallows a real 32-byte basic response,
			 * so "no rsp" used to be an inference, not an observation.
			 * Print the raw fields before deciding. */
			printf("SWEET MARK: dbraw dst=%#x ready=%d len=%u "
			    "w0=%#x w4=%#x w8=%#x w12=%#x w16=%#x w20=%#x\n",
			    dst_port, g_rsp_ready, g_rsp_len,
			    g_rsp_len >= 4 ? g_le32(g_rsp + 0) : 0,
			    g_rsp_len >= 8 ? g_le32(g_rsp + 4) : 0,
			    g_rsp_len >= 12 ? g_le32(g_rsp + 8) : 0,
			    g_rsp_len >= 16 ? g_le32(g_rsp + 12) : 0,
			    g_rsp_len >= 20 ? g_le32(g_rsp + 16) : 0,
			    g_rsp_len >= 24 ? g_le32(g_rsp + 20) : 0);
			printf("SWEET GLINK: data_buffer: no rsp (dst=%#x)\n",
			    dst_port);
			sweet_log_sync();
		}
		return -1;
	}
	{
		uint32_t w16 = g_le32(g_rsp + 16);
		uint32_t rop = g_le32(g_rsp + 20);
		uint32_t w24 = g_le32(g_rsp + 24);
		uint32_t w28 = g_le32(g_rsp + 28);
		uint32_t w32 = g_le32(g_rsp + 32);
		uint32_t w36 = g_le32(g_rsp + 36);

		if (w16 != APM_RSP_DATA_BUFFER_DONE_V2 &&
		    rop != APM_RSP_DATA_BUFFER_DONE_V2) {
			if (dbg++ < 4) {
				printf("SWEET GLINK: data_buffer: dst=%#x w16=%#x "
				    "w20=%#x w24=%#x w28=%#x w32=%#x w36=%#x\n",
				    dst_port, w16, rop, w24, w28, w32, w36);
				sweet_log_sync();
			}
			return -1;
		}
		/* Print the first few replies whether or not they are errors:
		 * a successful DATA_BUFFER_DONE is exactly what we are looking
		 * for and it used to be silent. */
		if (dbg++ < 4) {
			printf("SWEET GLINK: data_buffer: dst=%#x done status=%#x "
			    "w28=%#x w32=%#x\n", dst_port, w36, w28, w32);
			g_dump("data_buffer rsp", g_rsp, g_rsp_len);
			sweet_log_sync();
		}
		return w36 == 0 ? 0 : -1;
	}
}

static int g_tone_filled;

/* Map the PCM region and fill the tone *before* the graph starts.  The
 * vendor's packet order is GRAPH_OPEN -> SHM_MAP -> SET_CFG -> PREPARE ->
 * START -> DATA_BUFFER.  Run67 did the map/fill after START and the machine
 * froze at the first write, right after "shm mapped": once the WR module is
 * running it can see the region, so the AP must not be the one touching it
 * later.  No flush_dcache64() here: it takes a physical address and goes
 * through phystokv_range(), which panics for any PA outside the ADT bank
 * (run64, PA 0xa2e00000).  The ml_io_map window is Device-nGnRnE. */
static int
g_pcm_prepare(void)
{
	if (g_apm_shm_map(0x2002u) != 0 || g_shm_pa == 0) {
		printf("SWEET GLINK: pcm: no shared memory\n");
		sweet_log_sync();
		return -1;
	}
	if (g_shm_va && !g_tone_filled) {
		g_fill_tone(G_PCM_OFF_A);
		g_fill_tone(G_PCM_OFF_B);
		__asm__ volatile("dsb sy" ::: "memory");
		g_tone_filled = 1;
		printf("SWEET GLINK: pcm: tone filled before start\n");
	} else if (!g_shm_va) {
		printf("SWEET GLINK: pcm: using SHIM-prefilled low DRAM\n");
	}
	sweet_log_sync();
	return 0;
}


/*
 * 0243: a dense IBAT burst in three phases, so that 出声 has a witness that is
 * neither a register snapshot nor a GLINK reply.  Everything static so far says
 * the same thing (0237/anchor3/nobb: identical PA registers at three time
 * points), and a static value cannot tell "configured" from "driving".  Energy
 * can: an amplifier pushing a real waveform takes current, and none of our
 * bookkeeping can fake that.
 *
 * The path used here is the direct SPMI read that SweetBattery.cpp already uses
 * (iokit/Kernel/arm/SweetBattery.cpp:236-272): sid 0, 0x4805 gives the current
 * channel subtype (3 or 4), 0x48c2/0x48c3 hold the raw word, 0x8000 means "no
 * data", scale is 152588 (subtype 3) or 305176 (subtype 4), and the sign is
 * flipped, so a NEGATIVE printed mA is discharge.  We deliberately do not use
 * the "SWEET BATT: tick" numbers: those come from a pmic_glink property cache
 * printed once per ~30 s, which cannot overlap a sub-second tone at all.
 *
 * Each phase prints its own summary with ok=<valid>/<N> and the usb bit, then
 * syncs the log.  A phase that collected no samples is therefore visible as a
 * broken instrument (void) rather than as a reading of zero, and whether USB was
 * attached can be scored from the log itself instead of from memory.
 */
static void
g_ibat_burst(const char *phase)
{
	int burst = 0;
	int exp = 0;
	int rc = -1;
	int n;
	const int N = 12;
	int ok = 0;
	int lo = 0, hi = 0, mean = 0;
	int vals[12];
	uint8_t subtype = 0xff, usb = 0xff;

	/*
	 * Report the gate, but only AFTER parsing it: the first cut of this fix-up
	 * printed "flag=%d" ahead of PE_parse_boot_argn and would have read flag=0
	 * even on a boot where the argument had arrived - a sentinel that cannot
	 * occur as a reading is not a sentinel.
	 *
	 * sweetexp is the aggregate switch.  The boot-args field is 607 bytes and
	 * mkpayload used to slice the overflow silently, which is how ring-ab lost
	 * three flags at once and ran its tone uncapped; instruments from here on
	 * share one argument so that adding a witness costs no cmdline bytes at all
	 * (bit 0 = this IBAT burst, bit 1 = the IOKit audio service probe).
	 */
	(void)PE_parse_boot_argn("sweetibatburst", &burst, sizeof(burst));
	(void)PE_parse_boot_argn("sweetexp", &exp, sizeof(exp));
	if (!burst && (exp & 0x1) != 0) {
		burst = 1;
	}
	printf("SWEET MARK: ibat %s flag=%d exp=%#x\n", phase, burst, exp);
	sweet_log_sync();
	if (!burst) {
		return;
	}
	rc = sweet_spmi_read(0, 0x4805, &subtype);
	printf("SWEET MARK: ibat %s gauge sid=0 addr=0x4805 rc=%d val=%#x\n",
	    phase, rc, subtype);
	sweet_log_sync();
	if (rc != 0 || (subtype != 3 && subtype != 4)) {
		/*
		 * Located, not assumed.  The live Android device tree declares no
		 * fuel-gauge function under any SPMI PMIC - the SIDs it does declare
		 * are 0 pmk8350, 1 pm8350, 2 pm8350c, 3 pm8350b, 4 pmr735a,
		 * 7 pm8450, 8/9 pm8010-debug, c/d pm8008i/j
		 * (logs/fdt/marble-live.dts:8007,8400,8441,8673,8842,8891) - and its
		 * "qcom,battery_charger" node hangs off compatible "qcom,pmic-glink"
		 * (same file :9007,9013,9019).  That predicts there is nothing to read
		 * over SPMI here at all, which is what this boot's 0xff agreed with.
		 *
		 * The previous version of this line ORed "read failed" together with
		 * "read a value I did not expect", so both printed subtype=255 - my own
		 * rule says an instrument has to be able to tell its own failure from a
		 * reading, and this one could not.  Hence rc and val are printed
		 * separately now.  The sweep spends the rest of the prediction: one
		 * line per candidate SID, pre-phase only, so it costs nothing on a boot
		 * that already carries this instrument.  0xa5 is a sentinel written
		 * before the read, so a failed read cannot masquerade as a value.
		 */
		if (rc == 0) {
			printf("SWEET MARK: ibat %s gauge present, unexpected val\n",
			    phase);
			sweet_log_sync();
			return;
		}
		if (phase[0] == 'p' && phase[1] == 'r') {
			static const unsigned char sids[] = {0, 1, 2, 3, 4, 7};
			int rc2;

			for (n = 0; n < (int)(sizeof(sids)); n++) {
				uint8_t v = 0xa5;

				rc2 = sweet_spmi_read(sids[n], 0x4805, &v);
				printf("SWEET MARK: ibat sweep sid=%u addr=0x4805"
				    " rc=%d val=%#x\n", sids[n], rc2, v);
				sweet_log_sync();
			}
		}
		return;
	}
	for (n = 0; n < N; n++) {
		uint8_t l = 0, h = 0;
		int raw, ma;

		if (sweet_spmi_read(0, 0x48c2, &l) == 0 &&
		    sweet_spmi_read(0, 0x48c3, &h) == 0) {
			raw = (int)l | ((int)h << 8);
			if (raw != 0x8000) {
				if (raw & 0x8000) {
					raw -= 65536;
				}
				ma = (int)(-((int64_t)raw * (subtype == 3 ?
				    152588 : 305176)) / 1000000);
				vals[ok++] = ma;
			}
		}
		delay(10000);
	}
	(void)sweet_spmi_read(0, 0x1310, &usb);
	for (n = 0; n < ok; n++) {
		if (n == 0 || vals[n] < lo) {
			lo = vals[n];
		}
		if (n == 0 || vals[n] > hi) {
			hi = vals[n];
		}
		mean += vals[n];
	}
	if (ok > 0) {
		mean /= ok;
	}
	printf("SWEET MARK: ibat %s ok=%d/%d sub=%u usb=%#x min=%d max=%d "
	    "mean=%d mA\n", phase, ok, N, subtype, usb, lo, hi, mean);
	sweet_log_sync();
}

static void
g_audio_tone(uint32_t src_port, uint32_t dst_port)
{
	int pcm = 0;

	/* The non-secure world cannot set S2CR.BYPASS (run71 read back
	 * TYPE=FAULT), so the DSP's first buffer access needs a real stage-2
	 * context.  g_smmu_probe() builds one under the same sweetpcm flag.
	 * Keep this probe opt-in: if the translation is wrong the DSP stalls
	 * the interconnect and the phone freezes. */
	(void)PE_parse_boot_argn("sweetpcm", &pcm, sizeof(pcm));
	if (!pcm) {
		printf("SWEET GLINK: pcm probe disabled (sweetpcm=1 to enable)\n");
		sweet_log_sync();
		return;
	}
	/* The vendor maps the PCM region from 0x2002 and feeds buffers from
	 * the stream port 0x2011; graph commands stay on 0x2010. */
	if (g_pcm_prepare() != 0) {
		printf("SWEET GLINK: pcm: tone skipped\n");
		sweet_log_sync();
		return;
	}

	{
		int nodata = 0;

		(void)PE_parse_boot_argn("sweetnodata", &nodata,
		    sizeof(nodata));
		if (nodata) {
			printf("SWEET GLINK: sweetnodata: START only, no "
			    "DATA_BUFFER\n");
			sweet_log_sync();
			return;
		}
	}
	/* Only the WR module accepts buffers (vendor trace: every
	 * DATA_BUFFER_V2 goes to the WR instance; SAL never answers).  Send
	 * exactly one probe there and stop guessing.
	 *
	 * sweetnowait=1 does not require a reply.  With the WR SET_CFG gone
	 * (sweetwrmf=0) nothing dies any more, but the DSP answers nothing
	 * either, so waiting for DATA_BUFFER_DONE means the tone loop never
	 * runs and we can never find out whether it is actually playing.
	 * Fire-and-forget is the only way to hear it. */
	int nw = 0;
	{
		static int nw_cached = -1;

		if (nw_cached < 0) {
			nw_cached = 0;
			(void)PE_parse_boot_argn("sweetnowait", &nw_cached,
			    sizeof(nw_cached));
		}
		nw = nw_cached;
	}
	/*
	 * sweetdspcheck=1 answers the question pacing cannot: is the DSP still
	 * ALIVE after a buffer?  l2/l3 stopped getting intents back after ~96
	 * sends, i.e. the DSP stopped reading GLINK at all - which would mean it
	 * died on the first buffer and every "completed" run since was talking
	 * to a corpse.  Ask APM for its state before and after one buffer.
	 */
	int dspcheck = 0;
	{
		(void)PE_parse_boot_argn("sweetdspcheck", &dspcheck,
		    sizeof(dspcheck));
	}
	if (dspcheck && dst_port == G_VP_WR_IID) {
		g_apm_state_query_sync("before-buf");
		{
			int r = g_apm_data_buffer(g_stream_port, G_VP_WR_IID,
			    G_PCM_OFF_A, G_PCM_BYTES, 0);

			printf("SWEET GLINK: dspcheck buffer -> %d\n", r);
			sweet_log_sync();
		}
		g_apm_state_query_sync("after-buf");
		return;
	}
	if (dst_port == G_VP_WR_IID) {
		int r = g_apm_data_buffer(g_stream_port, G_VP_WR_IID,
		    G_PCM_OFF_A, G_PCM_BYTES, 0);

		printf("SWEET GLINK: buffer probe dst=%#x -> %d%s\n",
		    G_VP_WR_IID, r, (r != 0 && nw) ? " (ignored)" : "");
		sweet_log_sync();
		if (r != 0 && !nw) {
			return;
		}
	}
	printf("SWEET GLINK: pcm tone start (dst=%#x)%s\n", dst_port,
	    nw ? " fire-and-forget" : "");
	g_ibat_burst("pre");

	/* 2000 buffers x 100 ms of waiting is 200 s; that only makes sense when
	 * the DSP actually replies.  Fire-and-forget needs a few seconds of
	 * audio, not five minutes. */
	/*
	 * The shim fills the whole shared-memory region with raw PCM
	 * (48 kHz/24-bit/stereo), not just the two 1152-byte buffers the vendor
	 * ping-pongs.  Streaming across the region plays that clip instead of a
	 * synthetic tone.  Stay one buffer short of the end so no offset runs
	 * past what SHM_MAP declared - the DSP validates the range.
	 */
	const uint32_t region = (uint32_t)g_shm_region_size();
	const uint32_t blocks = (region > G_PCM_BYTES) ?
	    ((region - G_PCM_BYTES) / G_PCM_BYTES + 1u) : 1u;
	/* Fire-and-forget walks the whole region once, i.e. plays the entire
	 * clip; the reply-waiting path keeps the vendor's 2000 ping-pongs. */
	int tone_buffers = nw ? (int)blocks : 2000;
	{
		/* sweetbufs=N caps the loop so region size and flood length can
		 * be tested separately instead of changing together. */
		int cap = 0;

		(void)PE_parse_boot_argn("sweetbufs", &cap, sizeof(cap));
		if (cap > 0 && cap < tone_buffers) {
			tone_buffers = cap;
		}
	}

	if (tone_buffers > 1200) {
		tone_buffers = 1200;
	}

	for (int n = 0; n < tone_buffers; n++) {
		/* 0243: sampled once, at the middle of the feed loop, so the burst
		 * overlaps the tone; this is not a per-buffer probe. */
		if (n == tone_buffers / 2) {
			g_ibat_burst("tone");
		}
		uint32_t off = nw ?
		    ((uint32_t)((unsigned)n % blocks) * G_PCM_BYTES) :
		    ((n & 1) ? G_PCM_OFF_B : G_PCM_OFF_A);

		{
			/* sweetbufgap=<ms> paces the fire-and-forget stream.  Sending
			 * 909 buffers back to back killed the DSP (j1); 30 did not.
			 * One buffer is 1152 bytes = 192 frames = 4 ms at 48 kHz,
			 * so 4 ms is roughly real time and anything larger just
			 * underruns. */
			static int gap = -1;

			if (gap < 0) {
				gap = 0;
				(void)PE_parse_boot_argn("sweetbufgap", &gap,
				    sizeof(gap));
				if (gap > 0 && gap < 500) {
					printf("SWEET GLINK: tone pace %d ms/buffer\n",
					    gap);
					sweet_log_sync();
				}
			}
			if (gap > 0) {
				delay(gap);
			}
		}
		if (nw && (n & 31) == 0) {
			/* So a death inside the loop still tells us how far it got. */
			printf("SWEET GLINK: tone buf %d/%d off=%#x\n", n,
			    tone_buffers, off);
			sweet_log_sync();
		}
		if (g_apm_data_buffer(g_stream_port, dst_port, off, G_PCM_BYTES,
		    (uint32_t)(n & 1)) != 0 && !nw) {
			printf("SWEET GLINK: pcm buffer %d not consumed\n", n);
			return;
		}
	}
	printf("SWEET GLINK: pcm tone done (%d buffers)\n", tone_buffers);
	g_ibat_burst("post");
}

/*
 * The AGM table's simple graph feeds a CODEC_DMA sink from a SAL module.
 * That graph is kept as the fallback control case below; the TDM graph above
 * replaces both ends with the shared-memory writer and the TDM sink.
 */
static void
g_audio_graph_cfg(uint32_t src_port)
{
	uint8_t mf[12], intf[12];
	uint32_t v, width = 16;
	int i, ok = 0;

	/* This firmware rejects data_format 0: fixed point is 1.  The vendor
	 * endpoint runs 96 kHz / 32 bit; try that when 48 kHz / 16 bit is
	 * refused, and make the SAL width follow whatever was accepted. */
	for (i = 0; i < 2; i++) {
		uint32_t rate = i ? 96000 : 48000;

		width = i ? 32 : 16;
		g_put32(mf + 0, rate);
		g_put32(mf + 4, width | (2u << 16));    /* bit_width, channels */
		g_put32(mf + 8, 1);                     /* DATA_FORMAT_FIXED_POINT */
		if (g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001017, mf, 12,
		    "cdma/mf_cfg") == 0) {
			ok = 1;
			break;
		}
	}
	if (!ok) {
		printf("SWEET GLINK: cdma media format rejected at 48k and 96k\n");
	}

	v = width;
	(void)g_apm_set_cfg(src_port, G_SAL_IID, 0x08001016, &v, 4,
	    "sal/output_cfg");
	v = 1;
	(void)g_apm_set_cfg(src_port, G_SAL_IID, 0x0800101e, &v, 4,
	    "sal/limiter");

	v = 1;
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001018, &v, 4,
	    "cdma/frame_size");

	/* Which interface-parameter family this module accepts depends on its
	 * revision: the vendor's hw endpoint took 0x0800101B (rejected here
	 * with status 3), while our CODEC_DMA instance knows 0x08001063 but
	 * refused every value we tried.  The vendor's body starts with
	 * {0, 2}: lpaif_type 0 and interface index 2, so sweep that space. */
	{
		static const uint32_t intf_v2[7] = {
			0x00000000, 0x00000002, 0x00010001, 0x00000003,
			0x00200004, 0x00000000, 0x00000001,
		};
		static const uint32_t types[2] = { 0, 3 };
		static const uint32_t idxs[3] = { 2, 1, 0 };
		int done = 0;

		(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x0800101b, intf_v2,
		    28, "cdma/intf_v2");

		for (i = 0; i < 6 && !done; i++) {
			uint32_t t = types[i / 3], x = idxs[i % 3];
			char what[24];

			snprintf(what, sizeof(what), "cdma/intf%u/%u", t, x);
			g_put32(intf + 0, t);
			g_put32(intf + 4, x);
			g_put32(intf + 8, 3);
			if (g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001063,
			    intf, 12, what) == 0) {
				done = 1;
			}
		}
	}
	v = 0;
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001176, &v, 4,
	    "cdma/power_mode");
}

/*
 * The speaker path does not go through the WCD codec's CODEC_DMA: the two
 * AW882xx amplifiers hang off the LPASS TDM/I2S lines.  The vendor's capture
 * graph proves the endpoint family and its configuration (TDM interface 2,
 * 4 slots of 32 bit, slots 0/1 active, internal sync, 96 kHz / 32 bit).
 *
 * The graph's input must be MODULE_ID_WR_SHARED_MEM_EP (0x07001000), not the
 * SAL module the AGM table happens to use: only the shared-memory endpoint
 * accepts PARAM_ID_MEDIA_FORMAT and the DATA_BUFFER_V2 stream.  Both the WR
 * endpoint and the TDM sink run 96 kHz / 32 bit, so no converter is needed.
 */
static void
g_audio_graph_cfg_tdm(uint32_t src_port)
{
	/* PARAM_ID_TDM_INTF_CFG, captured verbatim from the vendor endpoint:
	 * lpaif 0, intf 2, sync internal, data-out enabled, slot mask 3,
	 * 4 slots/frame, 32-bit slots, short-sync bit mode, 1 BCLK delay. */
	static const uint8_t tdm_intf[28] = {
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
	};
	uint8_t mf[12], sal_mf[32];
	uint32_t v;

	/* PARAM_ID_MEDIA_FORMAT for both ends: the WR writes 96 kHz / 32 bit
	 * stereo, and the sink's graph-side input must match it. */
	g_put32(sal_mf + 0, 1);                 /* fixed point */
	g_put32(sal_mf + 4, 0x09001000);        /* MEDIA_FMT_ID_PCM */
	g_put32(sal_mf + 8, 18);                /* PCM payload size */
	g_put32(sal_mf + 12, 96000);
	sal_mf[16] = 32; sal_mf[17] = 0;        /* bit_width */
	sal_mf[18] = 1;                         /* alignment */
	sal_mf[19] = 0;                         /* 0 = interleaved */
	sal_mf[20] = 32; sal_mf[21] = 0;        /* bits_per_sample */
	sal_mf[22] = 31; sal_mf[23] = 0;        /* q_factor */
	sal_mf[24] = 1;  sal_mf[25] = 0;        /* endianness */
	sal_mf[26] = 2;  sal_mf[27] = 0;        /* channels */
	sal_mf[28] = 1;  sal_mf[29] = 2;        /* L, R */
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x0800100c, sal_mf, 30,
	    "tdm/media_format");

	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x0800101b, tdm_intf,
	    sizeof(tdm_intf), "tdm/intf");

	/* PARAM_ID_HW_EP_MF_CFG: 96 kHz, 32 bit, 2 channels, fixed point. */
	g_put32(mf + 0, 96000);
	g_put32(mf + 4, 32u | (2u << 16));
	g_put32(mf + 8, 1);
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001017, mf, 12,
	    "tdm/mf");

	v = 1;
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001018, &v, 4,
	    "tdm/frame_size");
	v = 0;
	(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x08001176, &v, 4,
	    "tdm/power_mode");

	/* PARAM_ID_HW_INTF_CLK_CFG: default clock id/root, 12.288 MHz
	 * (96 kHz x 4 slots x 32 bit). */
	{
		uint32_t clk[4] = { 0, 12288000, 0, 0 };

		(void)g_apm_set_cfg(src_port, G_CDMA_IID, 0x0800113c, clk,
		    16, "tdm/clk_cfg");
	}

	(void)g_apm_set_cfg(src_port, G_SAL_IID, 0x0800100c, sal_mf, 30,
	    "wr/media_format");
}


/*
 * The vendor's real speaker graph (subgraph 0xb0000002) from the AGM
 * service: SAL(0x4012) -> 0x48ba -> AW module(0x42d3) -> 0x418e ->
 * 0x4006 -> 0x4007 -> TDM sink(0x428f).  The TDM side is 3 channels
 * (slot mask 7); the AW module supplies speaker protection.
 */
static void
g_audio_graph_cfg_vp(uint32_t src_port)
{
	static const uint8_t tdm_intf[28] = {
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
	};
	/* Vendor AW parameter 0x10013D2A, captured from the speaker graph. */
	/*
	 * 0248: the vendor sends this 28-byte payload twice for the AW module -
	 * {0, 0x10013d1b, 1, 0, 0, 0, 0x632b} and {0, 0x10013d1c, 1, 0, 0, 0,
	 * 0x6f60}.  Our previous copy put the value in word [5] instead of [6]
	 * and never sent the second one, which fits ep9: the tone ran and not
	 * one PA register moved.
	 */
	static const uint8_t aw[28] = {
		0x00, 0x00, 0x00, 0x00, 0x1b, 0x3d, 0x01, 0x10,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x2b, 0x63, 0x00, 0x00,
	};
	static const uint8_t aw2[28] = {
		0x00, 0x00, 0x00, 0x00, 0x1c, 0x3d, 0x01, 0x10,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x6f, 0x00, 0x00,
	};
	uint8_t mf[32];
	uint32_t v;
	int rc;

	/* The AP endpoint is the WR_SHARED_MEM_EP, not SAL: in the vendor
	 * kprobe trace the module that receives media-format 0x0800100C and
	 * every DATA_BUFFER_V2 is the WR instance (0x40bd), while SAL only
	 * gets module configs.  96 kHz / 32 bit / stereo first, then the
	 * vendor's 48 kHz / 24 bit form if the module refuses it. */
	g_put32(mf + 0, 1);
	g_put32(mf + 4, 0x09001000);
	g_put32(mf + 8, 18);
	/* Vendor kprobe 2026-10-27, SET_CFG(0x40bd, 0x0800100C) byte for
	 * byte: 48000 Hz, 16-bit, q15, LSB-aligned, LE, stereo {1,2}.
	 * We had been declaring 24-bit here while the vendor says 16. */
	g_put32(mf + 12, 48000);
	mf[16] = 16; mf[17] = 0; mf[18] = 1; mf[19] = 0;
	mf[20] = 16; mf[21] = 0; mf[22] = 15; mf[23] = 0;
	mf[24] = 1;  mf[25] = 0; mf[26] = 2;  mf[27] = 0;
	mf[28] = 1;  mf[29] = 2;
	{
		/* The vendor trace never configures the WR by SET_CFG; the
		 * module's format comes from the graph.  sweetwrmf=0 drops our
		 * extra call so "harmless but wrong" can be ruled out without
		 * touching the graph. */
		int wrmf = 1;

		(void)PE_parse_boot_argn("sweetwrmf", &wrmf, sizeof(wrmf));
		rc = wrmf ? g_apm_set_cfg(src_port, G_VP_WR_IID, 0x0800100c,
		    mf, 30, "vp/wr_mf") : 0;
	}
	if (rc != 0) {
		g_put32(mf + 12, 48000);
		mf[16] = 16; mf[17] = 0;
		mf[20] = 16; mf[21] = 0; mf[22] = 15; mf[23] = 0;
		(void)g_apm_set_cfg(src_port, G_VP_WR_IID, 0x0800100c, mf, 30,
		    "vp/wr_mf48");
	}

	/* TDM sink input: 96 kHz / 32 bit / 3 channels. */
	g_put32(mf + 0, 1);
	g_put32(mf + 4, 0x09001000);
	g_put32(mf + 8, 19);
	g_put32(mf + 12, 96000);
	mf[16] = 32; mf[17] = 0; mf[18] = 1; mf[19] = 0;
	mf[20] = 32; mf[21] = 0; mf[22] = 31; mf[23] = 0;
	mf[24] = 1;  mf[25] = 0; mf[26] = 3;  mf[27] = 0;
	mf[28] = 1;  mf[29] = 2; mf[30] = 3;
	/* The vendor never gives the TDM endpoint a generic media format
	 * (0x0800100C); it sends the TDM-specific pair below, which is why
	 * 0x0800100C came back EUNSUPPORTED (0x3) every round.  Values are
	 * copied byte-for-byte from the vendor GPR trace. */
	{
		uint8_t tdm_rate[12] = { 0 };

		g_put32(tdm_rate + 0, 96000);
		g_put32(tdm_rate + 4, 0x30020);
		g_put32(tdm_rate + 8, 1);
		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001017,
		    tdm_rate, 12, "vp/tdm_rate");
	}

	{
		uint8_t tdm_cfg[28] = { 0 };

		g_put32(tdm_cfg + 0, 0);
		g_put32(tdm_cfg + 4, 2);
		g_put32(tdm_cfg + 8, 0x10001);
		g_put32(tdm_cfg + 12, 7);
		g_put32(tdm_cfg + 16, 0x200004);
		g_put32(tdm_cfg + 20, 0);
		g_put32(tdm_cfg + 24, 1);
		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x0800101b,
		    tdm_cfg, 28, "vp/tdm_cfg");
	}

	/*
	 * sweetcccfg=1 only.  Same two parameters, same bytes, as ONE two-record
	 * list to the APM: (16 + align8(12)) + (16 + align8(28)) = 80 bytes, so it
	 * must go in-band by the threshold above.  Nothing else in this round
	 * changes; without the flag the block below does not run at all.
	 */
	{
		int on = 0;

		(void)PE_parse_boot_argn("sweetcccfg", &on, sizeof(on));
		if (on != 0) {
			static const uint32_t cc_rate[3] = { 96000, 0x30020, 1 };
			static const uint32_t cc_cfg[7] = {
				0, 2, 0x10001, 7, 0x200004, 0, 1
			};
			uint8_t body[SWEET_CC_MAX];
			uint32_t off = 0;
			uint32_t i;

			memset(body, 0, sizeof(body));
			g_put32(body + off + 0, G_VP_TDM_IID);
			g_put32(body + off + 4, 0x08001017);
			g_put32(body + off + 8, sizeof(cc_rate));
			for (i = 0; i < 3; i++) {
				g_put32(body + off + 16 + i * 4, cc_rate[i]);
			}
			off += 16 + ((sizeof(cc_rate) + 7u) & ~7u);
			g_put32(body + off + 0, G_VP_TDM_IID);
			g_put32(body + off + 4, 0x0800101b);
			g_put32(body + off + 8, sizeof(cc_cfg));
			for (i = 0; i < 7; i++) {
				g_put32(body + off + 16 + i * 4, cc_cfg[i]);
			}
			off += 16 + ((sizeof(cc_cfg) + 7u) & ~7u);
			(void)g_apm_set_cfg_batch(src_port, body, off, 2,
			    "cc_batch");
		}
	}
	/*
	 * sweetoobcfg=1 only, and not together with sweetcccfg: one container per
	 * round.  9 records x 32 bytes = 288 > 256, so this is the form that has to
	 * go through shared memory; 8 records would sit exactly on the in-band cap.
	 */
	{
		int on = 0;

		(void)PE_parse_boot_argn("sweetoobcfg", &on, sizeof(on));
		if (on != 0) {
			(void)g_apm_set_cfg_oob(src_port, "oob_batch");
		}
	}

	g_put32(mf + 0, 96000);
	g_put32(mf + 4, 32u | (3u << 16));
	g_put32(mf + 8, 1);
	(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001017, mf, 12,
	    "vp/tdm_ep_mf");

	v = 1;
	(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001018, &v, 4,
	    "vp/tdm_frame");
	/*
	 * 0248: the vendor trace never sends 0x08001176 at all - it comes from the
	 * CDMA-era list.  sweettdmpwr keeps the shipped behaviour (0) addressable
	 * without another build: 1 sends the value 1, 2 does not send the packet.
	 */
	{
		int tp = 0;

		(void)PE_parse_boot_argn("sweettdmpwr", &tp, sizeof(tp));
		if (tp != 2) {
			v = tp == 1 ? 1 : 0;
			(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001176,
			    &v, 4, "vp/tdm_power");
		} else {
			printf("SWEET GLINK: tdm_power skipped (vendor does not send it)\n");
			sweet_log_sync();
		}
	}
	{
		uint32_t clk[4] = { 0, 12288000, 0, 0 };

		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x0800113c, clk,
		    16, "vp/tdm_clk");
	}

	(void)g_apm_set_cfg(src_port, G_VP_AW_IID, 0x10013d2a, aw,
	    sizeof(aw), "vp/aw_cfg");
	(void)g_apm_set_cfg(src_port, G_VP_AW_IID, 0x10013d2a, aw2,
	    sizeof(aw2), "vp/aw_cfg2");

	{
		/*
		 * sweetep4308=1 also configures the second endpoint instance the
		 * vendor sets these parameters on (iid 0x4308, slot_mask 3 instead
		 * of 7).  Default off: it is the next single variable, not this one.
		 */
		int ep4308 = 0;

		(void)PE_parse_boot_argn("sweetep4308", &ep4308, sizeof(ep4308));
		if (ep4308) {
			uint8_t mf2[12];
			uint8_t cf2[28] = { 0 };

			g_put32(mf2 + 0, 96000);
			g_put32(mf2 + 4, 32u | (2u << 16));
			g_put32(mf2 + 8, 1);
			(void)g_apm_set_cfg(src_port, 0x4308u, 0x08001017, mf2,
			    sizeof(mf2), "ep4308_mf");
			g_put32(cf2 + 0, 0);
			g_put32(cf2 + 4, 2);
			g_put32(cf2 + 8, 0x10001);
			g_put32(cf2 + 12, 3);
			g_put32(cf2 + 16, 0x200004);
			g_put32(cf2 + 20, 0);
			g_put32(cf2 + 24, 1);
			(void)g_apm_set_cfg(src_port, 0x4308u, 0x0800101b, cf2,
			    sizeof(cf2), "ep4308_cfg");
		}
	}

	{
		/*
		 * sweetvcfg2=1 replays the four module configs the vendor sends and
		 * we never do, byte for byte, from the two kprobe captures
		 * (docs/audio-closeout.md section 3): param 0x08001008 length 30 to
		 * 0x40bf and 0x40c0, param 0x08001024 length 14 to 0x40c8, and the
		 * port map 0x080010C4 = {0x428f} to 0x4165.
		 *
		 * Those four instance ids are NOT inside the graph we open - a byte
		 * search for bf 40 / c0 40 / c8 40 / 65 41 in g_graph_vp_wr returns
		 * zero hits - so the honest expectation is an error or silence.  The
		 * point of the boot is precisely that: if any of the four answers
		 * status 0 then the graph does contain them after all and the fix is
		 * "send more config"; if all four fail then the gap is structural and
		 * the only real options are the vendor blob (already judged dead in
		 * this same permission state) or extending the graph by hand.
		 *
		 * No param here is given a name: 0x08001008/0x08001024/0x080010C4 are
		 * identified only by position and length in the captured packets.
		 */
		int vcfg2 = 0;
		int rc_bf = -1, rc_c0 = -1, rc_c8 = -1, rc_65 = -1;
		uint8_t conv[30];
		uint8_t scl[14];
		uint8_t pmap[4];

		(void)PE_parse_boot_argn("sweetvcfg2", &vcfg2, sizeof(vcfg2));
		if (vcfg2) {
			memset(conv, 0, sizeof(conv));
			g_put32(conv + 0, 1);
			g_put32(conv + 4, 0x09001000);
			g_put32(conv + 8, 18);
			g_put32(conv + 12, 0x00010010);
			g_put32(conv + 16, 0x000f0010);
			g_put32(conv + 20, 0x00030001);
			g_put32(conv + 24, 0x00020000);
			conv[28] = 0x01;
			conv[29] = 0x02;

			memset(scl, 0, sizeof(scl));
			g_put32(scl + 0, 96000);
			g_put32(scl + 4, 0x00030018);
			g_put32(scl + 8, 0x00020001);
			scl[12] = 0x03;

			g_put32(pmap + 0, G_VP_TDM_IID);

			rc_bf = g_apm_set_cfg(src_port, 0x40bfu, 0x08001008u,
			    conv, sizeof(conv), "vp/cfg_40bf");
			rc_c0 = g_apm_set_cfg(src_port, 0x40c0u, 0x08001008u,
			    conv, sizeof(conv), "vp/cfg_40c0");
			rc_c8 = g_apm_set_cfg(src_port, 0x40c8u, 0x08001024u,
			    scl, sizeof(scl), "vp/cfg_40c8");
			rc_65 = g_apm_set_cfg(src_port, 0x4165u, 0x080010c4u,
			    pmap, sizeof(pmap), "vp/cfg_4165");
			printf("SWEET MARK: vcfg2 rc bf=%d c0=%d c8=%d "
			    "p65=%d\n", rc_bf, rc_c0, rc_c8, rc_65);
			sweet_log_sync();
		}
	}
}

/* PREPARE / START / STOP take a SUB_GRAPH_LIST {count, id}. */
static int
g_apm_graph_ctrl(uint32_t src_port, uint32_t sg_id, uint32_t opcode,
    const char *what)
{
	uint8_t pl[24];

	memset(pl, 0, sizeof(pl));
	g_put32(pl + 0, 1);                     /* module_instance_id: APM */
	g_put32(pl + 4, APM_PARAM_ID_SUB_GRAPH_LIST);
	g_put32(pl + 8, 8);                     /* param_size */
	g_put32(pl + 12, 0);                    /* error_code */
	g_put32(pl + 16, 1);                    /* num_sub_graphs */
	g_put32(pl + 20, sg_id);
	return g_apm_cmd(src_port, 1, pl, sizeof(pl), opcode, what);
}

/* Same packet with an arbitrary subgraph list: the vendor groups several
 * subgraphs into one graph and prepares/starts them together (the list param
 * grows to 4 * n bytes). */
static int
g_apm_graph_ctrl_list(uint32_t src_port, const uint32_t *ids, uint32_t n,
    uint32_t opcode, const char *what)
{
	uint8_t pl[24 + 4 * 8];
	uint32_t sz = 4 * n;

	if (n == 0 || n > 8) {
		return -1;
	}
	memset(pl, 0, sizeof(pl));
	g_put32(pl + 0, 1);                     /* module_instance_id: APM */
	g_put32(pl + 4, APM_PARAM_ID_SUB_GRAPH_LIST);
	g_put32(pl + 8, sz);                    /* param_size */
	g_put32(pl + 12, 0);                    /* error_code */
	g_put32(pl + 16, n);                    /* num_sub_graphs */
	for (uint32_t i = 0; i < n; i++) {
		g_put32(pl + 20 + 4 * i, ids[i]);
	}
	return g_apm_cmd(src_port, 1, pl, 20 + sz, opcode, what);
}

static int
g_apm_graph_start_list(uint32_t src_port, const uint32_t *ids, uint32_t n)
{
	if (g_apm_graph_ctrl_list(src_port, ids, n, APM_CMD_GRAPH_PREPARE,
	    "graph_prepare") != 0) {
		return -1;
	}
	return g_apm_graph_ctrl_list(src_port, ids, n, APM_CMD_GRAPH_START,
	    "graph_start");
}

static int
g_apm_graph_start(uint32_t src_port, uint32_t sg_id)
{
	if (g_apm_graph_ctrl(src_port, sg_id, APM_CMD_GRAPH_PREPARE,
	    "graph_prepare") != 0) {
		return -1;
	}
	if (g_apm_graph_ctrl(src_port, sg_id, APM_CMD_GRAPH_START,
	    "graph_start") != 0) {
		return -1;
	}
	return 0;
}

static void
g_apm_graph_bringup(void)
{
	/* The vendor's AGM clients send from 0x2010/0x2011. */
	/*
	 * Vendor trace: the stream graph (0x1f0 bytes) is opened from 0x2010
	 * but the DEVICE graph that contains WR_SHARED_MEM_EP (0xb38) is
	 * opened from 0x2011 - and DATA_BUFFER is pushed from 0x2011 too, so
	 * the sink and its buffers share one client port.  We merge both
	 * graphs into one and open it from 0x2010, then push data from
	 * 0x2011: the WR belongs to a graph its buffers do not come from.
	 * sweets1gp selects the port so this can be tested both ways.
	 */
	uint32_t port = 0x2010u;
	{
		int gp = 0;

		(void)PE_parse_boot_argn("sweetgraphport", &gp, sizeof(gp));
		if (gp > 0) {
			port = (uint32_t)gp;
			printf("SWEET GLINK: graph port override %#x\n", port);
			sweet_log_sync();
		}
	}
	/* Buffers must be pushed from the port that owns the graph. */
	g_stream_port = port;
	uint8_t tdm_payload[sizeof(g_graph_open_payload)];

	/* The vendor's device graph starts at SAL, which has no shared-memory
	 * input: in the vendor kprobe trace DATA_BUFFER_V2 goes to a
	 * WR_SHARED_MEM_EP instance that lives in a separate stream subgraph.
	 * Merge one WR module into the device graph (WR:1 -> SAL:2) so a
	 * single subgraph has a real AP input.  The module list, port table
	 * and connection come from the vendor stream-graph dump 0xb00000d6
	 * (WR has 0 input ports and 1 output port). */
	/*
	 * sweetshmgraph=1 probes the vendor's shared-memory form: a 40-byte
	 * GRAPH_OPEN whose descriptor is {offset, 0, handle, size} with the
	 * graph definition inside the mapped region.
	 *
	 * ★ It is opt-in, NOT the default.  The audio bring-up runs on every
	 * boot, and this form makes the DSP read the region - which hangs the
	 * SoC before the GUI ever starts (run115).  The inline form needs no
	 * DSP memory access, so it is the safe default and lets iOS boot.
	 */
	/* sweetvgraph=1 opens the vendor's real 4-subgraph graph.  The
	 * definition lives in the mapped region at offset 0, exactly as the
	 * vendor sends it; sweetvgoff/sweetvglen let the 12-byte host-header
	 * question be tested without another build. */
	{
		int vg = 0;

		(void)PE_parse_boot_argn("sweetvgraph", &vg, sizeof(vg));
		if (vg) {
			/* sweetvgraph=1 replays g_graph_vendor4 (the October heap copy:
			 * 4 bytes short of its own last record, framed with a 12-byte host
			 * header).  =2 sends this generation's container, self-consistent
			 * and framed with the 16-byte header the vendor's 2712 implies. */
			const uint8_t *body = vg == 2 ? g_graph_ukee_current :
			    g_graph_vendor4;
			const uint32_t *sgs = vg == 2 ? g_ukee_current_sgs :
			    g_vendor4_sgs;
			const char *tag = vg == 2 ? "graph_open/ukee2960" :
			    "graph_open/vendor4";
			uint32_t body_sz = vg == 2 ?
			    (uint32_t)sizeof(g_graph_ukee_current) :
			    (uint32_t)sizeof(g_graph_vendor4);
			int vgoff = vg == 2 ? 16 : 12;
			int vglen = vg == 2 ? 2944 : 2712;

			(void)PE_parse_boot_argn("sweetvgoff", &vgoff,
			    sizeof(vgoff));
			(void)PE_parse_boot_argn("sweetvglen", &vglen,
			    sizeof(vglen));
			if (vgoff < 0 || vgoff > 64 ||
			    (uint32_t)(vgoff + vglen) > body_sz) {
				printf("SWEET GLINK: vgraph off/len out of range\n");
				sweet_log_sync();
			} else if (g_pcm_prepare() != 0) {
				printf("SWEET GLINK: vgraph: no shared memory\n");
				sweet_log_sync();
			} else {
				int rc;

				/* Why this is wrapped: if the DSP faulted on our region,
				 * the fault register is the only thing that will still say so. */
				g_smmu_faults("vgraph-pre");
				rc = g_apm_graph_open_shm(port, body + vgoff,
				    (uint32_t)vglen, 0, tag);
				g_smmu_faults("vgraph-post");
				if (rc == 0) {
					g_audio_graph_cfg_vp(port);
					if (g_apm_graph_start_list(port, sgs, 4) == 0) {
						printf("SWEET GLINK: vgraph %d started\n", vg);
						sweet_log_sync();
						g_audio_tone(port, G_VP_WR_IID);
					} else {
						printf("SWEET GLINK: vgraph %d did not start\n", vg);
						sweet_log_sync();
					}
					return;
				}
				printf("SWEET GLINK: vgraph %d open failed\n", vg);
				sweet_log_sync();
			}
		}
	}
	{
		int shmgraph = 0;

		(void)PE_parse_boot_argn("sweetshmgraph", &shmgraph,
		    sizeof(shmgraph));
		if (shmgraph && g_pcm_prepare() == 0 &&
		    g_apm_graph_open_shm(port, g_graph_vp_wr,
		    sizeof(g_graph_vp_wr), G_GRAPH_OFF,
		    "graph_open/vp-wr-shm") == 0) {
			g_audio_graph_cfg_vp(port);
			if (g_apm_graph_start(port, APM_VP_SG_ID) == 0) {
				printf("SWEET GLINK: shm graph started\n");
				sweet_log_sync();
				g_audio_tone(port, G_VP_WR_IID);
			} else {
				printf("SWEET GLINK: shm graph did not start\n");
				sweet_log_sync();
			}
			return;
		}
	}
	if (g_apm_cmd(port, 1, g_graph_vp_wr, sizeof(g_graph_vp_wr),
	    APM_CMD_GRAPH_OPEN, "graph_open/vp-wr") == 0) {
		g_pcm_prepare();
		g_audio_graph_cfg_vp(port);
		if (g_apm_graph_start(port, APM_VP_SG_ID) == 0) {
			printf("SWEET GLINK: vendor graph + WR input started\n");
			sweet_log_sync();
			g_audio_tone(port, G_VP_WR_IID);
		} else {
			printf("SWEET GLINK: vp-wr graph opened but did not start\n");
			sweet_log_sync();
		}
		return;
	}
	/* Control: the unmodified vendor graph.  It can open and start but
	 * has no module that accepts DATA_BUFFER_V2. */
	if (g_apm_cmd(port, 1, g_graph_vp, sizeof(g_graph_vp),
	    APM_CMD_GRAPH_OPEN, "graph_open/vp-inline") == 0) {
		g_audio_graph_cfg_vp(port);
		if (g_apm_graph_start(port, APM_VP_SG_ID) == 0) {
			printf("SWEET GLINK: vendor graph started\n");
			sweet_log_sync();
			g_audio_tone(port, G_VP_SAL_IID);
		} else {
			printf("SWEET GLINK: vendor graph opened but did not start\n");
			sweet_log_sync();
		}
		return;
	}
	/* Shared memory: map from the same client port that sends the
	 * command; a handle owned by another port may be rejected. */
	if (g_apm_shm_map(port) == 0) {
		if (g_apm_graph_open_shm(port, g_graph_vp,
		    sizeof(g_graph_vp), G_GRAPH_OFF,
		    "graph_open/vp-shm") == 0) {
			g_audio_graph_cfg_vp(port);
			if (g_apm_graph_start(port, APM_VP_SG_ID) == 0) {
				g_audio_tone(port, G_VP_SAL_IID);
			}
			return;
		}
	} else {
		printf("SWEET GLINK: no shared memory for the vendor graph\n");
	}

	/*
	 * The speakers are on the TDM lines, so the endpoint must be
	 * MODULE_ID_TDM_SINK, not CODEC_DMA_SINK, and the input must be the
	 * write-shared-memory endpoint.  Only the two module IDs change at
	 * fixed offsets in the serialised graph.
	 */
	memcpy(tdm_payload, g_graph_open_payload, sizeof(tdm_payload));
	g_put32(tdm_payload + G_INPUT_MODULE_OFF, G_WR_MODULE_ID);
	g_put32(tdm_payload + G_CDMA_MODULE_OFF, G_TDM_MODULE_ID);
	if (g_apm_cmd(port, 1, tdm_payload, sizeof(tdm_payload),
	    APM_CMD_GRAPH_OPEN, "graph_open/tdm-inline") == 0) {
		g_audio_graph_cfg_tdm(port);
		/* The PCM region was mapped before the vendor attempt. */
		if (g_apm_graph_start(port, APM_GRAPH_SG_ID) == 0) {
			g_audio_tone(port, G_SAL_IID);
		}
		return;
	}

	/* Inline (mainline style) keeps working as the control case. */
	if (g_apm_cmd(port, 1, g_graph_open_payload,
	    sizeof(g_graph_open_payload), APM_CMD_GRAPH_OPEN,
	    "graph_open/inline") == 0) {
		g_audio_graph_cfg(port);
		if (g_apm_graph_start(port, APM_GRAPH_SG_ID) == 0) {
			/* The tone needs a mapped region for the PCM buffers. */
			if (g_apm_shm_map(0x2002u) == 0) {
				g_audio_tone(port, G_SAL_IID);
			} else {
				printf("SWEET GLINK: no shared memory for PCM\n");
			}
		}
		return;
	}

	/* The vendor path: payload in mapped shared memory. */
	if (g_apm_shm_map(0x2002u) != 0) {
		printf("SWEET GLINK: graph_open failed (no shared memory)\n");
		return;
	}
	if (g_apm_graph_open_shm(port, g_graph_open_payload,
	    sizeof(g_graph_open_payload), G_GRAPH_OFF,
	    "graph_open/shm") == 0) {
		g_audio_graph_cfg(port);
		if (g_apm_graph_start(port, APM_GRAPH_SG_ID) == 0) {
			/* The tone needs a mapped region for the PCM buffers. */
			if (g_apm_shm_map(0x2002u) == 0) {
				g_audio_tone(port, G_SAL_IID);
			} else {
				printf("SWEET GLINK: no shared memory for PCM\n");
			}
		}
		return;
	}
	if (g_apm_graph_open_shm(port, g_graph_vendor_payload,
	    sizeof(g_graph_vendor_payload), G_GRAPH_OFF,
	    "graph_open/vendor") == 0) {
		g_apm_graph_start(port, APM_VENDOR_SG_ID);
		return;
	}
	printf("SWEET GLINK: graph_open failed on every payload\n");
}



/* One 24-byte battmgr GET_PROP round trip.  Layout from upstream
 * qcom_battmgr.c:
 *
 *   request  { hdr{owner,type,opcode}, battery_id, property, value }
 *   response { hdr{owner,type,opcode}, property, value, result }
 *
 * The property id lives at +16 in the request, not +12: putting it at
 * +12 makes the firmware read property 0 (BATT_STATUS) for every query,
 * which is why every earlier answer was "prop=0 value=1". */
/* Reclaim this channel's TX intents.  The ADSP does not always send
 * RX_DONE for a request, and a lost one would otherwise make the periodic
 * refresh fail forever.  Requests on this channel are synchronous, so no
 * other user of these intents can be in flight here. */
static void
g_batt_reclaim(void)
{
	int n = 0;

	for (int i = 0; i < g_nriids; i++) {
		if (g_riids[i].chan == g_batt_rcid && g_riids[i].in_use) {
			g_riids[i].in_use = 0;
			n++;
		}
	}
	if (n) {
		printf("SWEET BATT: reclaimed %d intent(s)\n", n);
	}
}

static int
g_batt_req(uint32_t op, uint32_t prop, const char *tag, int quiet)
{
	struct g_riid *r;
	uint8_t pkt[24];
	uint32_t v = 0, rprop = 0, result = 1;
	int got = 0;

	r = g_pick_riid(g_batt_rcid, 24);
	if (r == NULL) {
		g_batt_reclaim();
		r = g_pick_riid(g_batt_rcid, 24);
	}
	if (r == NULL) {
		printf("SWEET BATT: %s no intent\n", tag);
		return 0;
	}
	for (int i = 0; i < 24; i++) {
		pkt[i] = 0;
	}
	g_put32(pkt, 0x800au);		/* PMIC_GLINK_OWNER_BATTMGR */
	g_put32(pkt + 4, 1);		/* PMIC_GLINK_REQ_RESP */
	g_put32(pkt + 8, op);
	g_put32(pkt + 12, 0);		/* battery id */
	g_put32(pkt + 16, prop);	/* property */
	g_put32(pkt + 20, 0);		/* value (GET ignores it) */

	/* The first message after OPEN can be an unsolicited notification;
	 * retry until the opcode/property match this request. */
	for (int attempt = 0; attempt < 3 && !got; attempt++) {
		r->in_use = 1;
		g_batt_ready = 0;
		g_batt_fill = 0;
		g_send_tx_data_lcid(G_BATT_LCID, r->id, pkt, sizeof(pkt));
		for (unsigned i = 0; i < 60 && !g_batt_ready; i += 5) {
			g_drain(0);
			if (!g_batt_ready) {
				g_wait(5);
			}
		}
		if (!g_batt_ready || g_batt_rsp_len < 24) {
			break;
		}
		result = g_le32(g_batt_rsp + 20);
		rprop = g_le32(g_batt_rsp + 12);
		v = g_le32(g_batt_rsp + 16);
		if (g_le32(g_batt_rsp + 8) == op && rprop == prop &&
		    result == 0) {
			got = 1;
		} else {
			if (!quiet) {
				printf("SWEET BATT: %s ignored op=%#x prop=%u"
				    " result=%u\n", tag, g_le32(g_batt_rsp + 8),
				    rprop, result);
			}
		}
	}
	if (!got) {
		if (!quiet) {
			printf("SWEET BATT: %s op=%#x prop=%u no valid response\n",
			    tag, op, prop);
		}
		if (g_batt_rsp_len >= 24) {
			g_dump(tag, g_batt_rsp, g_batt_rsp_len);
		}
		return 0;
	}
	static int raw_shown;
	if (!quiet && raw_shown < 2) {
		raw_shown++;
		g_dump(tag, g_batt_rsp, g_batt_rsp_len);
	}
	if (op == 0x30u && prop == 0) {
		g_batt_s_status = (int)v;
	} else if (op == 0x30u && prop == 4) {
		/* battmgr reports capacity in 0.01 % units (10000 = 100.00 %),
		 * not whole percent: without the /100 a 50 % battery reads as
		 * 5000 and SweetBattery clamps it to 100 %. */
		g_batt_s_cap = (int)(v / 100u);
	} else if (op == 0x30u && prop == 7) {
		g_batt_s_mv = (int)(v / 1000u);
	} else if (op == 0x30u && prop == 9) {
		/* Negative = charging, in microamps, two's complement. */
		g_batt_s_ma = (int)((int32_t)v / 1000);
	} else if (op == 0x32u && prop == 0) {
		g_batt_s_usb = (int)v;
	}
	if (!quiet) {
		printf("SWEET BATT: %s op=%#x prop=%u value=%u\n", tag, op,
		    rprop, v);
	}
	return 1;
}

/*
 * Periodic refresh: the initial probe is one-shot, so without this the
 * cached capacity never changes and iOS shows the boot-time value forever.
 * SweetBattery polls every 30 s; ask battmgr again on the same cadence.
 */
static void
g_batt_tick(void)
{
	static int tick_n;
	int ok = 0;

	g_drain(0);
	ok += g_batt_req(0x30u, 0, "status", 1);
	ok += g_batt_req(0x30u, 4, "capacity", 1);
	ok += g_batt_req(0x30u, 7, "vbat", 1);
	ok += g_batt_req(0x30u, 9, "ibat", 1);
	ok += g_batt_req(0x32u, 0, "usb", 1);
	/* Poll every 3 s for a quick charger reaction; print every tenth
	 * tick (30 s) so the ring does not fill with identical lines. */
	tick_n++;
	if (tick_n % 6 == 1) {
		printf("SWEET BATT: tick %d ok=%d/5 cap=%d%% %dmV %dmA "
		    "status=%d usb=%d\n", tick_n, ok, g_batt_s_cap,
		    g_batt_s_mv, g_batt_s_ma, g_batt_s_status, g_batt_s_usb);
		sweet_log_sync();
	}
}

static void
g_batt_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	uint32_t wait_ms = 5000;	/* 5 s; the UI timer refreshes at 3 s */

	for (;;) {
		uint64_t deadline;

		clock_interval_to_deadline(wait_ms, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&g_batt_chan, THREAD_UNINT,
		    deadline);
		thread_block(THREAD_CONTINUE_NULL);
		if (g_batt_rcid != 0) {
			g_batt_tick();
		}
		wait_ms = 5000;
	}
}

/*
 * Last battery sample for SweetBattery.  Returns 0 until the pmic_glink
 * probe has both a status and a capacity, so iOS keeps using its own
 * fallback (battery-only, no fake AC) when this path is off.
 */
int
sweet_batt_get(unsigned *percent, unsigned *mv, int *ma, unsigned *status,
    unsigned *usb)
{
	if (g_batt_s_status < 0 || g_batt_s_cap < 0) {
		return 0;
	}
	*percent = (unsigned)g_batt_s_cap;
	*mv = g_batt_s_mv < 0 ? 0 : (unsigned)g_batt_s_mv;
	*ma = g_batt_s_ma;
	*status = (unsigned)g_batt_s_status;
	*usb = g_batt_s_usb < 0 ? 0 : (unsigned)g_batt_s_usb;
	return 1;
}

/*
 * Optional battery read-out.  sweetbatt=1 opens our side of
 * PMIC_RTR_ADSP_APPS, answers the ADSP's own OPEN, and asks the battmgr
 * firmware for the battery properties.  Read-only: nothing here changes a
 * charger setting.
 */
static void
g_batt_probe(void)
{
	uint8_t name[G_BATT_NAME_LEN];
	uint8_t ib[8];
	int on = 0;

	(void)PE_parse_boot_argn("sweetbatt", &on, sizeof(on));
	if (!on) {
		return;
	}
	printf("SWEET BATT: opening %s\n", G_BATT_NAME);
	for (unsigned i = 0; i < G_BATT_NAME_LEN; i++) {
		name[i] = (uint8_t)G_BATT_NAME[i];
	}
	g_send(CMD_OPEN, (uint16_t)G_BATT_LCID, G_BATT_NAME_LEN, name,
	    G_BATT_NAME_LEN);
	for (unsigned i = 0; i < 2000 && !(g_batt_acked && g_batt_remote);
	    i += 5) {
		g_drain(0);
		if (!(g_batt_acked && g_batt_remote)) {
			g_wait(5);
		}
	}
	printf("SWEET BATT: ack=%d remote=%d rcid=%u\n", g_batt_acked,
	    g_batt_remote, g_batt_rcid);
	if (!g_batt_acked || !g_batt_remote) {
		return;
	}
	g_send_open_ack(g_batt_rcid);
	g_put32(ib, 128);
	g_put32(ib + 4, G_BATT_LIID);
	g_send(CMD_INTENT, (uint16_t)G_BATT_LCID, 1, ib, 8);
	for (unsigned i = 0; i < 2000; i += 5) {
		g_drain(0);
		if (g_pick_riid(g_batt_rcid, 24) != NULL) {
			break;
		}
		g_wait(5);
	}
	if (g_pick_riid(g_batt_rcid, 24) == NULL) {
		printf("SWEET BATT: no pmic intent (rcid=%u)\n", g_batt_rcid);
		return;
	}
	/* enum battery_property_id from qti_battery_charger.c, and the
	 * BC_BATTERY_STATUS_GET / BC_USB_STATUS_GET opcodes. */
	(void)g_batt_req(0x30u, 0, "batt-status", 0);
	(void)g_batt_req(0x30u, 4, "capacity", 0);
	(void)g_batt_req(0x30u, 7, "vbat", 0);
	(void)g_batt_req(0x30u, 9, "ibat", 0);
	(void)g_batt_req(0x30u, 12, "temp", 0);
	(void)g_batt_req(0x32u, 0, "usb-online", 0);
	/* The worker's first request is seconds away; if the channel dies
	 * right after the probe, this tells us in the same boot. */
	printf("SWEET BATT: immediate re-check\n");
	(void)g_batt_req(0x30u, 4, "capacity-now", 0);
	{
		thread_t th;

		if (kernel_thread_start(g_batt_thread, NULL, &th) ==
		    KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
	sweet_log_sync();
}

/*
 * ---- SMP scheduling sampler ----
 *
 * Starting CPU1 is only half the question; the other half is whether the
 * scheduler actually gives it work.  A kernel thread samples every
 * processor's active_thread != idle_thread once per interval and prints a
 * per-CPU busy count, then resets the SoC so the ring can be pulled from
 * Android/TWRP.  Built for the multi-core bring-up:
 *
 *   KEEPALIVE=1 CPUS=4 ... sweetsmp=1 sweetsmpn=20 sweetsmpms=1000
 */
/* osfmk/kern/processor.c exports these: processor.h is MACH_KERNEL_PRIVATE,
 * so a BSD file cannot read processor_array[] or struct processor itself. */
#define SWEET_MAX_CPUS 64
extern int sweet_cpu_busy(int cpu);
extern uint64_t sweet_cpu_tid(int cpu);

static int      g_smp_wait;
static uint32_t g_smp_busy[SWEET_MAX_CPUS];
static uint64_t g_smp_tid[SWEET_MAX_CPUS];
static uint32_t g_smp_interval_ms = 1000;
static int      g_smp_samples = 20;

static void
sweet_smp_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;

	for (int n = 0; n < g_smp_samples; n++) {
		unsigned int ncpu = ml_get_cpu_count();
		uint64_t self = thread_tid(current_thread());

		if (ncpu > SWEET_MAX_CPUS) {
			ncpu = SWEET_MAX_CPUS;
		}
		for (unsigned int i = 0; i < ncpu; i++) {
			uint64_t tid;

			if (sweet_cpu_busy((int)i) != 1) {
				continue;
			}
			tid = sweet_cpu_tid((int)i);
			if (tid != 0 && tid != self) {
				g_smp_busy[i]++;
				g_smp_tid[i] = tid;
			}
		}
		assert_wait_timeout(&g_smp_wait, THREAD_INTERRUPTIBLE,
		    g_smp_interval_ms, NSEC_PER_MSEC);
		(void)thread_block(THREAD_CONTINUE_NULL);
	}

	printf("SWEET SMP: ncpu=%u samples=%d interval=%ums\n",
	    ml_get_cpu_count(), g_smp_samples, g_smp_interval_ms);
	for (unsigned int i = 0; i < ml_get_cpu_count() &&
	    i < SWEET_MAX_CPUS; i++) {
		printf("SWEET SMP: cpu%u busy=%u/%d last_tid=%llu\n", i,
		    g_smp_busy[i], g_smp_samples,
		    (unsigned long long)g_smp_tid[i]);
	}
	printf("SWEET SMP: done, resetting\n");
	sweet_log_sync();

	{
		register uint64_t x0 __asm("x0") = 0x84000009ull;

		__asm volatile("dsb sy" ::: "memory");
		__asm volatile("smc #0" : "+r"(x0) ::
		    "x1", "x2", "x3", "memory");
	}
	for (;;) {
		delay(1000);
	}
}

static void
sweet_smp_start(void)
{
	int en = 0, n = 20, ms = 1000;
	thread_t th;

	(void)PE_parse_boot_argn("sweetsmp", &en, sizeof(en));
	if (!en) {
		return;
	}
	(void)PE_parse_boot_argn("sweetsmpn", &n, sizeof(n));
	(void)PE_parse_boot_argn("sweetsmpms", &ms, sizeof(ms));
	if (n > 0 && n < 1000) {
		g_smp_samples = n;
	}
	if (ms >= 100 && ms <= 10000) {
		g_smp_interval_ms = (uint32_t)ms;
	}
	if (kernel_thread_start((thread_continue_t)sweet_smp_thread, NULL,
	    &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		printf("SWEET SMP: sampler ncpu=%u n=%d ms=%d\n",
		    ml_get_cpu_count(), g_smp_samples, g_smp_interval_ms);
	} else {
		printf("SWEET SMP: kernel_thread_start failed\n");
	}
	sweet_log_sync();
}

void
sweet_glink_probe(void)
{
	int enabled = 0;
	int rc;

	g_cmddb_dump();
	/* Snapshot first: everything after this may write. */
	g_gpu_snapshot();
	/* SPMI rail probe next: g_gpu_probe resets the SoC when it is done. */
	g_rail_probe();
	/* Independent of the audio stack; gated by sweetgpu=1. */
	g_gpu_probe();
	/* Kernel-side GMU bring-up; gated by sweetgmuboot=1. */
	g_gmu_boot();
	/* Independent too: gated by sweetsmp=1. */
	sweet_smp_start();

	(void)PE_parse_boot_argn("sweetshmhi", &g_shm_hi, sizeof(g_shm_hi));
	if (g_shm_hi) {
		printf("SWEET GLINK: shm high word %#x\n", g_shm_hi);
	}

	(void)PE_parse_boot_argn("sweetglink", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET GLINK: disabled (sweetglink=1)\n");
		return;
	}
	printf("SWEET GLINK: ===== minimal GLINK/GPR client =====\n");
	/* Build tag: watchers key on this so an intermediate kernel cannot be
	 * mistaken for the one that was just pushed.  Bump it every change. */
	printf("SWEET AUDIO: build v97 (battery refresh worker)\n");

	/* RTC first, then haptics; dumps afterwards in case one NAKs.
	 * The boot buzz is opt-in now (sweethap=1); normal boots stay quiet. */
	g_clock_from_bootarg();
	{
		int hap = 0;

		(void)PE_parse_boot_argn("sweethap", &hap, sizeof(hap));
		if (hap) {
			g_haptics_try();
		}
	}
	g_rtc_set_from_pmic();
	g_spmi_dump("rtc", 0, 0x6100, 16);
	g_spmi_dump("hap", 3, 0xf000, 16);
	g_spmi_dump("hap", 3, 0xf100, 16);
	/* 0x151e9000 (lpass_tbu) faults from XNU; do not touch it again. */
	g_mmio_dump("apps_smmu", 0x15000000u, 8);

	if (!g_smem_init()) {
		goto out;
	}
	if (g_tx_len > 0x10000u || g_rx_len > 0x10000u) {
		printf("SWEET GLINK: fifo size out of range\n");
		goto out;
	}

	g_ipcc = g_win_alloc(PAGE_SIZE);
	if (g_ipcc != 0) {
		g_ipcc = g_win_map(g_ipcc, G_IPCC_PA, 0x100);
	}

	for (int i = 0; i < G_NINTENTS; i++) {
		g_liids[i].id = (uint32_t)(i + 1);
		g_liids[i].size = G_INTENT_SIZE;
	}
	g_nliids = G_NINTENTS;

	/* Version negotiation. */
	g_send_version();
	rc = g_wait_flag(&g_ver_acked, 2000);
	printf("SWEET GLINK: version %s\n", rc ? "ok" : "TIMEOUT");

	/* Open adsp_apps. */
	g_send_open();
	for (unsigned i = 0; i < 400; i += 5) {
		g_drain(0);
		if (g_open_acked && g_remote_open) {
			break;
		}
		g_wait(5);
	}
	printf("SWEET GLINK: open ack=%d remote_open=%d rcid=%u\n",
	    g_open_acked, g_remote_open, g_rcid);
	if (g_open_acked && g_remote_open) {
		g_send_open_ack(g_rcid);
		printf("SWEET GLINK: OPEN_ACK sent for rcid=%u\n", g_rcid);
		for (int i = 0; i < g_nliids; i++) {
			g_advertise_intent(g_liids[i].id, g_liids[i].size);
		}
		printf("SWEET GLINK: %d intents advertised\n", g_nliids);
		g_drain(100);
		g_batt_probe();
	} else {
		printf("SWEET GLINK: channel did not open, skipping PRM\n");
		goto out;
	}

	/* Cast the PRM vote and look at the reply. */
	sweet_log_sync();
	g_prm_vote_lpass();
	for (unsigned i = 0; i < 3000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (g_rsp_ready) {
		g_dump("PRM response", g_rsp, g_rsp_len);
		if (g_rsp_len >= 32) {
			printf("SWEET GLINK: resp opcode %#x status %#x\n",
			    g_le32(g_rsp + 24), g_le32(g_rsp + 28));
		}
	} else {
		printf("SWEET GLINK: no PRM response\n");
	}
	g_drain(100);

	/* Second opinion: ask the APM core for its state. */
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_apm_state_query();
	for (unsigned i = 0; i < 3000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
	}
	if (g_rsp_ready) {
		g_dump("APM response", g_rsp, g_rsp_len);
	} else {
		printf("SWEET GLINK: no APM response\n");
	}
	g_drain(100);

	/* Clocks the AP needs before it may read inside those macros. */
	{
		int va_clk, rx_clk, ampfirst = 0;

		sweet_log_sync();
		va_clk = g_prm_vote_clock(0x307, 19200000, 2);   /* VA_CORE_MCLK */
		sweet_log_sync();
		rx_clk = g_prm_vote_clock(0x30e, 22579200, 5);   /* RX_CORE_MCLK */

		g_drain(100);
		printf("SWEET GLINK: clocks va=%d rx=%d\n", va_clk, rx_clk);
		/* 0242: pre-stream half of the clock-path witness. */
		g_clk_witness("pre");
		/* 0237: the pre-stream half of the paired RX-macro read. */
		if (rx_clk == 0) {
			g_rx_snapshot("pre-stream");
		}
		/*
		 * sweetampfirst=1 configures the AW882xx PAs BEFORE the graph is
		 * opened and the tone is fed.  In every run so far the order was
		 * g_apm_graph_bringup() -> sweet_i2c_probe_amps(): by the time the PA
		 * got its vendor Music profile the tone loop had already sent all of
		 * its buffers, so no boot could have produced sound no matter which
		 * graph opened.  The GENI branch inside probe_amps() only runs on the
		 * SECOND call (patch 0124's early probe is call 1 and deliberately
		 * skips GENI before UFS is mounted), so this call is the one that
		 * actually programs the PA; the later call is skipped to keep the
		 * number of GENI passes at one.
		 */
		(void)PE_parse_boot_argn("sweetampfirst", &ampfirst,
		    sizeof(ampfirst));
		if (ampfirst) {
			printf("SWEET MARK: ampfirst before bringup\n");
			sweet_log_sync();
			sweet_i2c_probe_amps();
			/*
			 * 0236: with sweetampverify=1 take a snapshot here too, before the
			 * graph is opened.  Two snapshots inside the SAME boot turn the
			 * read-back into a paired measurement - a register that differs
			 * between pre-stream and post-stream is attributable to the
			 * stream, which a single snapshot can never decide.  Read-only;
			 * the cost is eight GENI transfers.
			 */
			{
				int ampverify_pre = 0;

				(void)PE_parse_boot_argn("sweetampverify",
				    &ampverify_pre, sizeof(ampverify_pre));
				/*
				 * 0248: sweetexp bit2 is the same instrument, because the
				 * boot-args field has 590 bytes and this boot must change
				 * exactly one thing (the AW payload).  bit0/bit1 were taken
				 * by the IBAT burst and the audio-service probe.
				 */
				if (!ampverify_pre) {
					int exp2 = 0;

					(void)PE_parse_boot_argn("sweetexp",
					    &exp2, sizeof(exp2));
					if (exp2 & 4) {
						ampverify_pre = 1;
					}
				}
				if (ampverify_pre) {
					sweet_i2c_amp_readback("pre-stream");
				}
			}
		}
		g_apm_graph_bringup();
		sweet_log_sync();
		if (!ampfirst) {
			sweet_i2c_probe_amps();
		}
		/*
		 * sweetampverify=1 reads the PA back over the same GENI path that
		 * already reads its device id.  Two reasons this is the witness to
		 * chase: it does not depend on the GLINK reply at all, and
		 * "profile 44/44" up in sweet_i2c.c is only a transmit count - a
		 * read-back is the first real proof the PA holds what we wrote.
		 * Run AFTER bringup(), so the graph is started and the DATA_BUFFER
		 * loop has been fed: a value that differs from the ACF table then
		 * says something drove the PA, which is not the same as saying it
		 * made sound.
		 */
		{
			int ampverify = 0;

			(void)PE_parse_boot_argn("sweetampverify", &ampverify,
			    sizeof(ampverify));
			if (!ampverify) {
				int exp2 = 0;

				(void)PE_parse_boot_argn("sweetexp", &exp2,
				    sizeof(exp2));
				if (exp2 & 4) {
					ampverify = 1;
				}
			}
			if (ampverify) {
				sweet_i2c_amp_readback("post-stream");
			}
		}
		if (rx_clk == 0) {
			g_rx_snapshot("post-stream");
		}
		/* 0242: did the clock path move once the stream ran? */
		g_clk_witness("post");
		g_lpass_probe(va_clk == 0, rx_clk == 0);
	}


out:
	printf("SWEET GLINK: ===== done =====\n");
	{
		int rb = 0;

		(void)PE_parse_boot_argn("sweetreboot", &rb, sizeof(rb));
		if (rb) {
			/* Boot arg sweetreboot=1: reset the SoC so the next
			 * ring pull does not need a human with a power
			 * button.  smc #0 is already used by the SCM path. */
			printf("SWEET GLINK: PSCI SYSTEM_RESET\n");
			sweet_log_sync();
			{
				register uint64_t x0 __asm("x0") = 0x84000009ull;

				__asm volatile("dsb sy" ::: "memory");
				__asm volatile("smc #0" : "+r"(x0) ::
				    "x1", "x2", "x3", "memory");
			}
			for (;;) {
				delay(1000);
			}
		}
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
