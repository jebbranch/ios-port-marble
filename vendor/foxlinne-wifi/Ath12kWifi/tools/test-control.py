#!/usr/bin/env python3
"""Check the shared native control framing on the host and kernel target."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / "out/control-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
compiler = shutil.which("clang")
if not compiler: raise SystemExit("clang is required")
sources = [root / "tests/control_wire_test.c", root / "src/nw_control_wire.c"]
subprocess.run([compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-I", str(root / "include"),
                *map(str, sources), "-o", str(out / "control-test")], check=True)
result = subprocess.run([str(out / "control-test")], text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
resource = subprocess.check_output([compiler, "-print-resource-dir"], text=True).strip()
kernel = out / "nw_control_wire.kernel.o"
subprocess.run([compiler, "-std=gnu11", "-O2", "-target", "arm64e-apple-ios27.0", "-mkernel", "-ffreestanding",
                "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only", "-nostdinc",
                "-isystem", str(root / "shim/libc"), "-isystem", resource + "/include", "-I", str(root / "include"),
                "-c", str(sources[1]), "-o", str(kernel)], check=True)
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"], "hardware_verified": False,
    "transport_connected": False, "authentication_migrated": False,
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in [*sources, root / "include/nw_control.h"]},
    "kernel_object_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "output": result.stdout,
}, indent=2) + "\n")
