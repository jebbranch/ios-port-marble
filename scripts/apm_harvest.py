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
import os
import re
import struct
import subprocess
import sys
import time

ADB = ["adb", "-s", "2aa26095"]
DEV = "/data/local/tmp"
TLV = (0x08001008, 0x080010C4, 0x0800100C, 0x08001001, 0x08001002)
OPS = {0x01001006: "SET_CFG", 0x0100100C: "SHM_MAP", 0x0100100D: "SHM_UNMAP",
       0x01001000: "GRAPH_OPEN", 0x01001001: "GRAPH_PREPARE",
       0x01001002: "GRAPH_START"}
# The vendor GRAPH_OPEN payload (the graph body) starts with a compact TLV
# header {iid=1, PARAM_ID_SUB_GRAPH_CONFIG=0x08001001, size=0x30}; that is the
# same 12-byte signature found in the AGM heap (docs/vendor-graph.md).
GRAPH_SIG = struct.pack("<III", 1, 0x08001001, 0x30)
# The two payload sizes the vendor actually opens: 0x1f0 stream graph,
# 0xb38 device graph (work/sweet-audio/traces/trace_ui.txt).
GRAPH_SIZES = (0x1f0, 0xb38)
# A graph body starts with its own header {graph_size, num_subgraphs}; g_graph_vendor4
# proves the framing (0xaa4 = 2724 = the array length).  So a heap candidate is only
# accepted if `count` is small AND [8, size) actually holds that many GRAPH_SIG hits -
# a bare u32==size search matches pointer tables and is worthless.
UPROBES = ("ugwr", "ugrw", "udpw", "umap", "umeta", "ucald", "usetcal",
           "usetcfg", "ugrcal", "ugrcfg", "uioctl", "ushmal")
# which thread/proc ids show up is boot-dependent; the parse keys on names only.


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


def offsets(b, pat):
    """All offsets of pat via bytes.find - the per-byte range() equivalent here
    would cost minutes over a 2 MiB heap region."""
    out, i = [], b.find(pat)
    while i >= 0:
        out.append(i)
        i = b.find(pat, i + 1)
    return out


def graph_headers(b):
    """Verified graph bodies: {graph_size, num_subgraphs} followed by at least
    num_subgraphs GRAPH_SIG headers inside the declared size."""
    out = []
    for o in range(0, len(b) - 8, 4):
        size, count = struct.unpack_from("<II", b, o)
        if not (0x100 <= size <= 0x4000) or not (1 <= count <= 8) or o + size > len(b):
            continue
        inner = [i for i in offsets(b[o + 8:o + size], GRAPH_SIG)]
        if len(inner) < count:
            continue
        sgs = [struct.unpack_from("<I", b, o + 8 + i + 16)[0] for i in inner[:count]]
        out.append((o, size, count, sgs))
    return out


def scan_bytes(b, sizes):
    """Body-detection over one blob: sizes seen in the trace + the vendor graph
    sizes, the subgraph-config TLV, and the GRAPH_OPEN body signature."""
    hits = []
    for want in sorted(sizes | set(GRAPH_SIZES)):
        hits += [f"size={want}@{o:#x}" for o in offsets(b, struct.pack("<I", want))]
    for t in TLV:
        hits += [f"TLV{t:#010x}@{o:#x}" for o in offsets(b, struct.pack("<I", t))]
    hits += [f"GRAPH_SIG@{o:#x}" for o in offsets(b, GRAPH_SIG)]
    return hits


def save_body(b, o, size, out_dir):
    """Write one verified body out as both raw bytes and a C array, so a patch can
    take it without a hex re-parse."""
    os.makedirs(out_dir, exist_ok=True)
    raw = os.path.join(out_dir, f"graph-body-{size:#x}.bin")
    src = os.path.join(out_dir, f"graph-body-{size:#x}.carray")
    open(raw, "wb").write(b[o:o + size])
    with open(src, "w") as f:
        f.write(f"static const uint8_t g_graph_vendor_dev[{size}] = {{\n")
        chunk = b[o:o + size]
        for i in range(0, size, 12):
            f.write("\t" + " ".join(f"0x{v:02x}," for v in chunk[i:i + 12]) + "\n")
        f.write("};\n")
    print(f"     saved {raw} + {src}")


def scan_files(paths, sizes, out_dir="logs"):
    """Offline mode: run the same detector over dumps already on disk."""
    found = 0
    for p in paths:
        try:
            b = open(p, "rb").read()
        except OSError as e:
            print(f"  {p}: unreadable ({e})")
            continue
        heads = graph_headers(b)
        hits = scan_bytes(b, sizes)
        print(f"  {p} ({len(b)} B) head={b[:32].hex(' ')}")
        for o, size, count, sgs in heads:
            found += 1
            print(f"     GRAPH BODY @{o:#x} size={size}({size:#x}) count={count} "
                  f"subgraphs={' '.join(hex(s) for s in sgs)}")
            if size in GRAPH_SIZES:
                save_body(b, o, size, out_dir)
        if hits and not heads:
            print("     markers: " + " ".join(hits[:12]))
    if not found:
        print("  no verified graph body in these files")
    return 0


def find_agm_pid():
    for tok in sh("ps -A -o PID= -o NAME= | grep -i AGMIPC").split():
        if tok.isdigit():
            return int(tok)
    return 0


def heap_regions(pid, cap=8 << 20):
    """Writable allocator regions of a process - where graph bodies live.  File-backed
    rw maps (.bss/.data) are excluded; so are stacks, which is why the name has to
    say malloc."""
    rows = []
    for line in sh(f"cat /proc/{pid}/maps").splitlines():
        f = line.split()
        if len(f) < 5 or not f[1].startswith("rw") or (len(f) > 5 and "malloc" not in " ".join(f[5:])):
            continue
        a, b = f[0].split("-")
        size = int(b, 16) - int(a, 16)
        if 0x1000 <= size <= cap:
            rows.append((int(a, 16), size))
    return rows


def heapscan(pids, out_dir="logs", max_regions=48):
    """No tracefs, no probes, no tone needed: find graph bodies in a process heap by
    their own header.  This ROM's root cannot even setenforce, so the kprobe route in
    main() is dead here (verified 2026-11-03)."""
    found = {}
    for pid in pids:
        regions = heap_regions(pid)[:max_regions]
        comm = sh(f"cat /proc/{pid}/comm").strip()
        print(f"== pid={pid} ({comm}): {len(regions)} malloc regions, "
              f"{sum(s for _, s in regions) / 1e6:.1f} MiB")
        for i, (va, size) in enumerate(regions):
            name = f"hs-{pid}-{i}-{va:x}.bin"
            sh(f"{DEV}/apm_shm_read {pid} {va:#x} {size} {DEV}/{name} >/dev/null 2>&1")
            subprocess.run(ADB + ["pull", f"{DEV}/{name}", f"/tmp/{name}"],
                           capture_output=True, timeout=300)
            sh(f"rm -f {DEV}/{name}")
            try:
                b = open(f"/tmp/{name}", "rb").read()
            except OSError:
                continue
            os.remove(f"/tmp/{name}")
            for o, sz, count, sgs in graph_headers(b):
                mark = "  <== WANTED" if sz in GRAPH_SIZES else ""
                print(f"     body @{o:#x} size={sz}({sz:#x}) count={count} "
                      f"sgs={' '.join(hex(s) for s in sgs)}{mark}")
                if sz in GRAPH_SIZES and sz not in found:
                    found[sz] = True
                    save_body(b, o, sz, out_dir)
    print("== wanted sizes still missing: "
          f"{[hex(s) for s in GRAPH_SIZES if s not in found] or 'none'}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", default=f"{DEV}/agmplay /vendor/etc/bottom_spk.wav "
                    f"-D 100 -d 100 -i TDM-LPAIF-RX-TERTIARY-VIRT-0 -dkv 0xA2000100")
    ap.add_argument("--secs", type=int, default=14)
    ap.add_argument("--want", type=int, default=18128)
    ap.add_argument("--dump", type=int, default=0x2000,
                    help="bytes to read at each pointer when the trace gives no size")
    ap.add_argument("--hot", action="store_true",
                    help="also probe gsl_write/graph_write/dp_write (PCM hot path; the phone "
                         "self-rebooted once inside a 100 s window with them on)")
    ap.add_argument("--scan", nargs="*", default=[],
                    help="offline mode: run the body detector over these local files and exit")
    ap.add_argument("--pid", default="",
                    help="comma-separated pids for --heapscan (default: the AGMIPC service)")
    ap.add_argument("--heapscan", action="store_true",
                    help="no probes, no tone: read the AGM heap and find graph bodies by their "
                         "own {graph_size, num_subgraphs} header (the only route left on this ROM)")
    a = ap.parse_args()

    if a.heapscan:
        pids = ([int(x) for x in a.pid.split(",")] if a.pid else
                [p for p in find_agm_pid() and [find_agm_pid()] or []])
        return heapscan(pids)
    if a.scan:
        return scan_files(a.scan, set())

    # --- (re)install the probes.  tracefs resets every boot, and clearing
    # kprobe_events needs the existing events disabled first (else EBUSY).
    cold = ("umap:0x33580 umeta:0x33588 ucald:0x33400 usetcal:0x33390 "
            "usetcfg:0x33398 ugrcal:0x334c8 ugrcfg:0x33498 uioctl:0x333c8 ushmal:0x33558")
    full = "ugwr:0x333d8 ugrw:0x334b0 udpw:0x337c8 " + cold
    print(sh("T=/sys/kernel/tracing; for f in $T/events/kprobes/*/enable; do echo 0 > $f 2>/dev/null; done; "
             "echo > $T/kprobe_events 2>/dev/null; sh /data/local/tmp/gpr_capture_dev.sh setup 384"))
    print(sh(f"PROBES='{full if a.hot else cold}' sh /data/local/tmp/apm_blob_dev.sh arm | tail -1"))
    sh(f"echo > /sys/kernel/tracing/trace; echo 1 > /sys/kernel/tracing/tracing_on; "
       f"timeout {a.secs} {a.cmd} >/dev/null 2>&1; echo 0 > /sys/kernel/tracing/tracing_on; echo triggered")
    time.sleep(0.5)
    trace = sh("cat /sys/kernel/tracing/trace", timeout=300)
    apm, ups = parse(trace)

    print(f"== APM-level traffic ({len(apm)} packets)")
    sizes = set()
    graph_open = []
    for pid, op, dst, tok, handle, size in apm:
        if op == "SET_CFG":
            sizes.add(size)
        elif op == "GRAPH_OPEN":
            # the payload for a GRAPH_OPEN is the graph body: its size is what
            # the dump below has to match.
            sizes.add(size)
            graph_open.append((pid, dst, handle, size))
        print(f"  pid={pid} {op} dst={dst} tok={tok} handle={handle:#010x} "
              f"size={size}" + ("   <== WANTED" if size == a.want else ""))
    print(f"== uprobe hits: {len(ups)}")
    for pid, dst, handle, size in graph_open:
        print(f"  GRAPH_OPEN pid={pid} src/dst={dst:#x} handle={handle:#010x} body={size}")

    if not any(s == a.want for s in sizes):
        print(f"  (no {a.want}-byte SET_CFG in this window; harvesting the sizes we did see)")

    dumps = []
    for n, pid, d in ups:
        for i in range(6):
            v = d.get("a" + str(i), 0)
            # Android arm64 user VAs live above 4 GiB (heap/mmap base), so a
            # small integer here is a length/thread-id, not a pointer.
            if not (0x1_0000_0000 < v < 0x8000_00000000):
                continue
            tag = (v >> 40) & 0xFF
            if tag not in (0x00, 0xb4) or (v & 3):
                continue
            # dump the untagged form too: /proc/pid/mem rejects a tagged VA
            dumps.append((n, i, pid, v))
            if tag:
                dumps.append((n, i, pid, v & 0x00FF_FFFFFFFF))
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
        hits = scan_bytes(b, sizes)
        head = b[:32].hex(" ")
        print(f"  {name} ({len(b)} B) head={head}")
        if hits:
            print("     markers: " + " ".join(hits[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
