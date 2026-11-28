#!/usr/bin/env python3
"""Exercise production atomic locks and skb queues with real host threads."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / "out/locks-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/pin-headers.sh")], check=True)
source = root / "tests/locks_test.c"
command = [shutil.which("clang"), "-std=gnu11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-Wno-macro-redefined"]
for directory in ("shim/include", "include", "out/pinned"):
    command += ["-I", str(root / directory)]
command += [str(source), "-o", str(out / "locks-test")]
subprocess.run(command, check=True)
result = subprocess.run([str(out / "locks-test")], text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "real_host_threads": True, "sanitizers": ["address", "undefined"], "hardware_verified": False,
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in (source, root / "shim/include/lx.h", root / "shim/include/lx_skb.h")},
    "output": result.stdout,
}, indent=2) + "\n")
