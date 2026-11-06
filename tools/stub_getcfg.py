#!/usr/bin/env python3
"""Host-side compile+geometry check for the GET_CFG path (0269, rewritten 0270).

0270 matters because the first version sent its query through g_apm_cmd(), which
parses EVERY reply as a basic response.  On a structured GET_CFG reply that made
the echoed module instance id print as "status" - so a perfectly good answer
looked like a failure - and on a basic result it printed the ORIGINAL command
opcode as a status.  The rewritten function sends the command itself and branches
on the reply's own GPR opcode.  This stub compiles it against a fake transport
and replays five situations, including the one that must never be read as data:
a structured reply whose param_size came back still 72, i.e. nobody wrote.

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


def val(name):
    """Pull the constant out of the driver itself so the stub cannot invent one
    (a copied-from-the-wrong-namespace value is a known failure mode here)."""
    mm = re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+)u?", text)
    if not mm:
        raise SystemExit("#define " + name + " not found in " + SRC)
    return mm.group(1)


DEFS = "\n".join([
    "#define APM_CMD_GET_CFG " + val("APM_CMD_GET_CFG"),
    "#define APM_CMD_RSP_GET_CFG " + val("APM_CMD_RSP_GET_CFG"),
    "#define GPR_DOMAIN_ADSP 0x2u",
    "#define GPR_DOMAIN_APPS 0x3u",
])

HARNESS = """
DEFS
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define G_INTENT_SIZE 1024u

static uint8_t g_rsp[G_INTENT_SIZE];
static uint32_t g_rsp_len;
static int g_rsp_ready;

static uint32_t sent_pkt_size;
static uint32_t sent_hdr[6];
static uint32_t sent_rec[4];

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

static void
g_send_gpr(const uint8_t *pkt, uint32_t len)
{
	sent_pkt_size = len;
	for (int i = 0; i < 6; i++) {
		sent_hdr[i] = g_le32(pkt + 4 * i);
	}
	for (int i = 0; i < 4; i++) {
		sent_rec[i] = g_le32(pkt + 40 + 4 * i);
	}
}

/* the reply is *pending* until the transport delivers it, which is how the real
 * GLINK works and it is what tests the function's own reset-before-send: a stub
 * that left g_rsp_ready set across calls would hide a stale-response bug. */
static uint8_t pending[G_INTENT_SIZE];
static uint32_t pending_len;
static int pending_ready;

static int
g_drain(unsigned budget_ms)
{
	(void)budget_ms;
	if (!pending_ready) {
		return 0;
	}
	memcpy(g_rsp, pending, pending_len);
	g_rsp_len = pending_len;
	g_rsp_ready = 1;
	pending_ready = 0;
	return 1;
}

static void
g_wait(unsigned ms)
{
	(void)ms;
}

static void sweet_log_sync(void);

static void
sweet_log_sync(void)
{
}

%%FUNC%%

/* kind 0 = structured reply carrying data; 1 = structured reply whose
 * param_size is still the 72 we offered (nobody wrote); 2 = basic result;
 * 3 = nothing arrives. */
static void
fake_reply(int kind, uint32_t rate)
{
	memset(pending, 0, sizeof(pending));
	memset(g_rsp, 0, sizeof(g_rsp));
	g_rsp_ready = 0;
	g_rsp_len = 0;
	pending_len = 0;
	pending_ready = 0;
	if (kind == 3) {
		return;
	}
	g_put32(pending + 20, kind == 2 ? 0x02001005u : 0x02001000u);
	if (kind == 2) {
		pending_len = 32;
		g_put32(pending + 24, 0x01001007u);	/* the ORIGINAL opcode */
		g_put32(pending + 28, 0x3u);		/* AR_EUNSUPPORTED */
	} else {
		pending_len = kind == 0 ? 44 + 12 : 44;
		g_put32(pending + 24, 0u);		/* overall status */
		g_put32(pending + 28, 0x48bau);	/* iid echo */
		g_put32(pending + 32, 0x08001024u);	/* param echo */
		g_put32(pending + 36, kind == 0 ? 12u : 72u);
		g_put32(pending + 40, 0u);		/* error_code */
		if (kind == 0) {
			g_put32(pending + 44, rate);
			g_put32(pending + 48, (uint32_t)32 | ((uint32_t)2 << 16));
			g_put32(pending + 52, (uint32_t)1 | ((uint32_t)2 << 16));
		}
	}
	pending_ready = 1;
}

int
main(void)
{
	printf("read SRC_FILE\\n");
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "geometry");
	printf("sent: pkt_size=%u hdr{ver+len=%#x dom=%#x src=%#x dst=%#x tok=%#x op=%#x}\\n",
	    sent_pkt_size, sent_hdr[0], sent_hdr[1], sent_hdr[2], sent_hdr[3],
	    sent_hdr[4], sent_hdr[5]);
	printf("      record{iid=%#x param=%#x size=%u err=%u}  (size = the 72-byte CAPACITY)\\n",
	    sent_rec[0], sent_rec[1], sent_rec[2], sent_rec[3]);
	fake_reply(0, 96000u);
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "got96k");
	fake_reply(0, 48000u);
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "got48k");
	fake_reply(1, 0u);
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "size-not-written-back");
	fake_reply(2, 0u);
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "basic-result");
	fake_reply(3, 0u);
	g_apm_get_cfg_dump(0x2010u, 0x48bau, 0x08001024u, 72, "no-reply");
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

print("expect  pkt_size=128, dst=0x48ba, op=0x1001007, record size=72 (CAPACITY);")
print("        got96k -> rate=96000 bw=32 nch=2 cmap=0x20001 (L,R) size=12;")
print("        got48k -> rate=48000;")
print("        size-not-written-back -> size=72 rate=0 (instrument reading, not a verdict);")
print("        basic-result -> echoed_op=0x1001007 status=0x3 and NO param fields;")
print("        no-reply -> one honest 'no response' line, never a dump of zeros.")
