#!/usr/bin/env python3
"""harvest = trigger + capture + dump + identify, in one shot.

Sequence:
  1. flush tracefs, turn tracing on, run a vendor trigger (default: Mi's own
     factory agmplay command, learned from `strings /vendor/bin/audio-factory-test`)
  2. stop tracing, pull the trace
  3. decode: APM-level SET_CFG packets (dst=1) + every gsl uprobe hit
  4. for each uprobe pointer arg, dump memory *immediately* (the buffer is only
     alive for a few ms after the call - that is why this must be one script)
  5. scan the dumps for: the exact payload_size we saw in the trace, the
     mem_map_handle value, and AudioReach TLV markers (0x08001xxx)

Everything is read-only on the device except scratch files under /data/local/tmp.

usage:
  python3 scripts/apm_harvest.py                  # default speaker test tone
  python3 scripts/apm_harvest.py --cmd '<agmplay ...>' --want 18128
"""
import argparse
import re
import struct
import subprocess
import sys
import time

ADB = ["adb", "-s", "2aa26095"]
DEV = "/data/local/tmp"
TLV = (0x08001008, 0x080010C4, 0x0800100C, 0x08001001, 0x08001002)
OPS = {0x01001006: "SET_CFG", 0x0100100C: "SHM_MAP", 0x0100100D: "SHM_UNMAP"}
UPROBES = ("ugwr", "ugrw", "udpw", "umap", "umeta", "ucald", "usetcal",
           "usetcfg", "ugrcal", "ugrcfg", "uioctl", "ushmal")


def sh(devcmd, timeout=120):
    """Run a root shell command on the device (always through su -c)."""
    r = subprocess.run(ADB + ["shell", "su", "-c", devcmd],
                       capture_output=True, text=True, timeout=timeout)
    return (r.stdout or "") + (r.stderr or "")


FIELDS = re.compile(r"\b(p\d+|a\d+|common_pid|pid)=(0x[0-9a-fA-F]+|\d+)")
TASK = re.compile(r"([\w.:$#\-\[\]]+)-(\d+)\s+\[")


def parse(trace):
    apm, ups = [], []
    for line in trace.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        d = {k: int(v, 0) for k, v in FIELDS.findall(line)}
        m = TASK.search(line)
        pid = int(m.group(2)) if m else d.get("common_pid", 0)
        if "gprsend" in line and d.get("p5") in OPS:
            apm.append((pid, OPS[d["p5"]], d.get("p3"), d.get("p4"),
                        d.get("p8"), d.get("p9")))
        for n in UPROBES:
            if n + ":" in line:
                ups.append((n, pid, d))
                break
    return apm, ups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", default=f"{DEV}/agmplay /vendor/etc/bottom_spk.wav "
                    f"-D 100 -d 100 -i TDM-LPAIF-RX-TERTIARY-VIRT-0 -dkv 0xA2000100")
    ap.add_argument("--secs", type=int, default=14)
    ap.add_argument("--want", type=int, default=18128)
    ap.add_argument("--dump", type=int, default=0x2000,
                    help="bytes to read at each pointer when the trace gives no size")
    a = ap.parse_args()

    sh("T=/sys/kernel/tracing; for e in rhb rhw kbw; do echo 0 > $T/events/kprobes/$e/enable 2>/dev/null; done; "
       f"echo 1 > $T/events/kprobes/gprsend/enable 2>/dev/null; echo > $T/trace; echo 1 > $T/tracing_on; "
       f"timeout {a.secs} {a.cmd} >/dev/null 2>&1; echo 0 > $T/tracing_on; echo triggered")
    time.sleep(0.5)
    trace = sh("cat /sys/kernel/tracing/trace", timeout=300)
    apm, ups = parse(trace)

    print(f"== APM-level traffic ({len(apm)} packets)")
    sizes = set()
    for pid, op, dst, tok, handle, size in apm:
        if op == "SET_CFG":
            sizes.add(size)
            print(f"  pid={pid} {op} dst={dst} tok={tok} handle={handle:#010x} "
                  f"size={size}" + ("   <== WANTED" if size == a.want else ""))
    print(f"== uprobe hits: {len(ups)}")

    if not any(s == a.want for s in sizes):
        print(f"  (no {a.want}-byte SET_CFG in this window; harvesting the sizes we did see)")

    dumps = []
    for n, pid, d in ups:
        for i in range(6):
            v = d.get("a" + str(i), 0)
            # Android arm64 user VAs live above 4 GiB (heap/mmap base), so a
            # small integer here is a length/thread-id, not a pointer.
            if not (0x1_0000_0000 < v < 0x800000000000):
                continue
            if (v >> 40) & 0xFF in (0x00, 0xb4) and (v & 3) == 0:
                dumps.append((n, i, pid, v))
    seen = set()
    dumps = dumps[:40]           # round-trips are ~0.2 s each; the buffer must be dumped fast
    for n, i, pid, v in dumps:
        key = (pid, v)          # exact pointer: the same page can hold 2 buffers
        if key in seen:
            continue
        seen.add(key)
        name = f"hb-{n}-a{i}-{v:x}.bin"
        sh(f"/data/local/tmp/apm_shm_read {pid} {v:#x} {a.dump} {DEV}/{name} >/dev/null 2>&1")
        got = sh(f"stat -c %s {DEV}/{name} 2>/dev/null").strip()
        if not got.isdigit() or got == "0":
            continue
        subprocess.run(ADB + ["pull", f"{DEV}/{name}", f"/tmp/{name}"],
                       capture_output=True, timeout=120)
        try:
            b = open(f"/tmp/{name}", "rb").read()
        except OSError:
            continue
        hits = []
        for want in sorted(sizes | {a.want}):
            p = struct.pack("<I", want)
            hits += [f"size={want}@{o:#x}" for o in range(len(b) - 3) if b[o:o + 4] == p]
        for t in TLV:
            p = struct.pack("<I", t)
            hits += [f"TLV{t:#010x}@{o:#x}" for o in range(len(b) - 3) if b[o:o + 4] == p]
        head = b[:32].hex(" ")
        print(f"  {name} ({len(b)} B) head={head}")
        if hits:
            print("     markers: " + " ".join(hits[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
