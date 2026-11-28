#!/usr/bin/env python3
"""Compile the boot hook against captured XNU headers; bind its runtime symbol."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def symbols(path, option):
    text = subprocess.check_output(["xcrun", "nm", option, str(path)], text=True)
    return {line.split()[-1] for line in text.splitlines() if line.split()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildtree", type=Path, required=True,
                        help="extracted DXB buildtree containing recorded BSD flags and headers")
    parser.add_argument("--bundle", type=Path, required=True)
    args = parser.parse_args()
    tree = args.buildtree.resolve()
    bsd = tree / "build/xnu.obj/DEVELOPMENT_ARM64_SWEET/bsd/DEVELOPMENT"
    flags = (bsd / ".CFLAGS").read_text().rstrip("\0\n")
    dxb = re.search(r"-I(?P<root>[^ ]+)/xnu/bsd(?:\s|$)", flags).group("root")
    sdk = subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    # DXB's dump has lost the quotes around the NMBCLUSTERS expression.
    flags = re.sub(r"-DCONFIG_NMBCLUSTERS=.*? (?=-D)",
                   lambda m: shlex.quote(m.group().rstrip()) + " ", flags)
    flags = re.sub(r"-I/Applications/[^ ]+/SDKs/MacOSX[^/ ]*\.sdk/",
                   "-I" + sdk + "/", flags)
    flags = [arg.replace(dxb, str(tree)) for arg in shlex.split(flags)[1:]
             if not arg.startswith("-mmacosx-version-min=")]
    source = ROOT.parent / "chain-tree-wifi/bsd/dev/sweet_wifi.c"
    runtime = "_sweet_wifi_runtime_start"
    with tempfile.TemporaryDirectory(prefix="wifi-xnu-hook-") as folder:
        for enabled in (0, 1):
            obj = Path(folder) / f"wifi-hook-{enabled}.o"
            subprocess.run(["xcrun", "clang", *flags, "-Werror=implicit-function-declaration",
                            "-Werror=incompatible-pointer-types", "-mmacosx-version-min=15.0",
                            f"-DSWEET_WIFI_EMBEDDED={enabled}", "-c", str(source),
                            "-o", str(obj)], cwd=bsd, check=True)
            if (runtime in symbols(obj, "-u")) != bool(enabled):
                raise ValueError(f"unexpected workspace-runtime import with embedded={enabled}")
        linked = Path(folder) / "wifi-hook-and-bundle.o"
        subprocess.run(["xcrun", "ld", "-r", "-arch", "arm64", "-platform_version",
                        "macos", "15.0", "15.0", str(obj), str(args.bundle.resolve()),
                        "-o", str(linked)], check=True)
        if runtime in symbols(linked, "-u") or runtime not in symbols(linked, "-g"):
            raise ValueError("the staged bundle did not satisfy the hook's runtime import")
    print("PASS: real XNU BSD compile (public/local), runtime import resolved by bundle")


if __name__ == "__main__":
    main()
