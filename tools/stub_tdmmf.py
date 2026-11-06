#!/usr/bin/env python3
"""Host-side bytes check for 0273's TDM container-width knob (sweettdmvendor bit3).

The whole accepted playback, walked at once by tools/vendor_tx_timeline.py, says
  SET_CFG(0x428f, 0x08001017) psize=12 :: 0x00017700 0x00030018 0x00000001
while the H1/J1/K1/K2 baseline sends 0x00030020 in that second word - 24-bit vs
32-bit.  bit3 switches to the vendor's word WITHOUT touching the three invented
params the other bits of the same knob mask, so one build covers both and the
default (7) stays byte-identical.

    python3 tools/stub_tdmmf.py [path/to/sweet_glink.c]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.path.join(ROOT, "..", "chain-tree", "bsd", "dev",
                                   "sweet_glink.c"))
lines = open(SRC, errors="replace").read().split("\n")

start = None
for i, ln in enumerate(lines):
    if "uint8_t tdm_rate[12] = { 0 };" in ln:
        start = i
        break
if start is None:
    sys.exit("tdm_rate region not found in " + SRC)
end = None
for j in range(start, len(lines)):
    if '"vp/tdm_rate");' in lines[j]:
        end = j + 1
        break
if end is None:
    sys.exit("the tdm_rate region never reaches its send in " + SRC)
BODY = "\n".join(lines[start:end])
if "g_apm_set_cfg(" not in BODY:
    sys.exit("extracted region contains no send - measuring nothing")

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DEFS_MARKER

static int bootarg_tv;
static uint32_t last_w[3];
static uint32_t last_len;
static uint32_t last_iid;
static uint32_t last_pid;

static int
PE_parse_boot_argn(const char *name, void *value, unsigned long len)
{
	(void)len;
	if (strcmp(name, "sweettdmvendor") != 0) {
		return 0;
	}
	memcpy(value, &bootarg_tv, sizeof(int));
	return 1;
}

static void
g_put32(void *p, uint32_t v)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static int
g_apm_set_cfg(uint32_t src, uint32_t iid, uint32_t pid, const void *data,
    uint32_t dlen, const char *what)
{
	unsigned int k;

	(void)src; (void)what;
	last_iid = iid;
	last_pid = pid;
	last_len = dlen;
	for (k = 0; k < 3; k++) {
		const uint8_t *b = (const uint8_t *)data + 4 * k;

		last_w[k] = b[0] | (b[1] << 8) | (b[2] << 16) |
		    ((uint32_t)b[3] << 24);
	}
	return 0;
}

static void sweet_log_sync(void);

static void
sweet_log_sync(void)
{
}

static void
send_region(uint32_t src_port)
{
%%BODY%%
}

int
main(void)
{
	static const int vals[] = { 0, 7, 8, 15, 16 };
	unsigned int i;

	printf("read SRC_FILE\\n");
	printf("region: :STARTLN..:ENDLN (%d lines)\\n", :NLINE);
	printf("  tv    iid     param      len  w0       w1       w2\\n");
	for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		bootarg_tv = vals[i];
		memset(last_w, 0, sizeof(last_w));
		last_len = 0;
		send_region(0x2010u);
		printf("  %3d  %#-7x %#-10x %u    %#x %#x %#x\\n",
		    vals[i], last_iid, last_pid, last_len,
		    last_w[0], last_w[1], last_w[2]);
	}
	return 0;
}
"""

nline = end - start
def define_of(symbol):
    for ln in lines:
        if ln.startswith("#define " + symbol + " "):
            return ln
    sys.exit("#define " + symbol + " not found in " + SRC)


main_src = (HARNESS.replace("DEFS_MARKER", define_of("G_VP_TDM_IID"))
            .replace("%%BODY%%", BODY)
            .replace("SRC_FILE", SRC)
            .replace(":STARTLN", str(start + 1))
            .replace(":ENDLN", str(end))
            .replace(":NLINE", str(nline)))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_tdmmf.c")
    xpath = os.path.join(td, "stub_tdmmf")
    open(cpath, "w").write(main_src)
    cc = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
         "-o", xpath, cpath], capture_output=True, text=True)
    if cc.returncode != 0:
        print("COMPILE FAILED - stub's own error first; source below:")
        print(cc.stderr)
        print("---- generated source ----")
        print(main_src)
        sys.exit(1)
    print("compile clean: -std=gnu11 -Wall -Wextra -Wformat=2 -Werror")
    run = subprocess.run([xpath], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.returncode:
        sys.stderr.write(run.stderr)
        sys.exit(run.returncode)

print("expect  tv=0/7 -> w1=0x30020 (the H1/J1/K1/K2 baseline, byte-identical);")
print("        tv=8/15 -> w1=0x30018 (the vendor's word), w0=0x17700 and w2=1 always;")
print("        iid=0x428f param=0x8001017 len=12 for every value, tv=16 (no bit3)")
print("        back on 0x30020 proves the mask is a bit, not a range.")
