#!/usr/bin/env python3
"""Decode the vendor's OWN accepted playback out of the 2026-10-27 gprsend kprobe.

Why this file exists: up to 2026-11-06 we scored vendor traffic record by record,
and two of those readings turned out to be wrong in ways only a whole-timeline
walk exposes - the WR media format is byte-for-byte OURS (the "vendor is 24-bit/
Q23" premise never was in this capture), while the TDM endpoint's MF_CFG and the
MFC's output record both carry 0x00030018 where we send 0x00030020/0x00020020.
Both were found by walking all 112 rows instead of grepping one opcode.

Field map (verified against rows whose meaning is independently known):
  p2=src port  p3=dst  p5=opcode  p9=payload_size
  payload words start at p10  ->  p10=module_instance_id  p11=param_id
  p12=param_size  p13=error_code  p14..=the param body
So a body word N lives at p(14+N) - NOT at p6+N (the same trap as the RX capture).

  python3 tools/vendor_tx_timeline.py [logs/gpr-20261027-090802.txt]
"""
import pathlib
import re
import sys
from collections import Counter

APM = {
    0x01001000: "GRAPH_OPEN", 0x01001001: "GRAPH_PREPARE",
    0x01001002: "GRAPH_START", 0x01001003: "APM_01001003",
    0x01001004: "GRAPH_CLOSE", 0x01001005: "GRAPH_STOP",
    0x01001006: "SET_CFG", 0x01001007: "GET_CFG",
    0x0100100c: "SHM_MAP", 0x0100100d: "APM_0100100d",
    0x0100100f: "PRM_REQ_HW_RSC", 0x01001010: "APM_01001010",
    0x0400100a: "DATA_BUFFER_V2",
}
# The AudioReach param ids we have already named from the local ASoC header, so
# the table prints names only where a name exists in OUR repo (never invented).
PARAM = {
    0x0800100C: "PARAM_ID_MEDIA_FORMAT",
    0x08001008: "PARAM_08001008(unamed here)",
    0x08001017: "PARAM_ID_HW_EP_MF_CFG",
    0x08001018: "PARAM_ID_HW_EP_FRAME_SIZE_FACTOR",
    0x0800101b: "PARAM_0800101b(TDM_INTF_CFG)",
    0x08001024: "PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT",
    0x0800102c: "PARAM_ID_RSC_AUDIO_HW_CLK",
    0x080010c4: "PARAM_080010c4(unamed here)",
}


def rows(path):
    out = []
    for line in open(path, errors="replace"):
        if "gprsend:" not in line:
            continue
        m = re.search(r"(\d+\.\d+): gprsend:", line)
        p = {int(k): int(v) for k, v in re.findall(r"\bp(\d+)=(\d+)", line)}
        out.append((float(m.group(1)), p))
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else str(
        pathlib.Path(__file__).resolve().parent.parent /
        "logs/gpr-20261027-090802.txt")
    rs = rows(path)
    if not rs:
        sys.exit(f"no gprsend rows in {path}")
    t0 = rs[0][0]
    cnt = Counter()
    print(f"{path}: {len(rs)} gprsend rows\n")
    print(f"{'#':>4} {'t':>9} {'opcode':<16} {'src':>6} {'dst':>6} {'paylen':>7} body")
    for i, (t, p) in enumerate(rs):
        op = p.get(5, -1)
        nm = APM.get(op, f"op{op:#x}")
        cnt[nm] += 1
        if nm == "DATA_BUFFER_V2":
            continue
        body = ""
        if op == 0x0100100f:
            body = (f"clk={p.get(15):#x} freq={p.get(16)} attr={p.get(17)} "
                    f"root={p.get(18)} (param {p.get(11):#x} "
                    f"{PARAM.get(p.get(11), '')})")
        elif op in (0x01001006, 0x01001007):
            d = [p.get(14 + k, 0) for k in range(8)]
            body = (f"iid={p.get(10):#06x} {p.get(11):#x} "
                    f"{PARAM.get(p.get(11), '')} psize={p.get(12)} :: " +
                    " ".join(f"{x:#010x}" for x in d[:5]))
        else:
            body = f"payload_words={[hex(p.get(10 + k, 0)) for k in range(4)]}"
        print(f"{i:>4} +{(t - t0) * 1000:8.2f} {nm:<16} {p.get(2, 0):#06x} "
              f"{p.get(3, 0):#06x} {p.get(9, 0):>7} {body}")
    db = [p for _, p in rs if p.get(5) == 0x0400100a]
    if db:
        gaps = [round((rs[j + 1][0] - rs[j][0]) * 1000, 1)
                for j in range(len(rs) - 1) if rs[j][1].get(5) == 0x0400100a]
        print(f"\nDATA_BUFFER_V2: {len(db)} sends, dst={db[0].get(3):#x}, "
              f"payload_size field={db[0].get(9)}, median spacing "
              f"{sorted(gaps)[len(gaps) // 2] if gaps else '?'} ms")
        print("  (this capture leaves the buffer's address/length words zero for "
              "this opcode, so it does NOT settle buffer geometry against ours)")
    print("\ncounts:", dict(cnt))


main()
