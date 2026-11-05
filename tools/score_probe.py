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
import pathlib
import subprocess
import sys

ROUNDS = {
    "l1": {
        "ver": "v9", "prefix": "oobcfg v9", "tag": "oob_batch", "old": ["v8"],
        "base": "logs/ring-k1-iidonly.txt",
        "expect": ["oobcfg v9 try 0 form=inband off=0x1000 len=288 recs=4"],
        "absent": [],
        "verdict": {
            "0": "branch 1: in-band accepted => the wall is the out-of-band path",
            "0x1": "branch 2: in-band refused too => length is not proven to be the rule "
                   "(vendor never sends an in-band SET_CFG; L2 is the next discriminating shot)",
        },
    },
    "l2": {
        "ver": "v10", "prefix": "oobcfg v10", "tag": "oob_batch", "old": ["v9", "v8"],
        "base": "logs/ring-l1-inband.txt",
        "expect": ["oobcfg v10 try 0 form=inband off=0x1000 len=216 recs=3",
                   "APM oob_batch src=0x2010 dst=0x1 (256 bytes)"],
        "absent": [],
        "verdict": {
            "0": "branch 1: 216 B in-band is command-level accepted => the length that "
                 "crosses 256 is in the boundary, and <=256 in-band + real iid + real param "
                 "+ size 52 is a usable combination",
            "0x1": "branch 2: same code as L1 at 288 B => length is NOT in the boundary; "
                   "against C only the param id and the per-record size are left",
        },
    },
    # M1c = the same kernel with sweetcccfg removed, so nothing is sent at all
    "m1c": {
        "ver": "v2", "prefix": "cccfg v2", "tag": "cc_batch", "old": ["v1"],
        "base": "logs/ring-m1-cccfg.txt",
        "expect": [],
        # the control gate: the probe must NOT be in the ring
        "absent": ["cccfg v2 dst=1 inband", "cccfg v2 rec ", "cc_batch", "cccfg v2 reject",
                   "cccfg v2 bad size", "cccfg v2 walk"],
        "verdict": {},
    },
    # M1 = C's accepted container (0263) with only the two param ids moved to 0x08001004.
    "m1": {
        "ver": "v2", "prefix": "cccfg v2", "tag": "cc_batch", "old": ["v1"],
        "base": "logs/ring-c2-ccbatch.txt",
        "expect": ["cccfg v2 dst=1 inband recs=2 len=80",
                   "cccfg v2 rec 0 iid=0x428f param=0x8001004 size=12",
                   "cccfg v2 rec 1 iid=0x428f param=0x8001004 size=28",
                   "APM cc_batch src=0x2010 dst=0x1 (120 bytes)"],
        # M1-5: any of these means the container never went out => not a param reading
        "absent": ["cccfg v2 reject", "cccfg v2 bad size", "cccfg v2 walk"],
        "verdict": {
            "0": "branch 1: param does not participate even on the accepted baseline => what is "
                 "left against C is the per-record size (next shot M2s: sizes 12/28 -> 52/52)",
            "0x1": "branch 2: param=0x08001004 alone turns an accepted container into a refused "
                   "one => APM gates on param, and this key is the chain-level one in ACDB "
                   "(next shot M2r moves param+size, corroborating only, not single-variable)",
        },
    },
    # N1 = K1's out-of-band send (0264) with error_code pre-set to 0xa5a5a5a5.
    "n1": {
        "ver": "v11", "prefix": "oobcfg v11", "tag": "oob_batch", "old": ["v10", "v9", "v8"],
        "base": "logs/ring-k1-iidonly.txt",
        "expect": ["oobcfg v11 try 0 form=bare off=0x1000 addr=0x1000 len=288 recs=4"],
        "absent": [],
        "verdict": {
            "0x1": "expected (== K1): the pre-set field did not move the code, so the record-level "
                   "reading is now the only new thing this round buys - read touched= and err=",
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


def window_check(ring):
    """The decoder prints raw[4096 : 4096+total].  Anything past that byte is a PREVIOUS
    boot's leftovers - the ring is never erased - and M1 proved those leftovers include
    `pcm tone done` and `PSCI SYSTEM_RESET` at the exact offsets the prior round used.
    So score the decoded window only, and say out loud when the window and the raw
    disagree."""
    import struct
    raw = pathlib.Path("logs/raw") / (pathlib.Path(ring).stem + ".bin")
    if not raw.exists():
        print(f"  window: no raw file ({raw}) - scored bytes are unverified")
        return None
    d = raw.read_bytes()
    magic = d[:8]
    ver, size = struct.unpack_from("<II", d, 8)
    total, seq = struct.unpack_from("<QQ", d, 16)
    txt = pathlib.Path(ring).stat().st_size
    print(f"  window: magic={magic.decode(errors='replace')} total={total:#x} seq={seq} "
          f"decoded={txt:#x} match={total == txt}")
    for name in ("pcm tone done", "===== done =====", "PSCI SYSTEM_RESET"):
        at = d.find(name.encode(), 4096 + total)
        if at >= 0:
            print(f"  !! '{name}' exists in the raw at {at:#x}, past this session's window - "
                  "stale bytes from an earlier boot, not evidence")
    return total


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: score_probe.py <l1|l2|m1|m1c|n1> <ring> <sweettime>")
    round_id, ring, want_time = sys.argv[1], sys.argv[2], sys.argv[3]
    cfg = ROUNDS[round_id]
    pre, tag, base = cfg["prefix"], cfg["tag"], cfg["base"]

    if count(ring, f"sweettime={want_time}") == 0:
        sys.exit(f"identity: no sweettime={want_time} in {ring} - wrong ring, nothing scored")

    print(f"=== {round_id.upper()}-1 the send is what the gate says ===")
    for want in cfg["expect"]:
        hit = count(ring, want)
        print(f"  {'OK ' if hit == 1 else 'MISS'} ({hit}) {want}")
    for l in lines(ring, pre) + lines(ring, f"APM {tag}"):
        print("  " + l[:170])

    print(f"=== {round_id.upper()}-5 the container must not have been refused locally ===")
    for want in cfg["absent"] or [f"{pre} no mapping"]:
        hit = count(ring, want)
        print(f"  {'OK ' if hit == 0 else 'HIT '} ({hit}) {want}")

    print(f"=== {round_id.upper()}-2 main gate: the response ===")
    rsp = lines(ring, f"{tag}.*rsp op=0x1001006")
    for l in rsp:
        print("  " + l[:170])
    codes = [m.group(1) for l in rsp for m in [re.search(r"status=(0x[0-9a-f]+|\d+)", l)] if m]
    print(f"  status read: {codes}")

    print("=== probe self-evidence (one send only) ===")
    for pat in (f"{pre} rec ", f"{pre} body touched=",
                "is past the declared region", "recs not read back", f"{pre} no mapping"):
        print(f"  {pat:40} {count(ring, pat)}")

    print(f"=== {round_id.upper()}-3/-4 counters, this ring vs {base} ===")
    window_check(ring)
    for name, pat in (("pcm tone done", "pcm tone done"),
                      ("===== done =====", "===== done ====="),
                      ("PSCI SYSTEM_RESET", "PSCI SYSTEM_RESET"),
                      ("data_buffer send", "data_buffer send"),
                      ("done status=0", "done status=0"),
                      ("no rsp", "no rsp"),
                      ("vp/ all", "vp/"),
                      ("vp/ rsp", "vp/.*rsp"),
                      ("gate flag", "gate flag"),
                      ("panic", "panic")):
        print(f"  {name:26} round={count(ring, pat):4}   base={count(base, pat):4}")
    for old in cfg["old"]:
        pat = f"oobcfg {old} " if round_id.startswith("n") or round_id.startswith("l") else f"cccfg {old} "
        print(f"  older marker {pat!r:22} round={count(ring, pat):4} (want 0)")

    print(f"=== {round_id.upper()}-instrument (recorded, not evidence unless N1) ===")
    for l in lines(ring, f"{pre} body touched="):
        print("  " + l[:170])
    for l in lines(ring, f"{pre} rec "):
        print("  " + l[:170])
    if round_id != "n1":
        print("  touched=0 cannot separate 'never wrote' from 'wrote zero' (field pre-zeroed)")
    else:
        print("  sentinel 0xa5a5a5a5: touched=0 => never written; err=0xa5a5a5a5 on all four is the pre-set surviving")

    print("=== verdict, per the pre-boot table ===")
    if not codes:
        print("  no status line => the no-rsp branch: do not score, stop and report")
    else:
        print("  " + cfg["verdict"].get(codes[0],
              f"a third distinguishable reading ({codes[0]}) => bank it as a baseline code"))


if __name__ == "__main__":
    main()
