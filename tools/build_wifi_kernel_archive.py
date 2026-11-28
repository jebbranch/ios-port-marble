#!/usr/bin/env python3
"""Build the pinned marble ath11k backend and firmware as an arm64 XNU archive."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = ROOT / "vendor/foxlinne-wifi"
MODULE = BUNDLE / "Ath12kWifi"


def run(*args):
    return subprocess.check_output(args, text=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def llvm_tool(name, directory):
    if directory:
        return str(directory / name)
    installed = shutil.which(name)
    if installed:
        return installed
    raise ValueError(f"{name} not found; set --llvm-bin to an LLVM toolchain")


def jobs_for(clang, out, target):
    resource = Path(run(clang, "-print-resource-dir").strip()) / "include"
    opts = [
        "-target", target, "-mkernel", "-O2", "-ffreestanding", "-fno-builtin",
        "-fno-common", "-fno-strict-aliasing", "-fwrapv", "-fno-stack-protector",
        "-fno-delete-null-pointer-checks", "-mgeneral-regs-only",
        "-momit-leaf-frame-pointer", "-std=gnu11", "-Wno-incompatible-pointer-types",
        "-Werror=implicit-function-declaration", "-Wno-nullability-completeness",
        "-DCONFIG_ATH11K_DEBUG", "-DCONFIG_CFG80211=1", "-DCONFIG_MAC80211=1",
        "-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0", '-DCONFIG_MAC80211_RC_DEFAULT=""',
        "-nostdinc", "-isystem", str(MODULE / "shim/libc"), "-isystem", str(resource),
        "-include", "lx_pci.h",
    ]
    for d in ("shim/include", "include", "out/pinned", "src", "out"):
        opts += ["-I", str(MODULE / d)]
    dirs = {
        "ath11k": "ath11k-src", "mhi": "mhi-src", "qmi": "os-src",
        "mac80211": "mac80211", "cfg80211": "cfg80211", "crypto": "crypto",
        "generated": "",
    }
    seen = set()
    for row in (MODULE / "tools/sources-ath11k.txt").read_text().splitlines():
        row = row.strip()
        if not row or row.startswith("#"):
            continue
        tree, name = row.split(":", 1) if ":" in row else ("", row)
        source = MODULE / "out" / dirs[tree] / name if tree else MODULE / name
        extra = []
        includes = [dirs[tree]] if tree in dirs and tree != "generated" else []
        if tree == "generated" or name.startswith("src/nw_cfg80211_"):
            includes = ["cfg80211"]
        elif name == "src/nw_fils.c":
            includes = ["mac80211"]
        elif name == "src/nw_crypto_mac80211.c":
            includes = ["crypto", "mac80211"]
        elif name.startswith("src/"):
            includes = ["ath11k-src"]
        if "crypto" in includes:
            extra += ["-I", str(MODULE / "shim/crypto")]
        for include in includes:
            extra += ["-I", str(MODULE / "out" / include)]
        if tree == "crypto" and name == "aes-siv.c":
            extra += ["-Daes_siv_encrypt=nw_aes_siv_encrypt",
                      "-Daes_siv_decrypt=nw_aes_siv_decrypt"]
        obj = out / (f"{tree or 'native'}_{source.stem}.o")
        if obj in seen or not source.is_file():
            raise ValueError(f"duplicate object or missing input: {source}")
        seen.add(obj)
        yield source, obj, [clang, "-c", *extra, *opts, str(source), "-o", str(obj)]


def symbols(nm, path):
    for line in run(nm, "-g", str(path)).splitlines():
        words = line.split()
        if len(words) >= 2 and len(words[-2]) == 1:
            yield words[-2], words[-1]


def firmware_object(clang, firmware, out, target):
    manifest = json.loads((BUNDLE / "firmware-manifest.json").read_text())
    names = {"amss.bin": "amss", "board-2.bin": "board2", "m3.bin": "m3",
             "regdb.bin": "regdb"}
    rows = {entry["name"]: entry for entry in manifest["files"]}
    if set(rows) != set(names) | {"Notice.txt"}:
        raise ValueError("unexpected WCN6855 firmware inventory")
    lines = [".section __TEXT,__const"]
    for name, entry in rows.items():
        path = firmware / name
        if path.stat().st_size != entry["bytes"] or digest(path) != entry["sha256"]:
            raise ValueError(f"firmware verification failed: {name}")
        if name == "Notice.txt":
            continue
        symbol = "_sweet_wifi_fw_" + names[name]
        lines += [".p2align 4", f".globl {symbol}, {symbol}_end", f"{symbol}:",
                  f".incbin {json.dumps(str(path))}", f"{symbol}_end:"]
    # This translation unit is generated data, never committed as source.
    assembly = out / "sweet_wifi_firmware.S"
    assembly.write_text("\n".join(lines) + "\n")
    obj = out / "sweet_wifi_firmware.o"
    run(clang, "-target", target, "-c", str(assembly), "-o", str(obj))
    shutil.copyfile(firmware / "Notice.txt", out / "Firmware-Notice.txt")
    return obj


def kernel_platform(kernel):
    # This port boots iOS userspace, but DXB actually links a macOS-platform
    # kernel. Keep the object platform compatible with the final linker.
    output = run("xcrun", "vtool", "-show-build", str(kernel))
    builds = re.findall(r"\bplatform\s+(MACOS|IOS)\s+minos\s+(\S+)\s+sdk\s+(\S+)",
                        output)
    if len(builds) != 1:
        raise ValueError("expected one arm64 XNU Mach-O build platform")
    platform, minimum, sdk = builds[0]
    platform = {"MACOS": "macos", "IOS": "ios"}[platform]
    triple_os = "macosx" if platform == "macos" else "ios"
    return platform, minimum, sdk, "arm64-apple-" + triple_os + minimum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--clang", type=Path, help="compiler override (Clang 19 or newer recommended)")
    parser.add_argument("--kernel", type=Path, required=True,
                        help="existing XNU 8796 kernel used to audit imports")
    parser.add_argument("--out", type=Path, default=ROOT / "out/wifi-full/arm64-archive")
    parser.add_argument("--firmware", type=Path, default=ROOT / "out/wifi-full/firmware")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists():
        parser.error(f"choose an unused artifact directory: {out}")
    identity = json.loads((MODULE / "out/source-identity.json").read_text())
    platform, minimum, sdk, target = kernel_platform(args.kernel)
    clang = str(args.clang.resolve()) if args.clang else run("xcrun", "-f", "clang").strip()
    nm = llvm_tool("llvm-nm", args.llvm_bin)
    objcopy = llvm_tool("llvm-objcopy", args.llvm_bin)
    ar = llvm_tool("llvm-ar", args.llvm_bin)
    out.mkdir(parents=True)
    raw = out / "raw"
    raw.mkdir()
    jobs = list(jobs_for(clang, raw, target))

    def compile_unit(job):
        src, obj, cmd = job
        result = subprocess.run(cmd, capture_output=True, text=True)
        return obj, result

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(compile_unit, jobs))
    errors = [(obj, result.stderr) for obj, result in results if result.returncode]
    if errors:
        log = out / "compile-errors.log"
        log.write_text("\n".join(f"{obj.name}:\n{text}" for obj, text in errors))
        raise SystemExit(f"{len(errors)} translation units failed; see {log}")
    objects = [obj for obj, _ in results]
    # Preserve only the documented board ABI. Rename internal definitions AND
    # their references across every translation unit so they cannot bind XNU.
    keep = set()
    for name in ("nw_abi.h", "nw_control_endpoint.h"):
        keep.update("_" + s for s in re.findall(r"\b(nw_wifi_\w+)\s*\(",
                                                (MODULE / "include" / name).read_text()))
    keep.update(("_nw_wifi_stats", "_nw_wifi_where"))
    defined = {name for obj in objects for kind, name in symbols(nm, obj) if kind != "U"}
    mapping = out / "rename.txt"
    mapping.write_text("".join(f"{n} _nwk{n}\n" for n in sorted(defined - keep)))
    final = []
    for obj in objects:
        dest = out / obj.name
        run(objcopy, f"--redefine-syms={mapping}", str(obj), str(dest))
        final.append(dest)
    final.append(firmware_object(clang, args.firmware.resolve(), out, target))
    sdk_path = run("xcrun", "--sdk", "macosx", "--show-sdk-path").strip()
    for name in ("sweet_wifi_firmware", "sweet_wifi_dma_plan", "sweet_wifi_smmu",
                 "sweet_wifi_dma_arena", "sweet_wifi_runtime"):
        obj = out / (name + "_adapter.o")
        run(clang, "-target", target, "-mkernel", "-ffreestanding", "-fno-builtin",
            "-std=c11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
            "-D_FORTIFY_SOURCE=0", "-isysroot", sdk_path,
            "-I", str(MODULE / "include"),
            "-c", str(BUNDLE / "xnu" / (name + ".c")), "-o", str(obj))
        final.append(obj)
    keep.update(("_sweet_wifi_firmware_get", "_sweet_wifi_firmware_inventory",
                 "_sweet_wifi_dma_plan_route", "_sweet_wifi_dma_build_tables",
                 "_sweet_wifi_smmu_commit", "_sweet_wifi_dma_arena_init",
                 "_sweet_wifi_dma_arena_alloc", "_sweet_wifi_dma_arena_map",
                 "_sweet_wifi_dma_arena_free", "_sweet_wifi_runtime_start"))
    definitions = {}
    references = set()
    for obj in final:
        for kind, name in symbols(nm, obj):
            if kind == "U":
                references.add(name)
            else:
                definitions.setdefault(name, []).append(obj.name)
    kernel = {name for kind, name in symbols(nm, args.kernel) if kind != "U"}
    missing = sorted(references - definitions.keys() - kernel)
    duplicates = {name: files for name, files in definitions.items() if len(files) > 1}
    collisions = sorted(definitions.keys() & kernel)
    passed = not (missing or duplicates or collisions or keep - definitions.keys())
    archive = out / "libmarble_wifi.a"
    if passed:
        run(ar, "rcsD", str(archive), *map(str, final))
        bundle = out / "sweet_wifi_bundle.o"
        ld = run("xcrun", "-f", "ld").strip()
        run(ld, "-r", "-arch", "arm64", "-platform_version", platform, minimum, sdk,
            "-all_load", str(archive), "-o", str(bundle))
        linked = list(symbols(nm, bundle))
        if ({name for kind, name in linked if kind == "U"} != references - definitions.keys()
                or keep - {name for kind, name in linked if kind != "U"}):
            raise ValueError("relocatable link changed the board ABI or unresolved imports")
    report = {
        "passed": passed, "target": target, "platform": platform,
        "wireless_objects": len(objects),
        "compiler": run(clang, "--version").splitlines()[0],
        "kernel_imports": sorted(references - definitions.keys()),
        "missing_imports": missing, "duplicate_definitions": duplicates,
        "xnu_collisions": collisions, "missing_exports": sorted(keep - definitions.keys()),
        "kernel_sha256": digest(args.kernel), "source_identity": identity,
        "archive_sha256": digest(archive) if passed else None,
        "relocatable_bundle": bundle.name if passed else None,
        "firmware_provider": "embedded, read-only",
        "board_runtime": "sweet_wifi_runtime_start",
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(objects)} wireless objects; {len(missing)} missing imports; "
          f"{len(duplicates)} duplicate definitions; {len(collisions)} XNU collisions")
    if not passed:
        raise SystemExit(f"Wi-Fi archive rejected; see {out / 'report.json'}")


if __name__ == "__main__":
    main()
