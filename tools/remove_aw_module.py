#!/usr/bin/env python3
"""B1 - remove the AFE-id module from the A-form body and rewire around it.

The body we ship (856 B) was built by editing the accepted g_graph_vp_wr in place,
so SG_CONFIG and CONTAINER_CONFIG are provably the accepted bytes.  This removes one
module - 0x10013D01 (AFE AWDSP_RX) with instance 0x42D3 - because an outside review
argued an AFE module id has no business in an APM MODULE_LIST, while our own capture
shows the vendor sending SET_CFG to dst=0x42d3 with param=0x10013d2a.  The two pieces
of evidence conflict, so the module comes out and the boot decides.

Removing it touches three blocks, which is one semantic variable:
  MODULE_LIST : drop the (0x10013d01, 0x42d3) pair, count 10 -> 9
  MODULE_PROP : drop that iid's PORT_INFO record, count 10 -> 9
  MODULE_CONN : 0x48ba:1 -> 0x42d3:2 and 0x42d3:1 -> 0x418e:2 become
                0x48ba:1 -> 0x418e:2, so the chain stays connected
SG_CONFIG and CONTAINER_CONFIG are asserted byte-identical to the input, and the
result must walk with zero slack, or this refuses to write anything.
"""
import re
import struct
import sys

SRC = "docs/data/h1p-graph-body-patched-vp-wr.txt"
OUT = "docs/data/b1-graph-body-no-aw.txt"
MOD, IID = 0x10013D01, 0x42D3
MFC, SPLITTER = 0x48BA, 0x418E
NAMES = {0x08001000: "CONTAINER_CONFIG", 0x08001001: "SG_CONFIG",
         0x08001002: "MODULE_LIST", 0x08001003: "MODULE_PROP",
         0x08001004: "MODULE_CONN"}


def load(path):
    txt = open(path, errors="replace").read()
    m = re.search(r"\[(\d+)\] = \{(.*?)\n\};", txt, re.S)
    if not m:
        sys.exit(f"FATAL: no C array in {path}")
    d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if int(m.group(1)) != len(d):
        sys.exit(f"FATAL: declared {m.group(1)} parsed {len(d)}")
    return d


def split(d):
    out, off = [], 0
    while off + 16 <= len(d):
        iid, pid, sz, err = struct.unpack_from("<IIII", d, off)
        if pid not in NAMES:
            sys.exit(f"FATAL: unknown param {pid:#x} at {off}")
        out.append((pid, d[off:off + 16], d[off + 16:off + 16 + sz]))
        off += 16 + ((sz + 7) & ~7)
    if off != len(d):
        sys.exit(f"FATAL: blocks end at {off}, body is {len(d)}")
    return out


def words(b):
    return list(struct.unpack(f"<{len(b) // 4}I", b))


def pack(w):
    return struct.pack(f"<{len(w)}I", *w)


def main():
    d = load(SRC)
    blocks = split(d)
    print(f"input: {len(d)} B, " + ", ".join(f"{NAMES[p]}={len(b)}" for p, _, b in blocks))
    out = bytearray()
    counts = {}
    for pid, head, body in blocks:
        w = words(body)
        if pid == 0x08001002:
            n = w[3]
            pairs = [(w[4 + 2 * i], w[5 + 2 * i]) for i in range(n)]
            if (MOD, IID) not in pairs:
                sys.exit(f"FATAL: ({MOD:#x},{IID:#x}) not in MODULE_LIST")
            pairs = [p for p in pairs if p != (MOD, IID)]
            w = w[:3] + [len(pairs)] + [v for p in pairs for v in p]
            counts["MODULE_LIST"] = (n, len(pairs))
        elif pid == 0x08001003:
            n = w[0]
            recs, i = [], 1
            for _ in range(n):
                iid, npc = w[i], w[i + 1]
                ln = 2 + sum(2 + max(1, (w[i + 2 + 2 * k + 1] + 3) // 4)
                             for k in range(npc))
                recs.append((iid, w[i:i + ln]))
                i += ln
            if i != len(w):
                sys.exit(f"FATAL: MODULE_PROP walk ended at {i} of {len(w)}")
            keep = [r for r in recs if r[0] != IID]
            if len(keep) != len(recs) - 1:
                sys.exit("FATAL: expected exactly one PORT_INFO record for that iid")
            w = [len(keep)] + [v for _, r in keep for v in r]
            counts["MODULE_PROP"] = (n, len(keep))
        elif pid == 0x08001004:
            n = w[0]
            links = [tuple(w[1 + 4 * i:5 + 4 * i]) for i in range(n)]
            a = (MFC, 1, IID, 2)
            b = (IID, 1, SPLITTER, 2)
            if a not in links or b not in links:
                sys.exit(f"FATAL: expected both links through {IID:#x}; got {links}")
            links = [l for l in links if l not in (a, b)] + [(MFC, 1, SPLITTER, 2)]
            w = [len(links)] + [v for l in links for v in l]
            counts["MODULE_CONN"] = (n, len(links))
        nb = pack(w)
        pad = b"\x00" * (((len(nb) + 7) & ~7) - len(nb))
        out += head[:8] + struct.pack("<I", len(nb)) + head[12:16] + nb + pad
    out = bytes(out)

    back = {p: b for p, _, b in split(out)}
    fwd = {p: b for p, _, b in blocks}
    for pid in (0x08001001, 0x08001000):
        if back[pid] != fwd[pid]:
            sys.exit(f"FATAL: {NAMES[pid]} changed - refusing to emit")
    print("gate: SG_CONFIG and CONTAINER_CONFIG byte-identical  OK")
    for k, (a, b) in counts.items():
        print(f"  {k:12s} count {a} -> {b}")
    print(f"total: {len(d)} -> {len(out)} B ({len(out) - len(d):+d}), walk slack 0")
    with open(OUT, "w") as h:
        h.write(f"/* B1 - the 856-byte A-form body with module {MOD:#x}/iid {IID:#x} removed\n"
                " * and MFC:1->SPLITTER:2 rewired. SG_CONFIG/CONTAINER_CONFIG asserted\n"
                f" * byte-identical to the input. {len(out)} B, walks with slack 0.\n"
                " */\nstatic const uint8_t g_graph_aform_no_aw[%d] = {\n" % len(out))
        for i in range(0, len(out), 12):
            h.write("\t" + " ".join("0x%02x," % x for x in out[i:i + 12]) + "\n")
        h.write("};\n")
    print("wrote", OUT)


main()
