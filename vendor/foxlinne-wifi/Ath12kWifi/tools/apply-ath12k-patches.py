#!/usr/bin/env python3
"""Apply reviewed patches to generated ath12k copies and record their identity."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
tree = root / "out/ath12k-src"
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
records = []
patches = [("ath12k-vht-rx-mcs.patch", ["dp_rx.c"]),
           ("ath12k-encrypted-eapol.patch", ["dp_tx.c", "dp_tx.h", "wifi7/dp_tx.c"])]
for name, targets in patches:
    patch = root / "tools/patches" / name
    inputs = {}
    for target in targets:
        destination = tree / target
        inputs[target] = destination.read_bytes()
        if destination.is_symlink():
            destination.unlink()
            destination.write_bytes(inputs[target])
    subprocess.run(["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch)], cwd=tree, check=True)
    for target in targets:
        original = root / "deps/linux/drivers/net/wireless/ath/ath12k" / target
        records.append({"patch": name, "target": target,
                        "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
                        "upstream_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                        "input_sha256": hashlib.sha256(inputs[target]).hexdigest(),
                        "output_sha256": hashlib.sha256((tree / target).read_bytes()).hexdigest()})
(root / "out/ath12k-patches.json").write_text(json.dumps(records, indent=2) + "\n")
