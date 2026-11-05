#!/bin/bash
# Capture the GPR packets Android *receives* from the ADSP (the response direction).
#
#   tools/gpr_rx_capture.sh arm [words]     # append ONE kprobe, never clear the file
#   tools/gpr_rx_capture.sh start|stop
#   tools/gpr_rx_capture.sh pull            # fetch + decode
#   tools/gpr_rx_capture.sh clean           # remove ONLY our probe
#
# Why this exists: every capture so far watched gpr_send_pkt, i.e. the send direction
# only (logs/vendor-gprsend.txt, 42 packets).  We have never seen what an *accepted*
# command's response looks like on the wire, which is exactly what audio blocker (2)
# needs.  The router's receive entry is gpr_callback in gpr_dlkm; its name carries a
# compiler hash, so it is looked up at runtime instead of being hard-coded.
#
# HARD RULE: this phone already runs the GPU/RPMh probes (kbw, rhw, rhb) and they
# re-enable themselves.  `echo > kprobe_events` would delete them, so this script only
# ever appends `p:gpr_rx ...` and removes it with `-:gpr_rx`.  It also leaves
# tracing_on alone unless asked.
#
# Register guess to be falsified by the data itself: x1 is taken as the packet pointer
# (same convention as the send probe).  x0/x2 are dumped too, so if x1 is not a GPR
# header the dump is obviously garbage rather than silently mis-read.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SERIAL="${SERIAL:-2aa26095}"
OUTDIR="${OUTDIR:-$ROOT/logs}"
ADB="adb -s $SERIAL"
T=/sys/kernel/tracing
DEVSH=/data/local/tmp/gpr_rx_dev.sh

devscript() {
	cat <<'EOS'
#!/system/bin/sh
set -u
T=/sys/kernel/tracing
W="${2:-32}"
case "$1" in
setup)
	# resolve the hashed symbol at runtime; refuse to guess
	SYM=$(grep -E ' t gpr_callback' /proc/kallsyms | grep gpr_dlkm | grep -v cfi_jt | head -1 | awk '{print $3}')
	[ -n "$SYM" ] || { echo "NO SYMBOL: gpr_callback not in kallsyms"; exit 1; }
	echo "symbol: $SYM"
	if grep -q '^p:kprobes/gpr_rx ' $T/kprobe_events 2>/dev/null; then
		echo "already armed"; exit 0
	fi
	FIELDS="r0=%x0 r1=%x1 r2=%x2"; i=0
	while [ "$i" -lt "$W" ]; do
		FIELDS="$FIELDS p$i=+$((i*4))(%x1):u32"
		i=$((i+1))
	done
	echo "p:gpr_rx $SYM $FIELDS" >> $T/kprobe_events || {
		echo "kprobe add FAILED"; cat $T/error_log 2>/dev/null | tail -3; exit 1; }
	echo 1 > $T/events/kprobes/gpr_rx/enable
	echo "armed: $W words ($((W*4)) bytes) from x1, plus r0/r1/r2"
	echo "existing probes kept:"; grep -c '^p:' $T/kprobe_events
	;;
start) echo 1 > $T/tracing_on; echo 1 > $T/events/kprobes/gpr_rx/enable ;;
stop)  echo 0 > $T/events/kprobes/gpr_rx/enable ;;
clean)
	echo 0 > $T/events/kprobes/gpr_rx/enable 2>/dev/null
	echo '-:gpr_rx' >> $T/kprobe_events 2>/dev/null
	echo "removed gpr_rx only; remaining:"; grep '^p:' $T/kprobe_events | sed 's/ .*//'
	;;
esac
EOS
}

push_dev() {
	devscript > /tmp/gpr_rx_dev.sh
	$ADB push /tmp/gpr_rx_dev.sh "$DEVSH" >/dev/null
	$ADB shell "su -c 'chmod 755 $DEVSH'"
}

case "${1:-}" in
arm)   push_dev; $ADB shell "su -c 'sh $DEVSH setup ${2:-32}'" ;;
start) $ADB shell "su -c 'sh $DEVSH start'" ;;
stop)  $ADB shell "su -c 'sh $DEVSH stop'" ;;
pull)
	mkdir -p "$OUTDIR"
	f="$OUTDIR/gpr-rx-$(date +%Y%m%d-%H%M%S).txt"
	$ADB shell "su -c 'cat $T/trace'" > "$f"
	echo "trace: $f  ($(wc -l < "$f" | tr -d ' ') lines)"
	echo "gpr_rx hits: $(LC_ALL=C grep -ac 'gpr_rx:' "$f")"
	;;
clean) $ADB shell "su -c 'sh $DEVSH clean'" ;;
*) sed -n '2,30p' "$0"; exit 2 ;;
esac
