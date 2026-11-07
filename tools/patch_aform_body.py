#!/usr/bin/env python3
"""H0' - patch the ACCEPTED body in place, so the layout is not a belief.

`g_graph_vp_wr` is the only GRAPH_OPEN body this DSP has ever answered (status=0,
every boot, through PREPARE/START and 120 buffers).  Its blocks, read as words:
    SG_CONFIG    48 B = {1, sg, nprops=3, (id,size,val)x3}
    CONTAINER   100 B = {1, container, nprops=7, ...}
    MODULE_LIST  80 B = {1, sg, container, n=8, (module,iid)x8}
    MODULE_PROP 196 B = {n=8, (iid, 1, 0x08001015, 8, in, out)x8}
    MODULE_CONN 244 B = {n=15, (src,sport,dst,dport)x15}
That adds up exactly, which is how I know these layouts and not my earlier guess
(my own emitter got SG_CONFIG wrong by one word and SCENARIO wrong by value).

The change is the one this session narrowed to: the accepted graph already has both
WR_SHARED_MEM_EP 0x40bd and the hardware end (TDM_SINK 0x428f, MFC 0x48ba, AW
0x10013d01/0x42d3, SAL 0x4012, SPLITTER, MUX_DEMUX, DATA_LOGGING).  It is missing
only PCM_DEC 0x07001005/0x40bf and PCM_CNV 0x07001003/0x40c0, so:
  MODULE_LIST : n 8 -> 10, append those two pairs
  MODULE_PROP : n 8 -> 10, append their PORT_INFO records (in=1, out=1)
  MODULE_CONN : the single link 0x40bd:1 -> 0x4012:2 becomes three,
                0x40bd:1 -> 0x40bf:2, 0x40bf:1 -> 0x40c0:2, 0x40c0:1 -> 0x4012:2
  SG_CONFIG and CONTAINER_CONFIG are copied byte for byte and the script aborts if
  that is not true - the whole point is that nothing outside the three blocks moves.
"""
import re
import struct
import sys

SRC = "../chain-tree/bsd/dev/sweet_glink.c"
OUT = "docs/data/h1p-graph-body-patched-vp-wr.txt"
PCM_DEC, IID_DEC = 0x07001005, 0x40BF
PCM_CNV, IID_CNV = 0x07001003, 0x40C0
WR, SAL = 0x40BD, 0x4012

names = {0x08001000: "CONTAINER_CONFIG", 0x08001001: "SG_CONFIG",
         0x08001002: "MODULE_LIST", 0x08001003: "MODULE_PROP",
         0x08001004: "MODULE_CONN"}


def load():
    src = open(SRC, errors="replace").read()
    m = re.search(r"static const uint8_t g_graph_vp_wr\[(\d+)\] = \{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit("FATAL: g_graph_vp_wr not found")
    d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if int(m.group(1)) != len(d):
        sys.exit("FATAL: declared size != byte count")
    return d


def split(d):
    """[(param, header_bytes, body_bytes)] preserving everything."""
    out, off = [], 0
    while off + 16 <= len(d):
        iid, pid, sz, err = struct.unpack_from("<IIII", d, off)
        if pid not in names:
            sys.exit(f"FATAL: unexpected param {pid:#x} at {off} - layout belief is wrong")
        out.append((pid, d[off:off + 16], d[off + 16:off + 16 + sz]))
        off += 16 + ((sz + 7) & ~7)
    if off != len(d):
        sys.exit(f"FATAL: blocks end at {off}, body is {len(d)}")
    return out


def words(body):
    return list(struct.unpack(f"<{len(body) // 4}I", body))


def pack(ws):
    return struct.pack(f"<{len(ws)}I", *ws)


def main():
    d = load()
    blocks = split(d)
    print(f"accepted body: {len(d)} B, {len(blocks)} blocks: "
          + ", ".join(f"{names[p]}={len(b)}" for p, _, b in blocks))
    have = dict((p, (h, b)) for p, h, b in blocks)
    for p in (0x08001001, 0x08001000):
        if p not in have:
            sys.exit("FATAL: SG_CONFIG/CONTAINER_CONFIG not both present")

    ml_h, ml_b = have[0x08001002]
    mp_h, mp_b = have[0x08001003]
    mc_h, mc_b = have[0x08001004]
    w = words(ml_b)
    if w[0] != 1 or w[3] != 8 or len(w) != 20:
        sys.exit(f"FATAL: MODULE_LIST layout differs from the read above: {w[:4]} n={len(w)}")
    w[3] = 10
    w += [PCM_DEC, IID_DEC, PCM_CNV, IID_CNV]
    new_ml = pack(w)

    p = words(mp_b)
    if p[0] != 8 or len(p) != 49:
        sys.exit(f"FATAL: MODULE_PROP layout differs: count={p[0]} words={len(p)}")
    p[0] = 10
    p += [IID_DEC, 1, 0x08001015, 8, 1, 1, IID_CNV, 1, 0x08001015, 8, 1, 1]
    new_mp = pack(p)

    c = words(mc_b)
    n = c[0]
    links = [(c[1 + 4 * k], c[2 + 4 * k], c[3 + 4 * k], c[4 + 4 * k]) for k in range(n)]
    if len(links) != n or len(c) != 1 + 4 * n:
        sys.exit(f"FATAL: MODULE_CONN words {len(c)} != 1+4*{n}")
    hit = [i for i, t in enumerate(links) if t == (WR, 1, SAL, 2)]
    if len(hit) != 1:
        sys.exit(f"FATAL: expected exactly one {WR:#06x}:1->{SAL:#06x}:2 link, found {len(hit)}")
    links = links[:hit[0]] + [(WR, 1, IID_DEC, 2), (IID_DEC, 1, IID_CNV, 2),
                              (IID_CNV, 1, SAL, 2)] + links[hit[0] + 1:]
    new_mc = pack([len(links)] + [v for t in links for v in t])

    out = bytearray()
    for pid, head, body in blocks:
        nb = {0x08001002: new_ml, 0x08001003: new_mp, 0x08001004: new_mc}.get(pid, body)
        pad = b"\x00" * (((len(nb) + 7) & ~7) - len(nb))
        out += head[:8] + struct.pack("<I", len(nb)) + head[12:16] + nb + pad
    out = bytes(out)

    back = split(out)
    bd = dict((pid, body) for pid, _, body in back)
    for pid in (0x08001001, 0x08001000):
        if bd[pid] != have[pid][1]:
            sys.exit(f"FATAL: {names[pid]} moved - the patch is not single-concern")
    print("gate: SG_CONFIG and CONTAINER_CONFIG byte-identical to the accepted body  OK")
    for pid in (0x08001002, 0x08001003, 0x08001004):
        print(f"  {names[pid]:12s} {len(have[pid][1]):3d} -> {len(bd[pid]):3d} B")
    print(f"total: {len(d)} -> {len(out)} B ({len(out) - len(d):+d})")

    with open(OUT, "w") as h:
        h.write("/* H0' - g_graph_vp_wr patched in place: +PCM_DEC 0x40bf +PCM_CNV 0x40c0,\n"
                " * WR:1->SAL:2 rewired through them. SG_CONFIG/CONTAINER_CONFIG are\n"
                f" * byte-identical to the accepted body (asserted by the tool). {len(out)} B.\n"
                " */\nstatic const uint8_t g_graph_vp_wr_dec_cnv[%d] = {\n" % len(out))
        for i in range(0, len(out), 12):
            h.write("\t" + " ".join("0x%02x," % b for b in out[i:i + 12]) + "\n")
        h.write("};\n")
    print("wrote", OUT)


main()
