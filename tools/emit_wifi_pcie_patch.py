#!/usr/bin/env python3
"""Emit 0300: QCA6490 power, PCIe0 PHY/link and endpoint identification."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-wifi"
BASE = "3b16c7a"
FILES = ("bsd/dev/sweet_wifi.c", "bsd/dev/sweet_wifi_phy.h")


def main():
    data = subprocess.check_output([
        "git", "-C", str(CHAIN), "diff", BASE, "--", *FILES])
    if data.count(b"diff --git ") != 2:
        raise ValueError("0300 must touch exactly two Wi-Fi files")
    required = (
        b"SWEET MARK: wifi pcie v1 stage=%d id=%#x subsystem=%#x "
        b"bar=%#x bytes=%#x soc=%#x sub=%#x",
        b"smpe2", b"smpb11", b"ldoc10", b"cx.lvl", b"SN7",
        b"0x1103u", b"0x17cbu", b"sw_wifi_rpmh_poisoned",
        b"FoXLinne/UniXNU_marble",
    )
    if not all(item in data for item in required):
        raise ValueError("0300 identity, resource list, safety or attribution missing")
    output = ROOT / "xnu/patches/0300-sweet-wifi-pcie-identify.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, two files")


if __name__ == "__main__":
    main()
