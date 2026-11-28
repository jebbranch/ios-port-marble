#!/usr/bin/env python3
"""Stage the locally linked Wi-Fi bundle in a patched XNU build tree."""
import argparse
import json
from pathlib import Path
import shutil
import struct


def stage(source, xnu):
    source = source.resolve()
    xnu = xnu.resolve()
    report = json.loads((source / "report.json").read_text())
    if (report.get("passed") is not True or report.get("platform") != "macos"
            or not report.get("target", "").startswith("arm64-apple-macosx")
            or report.get("relocatable_bundle") != "sweet_wifi_bundle.o"
            or report.get("board_runtime") != "sweet_wifi_runtime_start"):
        raise ValueError("expected a passed arm64 DXB bundle with the board runtime")
    bundle = source / "sweet_wifi_bundle.o"
    with bundle.open("rb") as handle:
        header = handle.read(32)
    if len(header) != 32 or struct.unpack_from("<4I", header) != (
            0xFEEDFACF, 0x0100000C, 0, 1):  # Mach-O, plain arm64, MH_OBJECT
        raise ValueError("expected a plain arm64 Mach-O object")
    makefile = (xnu / "makedefs/MakeInc.def").read_text()
    if ("ifeq ($(SWEET_WIFI_EMBEDDED),1)" not in makefile
            or "$(SRCROOT)/bsd/dev/sweet_wifi_bundle.o" not in makefile
            or "-DSWEET_WIFI_EMBEDDED=1" not in makefile
            or "sweet_wifi_runtime_start" not in
            (xnu / "bsd/dev/sweet_wifi.c").read_text()):
        raise ValueError("apply patches 0302 and 0303 to this XNU tree first")
    target = xnu / "bsd/dev/sweet_wifi_bundle.o"
    with bundle.open("rb") as incoming, target.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True,
                        help="output of build_wifi_kernel_archive.py")
    parser.add_argument("--xnu", type=Path, required=True, help="patched XNU source tree")
    args = parser.parse_args()
    print(stage(args.build, args.xnu))
    print("Set SWEET_WIFI_EMBEDDED=1 for the local SWEET kernel build.")


if __name__ == "__main__":
    main()
