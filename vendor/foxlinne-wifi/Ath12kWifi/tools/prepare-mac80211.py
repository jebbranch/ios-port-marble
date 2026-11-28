#!/usr/bin/env python3
"""Stage locked, unmodified mac80211 sources and remove tracing instrumentation."""
from pathlib import Path
import argparse
import json
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--subsystem", choices=("mac80211", "cfg80211"), default="mac80211")
args = parser.parse_args()
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
subprocess.run(["bash", str(root / "tools/pin-headers.sh")], check=True)
source = root / "deps/linux"
source_tree = "net/mac80211" if args.subsystem == "mac80211" else "net/wireless"
output = root / "out" / args.subsystem
output.mkdir(parents=True, exist_ok=True)
paths = sorted((source / source_tree).glob("*.[ch]"))
trace_names = set()
for path in paths:
    if path.name in ("trace.h", "trace_msg.h"):
        continue
    destination = output / path.name
    if destination.is_symlink():
        destination.unlink()
    elif destination.exists():
        raise SystemExit(f"preserve non-generated source: {destination}")
    destination.symlink_to(path)
    trace_names.update(re.findall(r"\b(trace_[a-zA-Z0-9_]+)\s*\(", path.read_text()))
trace = "/* Tracing is disabled; protocol sources are unchanged. */\n"
trace += "\n".join(f"#define {name}(...) ((void)0)" for name in sorted(trace_names)) + "\n"
(output / "trace.h").write_text(trace)
(output / "trace_msg.h").write_text(trace)
(output / "source.json").write_text(json.dumps({
    "linux_commit": json.loads((root / "tools/source-lock.json").read_text())["linux"]["commit"],
    "source_files": [str(path.relative_to(source)) for path in paths],
    "instrumentation_only": ["trace.h", "trace_msg.h"],
}, indent=2) + "\n")
print(f"staged {len(paths)} {args.subsystem} source/header files: {output}")
