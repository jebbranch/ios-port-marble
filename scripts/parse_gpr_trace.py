#!/usr/bin/env python3
"""Decode the GPR packet trace captured with a kprobe on gpr_send_pkt.

The capture is a kprobe on the vendor's gpr_send_pkt(); each event carries
a0/a1 plus p0..p23 = the first 96 bytes of the message as **decimal** u32
words (kprobe prints them in decimal - reading them as hex silently yields
nonsense, which cost this project a day).

    # capture (on the phone, while Android runs)
    FIELDS=""; for i in $(seq 0 23); do FIELDS="$FIELDS p$i=+$((i*4))(%x1):u32"; done
    echo "p:gprpkt gpr_send_pkt a0=%x0 a1=%x1$FIELDS" >> kprobe_events

    # decode
    scripts/parse_gpr_trace.py docs/traces/trace_ui.txt
    scripts/parse_gpr_trace.py docs/traces/trace_ui.txt --opcode 0x0400100a

Packet layout (all words little endian, 32 bit):

    w0  (cmd << 4) | (len << 8)      cmd = 6 for every APM message
    w1  domains
    w2  src_port        w3  dst_port
    w4  token           w5  opcode
    w6  payload_addr_lsw   w7  payload_addr_msw
    w8  mem_map_handle     w9  payload_size
    w10..                   inline body, when the payload is not in shared memory

Two message shapes matter:

  * SHARED_MEM_MAP_REGIONS (0x0100100c) has NO payload descriptor; its body
    starts at w6: {pool|count<<16, property_flag, addr_lsw, addr_msw, size}.
  * APM-level commands (dst = 1) reference the mapped region through
    {offset, 0, handle, size} and keep the payload in shared memory.
    Module-level commands (dst = a module iid) carry the payload inline
    with handle = 0.
"""
import argparse
import collections
import re
import sys

OPCODES = {
    0x01001000: "GRAPH_OPEN",
    0x01001001: "GRAPH_PREPARE",
    0x01001002: "GRAPH_START",
    0x01001003: "GRAPH_STOP",
    0x01001004: "GRAPH_CLOSE",
    0x01001005: "GRAPH_FLUSH",
    0x01001006: "SET_CFG",
    0x01001007: "GET_CFG",
    0x0100100C: "SHM_MAP",
    0x0100100D: "SHM_UNMAP",
    0x0100100E: "?0x0100100e",
    0x0100100F: "?0x0100100f",
    0x01001010: "?0x01001010",
    0x0400100A: "DATA_BUFFER_V2",
}


def parse(path):
    pkts = []
    word = re.compile(r"\bp(\d+)=(\d+)")
    for line in open(path, errors="ignore"):
        if "p23=" not in line:
            continue
        w = {int(m.group(1)): int(m.group(2)) for m in word.finditer(line)}
        if len(w) >= 24:
            pkts.append(w)
    return pkts


def name(op):
    return OPCODES.get(op, "?")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--opcode", type=lambda s: int(s, 0),
                    help="only show packets with this opcode")
    ap.add_argument("--limit", type=int, default=3,
                    help="how many matching packets to dump (default 3)")
    args = ap.parse_args()

    pkts = parse(args.trace)
    if not pkts:
        sys.exit("no packets found (points is the capture from gpr_send_pkt?)")
    print(f"{len(pkts)} packets")

    if args.opcode is None:
        counts = collections.Counter(p[5] for p in pkts)
        print("\nopcode                              count  shape (src -> dst)")
        for op, n in counts.most_common():
            pairs = collections.Counter((p[2] & 0xffff, p[3] & 0xffff)
                                        for p in pkts if p[5] == op)
            shape = ", ".join(f"{s:#06x}->{d:#06x} x{c}"
                              for (s, d), c in pairs.most_common(3))
            print(f"  {op:#010x} {name(op):16s} {n:6d}  {shape}")
        return

    shown = 0
    for p in pkts:
        if p[5] != args.opcode:
            continue
        ln = (p[0] >> 8) & 0xffff
        print(f"\n--- {name(p[5])} {p[5]:#010x} len={ln} "
              f"src={p[2] & 0xffff:#06x} dst={p[3] & 0xffff:#06x} "
              f"token={p[4]:#x} dom={p[1]:#x}")
        print(f"    desc: addr={p[6]:#x}/{p[7]:#x} "
              f"handle={p[8]:#x} size={p[9]:#x}")
        body = " ".join(f"{p[i]:#010x}" for i in range(10, 24))
        print(f"    body: {body}")
        shown += 1
        if shown >= args.limit:
            break


if __name__ == "__main__":
    main()
