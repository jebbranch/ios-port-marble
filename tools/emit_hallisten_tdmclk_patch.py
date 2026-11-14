#!/usr/bin/env python3
"""Emit 0297: exact vendor 0x204 TDM clock lifetime for bounded listen."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "016bd75"
FILE = "bsd/dev/sweet_glink.c"


def main():
    data = subprocess.check_output(
        ["git", "-C", str(CHAIN), "diff", BASE, "--", FILE])
    if data.count(b"diff --git ") != 1:
        raise ValueError("0297 must touch one kernel file")
    required = (
        b"SWEET MARK: listen tdmclk v1 request rc=%d",
        b"SWEET MARK: listen tdmclk v1 release rc=%d",
        b"SWEET MARK: halstream teardown v2 open=%d stop=%d close=%d ",
        b"tdmrel=%d unmap=%d",
        b"g_put32(packet + 60, 0x204u)",
        b"g_put32(packet + 64, 12288000u)",
        b"g_put32(packet + 68, 4u)",
    )
    if not all(item in data for item in required):
        raise ValueError("0297 clock identity or safety marker missing")
    output = ROOT / "xnu/patches/0297-sweet-hallisten-tdm-clock.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, one file")


if __name__ == "__main__":
    main()
