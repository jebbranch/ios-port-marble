#!/usr/bin/env python3
"""Run the real supplicant/backend against an offline AP; no Wi-Fi hardware."""
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
report_path = root / "out/wpa-host/report.json"
report = json.loads(report_path.read_text()) if report_path.exists() else {}
expected = report.get("adapter_sha256", {})
current = bool(expected) and all((root / name).exists() and hashlib.sha256((root / name).read_bytes()).hexdigest() == digest
                               for name, digest in expected.items())
current &= report.get("prepare_script_sha256") == hashlib.sha256((root / "tools/prepare-supplicant.py").read_bytes()).hexdigest()
if not current:
    subprocess.run(["python3", str(root / "tools/build-supplicant.py"), "--target", "host"], check=True)
out = root / "out/supplicant-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
build = root / "out/wpa-host/wpa_supplicant"
commands = [shlex.split(line) for line in (root / "out/wpa-host/build.log").read_text().splitlines()
            if ' -o wpa_supplicant ' in line]
if len(commands) != 1: raise SystemExit("missing actual supplicant link command")
objects = [item for item in commands[0] if item.endswith(".o") and item not in
           (str(build / "main.o"), str(build / "src/drivers/nw_control_client.o"))]
flags = ["-O1", "-g", "-pthread", "-DCONFIG_CTRL_IFACE", "-DCONFIG_DRIVER_SHENG", "-DCONFIG_SME", "-DCONFIG_NO_TKIP"]
for directory in ("out/wpa-src/src", "out/wpa-src/src/utils", "out/wpa-src/wpa_supplicant", "include", "userspace"):
    flags += ["-I", str(root / directory)]
compiler = commands[0][0]
subprocess.run([compiler, *flags, "-Dnw_control_client_open=nw_unused_native_open", "-c",
    str(root / "userspace/nw_control_client.c"), "-o", str(out / "client.o")], check=True)
subprocess.run([compiler, *flags, str(root / "tests/supplicant_test.c"),
    str(root / "out/wpa-src/src/crypto/aes-wrap.c"), str(out / "client.o"),
    *objects, "-o", str(out / "supplicant-test")], check=True)
config = out / "test.conf"
config.write_text("ap_scan=1\n")
output = ""
for mode in ("wpa2", "open", "wrong"):
    try:
        result = subprocess.run([str(out / "supplicant-test"), str(config), mode], capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode(), end="")
        print((error.stderr or b"").decode(), end="")
        raise SystemExit(f"offline {mode} supplicant timed out")
    print(result.stdout, end=""); print(result.stderr, end="")
    if result.returncode: raise SystemExit(result.returncode)
    output += result.stdout
(out / "report.json").write_text(json.dumps({"passed": True, "original_supplicant_sme_and_wpa": True,
    "physical_radio_simulated": True, "hardware_verified": False,
    "output": output, "host_build": json.loads((root / "out/wpa-host/report.json").read_text()),
    "test_sha256": hashlib.sha256((root / "tests/supplicant_test.c").read_bytes()).hexdigest()}, indent=2) + "\n")
