#!/usr/bin/env python3
"""Read the SWEETLOG ring header and prove what this session actually wrote.

The pull decodes raw[0x1000 : 0x1000 + header.total], and the ring is never erased, so a
short run leaves the PREVIOUS boot's records sitting right after its own window - which is
how I nearly read L2's `pcm tone done` / `PSCI SYSTEM_RESET` as M1's own ending.

  tools/ring_window.py m1                 # header vs decoded, stale-tail warnings
  tools/ring_window.py m1 --dump 0x15000 0x15200
"""
import pathlib
import re
import struct
import sys

ENDINGS = ("pcm tone done", "===== done =====", "PSCI SYSTEM_RESET")
ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: ring_window.py <label|path> [--dump LO HI]")
    arg = sys.argv[1]
    if arg.endswith(".bin"):
        raw = pathlib.Path(arg)
    else:
        cand = sorted((ROOT / "logs" / "raw").glob(f"ring-{arg}*.bin"),
                      key=lambda p: p.stat().st_mtime)
        if not cand:
            sys.exit(f"no raw ring matching ring-{arg}*.bin in {ROOT/'logs/raw'}")
        raw = cand[-1]
    txt = ROOT / "logs" / f"{raw.stem}.txt"
    d = raw.read_bytes()
    magic = d[:8]
    ver, size = struct.unpack_from("<II", d, 8)
    total, seq = struct.unpack_from("<QQ", d, 16)
    print(f"{raw.name}: magic={magic.decode(errors='replace')} ver={ver} "
          f"ring={size:#x} total={total:#x} seq={seq}")
    if magic != b"SWEETLOG":
        sys.exit("not a SWEETLOG ring (wrong partition offset?)")
    if txt.exists():
        n = txt.stat().st_size
        print(f"  decoded {txt.name}: {n:#x} bytes  window-match={n == total}")
    end = 0x1000 + total
    for name in ENDINGS:
        needle = name.encode()
        inside = d.count(needle, 0x1000, end)
        past, i = [], end
        while True:
            j = d.find(needle, i)
            if j < 0:
                break
            past.append(hex(j))
            i = j + 1
        flag = "" if inside else "  <-- MISSING from this session"
        print(f"  {name:22} in-window={inside} past-window={past or '-'}{flag}")
    if "--dump" in sys.argv:
        lo = int(sys.argv[sys.argv.index("--dump") + 1], 0)
        hi = int(sys.argv[sys.argv.index("--dump") + 2], 0)
        for p in re.findall(rb"[ -~]{8,}", d[lo:hi]):
            print(f"  {lo:#x}+  {p.decode()[:96]}")
            lo += len(p) + 1


if __name__ == "__main__":
    main()
