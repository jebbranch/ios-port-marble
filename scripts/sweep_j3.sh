#!/bin/bash
# v70 sweep: 3 boots, 3 log pulls.  LISTEN to the speaker.
#
#   ./scripts/sweep_j3.sh            # build 3 images, boot each, pull its log
#   DRY_RUN=1 ./scripts/sweep_j3.sh  # only build the images
#
# RAM-only, same as the other sweeps.
#
# h round: dropping the WR SET_CFG (sweetwrmf=0) stopped the SoC from dying -
# a DATA_BUFFER to the real WR now completes.  But the DSP replies nothing, so
# the tone loop never ran.  sweetnowait=1 fires buffers without waiting.
#
# v70 also makes the audio real: the shim embeds a 48 kHz/24-bit/stereo PCM
# clip (PCM_SRC=<file> at image-build time, else a 1 kHz sine) and the kernel
# streams it across the whole shared-memory region.
#
#   j1  fire-and-forget, real clip
#   j2  graph and data both on 0x2011 (vendor's device-graph port)
#   j3  data pushed from 0x2010 (the port that opened our graph)
#
# The pass criterion is AUDIBLE SOUND.  Logs only tell us it ran.
#
# BASE is identical to the b-sweep except nothing extra: sweetpcm=1 is
# mandatory or nothing below "graph started" ever runs.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
BASE="sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1"
MARKER="build v70"
OUTDIR="$ROOT/images/sweep"

if [ -z "${D22AP_ADT:-}" ] && [ ! -f "$ROOT/research/ipsw-x/d22ap.adt" ]; then
  for cand in "$ROOT/../xnu/research/ipsw-x/d22ap.adt" \
              "$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt"; do
    [ -f "$cand" ] && { export D22AP_ADT="$cand"; break; }
  done
fi
[ -n "${D22AP_ADT:-}" ] && echo "base ADT: $D22AP_ADT"

# label | extra boot args | what it answers
RUNS=(
  "j1-pcm|sweets1attr=1 sweetwrmf=0 sweetnowait=1|real PCM clip, WR on 0x2011"
  "j2-port-2011|sweets1attr=1 sweetwrmf=0 sweetnowait=1 sweetgraphport=0x2011|graph+data on 0x2011"
  "j3-data-2010|sweets1attr=1 sweetwrmf=0 sweetnowait=1 sweetdbsrc=0x2010|data from the graph's own port"
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
  echo "local out/kernel.adsp is not $MARKER; fetching kernel-latest"
  if ! ./scripts/fetch_kernel_api.sh; then
    MARBLE_PROXY="${MARBLE_PROXY:-}" ./scripts/fetch_kernel.sh || exit 1
  fi
fi
if ! grep -aq "$MARKER" out/kernel.adsp; then
  echo "!! out/kernel.adsp still lacks '$MARKER'." >&2
  echo "   The CI build is probably not published yet - wait and rerun." >&2
  exit 1
fi
echo "ok: $(grep -ao 'SWEET AUDIO: build v[0-9]*[^"]*' out/kernel.adsp | head -1)"

# ---------------------------------------------------------------- 2. images
mkdir -p "$OUTDIR"
n=0
for spec in "${RUNS[@]}"; do
  n=$((n + 1))
  label="${spec%%|*}"
  rest="${spec#*|}"
  extra="${rest%%|*}"
  say "build $n/${#RUNS[@]}: $label   [$extra]"
  # shellcheck disable=SC2086
  if ! ./scripts/mkimage.sh $BASE $extra > "$OUTDIR/$label.build.log" 2>&1; then
    echo "!! mkimage failed for $label (see $OUTDIR/$label.build.log)" >&2
    tail -20 "$OUTDIR/$label.build.log" >&2
    exit 1
  fi
  cp images/shim-iosroot-adsp.img "$OUTDIR/$label.img" || exit 1
  if ! grep -q "^cmdline:.*$extra" "$OUTDIR/$label.build.log"; then
    echo "!! $label.img cmdline does not contain '$extra'" >&2
    exit 1
  fi
  echo "ok: $OUTDIR/$label.img"
done

if [ "${DRY_RUN:-0}" = 1 ]; then
  say "DRY_RUN=1: images built, not booting"
  ls -lh "$OUTDIR" | grep -E "f[0-9]-"
  exit 0
fi

# ------------------------------------------------------- 3. boot + pull
n=0
for spec in "${RUNS[@]}"; do
  n=$((n + 1))
  label="${spec%%|*}"
  rest="${spec#*|}"
  extra="${rest%%|*}"
  question="${rest#*|}"
  say "run $n/${#RUNS[@]}: $label"
  echo "  question: $question"
  ensure_fastboot || exit 1
  echo "  fastboot boot $OUTDIR/$label.img"
  fastboot -s "$SERIAL" boot "$OUTDIR/$label.img" || { echo "!! fastboot boot failed" >&2; exit 1; }
  echo "  kernel is running; waiting for it to reset (usually 2-5 min)"
  if ! wait_for_adb; then
    echo
    echo "  ------------------------------------------------------------"
    echo "  no adb after 8 min.  Either the SoC hung (that IS a result)"
    echo "  or the phone is just slow.  Hold Power ~10 s to force a"
    echo "  reboot, let it land in TWRP, then press Enter."
    echo "  ------------------------------------------------------------"
    read -r _ || true
    for _i in $(seq 1 150); do have_adb && break; sleep 2; done
    have_adb || { echo "!! still no adb; pull by hand: ./scripts/pull_logs.sh $label" >&2; exit 1; }
  fi
  ./scripts/pull_logs.sh "$label" || { echo "!! pull_logs failed for $label" >&2; exit 1; }
  LOG="logs/ring-$label.txt"
  echo "  -> $LOG"
  # verdict: completed / probe line / hung, in that order of certainty
  if grep -q "===== done =====" "$LOG" 2>/dev/null; then
    echo "     VERDICT: COMPLETED (reached the end of the run)"
  else
    probe=$(grep -m1 "post-send cb" "$LOG" 2>/dev/null)
    if [ -n "$probe" ]; then
      echo "     VERDICT: hung, but got the probe -> $probe"
    elif grep -q "data_buffer sent" "$LOG" 2>/dev/null; then
      echo "     VERDICT: hung before the probe (died on the first SMMU read)"
    else
      echo "     VERDICT: never reached DATA_BUFFER - check the log"
    fi
  fi
done

say "done: ${#RUNS[@]} runs"
for spec in "${RUNS[@]}"; do
  echo "  logs/ring-${spec%%|*}.txt"
done
