#!/usr/bin/env python3
"""Emit 0301 against the exact published 0300 chain position."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-wifi"
BASE = "6ded5c7b05918ba014082316c3bbe6a2e13165f9"
FILE = "bsd/dev/sweet_wifi.c"
PATCH = ROOT / "xnu/patches/0301-sweet-wifi-tcs-irq-guards.patch"


def main():
    data = subprocess.check_output([
        "git", "-C", str(CHAIN), "diff", BASE, "--", FILE])
    if data.count(b"diff --git ") != 1:
        raise ValueError("0301 must change exactly one kernel file")
    for required in (
        b"SW_WIFI_TCS_PA", b"0x17a20d00u",
        b"extern void IOSleep(unsigned milliseconds);",
        b"extern void IODelay(unsigned microseconds);",
        b"SWEET MARK: wifi pcie v2",
    ):
        if required not in data:
            raise ValueError("missing 0301 fix: " + str(required))
    PATCH.write_bytes(data)
    print(f"{PATCH}: {len(data)} bytes")


if __name__ == "__main__":
    main()
