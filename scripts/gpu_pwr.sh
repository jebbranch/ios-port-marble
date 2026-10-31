#!/bin/bash
# One boot of the GPU v9 probe: read-only survey by default.
#
#   ./scripts/gpu_pwr.sh build          # images/gpu-pwr.img (+ .sweettime)
#   ./scripts/gpu_pwr.sh run            # boot it, listen on USB, pull the ring
#   ./scripts/gpu_pwr.sh build "GP_SURVEY=0 GP_PWR=1"
#                                       # the old collapse/power-on experiment
#
# GP_PWR is deliberately NOT a default: collapsing CX_GDSCR puts the domain in
# a state the vendor trace never enters, and it has never come back from it.
#
# Why the UDP listener: the kernel streams its console to 192.168.7.1:5140 over
# the USB ECM link (sweetusblog).  Listening **before** fastboot boot captures
# the SWEET GPU lines even if the phone never resets by itself.  The ring pull
# afterwards is the authoritative copy and proves the image by sweettime.
#
# The kernel must carry 0215 (v9 result fields + a PSCI reset for sweetglink=0
# runs); with it the phone reboots to Android on its own and the pull is
# unattended.  Without it, press power once after the probe.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
OUTDIR="$ROOT/images"
IMG="$OUTDIR/gpu-pwr.img"
LABEL="${2:-pwr}"
ADT="${D22AP_ADT:-$ROOT/../xnu/research/ipsw-x/d22ap.adt}"

build() {
	local extra="${1:-GP_SURVEY=1}"
	# shellcheck disable=SC2086
	env D22AP_ADT="$ADT" CPUS="${CPUS:-8}" GPU_PROBE_LEVEL=1 $extra \
		./scripts/mkimage.sh sweetgpu=1 sweetglink=0 ${EXTRA_ARGS:-} \
		> "$OUTDIR/gpu-pwr.build.log" 2>&1 || {
			tail -20 "$OUTDIR/gpu-pwr.build.log" >&2; return 1; }
	grep -q "sweetgpu=1" "$OUTDIR/gpu-pwr.build.log" || return 1
	cp -f images/shim-iosroot-adsp.img "$IMG" || return 1
	sed -n 's/.*sweettime=\([0-9][0-9]*\).*/\1/p' \
		"$OUTDIR/gpu-pwr.build.log" | tail -1 > "$OUTDIR/gpu-pwr.sweettime"
	echo "image: $IMG  sweettime=$(cat "$OUTDIR/gpu-pwr.sweettime")"
	grep -ao 'gpuprobe-[a-z0-9-]*' shim/out-xnu/Image | head -1
}

run() {
	local want got st
	want=$(cat "$OUTDIR/gpu-pwr.sweettime" 2>/dev/null || echo 0)
	[ -f "$IMG" ] || { echo "missing $IMG; build first" >&2; return 1; }

	if ! fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL"; then
		adb -s "$SERIAL" reboot bootloader >/dev/null 2>&1 || true
		for _ in $(seq 1 90); do
			fastboot devices 2>/dev/null | awk '{print $1}' \
				| grep -qx "$SERIAL" && break
			sleep 2
		done
	fi
	fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL" || {
		echo "!! $SERIAL is not in fastboot (phone hung? press power)" >&2
		return 1
	}

	local con="logs/console-gpu-$LABEL.log"
	: > "$con"
	python3 tools/console_udp.py "$con" --timeout 600 >/dev/null 2>&1 &
	local lpid=$!
	sleep 2

	echo "booting $IMG"
	fastboot -s "$SERIAL" boot "$IMG" || { kill $lpid; return 1; }

	echo "waiting for adb (Android after the probe resets itself)..."
	local got_adb=0
	for _ in $(seq 1 120); do
		st=$(adb -s "$SERIAL" get-state 2>/dev/null || true)
		[ "$st" = "device" ] || [ "$st" = "recovery" ] && { got_adb=1; break; }
		sleep 5
	done
	sleep 5
	kill $lpid 2>/dev/null
	echo "--- live console (SWEET GPU) ---"
	grep -a "SWEET GPU" "$con" | tail -12

	if [ "$got_adb" = 0 ]; then
		echo "!! no adb: the phone is still in XNU (or hung)."
		echo "   power-cycle once, then: SERIAL=$SERIAL ./scripts/pull_logs.sh gpu-$LABEL"
		return 2
	fi

	SERIAL="$SERIAL" ./scripts/pull_logs.sh "gpu-$LABEL" >/dev/null 2>&1 || {
		echo "!! log pull failed" >&2; return 1; }

	got=$(sed -n 's/.*boot-args = .*sweettime=\([0-9][0-9]*\).*/\1/p' \
		"logs/ring-gpu-$LABEL.txt" | tail -1)
	echo "sweettime: image=$want log=$got"
	[ "$want" = "$got" ] || { echo "!! STALE RING (kernel did not run)"; return 2; }

	echo "--- ring (SWEET GPU) ---"
	grep -a "SWEET GPU" "logs/ring-gpu-$LABEL.txt" | tail -12
}

case "${1:-}" in
build) build "${2:-GP_SURVEY=1}" ;;
run) run ;;
*) sed -n '2,18p' "$0"; exit 2 ;;
esac
