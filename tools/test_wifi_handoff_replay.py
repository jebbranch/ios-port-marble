#!/usr/bin/env python3
"""Replay 0299 on explicit baselines and require exact three-file post-images."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PATCH = ROOT / "xnu/patches/0299-sweet-wifi-readonly-handoff.patch"
CHAINS = (
    ("chain-tree-wifi", "2eebee3466cb94b84fe670bc6b8c26a0c2761868", "3b16c7a",
     ("bsd/dev/sweet_wifi.c",)),
    ("chain-tree-conf", "7749711f83079c2b5487d0e8f0b2e30aaa9cfe63", "82eb50e",
     ("bsd/conf/files",)),
    ("chain-tree-ufs", "7c472fd05d42f531042bd9c87271b41cdd2cd5d2", "82892f3",
     ("bsd/dev/sweet_ufs.c",)),
)


def main():
    with tempfile.TemporaryDirectory(prefix="wifi-handoff-replay-") as name:
        tree = Path(name)
        expected = {}
        for directory, base, post, files in CHAINS:
            source = ROOT.parent / directory
            for file in files:
                target = tree / file
                before = subprocess.run(
                    ["git", "-C", str(source), "show", f"{base}:{file}"],
                    capture_output=True)
                if before.returncode == 0:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(before.stdout)
                elif directory != "chain-tree-wifi":
                    raise ValueError("missing baseline " + file)
                expected[file] = subprocess.check_output([
                    "git", "-C", str(source), "show", f"{post}:{file}"])
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(tree), "add", "."], check=True)
        subprocess.run(["git", "-C", str(tree), "commit", "-qm", "baseline"],
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
        "scope": "read-only Wi-Fi source, build entry and startup hook",
    }, indent=2))


if __name__ == "__main__":
    main()
