#!/usr/bin/env python3
"""Build and verify the exact 0296 guarded AW88266 listening image with 300 second pre-trigger wait. No device access."""
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
LABEL = "hallisten-0296-20261114"
IMAGE = ROOT / "images" / (LABEL + ".img")
OUT = ROOT / "out" / LABEL
NDK = Path("/opt/homebrew/Caskroom/android-ndk/28/AndroidNDK13004108.app/Contents/NDK")
MARKERS = (
    "SWEET MARK: halstream v2 periods=4 bytes=4096 divisor=%u ceiling=%u cap=30 listen=%u",
    "SWEET MARK: listen v1 awcfg-sent=1",
    "SWEET MARK: listen v1 started=1 peak-limit=16384 volume-reg=0x05a9",
    "SWEET MARK: listen v1 stopped=1",
    "SWEET MARK: halstream amp-bypass v1 before MMIO",
    "SWEET MARK: halstream awcfg-bypass v1",
    "SWEET MARK: halstream guard v2 pass=%d s1ap=%d",
    "SWEET MARK: halstream graph v1 bytes=%u off=%#x",
    "SWEET MARK: halstream ctrl v1 %s op=%#x status=%#x",
    "SWEET MARK: halstream teardown v1 open=%d stop=%d close=%d unmap=%d",
    "SWEET HALSTREAM: ready src=%#x dst=%#x handle=%#x base=%#llx wait-ms=300000",
)
BASE_ARGS = (
    "debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 "
    "ufsroot=userdata ufsroot_lba=0x3603600 ufsroot_blocks=5243387 "
    "ufsrw=1 rootrw=1 wdt=-1 sweetusb=2 marbleusb=1 ufslogsecs=1 "
    "sweetsample=- cs_enforcement_disable=1 unentitled_ios_sim_launch=1 "
    "sweettouch=0 marble_touch=1 sweetfb_noconsole=1 sweetiolog=- "
    "sweetadsp=1 gicrstride=0x40000 sweetbatt=0 sweetglink=1 "
    "sweethalbridge=1 sweethalstream=1 sweetpcm=1 sweetdescsep=1 "
    "sweets1=1 sweets1ap=3 sweetiova=1 sweets1va=0x1FFE8000 "
    "sweetshmhi=1 sweettdmvendor=7 sweeti2c=0 sweetamp=0 "
    "sweetgpu=0 sweetlog=1 sweetlisten=1"
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args, cwd=None, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True)


def flags(command):
    pairs = [word.split("=", 1) for word in command.split() if "=" in word]
    result = dict(pairs)
    assert len(result) == len(pairs), "duplicate boot argument"
    assert len(command.encode()) <= 590, "boot argument budget exceeded"
    expected = {
        "cpus": "8", "sweetadsp": "1", "sweetbatt": "0",
        "sweetglink": "1", "sweethalbridge": "1", "sweethalstream": "1",
        "sweetpcm": "1", "sweetdescsep": "1", "sweets1": "1",
        "sweets1ap": "3", "sweetiova": "1", "sweets1va": "0x1FFE8000",
        "sweetshmhi": "1", "sweettdmvendor": "7", "sweeti2c": "0",
        "sweetamp": "0", "sweetgpu": "0", "sweetlog": "1", "sweetlisten": "1",
        "sweetiolog": "-", "marble_touch": "1",
    }
    assert all(result.get(key) == value for key, value in expected.items())
    forbidden = {
        "sweetreboot", "sweetnowait", "sweetampfirst", "sweetampverify",
        "sweetvol", "sweetbufs", "sweetdbb", "sweetexp", "sweetgmuboot",
        "sweethap", "sweetsmp", "sweetmfc", "sweetmfcget", "sweetvcfg2",
        "sweetep4308", "sweettdmclk", "sweetvgraph", "sweetvgraph7",
        "sweetvgraph8", "sweetshmgraph", "sweetshmsize",
    }
    assert not forbidden & result.keys()
    # Omitted intentionally: implementation and guard both default to flag 4.
    assert "sweetshmflag" not in result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--raw-sha", required=False)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.verify_only:
        manifest = json.loads((OUT / "manifest.json").read_text())
        image = IMAGE.read_bytes()
        kernel = (OUT / "kernel.adsp").read_bytes()
        payload = (OUT / "marble-adsp.payload").read_bytes()
        assert sha(image) == manifest["image_sha256"]
        assert sha(kernel) == manifest["kernel_sha256"]
        assert len(image) == manifest["image_bytes"] and image[:8] == b"ANDROID!"
        kernel_size = struct.unpack_from("<I", image, 8)[0]
        where = 4096 + kernel_size - len(payload)
        assert image[where:where + len(payload)] == payload
        command = payload[0x48:0x2a8].split(b"\0", 1)[0].decode()
        assert command == manifest["cmdline"]
        flags(command)
        assert all(kernel.count(marker.encode()) == 1 for marker in MARKERS)
        nm = subprocess.check_output([
            str(NDK / "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"),
            "-n", str(OUT / "kernel.adsp")], text=True)
        assert all(re.search(r"^\S+\s+\w\s+" + re.escape(name) + r"$",
                             nm, re.M) for name in manifest["symbols"])
        print(json.dumps({
            "passed": True, "image": str(IMAGE), "sha256": sha(image),
            "cmdline_bytes": len(command.encode()), "device_access": False,
        }, indent=2))
        return
    if args.raw is None or not args.raw_sha:
        parser.error("--raw and --raw-sha are required")
    raw = args.raw.read_bytes()
    assert sha(raw) == args.raw_sha
    assert all(raw.count(marker.encode()) == 1 for marker in MARKERS)
    workflow = (ROOT / ".github/workflows/xnu.yml").read_text()
    marker_loop = workflow.split('for s in "SWEET: halt:', 1)[1].split("; do", 1)[0]
    cumulative = ["SWEET: halt:" + marker_loop.split('"', 1)[0]]
    cumulative += re.findall(r'"([^"]+)"', marker_loop.split('"', 1)[1])
    assert all(marker.encode() in raw for marker in cumulative)
    original = bytes.fromhex("28e21bd5")
    assert raw.count(original) == 1
    offset = raw.index(original)
    patched = raw[:offset] + bytes.fromhex("1f2003d5") + raw[offset + 4:]
    assert not patched.count(original)
    OUT.mkdir(parents=True, exist_ok=True)
    kernel_path = OUT / "kernel.adsp"
    kernel_path.write_bytes(patched)
    nm = subprocess.check_output([
        str(NDK / "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"),
        "-n", str(kernel_path)], text=True)
    symbols = (
        "_sweet_audio_bridge_init", "_sweet_audio_bridge_consume",
        "_sweet_audio_bridge_muted", "_sweet_audio_stream_run",
        "_sweet_audio_stream_rx", "_sweet_audio_stream_finish",
        "_sweet_audio_stream_faulted", "_sweet_audio_stream_tick",
        "_marble_pcm_init", "_marble_pcm_stage", "_marble_pcm_packet",
        "_marble_pcm_done", "_marble_pcm_expire", "_marble_pcm_abort",
        "_marble_pcm_output", "_sweet_i2c_listen_start",
        "_sweet_i2c_listen_stop",
    )
    assert all(re.search(r"^\S+\s+\w\s+" + re.escape(name) + r"$", nm, re.M)
               for name in symbols)
    (OUT / "symbols.txt").write_text(
        "\n".join(line for line in nm.splitlines()
                  if any(line.endswith(" " + symbol) for symbol in symbols)) + "\n")

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
    pack = load("halstream_mkpayload", ROOT / "scripts/mkpayload.py")
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
        "symbols": list(symbols), "kernel_rebases": rebases,
        "kernel_segments_verified": True, "gpu_shim_probe_level": 0,
        "device_booted": False, "budget_used": 70,
        "stage": "guarded HAL to DSP to AW88266, bounded listening test",
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
