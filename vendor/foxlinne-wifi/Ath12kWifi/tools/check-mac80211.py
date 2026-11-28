#!/usr/bin/env python3
"""Compile original mac80211 units; retain exact source/error coverage per unit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--subsystem", choices=("mac80211", "cfg80211"), default="mac80211")
parser.add_argument("--files", nargs="+")
mac80211_files = [
    "main", "driver-ops", "sta_info", "scan", "offchannel", "ht", "agg-tx", "agg-rx",
    "vht", "he", "eht", "iface", "link", "rate", "cfg", "rx", "tx", "key",
    "util", "parse", "wme", "chan", "mlme", "status", "s1g", "ibss",
    "michael", "tkip", "wep", "wpa", "spectmgmt", "tdls", "ocb", "airtime", "uhr", "wbrf",
]
parser.add_argument("--kernel", action="store_true", help="compile arm64e iOS 27 kernel objects")
args = parser.parse_args()
if args.files is None:
    args.files = mac80211_files if args.subsystem == "mac80211" else ["core", "scan", "chan", "util", "mlme", "sme", "reg", "radiotap", "ibss", "mesh", "ap", "ocb"]
subprocess.run(["python3", str(root / "tools/prepare-mac80211.py"), "--subsystem", args.subsystem], check=True)
output = root / "out" / args.subsystem / ("kernel" if args.kernel else "host")
output.mkdir(exist_ok=True)
compiler = shutil.which("clang")
if not compiler:
    raise SystemExit("clang is required")
flags = ["-c", "-O2", "-std=gnu11", "-fno-strict-aliasing", "-fwrapv", "-ferror-limit=15", "-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0",
         "-DCONFIG_CFG80211=1", "-DCONFIG_MAC80211=1",
         '-DCONFIG_MAC80211_RC_DEFAULT=""',
         "-Wno-incompatible-pointer-types", "-Wno-pointer-sign", "-Wno-macro-redefined", "-include", "lx_pci.h"]
if args.kernel:
    resource = subprocess.check_output([compiler, "-print-resource-dir"], text=True).strip()
    flags += ["-target", "arm64e-apple-ios27.0", "-mkernel", "-ffreestanding", "-fno-builtin",
              "-fno-common", "-fno-stack-protector", "-mgeneral-regs-only", "-nostdinc",
              "-isystem", str(root / "shim/libc"), "-isystem", resource + "/include"]
for directory in ("shim/include", "include", "out/pinned", "out/" + args.subsystem):
    flags += ["-I", str(root / directory)]
results = []
interface_headers = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for directory in ("shim/include", "shim/libc", "include", "out/pinned")
                     for path in sorted((root / directory).rglob("*.h")) if path.is_file()}
interface_hash = hashlib.sha256(json.dumps(interface_headers, sort_keys=True).encode()).hexdigest()
for name in args.files:
    if not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in name):
        raise SystemExit(f"invalid translation unit: {name}")
    source = root / "out" / args.subsystem / f"{name}.c"
    destination = output / f"{name}.o"
    command = [compiler, *flags, str(source), "-o", str(destination)]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    (output / f"{name}.log").write_text(result.stdout + result.stderr)
    record = {"unit": name, "compiled": result.returncode == 0,
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "command": command}
    if record["compiled"]:
        record["object_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    else:
        destination.unlink(missing_ok=True)
        record["first_errors"] = [line for line in result.stderr.splitlines() if "error:" in line][:4]
    results.append(record)
    print(f"{'compiled' if record['compiled'] else 'pending'}: {name}")
    for line in record.get("first_errors", []):
        print("  " + line)
report = {"linux_commit": json.loads((root / "tools/source-lock.json").read_text())["linux"]["commit"],
          "subsystem": args.subsystem, "kernel_target": args.kernel, "units": results, "linked": False, "hardware_verified": False}
report["interface_sha256"] = interface_hash
(output / "compile-report.json").write_text(json.dumps(report, indent=2) + "\n")
raise SystemExit(0 if all(item["compiled"] for item in results) else 1)
