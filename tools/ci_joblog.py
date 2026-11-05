#!/usr/bin/env python3
"""Fetch the Actions job log for one commit and print the patch-skip lines.

Nothing here boots a device; it only reads CI's own report, which the release
asset hides because our grep excludes the whole "Skipping patch ..." line.
"""
import json
import re
import subprocess
import sys
import tempfile
import os

REPO = "jebbranch/ios-port-marble"
SHA = sys.argv[1] if len(sys.argv) > 1 else "11ac91795cea5357f3b8bf9f82a8aaa50a7b1bcf"
PROXY = "http://127.0.0.1:7890"


def token():
    r = subprocess.run(["git", "credential", "fill"],
                       input="protocol=https\nhost=github.com\n\n",
                       text=True, capture_output=True, check=True)
    for line in r.stdout.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    raise SystemExit("no GitHub credential in Keychain")


def api(path, tok):
    out = subprocess.run(["curl", "-fsSL", "--max-time", "90", "-x", PROXY,
                          "-H", f"Authorization: Bearer {tok}",
                          "-H", "Accept: application/vnd.github+json",
                          f"https://api.github.com{path}"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"api {path} failed: {out.stderr[:300]}")
    return json.loads(out.stdout)


tok = token()
runs = api(f"/repos/{REPO}/actions/runs?head_sha={SHA}&per_page=5", tok).get("workflow_runs", [])
if not runs:
    raise SystemExit("no run for that sha")
run = runs[0]
print(f"run {run['run_number']} id={run['id']} status={run['status']} conclusion={run['conclusion']}")
jobs = api(f"/repos/{REPO}/actions/runs/{run['id']}/jobs", tok).get("jobs", [])
for job in jobs:
    print(f"job {job['id']} {job['name']} {job['conclusion']}")
    steps = [(s['number'], s['name'], s['conclusion']) for s in job.get('steps', [])]
    for n, name, concl in steps:
        if concl not in (None, "success", "skipped"):
            print(f"  FAILED STEP {n}: {name} -> {concl}")

job = jobs[0]
# /answers with a 302 to the archive; curl without -L prints the redirect target
red = subprocess.run(["curl", "-s", "--max-time", "90", "-x", PROXY, "-o", "/dev/null", "-w", "%{redirect_url}",
                      "-H", f"Authorization: Bearer {tok}",
                      f"https://api.github.com/repos/{REPO}/actions/jobs/{job['id']}/logs"],
                     capture_output=True, text=True)
loc = red.stdout.strip()
if not loc:
    body = subprocess.run(["curl", "-s", "--max-time", "90", "-x", PROXY,
                           "-H", f"Authorization: Bearer {tok}",
                           f"https://api.github.com/repos/{REPO}/actions/jobs/{job['id']}/logs"],
                          capture_output=True, text=True).stdout
    print("no redirect; first bytes of body:")
    print(body[:400])
    raise SystemExit(1)
path = tempfile.mktemp(suffix=".log")
subprocess.run(["curl", "-fsSL", "--max-time", "600", "-x", PROXY, "-o", path, loc], check=True)
print(f"log bytes={os.path.getsize(path)} -> {path}")
skip = []
with open(path, "r", errors="replace") as fh:
    for line in fh:
        if "Skipping patch" in line or "missing kernel marker" in line or "patch failed" in line \
           or "Running script" in line and "sweet" in line:
            skip.append(line.rstrip()[:400])
print(f"=== {len(skip)} skip/marker lines ===")
for s in skip[:40]:
    print(s)
