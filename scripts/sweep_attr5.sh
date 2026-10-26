#!/bin/bash
# v61 leaf-attribute sweep: 5 boots, 5 log pulls, unattended.
#
#   ./scripts/sweep_attr5.sh            # build 5 images, boot each, pull its log
#   DRY_RUN=1 ./scripts/sweep_attr5.sh  # only build the 5 images (no device I/O)
#
# RAM-only.  Nothing is flashed: every run is a single `fastboot boot`.
#
# All five runs share the configuration that already produced a *successful*
# translation in run121/run124 (32-bit IOVA 0x1FFE8000 -> tone page), and
# differ in exactly one knob, so a "no longer hangs" result is attributable.
#
#   BASE = sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1
#
# `sweetpcm=1` is REQUIRED: the entire data-plane probe (DATA_BUFFER,
# sweetnolpass) is behind that flag.  Without it every run stops at
# "graph started" and resets cleanly, which looks like success and answers
# nothing -- runs a1..a5 wasted five boots that way.
#
# `sweetnolpass=1` gives us the ~200 ms heartbeat after DATA_BUFFER without
# touching LPASS; that heartbeat is the pass/fail signal.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
# sweetpcm=1 is mandatory: the whole data-plane probe (incl. sweetnolpass)
# is behind it, and without it the run stops at "graph started" and resets
# cleanly without ever sending DATA_BUFFER.
BASE="sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1"
MARKER="build v61"
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
  "b1-attr1|sweets1attr=1"
  "b2-attr2|sweets1attr=2"
  "b3-attr3|sweets1attr=3"
  "b4-attr4|sweets1attr=4"
  "b5-attr5|sweets1attr=5"
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

# Wait until adb answers again after the kernel resets.
# If the device lands in the bootloader instead, reboot once to get adb.
wait_for_adb() {
  local t=0 saw_fastboot=0
  while [ "$t" -lt 420 ]; do
    if have_adb; then
      sleep 8                      # let adbd settle before dd
      return 0
    fi
    if in_fastboot; then
      saw_fastboot=1
    elif [ "$saw_fastboot" = 1 ] && [ "$t" -ge 60 ]; then
      echo "  device rebooted into the bootloader; rebooting to get adb"
      fastboot -s "$SERIAL" reboot >/dev/null 2>&1 || true
      saw_fastboot=0
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
    echo "  no adb after 6 min.  Either the SoC hung (that IS a result:"
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

say "done: 5 runs"
ls -lh logs/ring-a*.txt 2>/dev/null
echo
echo "send me these five files:"
for spec in "${RUNS[@]}"; do
  echo "  logs/ring-${spec%%|*}.txt"
done
