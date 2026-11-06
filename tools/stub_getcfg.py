#!/usr/bin/env python3
"""Host-side compile+geometry check for APM_CMD_GET_CFG (0269).

0269 is the first time this port READS from a module, so the two things worth
proving before any boot are the packet geometry (the external note says
param_size must carry the buffer CAPACITY, and payload_size must be
16 + align8(capacity)) and the reply window the dump prints.  This stub lifts
g_apm_get_cfg_dump() verbatim out of the driver, records what g_apm_cmd() would
have been handed, and replays three fake replies: a GET_CFG-shaped one, a short
one, and no response at all - so the dump's own bounds checks get exercised.

    python3 tools/stub_getcfg.py [path/to/sweet_glink.c]
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

m = re.search(r"static void\ng_apm_get_cfg_dump\(.*?\n\}\n", text, re.S)
if not m:
    raise SystemExit("g_apm_get_cfg_dump not found in " + SRC)
FUNC = m.group(0)

# the three constants the function depends on, taken from the file itself
def val(name):
    mm = re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+)u?", text)
    if not mm:
        raise SystemExit("#define " + name + " not found in " + SRC)
    return mm.group(1)

DEFS = "#define APM_CMD_GET_CFG %s\n#define G_VP_MFC_IID %s\n" % (
    val("APM_CMD_GET_CFG"), val("G_VP_MFC_IID"))

HARNESS = """
DEFS
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_rsp[1024];
static uint32_t g_rsp_len;
static int g_rsp_ready;

static uint32_t
g_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	    ((uint32_t)p[3] << 24);
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

static int last_dst;
static uint32_t last_opcode;
static uint32_t last_plen;
static uint32_t last_rec[4];
static uint32_t last_total;

static int
g_apm_cmd(uint32_t src_port, uint32_t dst_port, const uint8_t *payload,
    uint32_t plen, uint32_t opcode, const char *what)
{
	(void)src_port;
	(void)what;
	last_dst = (int)dst_port;
	last_opcode = opcode;
	last_plen = plen;
	for (int i = 0; i < 4; i++) {
		last_rec[i] = g_le32(payload + 4 * i);
	}
	last_total = 24 + 16 + plen;
	return 0;
}

static void sweet_log_sync(void);

static void
sweet_log_sync(void)
{
}

%%FUNC%%

static void
fake_reply(int kind)
{
	memset(g_rsp, 0, sizeof(g_rsp));
	g_rsp_ready = 0;
	g_rsp_len = 0;
	if (kind == 0) {
		/* GET_CFG reply: GPR header (opcode at word 5 = 0x02001000),
		 * then status, iid, param, size, err, then the MFC struct:
		 * rate 96000, bit_width 32, num_channels 2, channel map 1,2. */
		g_rsp_len = 44 + 12;
		g_put32(g_rsp + 20, 0x02001000u);
		g_put32(g_rsp + 24, 0u);
		g_put32(g_rsp + 28, 0x48bau);
		g_put32(g_rsp + 32, 0x08001024u);
		g_put32(g_rsp + 36, 12u);
		g_put32(g_rsp + 40, 0u);
		g_put32(g_rsp + 44, 96000u);
		g_put32(g_rsp + 48, (uint32_t)32 | ((uint32_t)2 << 16));
		g_put32(g_rsp + 52, (uint32_t)1 | ((uint32_t)2 << 16));
		g_rsp_ready = 1;
	} else if (kind == 1) {
		g_rsp_len = 24;	/* basic response only - must not overread */
		g_put32(g_rsp + 20, 0x02001005u);
		g_rsp_ready = 1;
	}
	/* kind 2: nothing arrives, g_rsp_ready stays 0 */
}

int
main(void)
{
	int kind;

	printf("read SRC_FILE\\n");
	g_apm_get_cfg_dump(0x2010u, G_VP_MFC_IID, 0x08001024u, 72, "geometry");
	printf("packet: dst=%#x opcode=%#x record{iid=%#x,param=%#x,size=%u,err=%u}\\n",
	    last_dst, last_opcode, last_rec[0], last_rec[1], last_rec[2],
	    last_rec[3]);
	printf("        apm_payload=%u  gpr_packet_total=%u\\n", last_plen,
	    last_total);
	for (kind = 0; kind < 3; kind++) {
		fake_reply(kind);
		g_apm_get_cfg_dump(0x2010u, G_VP_MFC_IID, 0x08001024u, 72,
		    kind == 0 ? "full-reply" :
		    (kind == 1 ? "short-reply" : "no-reply"));
	}
	return 0;
}
"""

main_src = (HARNESS
            .replace("DEFS", DEFS)
            .replace("%%FUNC%%", FUNC)
            .replace("SRC_FILE", SRC))

with tempfile.TemporaryDirectory() as td:
    cpath = os.path.join(td, "stub_getcfg.c")
    xpath = os.path.join(td, "stub_getcfg")
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

print("expect  dst=0x48ba  opcode=0x1001007  record{iid=0x48ba,param=0x8001024,")
print("        size=72 (the buffer CAPACITY, not a payload length),err=0}")
print("        apm_payload=88 (the external note's 0x58) and total=128;")
print("        full-reply prints w5=0x2001000 w6=0 w7=0x48ba w8=0x8001024")
print("        w9=0xc w11=0x17700 (96000) and stops at the length bound;")
print("        short-reply prints only the words that fit;")
print("        no-reply says 'no response' instead of dumping zeros.")
