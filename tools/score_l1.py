#!/usr/bin/env python3
"""Score the L1 round against the gate table committed before booting.

Two rules this encodes, both learned the hard way:
  * identity first - a ring whose sweettime is not this image's is not scorable;
  * the regression gates are counted *against K1's ring*, not against numbers I
    remember, because "vp/=9" counts response lines while a plain grep counts
    sends plus responses.
"""
import re
import subprocess
import sys

ring = sys.argv[1]
want_time = sys.argv[2] if len(sys.argv) > 2 else None
K1 = "logs/ring-k1-iidonly.txt"


def count(path, pat):
    out = subprocess.run(["sh", "-c", f"LC_ALL=C grep -ac '{pat}' {path} || true"],
                         capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def lines(path, pat):
    out = subprocess.run(["sh", "-c", f"LC_ALL=C grep -a '{pat}' {path} || true"],
                         capture_output=True, text=True)
    return [l.rstrip() for l in out.stdout.splitlines()]


if want_time and count(ring, f"sweettime={want_time}") == 0:
    sys.exit(f"L1-identity: no sweettime={want_time} in {ring} - wrong ring, nothing scored")

print("=== L1-1 the send is in-band ===")
for l in lines(ring, "oobcfg v9 try") + lines(ring, "APM oob_batch"):
    print("  " + l[:160])

print("=== L1-2 main gate: the response ===")
rsp = lines(ring, "oob_batch.*rsp op=0x1001006")
if not rsp:
    print("  (no oob_batch response line at all)")
for l in rsp:
    print("  " + l[:160])
codes = [m.group(1) for l in rsp for m in [re.search(r"status=(0x[0-9a-f]+|\d+)", l)] if m]
print(f"  status read: {codes}")

print("=== probe self-evidence (one send only) ===")
for pat in ("oobcfg v9 rec ", "oobcfg v9 body touched=", "is past the declared region",
            "recs not read back"):
    print(f"  {pat:34} {count(ring, pat)}")

print("=== L1-3/L1-4 counters, this ring vs K1 ===")
for name, pat in (("data_buffer send", "data_buffer send"),
                  ("done status=0", "done status=0"),
                  ("no rsp", "no rsp"),
                  ("vp/ all", "vp/"),
                  ("vp/ rsp", "vp/.*rsp"),
                  ("gate flag", "gate flag"),
                  ("panic", "panic"),
                  ("oobcfg v8 (must be 0)", "oobcfg v8")):
    print(f"  {name:24} L1={count(ring, pat):4}   K1={count(K1, pat):4}")

print("=== verdict, per the pre-boot table ===")
if not codes:
    print("  no status line => L1-2 falls on the no-rsp branch: do not score, stop and report")
else:
    c = codes[0]
    if c == "0":
        print("  branch 1: in-band accepted => the wall is the out-of-band path")
    elif c == "0x1":
        print("  branch 2: in-band refused too => length is not proven to be the rule")
        print("            (vendor never sends in-band SET_CFG; L2 is the next discriminating shot)")
    else:
        print(f"  branch 3: a new distinguishable reading ({c}) => bank it as a baseline code")
