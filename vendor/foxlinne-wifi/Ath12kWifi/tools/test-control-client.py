#!/usr/bin/env python3
"""Connect the user client and production worker handoff over real host sockets."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / "out/control-client-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/pin-headers.sh")], check=True)
sources = [root / path for path in ("tests/control_client_test.c", "userspace/nw_control_client.c",
           "src/nw_control.c", "src/nw_control_wire.c", "shim/src/lx_rt.c")]
compiler = shutil.which("clang")
if not compiler: raise SystemExit("clang is required")
command = [compiler, "-std=gnu11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for directory in ("shim/include", "include", "userspace", "out/pinned"): command += ["-I", str(root / directory)]
subprocess.run([*command, *map(str, sources), "-o", str(out / "control-client-test")], check=True)
result = subprocess.run([str(out / "control-client-test")], text=True, capture_output=True, timeout=20)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "real_host_sockets": True, "sanitizers": ["address", "undefined"],
    "native_xnu_socket_verified": False, "authentication_migrated": False, "hardware_verified": False,
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
    "output": result.stdout,
}, indent=2) + "\n")
