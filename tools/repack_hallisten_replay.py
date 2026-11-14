#!/usr/bin/env python3
"""Create an exact free replay by changing only the 10-digit sweettime."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "images/hallisten-0295-20261114.img"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_builder():
    path = ROOT / "tools/build_hallisten_image.py"
    spec = importlib.util.spec_from_file_location("hallisten_builder", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sweettime", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    if not 1000000000 <= args.sweettime <= 9999999999:
        parser.error("sweettime must be exactly ten decimal digits")
    source = SOURCE.read_bytes()
    hits = list(re.finditer(rb"sweettime=(\d{10})", source))
    if len(hits) != 1:
        raise SystemExit("source must contain exactly one ten-digit sweettime")
    hit = hits[0]
    old = int(hit.group(1))
    replacement = str(args.sweettime).encode()
    changed = source[:hit.start(1)] + replacement + source[hit.end(1):]
    if len(changed) != len(source):
        raise AssertionError("replay image size changed")
    differences = [index for index, pair in enumerate(zip(source, changed))
                   if pair[0] != pair[1]]
    allowed = set(range(hit.start(1), hit.end(1)))
    if not differences or not set(differences) <= allowed:
        raise AssertionError("bytes outside sweettime changed")
    if changed.count(b"sweettime=" + replacement) != 1:
        raise AssertionError("new identity is not unique")
    builder = load_builder()
    # Extract and re-check the payload command line from the unchanged boot image.
    manifest = json.loads((ROOT / "out/hallisten-0295-20261114/manifest.json").read_text())
    old_command = manifest["cmdline"]
    new_command = re.sub(r"sweettime=\d{10}",
                         "sweettime=" + str(args.sweettime), old_command)
    builder.flags(new_command)
    if len(new_command.encode()) != 590:
        raise AssertionError("replay command line length changed")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(changed)
    result = {
        "source": str(SOURCE), "source_sha256": sha(source),
        "image": str(args.output), "image_sha256": sha(changed),
        "bytes": len(changed), "old_sweettime": old,
        "sweettime": args.sweettime, "cmdline": new_command,
        "cmdline_bytes": len(new_command.encode()),
        "differing_byte_count": len(differences),
        "difference_offsets": differences,
        "only_sweettime_changed": True,
        "same_kernel_and_configuration": True,
        "device_booted": False, "budget_charge": 0,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
