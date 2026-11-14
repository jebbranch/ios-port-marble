#!/usr/bin/env python3
"""Replay 0298 on the committed 0297 GLINK baseline and compare post-image."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "bde3028"
FILE = Path("bsd/dev/sweet_glink.c")
PATCH = ROOT / "xnu/patches/0298-sweet-hallisten-prm-reply.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="hallisten-prm-reply-") as name:
        tree = Path(name)
        target = tree / FILE
        target.parent.mkdir(parents=True)
        target.write_bytes(subprocess.check_output(
            ["git", "-C", str(CHAIN), "show", f"{BASE}:{FILE}"]))
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", str(FILE)], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "0297"], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "--check", "-p1",
                        str(PATCH)], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "-p1", str(PATCH)],
                       check=True)
        if target.read_bytes() != (CHAIN / FILE).read_bytes():
            raise ValueError("0298 replay mismatch")
    print(json.dumps({
        "passed": True,
        "baseline": BASE,
        "file": str(FILE),
        "scope": "APM and PRM control replies retain distinct strict identities",
    }, indent=2))


if __name__ == "__main__":
    main()
