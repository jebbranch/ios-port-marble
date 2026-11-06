#!/usr/bin/env python3
"""Host-side compile+behaviour check for sweettdmvendor (0267).

The claim 0267 is built on: of the five parameters we push to the TDM module
(0x428f), only two appear anywhere in the vendor's accepted traffic
(0x08001017 MF_CFG and 0x0800101b TDM_INTF_CFG).  The other three -
0x08001018 = 1, 0x08001176 = <0|1>, 0x0800113c = {0, 12288000, 0, 0} - come
from this driver's CDMA-era list.  sweettdmvendor is a bitmask so one build can
bisect them with boot args only, and a round must be able to PROVE which packets
went out; before 0267, sweettdmpwr=0 and =1 printed identical lines.

This stub extracts the three guarded regions from the real source, compiles them
against a g_apm_set_cfg that records every (param, size, first data word), and
prints the send table for every mask x tdmpwr combination.

    python3 tools/stub_tdmvendor.py [path/to/sweet_glink.c]
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.path.join(ROOT, "..", "chain-tree", "bsd", "dev",
                                   "sweet_glink.c"))
lines = open(SRC, errors="replace").read().split("\n")


def block(anchor, prev_brace=False):
    """prev_brace: if the matched line is inside a block whose opening `{` is a
    bare line above it, start there instead - brace counting cannot close a
    region it never opened."""
    """Brace-balanced region starting at the line containing `anchor`.  A line
    without `{` cannot end the region, so a declaration-only first line is
    absorbed instead of returning a one-line block."""
    for i, ln in enumerate(lines):
        if anchor in ln:
            if prev_brace:
                k = i
                while k > 0 and lines[k].strip() != "{":
                    k -= 1
                i = k
            depth = 0
            seen_open = False
            out = []
            for j in range(i, len(lines)):
                out.append(lines[j])
                depth += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    seen_open = True
                if seen_open and depth <= 0:
                    return "\n".join(out), i + 1
            raise SystemExit("unbalanced block at " + SRC + ":" + str(i + 1))
    raise SystemExit("anchor not found in " + SRC + ": " + repr(anchor))


B0, b0_ln = block("int tdmsup = 0;", prev_brace=True)
B1, b1_ln = block("if (!(tdm_sup & 1)) {")
B2, b2_ln = block("int tp = 0;")
B4, b4_ln = block("if (!(tdm_sup & 4)) {")

for name, frag in (("1018", B1), ("1176", B2), ("113c", B4)):
    if "g_apm_set_cfg" not in frag:
        raise SystemExit("the extracted region for " + name +
                         " contains no g_apm_set_cfg call - the extraction is "
                         "measuring nothing")


def define_of(symbol):
    """Pull the real #define out of the source so the stub cannot invent a
    module id (a copied-from-the-wrong-namespace constant is a known failure
    mode of this project)."""
    for ln in lines:
        if ln.startswith("#define " + symbol + " "):
            return ln
    raise SystemExit("#define " + symbol + " not found in " + SRC)


DEFS = "\n".join([define_of("G_VP_TDM_IID")])

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DEFS

#define MAXSENT 8

static int bootarg_sup;
static int bootarg_pwr;
static int sent_n;
static int sent_iid[MAXSENT];
static int sent_pid[MAXSENT];
static int sent_size[MAXSENT];
static int sent_word0[MAXSENT];

static int
PE_parse_boot_argn(const char *name, void *value, unsigned long len)
{
	(void)len;
	if (strcmp(name, "sweettdmvendor") == 0) {
		memcpy(value, &bootarg_sup, sizeof(int));
	} else if (strcmp(name, "sweettdmpwr") == 0) {
		memcpy(value, &bootarg_pwr, sizeof(int));
	} else {
		return 0;
	}
	return 1;
}

/* Records what the real builder would have put on the wire. */
static int
g_apm_set_cfg(uint32_t src_port, uint32_t mod_iid, uint32_t pid,
    const void *data, uint32_t dlen, const char *what)
{
	uint32_t w0 = 0;

	(void)src_port;
	(void)what;
	if (sent_n < MAXSENT) {
		sent_iid[sent_n] = (int)mod_iid;
	sent_pid[sent_n] = (int)pid;
		sent_size[sent_n] = (int)dlen;
		if (dlen >= 4) {
			memcpy(&w0, data, 4);
		}
		sent_word0[sent_n] = (int)w0;
		sent_n++;
	}
	return 0;
}

static void sweet_log_sync(void);

static void
sweet_log_sync(void)
{
}

static int tdm_sup;

static void
send_region(uint32_t src_port)
{
	uint32_t v = 0;

%%B0%%
%%B1%%
%%B2%%
%%B4%%
}

int
main(void)
{
	static const int masks[] = { 0, 1, 2, 4, 7 };
	static const int tpows[] = { 0, 1, 2 };
	unsigned int m, p;

	printf("read SRC_FILE\\n");
	printf("regions extracted: 1018 at :B1_LN  1176 at :B2_LN  113c at :B4_LN\\n");
	printf(" sup tdpwr  sends to 0x428f, in order\\n");
	for (m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
		for (p = 0; p < sizeof(tpows) / sizeof(tpows[0]); p++) {
			int i;

			bootarg_sup = masks[m];
			bootarg_pwr = tpows[p];
			sent_n = 0;
			send_region(0x2010u);
			printf("  %3d  %5d  ", masks[m], tpows[p]);
			for (i = 0; i < sent_n; i++) {
				printf("%s%#x/%#x(%d,val%#x)", i ? " " : "",
				    (unsigned)sent_iid[i], (unsigned)sent_pid[i],
				    sent_size[i], sent_word0[i]);
			}
			if (sent_n == 0) {
				printf("(nothing)");
			}
			printf("   n=%d\\n", sent_n);
		}
	}
	return 0;
}
"""

main_src = (HARNESS
            .replace("DEFS", DEFS)
            .replace("%%B0%%", B0)
            .replace("%%B1%%", B1)
            .replace("%%B2%%", B2)
            .replace("%%B4%%", B4)
            .replace("SRC_FILE", SRC)
            .replace("B0_LN", str(b0_ln))
            .replace("B1_LN", str(b1_ln))
            .replace("B2_LN", str(b2_ln))
            .replace("B4_LN", str(b4_ln)))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_tdmvendor.c")
    xpath = os.path.join(td, "stub_tdmvendor")
    open(cpath, "w").write(main_src)
    cc = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
         "-o", xpath, cpath], capture_output=True, text=True)
    if cc.returncode != 0:
        print("COMPILE FAILED - read it as the stub's own error first; the "
              "generated source is dumped below:")
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

print("expect  (sup=0,tdpwr=0/1) -> 3 sends (1018, 1176 with val per tdpwr, 113c);")
print("        sup=1/2/4 -> that one missing;  sup=7 -> (nothing);")
print("        tdpwr=2 removes 1176 even when sup allows it.")
print("Every row must be reproducible from the ring: 0267 prints one marker")
print("line per decision, so a round can name which packets actually left.")
