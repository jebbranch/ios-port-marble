#!/usr/bin/env python3
"""Compile-check the 0276/0277 latency census in isolation.

Why this exists: CI is the only place the real XNU tree is compiled, so a type or
syntax slip inside sweet_glink.c costs a build cycle (and today one did fail with
no locally readable log).  The census is exactly the kind of code that CAN be
checked here: two helpers plus the sampling block, with hand-written fakes for
everything they touch.  What this proves is limited and stated as such - it proves
these constructs typecheck and that every format specifier matches the argument
that the driver passes it (-Wformat=2 -Werror), nothing more, and it does not
replace the CI build.

    python3 tools/stub_dblat.py [path/to/sweet_glink.c]
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
text = open(SRC, errors="replace").read()
print(f"read {SRC}")


def grab(pattern, name):
    m = re.search(pattern, text, re.S)
    if not m:
        raise SystemExit(f"{name} not found in {SRC} - the stub measures nothing")
    return m.group(0)


HELPERS = grab(r"static uint32_t g_db_wait_n;\n.*?\n\}\n\nstatic void\ng_db_wait_report.*?\n\}\n",
               "the census helpers")
BLOCK = grab(r"\t\t\tunsigned waited = 0;\n.*?g_db_wait_sample\(waited, limit, g_rsp_ready != 0\);",
             "the sampling block")

for needle in ("sweetdbstep", "dblat v2"):
    if needle not in HELPERS + BLOCK:
        print(f"  note: '{needle}' is not in the extracted region")
if "dblat v2" not in HELPERS:
    raise SystemExit("the census report is not dblat v2 - wrong revision")

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t g_rsp_ready;

static int
PE_parse_boot_argn(const char *name, void *value, unsigned long len)
{
	(void)name; (void)len;
	memcpy(value, &(int){5}, sizeof(int));	/* step=5 unless overridden */
	return 1;
}

static int g_waits_called;
static unsigned g_last_wait_ms;
static unsigned fake_hit_at;	/* the reply is 'ready' after this many sleeps */

static void
g_wait(unsigned ms)
{
	g_waits_called++;
	g_last_wait_ms = ms;
	if ((unsigned)g_waits_called >= fake_hit_at) {
		g_rsp_ready = 1;
	}
}

static int
g_drain(unsigned budget_ms)
{
	(void)budget_ms;
	return 0;
}

static void sweet_log_sync(void);

static void
sweet_log_sync(void)
{
}

%%HELPERS%%

/* The extracted sampling block, in the same shape the driver holds it in:
 * inside a block that already has `limit` and reads g_rsp_ready. */
static int
sample_once(unsigned limit, unsigned ready_after)
{
	/* g_rsp_ready is set by the drain BEFORE the first sleep only when the
	 * reply is already there (ready_after == 0); otherwise the fake answers
	 * on the ready_after'th sleep, which is exactly the shape the driver
	 * sees. */
	(void)limit;
	g_rsp_ready = (ready_after == 0u) ? 1u : 0u;
	fake_hit_at = ready_after;
	g_waits_called = 0;
%%BLOCK%%
	return (int)waited;
}

int
main(void)
{
	unsigned limit = 100u;
	unsigned n;

	printf("census self-check: the reply is made to appear after N sleeps\\n");
	for (n = 0; n < 5; n++) {
		int waited;

		g_rsp_ready = 0;
		g_waits_called = 0;
		/* fake 'the answer shows up after n drains' */
		sample_once(limit, n);
		waited = (int)(g_waits_called * 5u);
		printf("  reply after %u sleeps -> sample=%d ms (waits=%d)\\n",
		    n, waited, g_waits_called);
	}
	g_db_wait_report("selftest");
	printf("expect step=5 in the line; n=5; min=0 (first drain hit) or 5; "
	       "timeout=0\\n");
	printf("then: a window that never answers must show timeout=1 and "
	       "min=max=avg=%u\\n", limit);
	g_db_wait_sample(limit, limit, 0);
	g_db_wait_report("timeout-case");
	return 0;
}
"""

main_src = (HARNESS.replace("%%HELPERS%%", HELPERS).replace("%%BLOCK%%", BLOCK))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_dblat.c")
    xpath = os.path.join(td, "stub_dblat")
    open(cpath, "w").write(main_src)
    cc = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
         "-o", xpath, cpath], capture_output=True, text=True)
    if cc.returncode != 0:
        print("COMPILE FAILED - this is the class of error that costs a CI cycle:")
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
