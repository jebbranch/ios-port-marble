#!/usr/bin/env python3
"""Fetch immutable WCN6855 blobs and verify the reviewed firmware manifest."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUNDLE = ROOT / "vendor/foxlinne-wifi"
OUTPUT = ROOT / "out/wifi-full/firmware"


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((BUNDLE / "firmware-manifest.json").read_text())
    for entry in manifest["files"]:
        target = OUTPUT / entry["name"]
        if target.exists() and target.stat().st_size == entry["bytes"]:
            if hashlib.sha256(target.read_bytes()).hexdigest() == entry["sha256"]:
                print("verified", entry["name"])
                continue
            raise ValueError("existing firmware digest mismatch: " + str(target))
        # Use the exact Git blob object rather than a mutable master path.
        url = ("https://api.github.com/repos/endlessm/linux-firmware/git/blobs/" +
               entry["git_blob"])
        subprocess.run([
            "curl", "-fsSL", "-C", "-", "--max-time", "55",
            "-H", "Accept: application/vnd.github.raw+json",
            "-o", str(target), url,
        ], check=True)
        if target.stat().st_size != entry["bytes"]:
            raise ValueError("firmware size mismatch: " + entry["name"])
        if hashlib.sha256(target.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("firmware digest mismatch: " + entry["name"])
        print("verified", entry["name"])
    (OUTPUT / "verified-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
