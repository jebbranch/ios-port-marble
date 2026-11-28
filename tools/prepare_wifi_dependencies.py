#!/usr/bin/env python3
"""Prepare locked public dependencies, fetching commits instead of moving refs."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
MODULE = ROOT / "vendor/foxlinne-wifi/Ath12kWifi"
# The stable kernel.org server can ignore blob:none and send the whole tree.
LINUX_MIRROR = "https://github.com/gregkh/linux.git"


def git(path, *args):
    subprocess.run(["git", "-C", str(path), *args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", choices=("linux", "freebsd", "supplicant", "prepare"))
    args = parser.parse_args()
    lock = json.loads((MODULE / "tools/source-lock.json").read_text())
    deps = MODULE / "deps"
    deps.mkdir(exist_ok=True)
    if args.name in ("linux", "freebsd"):
        entry = lock[args.name]
        target = deps / args.name
        if target.exists():
            raise ValueError("dependency already exists; verify separately, never overwrite")
        target.mkdir()
        git(target, "init", "-q")
        git(target, "remote", "add", "origin",
            LINUX_MIRROR if args.name == "linux" else entry["url"])
        git(target, "config", "remote.origin.promisor", "true")
        git(target, "config", "remote.origin.partialclonefilter", "blob:none")
        git(target, "fetch", "--filter=blob:none", "--depth=1", "origin", entry["commit"])
        git(target, "sparse-checkout", "set", "--no-cone", *entry["paths"])
        git(target, "checkout", "--detach", "FETCH_HEAD")
        revision = subprocess.check_output(
            ["git", "-C", str(target), "rev-parse", "HEAD"], text=True).strip()
        if revision != entry["commit"]:
            raise ValueError("immutable dependency identity mismatch")
    elif args.name == "supplicant":
        # The upstream archive loader validates digest and safe members.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "wifi_locked_sources", MODULE / "tools/fetch-sources.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        print(module.resolve_archive(lock["supplicant"], False))
    else:
        subprocess.run(["python3", str(MODULE / "tools/fetch-sources.py"), "--check"],
                       check=True)
        subprocess.run(["bash", str(MODULE / "tools/prepare.sh")], check=True)
        subprocess.run(["python3", str(MODULE / "tools/prepare-ath11k.py")], check=True)


if __name__ == "__main__":
    main()
