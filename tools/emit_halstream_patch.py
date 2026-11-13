#!/usr/bin/env python3
"""Emit 0293 from explicit last-published chain revisions, never current HEAD^."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAINS = (
    ("chain-tree", "7d24fbc3f9c59ced47db49314d9f120b24c96ab1",
     ("bsd/dev/sweet_glink.c",)),
    ("chain-tree-i2c", "06b4875c7684e5880c85686bca6850d715efbc96",
     ("bsd/dev/sweet_i2c.c",)),
    ("chain-tree-conf", "af6ead7d8ae8ac302a677ffb2e67d2c08c050877",
     ("bsd/conf/files",)),
    ("chain-tree-halbridge", "b0a29db",
     ("bsd/dev/sweet_audio_bridge.c",)),
    ("chain-tree-halstream", "7e69fbde9ab5c40f931e0dae692b49915d78e70b",
     ("bsd/dev/sweet_audio_periods.c", "bsd/dev/sweet_audio_periods.h",
      "bsd/dev/sweet_audio_stream.c", "bsd/dev/sweet_audio_stream.h")),
)


def main():
    parts = []
    for directory, base, files in CHAINS:
        tree = ROOT.parent / directory
        parts.append(subprocess.check_output(
            ["git", "-C", str(tree), "diff", base, "--", *files]))
    patch = ROOT / "xnu/patches/0293-sweet-hal-bounded-stream.patch"
    data = b"".join(parts)
    if data.count(b"diff --git ") != 8 or not data:
        raise ValueError("expected exactly eight explicit kernel files")
    patch.write_bytes(data)
    print(f"{patch}: {len(data)} bytes, 8 files")


if __name__ == "__main__":
    main()
