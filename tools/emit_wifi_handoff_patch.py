#!/usr/bin/env python3
"""Emit 0299 from explicit Wi-Fi, conf and UFS chain baselines."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAINS = (
    ("chain-tree-wifi", "2eebee3466cb94b84fe670bc6b8c26a0c2761868",
     ("bsd/dev/sweet_wifi.c",)),
    ("chain-tree-conf", "7749711f83079c2b5487d0e8f0b2e30aaa9cfe63",
     ("bsd/conf/files",)),
    ("chain-tree-ufs", "7c472fd05d42f531042bd9c87271b41cdd2cd5d2",
     ("bsd/dev/sweet_ufs.c",)),
)


def main():
    parts = []
    for directory, base, files in CHAINS:
        parts.append(subprocess.check_output([
            "git", "-C", str(ROOT.parent / directory),
            "diff", base, "--", *files,
        ]))
    data = b"".join(parts)
    if data.count(b"diff --git ") != 3:
        raise ValueError("0299 must touch exactly three files")
    required = (
        b"SWEET MARK: wifi handoff v1 readonly=1 stage=%d "
        b"gdsc=%#x halted=%#x wlan=%#x perst=%#x",
        b"bsd/dev/sweet_wifi.c",
        b"sweet_wifi_init();",
        b"FoXLinne/UniXNU_marble",
    )
    if not all(item in data for item in required):
        raise ValueError("0299 identity, build or attribution missing")
    output = ROOT / "xnu/patches/0299-sweet-wifi-readonly-handoff.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, three files")


if __name__ == "__main__":
    main()
