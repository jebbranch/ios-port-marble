#!/usr/bin/env python3
"""Stage explicit executor roles in generated OS-adapted Linux copies."""
import hashlib
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
out = root / "out/os-src"
out.mkdir(exist_ok=True)
inputs = {"qmi_interface.c": "drivers/soc/qcom/qmi_interface.c", "mhi-init.c": "drivers/bus/mhi/host/init.c"}
records = []
for target, source in inputs.items():
    destination = out / target
    if destination.is_symlink(): destination.unlink()
    contents = (root / "deps/linux" / source).read_bytes()
    destination.write_bytes(contents)
    records.append({"source": source, "target": target, "input_sha256": hashlib.sha256(contents).hexdigest()})
patch = root / "tools/patches/linux-completion-workqueues.patch"
subprocess.run(["patch", "--batch", "--forward", "--fuzz=0", "--no-backup-if-mismatch", "-p1", "-i", str(patch)], cwd=out, check=True)
for record in records: record["output_sha256"] = hashlib.sha256((out / record["target"]).read_bytes()).hexdigest()
destination = root / "out/mhi-src/init.c"
destination.unlink(missing_ok=True); destination.symlink_to(out / "mhi-init.c")
destination = out / "qmi_encdec.c"
destination.unlink(missing_ok=True); destination.symlink_to(root / "deps/linux/drivers/soc/qcom/qmi_encdec.c")
(root / "out/os-patches.json").write_text(json.dumps({
    "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(), "files": records,
}, indent=2) + "\n")
