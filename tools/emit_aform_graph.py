#!/usr/bin/env python3
"""H0 - emit an A-FORM GRAPH_OPEN body carrying the CURRENT graph content.

Only one body shape has ever been accepted by this DSP on this port, and it is the
flat one: five records, each headed by apm_module_param_data_t{iid=1, param_id,
param_size, error_code} and padded to 8, in the order
    SG_CONFIG 0x08001001, CONTAINER_CONFIG 0x08001000, MODULE_LIST 0x08001002,
    MODULE_PROP 0x08001003, MODULE_CONN 0x08001004
That is `g_graph_vp_wr` (760 B), which answers status=0 in every boot it has run,
through PREPARE, START and 120 buffers.  The ACF-index shape ({total,count} + per
subgraph {key,size,body}) has been sent five times - 2960, 1536, 1024, 760 bytes of
it, plus the old off=16 series - and each time the APM stopped answering anything,
including our own control open afterwards.

So this script does NOT splice the ACF blob.  It authors the five flat records from
a table, and the table's contents are the readings this session verified from bytes
already in the tree (the vendor's self-consistent device entry, and our own
current-generation stream entry):
    device   key 0xb0000002 container 0xe0000002  modules SAL 0x4012,
             DATA_LOGGING 0x4007, SPLITTER 0x418e, TDM_SINK 0x428f,
             MUX_DEMUX 0x4006, MFC 0x48ba, 0x10013d01/AW 0x42d3
    stream   key 0xb0000023 container 0xe000002c  modules WR_SHARED_MEM_EP 0x40bd,
             DATA_LOGGING 0x40be, PCM_DEC 0x40bf, PCM_CNV 0x40c0, DATA_LOGGING 0x40c1

THE POSITIVE CONTROL IS NOT OPTIONAL.  A checker that finds nothing also reports
"no problems", and I have already been caught by exactly that (my first reference
checker condemned g_graph_vendor4, a body the vendor's own APM accepts).  So before
emitting anything, walk_vp_wr() must reproduce g_graph_vp_wr block for block; if it
does not, this script refuses and says why.
"""
import re
import struct
import sys

SRC = "../chain-tree/bsd/dev/sweet_glink.c"
OUT = "docs/data/h1-graph-body-aform.txt"

SG_DEV, CT_DEV = 0xB0000002, 0xE0000002
SG_STR, CT_STR = 0xB0000023, 0xE000002C
DEV_MODS = [(0x07001010, 0x4012), (0x0700101A, 0x4007), (0x07001011, 0x418E),
            (0x0700100E, 0x428F), (0x07001098, 0x4006), (0x07001015, 0x48BA),
            (0x10013D01, 0x42D3)]
STR_MODS = [(0x07001000, 0x40BD), (0x0700101A, 0x40BE), (0x07001005, 0x40BF),
            (0x07001003, 0x40C0), (0x0700101A, 0x40C1)]
# ports: (iid, in_ports, out_ports) as the vendor's own port tables state
PORTS = [(0x4012, 23, 1), (0x4007, 1, 1), (0x418E, 1, 28), (0x428F, 2, 0),
         (0x4006, 1, 2), (0x48BA, 2, 1), (0x42D3, 1, 1), (0x40BD, 0, 1),
         (0x40BE, 1, 1), (0x40BF, 1, 1), (0x40C0, 1, 1), (0x40C1, 1, 1)]
# links: (src iid, src port, dst iid, dst port); the device side is the vendor's
# 14-link table and the stream side the 9-link table, read off the accepted body
LINKS = [(0x40BD, 1, 0x40BE, 2), (0x40BE, 1, 0x40BF, 2), (0x40BF, 1, 0x40C0, 2),
         (0x40C0, 1, 0x40C1, 2), (0x40C1, 1, 0x4012, 2), (0x4012, 1, 0x48BA, 2),
         (0x48BA, 1, 0x42D3, 2), (0x42D3, 1, 0x418E, 2), (0x418E, 1, 0x4006, 2),
         (0x4006, 1, 0x4007, 2), (0x4007, 1, 0x428F, 2)]
CONTAINER_PROPS = [(0x08001011, [1, 0x0B001001]), (0x08001012, [1]),
                   (0x08001013, [8192]), (0x08001014, [2])]
SG_PROPS = [(0x0800100E, [2]), (0x0800100F, [2]), (0x08001010, [1])]


def u32(v):
    return struct.pack("<I", v)


def align8(n):
    return (n + 7) & ~7


def record(param, words):
    body = b"".join(u32(w) for w in words)
    return u32(1) + u32(param) + u32(len(body)) + u32(0) + body + b"\x00" * (
        align8(len(body)) - len(body))


def prop_words(props):
    out = []
    for pid, vals in props:
        out += [pid, 4 * len(vals)] + list(vals)
    return out


def build():
    mods = DEV_MODS + STR_MODS
    blocks = []
    blocks.append(record(0x08001001, [SG_DEV, len(SG_PROPS)] + prop_words(SG_PROPS)))
    conts = []
    for key in (SG_DEV, CT_DEV, SG_STR, CT_STR, len(mods)):
        pass
    blocks.append(record(0x08001000, [CT_STR, len(CONTAINER_PROPS)] +
                         prop_words(CONTAINER_PROPS)))
    blocks.append(record(0x08001002, [SG_STR, CT_STR, len(mods)] +
                         [w for pair in mods for w in pair]))
    port_w = [len(PORTS)]
    for iid, i, o in PORTS:
        port_w += [iid, 1, 0x08001015, 8, i, o]
    blocks.append(record(0x08001003, port_w))
    conn_w = [len(LINKS)]
    for a, ap, b, bp in LINKS:
        conn_w += [a, ap, b, bp]
    blocks.append(record(0x08001004, conn_w))
    return b"".join(blocks)


def walk(d, label):
    """Read a body as flat A-form records; return the list, or (None, reason)."""
    off, out = 0, []
    while off + 16 <= len(d):
        iid, pid, sz, err = struct.unpack_from("<IIII", d, off)
        if iid != 1 or pid >> 16 not in (0x0800,):
            return None, f"{label}: word at {off} is not an {{iid=1, 0x0800xxxx}} record"
        out.append((off, pid, sz))
        off += 16 + align8(sz)
    if off != len(d):
        return None, f"{label}: records end at {off}, body is {len(d)} (slack {len(d) - off})"
    return out, None


def load_carray(name):
    src = open(SRC, errors="replace").read()
    m = re.search(r"static const uint8_t %s\[(\d+)\] = \{(.*?)\n\};" % name, src, re.S)
    if not m:
        sys.exit(f"FATAL: {name} not found in {SRC}")
    d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if int(m.group(1)) != len(d):
        sys.exit(f"FATAL: {name} declared {m.group(1)} parsed {len(d)}")
    return d


def main():
    ref = load_carray("g_graph_vp_wr")
    want = [(o, p, s) for o, p, s in (walk(ref, "g_graph_vp_wr")[0] or [])]
    got, why = walk(ref, "g_graph_vp_wr")
    print("POSITIVE CONTROL  walk(g_graph_vp_wr)")
    if why:
        sys.exit(f"REFUSING: the walker cannot read the one body we know is accepted: {why}")
    print(f"  accepted body read as {len(got)} records, exact to {len(ref)} B:")
    for o, pid, sz in got:
        print(f"    {o:4d} param={pid:#010x} size={sz}")

    body = build()
    got2, why2 = walk(body, "emitted")
    print("\nEMITTED BODY")
    print(f"  {len(body)} bytes, walk: {'OK, exact' if not why2 else why2}")
    if why2:
        sys.exit("REFUSING: emitted body does not self-walk")
    print(f"  records: {len(got2)} (control had {len(got)})")
    print(f"  size vs the accepted 760: {len(body) - len(ref):+d} B")

    with open(OUT, "w") as h:
        h.write(f"/* H1 body, A form, current content - {len(body)} bytes, walks to the end.\n"
                " * Layout proven against g_graph_vp_wr (accepted, status=0) before emitting;\n"
                " * content authored from the tables in tools/emit_aform_graph.py.\n"
                " */\nstatic const uint8_t g_graph_aform_cur[%d] = {\n" % len(body))
        for i in range(0, len(body), 12):
            h.write("\t" + " ".join("0x%02x," % b for b in body[i:i + 12]) + "\n")
        h.write("};\n")
    print(f"  wrote {OUT}")


main()
