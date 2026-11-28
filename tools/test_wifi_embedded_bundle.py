#!/usr/bin/env python3
"""Exercise the local kernel build input, relocatable object and firmware provider."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from stage_wifi_kernel_bundle import stage

ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "vendor/foxlinne-wifi/xnu"
PATCH = ROOT / "xnu/patches/0302-sweet-wifi-embedded-archive.patch"
HOOK = ROOT / "xnu/patches/0303-sweet-wifi-runtime-boot-hook.patch"
CHAIN = ROOT.parent / "chain-tree-wifi"
SOURCE = "bsd/dev/sweet_wifi.c"
NW_ABI = ROOT / "vendor/foxlinne-wifi/Ath12kWifi/include"


def call(*cmd, cwd=None):
    return subprocess.check_output(cmd, cwd=cwd, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--xnu", type=Path, required=True,
                        help="XNU 8796 base used to replay patch 0302")
    args = parser.parse_args()
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix="wifi-link-test-") as name:
        tree = Path(name).resolve()
        (tree / "makedefs").mkdir()
        (tree / "bsd/dev").mkdir(parents=True)
        shutil.copyfile(args.xnu / "makedefs/MakeInc.def", tree / "makedefs/MakeInc.def")
        call("git", "apply", "--check", str(PATCH), cwd=tree)
        call("git", "apply", str(PATCH), cwd=tree)
        (tree / SOURCE).write_text(call("git", "-C", str(CHAIN), "show", "80e5363:" + SOURCE))
        call("git", "apply", "--check", str(HOOK), cwd=tree)
        call("git", "apply", str(HOOK), cwd=tree)
        assert (tree / SOURCE).read_bytes() == (CHAIN / SOURCE).read_bytes()
        staged = stage(build, tree)
        assert staged.read_bytes() == (build / staged.name).read_bytes()
        try:
            stage(build, tree)
        except FileExistsError:
            pass
        else:
            raise AssertionError("staging silently replaced an existing object")

        # Ask make to expand the actual MakeInc.def, not a duplicate of the
        # patch conditional. No build prerequisites are invoked here.
        harness = (
            "include makedefs/MakeInc.def\n"
            "probe:\n"
            "\t@printf '%s\\n' '$(LDFILES_KERNEL_ONLY)' '$(LDFLAGS_KERNEL_ONLY)' '$(DEFINES)'\n"
        )
        for machine, flag, enabled in (
            ("SWEET", "0", False), ("SWEET", "1", True), ("VMAPPLE", "1", False),
        ):
            result = subprocess.run(
                ["make", "-s", "-f", "-", "probe", f"SRCROOT={tree}", "TARGET=build",
                 f"CURRENT_MACHINE_CONFIG={machine}", f"SWEET_WIFI_EMBEDDED={flag}"],
                input=harness, cwd=tree, check=True, text=True, capture_output=True,
            ).stdout.splitlines()
            assert (str(staged) in result[0].split()) == enabled, result
            assert ("-u,_nw_wifi_start,-u,_sweet_wifi_firmware_get" in result[1]) == enabled, result
            assert ("-DSWEET_WIFI_EMBEDDED=1" in result[2].split()) == enabled, result

        # Exercise actual embedded bytes and symbol relocations from the same
        # object the XNU linker receives, using libc for the hosted test only.
        for test in ("wifi_firmware_host", "wifi_dma_plan_host", "wifi_smmu_host",
                     "wifi_dma_arena_host", "wifi_transfer_host"):
            exe = tree / test
            call("xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wl,-dead_strip", "-mmacosx-version-min=15.0", "-I", str(ADAPTER),
                 "-I", str(NW_ABI),
                 str(ROOT / "tools" / (test + ".c")),
                 str(ROOT / "tools/wifi_kernel_imports_host.c"), str(staged), "-o", str(exe))
            print(call(str(exe)).strip())
    print("PASS: 0302/0303 replay, exact hook post-image, staging and opt-in make flags")


if __name__ == "__main__":
    main()
