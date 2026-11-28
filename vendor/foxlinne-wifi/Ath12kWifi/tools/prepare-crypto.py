#!/usr/bin/env python3
"""Stage unchanged, hash-verified portable AES providers for the kernel adapter."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
lock = json.loads((root / "tools/source-lock.json").read_text())["supplicant"]
output = root / "out/crypto"
output.mkdir(exist_ok=True)
for name in lock["required"]:
    if not name.startswith("src/crypto/"):
        continue
    source = root / "deps" / lock["directory"] / name
    destination = output / source.name
    if destination.is_symlink():
        destination.unlink()
    elif destination.exists():
        raise SystemExit(f"preserve non-generated crypto source: {destination}")
    destination.symlink_to(source)
print(f"staged locked portable crypto sources: {output}")
