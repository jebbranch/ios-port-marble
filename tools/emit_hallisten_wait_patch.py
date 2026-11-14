#!/usr/bin/env python3
"""Emit 0296: only the bounded HAL wait changes from 120 s to 300 s."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-halstream"
BASE = "5ab596d"
FILE = "bsd/dev/sweet_audio_stream.c"


def main():
    data = subprocess.check_output(
        ["git", "-C", str(CHAIN), "diff", BASE, "--", FILE])
    if data.count(b"diff --git ") != 1:
        raise ValueError("0296 must touch one file")
    required = (b"wait-ms=300000", b"attempt < 30000u")
    changed = b"\n".join(line for line in data.splitlines()
                         if line.startswith((b"+", b"-"))
                         and not line.startswith((b"+++", b"---")))
    forbidden = (b"divisor=", b"ceiling=", b"pre_play", b"post_play")
    if not all(item in changed for item in required) or any(
            item in changed for item in forbidden):
        raise ValueError("0296 changed more than the wait window")
    output = ROOT / "xnu/patches/0296-sweet-hallisten-wait300s.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, one file")


if __name__ == "__main__":
    main()
