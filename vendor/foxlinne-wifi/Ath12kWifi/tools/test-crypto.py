#!/usr/bin/env python3
"""Exercise the production mac80211 crypto adapter with upstream AES sources."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kernel", action="store_true", help="also compile arm64e iOS 27 kernel objects")
args = parser.parse_args()
subprocess.run(["python3", str(root / "tools/prepare-mac80211.py")], check=True)
subprocess.run(["python3", str(root / "tools/prepare-crypto.py")], check=True)
output = root / "out/crypto-test"
output.mkdir(exist_ok=True)
(output / "report.json").unlink(missing_ok=True)
compiler = shutil.which("clang")
if not compiler:
    raise SystemExit("clang is required")
provider = [root / "out/crypto" / (name + ".c") for name in
            ("aes-internal", "aes-internal-enc", "aes-ccm", "aes-gcm", "aes-omac1", "aes-ctr", "aes-siv")]
sources = [root / "tests/mac80211_crypto_test.c", root / "src/nw_crypto_mac80211.c", *provider]
includes = []
for directory in ("shim/crypto", "shim/include", "include", "out/pinned", "out/mac80211", "out/crypto"):
    includes += ["-I", str(root / directory)]
flags = ["-std=gnu11", "-O1", "-g", "-fno-strict-aliasing", "-fwrapv", "-Wno-macro-redefined",
         "-Daes_siv_encrypt=nw_aes_siv_encrypt", "-Daes_siv_decrypt=nw_aes_siv_decrypt", *includes]
command = [compiler, *flags, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           *map(str, sources), "-o", str(output / "crypto-test")]
subprocess.run(command, check=True, cwd=root)
result = subprocess.run([str(output / "crypto-test")], check=True, capture_output=True, text=True)
print(result.stdout, end="")
kernel = []
if args.kernel:
    resource = subprocess.check_output([compiler, "-print-resource-dir"], text=True).strip()
    kernel_flags = ["-target", "arm64e-apple-ios27.0", "-mkernel", "-ffreestanding", "-fno-builtin",
                    "-fno-common", "-fno-stack-protector", "-mgeneral-regs-only", "-nostdinc",
                    "-isystem", str(root / "shim/libc"), "-isystem", resource + "/include"]
    for source in sources[1:]:
        destination = output / (source.stem + ".kernel.o")
        subprocess.run([compiler, *flags, *kernel_flags, "-c", str(source), "-o", str(destination)], check=True)
        kernel.append({"source": str(source.relative_to(root)),
                       "object_sha256": hashlib.sha256(destination.read_bytes()).hexdigest()})
report = {"passed": True, "sanitizers": ["address", "undefined"],
          "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
          "kernel_objects": kernel, "linked": False, "hardware_verified": False,
          "output": result.stdout}
(output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
