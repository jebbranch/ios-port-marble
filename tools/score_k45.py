#!/usr/bin/env python3
"""Score K4 / K5 against the tables committed in docs/audio-closeout.md BEFORE
either was booted (commit b4e6de4).

Both rounds are register-self-proving, which is the whole point of 0272+0273:
  K4 sweetmfc=4  - the vendor's four-word / declared-14 MFC output record.  The
                   primary read is the post-set line's WIDTH: 24 means the
                   vendor's container got stored, 32 means it is still K2's own
                   value, and rate=-1 means the 14-byte record was refused where
                   0273's 12-byte one was accepted - a real negative.
  K5 sweettdmvendor bit3 - the TDM endpoint's MF_CFG second word 0x00030020 ->
                   0x00030018, read back from the module K1 proved readable.
The WR slot is printed as the known E2 (that module declines GET) and is never
treated as evidence in either direction.

  tools/score_k45.py k4 logs/ring-k4-*.txt <sweettime> [image.img]
"""
import re
import sys

import score_k1 as K


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: score_k45.py <k4|k5> <ring> <sweettime> [image]")
    mode, ring, want = sys.argv[1], sys.argv[2], sys.argv[3]
    img = sys.argv[4] if len(sys.argv) > 4 else None
    if mode not in ("k4", "k5"):
        sys.exit("mode must be k4 or k5")

    print(f"=== {mode.upper()}-0 identity ===")
    if K.count(ring, f"sweettime={want}") == 0:
        sys.exit(f"  FAIL: no sweettime={want} in {ring} - wrong ring, nothing scored")
    print(f"  sweettime={want} hits={K.count(ring, want)}")
    if img:
        for pat, minimum in (("SWEET MARK: tdmmf v1 ", 1), ("mfcget v4 ", 5),
                             ("mfcget v3 ", 0)):
            n = int(K.sh(f"LC_ALL=C grep -ao -F '{pat}' {img} | wc -l").strip() or 0)
            ok = n >= minimum if minimum else n == 0
            print(f"  image '{pat}' x {n} "
                  f"({'OK' if ok else '<< IDENTITY FAIL - not the 0273 build'})")
    label = re.sub(r"^.*?ring-", "", ring).replace(".txt", "")
    print(K.sh(f"python3 tools/ring_window.py {label} 2>&1 | tail -6"))

    got, problems = K.parse(ring, K.ORDER_K2)
    print(f"=== {mode.upper()}-1 the 0273 witness lines ===")
    for l in K.lines(ring, "tdmmf v1 "):
        print("  " + l.strip()[:170])
    for l in K.lines(ring, "mfc v1 sel="):
        print("  " + l.strip()[:170])
    want_tv = "tv=7 w1=0x30020" if mode == "k4" else "tv=15 w1=0x30018"
    n_tv = K.count(ring, want_tv)
    print(f"  {'OK ' if n_tv == 1 else 'MISS'} ({n_tv}) tdmmf says {want_tv}"
          f"  <- the knob state this round claims")

    print(f"=== {mode.upper()}-2 every read line (9 expected) ===")
    for p in problems:
        print(f"  !! {p}")
    n = 0
    for phase in ("pre-set", "post-set", "post-prepare", "post-start", "post-tone"):
        for iid, f in sorted(got.get(phase, {}).items()):
            n += 1
            print(f"  {phase:13s} {f['line'][:188]}")
    print(f"  total GET lines: {n}")

    def slot(phase, iid):
        return got.get(phase, {}).get(iid)

    print(f"=== {mode.upper()}-3 the primary gate ===")
    if mode == "k4":
        post = slot("post-set", 0x48ba)
        bw = (post or {}).get("bw")
        rate = (post or {}).get("rate")
        if post is None:
            print("  MISSING: no post-set line - sweetmfcget gate or the write did "
                  "not run; instrument reading, not a module answer")
        elif rate == -1:
            print("  REFUSED-14: the vendor's 14-byte/four-word record is NOT accepted "
                  "where 0273's own 12-byte record was (K2 read 96000/32/2). This is a "
                  "negative about the param's shape, and it is worth having.")
        elif bw == 24:
            print(f"  STORED-24: post-set reads rate={rate} bw={bw} nch={(post or {}).get('nch')} "
                  "- the vendor's container width is in the module")
        elif bw == 32:
            print("  KEPT-32: still the 32-bit width from our own 12-byte record - the "
                  "module did not take the new word even though the set was answered")
        else:
            print(f"  NEW SHAPE (bw={bw} rate={rate}) - report as-is, do not rename")
        for phase in ("post-prepare", "post-start", "post-tone"):
            f = slot(phase, 0x48ba)
            print(f"  {phase:13s} MFC bw={(f or {}).get('bw')} rate={(f or {}).get('rate')} "
                  f"-> {K.mfc_state(f)}")
    else:
        st = slot("post-start", 0x428f)
        tone = slot("post-tone", 0x428f)
        d1 = (st or {}).get("d1")
        if d1 == 0x30018:
            print(f"  STORED-24 at the endpoint: post-start d1=0x{d1:x} "
                  f"(d0=0x{(st or {}).get('d0', 0):x} d2={(st or {}).get('d2')}), "
                  f"post-tone d1=0x{(tone or {}).get('d1', 0):x}")
        elif d1 == 0x30020:
            print("  NOT REWRITTEN: the endpoint still reports our old 32-bit word even "
                  "though bit3 changed what we SEND (check the tdmmf line above) - that "
                  "combination would mean the width is fixed at graph open, not by SET_CFG")
        else:
            print(f"  NEW SHAPE (d1={d1}) - report as-is")
        print(f"  MFC control this round: post-start {K.mfc_state(slot('post-start', 0x48ba))}")
    print(f"  WR slot (expected E2, never evidence): post-start "
          f"{K.wr_state(slot('post-start', 0x40bd))}")
    print(f"  TDM slot as instrument control:       post-start "
          f"{K.tdm_state(slot('post-start', 0x428f))}")

    print(f"=== {mode.upper()}-4 instrument-failure shapes ===")
    kinds = [f["kind"] for d in got.values() for f in d.values()]
    for k in ("no", "basic", "unexpected"):
        print(f"  {k} branch lines: {kinds.count(k)}")

    print(f"=== {mode.upper()}-5 regression, item by item against K2/K3 ===")
    for pat in ("graph_open/vp-wr rsp op=0x1001000 status=0",
                "graph_prepare rsp op=0x1001001 status=0",
                "graph_start rsp op=0x1001002 status=0",
                "tone gain g=6 peak=6144", "pcm tone done (120 buffers)",
                "cba2r@", "ats1pr iova=0x1ffe8000 -> par=0 OK",
                "geni amp 0x34 r=0 id=0x2032", "shm mapped", "not sent"):
        c = K.count(ring, pat)
        print(f"  {('OK  ' if c >= 1 else 'MISS')} ({c}) {pat}")
    print(f"  data_buffer send: {K.count(ring, 'data_buffer send')} (K2/K3: 121)  "
          f"no rsp: {K.count(ring, 'no rsp')}  panic: {K.count(ring, 'panic')}")
    print(f"  done/PSCI: {K.count(ring, '===== done =====')} / "
          f"{K.count(ring, 'PSCI SYSTEM_RESET')}")
    print("  ear gate: sweetvol=6 = -14.5 dBFS; the reading belongs to the human, "
          "and both tables make it secondary to the register gate above.")


if __name__ == "__main__":
    main()
