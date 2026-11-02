#!/system/bin/sh
# Read-only helper for the 18 KB APM SET_CFG capture.
#  shmscan : list rw-s /dmabuf mappings of every audio process
#  shmdump <pid> <hexbase> <bytes> : dump via /proc/<pid>/mem to stdout
set -u
T=/data/local/tmp

case "${1:-}" in
shmscan)
	for p in $(pidof vendor.qti.hardware.AGMIPC@1.0-service) \
		 $(pidof android.hardware.audio.service_64) \
		 $(pidof audioserver) \
		 $(pidof audioadsprpcd) \
		 $(pidof media.swcodec) ; do
		[ -d "/proc/$p" ] || continue
		echo "== pid $p $(tr '\0' ' ' < /proc/$p/cmdline)"
		grep -aE 'rw-s|dmabuf' "/proc/$p/maps"
	done
	;;
shmstat)
	# dma-buf exporters still alive (sizes) - best effort, read-only
	ls -l /proc/$2/fd 2>/dev/null | grep -a dma | head
	;;
shmdump)
	pid=$2; base=$3; bytes=${4:-32768}
	# base must be page aligned; one dd, 4096-byte blocks
	skip=$(( 0x$base / 4096 ))
	count=$(( bytes / 4096 ))
	out="$T/shm-$pid-$base-$bytes.bin"
	dd if="/proc/$pid/mem" of="$out" bs=4096 skip=$skip count=$count 2>&1
	ls -l "$out"
	;;
*)
	echo "usage: $0 shmscan|shmdump <pid> <hexbase> [bytes]"
	;;
esac
