#!/bin/bash
# Boot one RAM-only image and leave it running for interactive use.
#
#   ./scripts/boot_interactive.sh [image]
#
# Default: the last known-good 8-core / keepalive / quiet image.
# This is for KEEPALIVE builds (no sweetreboot=1): it does not wait for a
# reset and does not pull SWEETLOG.  Nothing is written to the phone's
# partitions; the only device operation is `fastboot boot`.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

SERIAL="${SERIAL:-2aa26095}"
IMG="${1:-images/daily-v100b-8core-keepalive-quiet-v97g.img}"

[ -f "$IMG" ] || { echo "!! missing image: $IMG" >&2; exit 1; }
[ "$(wc -c < "$IMG")" -gt 10000000 ] || {
	echo "!! $IMG looks too small to be a boot image" >&2; exit 1; }

if LC_ALL=C grep -aq "sweetreboot=1" "$IMG"; then
	echo "!! $IMG still contains sweetreboot=1 (not a KEEPALIVE image)" >&2
	echo "   build with KEEPALIVE=1, or use scripts/boot_one.sh for log runs" >&2
	exit 1
fi

if fastboot devices 2>/dev/null | grep -q "$SERIAL"; then
	echo "device already in bootloader"
else
	STATE="$(adb -s "$SERIAL" get-state 2>/dev/null || true)"
	if [ "$STATE" = "recovery" ] || [ "$STATE" = "device" ]; then
		echo "adb state $STATE -> rebooting to bootloader"
		adb -s "$SERIAL" reboot bootloader || exit 1
	else
		echo "!! $SERIAL is not on adb or fastboot" >&2
		echo "   (in iOS? hold power until the screen goes dark, then boot TWRP)" >&2
		exit 1
	fi
fi

for _ in $(seq 1 60); do
	fastboot devices 2>/dev/null | grep -q "$SERIAL" && break
	sleep 2
done
fastboot devices 2>/dev/null | grep -q "$SERIAL" || {
	echo "!! $SERIAL did not appear in fastboot" >&2; exit 1; }

echo "booting $IMG (RAM only, no flash)"
fastboot -s "$SERIAL" boot "$IMG" || exit 1

# fastboot hands the image to the bootloader and the USB device drops while
# the kernel takes over.  Wait for that handover, then return.
for _ in $(seq 1 30); do
	fastboot devices 2>/dev/null | grep -q "$SERIAL" || break
	sleep 1
done

echo
echo "image handed over. This build is KEEPALIVE + QUIET:"
echo "  - stays in iOS (no automatic reset; sweetlog ring disabled)"
echo "  - to come back: hold power until the screen goes dark, then boot TWRP"
echo "  - if it panics, the panic screen is the only diagnostic (logs are off)"
