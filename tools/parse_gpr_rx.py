#!/usr/bin/env python3
"""Decode the GPR *receive* direction captured by tools/gpr_rx_capture.sh.

Why a separate parser: the send-direction parser (scripts/parse_gpr_trace.py) assumes
every packet is a command we sent.  These are packets the ADSP sent back, and the first
word's length field is the only thing that says how much of the 128-byte dump is real.
Words past that length are buffer residue (on the first capture p8 happened to hold
0xB0D89128, the vendor's mem_map_handle, which would have been very easy to mis-read as
part of a 32-byte response).  AGENTS section 5: never report data the instrument cannot
produce.

  tools/parse_gpr_rx.py logs/gpr-rx-*.txt [--opcode 0x01001006] [--verbose]
"""
import argparse
import collections
import pathlib
import re
import sys

OPS = {
    0x01001000: "GRAPH_OPEN", 0x01001001: "GRAPH_PREPARE", 0x01001002: "GRAPH_START",
    0x01001003: "GRAPH_STOP", 0x01001004: "GRAPH_CLOSE", 0x01001006: "SET_CFG",
    0x01001007: "GET_CFG", 0x0100100C: "SHM_MAP", 0x0100100D: "SHM_UNMAP",
    0x0400100A: "DATA_BUFFER_V2",
}
LINE = re.compile(r"gpr_rx:.*?\br0=(\S+) r1=(\S+) r2=(\S+) (.*)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--opcode", help="only show responses whose request opcode matches")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    rows = []
    for ln in pathlib.Path(a.trace).read_text(errors="replace").splitlines():
        m = LINE.search(ln)
        if not m:
            continue
        words = {}
        for k, v in re.findall(r"\bp(\d+)=(\d+)", m.group(4)):
            words[int(k)] = int(v)
        if 0 not in words:
            continue
        p0 = words[0]
        total, cmd, ver = p0 >> 8, (p0 >> 4) & 0xF, p0 & 0xF
        nw = total // 4
        payload = [words.get(i, 0) for i in range(6, min(nw, 32))]
        rows.append(dict(total=total, cmd=cmd, ver=ver, dom=words.get(1, 0),
                         src=words.get(2, 0), dst=words.get(3, 0), tok=words.get(4, 0),
                         rsp=words.get(5, 0), payload=payload,
                         residue=[words.get(i, 0) for i in range(nw, 32)]))

    print(f"packets parsed: {len(rows)}")
    print("\n== length distribution (total bytes incl. 24-byte header) ==")
    for n, c in sorted(collections.Counter(r["total"] for r in rows).items()):
        print(f"  {n:5} B  x{c}")
    print("\n== direction (domains word) ==")
    for d, c in collections.Counter(hex(r["dom"]) for r in rows).most_common(8):
        print(f"  {d:>10}  x{c}")

    print("\n== responses: (request opcode, status) census ==")
    cnt = collections.Counter()
    for r in rows:
        if r["payload"]:
            cnt[(r["payload"][0], r["payload"][1] if len(r["payload"]) > 1 else None)] += 1
    for (op, st), c in sorted(cnt.items(), key=lambda kv: -kv[1]):
        name = OPS.get(op, "?")
        sts = "-" if st is None else (hex(st) if isinstance(st, int) else st)
        print(f"  {op:#010x} {name:14} status={sts:>6}  x{c}")

    print("\n== packets longer than 32 B (would carry more than opcode+status) ==")
    long = [r for r in rows if r["total"] > 32]
    print(f"  count={len(long)}")
    for r in long[:12]:
        print("   ", hex(r["rsp"]), "src", hex(r["src"]), "dst", hex(r["dst"]),
              "payload", [hex(x) for x in r["payload"][:12]])

    want = int(a.opcode, 0) if a.opcode else 0x01001006
    sel = [r for r in rows if r["payload"] and r["payload"][0] == want]
    print(f"\n== {OPS.get(want, hex(want))} responses: {len(sel)} ==")
    for r in sel[:20]:
        print(f"   total={r['total']} src={r['src']:#x} dst={r['dst']:#x} tok={r['tok']} "
              f"rsp_op={r['rsp']:#010x} payload={[hex(x) for x in r['payload']]}")
    if a.verbose:
        print("\n== first 6 packets, in order ==")
        for r in rows[:6]:
            print("   ", {k: (hex(v) if isinstance(v, int) else v) for k, v in r.items()
                          if k != "residue"})


if __name__ == "__main__":
    main()
