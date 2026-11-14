#!/usr/bin/env python3
"""Emit 0298: bind PRM clock completion by its real response type and param."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "bde3028"
FILE = "bsd/dev/sweet_glink.c"


def main():
    data = subprocess.check_output(
        ["git", "-C", str(CHAIN), "diff", BASE, "--", FILE])
    if data.count(b"diff --git ") != 1:
        raise ValueError("0298 must touch one kernel file")
    required = (
        b"g_hal_xfer_reply",
        b"0x02001002u, PARAM_ID_RSC_AUDIO_HW_CLK",
        b"SWEET MARK: listen tdmclk v2 request rc=%d",
        b"SWEET MARK: listen tdmclk v2 release rc=%d",
    )
    if not all(item in data for item in required):
        raise ValueError("0298 PRM reply identity missing")
    output = ROOT / "xnu/patches/0298-sweet-hallisten-prm-reply.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, one file")


if __name__ == "__main__":
    main()
