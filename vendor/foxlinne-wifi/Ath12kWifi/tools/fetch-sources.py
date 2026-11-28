#!/usr/bin/env python3
"""Resolve one locked source version for ath12k, mac80211 and cfg80211."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def git(directory, *arguments):
    return subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                          capture_output=True, text=True).stdout.strip()


def resolve(name, source, check_only):
    directory = ROOT / "deps" / name
    if not directory.exists():
        if check_only:
            raise SystemExit(f"missing source: {directory}")
        subprocess.run(["git", "clone", "--depth", "1", "--branch", source["ref"],
                        "--filter=blob:none", "--sparse", source["url"], str(directory)], check=True)
    revision = git(directory, "rev-parse", "HEAD")
    if revision != source["commit"]:
        raise SystemExit(f"{name}: expected {source['commit']}, found {revision}; preserve the existing checkout and select the locked source")
    paths = [path.lstrip("/") for path in source["paths"]]
    # Some hosts have case-folding changes in unrelated kernel headers; only
    # imported source directories participate in this component's identity.
    changed = git(directory, "diff", "--name-only", "HEAD", "--", *paths).splitlines()
    imported_changes = [path for path in changed if not path.startswith("include/uapi/linux/netfilter")]
    if imported_changes:
        raise SystemExit(f"{name}: imported source was modified: {', '.join(imported_changes[:8])}")
    missing = [path for path in source["required"] if not (directory / path).is_file()]
    if missing and check_only:
        raise SystemExit(f"{name}: missing sparse paths: {', '.join(missing)}; run tools/fetch-sources.sh")
    if not check_only:
        subprocess.run(["git", "-C", str(directory), "sparse-checkout", "set", "--no-cone", *source["paths"]], check=True)
    hashes = {path: hashlib.sha256((directory / path).read_bytes()).hexdigest()
              for path in source["required"]}
    return {"revision": revision, "files_sha256": hashes}


def resolve_archive(source, check_only):
    archive = ROOT / "deps" / source["archive"]
    if not archive.exists():
        if check_only:
            raise SystemExit(f"missing source archive: {archive}")
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(source["url"], timeout=45) as response, temporary.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != source["sha256"]:
            temporary.unlink()
            raise SystemExit("supplicant archive hash mismatch")
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != source["sha256"]:
        raise SystemExit("supplicant archive hash mismatch")
    directory = ROOT / "deps" / source["directory"]
    hashes = {}
    with tarfile.open(archive) as package:
        if not directory.exists():
            if check_only:
                raise SystemExit(f"missing supplicant source: {directory}")
            # Only regular files/directories under the selected release root.
            for member in package.getmembers():
                path = (ROOT / "deps" / member.name).resolve()
                if (not (member.isfile() or member.isdir()) or
                        directory.resolve() not in (path, *path.parents)):
                    raise SystemExit(f"unexpected archive member: {member.name}")
            package.extractall(ROOT / "deps")
        for name in source["required"]:
            expected = package.extractfile(f"{source['directory']}/{name}").read()
            path = directory / name
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f"supplicant imported source mismatch: {name}")
            hashes[name] = hashlib.sha256(expected).hexdigest()
    return {"archive_sha256": source["sha256"], "files_sha256": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify installed sources without fetching")
    args = parser.parse_args()
    lock_path = ROOT / "tools/source-lock.json"
    lock = json.loads(lock_path.read_text())
    if lock["schema"] != 1:
        raise SystemExit("unsupported source-lock schema")
    (ROOT / "deps").mkdir(exist_ok=True)
    report = {"source_lock_sha256": hashlib.sha256(lock_path.read_bytes()).hexdigest(),
              "sources": {name: resolve(name, lock[name], args.check) for name in ("linux", "freebsd")}}
    report["sources"]["supplicant"] = resolve_archive(lock["supplicant"], args.check)
    output = ROOT / "out/source-identity.json"
    output.parent.mkdir(exist_ok=True)
    temporary = output.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    temporary.replace(output)
    print(f"verified locked Linux/mac80211/cfg80211, FreeBSD and supplicant sources: {output}")


if __name__ == "__main__":
    main()
