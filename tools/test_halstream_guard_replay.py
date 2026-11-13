#!/usr/bin/env python3
"""Apply 0294 to the committed 0293 file and compare the post-image."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree"
BASE = "a3d5127"
FILE = Path("bsd/dev/sweet_glink.c")
PATCH = ROOT / "xnu/patches/0294-sweet-halstream-s1ap3-guard.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="halstream-guard-replay-") as name:
        tree = Path(name)
        target = tree / FILE
        target.parent.mkdir(parents=True)
        target.write_bytes(subprocess.check_output(
            ["git", "-C", str(CHAIN), "show", f"{BASE}:{FILE}"]))
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", str(FILE)], check=True)
        subprocess.run(
            ["git", "-C", str(tree), "commit", "-qm", "0293 baseline"],
            check=True)
        subprocess.run(
            ["git", "-C", str(tree), "apply", "--check", "-p1", str(PATCH)],
            check=True)
        subprocess.run(
            ["git", "-C", str(tree), "apply", "-p1", str(PATCH)], check=True)
        if target.read_bytes() != (CHAIN / FILE).read_bytes():
            raise ValueError("0294 replay post-image differs from chain tree")
    print(json.dumps({
        "passed": True,
        "baseline": BASE,
        "file": str(FILE),
        "scope": "single patch apply/check and exact post-image comparison",
    }, indent=2))


if __name__ == "__main__":
    main()
