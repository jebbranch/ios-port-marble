#!/usr/bin/env python3
"""Score a stage-1 capture ring, including stale-window and hardware controls."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MARKER = b"SWEET MARK: halbridge v1 node=/dev/marble-audio ring=65536 capture-only"
PCM = re.compile(
    rb"SWEET HAL: pcm write=(\d+) frames=(\d+) sound=(\d+) peak=(\d+) "
    rb"queued=(\d+) overwritten=(\d+)")


def score(raw, expected_time):
    if len(raw) < 4096 or raw[:8] != b"SWEETLOG":
        raise ValueError("not a SWEETLOG region")
    version, capacity = struct.unpack_from("<II", raw, 8)
    total, sequence = struct.unpack_from("<QQ", raw, 16)
    if version != 1 or not 0 < total <= capacity or 4096 + capacity > len(raw):
        raise ValueError("invalid, truncated, or identity-wrapped ring")
    window = raw[4096:4096 + total]
    times = set(re.findall(rb"sweettime=(\d+)", window))
    records = [tuple(map(int, match.groups())) for match in PCM.finditer(window)]
    forbidden = {
        name: window.count(name.encode())
        for name in (
            "SWEET ADSP: early amp probe", "SWEET ADSP: Q6 released",
            "SWEET MARK: ampguard", "SWEET MARK: ampwrite",
            "SWEET MARK: ampfirst", "graph_open", "data_buffer send",
            "pcm tone start", "SWEET GMU:", "SWEET GPU:",
        )
    }
    monotonic = all(a[0] < b[0] and a[1] < b[1] and a[2] <= b[2]
                    for a, b in zip(records, records[1:]))
    sane = all(sound <= frames and peak <= 32768 and queued <= 65536
               and frames == queued + overwritten
               for _, frames, sound, peak, queued, overwritten in records)
    opens = list(map(int, re.findall(rb"SWEET HAL: open=(\d+)", window)))
    gates = {
        "identity": times == {str(expected_time).encode()},
        "marker": MARKER in window,
        "adsp_disabled": b"SWEET ADSP: disabled" in window,
        "hardware_isolated": not any(forbidden.values()),
        "node_open": bool(opens) and max(opens) >= 1,
        "pcm_progress": len(records) >= 2 and monotonic and sane,
        "non_silent": bool(records) and records[-1][1] >= 4096
                      and records[-1][2] > 0 and 8 < records[-1][3] <= 32768,
        "no_kernel_panic": not re.search(rb"panic\(cpu|panicString|Debugger called:", window),
    }
    crashes = {}
    for process in re.findall(rb"CRASH ([^\[\s]+)\[\d+\] sig", window):
        name = process.decode(errors="replace")
        crashes[name] = crashes.get(name, 0) + 1
    return {
        "capture_gates_passed": all(gates.values()), "gates": gates,
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "total": total, "sequence": sequence, "pcm_snapshots": len(records),
        "latest_pcm": dict(zip(
            ("writes", "frames", "sound_frames", "peak", "queued", "overwritten"),
            records[-1])) if records else None,
        "forbidden_runtime_counts": forbidden, "process_crashes": crashes,
        "past_window_marker_count": raw[4096 + total:].count(MARKER),
        "waveform_correctness": "not measured",
        "realtime_frame_rate": "not measured",
        "speaker_output": "not tested; capture-only has no hardware sink",
        "ui_failures": "recorded separately; user deferred UI polish",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--sweettime", type=int, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = score(args.raw.read_bytes(), args.sweettime)
    text = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(text)
    print(text, end="")
    raise SystemExit(0 if result["capture_gates_passed"] else 1)


if __name__ == "__main__":
    main()
