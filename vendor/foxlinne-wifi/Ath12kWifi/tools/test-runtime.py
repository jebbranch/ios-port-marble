#!/usr/bin/env python3
"""Exercise the production dispatcher with deterministic interrupts and time."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "out/runtime-test"
out.mkdir(parents=True, exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/pin-headers.sh")], check=True)
compiler = shutil.which("clang")
if not compiler:
    raise SystemExit("clang is required")
sources = [root / path for path in ("tests/runtime_test.c", "shim/src/lx_rt.c", "src/nw_rfkill.c", "src/nw_device.c")]
command = [compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-I", str(root / "shim/include"), "-I", str(root / "include"),
           "-I", str(root / "out/pinned"),
           *map(str, sources), "-o", str(out / "runtime-test")]
subprocess.run(command, cwd=root, check=True)
result = subprocess.run([str(out / "runtime-test")], cwd=root, text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"], "hardware_verified": False,
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
    "output": result.stdout,
}, indent=2) + "\n")
