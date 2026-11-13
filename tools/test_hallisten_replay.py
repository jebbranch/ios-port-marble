#!/usr/bin/env python3
"""Apply 0295 to explicit baselines and compare all six post-images."""
from pathlib import Path
import json
import subprocess
import tempfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from emit_hallisten_patch import CHAINS, ROOT


def main():
    patch = ROOT / "xnu/patches/0295-sweet-hal-aw88266-listen.patch"
    with tempfile.TemporaryDirectory(prefix="hallisten-replay-") as folder:
        tree = Path(folder)
        expected = {}
        for directory, base, files in CHAINS:
            source = ROOT.parent / directory
            for file in files:
                target = tree / file
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(subprocess.check_output([
                    "git", "-C", str(source), "show", base + ":" + file]))
                expected[file] = (source / file).read_bytes()
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", "."], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "baseline"], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "--check", "-p1",
                        str(patch)], check=True)
        subprocess.run(["git", "-C", str(tree), "apply", "-p1", str(patch)],
                       check=True)
        for file, data in expected.items():
            if (tree / file).read_bytes() != data:
                raise ValueError("post-image mismatch " + file)
    print(json.dumps({
        "passed": True, "files": list(expected),
        "scope": "six-file apply/check and exact post-image comparison",
    }, indent=2))


if __name__ == "__main__":
    main()
