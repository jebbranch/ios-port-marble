#!/usr/bin/env python3
"""Host-side compile+geometry check for sweetmfc (0268).

0268 configures the ONE MFC instance our graph actually contains
(MODULE_ID_MFC 0x07001015 at iid 0x48ba, byte-verified in g_graph_vp_wr), which
until now appeared in this driver only inside comments.  Three shapes are
selectable from one build, so the host test has to prove which bytes each shape
puts on the wire before any boot spends a shot:

  sel=1  single record, dst=0x48ba, param 0x08001024, size 12, rate 96000
  sel=2  single record, dst=0x48ba, param 0x08001024, size 12, rate 48000
  sel=3  vendor's two-record batch, dst=1, body 64 B, rates 48000 then 96000
  else   nothing

    python3 tools/stub_mfc.py [path/to/sweet_glink.c]
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


def define_of(symbol):
    for ln in lines:
        if ln.startswith("#define " + symbol + " "):
            return ln
    raise SystemExit("#define " + symbol + " not found in " + SRC)


BODY, body_ln = block('int mfc_sel = 0;')
for needle in ("g_apm_set_cfg(", "g_apm_set_cfg_batch("):
    if needle not in BODY:
        raise SystemExit("the extracted region contains no " + needle +
                         " - the stub would be measuring nothing")

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DEFS

static int bootarg_mfc;
static int last_kind;          /* 1 = single record, 2 = batch */
static uint32_t last_iid;
static uint32_t last_param;
static uint32_t last_size;
static uint32_t last_rate;
static int last_nrec;
static uint32_t last_bodylen;

static int
PE_parse_boot_argn(const char *name, void *value, unsigned long len)
{
	(void)len;
	if (strcmp(name, "sweetmfc") != 0) {
		return 0;
	}
	memcpy(value, &bootarg_mfc, sizeof(int));
	return 1;
}

static void
rec_word(const uint8_t *p, uint32_t *out)
{
	*out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The real little-endian writer, so the batch body the region builds is
 * decoded from the same bytes the DSP would read. */
static void
g_put32(void *p, uint32_t v)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

/* Mirrors the real builder's header: {iid, param, size, error} + data. */
static int
g_apm_set_cfg(uint32_t src_port, uint32_t mod_iid, uint32_t pid,
    const void *data, uint32_t dlen, const char *what)
{
	(void)src_port;
	(void)what;
	last_kind = 1;
	last_iid = mod_iid;
	last_param = pid;
	last_size = dlen;
	rec_word((const uint8_t *)data, &last_rate);
	last_nrec = 1;
	last_bodylen = 0;
	return 0;
}

static int
g_apm_set_cfg_batch(uint32_t src_port, uint8_t *body, uint32_t blen,
    uint32_t nrec, const char *what)
{
	(void)src_port;
	(void)what;
	last_kind = 2;
	last_bodylen = blen;
	last_nrec = (int)nrec;
	if (blen >= 28) {
		rec_word(body + 0, &last_iid);
		rec_word(body + 4, &last_param);
		rec_word(body + 8, &last_size);
		rec_word(body + 16, &last_rate);
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
	static const int sels[] = { 0, 1, 2, 3, 9 };
	unsigned int i;

	printf("read SRC_FILE\\n");
	printf("region extracted at :BODY_LN\\n");
	printf("  sel  form     target   param       rec_size  rate/body\\n");
	for (i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
		bootarg_mfc = sels[i];
		last_kind = 0;
		last_iid = 0;
		last_param = 0;
		last_size = 0;
		last_rate = 0;
		last_nrec = 0;
		last_bodylen = 0;
		send_region(0x2010u);
		printf("  %3d  %-8s %#-8x %#-10x %-9u %#x  nrec=%d body=%u\\n",
		    sels[i],
		    last_kind == 1 ? "single" :
		    (last_kind == 2 ? "batch" : "NONE"),
		    last_iid, last_param, last_size, last_rate,
		    last_nrec, last_bodylen);
	}
	return 0;
}
"""

main_src = (HARNESS
            .replace("DEFS", define_of("G_VP_MFC_IID"))
            .replace("%%BODY%%", BODY)
            .replace("SRC_FILE", SRC)
            .replace("BODY_LN", str(body_ln)))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_mfc.c")
    xpath = os.path.join(td, "stub_mfc")
    open(cpath, "w").write(main_src)
    cc = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
         "-o", xpath, cpath], capture_output=True, text=True)
    if cc.returncode != 0:
        print("COMPILE FAILED - stub's own error first; source dumped below:")
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

print("expect  sel=0/9 -> NONE (shipped sequence untouched);")
print("        sel=1 -> single  iid=0x48ba param=0x8001024 size=12 rate=0x17700 (96000);")
print("        sel=2 -> single  ... rate=0xbb80 (48000);")
print("        sel=3 -> batch   iid=0x48ba param=0x8001024 nrec=2 body=64 (the vendor's size).")
