#!/usr/bin/env python3
"""Replay 0297 on the committed 0295 GLINK baseline and compare post-image."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "016bd75"
FILE = Path("bsd/dev/sweet_glink.c")
PATCH = ROOT / "xnu/patches/0297-sweet-hallisten-tdm-clock.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="hallisten-tdmclk-replay-") as name:
        tree = Path(name)
        target = tree / FILE
        target.parent.mkdir(parents=True)
        target.write_bytes(subprocess.check_output(
            ["git", "-C", str(CHAIN), "show", f"{BASE}:{FILE}"]))
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", str(FILE)], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "0295"], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "--check", "-p1",
                        str(PATCH)], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "-p1", str(PATCH)],
                       check=True)
        if target.read_bytes() != (CHAIN / FILE).read_bytes():
            raise ValueError("0297 replay mismatch")
    print(json.dumps({
        "passed": True,
        "baseline": BASE,
        "file": str(FILE),
        "scope": "vendor request/release packet, strict reply binding and teardown",
    }, indent=2))


if __name__ == "__main__":
    main()
