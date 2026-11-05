#!/usr/bin/env python3
"""Host-side compile+behaviour check for the sweettdmclk position selector (0266).

Why this exists: 0265's early gate was `tdmclk != 0`, so a round that asked for
the *late* vote (2) would also have fired the *early* one - two variables in one
boot.  0266 makes it `tdmclk == 1`.  That is exactly the kind of off-by-semantics
bug a boot cannot afford, and it is testable here for free: extract both gated
blocks from the real source, compile them against stubs, and print which side
fires for each selector value.

    python3 tools/stub_tdmclk.py [path/to/sweet_glink.c]

It prints the file it read (a stub that silently compiles a stale copy is worse
than no stub), compiles with the CI warning set, and runs a 4-value table.
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


def block(anchor):
    """The brace-balanced statement starting at the line containing `anchor`,
    plus its 1-based line number.  A line with no `{` never closes the block,
    so a declaration-only first line cannot end the extraction."""
    for i, ln in enumerate(lines):
        if anchor in ln:
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


EARLY, early_ln = block("if (tdmclk == 1) {")
# The late gate is measured, its boot-arg parse is re-written by the harness
# below (the real one is the two lines above this block), so anchor on the
# `if` itself and keep the guard's body byte-for-byte.
LATE, late_ln = block("if (tdm_late == 2) {")

VOTE_RE = re.compile(
    r"g_prm_vote_clock_ex\(\s*0x204u,\s*12288000u,\s*4u,\s*0u\s*\)")


def count_votes(fragment, counter):
    patched, n = VOTE_RE.subn(
        "(" + counter + "++, g_prm_vote_clock_ex(0x204u, 12288000u, 4u, 0u))",
        fragment)
    if n != 1:
        raise SystemExit("expected exactly one vote call in the extracted "
                         "block, found %d - the stub would be measuring "
                         "something other than the shipped code" % n)
    return patched


HARNESS = """
#include <stdio.h>
#include <string.h>

static int bootarg_value;      /* what PE_parse_boot_argn hands back */
static int early_votes;
static int late_votes;
static int tone_called;

static int
PE_parse_boot_argn(const char *name, void *value, unsigned long len)
{
	(void)name;
	(void)len;
	memcpy(value, &bootarg_value, sizeof(int));
	return 1;
}

/* Stands in for the real 76-byte PRM_REQUEST_HW_RSC send. */
static int
g_prm_vote_clock_ex(unsigned int clk, unsigned int freq, unsigned int attr,
    unsigned int root)
{
	(void)clk;
	(void)freq;
	(void)attr;
	(void)root;
	return 0;
}

static void sweet_log_sync(void);
static void g_audio_tone(unsigned int src, unsigned int dst);

static void
sweet_log_sync(void)
{
}

static void
g_audio_tone(unsigned int src, unsigned int dst)
{
	(void)src;
	(void)dst;
	tone_called++;
}

/*
 * The two extracted blocks, in the order the kernel reaches them: the early
 * vote sits in sweet_glink_probe() before graph_open, the late one inside
 * g_apm_graph_bringup() once graph_start has answered 0.
 */
static void
early_block(int tdmclk)
{
%%EARLY%%
}

static void
bringup_tail(unsigned int port, unsigned int iid)
{
	/* harness re-write of the two lines above the extracted gate */
	int tdm_late = 0;

	(void)PE_parse_boot_argn("sweettdmclk", &tdm_late, sizeof(tdm_late));
%%LATE%%
	g_audio_tone(port, iid);
}

int
main(void)
{
	static const int vals[] = { 0, 1, 2, 3 };
	unsigned int i;

	printf("read SRC_FILE\\n");
	printf("early block at :EARLY_LN   late block at :LATE_LN\\n");
	printf("  sel  early_vote  late_vote  tone_runs\\n");
	for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		int e0, l0, t0;
		int tdmclk = 0;

		bootarg_value = vals[i];
		e0 = early_votes;
		l0 = late_votes;
		t0 = tone_called;
		(void)PE_parse_boot_argn("sweettdmclk", &tdmclk, sizeof(tdmclk));
		early_block(tdmclk);
		bringup_tail(0x2010u, 0x40bdu);
		printf("  %3d          %d          %d          %d\\n", vals[i],
		    early_votes - e0, late_votes - l0, tone_called - t0);
	}
	return 0;
}
"""

main_src = (HARNESS
            .replace("%%EARLY%%", count_votes(EARLY, "early_votes"))
            .replace("%%LATE%%", count_votes(LATE, "late_votes"))
            .replace("SRC_FILE", SRC)
            .replace("EARLY_LN", str(early_ln))
            .replace("LATE_LN", str(late_ln)))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_tdmclk.c")
    xpath = os.path.join(td, "stub_tdmclk")
    open(cpath, "w").write(main_src)
    cc = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
         "-o", xpath, cpath], capture_output=True, text=True)
    if cc.returncode != 0:
        print("COMPILE FAILED (read it as the stub's own error first - the "
              "generated source is dumped below):")
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

print("expect  sel=0 -> 0/0, sel=1 -> 1/0, sel=2 -> 0/1, sel=3 -> 0/0")
print("a sel=2 row showing early_vote=1 is the 0265 bug 0266 exists to kill.")
print("tone_runs must be 1 on every row: the late vote must never skip the tone.")
