#!/bin/bash
# Capture the real GPR packets a running Android sends to the ADSP.
#
#   ./tools/gpr_capture.sh arm [bytes]        # install a kprobe on gpr_send_pkt
#   ./tools/gpr_capture.sh run -- <cmd>       # trace while <cmd> runs on device
#   ./tools/gpr_capture.sh pull               # fetch + decode the trace
#   ./tools/gpr_capture.sh clean              # remove the kprobe
#
# Why: our XNU driver has to speak the same AudioReach protocol as the vendor
# HAL.  Guessing the packet shapes from headers cost several boots; the vendor
# itself will tell us, for free, if we watch gpr_send_pkt in its own kernel.
#
# Requirements: root on the device (SukiSU `su`), tracefs with kprobe_events.
# A kernel with CONFIG_KPROBE_EVENTS but without FUNCTION_TRACER is fine —
# only kprobe_events is used.
#
# The kprobe dumps N bytes of the buffer in x1 as u32 words (pK = offset 4*K,
# printed in decimal).  GPR header (24 bytes):
#
#   +0  u32  (ver:4 | cmd:4 | len:24)      len = payload bytes, not total
#   +4  u32  domains        0x302 = APPS(3) -> ADSP(2)
#   +8  u32  src            client port that sent it
#   +12 u32  dst            1 = APM, else module instance id
#   +16 u32  token
#   +20 u32  opcode
#
# Known opcodes: GRAPH_OPEN 0x01001000, PREPARE 01, START 02, STOP 03,
# CLOSE 04, SET_CFG 0x01001006, SHM_MAP 0x0100100c, SHM_UNMAP 0x0100100d,
# DATA_BUFFER_V2 0x0400100a.
#
# APM payload for module commands (16-byte apm_cmd_hdr, then the body):
#   +0 addr_lsw  +4 addr_msw  +8 mem_map_handle  +12 payload_size
# For GRAPH_OPEN / DATA_BUFFER the body is instead described *out of band*:
# {offset_in_mapped_region, 0, handle, size}.  The vendor puts the graph blob
# at offset 0 and then reuses the same region for PCM at 0, 0x1000, 0x2000,
# 0x3000.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SERIAL="${SERIAL:-2aa26095}"
OUTDIR="${OUTDIR:-$ROOT/logs}"
T=/sys/kernel/tracing
ADB="adb -s $SERIAL"

DEVSH=/data/local/tmp/gpr_capture_dev.sh

devscript() {
	cat <<'EOS'
#!/system/bin/sh
# device half of tools/gpr_capture.sh
set -u
T=/sys/kernel/tracing
N="${2:-384}"
case "$1" in
setup)
	echo 0 > $T/tracing_on
	echo > $T/kprobe_events
	# u32 words: kprobe prints them in decimal, which parse_gpr_trace.py
	# expects.  128 fetchargs is the hard cap, so 96 words = 384 bytes.
	FIELDS=""; i=0
	while [ $((i*4)) -lt "$N" ]; do
		[ -n "$FIELDS" ] && FIELDS="$FIELDS "
		FIELDS="${FIELDS}p$i=+$((i*4))(%x1):u32"
		i=$((i+1))
	done
	echo "p:gprsend gpr_send_pkt $FIELDS" >> $T/kprobe_events || {
		echo "kprobe add failed"; cat $T/error_log; exit 1; }
	echo 8192 > $T/buffer_size_kb
	echo 1 > $T/events/kprobes/gprsend/enable
	echo "probe armed, $i words ($N bytes)"
	;;
start) echo 1 > $T/tracing_on ;;
stop)  echo 0 > $T/tracing_on ;;
clean)
	echo 0 > $T/tracing_on
	echo 0 > $T/events/kprobes/gprsend/enable 2>/dev/null
	echo > $T/kprobe_events
	;;
esac
EOS
}

push_dev() {
	devscript > /tmp/gpr_capture_dev.sh
	$ADB push /tmp/gpr_capture_dev.sh "$DEVSH" >/dev/null
	$ADB shell "su -c 'chmod 755 $DEVSH'"
}

case "${1:-}" in
arm)
	push_dev
	$ADB shell "su -c 'sh $DEVSH setup ${2:-384}'"
	;;
run)
	shift
	[ "${1:-}" = "--" ] && shift
	[ $# -gt 0 ] || { echo "usage: $0 run -- <device command>" >&2; exit 2; }
	$ADB shell "su -c 'sh $DEVSH start; $*; echo rc=\$?; sh $DEVSH stop'"
	;;
pull)
	mkdir -p "$OUTDIR"
	f="$OUTDIR/gpr-$(date +%Y%m%d-%H%M%S).txt"
	$ADB shell "su -c 'cat $T/trace'" > "$f"
	echo "trace: $f"
	python3 "$HERE/../scripts/parse_gpr_trace.py" "$f"
	;;
clean)
	$ADB shell "su -c 'sh $DEVSH clean'" 2>/dev/null || true
	echo "probe removed"
	;;
*)
	sed -n '2,40p' "$0"
	exit 2
	;;
esac
