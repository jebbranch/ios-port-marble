#!/usr/bin/env python3
"""Score the frozen bounded HAL-stream run and decode its raw PCM witness."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def score(raw, text, expected_time):
    if len(raw) < 4096 or raw[:8] != b"SWEETLOG":
        raise ValueError("not a SWEETLOG region")
    version, capacity = struct.unpack_from("<II", raw, 8)
    total, sequence = struct.unpack_from("<QQ", raw, 16)
    if version != 1 or not 0 < total <= capacity or 4096 + capacity > len(raw):
        raise ValueError("invalid or wrapped ring")
    window = raw[4096:4096 + total]
    if window != text:
        raise ValueError("decoded text is not the current raw window")
    times = set(re.findall(rb"sweettime=(\d+)", window))
    end = re.findall(
        rb"SWEET HALSTREAM: end result=(-?\d+) attempted=(\d+) sent=(\d+) "
        rb"done=(\d+) reject=(\d+) fault=(\d+) source=(\d+) silence=(\d+) "
        rb"peak-in=(\d+) peak-out=(\d+)", window)
    teardown = re.findall(
        rb"SWEET MARK: halstream teardown v1 open=(-?\d+) stop=(-?\d+) "
        rb"close=(-?\d+) unmap=(-?\d+)", window)
    controls = {
        name: re.findall(
            rb"SWEET MARK: halstream ctrl v1 " + name.encode() +
            rb" op=0x[0-9a-f]+ status=(0|0x[0-9a-f]+)", window)
        for name in ("open", "prepare", "start", "stop", "close", "unmap")
    }
    raw_rows = re.findall(
        rb"SWEET HALSTREAM: raw frame=(\d+) off=(\d+) bytes=(\d+) ([0-9a-f]+)",
        window)
    chunks = {}
    first_frame = None
    for frame, offset, count, hexdata in raw_rows:
        frame, offset, count = int(frame), int(offset), int(count)
        data = bytes.fromhex(hexdata.decode())
        if len(data) != count or offset in chunks:
            raise ValueError("bad or duplicate raw witness row")
        first_frame = frame if first_frame is None else first_frame
        if frame != first_frame:
            raise ValueError("raw witness changed start frame")
        chunks[offset] = data
    witness = b""
    if chunks:
        expected = 0
        for offset in sorted(chunks):
            if offset != expected:
                raise ValueError("raw witness has a gap")
            witness += chunks[offset]
            expected += len(chunks[offset])
    if len(witness) % 4:
        raise ValueError("raw witness is not S16 stereo")
    samples = struct.unpack("<%dh" % (len(witness) // 2), witness) if witness else ()
    left, right = samples[0::2], samples[1::2]
    channel_equal = sum(a == b for a, b in zip(left, right))
    channel_sign_same = sum((a == 0 or b == 0 or (a < 0) == (b < 0))
                            for a, b in zip(left, right))
    pcm = {
        "start_frame": first_frame, "bytes": len(witness),
        "frames": len(left), "left_peak": max(map(abs, left), default=0),
        "right_peak": max(map(abs, right), default=0),
        "nonzero_samples": sum(value != 0 for value in samples),
        "channel_equal_frames": channel_equal,
        "channel_same_sign_frames": channel_sign_same,
    }
    forbidden = {
        name: window.count(name.encode())
        for name in (
            "SWEET MARK: ampwrite", "SWEET MARK: ampguard",
            "pcm tone start", "pcm tone done", "data_buffer send",
            "SWEET GPU:", "SWEET GMU:", "panic(cpu", "Debugger called:",
        )
    }
    gates = {
        "identity": times == {str(expected_time).encode()},
        "window": len(text) == total,
        "guard": window.count(
            b"SWEET MARK: halstream guard v2 pass=1 s1ap=3") == 1,
        "amp_bypass": window.count(
            b"SWEET MARK: halstream amp-bypass v1 before MMIO") == 1,
        "awcfg_bypass": window.count(
            b"SWEET MARK: halstream awcfg-bypass v1") == 1,
        "graph": window.count(
            b"SWEET MARK: halstream graph v1 bytes=856 off=0x4000") == 1,
        "controls": all(values == [b"0"] for values in controls.values()),
        "stream": len(end) == 1 and tuple(map(int, end[0])) ==
            (0, 30, 30, 30, 0, 0, 30720, 0, 700, 21),
        "teardown": teardown == [(b"1", b"0", b"0", b"0")],
        "raw_pcm": pcm["bytes"] == 512 and pcm["frames"] == 128
            and pcm["nonzero_samples"] > 0
            and 0 < max(pcm["left_peak"], pcm["right_peak"]) <= 700
            and pcm["channel_same_sign_frames"] >= 96,
        "hardware_isolated": not any(forbidden.values()),
    }
    return {
        "passed": all(gates.values()), "gates": gates,
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "text_sha256": hashlib.sha256(text).hexdigest(),
        "total": total, "sequence": sequence, "pcm": pcm,
        "controls": {name: [value.decode() for value in values]
                     for name, values in controls.items()},
        "forbidden_counts": forbidden,
        "claim": "HAL PCM reached AudioReach and 30 buffers were strictly returned",
        "not_claimed": [
            "amplifier enabled", "speaker output", "audibility",
            "continuous playback beyond 30 buffers",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("text", type=Path)
    parser.add_argument("--sweettime", required=True, type=int)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = score(args.raw.read_bytes(), args.text.read_bytes(), args.sweettime)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(encoded)
    print(encoded, end="")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
