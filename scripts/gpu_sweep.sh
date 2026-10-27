#!/bin/bash
# GPU register matrix sweep: one boot per combination, unattended.
#
#   ./scripts/gpu_sweep.sh                 # all variants, read-only first
#   ./scripts/gpu_sweep.sh m3-cbar0r m5-cbar0w0
#   DRY_RUN=1 ./scripts/gpu_sweep.sh       # build the images only
#
# The shim does the MMIO with synchronous-abort recovery and leaves a result
# page in low DRAM; the v85 XNU prints that page.  A variant that never reaches
# XNU is marked NO-XNU -- that is the signal for a fatal access (SError/hang),
# because a plain data abort is recovered in the shim and still boots.
#
# Every boot is validated against its build-time sweettime: the SWEETLOG ring
# in userdata is not cleared by a failed boot, so an unchecked pull would show
# the previous run's log and look like success.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
OUTDIR="$ROOT/images/gpu-sweep"
LOGDIR="$ROOT/logs"
mkdir -p "$OUTDIR" "$LOGDIR"
SWEEP_LOG="$LOGDIR/gpu-sweep-$(date +%Y%m%d-%H%M%S).log"
BASE_ARGS="sweetgpu=1"

# label | build env | note
RUNS=(
  "m0-base|GPU_PROBE_LEVEL=1|SMMU ID/SMR/S2CR baseline"
  "m1-kgsl|GPU_PROBE_LEVEL=1 GP_KGSL_SCAN=1|KGSL 0x0/0x840/0x1000/0x2000"
  "m2-gr1|GPU_PROBE_LEVEL=1 GP_GR1_READ=1|CBA2R + CBFRSYNRA"
  "m3-cbar0r|GPU_PROBE_LEVEL=1 GP_CBAR_BANK=0|CBAR(0) read"
  "m4-cbar03r|GPU_PROBE_LEVEL=1 GP_CBAR_COUNT=4|CBAR(0..3) read"
  "m5-cbar0w0|GPU_PROBE_LEVEL=1 GP_CBAR_BANK=0 GP_CBAR_WRITE=1|CBAR(0)=0"
  "m6-cbar0w1|GPU_PROBE_LEVEL=1 GP_CBAR_BANK=0 GP_CBAR_WRITE=2|CBAR(0)=0x10000"
  "m7-sctlr0r|GPU_PROBE_LEVEL=1 GP_SCTLR_BANK=0|CB0 SCTLR read"
  "m8-sctlr0w|GPU_PROBE_LEVEL=1 GP_SCTLR_BANK=0 GP_SCTLR_WRITE=1|CB0 SCTLR=0"
  "m9-cb0regs|GPU_PROBE_LEVEL=1 GP_CB_REGS=1|CB0 TTBR0/TCR/MAIR"
  "mA-readall|GPU_PROBE_LEVEL=1 GP_KGSL_SCAN=1 GP_GR1_READ=1 GP_CBAR_COUNT=4 GP_SCTLR_BANK=0 GP_CB_REGS=1|all reads"
)

say() { printf '%s\n' "$*" | tee -a "$SWEEP_LOG"; }

in_fastboot() {
  fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL"
}
have_adb() {
  local st
  st=$(adb -s "$SERIAL" get-state 2>/dev/null || true)
  [ "$st" = "device" ] || [ "$st" = "recovery" ]
}

ensure_fastboot() {
  in_fastboot && return 0
  adb -s "$SERIAL" reboot bootloader >/dev/null 2>&1 || true
  for _ in $(seq 1 120); do
    in_fastboot && return 0
    have_adb && adb -s "$SERIAL" shell reboot bootloader >/dev/null 2>&1 || true
    sleep 2
  done
  return 1
}

build_one() {
  local label="$1" envs="$2" log="$OUTDIR/$label.build.log"
  # shellcheck disable=SC2086
  env $envs ./scripts/mkimage.sh $BASE_ARGS > "$log" 2>&1 || return 1
  grep -q "sweetgpu=1" "$log" || return 1
  sed -n 's/.*sweettime=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1 \
    > "$OUTDIR/$label.sweettime"
  cp -f images/shim-iosroot-adsp.img "$OUTDIR/$label.img" || return 1
  return 0
}

run_one() {
  local label="$1" want got st
  local img="$OUTDIR/$label.img"
  [ -f "$img" ] || { say "RESULT $label BUILD-MISSING"; return 1; }
  want=$(cat "$OUTDIR/$label.sweettime" 2>/dev/null || echo 0)

  ensure_fastboot || { say "RESULT $label NO-FASTBOOT"; return 1; }
  say "BOOT $label (sweettime=$want)"
  fastboot -s "$SERIAL" boot "$img" >> "$SWEEP_LOG" 2>&1 || {
    say "RESULT $label BOOT-FAILED"; return 1;
  }

  local got_adb=0
  for _ in $(seq 1 96); do
    if have_adb; then got_adb=1; sleep 5; break; fi
    sleep 5
  done
  if [ "$got_adb" = 0 ]; then
    say "RESULT $label NO-ADB (fatal or hung)"
    # A panic screen resets after 40 s; give it one more minute.
    for _ in $(seq 1 12); do
      have_adb && { got_adb=1; break; }
      in_fastboot && break
      sleep 5
    done
    [ "$got_adb" = 0 ] && { say "RESULT $label UNRECOVERED"; return 1; }
  fi

  SERIAL="$SERIAL" ./scripts/pull_logs.sh "gpu-$label" >/dev/null 2>&1 || {
    say "RESULT $label LOG-PULL-FAILED"; return 1;
  }
  got=$(sed -n 's/.*SWEET RTC: clock set from sweettime=\([0-9][0-9]*\).*/\1/p' \
    "$LOGDIR/ring-gpu-$label.txt" | tail -1)
  if [ "$want" != "$got" ]; then
    say "RESULT $label NO-XNU (image=$want log=$got)"
    grep -a "SWEET GPU" "$LOGDIR/ring-gpu-$label.txt" | tail -4 | tee -a "$SWEEP_LOG"
    return 2
  fi
  say "RESULT $label OK"
  grep -a "SWEET GPU" "$LOGDIR/ring-gpu-$label.txt" | tee -a "$SWEEP_LOG"
  return 0
}

# ---------------------------------------------------------------- main
say "gpu sweep: $SWEEP_LOG"
say "kernel: $(grep -ao 'SWEET AUDIO: build v[0-9]*[^"]*' out/kernel.adsp | head -1)"

picked=("$@")
if [ ${#picked[@]} -eq 0 ]; then
  for spec in "${RUNS[@]}"; do picked+=("${spec%%|*}"); done
fi

fail=0
for label in "${picked[@]}"; do
  spec=""
  for s in "${RUNS[@]}"; do [ "${s%%|*}" = "$label" ] && { spec="$s"; break; }; done
  if [ -z "$spec" ]; then say "RESULT $label UNKNOWN"; fail=1; continue; fi
  rest="${spec#*|}"; envs="${rest%%|*}"; note="${rest#*|}"
  say "=== $label  [$note]  ($envs)"
  if ! build_one "$label" "$envs"; then
    say "RESULT $label BUILD-FAILED (see $OUTDIR/$label.build.log)"
    fail=1; continue
  fi
  if [ "${DRY_RUN:-0}" = 1 ]; then say "RESULT $label BUILT"; continue; fi
  run_one "$label" || fail=1
done

say "=== summary ==="
grep -a "^RESULT " "$SWEEP_LOG" | tail -50 | tee -a "$SWEEP_LOG"
exit $fail
