#!/bin/bash
# One-shot capture of the vendor's 18 KB APM-level SET_CFG body on marble.
#
# What it does:
#   arm   - silence the leftover GPU/rpmh kprobes (they flood 200k events/boot
#           and squeeze everything else out of the ring), enable the gpr_send_pkt
#           kprobe + the gsl_* uprobes, prove the uprobe instrument works with a
#           self-test binary, then start recording.  After this, *play any audio
#           on the phone* (video, music, ringtone preview).  tinyplay does not
#           work: the ASoC machine delegates PCM to the userspace service and
#           shuts the substream straight down (dmesg: check_userspace_service_state).
#   pull  - stop recording, archive the trace to logs/, decode it, and dump every
#           (pointer, ~18k) candidate that a uprobe exposed.  Heap mappings are
#           readable; the /dmabuf: shm mapping is VM_PFNMAP and returns EIO to
#           both /proc/pid/mem and PTRACE_PEEKDATA, which is why we fish for the
#           *source* buffer instead of the shm itself.
#   clean - remove the uprobes (the gpr kprobe is left alone; tools/gpr_capture.sh
#           clean removes it).
#
# Red lines: read-only.  Nothing is written to any boot-critical partition; the
# only device writes are scratch files under /data/local/tmp.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SERIAL="${SERIAL:-2aa26095}"
ADB="adb -s $SERIAL"
DEV=/data/local/tmp
OUTDIR="${OUTDIR:-$ROOT/logs}"

need_dev() {
	$ADB push "$HERE/apm_blob_dev.sh" "$DEV/apm_blob_dev.sh" >/dev/null
	$ADB push /tmp/uprobe_selftest "$DEV/uprobe_selftest" >/dev/null 2>&1
	$ADB push /tmp/apm_shm_read "$DEV/apm_shm_read" >/dev/null 2>&1
	$ADB shell "su -c 'chmod 755 $DEV/apm_blob_dev.sh $DEV/uprobe_selftest $DEV/apm_shm_read 2>/dev/null'"
}

case "${1:-}" in
arm)
	need_dev
	# tracefs resets on every boot, so the gpr kprobe has to be (re)installed.
	# Disable every kprobe event first: `echo > kprobe_events` returns EBUSY while
	# an event is still enabled, and 99-gpuclk.sh leaves three of them enabled.
	$ADB shell "su -c 'T=/sys/kernel/tracing
		for f in \$T/events/kprobes/*/enable; do echo 0 > \$f 2>/dev/null; done
		echo > \$T/kprobe_events 2>/dev/null || true'"
	bash "$HERE/gpr_capture.sh" arm | tail -1
	$ADB shell "su -c 'T=/sys/kernel/tracing
		for e in rhb rhw kbw; do echo 0 > \$T/events/kprobes/\$e/enable 2>/dev/null; done
		echo 1 > \$T/events/kprobes/gprsend/enable 2>/dev/null
		sh $DEV/apm_blob_dev.sh arm'"
	echo "-- positive control (uprobe self-test)"
	$ADB shell "su -c 'T=/sys/kernel/tracing
		echo \"p:uself $DEV/uprobe_selftest:0x171cc a0=%x0:x64\" >> \$T/uprobe_events
		echo 1 > \$T/events/uprobes/uself/enable
		echo > \$T/trace; echo 1 > \$T/tracing_on
		$DEV/uprobe_selftest >/dev/null
		echo selftest-hits=\$(grep -ac uself \$T/trace)
		echo 0 > \$T/events/uprobes/uself/enable'"
	$ADB shell "su -c 'T=/sys/kernel/tracing; echo > \$T/trace; echo 1 > \$T/tracing_on; echo recording'"
	echo "READY — now play any audio on the phone for ~5 s, then run:"
	echo "     $0 pull"
	;;
pull)
	mkdir -p "$OUTDIR"
	f="$OUTDIR/apm-blob-$(date +%Y%m%d-%H%M%S).txt"
	$ADB shell "su -c 'echo 0 > /sys/kernel/tracing/tracing_on; cat /sys/kernel/tracing/trace'" > "$f"
	echo "trace: $f"
	python3 "$ROOT/scripts/parse_apm_blob.py" "$f"
	;;
clean)
	$ADB shell "su -c 'sh $DEV/apm_blob_dev.sh clean'"
	;;
*)
	sed -n '2,30p' "$0"
	exit 2
	;;
esac
