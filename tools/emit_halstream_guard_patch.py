#!/usr/bin/env python3
"""Emit 0294 from the committed 0293 chain position only."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "a3d5127"
FILE = "bsd/dev/sweet_glink.c"


def main():
    data = subprocess.check_output(
        ["git", "-C", str(CHAIN), "diff", BASE, "--", FILE])
    if data.count(b"diff --git ") != 1:
        raise ValueError("0294 must touch exactly sweet_glink.c")
    required = (
        b'"sweets1ap", &s1ap',
        b"s1ap != 3",
        b"SWEET MARK: halstream guard v2 pass=%d s1ap=%d",
    )
    if not all(item in data for item in required):
        raise ValueError("0294 guard evidence is incomplete")
    output = ROOT / "xnu/patches/0294-sweet-halstream-s1ap3-guard.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, one file")


if __name__ == "__main__":
    main()
