#!/usr/bin/env python3
"""Build and verify the corrected 0301 PCIe identity image; no device access."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
LABEL = "wifi-pcie-0301-20261123"
IMAGE = ROOT / "images" / (LABEL + ".img")
OUT = ROOT / "out" / LABEL
NDK = Path("/opt/homebrew/Caskroom/android-ndk/28/AndroidNDK13004108.app/Contents/NDK")
HANDOFF_MARKER = (
    "SWEET MARK: wifi handoff v1 readonly=1 stage=%d gdsc=%#x "
    "halted=%#x wlan=%#x perst=%#x"
)
PCIE_MARKER = (
    "SWEET MARK: wifi pcie v2 stage=%d id=%#x subsystem=%#x "
    "bar=%#x bytes=%#x soc=%#x sub=%#x"
)
BASE_ARGS = (
    "debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 "
    "ufsroot=userdata ufsroot_lba=0x3603600 ufsroot_blocks=5243387 "
    "ufsrw=1 rootrw=1 wdt=-1 sweetusb=2 marbleusb=1 ufslogsecs=1 "
    "sweetsample=- cs_enforcement_disable=1 unentitled_ios_sim_launch=1 "
    "sweettouch=0 marble_touch=0 sweetfb_noconsole=1 sweetiolog=- "
    "sweetadsp=0 gicrstride=0x40000 sweetbatt=0 sweetglink=0 "
    "sweethalbridge=0 sweeti2c=0 sweetamp=0 sweetgpu=0 sweetlog=1 "
    "sweetwifi=2"
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args, cwd=None, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def flags(command):
    pairs = [word.split("=", 1) for word in command.split() if "=" in word]
    result = dict(pairs)
    assert len(result) == len(pairs)
    assert len(command.encode()) <= 590
    expected = {
        "cpus": "8", "sweetadsp": "0", "sweetglink": "0",
        "sweethalbridge": "0", "sweeti2c": "0", "sweetamp": "0",
        "sweetgpu": "0", "sweetbatt": "0", "sweetlog": "1",
        "sweetwifi": "2", "sweetiolog": "-", "marble_touch": "0",
    }
    assert all(result.get(key) == value for key, value in expected.items())
    forbidden = {
        "sweetreboot", "sweetpcm", "sweets1", "sweethalstream",
        "sweetlisten", "sweetgmuboot", "sweetrail", "sweetgcanary",
        "sweetnowait", "sweetampfirst", "sweetampverify",
    }
    assert not forbidden & result.keys()
    allowed = {word.split("=", 1)[0] for word in BASE_ARGS.split() if "=" in word}
    assert set(result) == allowed | {"sweettime"}, "unreviewed boot argument"
    assert command.split().count("-noprogress") == 1
    assert all("=" in word or word == "-noprogress" for word in command.split())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--raw-sha")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.verify_only:
        manifest = json.loads((OUT / "manifest.json").read_text())
        image = IMAGE.read_bytes()
        kernel = (OUT / "kernel.adsp").read_bytes()
        payload = (OUT / "marble-adsp.payload").read_bytes()
        assert sha(image) == manifest["image_sha256"]
        assert sha(kernel) == manifest["kernel_sha256"]
        assert image[:8] == b"ANDROID!" and len(image) == manifest["image_bytes"]
        assert not kernel.count(bytes.fromhex("28e21bd5"))
        where = 4096 + struct.unpack_from("<I", image, 8)[0] - len(payload)
        assert image[where:where + len(payload)] == payload
        command = payload[0x48:0x2a8].split(b"\0", 1)[0].decode()
        assert command == manifest["cmdline"]
        flags(command)
        assert kernel.count(HANDOFF_MARKER.encode()) == 1
        assert kernel.count(PCIE_MARKER.encode()) == 1
        print(json.dumps({
            "passed": True, "image": str(IMAGE), "sha256": sha(image),
            "cmdline_bytes": len(command.encode()), "device_access": False,
        }, indent=2))
        return
    if args.raw is None or not args.raw_sha:
        parser.error("--raw and --raw-sha are required")
    raw = args.raw.read_bytes()
    assert sha(raw) == args.raw_sha
    assert raw.count(HANDOFF_MARKER.encode()) == 1
    assert raw.count(PCIE_MARKER.encode()) == 1
    workflow = (ROOT / ".github/workflows/xnu.yml").read_text()
    marker_loop = workflow.split('for s in "SWEET: halt:', 1)[1].split("; do", 1)[0]
    cumulative = ["SWEET: halt:" + marker_loop.split('"', 1)[0]]
    cumulative += re.findall(r'"([^"]+)"', marker_loop.split('"', 1)[1])
    assert all(marker.encode() in raw for marker in cumulative)
    original = bytes.fromhex("28e21bd5")
    assert raw.count(original) == 1
    offset = raw.index(original)
    patched = raw[:offset] + bytes.fromhex("1f2003d5") + raw[offset + 4:]
    assert patched.count(original) == 0
    OUT.mkdir(parents=True, exist_ok=True)
    kernel_path = OUT / "kernel.adsp"
    kernel_path.write_bytes(patched)
    nm = subprocess.check_output([
        str(NDK / "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"),
        "-n", str(kernel_path)], text=True)
    assert re.search(r"^\S+\s+\w\s+_sweet_wifi_init$", nm, re.M)
    (OUT / "symbols.txt").write_text(
        "\n".join(line for line in nm.splitlines()
                  if line.endswith(" _sweet_wifi_init")) + "\n")

    command = BASE_ARGS + " sweettime=" + str(int(time.time()))
    flags(command)
    adt = OUT / "marble.adt"
    firmware = OUT / "adsp2.fwblob"
    payload = OUT / "marble-adsp.payload"
    env = os.environ.copy()
    env["D22AP_ADT"] = str(ROOT.parent / "xnu/research/ipsw-x/d22ap.adt")
    run("python3", "-B", "-I", str(ROOT / "scripts/mkadt.py"),
        str(adt), "--highmem", env=env)
    run("python3", "-B", "-I", str(ROOT / "scripts/mkfwblob.py"),
        str(ROOT.parent / "adsp-fw"), str(firmware))
    run("python3", "-B", "-I", str(ROOT / "scripts/mkpayload.py"),
        str(kernel_path), str(adt), str(payload), "--cmdline", command,
        "--ramdisk", str(firmware))
    shim = OUT / "shim"
    shutil.copytree(ROOT / "shim", shim, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("out", "out-xnu", ".git"))
    for name in list(env):
        if name.startswith(("GP_", "PCM_")) or name == "GPU_PROBE_LEVEL":
            env.pop(name)
    env["GPU_PROBE_LEVEL"] = "0"
    run("bash", str(shim / "build_xnu.sh"), str(payload), env=env)
    IMAGE.parent.mkdir(exist_ok=True)
    shutil.copyfile(shim / "out-xnu/shim-xnu.img", IMAGE)

    image = IMAGE.read_bytes()
    payload_data = payload.read_bytes()
    body = (shim / "out-xnu/Image").read_bytes()
    assert image[:8] == b"ANDROID!" and image[4096:4096 + len(body)] == body
    assert body.endswith(payload_data) and payload_data[:8] == b"SWEETXNU"
    actual = payload_data[0x48:0x2a8].split(b"\0", 1)[0].decode()
    assert actual == command
    flags(actual)
    pack = load("wifi_mkpayload", ROOT / "scripts/mkpayload.py")
    kernel = bytearray(patched)
    macho = pack.parse_macho(kernel)
    rebases = pack.apply_chained_fixups(kernel, macho)
    segments = [struct.unpack_from("<4Q", payload_data, 0x2a8 + i * 32)
                for i in range(struct.unpack_from("<I", payload_data, 12)[0])]
    for segment in macho["segs"]:
        if not segment["vmsize"]:
            continue
        match = [item for item in segments if item[0] == segment["vmaddr"]]
        assert len(match) == 1
        _, size, location, count = match[0]
        assert (size, count) == (segment["vmsize"], segment["filesize"])
        assert payload_data[location:location + count] == kernel[
            segment["fileoff"]:segment["fileoff"] + count]
    tree, _ = pack.adt.parse(adt.read_bytes())
    cpus = next(node for node in tree.children if node.name == "cpus")
    mpidrs = [struct.unpack("<I", node.prop("reg"))[0] for node in cpus.children]
    assert mpidrs == list(range(0, 0x800, 0x100))
    manifest = {
        "image": str(IMAGE), "image_bytes": len(image),
        "image_sha256": sha(image), "raw_sha256": args.raw_sha,
        "kernel_sha256": sha(patched), "cntp_nop_offset": hex(offset),
        "cmdline": actual, "cmdline_bytes": len(actual.encode()),
        "sweettime": int(flags(actual)["sweettime"]),
        "cpu_count": len(mpidrs), "mpidrs": mpidrs,
        "workflow_markers_checked": len(cumulative),
        "symbols": ["_sweet_wifi_init"], "kernel_rebases": rebases,
        "kernel_segments_verified": True, "gpu_shim_probe_level": 0,
        "device_booted": False, "boot_budget_before": 74,
        "touch_disabled_for_tcs_isolation": True,
        "stage": "guarded QCA6490 power, PCIe link and endpoint identity",
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
