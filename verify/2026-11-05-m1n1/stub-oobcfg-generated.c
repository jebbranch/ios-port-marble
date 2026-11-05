/* host stub for the oobcfg rounds - not kernel code */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned long vm_offset_t;

#define G_INTENT_SIZE           1024u
#define GPR_DOMAIN_ADSP         2u
#define GPR_DOMAIN_APPS         3u
#define APM_CMD_SET_CFG         0x01001006u
#define STUB_STRIDE             72u

static vm_offset_t g_shm_va;
static uint32_t    g_shm_handle = 0xb0e145f8u;
static int         g_shm_mapped = 1;
static uint64_t    g_map_pa __attribute__((unused)) = 0;
static uint8_t     region[0x5000];
static uint8_t     g_rsp[G_INTENT_SIZE];
static uint32_t    g_rsp_len;
static int         g_rsp_ready;

/* the emulated DSP: does it write an answer into every record's error_code slot,
 * does it answer at all, and with what status */
static int      dsp_writes = 1;
static int      dsp_answers = 1;
static uint32_t dsp_code = 0;
static uint32_t dsp_status = 1;

static uint32_t
g_shm_region_size(void)
{
	return 0x5000u;
}

static void
g_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t
g_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
g_smmu_faults(const char *tag)
{
	(void)tag;
}

static void
sweet_log_sync(void)
{
	fflush(stdout);
}

__attribute__((unused)) static void
g_wait(unsigned ms)
{
	(void)ms;
}

__attribute__((unused)) static void
g_drain(unsigned budget_ms)
{
	(void)budget_ms;
}

/* one exchange, shared by both senders */
static void
dsp_answer(uint8_t *where, uint32_t nbytes)
{
	uint32_t i;

	if (dsp_answers) {
		g_put32(g_rsp + 20, 0x80u);
		g_put32(g_rsp + 24, APM_CMD_SET_CFG);
		g_put32(g_rsp + 28, dsp_status);
		g_rsp_len = 32;
		g_rsp_ready = 1;
	}
	if (!dsp_writes) {
		return;
	}
	for (i = 0; i + STUB_STRIDE <= nbytes; i += STUB_STRIDE) {
		g_put32(where + i + 12, dsp_code);
	}
}

/* out-of-band: the only copy the DSP can reach is the mapped region */
__attribute__((unused)) static void
g_send_gpr(const uint8_t *pkt, uint32_t len)
{
	uint32_t addr, handle, size;

	if (len != 40u || g_le32(pkt + 16) != 0x400u ||
	    g_le32(pkt + 20) != APM_CMD_SET_CFG) {
		printf("STUB !! descriptor wrong: len=%u w4=%#x w5=%#x\n",
		    len, g_le32(pkt + 16), g_le32(pkt + 20));
		return;
	}
	addr = g_le32(pkt + 24);
	handle = g_le32(pkt + 32);
	size = g_le32(pkt + 36);
	printf("STUB send oob len=%u w0=%#x w1=%#x op=%#x addr=%#x handle=%#x "
	    "size=%u\n", len, g_le32(pkt + 0), g_le32(pkt + 4),
	    g_le32(pkt + 20), addr, handle, size);
	dsp_answer(region + addr, size);
}

/* in-band: the payload is the caller's buffer */
__attribute__((unused)) static int
g_apm_cmd(uint32_t src_port, uint32_t dst_port, const uint8_t *payload,
    uint32_t plen, uint32_t opcode, const char *what)
{
	uint8_t *b = (uint8_t *)(uintptr_t)payload;

	printf("STUB send src=%#x dst=%#x plen=%u op=%#x what=%s inband=%s\n",
	    src_port, dst_port, plen, opcode, what, plen > 256u ? "no" : "yes");
	dsp_answer(b, plen);
	return (0);
}

static const uint32_t g_oob_offs[] = { 0x1000u };
#define SWEET_OOB_FORMS 1		/* 0 = bare offset, 1 = base + offset */
#define SWEET_OOB_RECS  4
#define SWEET_OOB_STRIDE 72		/* 16-byte wire header + align8(52) */
/*
 * L1 and L2 closed the transport and length axes: the same container is refused
 * 0x1 out-of-band (K1) and in-band at 288 and at 216 bytes.  N1 goes back to
 * K1's send - out-of-band, four records, 288 bytes - because the one thing that
 * still cannot be read is the per-record error_code, and in-band the payload
 * lives inside the ADSP-side packet, so the DSP has no way to write our body.
 * Only the out-of-band side can produce a non-zero read.  K1 is the control.
 */
#define SWEET_OOB_SEND  4

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
		printf("SWEET MARK: oobcfg v11 no mapping\n");
		return (-1);
	}
	memset(body, 0, sizeof(body));
	memset(pkt, 0, sizeof(pkt));
	g_smmu_faults("oobcfg-pre");
	for (i = 0; i < SWEET_OOB_SEND; i++) {
		g_put32(body + off + 0, g_oob_miids[i]);
		g_put32(body + off + 4, 0x08001004u);
		g_put32(body + off + 8, 52);
		g_put32(body + off + 12, 0xa5a5a5a5u);	/* error_code, pre-set */
		for (j = 0; j < 13; j++) {
			g_put32(body + off + 16 + j * 4, g_h_payload[i][j]);
		}
		off += SWEET_OOB_STRIDE;
	}
	/*
	 * N1 round: the send below is K1's (0260's landed state, chain-tree
	 * 021bb1e) restored verbatim apart from the marker and the pre-set
	 * error_code, so K1 is the control and the sentinel is the only variable.
	 */
	for (t = 0; t < sizeof(g_oob_offs) / sizeof(g_oob_offs[0]); t++) {
		const uint32_t oob_off = g_oob_offs[t];
		const int in_bounds = (oob_off + off <= g_shm_region_size());
		uint32_t f;

		if (!in_bounds) {
			printf("SWEET MARK: oobcfg v11 off=%#x is past the declared region %#x, body not written\n",
			    oob_off, g_shm_region_size());
		} else {
			memcpy((void *)(g_shm_va + oob_off), body, off);
		}
		__asm__ volatile("dsb sy" ::: "memory");

		for (f = 0; f < SWEET_OOB_FORMS; f++) {
			uint64_t addr = (f == 0) ? (uint64_t)oob_off :
			    g_map_pa + (uint64_t)oob_off;

			if (f == 1 && g_map_pa == 0) {
				printf("SWEET MARK: oobcfg v11 try %u no region base, abs form skipped\n",
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
			printf("SWEET MARK: oobcfg v11 try %u form=%s off=%#x addr=%#llx len=%u recs=%u handle=%#x\n",
			    t * SWEET_OOB_FORMS + f, f == 0 ? "bare" : "abs",
			    oob_off, addr, off, SWEET_OOB_SEND, g_shm_handle);
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
				printf("SWEET MARK: oobcfg v11 recs not read back (past the mapping)\n");
				sweet_log_sync();
				continue;
			}
			for (i = 0; i < SWEET_OOB_SEND; i++) {
				const uint8_t *rec = (const uint8_t *)g_shm_va +
				    oob_off + i * SWEET_OOB_STRIDE;

				printf("SWEET MARK: oobcfg v11 rec %u iid=%#x param=%#x size=%u err=%#x\n",
				    i, g_le32(rec + 0), g_le32(rec + 4),
				    g_le32(rec + 8), g_le32(rec + 12));
			}
			/*
			 * error_code read back zero on every probe boot, which could not
			 * distinguish "the DSP never wrote the body" from "it wrote zero".
			 * The field now starts at 0xa5a5a5a5, a value no answer can be, so
			 * touched=0 is positive evidence of no write-back and a non-zero
			 * count names the first changed byte.  Reads nothing extra.
			 */
			{
				const uint8_t *region = (const uint8_t *)g_shm_va + oob_off;
				uint32_t k;
				uint32_t touched = 0;
				uint32_t first = 0;

				for (k = 0; k < off; k++) {
					if (body[k] == region[k]) {
						continue;
					}
					if (touched == 0) {
						first = k;
					}
					touched++;
				}
				printf("SWEET MARK: oobcfg v11 body touched=%u first=%u of %u\n",
				    touched, first, off);
			}
			sweet_log_sync();
		}
	}
	return (0);
}

int
main(int argc, char **argv)
{
	const char *mode = (argc > 1) ? argv[1] : "write-zero";

	if (strcmp(mode, "silent") == 0) {
		dsp_writes = 0;
	} else if (strcmp(mode, "norsp") == 0) {
		dsp_answers = 0;
		dsp_writes = 0;
	} else if (strcmp(mode, "accept") == 0) {
		dsp_status = 0;
	} else if (strcmp(mode, "write-zero") != 0) {
		printf("STUB unknown mode %s\n", mode);
		return (2);
	}
	g_shm_va = (vm_offset_t)(uintptr_t)region;
	memset(region, 0xa5, sizeof(region));
	printf("STUB mode=%s writes=%d answers=%d code=%#x status=%#x\n",
	    mode, dsp_writes, dsp_answers, dsp_code, dsp_status);
	return (g_apm_set_cfg_oob(0x2010u, "oob_batch") == 0 ? 0 : 1);
}
