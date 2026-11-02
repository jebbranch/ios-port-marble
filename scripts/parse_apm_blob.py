#!/usr/bin/env python3
"""Decode a trace produced by tools/apm_blob_capture.sh.

Two jobs:

1. Find the APM-level SET_CFG packets the vendor sent (gprsend kprobe events;
   header layout documented in tools/gpr_capture.sh: p5 = opcode, p8 =
   mem_map_handle, p9 = payload_size) and report the size we are hunting.
2. Find the gsl uprobe hits (ugwr/ugrw/udpw/umap/umeta = gsl_write,
   gsl_graph_write, gsl_dp_write, gsl_shmem_map_allocation,
   gsl_shmem_get_metadata) and turn every plausible (pointer, length) pair into
   a ready-to-run dump command, because the *heap* copy is readable even though
   the /dmabuf: shm mapping is not (VM_PFNMAP -> EIO on /proc/pid/mem and on
   PTRACE_PEEKDATA).

Usage: parse_apm_blob.py <trace-file> [--want 18128] [--tol 400]
"""
import re
import sys

SET_CFG = 16781318  # 0x01001006
SHM_MAP = 16781324  # 0x0100100C
UNMAP = 16781325    # 0x0100100D
OPS = {SET_CFG: "APM_CMD_SET_CFG", SHM_MAP: "APM_CMD_SHM_MAP",
       UNMAP: "APM_CMD_SHM_UNMAP"}
UPR = ("ugwr", "ugrw", "udpw", "umap", "umeta", "ucald", "usetcal", "usetcfg",
       "ugrcal", "ugrcfg", "uioctl", "ushmal")
# events that hand us a *pointer to the source data* rather than a copy length:
# gsl_get_cal_data_from_acdb / gsl_set_cal / gsl_set_config / gsl_graph_set_* .
# Their args point into ordinary heap pages, which /proc/<pid>/mem CAN read.
SRC = ("ucald", "usetcal", "usetcfg", "ugrcal", "ugrcfg")
DUMP_LEN = 20480  # the vendor SHM_MAP for this packet reported 0x5000 bytes

args = [a for a in sys.argv[1:]]
want = 18128
tol = 400
if "--want" in args:
    i = args.index("--want")
    want = int(args[i + 1])
    del args[i:i + 2]
if "--tol" in args:
    i = args.index("--tol")
    tol = int(args[i + 1])
    del args[i:i + 2]
if not args:
    print(__doc__)
    sys.exit(2)

path = args[0]
fields = re.compile(r"\b(p\d+|a\d+|common_pid|pid)=(0x[0-9a-fA-F]+|\d+)")
taskcol = re.compile(r"([\w.:$#\-]+\]?)-(\d+)\s+\[")


def parse_line(line):
    """Field dict for a trace line; kprobe fields print decimal, x64 fetchargs
    print 0x..., and the pid only exists in the TASK-PID column."""
    d = {k: int(v, 0) for k, v in fields.findall(line)}
    m = taskcol.search(line)
    if m:
        d.setdefault("common_pid", int(m.group(2)))
        d["comm"] = m.group(1)
    return d

setcfg, shmmap, uprobe_hits = [], [], []
for line in open(path, "r", errors="replace"):
    if "gprsend" in line:
        d = parse_line(line)
        op = d.get("p5")
        if op in OPS:
            setcfg.append((d.get("common_pid", d.get("pid", 0)), OPS[op],
                           d.get("p3"), d.get("p4"), op, d.get("p8"),
                           d.get("p9")))
    for name in UPR:
        if name + ":" in line or name + " " in line:
            d = parse_line(line)
            uprobe_hits.append((name, d))
            break

print(f"== APM commands seen: {len(setcfg)}")
for pid, name, dst, tok, op, handle, size in setcfg[:40]:
    mark = "  <== TARGET" if (size and abs(size - want) <= tol) else ""
    print(f"  pid={pid:<7} {name:<16} dst={dst:<6} tok={tok:<5} "
          f"op={op:#010x} handle={handle:#010x} size={size}{mark}")

print(f"\n== uprobe hits: {len(uprobe_hits)}")
for name, d in uprobe_hits[:40]:
    print(f"  {name}: " + " ".join(f"a{i}={d.get('a' + str(i), 0):#x}"
                                   for i in range(6)))

print(f"\n== candidate dumps (size within {tol} of {want})")
found = 0
for name, d in uprobe_hits:
    pid = d.get("common_pid", d.get("pid", 0))
    for i in range(6):
        v = d.get("a" + str(i), 0)
        if abs(v - want) <= tol and v < 0x100000:
            ptrs = [d.get("a" + str(j), 0) for j in range(6)
                    if 0x1000 < d.get("a" + str(j), 0) < 0x800000000000]
            for p in ptrs:
                found += 1
                print(f"  {name} len={v} -> /data/local/tmp/apm_shm_read "
                      f"{pid} 0x{p:x} {v} /data/local/tmp/blob-{name}-{p:x}.bin")
if not found:
    print("  none: no uprobe arg pair looked like (ptr, ~18k). Either no gsl_"
          "write happened in the window (no real playback) or the blob is "
          "assembled field-by-field straight into the shm mapping.")

# The source-of-payload family gives pointers into heap pages (readable), so
# emit a dump for every pointer-ish arg; grep the dumps for a u32 == 18128 or
# an AudioReach TLV header (0x08001xxx) to identify the blob.
#
# gsl_ioctl is special and is the one that actually fires in practice:
#   a0 = 'GSL' magic (0x47534c), a1 = cmd id, a2 = pointer to the argument
#   struct, a3 = struct size.  The struct is a bundle of heap pointers, and heap
#   pages ARE readable, so the blob is reachable by walking a2 even when no
#   "write" function is ever called.
ptr_hits = 0
for name, d in uprobe_hits:
    if name == "uioctl":
        pid = d.get("common_pid", d.get("pid", 0))
        a1, a2, a3 = (d.get("a1", 0), d.get("a2", 0), d.get("a3", 0))
        if 0x1000 < a2 < 0x800000000000:
            n = a3 if 8 <= a3 <= 0x10000 else 64
            ptr_hits += 1
            print(f"  uioctl cmd={a1:#x} args={a2:#x} size={a3:#x} -> "
                  f"/data/local/tmp/apm_shm_read {pid} {a2:#x} {n} "
                  f"/data/local/tmp/ioctl{a1:x}-{a2:x}.bin")
        continue
    if name not in SRC:
        continue
    pid = d.get("common_pid", d.get("pid", 0))
    for i in range(6):
        v = d.get("a" + str(i), 0)
        if 0x1000 < v < 0x800000000000 and (v & 3) == 0:
            ptr_hits += 1
            print(f"  {name} a{i}={v:#x} -> /data/local/tmp/apm_shm_read {pid} "
                  f"0x{v:x} {DUMP_LEN} /data/local/tmp/src-{name}-a{i}-{v:x}.bin")
if ptr_hits:
    print(f"  ({ptr_hits} dumps to run; each dumped struct may itself hold "
          f"pointers - walk one level more and look for size==18128)")
else:
    print("  source family: no hits either -> libar-gsl was not used for this "
          "packet; the producer is a different library (libagm/plugin).")
print(f"\nIf the SET_CFG itself is missing from the trace, the vendor never "
      f"opened the device in this window: trigger a *real* playback "
      f"(video/music/ringtone preview), not tinyplay (ASoC delegates to the "
      f"userspace service and shuts the substream down immediately).")
