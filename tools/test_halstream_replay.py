#!/usr/bin/env python3
"""Replay 0293 on explicit baselines; require every post-image to match."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from emit_halstream_patch import CHAINS, ROOT


def main():
    patch = ROOT / "xnu/patches/0293-sweet-hal-bounded-stream.patch"
    with tempfile.TemporaryDirectory(prefix="halstream-replay-") as directory:
        tree = Path(directory)
        expected = {}
        for name, base, files in CHAINS:
            source = ROOT.parent / name
            for file in files:
                expected[file] = (source / file).read_bytes()
                target = tree / file
                target.parent.mkdir(parents=True, exist_ok=True)
                before = subprocess.run(
                    ["git", "-C", str(source), "show", base + ":" + file],
                    check=False, capture_output=True)
                if before.returncode == 0:
                    target.write_bytes(before.stdout)
                elif name != "chain-tree-halstream":
                    raise ValueError("baseline missing " + file)
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", "."], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "baseline"], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "--check", "-p1", str(patch)], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "-p1", str(patch)], check=True)
        for file, data in expected.items():
            if (tree / file).read_bytes() != data:
                raise ValueError("post-image mismatch " + file)
    print(json.dumps({"passed": True, "files": list(expected),
                      "scope": "git apply --check/apply and exact eight-file comparison"},
                     indent=2))


if __name__ == "__main__":
    main()
