#!/usr/bin/env python3
"""Build and run the host stub for L1 (patch 0261), from the post-apply file.

The code we are about to boot has never seen a real compiler - run 387 skipped
0261, so its kernel was K1's.  This extracts the reconstructed function verbatim
and runs it against an emulated DSP in both branches the gates care about.
"""
import subprocess
import os
import re
import sys
import pathlib

SRC = pathlib.Path(os.environ.get("STUB_SRC", "/tmp/v0261/bsd/dev/sweet_glink.c"))
print(f"extracting from {SRC}")
lines = SRC.read_text().splitlines(True)

# 1-based ranges: g_oob_offs .. end of g_apm_set_cfg_oob, verified by the markers
start = next(i for i, l in enumerate(lines) if l.startswith("static const uint32_t g_oob_offs[]"))
defs = [i for i, l in enumerate(lines) if i > start and re.match(r"^g_[a-z_]+\(", l)]
# the first hit is g_apm_set_cfg_oob's own definition line, which we want in
end = defs[1]
body = "".join(lines[start:end])
assert "g_apm_set_cfg_oob" in body and body.count("oobcfg v") >= 5, "extraction window wrong"
# trim back to the end of g_apm_set_cfg_oob (the next function's comment block)
cut = body.rindex("\n}\n") + 3
body = body[:cut]

head = r"""/* host stub for the L1 round - not kernel code */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned long vm_offset_t;

static vm_offset_t g_shm_va;
static uint32_t    g_shm_handle = 0xb0e145f8u;
static int         g_shm_mapped = 1;
static uint8_t     region[0x5000];

#define APM_CMD_SET_CFG 0x01001006u
/* 1 = the DSP writes an error_code into every record, 0 = it never touches it */
static int dsp_writes = 1;
static uint32_t dsp_code = 3;

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

static int
g_apm_cmd(uint32_t src_port, uint32_t dst_port, const uint8_t *payload,
    uint32_t plen, uint32_t opcode, const char *what)
{
	/* in-band: the payload rides in the packet, so a DSP that writes error
	 * codes has to write into the caller's buffer, which is what dsp_writes
	 * models.  plen is what the 256-byte threshold actually sees. */
	printf("STUB send src=%#x dst=%#x plen=%u op=%#x what=%s inband=%s\n",
	    src_port, dst_port, plen, opcode, what,
	    plen > 256u ? "no" : "yes");
	if (dsp_writes) {
		uint8_t *b = (uint8_t *)(uintptr_t)payload;
		uint32_t i;

		for (i = 0; i + 72u <= plen; i += 72u) {
			g_put32(b + i + 12, dsp_code);
		}
	}
	return (0);
}

"""

main = r"""
int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "silent") == 0) {
		dsp_writes = 0;
	}
	if (argc > 1 && strcmp(argv[1], "accept") == 0) {
		dsp_code = 0;
	}
	g_shm_va = (vm_offset_t)(uintptr_t)region;
	memset(region, 0xa5, sizeof(region));
	printf("STUB %s\n", dsp_writes ? "dsp writes" : "dsp silent");
	return (g_apm_set_cfg_oob(0x2010u, "oob_batch") == 0 ? 0 : 1);
}
"""

out = pathlib.Path("/tmp/stub0261.c")
out.write_text(head + body + main)

cc = ["cc", "-Wall", "-Wextra", "-Wformat=2", "-Werror", "-o", "/tmp/stub0261", str(out)]
r = subprocess.run(cc, capture_output=True, text=True)
print(r.stdout, r.stderr)
if r.returncode != 0:
    sys.exit(f"STUB COMPILE FAILED rc={r.returncode}")
print("STUB COMPILE OK (-Wall -Wextra -Wformat=2 -Werror)")
for mode in ("dsp-writes", "silent", "accept"):
    args = ["/tmp/stub0261"] + ([] if mode == "dsp-writes" else [mode])
    p = subprocess.run(args, capture_output=True, text=True)
    print(f"===== run {mode} rc={p.returncode} =====")
    print(p.stdout, end="")
    if p.stderr:
        print("STDERR:", p.stderr[:500])
