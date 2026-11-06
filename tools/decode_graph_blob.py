#!/usr/bin/env python3
"""Read the vendor's own GRAPH_OPEN body (g_graph_vendor4, 2724 bytes, lifted
out of the AGM service heap) through the parameter grammar that UniXNU-sheng's
independent AudioReach client writes.

WHY AN OUTSIDE GRAMMAR COUNTS.  Every AudioReach reading we scored before was
made against names we chose ourselves.  sheng's client is unrelated code
(BSD-2-Clause-Patent, package/ShengAudioReach.c, lines 7-11 and 86-122) and it
writes GRAPH_OPEN as five parameter blocks per subgraph, in this order:
    0x08001001 SG_CONFIG, 0x08001000 CONTAINER_CONFIG, 0x08001002 MODULE_LIST,
    0x08001003 MODULE_PROP, 0x08001004 MODULE_CONN
with property ids 0x0800100e..0x08001015.  Our captured vendor blob contains
those five ids, in that order, once per subgraph - that agreement is the point.

WHAT THIS SCRIPT DELIBERATELY DOES NOT CLAIM.  The word that precedes each
block's data is 12, 8 or 16 bytes depending on which reading you take, and the
observed gaps between consecutive blocks in this blob alternate (+60, +116,
+84, +188): the header size is NOT settled here.  So the script prints the raw
words of every block and interprets only the two shapes whose tuples are
self-validating (a module list whose count is followed by that many
(module, iid) pairs that are all in known ranges; a connection list whose
4-word tuples all name iids in range).  Anything implausible is printed as
raw and labelled.  Every loop is bounded by a literal, so this terminates.
"""
import re
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "../chain-tree/bsd/dev/sweet_glink.c"
NAME = sys.argv[2] if len(sys.argv) > 2 else "g_graph_vendor4"

PARAM = {0x08001000: "CONTAINER_CONFIG", 0x08001001: "SG_CONFIG",
         0x08001002: "MODULE_LIST", 0x08001003: "MODULE_PROP",
         0x08001004: "MODULE_CONN"}
MOD = {0x07001000: "WR_SHARED_MEM_EP", 0x07001001: "RD_SHARED_MEM_EP",
       0x07001003: "PCM_CNV", 0x07001004: "PCM_ENC", 0x07001005: "PCM_DEC",
       0x0700100e: "TDM_SINK", 0x07001010: "SAL", 0x07001011: "SPLITTER",
       0x07001015: "MFC", 0x07001019: "SOFT_PAUSE", 0x0700101a: "DATA_LOGGING",
       0x0700101b: "VOL_CTRL", 0x07001023: "CODEC_DMA_SINK",
       0x07001024: "CODEC_DMA_SOURCE", 0x07001038: "SYNC", 0x07001098: "MUX_DEMUX"}


def load(path, name):
    src = open(path, errors="replace").read()
    m = re.search(r"static const uint8_t %s\[(\d+)\] = \{(.*?)\n\};" % name,
                  src, re.S)
    if not m:
        sys.exit(f"FATAL: {name}[N] not in {path} - nothing decoded")
    d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if int(m.group(1)) != len(d):
        sys.exit(f"FATAL: declared {m.group(1)}, parsed {len(d)}")
    print(f"file : {path}\nblob : {name}, {len(d)} bytes (declaration matches)")
    return d


def main():
    d = load(SRC, NAME)
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    print(f"outer: declares size={u(0)} (real {len(d)}), subgraph_count={u(4)}")

    starts = [o for o in range(8, len(d) - 8, 4)
              if u(o) in PARAM and u(o + 4) and u(o + 4) % 4 == 0
              and u(o + 4) <= 4096 and o + 8 + u(o + 4) <= len(d)]
    print(f"scan : {len(starts)} blocks whose id is one of sheng's five and "
          f"whose declared length fits the blob\n")

    for o in starts:
        pid, ln = u(o), u(o + 4)
        nw = ln // 4
        w = [u(o + 8 + 4 * i) for i in range(nw)]
        print(f"{o:#6x} {PARAM[pid]:17s} declared_len={ln}")
        print(f"       raw: " + " ".join(f"{v:#09x}" for v in w[:24])
              + (" ..." if nw > 24 else ""))
        if pid == 0x08001002:
            cands = [(i, w[i]) for i in range(0, min(nw, 8)) if w[i] <= 16]
            for i, cnt in cands:
                pairs = w[i + 1:i + 1 + 2 * cnt]
                if len(pairs) == 2 * cnt and all(
                        (j % 2 == 0 and (p in MOD or 0x10000000 <= p < 0x11000000))
                        or (j % 2 == 1 and 0x4000 <= p < 0x5000)
                        for j, p in enumerate(pairs)):
                    print(f"       MODULES (count field at word {i}, value {cnt}):")
                    for k in range(cnt):
                        m, iid = pairs[2 * k], pairs[2 * k + 1]
                        print(f"         {m:#09x} {MOD.get(m, 'UNNAMED-IN-OUR-TABLES'):21s}"
                              f" iid={iid:#06x}")
                    break
            else:
                print("       MODULES: no self-validating count - NOT interpreted")
        if pid == 0x08001004:
            for i in range(0, min(nw, 4)):
                nl = w[i]
                tup = w[i + 1:i + 1 + 4 * nl]
                if nl and nl <= 32 and len(tup) == 4 * nl and all(
                        (j % 4 in (0, 2)) == (0x4000 <= p < 0x5000)
                        for j, p in enumerate(tup)):
                    print(f"       LINKS (count at word {i}, value {nl}, "
                          f"4+16*{nl}={4 + 16 * nl} bytes vs declared {ln}):")
                    for k in range(nl):
                        a, ap, b, bp = tup[4 * k:4 * k + 4]
                        print(f"         {a:#06x}:{ap} -> {b:#06x}:{bp}")
                    break
            else:
                print("       LINKS: no self-validating count - NOT interpreted")
        print()


main()
