#!/usr/bin/env python3
"""Replay 0300 on the committed 0299 Wi-Fi baseline and compare post-images."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-wifi"
BASE = "3b16c7a"
POST = "6ded5c7b05918ba014082316c3bbe6a2e13165f9"
FILES = ("bsd/dev/sweet_wifi.c", "bsd/dev/sweet_wifi_phy.h")
PATCH = ROOT / "xnu/patches/0300-sweet-wifi-pcie-identify.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="wifi-pcie-replay-") as name:
        tree = Path(name)
        expected = {}
        for file in FILES:
            target = tree / file
            before = subprocess.run([
                "git", "-C", str(CHAIN), "show", f"{BASE}:{file}"],
                capture_output=True)
            if before.returncode == 0:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(before.stdout)
            elif file != "bsd/dev/sweet_wifi_phy.h":
                raise ValueError("missing baseline " + file)
            expected[file] = subprocess.check_output([
                "git", "-C", str(CHAIN), "show", f"{POST}:{file}"])
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", "."], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "0299"],
                       check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "--check", "-p1",
                        str(PATCH)], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "-p1", str(PATCH)],
                       check=True)
        for file, data in expected.items():
            if (tree / file).read_bytes() != data:
                raise ValueError("post-image mismatch " + file)
    print(json.dumps({
        "passed": True,
        "files": list(expected),
        "scope": "QCA6490 RPMh, GPIO, PHY, link and endpoint identity",
    }, indent=2))


if __name__ == "__main__":
    main()
