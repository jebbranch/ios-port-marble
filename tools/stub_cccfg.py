#!/usr/bin/env python3
"""Compile and run the sweetcccfg selector block on the host, for sel = 1..4.

The code we are about to boot has never seen a compiler.  This lifts the block
out of the tree verbatim, wraps it in the handful of stubs it needs, builds it
with -Wall -Wextra -Wformat=2 -Werror, and then prints the exact bytes each
selector puts on the wire - so the packet geometry the gates quote (80 B body /
120 B packet for C, 64 B / 104 B for the MFC pair) is measured rather than
asserted.

  STUB_SRC=/path/to/bsd/dev/sweet_glink.c python3 tools/stub_cccfg.py
"""
import os
import pathlib
import re
import subprocess
import sys

SRC = pathlib.Path(os.environ.get("STUB_SRC",
    str(pathlib.Path(__file__).resolve().parents[2] / "chain-tree" / "bsd" / "dev" / "sweet_glink.c")))
print(f"extracting from {SRC}")
src = SRC.read_text()

start = src.index("sweetcccfg=<n> only")
start = src.rindex("/*", 0, start)
end = src.index("\n\t}\n", start) + 4          # close of the selector block
body = src[start:end]
assert "PE_parse_boot_argn(\"sweetcccfg\"" in body and "g_apm_set_cfg_batch" in body, \
    "extraction window wrong"
assert body.count("cccfg v3") >= 1, "marker not in the extracted block"
print(f"extracted {len(body.splitlines())} lines, marker occurrences: "
      f"{len(re.findall('cccfg v3', body))}")

C = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SWEET_CC_MAX    512u
#define G_VP_TDM_IID    0x428fu
#define G_VP_AW_IID     0x42d3u
#define ARM_START      static void arm(int);
static int g_sel;

static void
g_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void
sweet_log_sync(void)
{
	fflush(stdout);
}
static int
PE_parse_boot_argn(const char *k, void *out, size_t sz)
{
	(void)k; (void)sz;
	memcpy(out, &g_sel, sizeof(g_sel));
	return (1);
}
static uint32_t
g_apm_set_cfg_batch(uint32_t src_port, uint8_t *body, uint32_t blen,
    uint32_t recs, const char *what)
{
	uint32_t off = 0;
	uint32_t i;

	printf("STUB send %s src=%#x dst=1 packet=%u bytes (inband=%s)\n",
	    what, src_port, 24u + 16u + blen, blen > 256u ? "no" : "yes");
	for (i = 0; i < recs; i++) {
		uint32_t iid = off + 12 < blen ? (uint32_t)body[off] |
		    ((uint32_t)body[off + 1] << 8) | ((uint32_t)body[off + 2] << 16) |
		    ((uint32_t)body[off + 3] << 24) : 0;
		uint32_t par = (uint32_t)body[off + 4] | ((uint32_t)body[off + 5] << 8) |
		    ((uint32_t)body[off + 6] << 16) | ((uint32_t)body[off + 7] << 24);
		uint32_t sz = (uint32_t)body[off + 8] | ((uint32_t)body[off + 9] << 8) |
		    ((uint32_t)body[off + 10] << 16) | ((uint32_t)body[off + 11] << 24);
		printf("   rec %u iid=%#06x param=%#010x size=%u data=", i, iid, par, sz);
		for (unsigned k = 0; k < (sz + 3u) / 4u; k++) {
			printf(" %08x", (unsigned)body[off + 16 + k * 4] |
			    ((unsigned)body[off + 17 + k * 4] << 8) |
			    ((unsigned)body[off + 18 + k * 4] << 16) |
			    ((unsigned)body[off + 19 + k * 4] << 24));
		}
		printf("\n");
		off += 16u + ((sz + 7u) & ~7u);
	}
	printf("   body=%u bytes, padded sum=%u (must match)\n", blen, off);
	return (0);
}

static void
run(int sel)
{
	uint32_t src_port = 0x2010u;

	g_sel = sel;
"""

TAIL = r"""
}

int
main(void)
{
	for (int s = 1; s <= 4; s++) {
		printf("===== sweetcccfg=%d =====\n", s);
		run(s);
	}
	printf("===== sweetcccfg=7 (unknown value must behave like 1) =====\n");
	run(7);
	printf("===== sweetcccfg=0 (nothing must run) =====\n");
	run(0);
	return (0);
}
"""

out = pathlib.Path("/tmp/stub_cccfg.c")
out.write_text(C + body + TAIL)
cc = ["cc", "-Wall", "-Wextra", "-Wformat=2", "-Werror", "-o", "/tmp/stub_cccfg", str(out)]
r = subprocess.run(cc, capture_output=True, text=True)
print(r.stdout, r.stderr)
if r.returncode != 0:
    sys.exit(f"STUB COMPILE FAILED rc={r.returncode}")
print("STUB COMPILE OK (-Wall -Wextra -Wformat=2 -Werror)")
p = subprocess.run(["/tmp/stub_cccfg"], capture_output=True, text=True)
print(p.stdout, end="")
if p.stderr:
    print("STDERR:", p.stderr[:400])
sys.exit(p.returncode)
