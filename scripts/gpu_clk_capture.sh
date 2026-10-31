#!/bin/bash
# Capture how the vendor kernel turns the GPU clocks and rails on.
#
#   ./scripts/gpu_clk_capture.sh install   # write the boot-time kprobe script
#   ./scripts/gpu_clk_capture.sh reboot    # reboot Android to capture a boot
#   ./scripts/gpu_clk_capture.sh pull      # pull the trace and filter it
#   ./scripts/gpu_clk_capture.sh           # install + reboot (then run pull)
#
# Why: the AP cannot latch gpu_cc_ahb (0x911c bit 0) from bare metal.  The
# vendor kernel does it every time a GPU client appears, so kprobe its regmap
# and regulator calls and read the order back.  Requires Android + KernelSU
# (root adb); TWRP has no /data/adb.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SERIAL="${SERIAL:-2aa26095}"
OUT="$ROOT/logs/gpu-clk-$(date +%Y%m%d-%H%M%S).txt"
T=/sys/kernel/tracing

install() {
	adb -s "$SERIAL" shell "su -c 'mkdir -p /data/adb/post-fs-data.d; cat > /data/adb/post-fs-data.d/99-gpuclk.sh << \"EOF\"
#!/system/bin/sh
T=$T
i=0
while [ \\\$i -lt 120 ]; do
  grep -q regmap_update_bits_base /proc/kallsyms && break
  sleep 0.5
  i=\\\$((i+1))
done
[ -d \\\$T ] || exit 0
echo 0 > \\\$T/tracing_on 2>/dev/null
echo > \\\$T/kprobe_events 2>/dev/null
echo \"p:rub regmap_update_bits_base reg=%x1 mask=%x2 val=%x3\" >> \\\$T/kprobe_events 2>/dev/null
echo \"p:rw regmap_write reg=%x1 val=%x2\" >> \\\$T/kprobe_events 2>/dev/null
echo \"p:ren regulator_enable\" >> \\\$T/kprobe_events 2>/dev/null
echo \"p:rdis regulator_disable\" >> \\\$T/kprobe_events 2>/dev/null
echo 8192 > \\\$T/buffer_size_kb 2>/dev/null
for e in rub rw ren rdis; do echo 1 > \\\$T/events/kprobes/\\\$e/enable 2>/dev/null; done
echo 1 > \\\$T/tracing_on 2>/dev/null
EOF
chmod 755 /data/adb/post-fs-data.d/99-gpuclk.sh; echo installed'"
}

reboot() {
	adb -s "$SERIAL" reboot
	echo "rebooting; wait ~60 s then: $0 pull"
}

pull() {
	adb -s "$SERIAL" shell "su -c 'cp $T/trace /data/local/tmp/gpuclk.txt; wc -l /data/local/tmp/gpuclk.txt'" 2>&1 | tail -1
	adb -s "$SERIAL" pull /data/local/tmp/gpuclk.txt "$OUT" 2>&1 | tail -1
	echo "=== GPUCC/GCC clock writes (ordered) ==="
	grep -E "reg=0x(911c|9430|93ec|93f0|9108|905c|9504|62000|81004|81008|81010|7000)" "$OUT" \
		| sed -E 's/^ *[^ ]+-[0-9]+ +\[[0-9]+\] [^:]+ +([0-9.]+): /\1 /' \
		| head -120
	echo
	echo "=== gdsc / rail calls ==="
	grep -E "ren:|rdis:" "$OUT" | head -60
	echo
	echo "full trace: $OUT"
}

case "${1:-}" in
install) install ;;
reboot) reboot ;;
pull) pull ;;
*) install && reboot ;;
esac
