#!/usr/bin/env python3
"""Score K6a and K7 against the tables committed before either booted.

  tools/score_k6.py k6a logs/ring-k6a-*.txt <sweettime> [image]
  tools/score_k6.py k7  logs/ring-k7-*.txt  <sweettime> [image]

K6a (sweetvgraph=4, NO sweetmfc in the cmdline): the primary read is the PRE-set
answer - whether a vendor-shaped container handed the converter a format before
we wrote a single byte.  Note two things this scorer must not get wrong:
  * graph_open/vp-wr is EXPECTED to be absent, because this round does not open
    our own container; an absent line we predicted is not a regression;
  * if no module in the foreign container consumes DATA_BUFFER_V2, the ear is
    NOT TESTED - which is not the same sentence as "silent".
K7 (our graph, sweetdbb=4096 sweetvol=16): everything K4 proved must still hold;
the product of this round is the ear, so the hard gate is that the ring tail
survives in-window at all.
"""
import re
import sys

import score_k1 as K

MODES = ("k6a", "k7")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    mode, ring, want = sys.argv[1], sys.argv[2], sys.argv[3]
    img = sys.argv[4] if len(sys.argv) > 4 else None
    if mode not in MODES:
        sys.exit("mode must be k6a or k7")

    print(f"=== {mode}-0 identity ===")
    if K.count(ring, f"sweettime={want}") == 0:
        sys.exit(f"  FAIL: no sweettime={want} in {ring}")
    print(f"  sweettime hits={K.count(ring, want)}")
    if img:
        for pat in ("vginl v1 ", "dbb v1 ", "tdmmf v1 ", "mfcget v4 ", "mfcget v3 "):
            n = int(K.sh(f"LC_ALL=C grep -ao -F '{pat}' {img} | wc -l").strip() or 0)
            exp = 0 if pat.endswith("v3 ") else 1 if pat != "mfcget v4 " else 5
            print(f"  image '{pat}' x {n} (want {exp}) "
                  f"{'OK' if n == exp else '<< IDENTITY FAIL'}")
    label = re.sub(r"^.*?ring-", "", ring).replace(".txt", "")
    print(K.sh(f"python3 tools/ring_window.py {label} 2>&1 | tail -6"))

    # K7 writes the MFC (sweetmfc=4), so its read set is K2/K4/K5's nine-line
    # ORDER_K2; K6a must not, or the round has no answer about its own graph.
    got, problems = K.parse(ring, K.ORDER_K2 if mode == "k7" else K.ORDER)
    print(f"=== {mode}-1 witnesses and the 8 read lines ===")
    for pat in ("vginl v1 ", "dbb v1 ", "tdmmf v1 ", "mfc v1 sel="):
        for l in K.lines(ring, pat):
            print("  " + l.strip()[:170])
    for p in problems:
        print(f"  !! {p}")
    n = 0
    for phase in ("pre-set", "post-set", "post-prepare", "post-start", "post-tone"):
        for iid, f in sorted(got.get(phase, {}).items()):
            n += 1
            print(f"  {phase:13s} {f['line'][:188]}")
    print(f"  GET lines: {n} ({8 if mode == chr(107)+chr(54)+chr(97) else 9} expected; "
          "9 only when sweetmfc is present, which K6a must not have)")

    def slot(phase, iid):
        return got.get(phase, {}).get(iid)

    if mode == "k6a":
        print("=== K6a-2/3 does the vendor-shaped container open and start ===")
        for pat in ("graph_open/ukee2960-inband rsp", "vgraph 4 (in band) started",
                    "vgraph 4 (in band) did not start", "vgraph 4 (in band) open refused",
                    "APM graph_open/ukee2960-inband"):
            print(f"  ({K.count(ring, pat)}) {pat}")
        sent = K.count(ring, "APM graph_open/ukee2960-inband")
        refused = K.count(ring, "vgraph 4 (in band) open refused")
        started = K.count(ring, "vgraph 4 (in band) started")
        if started == 0:
            print("  K6a-4 IS NOT ANSWERABLE: the in-band container never ran "
                  "(it was never sent, or it was answered and refused), so the "
                  "code fell through to our own 800-byte graph and every GET "
                  "line above describes OUR graph, not the vendor-shaped one. "
                  "Reading the status line is the product of this round; reading "
                  "the MFC here would be a control replay dressed as a verdict.")
            return
        if sent == 0 and refused >= 1:
            print("  NOT SENT - the packet never reached the wire (our own builder "
                  "refused it locally; g_apm_cmd's inline cap is 1024 bytes and its "
                  "guard returns -1 BEFORE printing).  This is an instrument "
                  "failure, NOT a DSP answer, and it makes K6a-4 unanswerable: the "
                  "code then falls through to our own 800-byte container, so every "
                  "line after it is a control replay, not a vendor-graph reading.")
            return
        print("=== K6a-4 PRIMARY: the MFC before we write anything ===")
        if K.count(ring, "form=inband") == 0:
            print("  REFUSED: no 'vginl ... form=inband' witness in this ring, so the "
                  "container was never opened in band and no MFC reading here is a "
                  "K6a reading. Score nothing.")
            return
        pre = slot("pre-set", 0x48ba)
        s = K.mfc_state(pre)
        if s.startswith("VALID"):
            print(f"  CONTAINER-FED: pre-set already answers {s} with no SET_CFG to "
                  "0x48ba in this round => the graph data feeds the converter; the "
                  "command sequence was never the mechanism")
        elif s.startswith("INVALID"):
            print("  STILL UNFED: a vendor-shaped container in the transport the vendor "
                  "actually uses does NOT feed the converter => the gap is the 18128 "
                  "custom config or the modules we do not have (0x40bf/0x40c0/0x4165)")
        else:
            print(f"  {s} - instrument reading, not a module answer; score nothing else")
        for phase in ("post-prepare", "post-start", "post-tone"):
            print(f"  {phase:13s} MFC {K.mfc_state(slot(phase, 0x48ba))}   "
                  f"TDM {K.tdm_state(slot(phase, 0x428f))}")
        print("=== K6a-5 was any PCM actually consumed ===")
        print(f"  'pcm tone done': {K.count(ring, 'pcm tone done')}   "
              f"'not consumed': {K.count(ring, 'not consumed')}   "
              f"data_buffer send: {K.count(ring, 'data_buffer send')}")
        print(f"  => ear is {'VALID' if K.count(ring, 'not consumed') == 0 and K.count(ring, 'pcm tone done') else 'NOT TESTED (foreign container did not take the buffers)'}")
        print("=== K6a-6 regression AS REDEFINED FOR A DIFFERENT GRAPH ===")
        for pat, must in (("ats1pr iova=0x1ffe8000 -> par=0 OK", True),
                          ("cba2r@", True), ("geni amp 0x34 r=0 id=0x2032", True),
                          ("tone gain g=6 peak=6144", True), ("no rsp", False),
                          ("panic", False), ("===== done =====", True),
                          ("PSCI SYSTEM_RESET", True)):
            c = K.count(ring, pat)
            ok = (c >= 1) if must else (c == 0)
            print(f"  {'OK  ' if ok else 'CHECK'} ({c}) {pat}")
        print(f"  note: 'graph_open/vp-wr' count {K.count(ring, 'graph_open/vp-wr')} - "
              "absent by design this round, NOT a regression")
    else:
        print("=== K7-1 geometry witness ===")
        print(f"  ({K.count(ring, 'dbb v1 bytes=4096 ms=21')}) dbb v1 bytes=4096 ms=21")
        print(f"  ({K.count(ring, 'tone gain g=16 peak=16384')}) tone gain g=16 peak=16384")
        print("=== K7-2 registers must be unchanged from K4 ===")
        for phase in ("pre-set", "post-set", "post-prepare", "post-start", "post-tone"):
            print(f"  {phase:13s} MFC {K.mfc_state(slot(phase, 0x48ba))}")
        print(f"  TDM  post-start {K.tdm_state(slot('post-start', 0x428f))}")
        print(f"  'not consumed' {K.count(ring, 'not consumed')} (want 0)   "
              f"data_buffer send {K.count(ring, 'data_buffer send')}")
        print(f"  'pcm tone done': {K.count(ring, 'pcm tone done')}   "
              f"no rsp {K.count(ring, 'no rsp')}   panic {K.count(ring, 'panic')}")
        print(f"  tail: '===== done =====' {K.count(ring, '===== done =====')}  "
              "PSCI " + str(K.count(ring, "PSCI SYSTEM_RESET")) +
              "  (both must be IN-WINDOW per ring_window above, or the ear is untested)")
        print("=== K7-3 the ear (the only product of this round) ===")
        print("  ~2.5 s of audio at peak 16384 = -6.0 dBFS (clamp headroom to g=31 = "
              "-0.27 dBFS). The reading belongs to the human.")


if __name__ == "__main__":
    main()
