#!/bin/bash
# Boot one already-built image, wait for the phone to reset, pull the log.
#
#   ./scripts/boot_one.sh images/sweep/s1-default.img s1-default
#
# The kernel resets itself and lands in Android/TWRP (it does NOT come back
# to the bootloader), so we wait for adb, not for fastboot.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"; cd "$ROOT"
IMG="${1:?usage: boot_one.sh <img> <label>}"; LABEL="${2:?label}"
SERIAL="${SERIAL:-2aa26095}"

fastboot -s "$SERIAL" devices | grep -q "$SERIAL" || {
	echo "rebooting to bootloader..."
	adb -s "$SERIAL" reboot bootloader 2>/dev/null
	for _ in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q "$SERIAL" && break; sleep 2; done
}
fastboot devices 2>/dev/null | grep -q "$SERIAL" || { echo "!! not in fastboot" >&2; exit 1; }
echo "booting $IMG"
fastboot -s "$SERIAL" boot "$IMG" || exit 1
echo "waiting for the kernel to reset (2-5 min)..."
for t in $(seq 1 96); do
	[ "$(adb -s "$SERIAL" get-state 2>/dev/null)" = "device" ] && { sleep 8; break; }
	[ "$(adb -s "$SERIAL" get-state 2>/dev/null)" = "recovery" ] && { sleep 3; break; }
	sleep 5
done
SERIAL="$SERIAL" ./scripts/pull_logs.sh "$LABEL" 2>&1 | tail -3
echo "--- verdict ---"
if grep -q "ats1pr iova" "logs/ring-$LABEL.txt" 2>/dev/null; then
	grep -E "cba2r@|ats1pr iova|s1 cb|graph_open.*rsp|data_buffer send" "logs/ring-$LABEL.txt" | tail -6
else
	tail -4 "logs/ring-$LABEL.txt" 2>/dev/null
fi
