#!/system/bin/sh
# Device half of tools/apm_blob_capture.sh — uprobe the gsl shared-memory write
# path inside the running AGM service so we can find *where in userspace* the
# 18 KB APM SET_CFG body sits before it is copied into the /dmabuf: region.
#
# Why uprobes and not /proc/<pid>/mem: the shm mapping is VM_PFNMAP (dma_buf),
# so both pread() on /proc/pid/mem and PTRACE_PEEKDATA return EIO for it, while
# ordinary heap mappings read fine.  If we can catch the (buf,len) pair of the
# copy, the heap buffer itself is readable.
#
# Offsets are the .symtab/.dynsym values of /vendor/lib64/libar-gsl.so; that
# library's .text LOAD segment has p_offset == p_vaddr, so symbol value ==
# uprobe file offset (verified with llvm-readelf -l).
set -u
T=/sys/kernel/tracing
GSL=/vendor/lib64/libar-gsl.so
# write-into-shm family: gsl_write=0x333d8 gsl_graph_write=0x334b0
# gsl_dp_write=0x337c8 gsl_shmem_map_allocation=0x33580 gsl_shmem_get_metadata=0x33588
# source-of-payload family (exported by name in libar-gsl.so):
# gsl_get_cal_data_from_acdb=0x33400 gsl_set_cal=0x33390 gsl_set_config=0x33398
# gsl_graph_set_cal=0x334c8 gsl_graph_set_config=0x33498 gsl_ioctl=0x333c8
# gsl_shmem_alloc=0x33558
# If the blob is fetched from ACDB into a heap struct, the logged entry args are
# pointers into ordinary heap pages - and those ARE readable through
# /proc/<pid>/mem (the /dmabuf: mapping is not, see docs/audio-closeout.md).
# SAFETY: ugwr/ugrw/udpw (gsl_write family) sit on the PCM hot path - 12295 hits in
# 100 s once, and the phone self-rebooted inside that window (uptime went from 2111 s
# to 0; cause not proven, but do not run them for long).  Set PROBES from the caller
# to a cold-only list for long recordings; that is what apm_harvest.py now does.
PROBES="${PROBES:-ugwr:0x333d8 ugrw:0x334b0 udpw:0x337c8 umap:0x33580 umeta:0x33588 \
ucald:0x33400 usetcal:0x33390 usetcfg:0x33398 ugrcal:0x334c8 ugrcfg:0x33498 \
uioctl:0x333c8 ushmal:0x33558}"

case "${1:-}" in
arm)
	echo 0 > $T/tracing_on
	# disabling first is mandatory: "echo > uprobe_events" on an enabled event
	# returns EBUSY and leaves the old set in place (same trap as kprobe_events).
	for p in $PROBES; do
		n=${p%%:*}
		echo 0 > $T/events/uprobes/$n/enable 2>/dev/null
	done
	echo > $T/uprobe_events || echo "warn: uprobe_events busy, keeping old set"
	for p in $PROBES; do
		n=${p%%:*}; o=${p##*:}
# $argN is rejected by this kernel ("Invalid $-valiable specified"), so name the
# aarch64 argument registers directly.
		echo "p:$n $GSL:$o a0=%x0:x64 a1=%x1:x64 a2=%x2:x64 a3=%x3:x64 a4=%x4:x64 a5=%x5:x64" >> $T/uprobe_events
		if [ $? -ne 0 ]; then
			echo "UPROBE ADD FAILED $n $o"
			cat $T/error_log
			exit 1
		fi
	done
	echo 8192 > $T/buffer_size_kb
	for p in $PROBES; do
		n=${p%%:*}
		echo 1 > $T/events/uprobes/$n/enable || echo "enable failed $n"
	done
	echo "uprobes armed:"
	cat $T/uprobe_events
	;;
run)
	wav=${2:-/data/local/tmp/tone.wav}
	echo 1 > $T/tracing_on
	echo "-- tinyplay $wav"
	tinyplay "$wav" >/dev/null 2>&1
	echo "-- tinyplay rc=$?"
	echo 0 > $T/tracing_on
	;;
pull) cat $T/trace ;;
clean)
	for p in $PROBES; do
		n=${p%%:*}
		echo 0 > $T/events/uprobes/$n/enable 2>/dev/null
	done
	echo > $T/uprobe_events
	echo "uprobes removed"
	;;
*) echo "usage: $0 arm|run [wav]|pull|clean" ;;
esac
