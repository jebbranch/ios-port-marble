#!/usr/bin/env python3
"""Build and run the host stub for the oobcfg rounds (0260 K1 ... 0264 N1).

The code we are about to boot has never seen a real compiler - run 387 skipped
0261, so its kernel was K1's.  This extracts g_apm_set_cfg_oob verbatim from the
file a patch produces and runs it against an emulated DSP in the branches the
gates care about:

    STUB_SRC=/tmp/n1base/bsd/dev/sweet_glink.c python3 tools/stub_oobcfg.py

Which sender the extracted code uses decides the model.  Out of band (K1, N1) the
body only exists in the mapped region, so a DSP that answers touches the region
copy and the read-back can really change.  In band (L1, L2) the payload rides in
the packet, so the same emulation always reports touched=0 - which is why N1 went
back to the out-of-band send.
"""
import subprocess
import os
import re
import sys
import pathlib

SRC = pathlib.Path(os.environ.get("STUB_SRC", "/tmp/n1base/bsd/dev/sweet_glink.c"))
print(f"extracting from {SRC}")
lines = SRC.read_text().splitlines(True)

# ranges: g_oob_offs .. end of g_apm_set_cfg_oob, verified by the markers
start = next(i for i, l in enumerate(lines) if l.startswith("static const uint32_t g_oob_offs[]"))
defs = [i for i, l in enumerate(lines) if i > start and re.match(r"^g_[a-z_]+\(", l)]
# the first hit is g_apm_set_cfg_oob's own definition line, which we want in
end = defs[1]
body = "".join(lines[start:end])
assert "g_apm_set_cfg_oob" in body and body.count("oobcfg v") >= 5, "extraction window wrong"
# trim back to the end of g_apm_set_cfg_oob (the next function's comment block)
cut = body.rindex("\n}\n") + 3
body = body[:cut]

oob = "g_send_gpr" in body
marker = re.search(r"oobcfg v\d+", body).group(0)
sender = "out-of-band descriptor" if oob else "in-band g_apm_cmd"
print(f"extracted {len(body.splitlines())} lines, marker {marker}, sender {sender}")
sent = re.search(r"#define SWEET_OOB_SEND\s+(\d+)", body)
print("SWEET_OOB_SEND = " + (sent.group(1) if sent else "absent"))

head = r"""/* host stub for the oobcfg rounds - not kernel code */
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

"""

main = r"""
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
"""

out = pathlib.Path("/tmp/stub_oobcfg.c")
out.write_text(head + body + main)

cc = ["cc", "-Wall", "-Wextra", "-Wformat=2", "-Werror", "-o", "/tmp/stub_oobcfg",
      str(out)]
r = subprocess.run(cc, capture_output=True, text=True)
print(r.stdout, r.stderr)
if r.returncode != 0:
    sys.exit(f"STUB COMPILE FAILED rc={r.returncode}")
print("STUB COMPILE OK (-Wall -Wextra -Wformat=2 -Werror)")
for mode in ("write-zero", "silent", "norsp", "accept"):
    p = subprocess.run(["/tmp/stub_oobcfg", mode], capture_output=True, text=True)
    print(f"===== run {mode} rc={p.returncode} =====")
    print(p.stdout, end="")
    if p.stderr:
        print("STDERR:", p.stderr[:500])
