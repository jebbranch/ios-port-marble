#!/usr/bin/env python3
"""Apply 0301 to the actual 0300 file and compare with the corrected chain."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-wifi"
BASE = "6ded5c7b05918ba014082316c3bbe6a2e13165f9"
FILE = "bsd/dev/sweet_wifi.c"
PATCH = ROOT / "xnu/patches/0301-sweet-wifi-tcs-irq-guards.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="wifi-0301-replay-") as folder:
        tree = Path(folder)
        target = tree / FILE
        target.parent.mkdir(parents=True)
        target.write_bytes(subprocess.check_output([
            "git", "-C", str(CHAIN), "show", BASE + ":" + FILE]))
        subprocess.run(["git", "-C", folder, "apply", "--check", str(PATCH)],
                       check=True)
        subprocess.run(["git", "-C", folder, "apply", str(PATCH)], check=True)
        if target.read_bytes() != (CHAIN / FILE).read_bytes():
            raise ValueError("0301 post-image differs from chain")
    print(json.dumps({"passed": True, "baseline": BASE, "file": FILE,
                      "scope": "0300 baseline check/apply/exact post-image"}))


if __name__ == "__main__":
    main()
