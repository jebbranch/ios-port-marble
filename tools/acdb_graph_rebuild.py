#!/usr/bin/env python3
"""Rebuild the vendor GRAPH_OPEN container from the on-disk ACDB.

Why this exists: the AGM heap holds the assembled container only transiently, and
this ROM's root cannot install kprobes/uprobes (EACCES on tracefs, even setenforce
is refused), so the current 2872-byte device graph cannot be captured the way the
2724-byte one was.  But the *subgraph blobs* are stored verbatim in
/vendor/etc/acdbdata/ukee_*/*_acdb_cal.acdb, so the container is a deterministic
function of the ACDB plus whatever AGM inserts per record - and that insertion can
be derived by diffing a container we already have against the ACDB.

Method (no device, no boot):
  1. take a known-good container (g_graph_vendor4, 2724 B, vendor-declared length
     2712) and locate each of its subgraph blobs inside the ACDB byte for byte;
  2. diff the container region against the ACDB region and report where they diverge,
     which names the bytes AGM adds;
  3. solve the declared total: enumerate ACDB record combinations for a requested
     subgraph-id list until the sum equals the length the vendor's GRAPH_OPEN packet
     carries (496 for the stream graph, 2872 for the device graph).

usage:
  python3 tools/acdb_graph_rebuild.py --acdb ../sweet-audio/vendor-ukee/MTP_ukee_acdb_cal.acdb
  python3 tools/acdb_graph_rebuild.py --acdb ... --solve 0xb0000002,0xb0000025,0xb0000024,0xb0000023 --total 2872
"""
import argparse
import re
import struct
import sys

PROPS = 0x08001001            # PARAM_ID_SUB_GRAPH_CONFIG
SIG_HEAD = struct.pack("<II", 1, PROPS)


def props_offsets(b):
    """Every {iid=1, 0x08001001} header, at any alignment, with its size field."""
    out, i = [], b.find(SIG_HEAD)
    while i >= 0:
        size = struct.unpack_from("<I", b, i + 8)[0]
        out.append((i, size))
        i = b.find(SIG_HEAD, i + 1)
    return out


def walk_chain(b, off, limit=0x4000):
    """AudioReach TLV chain from a props header to the end of that subgraph blob."""
    start = off
    n = 0
    while off + 12 <= len(b) and n < 32:
        iid, pid, size = struct.unpack_from("<III", b, off)
        if iid > 0x20 or size == 0 or size > limit:
            break
        off = off + 12 + ((size + 7) & ~7)
        n += 1
    return off - start


def load_container(src_path, name):
    src = open(src_path).read()
    m = re.search(rf"{name}\[(\d+)\]\s*=\s*\{{(.*?)\n\}};", src, re.S)
    if not m:
        sys.exit(f"{name} not found in {src_path}")
    declared = int(m.group(1))
    arr = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
    if len(arr) != declared:
        sys.exit(f"{name}: parsed {len(arr)} bytes but source declares {declared}")
    return arr


def lcp_report(a, b, la, lb, label, max_diffs=6):
    """Longest common prefix, then where the two regions part ways."""
    n = min(len(a) - la, len(b) - lb)
    k = 0
    diffs = []
    while k < n and len(diffs) < max_diffs:
        if a[la + k] != b[lb + k]:
            va = struct.unpack_from("<I", a, la + k - ((la + k) % 4))[0]
            diffs.append((k, a[la + k], b[lb + k]))
            k += 4
        else:
            k += 1
    print(f"  {label}: common prefix {k} B (region {min(n, k)} B compared), "
          f"diffs(rel): " + ", ".join(f"+{d[0]}:{d[1]:02x}/{d[2]:02x}" for d in diffs))
    return k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--acdb", required=True)
    ap.add_argument("--container-src", default="work/chain-tree/bsd/dev/sweet_glink.c",
                    help="C file holding g_graph_vendor4")
    ap.add_argument("--declared", type=int, default=2712,
                    help="length the vendor GRAPH_OPEN packet carried for this container")
    ap.add_argument("--solve", default="",
                    help="comma-separated subgraph ids for the wanted container")
    ap.add_argument("--total", type=int, default=0, help="wanted container total length")
    a = ap.parse_args()

    acdb = open(a.acdb, "rb").read()
    recs = []
    for off, size in props_offsets(acdb):
        sg = struct.unpack_from("<I", acdb, off + 16)[0]
        recs.append((sg, off, size, walk_chain(acdb, off)))
    by_id = {}
    for sg, off, size, span in recs:
        by_id.setdefault(sg, []).append((off, size, span))
    print(f"== {a.acdb}: {len(recs)} props records, {len(by_id)} distinct subgraph ids")

    cont = load_container(a.container_src, "g_graph_vendor4")
    cheads = [o for o, s in props_offsets(cont) if s == 0x30]
    print(f"== container {len(cont)} B, header={struct.unpack_from('<II', cont, 0)}, "
          f"subgraph props at {[hex(o) for o in cheads]}")
    print(f"   vendor-declared payload length: {a.declared} "
          f"(difference {len(cont) - a.declared} B)")

    print("\n== 1) 容器里每个子图 blob 与 ACDB 记录的对比")
    for i, c0 in enumerate(cheads):
        c1 = cheads[i + 1] if i + 1 < len(cheads) else len(cont)
        sg = struct.unpack_from("<I", cont, c0 + 16)[0]
        cspan = walk_chain(cont, c0)
        cands = by_id.get(sg, [])
        hit = next((r for r in cands if acdb[r[0]:r[0] + 64] == cont[c0:c0 + 64]), None)
        print(f"  sg {sg:#010x}: container[{c0:#x},{c1:#x}) = {c1 - c0} B, "
              f"chain {cspan} B; ACDB 候选 {len(cands)} 个, "
              f"内容命中 = {hex(hit[0]) if hit else 'NO'}")
        if hit:
            lcp_report(cont, acdb, c0, hit[0], "blob from props")
            # the container region actually extends past the ACDB record: check the tail
            tail_start = hit[0] + cspan
            print(f"     ACDB record chain {hit[2]} B vs container chain {cspan} B "
                  f"-> delta {cspan - hit[2]}")
            print(f"     容器里这条记录后紧跟的 16 B: {cont[c0 + cspan:c0 + cspan + 16].hex(' ')}")
            print(f"     ACDB 里同位置后的 16 B:     {acdb[tail_start:tail_start + 16].hex(' ')}")
        # the record preamble: bytes before props, container vs ACDB
        print(f"     容器 props 前 20 B: {cont[c0 - 20:c0].hex(' ')}")
        if hit:
            print(f"     ACDB  props 前 20 B: {acdb[hit[0] - 20:hit[0]].hex(' ')}")

    if a.solve and a.total:
        want = [int(x, 16) if x.startswith("0x") else int(x) for x in a.solve.split(",")]
        pools = []
        for sg in want:
            cands = by_id.get(sg, [])
            print(f"== solve: sg {sg:#010x} 有 {len(cands)} 条 ACDB 记录 "
                  f"{[hex(c[0]) for c in cands][:6]}")
            pools.append(cands)
        import itertools
        combos = 0
        hits = 0
        for combo in itertools.product(*pools):
            combos += 1
            total = sum(c[2] for c in combo)
            if total + 8 * len(combo) == a.total or total == a.total:
                hits += 1
                print(f"   MATCH spans={[c[2] for c in combo]} offs={[hex(c[0]) for c in combo]}"
                      f" sum={total} +8/record={total + 8 * len(combo)}")
        print(f"== 枚举 {combos} 个组合，命中 {hits}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
