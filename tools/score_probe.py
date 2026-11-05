#!/usr/bin/env python3
"""Score a SET_CFG probe round against the gate table committed before booting.

Two rules this encodes, both learned the hard way:
  * identity first - a ring whose sweettime is not this image's is not scorable;
  * the regression gates are counted against the *previous* round's own ring, not
    against numbers I remember, because "vp/=9" counts response lines while a
    plain grep counts sends plus responses.

  tools/score_probe.py l2 logs/ring-l2-3recs.txt <sweettime>
"""
import re
import subprocess
import sys

ROUNDS = {
    "l1": {
        "ver": "v9", "base": "logs/ring-k1-iidonly.txt",
        "expect": ["oobcfg v9 try 0 form=inband off=0x1000 len=288 recs=4"],
        "verdict": {
            "0": "branch 1: in-band accepted => the wall is the out-of-band path",
            "0x1": "branch 2: in-band refused too => length is not proven to be the rule "
                   "(vendor never sends an in-band SET_CFG; L2 is the next discriminating shot)",
        },
    },
    "l2": {
        "ver": "v10", "base": "logs/ring-l1-inband.txt",
        "expect": ["oobcfg v10 try 0 form=inband off=0x1000 len=216 recs=3",
                   "APM oob_batch src=0x2010 dst=0x1 (256 bytes)"],
        "verdict": {
            "0": "branch 1: 216 B in-band is command-level accepted => the length that "
                 "crosses 256 is in the boundary, and <=256 in-band + real iid + real param "
                 "+ size 52 is a usable combination",
            "0x1": "branch 2: same code as L1 at 288 B => length is NOT in the boundary; "
                   "against C only the param id and the per-record size are left",
        },
    },
}


def count(path, pat):
    out = subprocess.run(["sh", "-c", f"LC_ALL=C grep -ac '{pat}' {path} || true"],
                         capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def lines(path, pat):
    out = subprocess.run(["sh", "-c", f"LC_ALL=C grep -a '{pat}' {path} || true"],
                         capture_output=True, text=True)
    return [l.rstrip() for l in out.stdout.splitlines()]


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: score_probe.py <l1|l2> <ring> <sweettime>")
    round_id, ring, want_time = sys.argv[1], sys.argv[2], sys.argv[3]
    cfg = ROUNDS[round_id]
    ver, base = cfg["ver"], cfg["base"]

    if count(ring, f"sweettime={want_time}") == 0:
        sys.exit(f"identity: no sweettime={want_time} in {ring} - wrong ring, nothing scored")

    print(f"=== {round_id.upper()}-1 the send is what the gate says ===")
    for want in cfg["expect"]:
        hit = count(ring, want)
        print(f"  {'OK ' if hit == 1 else 'MISS'} ({hit}) {want}")
    for l in lines(ring, f"oobcfg {ver} try") + lines(ring, "APM oob_batch"):
        print("  " + l[:170])

    print(f"=== {round_id.upper()}-2 main gate: the response ===")
    rsp = lines(ring, "oob_batch.*rsp op=0x1001006")
    for l in rsp:
        print("  " + l[:170])
    codes = [m.group(1) for l in rsp for m in [re.search(r"status=(0x[0-9a-f]+|\d+)", l)] if m]
    print(f"  status read: {codes}")

    print("=== probe self-evidence (one send only) ===")
    for pat in (f"oobcfg {ver} rec ", f"oobcfg {ver} body touched=",
                "is past the declared region", "recs not read back", f"oobcfg {ver} no mapping"):
        print(f"  {pat:38} {count(ring, pat)}")

    print(f"=== {round_id.upper()}-3/-4 counters, this ring vs {base} ===")
    for name, pat in (("data_buffer send", "data_buffer send"),
                      ("done status=0", "done status=0"),
                      ("no rsp", "no rsp"),
                      ("vp/ all", "vp/"),
                      ("vp/ rsp", "vp/.*rsp"),
                      ("gate flag", "gate flag"),
                      ("panic", "panic"),
                      (f"older markers (want 0)", f"oobcfg {'v9' if ver == 'v10' else 'v8'} ")):
        print(f"  {name:26} round={count(ring, pat):4}   base={count(base, pat):4}")

    print(f"=== {round_id.upper()}-5 instrument (recorded, not evidence) ===")
    for l in lines(ring, f"oobcfg {ver} body touched="):
        print("  " + l[:170])
    print("  touched=0 cannot separate 'never wrote' from 'wrote zero' (field pre-zeroed)")

    print("=== verdict, per the pre-boot table ===")
    if not codes:
        print("  no status line => the no-rsp branch: do not score, stop and report")
    else:
        print("  " + cfg["verdict"].get(codes[0],
              f"a third distinguishable reading ({codes[0]}) => bank it as a baseline code"))


if __name__ == "__main__":
    main()
