#!/usr/bin/env python3
"""Exercise production control handoff; transport/controller callbacks are modeled."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / "out/control-endpoint-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/pin-headers.sh")], check=True)
sources = [root / path for path in ("tests/control_endpoint_test.c", "src/nw_control.c", "src/nw_control_wire.c", "shim/src/lx_rt.c")]
compiler = shutil.which("clang")
if not compiler: raise SystemExit("clang is required")
command = [compiler, "-std=gnu11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for directory in ("shim/include", "include", "out/pinned"): command += ["-I", str(root / directory)]
subprocess.run([*command, *map(str, sources), "-o", str(out / "control-endpoint-test")], check=True)
result = subprocess.run([str(out / "control-endpoint-test")], text=True, capture_output=True, timeout=20)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"], "real_host_threads": True,
    "native_socket_connected": False, "authentication_migrated": False, "hardware_verified": False,
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                      [*sources, root / "include/nw_control.h", root / "include/nw_control_endpoint.h"]},
    "output": result.stdout,
}, indent=2) + "\n")
