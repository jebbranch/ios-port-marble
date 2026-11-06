#!/usr/bin/env python3
"""Score K2 - the first time this port WRITES MFC state - against the table
committed in docs/audio-closeout.md before the boot.

K1 settled two things this round leans on:
  * a GET body is retrievable when the module stores it (TDM 0x428f/0x08001017
    came back byte-identical to what g_audio_graph_cfg_vp wrote);
  * a GET that answers st!=0/err!=0 with param_size still 72 means "this module
    will not answer this param" (WR 0x40bd), which says NOTHING about storage.
So K2's primary gate is not the ear, it is the post-set read-back of the exact
12 bytes 0268's sweetmfc=1 sends: {0x00017700, 0x00020020, 0x00020001}.

  tools/score_k2.py logs/ring-k2-mfc-set.txt <sweettime> [image.img]
"""
import sys

import score_k1 as K

WRITTEN = {0x00017700: 96000, 0x0000bb80: 48000}


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: score_k2.py <ring> <sweettime> [image]")
    ring, want = sys.argv[1], sys.argv[2]
    img = sys.argv[3] if len(sys.argv) > 3 else None
    c, lines, sh = K.count, K.lines, K.sh

    print("=== K2-0 identity ===")
    if c(ring, f"sweettime={want}") == 0:
        sys.exit(f"  FAIL: no sweettime={want} in {ring} - wrong ring")
    print(f"  sweettime={want} hits={c(ring, want)}")
    if img:
        for pat in ("mfcget v4 ", "mfcget v3 ", "mfc v1 sel="):
            n = int(sh(f"LC_ALL=C grep -ao -F '{pat}' {img} | wc -l").strip() or 0)
            print(f"  image '{pat}' x {n}")
    label = __import__("re").sub(r"^.*?ring-", "", ring).replace(".txt", "")
    print(sh(f"python3 tools/ring_window.py {label} 2>&1 | tail -6"))

    got, problems = K.parse(ring, K.ORDER_K2)
    print("=== K2-1 the write itself ===")
    for l in lines(ring, "mfc v1 sel="):
        print("  " + l.strip()[:170])
    if not lines(ring, "mfc v1 sel="):
        print("  !! no 'mfc v1 sel=' line at all - the write never went out "
              "(cmdline or selector), so nothing below is a module reading")

    print("=== K2-2 every read line, in issue order (9 GETs expected) ===")
    for p in problems:
        print(f"  !! {p}")
    n = 0
    for phase in ("pre-set", "post-set", "post-prepare", "post-start", "post-tone"):
        for iid, f in sorted(got.get(phase, {}).items()):
            n += 1
            print(f"  {phase:13s} {f['line'][:185]}")
    print(f"  total GET lines: {n} (9 expected with sweetmfc=1 sweetmfcget=3)")

    def st_of(phase, iid, fn):
        return fn(got.get(phase, {}).get(iid))

    print("=== K2-3 the primary gate: what the MFC says right after our write ===")
    post = got.get("post-set", {}).get(0x48ba)
    verdict = None
    if post is None:
        verdict = ("MISSING", "no post-set line - sweetmfcget gate or the write path "
                   "did not run, this is an instrument reading")
    elif K.refused(post) and post.get("size") == 72:
        verdict = ("NOT STORED", "the MFC accepted the SET (check K2-1 rc) but answers "
                   "this param with st/err!=0 and wrote nothing: 0x08001024 is either "
                   "not gettable on 0x48ba or not storable - a real negative, and the "
                   "first one on the converter")
    elif post.get("rate") in WRITTEN and post.get("size") in (8, 12):
        verdict = ("STORED", f"post-set reads back rate={post['rate']} "
                   f"bw={post.get('bw')} nch={post.get('nch')} - our 12 bytes are in "
                   "the module, same shape as the TDM precedent")
    elif post.get("rate") == -1:
        verdict = ("UNCHANGED", "answered, written back (size moved) but still the "
                  "init default - the set did not become the format")
    else:
        verdict = ("NEW SHAPE", f"{post['line'][:120]}")
    print(f"  {verdict[0]}: {verdict[1]}")

    print("=== K2-4 did anything downstream drop it again? ===")
    for phase in ("post-prepare", "post-start", "post-tone"):
        print(f"  {phase:13s} MFC: {st_of(phase, 0x48ba, K.mfc_state)}")
    for phase in ("post-start", "post-tone"):
        print(f"  {phase:13s} WR : {st_of(phase, 0x40bd, K.wr_state)}   "
              f"TDM: {st_of(phase, 0x428f, K.tdm_state)}")
    print("  (the WR slot is expected to stay E2/refused - K1 proved that module does "
          "not answer this GET, so it is not evidence either way; the TDM slot is the "
          "instrument's own control and must still read 0x17700/0x30020/0x1)")

    print("=== K2-5 instrument-failure shapes (never module verdicts) ===")
    print(f"  'no response' lines: {c(ring, 'mfcget v4 ')} GETs printed, of which "
          f"no-response = {sum(1 for d in got.values() for f in d.values() if f['kind'] == 'no')}")
    print(f"  'unexpected rsp' lines: {c(ring, 'unexpected rsp=')}")
    print(f"  'basic' lines: {sum(1 for d in got.values() for f in d.values() if f['kind'] == 'basic')}")

    print("=== K2-6 regression, item by item against K1's own ring ===")
    for pat in ("graph_open/vp-wr rsp op=0x1001000 status=0",
                "graph_prepare rsp op=0x1001001 status=0",
                "graph_start rsp op=0x1001002 status=0",
                "tone gain g=6 peak=6144", "pcm tone done (120 buffers)",
                "cba2r@", "ats1pr iova=0x1ffe8000 -> par=0 OK",
                "geni amp 0x34 r=0 id=0x2032", "shm mapped", "not sent"):
        print(f"  {('OK  ' if c(ring, pat) >= 1 else 'MISS')} ({c(ring, pat)}) {pat}")
    print(f"  data_buffer send: {c(ring, 'data_buffer send')} (K1 121)  "
          f"no rsp: {c(ring, 'no rsp')}  panic: {c(ring, 'panic')}")
    print(f"  done/PSCI in window: {c(ring, '===== done =====')} / "
          f"{c(ring, 'PSCI SYSTEM_RESET')}")
    print("  ear gate: sweetvol=6 = -14.5 dBFS for 120 buffers - the reading is the "
          "user's, and K2's table makes it secondary to K2-3.")


if __name__ == "__main__":
    main()
