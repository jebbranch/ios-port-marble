#!/usr/bin/env python3
"""Emit 0295 from the committed 0294/0293 chain positions."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
CHAINS = (
    ("chain-tree", "c6b8cce9cd40a95ec0d39d91908b859f4c4b934a",
     ("bsd/dev/sweet_glink.c",)),
    ("chain-tree-i2c", "3c1bcdd6548703165ede9b51062734792199dbe5",
     ("bsd/dev/sweet_i2c.c",)),
    ("chain-tree-halstream", "7730a5bfb5c87f45c26b81ab0fde4f7ac15d04bc",
     ("bsd/dev/sweet_audio_periods.c", "bsd/dev/sweet_audio_periods.h",
      "bsd/dev/sweet_audio_stream.c", "bsd/dev/sweet_audio_stream.h")),
)


def main():
    parts = []
    for directory, base, files in CHAINS:
        parts.append(subprocess.check_output([
            "git", "-C", str(ROOT.parent / directory), "diff", base, "--", *files
        ]))
    data = b"".join(parts)
    if data.count(b"diff --git ") != 6:
        raise ValueError("0295 must touch exactly six files")
    required = (
        b"SWEET MARK: listen v1 started=1 peak-limit=16384 volume-reg=0x05a9",
        b"SWEET MARK: listen v1 stopped=1",
        b"SWEET MARK: listen v1 awcfg-sent=1",
        b"marble_pcm_output",
        b"pre_play",
        b"post_play",
    )
    if not all(item in data for item in required):
        raise ValueError("0295 safety or identity marker missing")
    output = ROOT / "xnu/patches/0295-sweet-hal-aw88266-listen.patch"
    output.write_bytes(data)
    print(f"{output}: {len(data)} bytes, six files")


if __name__ == "__main__":
    main()
