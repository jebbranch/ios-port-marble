#!/bin/bash
# kgsl-smmu ownership probe for marble (GPU SMMU, not apps-smmu).
#
#   ./scripts/gpu_probe.sh build        # gpu-l1.img, gpu-l4.img, gpu-l5.img
#   ./scripts/gpu_probe.sh run 1        # read-only: ID/SMR/S2CR
#   ./scripts/gpu_probe.sh run 4        # + S2CR BYPASS/FAULT write test + KGSL read
#   ./scripts/gpu_probe.sh run 5        # + TRANS S2CR, CBAR(0), CB0.SCTLR writes
#
# Why the shim does the risky part: an access to a powered-down Qualcomm block
# raises a synchronous external abort.  XNU would panic and the UFS log would
# stop; the shim's exception handler skips that one instruction, records
# ESR/FAR and carries on.  XNU only reads the result page at 0xa2e0f000 and
# prints it into the normal SWEETLOG ring.
#
# What decides the GPU route:
#   byp=0x10000  S2CR Bypass stuck      -> AP owns kgsl-smmu, port is viable
#   byp=0x20000  rewritten to Fault     -> same hypervisor lock as apps-smmu
#   rf=1         first read aborted     -> block is off/XPU; power it first
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
OUTDIR="$ROOT/images"

build_one() {
	local lvl="$1"
	echo "=== build level $lvl ==="
	GPU_PROBE_LEVEL="$lvl" ./scripts/mkimage.sh sweetgpu=1 \
		> "$OUTDIR/gpu-l$lvl.build.log" 2>&1 || {
		tail -20 "$OUTDIR/gpu-l$lvl.build.log" >&2
		return 1
	}
	cp images/shim-iosroot-adsp.img "$OUTDIR/gpu-l$lvl.img" || return 1
	grep -q "sweetgpu=1" "$OUTDIR/gpu-l$lvl.build.log" || {
		echo "!! sweetgpu=1 missing from the cmdline" >&2
		return 1
	}
	# The ring in userdata survives boots when XNU never runs, so the only
	# proof that a pulled log belongs to THIS image is its sweettime.
	sed -n 's/.*sweettime=\([0-9][0-9]*\).*/\1/p' "$OUTDIR/gpu-l$lvl.build.log" \
		| tail -1 > "$OUTDIR/gpu-l$lvl.sweettime"
	echo "sweettime: $(cat "$OUTDIR/gpu-l$lvl.sweettime")"
	ls -lh "$OUTDIR/gpu-l$lvl.img"
}

run_one() {
	local lvl="$1"
	local img="$OUTDIR/gpu-l$lvl.img"
	[ -f "$img" ] || { echo "missing $img; run: $0 build" >&2; return 1; }

	if ! fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL"; then
		echo "rebooting to bootloader..."
		adb -s "$SERIAL" reboot bootloader >/dev/null 2>&1 || true
		for _ in $(seq 1 90); do
			fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL" && break
			sleep 2
		done
	fi
	fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL" || {
		echo "!! $SERIAL is not in fastboot" >&2
		return 1
	}

	echo "booting $img"
	fastboot -s "$SERIAL" boot "$img" || return 1

	echo "waiting for the kernel to reset and Android to answer adb..."
	for _ in $(seq 1 120); do
		local st
		st=$(adb -s "$SERIAL" get-state 2>/dev/null || true)
		[ "$st" = "device" ] || [ "$st" = "recovery" ] && break
		sleep 5
	done
	sleep 5

	SERIAL="$SERIAL" ./scripts/pull_logs.sh "gpu-l$lvl" >/dev/null 2>&1 || {
		echo "!! log pull failed" >&2
		return 1
	}
	local want got
	want=$(cat "$OUTDIR/gpu-l$lvl.sweettime" 2>/dev/null || echo 0)
	got=$(sed -n 's/.*SWEET RTC: clock set from sweettime=\([0-9][0-9]*\).*/\1/p' \
		"logs/ring-gpu-l$lvl.txt" | tail -1)
	echo "sweettime: image=$want log=$got"
	if [ "$want" != "$got" ]; then
		echo "!! STALE RING: the log is not from this image." >&2
		echo "   XNU never ran (the shim most likely panicked)." >&2
		return 2
	fi
	echo "--- SWEET GPU ---"
	grep -a "SWEET GPU" "logs/ring-gpu-l$lvl.txt" || echo "  (no SWEET GPU lines)"
	echo "--- tail ---"
	tail -6 "logs/ring-gpu-l$lvl.txt"
}

case "${1:-}" in
build)
	build_one 1 || exit 1
	build_one 4 || exit 1
	build_one 5 || exit 1
	;;
run)
	lvl="${2:-1}"
	case "$lvl" in 1|4|5) ;; *) echo "level must be 1, 4 or 5" >&2; exit 2 ;; esac
	run_one "$lvl"
	;;
*)
	sed -n '2,26p' "$0"
	exit 2
	;;
esac
