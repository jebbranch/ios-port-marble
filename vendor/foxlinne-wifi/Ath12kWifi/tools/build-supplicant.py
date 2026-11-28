#!/usr/bin/env python3
"""Build the original supplicant plus the native XNU backend, without libnl."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--target", choices=("host", "ios"), default="ios")
parser.add_argument("--jobs", type=int, default=8)
args = parser.parse_args()
subprocess.run(["python3", str(root / "tools/prepare-supplicant.py")], check=True)
output = root / f"out/wpa-{args.target}"
output.mkdir(exist_ok=True)
(output / "report.json").unlink(missing_ok=True)
compiler = shutil.which("clang")
if not compiler: raise SystemExit("clang is required")
flags = ["-O2", "-g", "-Wall", "-MMD", "-Werror=implicit-function-declaration",
         "-I" + str(root / "include"), "-I" + str(root / "userspace"),
         "-I" + str(root / "out/wpa-src/wpa_supplicant")]
link = []
if args.target == "ios":
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    link = ["-target", "arm64e-apple-ios18.0", "-isysroot", sdk]
    flags += link
command = ["make", f"-j{args.jobs}", "wpa_supplicant", "V=1", f"CC={compiler}",
           f"BUILDDIR={output}", f"EXTRA_CFLAGS={shlex.join(flags)}", f"LDFLAGS={shlex.join(link)}"]
# Upstream shares this output between target-specific object directories.
# Relink once even if the previously built product belongs to another target.
(root / "out/wpa-src/wpa_supplicant/wpa_supplicant").unlink(missing_ok=True)
with (output / "build.log").open("w") as log:
    result = subprocess.run(command, cwd=root / "out/wpa-src/wpa_supplicant", stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    lines = (output / "build.log").read_text().splitlines()
    print("\n".join(line for line in lines if not line.startswith(compiler))[-12000:])
    raise SystemExit(result.returncode)
binary = output / "bin/wpa_supplicant"
binary.parent.mkdir(exist_ok=True)
shutil.copy2(root / "out/wpa-src/wpa_supplicant/wpa_supplicant", binary)
(output / "report.json").write_text(json.dumps({
    "target": args.target, "built": True, "hardware_verified": False,
    "authentication_algorithms_modified": False,
    "native_adapter": json.loads((root / "out/wpa-src/native-adapter.json").read_text()),
    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "adapter_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in [*sorted((root / "userspace").rglob("*.[ch]")),
                                    root / "userspace/supplicant.config", root / "include/nw_control.h",
                                    root / "src/nw_control_wire.c"]},
    "command": command,
    "prepare_script_sha256": hashlib.sha256((root / "tools/prepare-supplicant.py").read_bytes()).hexdigest(),
}, indent=2) + "\n")
print(f"built {args.target} native supplicant: {binary}")
