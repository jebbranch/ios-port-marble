#!/usr/bin/env python3
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / "chain-tree-halstream"
BASE = "5ab596d"
FILE = Path("bsd/dev/sweet_audio_stream.c")
PATCH = ROOT / "xnu/patches/0296-sweet-hallisten-wait300s.patch"


def main():
    with tempfile.TemporaryDirectory(prefix="hallisten-wait-replay-") as name:
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
            raise ValueError("0296 replay mismatch")
    print(json.dumps({
        "passed": True, "baseline": BASE, "file": str(FILE),
        "scope": "only wait string and loop bound",
    }, indent=2))


if __name__ == "__main__":
    main()
