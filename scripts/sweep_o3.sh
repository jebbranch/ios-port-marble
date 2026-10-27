#!/bin/bash
# v78: does the DSP finally consume DATA_BUFFER_V2?
#
#   ./scripts/sweep_o3.sh              # 4 boots, 4 logs, unattended
#   DRY_RUN=1 ./scripts/sweep_o3.sh    # only build the images
#
# Everything below is downstream of one fact: the vendor GPR capture
# (tools/gpr_capture.sh) says the WR endpoint 0x40bd is 48 kHz/16-bit and
# that DATA_BUFFER_V2 comes from the same client port that opened the graph
# (0x2010).  v78 fixes both.  This sweep varies only the graph definition:
#
#   o1  hand-built merged graph            (v77 behaviour, one subgraph)
#   o2  vendor 4-subgraph graph, off 12    (the 2724->2712 header guess)
#   o3  vendor 4-subgraph graph, off 0     (if the header is not a header)
#   o4  vendor graph + paced feed          (the DSP may need real-time)
#
# The pass signal is a DATA_BUFFER_DONE reply, i.e. the run reaches
# "===== done =====" instead of hanging after "data_buffer send".
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
# sweetpcm=1 is mandatory: the whole data-plane probe (incl. sweetnolpass)
# is behind it, and without it the run stops at "graph started" and resets
# cleanly without ever sending DATA_BUFFER.
BASE="sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1"
MARKER="build v78"
OUTDIR="$ROOT/images/sweep"

# mkimage.sh needs the Apple base ADT; it lives in the other work tree here.
if [ -z "${D22AP_ADT:-}" ] && [ ! -f "$ROOT/research/ipsw-x/d22ap.adt" ]; then
  for cand in "$ROOT/../xnu/research/ipsw-x/d22ap.adt" \
              "$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt"; do
    [ -f "$cand" ] && { export D22AP_ADT="$cand"; break; }
  done
fi
[ -n "${D22AP_ADT:-}" ] && echo "base ADT: $D22AP_ADT"

# label | extra boot args
RUNS=(
  "o1-merged|"
  "o2-vgraph12|sweetvgraph=1"
  "o3-vgraph0|sweetvgraph=1 sweetvgoff=0"
  "o4-vgraph-pace|sweetvgraph=1 sweetbufs=40 sweetbufgap=4"
)

say() { printf '\n=== %s ===\n' "$*"; }

in_fastboot() {
  fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL"
}

have_adb() {
  adb -s "$SERIAL" get-state >/dev/null 2>&1
}

ensure_fastboot() {
  in_fastboot && return 0
  adb -s "$SERIAL" reboot bootloader >/dev/null 2>&1 || true
  for _ in $(seq 1 120); do
    in_fastboot && return 0
    if adb -s "$SERIAL" get-state 2>/dev/null | grep -q recovery; then
      adb -s "$SERIAL" shell reboot bootloader >/dev/null 2>&1 || true
    fi
    sleep 1
  done
  echo "!! device $SERIAL is not in fastboot (hold Power+VolDown and retry)" >&2
  return 1
}

# After the kernel resets itself the phone boots Android or TWRP (both have
# adbd) - it does NOT come back to the bootloader, so waiting for fastboot is
# pointless.  Only if the bootloader really shows up do we nudge it along;
# otherwise just wait for adb, and let the caller handle a real hang.
wait_for_adb() {
  local t=0
  while [ "$t" -lt 480 ]; do
    if have_adb; then
      sleep 8                      # let adbd settle before dd
      return 0
    fi
    if in_fastboot; then
      echo "  device really is in the bootloader; rebooting to get adb"
      fastboot -s "$SERIAL" reboot >/dev/null 2>&1 || true
      sleep 30
      t=$((t + 30))
      continue
    fi
    sleep 5
    t=$((t + 5))
  done
  return 1
}

# ---------------------------------------------------------------- 1. kernel
say "kernel: expecting '$MARKER'"
if ! grep -aq "$MARKER" out/kernel.adsp 2>/dev/null; then
  echo "local out/kernel.adsp is not v61; fetching kernel-latest"
  if ! ./scripts/fetch_kernel_api.sh; then
    echo "fetch_kernel_api.sh failed; trying fetch_kernel.sh" >&2
    MARBLE_PROXY="${MARBLE_PROXY:-}" ./scripts/fetch_kernel.sh || exit 1
  fi
fi
if ! grep -aq "$MARKER" out/kernel.adsp; then
  echo "!! out/kernel.adsp still lacks '$MARKER'." >&2
  echo "   The CI build for v61 is probably not published yet - wait and rerun." >&2
  exit 1
fi
echo "ok: $(grep -ao 'SWEET AUDIO: build v[0-9]*[^"]*' out/kernel.adsp | head -1)"

# ---------------------------------------------------------------- 2. images
mkdir -p "$OUTDIR"
n=0
for spec in "${RUNS[@]}"; do
  n=$((n + 1))
  label="${spec%%|*}"
  extra="${spec#*|}"
  say "build $n/${#RUNS[@]}: $label   [$extra]"
  # shellcheck disable=SC2086
  if ! ./scripts/mkimage.sh $BASE $extra > "$OUTDIR/$label.build.log" 2>&1; then
    echo "!! mkimage failed for $label (see $OUTDIR/$label.build.log)" >&2
    tail -20 "$OUTDIR/$label.build.log" >&2
    exit 1
  fi
  cp images/shim-iosroot-adsp.img "$OUTDIR/$label.img" || exit 1
  # mkimage prints the full cmdline; prove our knobs are in it
  if ! grep -q "^cmdline:.*$extra" "$OUTDIR/$label.build.log"; then
    echo "!! $label.img cmdline does not contain '$extra'" >&2
    grep '^cmdline:' "$OUTDIR/$label.build.log" | tr ' ' '\n' | tail -12 >&2
    exit 1
  fi
  echo "ok: $OUTDIR/$label.img"
done

if [ "${DRY_RUN:-0}" = 1 ]; then
  say "DRY_RUN=1: images built, not booting"
  ls -lh "$OUTDIR"
  exit 0
fi

# ------------------------------------------------------- 3. boot + pull, x5
n=0
for spec in "${RUNS[@]}"; do
  n=$((n + 1))
  label="${spec%%|*}"
  say "run $n/${#RUNS[@]}: $label"
  ensure_fastboot || exit 1
  echo "  fastboot boot $OUTDIR/$label.img"
  fastboot -s "$SERIAL" boot "$OUTDIR/$label.img" || { echo "!! fastboot boot failed" >&2; exit 1; }
  echo "  kernel is running; waiting for it to reset (usually 2-5 min)"
  if ! wait_for_adb; then
    echo
    echo "  ------------------------------------------------------------"
    echo "  no adb after 8 min.  Either the SoC hung (that IS a result:"
    echo "  this attribute still kills the machine) or the phone is"
    echo "  just slow.  Hold Power ~10 s to force a reboot, let it land"
    echo "  in TWRP, then press Enter to continue the sweep."
    echo "  ------------------------------------------------------------"
    read -r _ || true
    for _i in $(seq 1 150); do have_adb && break; sleep 2; done
    have_adb || { echo "!! still no adb; pull the log by hand: ./scripts/pull_logs.sh $label" >&2; exit 1; }
  fi
  ./scripts/pull_logs.sh "$label" || { echo "!! pull_logs failed for $label" >&2; exit 1; }
  echo "  -> logs/ring-$label.txt"
  # a quick in-log verdict so the operator sees progress without reading them.
  # NOTE: an "nolpass alive" line alone does NOT mean survival -- the heartbeat
  # prints every ~100 ms and the old runs all died 100-200 ms in.  Only
  # reaching the end of the run counts.
  if grep -q "===== done =====" "logs/ring-$label.txt" 2>/dev/null; then
    echo "     VERDICT: COMPLETED (reached the end of the run)"
  elif grep -q "data_buffer sent" "logs/ring-$label.txt" 2>/dev/null; then
    echo "     VERDICT: hung after DATA_BUFFER ($(grep -c 'nolpass alive' "logs/ring-$label.txt" 2>/dev/null) heartbeat(s))"
  else
    echo "     VERDICT: never reached DATA_BUFFER - check the log"
  fi
done

say "done: ${#RUNS[@]} runs"
ls -lh logs/ring-o*.txt 2>/dev/null
echo
echo "send me these five files:"
for spec in "${RUNS[@]}"; do
  echo "  logs/ring-${spec%%|*}.txt"
done
