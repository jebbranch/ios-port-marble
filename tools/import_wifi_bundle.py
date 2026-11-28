#!/usr/bin/env python3
"""Import one immutable FoXLinne Wi-Fi source/board snapshot with file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
REV = "7a0d68fff2557f12be9f8c47f618fd332b7a3e67"
BOARD = (
    "MarbleWifi.cpp", "MarbleWifi.h", "MarbleWifiPhy.h",
    "MarbleWifiRuntime.cpp", "MarbleWifiDma.inc", "MarbleWifiArena.inc",
    "MarbleWifiControl.inc", "MarbleWifiNet.cpp",
    "MarbleWifiFirmware.c", "MarbleWifiFirmware.h",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT.parent.parent / "refs/FoXLinne-UniXNU_marble")
    args = parser.parse_args()
    source = args.source.resolve()
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != REV:
        raise ValueError("source must be the reviewed immutable snapshot")
    target = ROOT / "vendor/foxlinne-wifi"
    if target.exists():
        raise ValueError("refusing to replace an existing import")
    changed = subprocess.check_output([
        "git", "-C", str(source), "status", "--porcelain", "--",
        "package", "ios18/sources/Ath12kWifi", "scripts/build_marble_wifi.py",
        "profiles/wifi-wcn6855-hw21.json",
    ], text=True)
    if changed.strip():
        raise ValueError("source Wi-Fi snapshot is dirty")
    target.mkdir(parents=True)
    # Upstream agent rules concern development in their repository, not this
    # attributed read-only snapshot. Runtime/source manifests remain intact.
    shutil.copytree(source / "ios18/sources/Ath12kWifi", target / "Ath12kWifi",
                    ignore=shutil.ignore_patterns("AGENTS.md", ".git", "deps", "out", "__pycache__"))
    (target / "board").mkdir()
    for name in BOARD:
        shutil.copyfile(source / "package" / name, target / "board" / name)
    shutil.copyfile(source / "LICENSE", target / "LICENSE")
    shutil.copyfile(source / "scripts/build_marble_wifi.py", target / "build_marble_wifi.py")
    shutil.copyfile(source / "profiles/wifi-wcn6855-hw21.json", target / "firmware-manifest.json")
    hashes = {
        str(p.relative_to(target)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(target.rglob("*")) if p.is_file()
    }
    manifest = {
        "repository": "https://github.com/FoXLinne/UniXNU_marble",
        "revision": REV,
        "status": "attributed source import, not kernel integration",
        "backend": "ath11k WCN6855 hw2.1",
        "target": "XNU 8796 / iOS 16 / arm64",
        "upstream_target": "XNU 11417 / iOS 18 / arm64e",
        "files_sha256": hashes,
        "license_notes": [
            "Preserve per-file SPDX and upstream copyright notices.",
            "Linux GPL code is present in generated dependencies; redistribution requires source.",
            "Firmware redistribution is governed separately by Notice.txt.",
        ],
    }
    (target / "IMPORT.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"target": str(target), "files": len(hashes), "revision": REV}))


if __name__ == "__main__":
    main()
