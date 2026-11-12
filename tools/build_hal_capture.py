#!/usr/bin/env python3
"""Build and verify the isolated 0292 HAL-capture image. Never accesses a device."""
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
MARKER = "SWEET MARK: halbridge v1 node=/dev/marble-audio ring=65536 capture-only"
RAW_SHA = "f4c600f176cdd1265a284cef6fcee8e758ebae90accd0291be5e342ae5eaf93f"
NDK = Path("/opt/homebrew/Caskroom/android-ndk/28/AndroidNDK13004108.app/Contents/NDK")
ARGS = (
    "debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 "
    "ufsroot=userdata ufsroot_lba=0x3603600 ufsroot_blocks=5243387 "
    "ufsrw=1 rootrw=1 wdt=-1 sweetusb=2 marbleusb=1 ufslogsecs=1 "
    "sweetsample=- cs_enforcement_disable=1 unentitled_ios_sim_launch=1 "
    "sweettouch=0 marble_touch=1 sweetfb_noconsole=1 sweetiolog=- "
    "sweetadsp=0 gicrstride=0x40000 sweetbatt=0 sweetglink=0 "
    "sweethalbridge=1 sweeti2c=0 sweetamp=0 sweetgpu=0 sweetlog=1"
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args, cwd=None, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True)


def check_flags(cmd):
    pairs = [word.split("=", 1) for word in cmd.split() if "=" in word]
    flags = dict(pairs)
    assert len(flags) == len(pairs), "duplicate boot argument"
    assert len(cmd.encode()) <= 590, "boot-args budget exceeded"
    expected = {
        "cpus": "8", "sweethalbridge": "1", "sweetadsp": "0",
        "sweetglink": "0", "sweeti2c": "0", "sweetamp": "0",
        "sweetgpu": "0", "sweetbatt": "0", "sweetlog": "1",
        "sweetiolog": "-", "ufslogsecs": "1", "marble_touch": "1",
    }
    assert all(flags.get(key) == value for key, value in expected.items())
    allowed_sweet = {
        "sweetusb", "sweetsample", "sweettouch", "sweetfb_noconsole",
        "sweetiolog", "sweetadsp", "sweetbatt", "sweetglink",
        "sweethalbridge", "sweeti2c", "sweetamp", "sweetgpu",
        "sweetlog", "sweettime",
    }
    assert not (set(flags) - allowed_sweet) & {
        key for key in flags if key.startswith("sweet")
    }, "unreviewed sweet* flag"
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--label", default="hal-capture-0292-20261112")
    parser.add_argument("--verify-only", action="store_true",
                        help="verify saved files without rebuilding or changing sweettime")
    args = parser.parse_args()
    if not re.fullmatch(r"hal-capture-0292-[0-9]{8}", args.label):
        parser.error("label must be hal-capture-0292-YYYYMMDD")
    folder = ROOT / "out" / args.label
    image_path = ROOT / "images" / (args.label + ".img")
    if args.verify_only:
        manifest = json.loads((folder / "manifest.json").read_text())
        image = image_path.read_bytes()
        kernel = (folder / "kernel.adsp").read_bytes()
        payload = (folder / "marble-adsp.payload").read_bytes()
        assert digest(image) == manifest["image_sha256"]
        assert digest(kernel) == manifest["kernel_sha256"]
        assert manifest["raw_sha256"] == RAW_SHA
        assert image[:8] == b"ANDROID!" and len(image) == manifest["image_bytes"]
        # The loader also embeds the literal SWEETXNU. Locate the appended
        # payload from the Android header's kernel size, not a bare string hit.
        kernel_size = struct.unpack_from("<I", image, 8)[0]
        where = 4096 + kernel_size - len(payload)
        assert where >= 4096 and image[where:where + 8] == b"SWEETXNU"
        assert image[where:where + len(payload)] == payload
        cmd = payload[0x48:0x2a8].split(b"\0", 1)[0].decode()
        assert cmd == manifest["cmdline"]
        check_flags(cmd)
        assert kernel.count(MARKER.encode()) == 1
        assert not kernel.count(bytes.fromhex("28e21bd5"))
        print(json.dumps({"passed": True, "image": str(image_path),
                          "sha256": digest(image), "cmdline_bytes": len(cmd),
                          "device_access": False}, indent=2))
        return
    if args.raw is None:
        parser.error("--raw is required unless --verify-only is selected")
    folder.mkdir(parents=True, exist_ok=True)
    before_default = digest((ROOT / "out/kernel.adsp").read_bytes())
    raw = args.raw.read_bytes()
    assert digest(raw) == RAW_SHA, "release asset digest mismatch"
    assert raw.count(MARKER.encode()) == 1
    workflow = (ROOT / ".github/workflows/xnu.yml").read_text()
    marker_loop = workflow.split('for s in "SWEET: halt:', 1)[1].split("; do", 1)[0]
    markers = ["SWEET: halt:" + marker_loop.split('"', 1)[0]]
    markers += re.findall(r'"([^"]+)"', marker_loop.split('"', 1)[1])
    assert MARKER in markers
    assert all(marker.encode() in raw for marker in markers), "missing cumulative marker"
    original = bytes.fromhex("28e21bd5")
    assert raw.count(original) == 1, "expected exactly one raw CNTP_CTL site"
    offset = raw.index(original)
    patched = raw[:offset] + bytes.fromhex("1f2003d5") + raw[offset + 4:]
    assert patched.count(original) == 0
    kernel_path = folder / "kernel.adsp"
    kernel_path.write_bytes(patched)
    symbols = subprocess.check_output(
        [str(NDK / "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"),
         "-n", str(kernel_path)], text=True)
    required_symbols = [
        "_sweet_audio_bridge_init", "_sweet_audio_bridge_consume",
        "_bridge_open", "_bridge_write", "_bridge_ioctl", "_bridge_ring",
    ]
    assert all(re.search(r"^\S+\s+\w\s+" + re.escape(name) + r"$",
                         symbols, re.M) for name in required_symbols)
    (folder / "bridge-symbols.txt").write_text(
        "\n".join(line for line in symbols.splitlines()
                  if any(line.endswith(" " + name) for name in required_symbols)) + "\n")

    cmd = ARGS + " sweettime=" + str(int(time.time()))
    check_flags(cmd)
    adt_path = folder / "marble.adt"
    firmware_path = folder / "adsp2.fwblob"
    payload_path = folder / "marble-adsp.payload"
    env = os.environ.copy()
    env["D22AP_ADT"] = str(ROOT.parent / "xnu/research/ipsw-x/d22ap.adt")
    run("python3", "-B", "-I", str(ROOT / "scripts/mkadt.py"),
        str(adt_path), "--highmem", env=env)
    run("python3", "-B", "-I", str(ROOT / "scripts/mkfwblob.py"),
        str(ROOT.parent / "adsp-fw"), str(firmware_path))
    run("python3", "-B", "-I", str(ROOT / "scripts/mkpayload.py"),
        str(kernel_path), str(adt_path), str(payload_path),
        "--cmdline", cmd, "--ramdisk", str(firmware_path))
    shim = folder / "shim"
    shutil.copytree(ROOT / "shim", shim, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("out", "out-xnu", ".git"))
    for name in list(env):
        if name.startswith(("GP_", "PCM_")) or name == "GPU_PROBE_LEVEL":
            env.pop(name)
    env["GPU_PROBE_LEVEL"] = "0"
    run("bash", str(shim / "build_xnu.sh"), str(payload_path), env=env)
    image_path.parent.mkdir(exist_ok=True)
    shutil.copyfile(shim / "out-xnu/shim-xnu.img", image_path)

    payload = payload_path.read_bytes()
    image = image_path.read_bytes()
    body = (shim / "out-xnu/Image").read_bytes()
    assert image[:8] == b"ANDROID!"
    assert struct.unpack_from("<I", image, 40)[0] == 4
    assert image[44:1580].split(b"\0", 1)[0] == b"console=null"
    assert image[4096:4096 + len(body)] == body
    assert body.endswith(payload) and payload[:8] == b"SWEETXNU"
    actual_cmd = payload[0x48:0x2a8].split(b"\0", 1)[0].decode()
    assert actual_cmd == cmd
    check_flags(actual_cmd)
    tools = load("hal_mkpayload", ROOT / "scripts/mkpayload.py")
    kernel = bytearray(patched)
    macho = tools.parse_macho(kernel)
    rebases = tools.apply_chained_fixups(kernel, macho)
    assert macho["cputype"] == 0x100000C and macho["cpusub"] == 0
    nsegs = struct.unpack_from("<I", payload, 12)[0]
    segments = [struct.unpack_from("<4Q", payload, 0x2a8 + i * 32)
                for i in range(nsegs)]
    for segment in macho["segs"]:
        if not segment["vmsize"]:
            continue
        matching = [item for item in segments if item[0] == segment["vmaddr"]]
        assert len(matching) == 1
        _, size, where, count = matching[0]
        assert (size, count) == (segment["vmsize"], segment["filesize"])
        assert payload[where:where + count] == kernel[
            segment["fileoff"]:segment["fileoff"] + count]
    tree, _ = tools.adt.parse(adt_path.read_bytes())
    cpus = next(node for node in tree.children if node.name == "cpus")
    mpidrs = [struct.unpack("<I", node.prop("reg"))[0] for node in cpus.children]
    assert mpidrs == list(range(0, 0x800, 0x100))
    firmware_segment = [item for item in segments
                        if item[3] == firmware_path.stat().st_size
                        and payload[item[2]:item[2] + item[3]] == firmware_path.read_bytes()]
    assert len(firmware_segment) == 1
    assert digest((ROOT / "out/kernel.adsp").read_bytes()) == before_default
    manifest = {
        "image": str(image_path), "image_bytes": len(image),
        "image_sha256": digest(image), "raw_sha256": RAW_SHA,
        "kernel_sha256": digest(patched), "cntp_nop_offset": hex(offset),
        "cmdline": actual_cmd, "cmdline_bytes": len(actual_cmd.encode()),
        "cpu_count": len(cpus.children), "mpidrs": mpidrs,
        "workflow_markers_checked": len(markers),
        "bridge_symbols_linked": required_symbols, "kernel_rebases": rebases,
        "kernel_segments_verified": True, "gpu_shim_probe_level": 0,
        "default_kernel_unchanged": True, "device_booted": False,
        "hal_plugin_installed": False, "budget_used": 68,
        "status": "offline verified; device installation requires separate authorization",
    }
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
