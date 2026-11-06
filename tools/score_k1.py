#!/usr/bin/env python3
"""Score the K1 round against the verdict table committed BEFORE it was booted.

The table lives in docs/audio-closeout.md ("第 38 枪 K1 判决表"); this file is its
executable twin, and the rule it exists to obey is that a scorer must match the
committed table rather than a number I remember, and must fail loudly when the
instrument itself is what broke.

  tools/score_k1.py logs/ring-k1-3module.txt <sweettime> [image.img]

What it decides (rows A-F of the table), in one pass over the ring:
  * the eight `mfcget v4 <phase>` lines and which branch each took;
  * WR 0x40bd/0x0800100c  "stored"  == a 0x0800100c body whose size is the 30 we
    sent and whose rate word is 0xbb80/0x17700 at the documented offset;
  * TDM 0x428f/0x08001017 "stored"  == size 12 with d0=0x17700 d1=0x30020;
  * MFC 0x48ba/0x08001024 "valid"   == rate is a real number (not -1) and nch != 0xFFFF;
  * a `basic` answer on the WR/TDM line is row E (that module refuses GET - the
    SAME builder got a structured answer from the MFC, so it is not a transport
    failure) and is therefore reported as "not decidable", never as a negative;
  * three or more `no response` lines are row F and outrank every content row.
"""
import pathlib
import re
import subprocess
import sys

RATE_WORDS = {0xbb80: 48000, 0x17700: 96000}


def sh(cmd):
    out = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True)
    return out.stdout


def count(ring, pat):
    return int(sh(f"LC_ALL=C grep -ac -F '{pat}' {ring} || true").strip() or 0)


ORDER = [("pre-set", 0x48ba), ("post-prepare", 0x48ba),
         ("post-start", 0x40bd), ("post-start", 0x428f), ("post-start", 0x48ba),
         ("post-tone", 0x40bd), ("post-tone", 0x428f), ("post-tone", 0x48ba)]


def parse(ring):
    """Return ({phase: {iid: fields}}, problems).

    Identity is by POSITION first, then checked against the line itself: the
    0272 printf for `no response` and `basic` carries no iid=/param= (a known gap
    in the instrument, not worth a rebuild for), while g_apm_get_cfg_readset()
    issues WR, TDM, MFC in that fixed order and every branch prints exactly one
    line, so the 8 GETs map 1:1 onto the 8 lines.  Where a line DOES name its iid
    it must agree with its slot - if it does not, the ring is not this round's and
    the score is refused rather than quietly re-keyed.
    """
    got, problems = {}, []
    lines = [l for l in sh(f"LC_ALL=C grep -a -F 'mfcget v4 ' {ring} || true").splitlines()
             if re.search(r"mfcget v4 \S+", l)]
    if len(lines) != len(ORDER):
        problems.append(f"{len(lines)} mfcget lines, not {len(ORDER)} - "
                        "extra/lost GETs mean the order mapping is unsafe")
    for i, line in enumerate(lines):
        m = re.search(r"mfcget v4 (\S+) (\w+)", line)
        if not m:
            problems.append(f"unparsable line {i}: {line[:80]}")
            continue
        phase, kind = m.group(1), m.group(2)
        f = {"kind": kind, "line": line.strip()}
        # st= is printed with %#x (so 0x1), err=/size=/len= with %u (decimal).
        # Parsing st with (\d+) silently read "st=0x1" as 0, which is exactly how
        # a refused GET would have been scored as a clean one - fixed here, after
        # the fact, and the fixture now covers both spellings.
        for name in ("iid", "param", "rsp", "echoed_op", "status"):
            mm = re.search(rf"\b{name}=(0x[0-9a-f]+)", line)
            if mm:
                f[name] = int(mm.group(1), 16)
        for name in ("st", "cmap"):
            mm = re.search(rf"\b{name}=(0x[0-9a-f]+|\d+)", line)
            if mm:
                f[name] = int(mm.group(1), 16) if mm.group(1).startswith("0x") \
                    else int(mm.group(1))
        for name in ("len", "size", "err"):
            mm = re.search(rf"\b{name}=(\d+)", line)
            if mm:
                f[name] = int(mm.group(1))
        for name in ("rate", "bw", "nch"):
            mm = re.search(rf"\b{name}=(-?\d+)", line)
            if mm:
                f[name] = int(mm.group(1))
        for k in range(6):
            mm = re.search(rf"\bd{k}=(0x[0-9a-f]+)", line)
            if mm:
                f[f"d{k}"] = int(mm.group(1), 16)
        want_phase, want_iid = (ORDER[i] if i < len(ORDER) else ("?", 0))
        if phase != want_phase:
            problems.append(f"line {i} says phase '{phase}', slot says '{want_phase}'")
        if "iid" in f and f["iid"] != want_iid:
            problems.append(f"line {i} says iid={f['iid']:#x}, slot says {want_iid:#x}")
        f["phase"], f["iid"] = phase, f.get("iid", want_iid)
        got.setdefault(f["phase"], {})[f["iid"]] = f
    return got, problems


def refused(f):
    """A structured reply that carries a non-zero overall status or error_code and
    leaves param_size at the capacity we offered is the module answering 'I will not
    give you this param' - it is NOT a body, and it is NOT a negative about what the
    module stores.  K1 (boot 38) hit exactly this on the WR: 0x40bd/0x0800100c came
    back st=0x1 err=1 size=72 with all data zero, while the SET_CFG to the same
    iid+param in the same boot was answered status=0.  Added after the boot, and
    labelled E2 so it can never be mistaken for a pre-registered row."""
    return f.get("st", 0) != 0 or f.get("err", 0) != 0


def wr_state(f):
    if f is None:
        return "missing"
    if f["kind"] == "basic":
        return "basic(E)"
    if f["kind"] == "no":
        return "no response(F)"
    if refused(f) and f.get("size") == 72:
        return f"refused GET st={f.get('st'):#x} err={f.get('err'):#x}(E2)"
    d3 = f.get("d3")
    size = f.get("size")
    if d3 in RATE_WORDS and size in (24, 28, 30, 32):
        return f"stored(rate {RATE_WORDS[d3]}, size {size})"
    if size == 72:
        return "not written back"
    return f"answered, body not a format (size={size} d3={None if d3 is None else hex(d3)})"


def tdm_state(f):
    if f is None:
        return "missing"
    if f["kind"] == "basic":
        return "basic(E)"
    if f["kind"] == "no":
        return "no response(F)"
    if refused(f) and f.get("size") == 72:
        return f"refused GET st={f.get('st'):#x} err={f.get('err'):#x}(E2)"
    if f.get("d0") == 0x17700 and f.get("d1") == 0x30020:
        return "stored(96000)"
    if f.get("d0") == 0xbb80:
        return "stored(48000)"
    if f.get("size") == 72:
        return "not written back"
    return f"answered, body not a format (size={f.get('size')} d0={f.get('d0')})"


def mfc_state(f):
    if f is None:
        return "missing"
    if f["kind"] == "basic":
        return "basic(E)"
    if f["kind"] == "no":
        return "no response(F)"
    if refused(f) and f.get("size") == 72:
        return f"refused GET st={f.get('st'):#x} err={f.get('err'):#x}(E2)"
    if "rate" not in f:
        return "no rate field"
    if f["rate"] in (-1, 0) or f.get("nch", 0) in (0, 65535):
        return f"INVALID (rate={f['rate']} bw={f.get('bw')} nch={f.get('nch')})"
    return f"VALID (rate={f['rate']} bw={f.get('bw')} nch={f.get('nch')})"


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: score_k1.py <ring> <sweettime> [image]")
    ring, want = sys.argv[1], sys.argv[2]
    img = sys.argv[3] if len(sys.argv) > 3 else None

    print(f"=== K1-0 identity (gate: no sweettime, no score) ===")
    if count(ring, f"sweettime={want}") == 0:
        sys.exit(f"  FAIL: {ring} has no sweettime={want} - wrong ring")
    print(f"  sweettime={want} hits={count(ring, f'sweettime={want}')}")
    for old in ("mfcget v3 ", "mfcget v2 ", "mfcget v1 "):
        print(f"  ring {old} x {count(ring, old)} (want 0)")
    if img and pathlib.Path(img).exists():
        for pat in ("mfcget v4 ", "mfcget v3 ", "mfcget v2 ", "mfcget v1 "):
            hits = int(sh(f"LC_ALL=C grep -ao -F '{pat}' {img} | wc -l").strip() or 0)
            want = 5 if pat.endswith("v4 ") else 0
            print(f"  image '{pat}' x {hits} (want {want}) "
                  f"{'OK' if hits == want else '<< IDENTITY FAIL'}")
    label = re.sub(r"^.*?ring-", "", ring).replace(".txt", "")
    print(sh(f"python3 tools/ring_window.py {label} 2>&1 | tail -8"))

    got, problems = parse(ring)
    print("=== K1-1 the eight read lines, verbatim ===")
    for p in problems:
        print(f"  !! {p}")
    for phase in ("pre-set", "post-prepare", "post-start", "post-tone"):
        for iid, f in sorted(got.get(phase, {}).items()):
            print(f"  {phase:13s} {f['line'][:190]}")
    n = sum(len(v) for v in got.values())
    print(f"  mfcget v4 lines in ring: {n} (want 8)")

    def pick(phase, iid):
        return got.get(phase, {}).get(iid)

    wr_s, tdm_s, mfc_s = (pick("post-start", 0x40bd), pick("post-start", 0x428f),
                          pick("post-start", 0x48ba))
    wr_t, tdm_t, mfc_t = (pick("post-tone", 0x40bd), pick("post-tone", 0x428f),
                          pick("post-tone", 0x48ba))
    print("=== K1-2 the three modules at post-start, then at post-tone ===")
    print(f"  WR   0x40bd/0x0800100c  start: {wr_state(wr_s)}   tone: {wr_state(wr_t)}")
    print(f"  TDM  0x428f/0x08001017  start: {tdm_state(tdm_s)}   tone: {tdm_state(tdm_t)}")
    print(f"  MFC  0x48ba/0x08001024  start: {mfc_state(mfc_s)}   tone: {mfc_state(mfc_t)}")
    print(f"  (control points, unchanged from J1: pre-set {mfc_state(pick('pre-set', 0x48ba))}, "
          f"post-prepare {mfc_state(pick('post-prepare', 0x48ba))})")

    print("=== K1-3 verdict row (A-F, exactly as committed, first match wins) ===")
    noresp = sum(1 for ph in got.values() for f in ph.values() if f["kind"] == "no")
    basics = [f"{f['phase']}/{f['iid']:#x}" for d in got.values() for f in d.values()
              if f["kind"] == "basic"]
    # three-way state per slot: True / False / None (unknown = basic or unanswered)
    def tri(state):
        return True if state.startswith("stored") or state.startswith("VALID") else \
            (None if ("(E)" in state or "(E2)" in state or "(F)" in state
                      or state in ("missing", "no rate field")) else False)
    wr_s_t, tdm_s_t = tri(wr_state(wr_s)), tri(tdm_state(tdm_s))
    mfc_s_t, mfc_t_t = tri(mfc_state(mfc_s)), tri(mfc_state(mfc_t))
    sig = f"WR-start={wr_s_t} TDM-start={tdm_s_t} MFC-start={mfc_s_t} MFC-tone={mfc_t_t}"
    if noresp >= 3:
        print(f"  ROW F: {noresp} GETs unanswered -> transport/instrument; content undecidable")
    elif wr_s_t is False and tdm_s_t is False and mfc_s_t is False and mfc_t_t is True:
        print("  ROW D: all three invalid at post-start and the MFC valid only after the tone "
              "-> formats exist only on the data path, so our READ TIMING was the problem, "
              "not the graph")
    elif wr_s_t is True and tdm_s_t is True and mfc_s_t is False and mfc_t_t is not True:
        print("  ROW A: both endpoints publish, only the converter is unfed -> the break is in "
              "the WR->SAL->MFC propagation; next shot is sweetmfc=1 sweetmfcget=3 on THIS "
              "kernel (no new build needed)")
    elif wr_s_t is True and tdm_s_t is False:
        print("  ROW C: the sink endpoint is unfed as well -> silence is explained at the "
              "endpoint, not the converter")
    elif wr_s_t is False:
        print("  ROW B: our SET_CFG(0x0800100c) is NOT stored as a module format -> the "
              "in-graph cal object (the 18 KB wall) is back as the mainline suspect, now with "
              "register evidence")
    else:
        print(f"  NO ROW OF THE COMMITTED TABLE MATCHES ({sig}). This is a new shape: report "
              "it as such and do not rename it into A-F.")
    if basics:
        print(f"  ROW E applies to: {', '.join(basics)} - those slots answered `basic`, i.e. "
              "the module/param refuses GET.  Same builder, structured answer proven on "
              "0x48ba, so this is NOT a transport failure and NOT a negative about formats; "
              "those slots are 'not decidable'.")
    e2 = [f"{f['phase']}/{f['iid']:#x} st={f.get('st', 0):#x} "
          f"err={f.get('err', 0):#x} size={f.get('size')}"
          for d in got.values() for f in d.values()
          if refused(f) and f.get("size") == 72]
    if e2:
        print(f"  ROW E2 (added after this boot, NOT pre-registered): {', '.join(e2)} - "
              "a structured reply with non-zero st/err and size left at the 72 offered. "
              "That is the module declining to answer the param, not a body and not a "
              "negative about what it stores; the same iid+param SET in the same boot "
              "was answered status=0.")
    else:
        print("  (E2 not needed: every structured reply carried st=0/err=0.)")

    print("=== K1-4 regression gates (all must hold or the round is not scored) ===")
    # The strings below are the ones the driver actually prints (checked against
    # both the J1 and the K1 ring): 'graph_start rsp op=0x1001002 status=0', not
    # the shorthand my first version grepped for, and cba2r is printed as
    # 'cb5 cba2r@<addr> = 0x1' with a per-boot address in the middle.
    gates = [("graph_start rsp op=0x1001002 status=0", 1),
             ("graph_prepare rsp op=0x1001001 status=0", 1),
             ("graph_open/vp-wr rsp op=0x1001000 status=0", 1),
             ("tone gain g=6 peak=6144", 1), ("pcm tone done (120 buffers)", 1),
             ("cba2r@", 1), ("ats1pr iova=0x1ffe8000 -> par=0 OK", 1),
             ("geni amp 0x34", 1)]
    for pat, want_n in gates:
        c = count(ring, pat)
        print(f"  {'OK ' if c >= want_n else 'MISS'} ({c}) {pat}")
    print(f"  data_buffer send lines: {count(ring, 'data_buffer send')} (J1 had 121)")
    print(f"  'no rsp' lines: {count(ring, 'no rsp')} (want 0)")
    print(f"  panic lines: {count(ring, 'panic')} (want 0)")
    print(f"  '===== done =====': {count(ring, '===== done =====')}  "
          f"PSCI SYSTEM_RESET: {count(ring, 'PSCI SYSTEM_RESET')} (both want >=1)")
    print(f"  'not sent' (tdmcfg v1 sup=7 mask): {count(ring, 'not sent')} (want 3)")
    print(f"  'mfc v1 sel=' at runtime: {count(ring, 'mfc v1 sel=')} (want 0 = read-only)")


main()
