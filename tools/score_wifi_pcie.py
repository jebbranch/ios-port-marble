#!/usr/bin/env python3
"""Score a frozen 0301 PCIe identity run; never infer Wi-Fi or DMA success."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def score(raw, sweettime):
    if len(raw) < 4096 or raw[:8] != b"SWEETLOG":
        raise ValueError("not a SWEETLOG ring")
    version, capacity, total, sequence = struct.unpack_from("<IIQQ", raw, 8)
    if version != 1 or not 0 < total <= capacity or len(raw) < 4096 + capacity:
        raise ValueError("invalid or wrapped ring; preserve raw for separate analysis")
    text = raw[4096:4096 + total].decode("utf-8", errors="replace")
    num = r"(0x[0-9a-f]+|[0-9]+)"
    identity = re.findall(r"\bsweettime=([0-9]+)\b", text)
    handoff = re.findall(
        r"SWEET MARK: wifi handoff v1 readonly=1 stage=2 gdsc=" + num +
        r" halted=" + num + r" wlan=" + num + r" perst=" + num, text)
    endpoint = re.findall(
        r"SWEET MARK: wifi pcie v2 stage=2 id=" + num +
        r" subsystem=" + num + r" bar=" + num + r" bytes=" + num +
        r" soc=" + num + r" sub=" + num, text)
    failures = re.findall(
        r"SWEET WIFI: phase 2 failed errno=([0-9]+) poisoned=([01]) milestone=([0-9]+)",
        text)
    forbidden = {
        key: text.count(key) for key in (
            "SWEET GLINK:", "SWEET I2C:", "SWEET GPU:", "SWEET GMU:",
            "SWEET HAL: pcm write=", "panic(cpu", "Debugger called:",
        )
    }
    row = None
    if len(endpoint) == 1:
        row = dict(zip(
            ("id", "subsystem", "bar", "bytes", "soc", "sub"),
            (int(value, 0) for value in endpoint[0])))
    gates = {
        "identity": bool(identity) and all(int(v) == sweettime for v in identity),
        "single_handoff": len(handoff) == 1,
        "touch_disabled": "SWEET MARBLE TOUCH: disabled (marble_touch=0)" in text,
        "adsp_disabled": "SWEET ADSP: disabled (sweetadsp=1)" in text,
        "isolation": all(v == 0 for v in forbidden.values()),
        "single_endpoint": len(endpoint) == 1,
        "power_acknowledged": text.count("SWEET WIFI: phase2 milestone=1") == 1,
        "no_phase2_failure": not failures,
        "vendor_device": row is not None and row["id"] == 0x110317cb,
        "bar_geometry": row is not None and
            0x100000 <= row["bytes"] <= 0x2000000 and
            not row["bytes"] & (row["bytes"] - 1) and
            0x60300000 <= (row["bar"] & ~15) < 0x64000000 and
            (row["bar"] & ~15) + row["bytes"] <= 0x64000000,
    }
    valid = gates["identity"] and gates["single_handoff"] and gates["isolation"]
    return {
        "passed": all(gates.values()), "gates": gates,
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "sweettime": sweettime, "total": total, "sequence": sequence,
        "endpoint": row, "failures": failures, "forbidden_counts": forbidden,
        "classification": (
            "PCIe endpoint identified" if all(gates.values()) else
            "bounded phase2 failure" if valid and len(failures) == 1 and not endpoint else
            "incomplete or invalid evidence"),
        "not_claimed": ["firmware startup", "SMMU mapping", "DMA", "Wi-Fi association",
                        "networking", "GPU output"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--sweettime", type=int, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = score(args.raw.read_bytes(), args.sweettime)
    value = json.dumps(result, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(value + "\n")
    print(value)


if __name__ == "__main__":
    main()
