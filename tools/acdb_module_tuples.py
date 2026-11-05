#!/usr/bin/env python3
"""Dump the vendor's per-module parameter tuples for selected subgraphs.

Why this exists: blocker (2) of the audio line is "what should a graph-level
custom-config SET_CFG actually contain".  Boots G/H/I proved the command-level
status does resolve content (0x1 for our invented records, 0x3 for a record taken
out of the ACDB), so the next probes have to send records that are real in all
four fields - module instance id, param id, size and bytes - and those fields are
not guessed here, they are read out of the ACDB.

The records live inside the blob of param 0x08001003, which is a per-module cal
table laid out as

    { u32 module_count,
      ( u32 module_instance_id, u32 param_count,
        ( u32 param_id, u32 param_size, u8 param_size bytes padded to 4 )*param_count
      )*module_count }

and each props chain belongs to one subgraph, identified by the subgraph id that
appears inside that chain's 0x08001002 blob.

usage:
  python3 tools/acdb_module_tuples.py --acdb-dir ../phone-acdb \
      --subgraph 0xb0000002,0xb0000025,0xb0000024,0xb0000023
"""
import argparse
import glob
import os
import struct
import sys

HEADER_FAMILY = (0x08001000, 0x08001001, 0x08001002, 0x08001003, 0x08001004)
PROPS = 0x08001001
SIG_HEAD = struct.pack("<II", 1, PROPS)
MAX_RECORDS_PER_CHAIN = 64
MAX_RECORD_SIZE = 0x40000


def chain_records(buf, off):
    """Yield (iid, param_id, size, body_offset) along one props TLV chain."""
    out = []
    for _ in range(MAX_RECORDS_PER_CHAIN):
        if off + 12 > len(buf):
            break
        iid, pid, size = struct.unpack_from("<III", buf, off)
        if iid > 0x20 or size == 0 or size > MAX_RECORD_SIZE:
            break
        out.append((iid, pid, size, off + 12))
        off = off + 12 + ((size + 7) & ~7)
    return out


def subgraph_ids(iid, pid, size, body, buf):
    """Subgraph ids named by a chain's 0x08001002 blob."""
    if pid != 0x08001002:
        return set()
    words = size // 4
    if words == 0:
        return set()
    return {w for w in struct.unpack_from("<%dI" % words, buf, body)
            if (w & 0xF0000000) == 0xB0000000}


def module_tuples(size, body, buf):
    """Decode the per-module cal table carried by a 0x08001003 blob."""
    words = size // 4
    if words < 1:
        return
    w = list(struct.unpack_from("<%dI" % words, buf, body))
    pos = 0
    n_modules = w[pos]
    pos += 1
    if n_modules == 0 or n_modules > 4096:
        return
    for _ in range(n_modules):
        if pos + 2 > len(w):
            return
        miid, n_params = w[pos], w[pos + 1]
        pos += 2
        if n_params > 4096:
            return
        for _p in range(n_params):
            if pos + 2 > len(w):
                return
            pid, psize = w[pos], w[pos + 1]
            pos += 2
            nwords = (psize + 3) // 4
            if pos + nwords > len(w):
                return
            data = w[pos:pos + nwords]
            pos += nwords
            yield (miid, pid, psize, tuple(data))


def dump_top_level_records(bufs, wanted_param, wanted_size, limit):
    """Emit C initializer rows for chain-level records {iid, param, size, bytes}.

    The K rounds send records whose param/size pair appears as its own TLV record
    (0x08001004 does), which is a different place from the per-module table inside
    0x08001003, so it needs its own extraction path.
    """
    seen = set()
    rows = []
    for name, buf in bufs:
        off = buf.find(SIG_HEAD)
        while off >= 0:
            for iid, pid, size, body in chain_records(buf, off):
                if pid != wanted_param or (wanted_size and size != wanted_size):
                    continue
                words = struct.unpack_from("<%dI" % ((size + 3) // 4), buf, body)
                if words in seen:
                    continue
                seen.add(words)
                rows.append((name, iid, pid, size, words, off))
                if limit and len(rows) >= limit:
                    return rows
            off = buf.find(SIG_HEAD, off + 1)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--acdb-dir", default=os.path.join(os.path.dirname(__file__),
                                                       "..", "..", "phone-acdb"))
    ap.add_argument("--subgraph", default="0xb0000002,0xb0000025,0xb0000024,0xb0000023")
    ap.add_argument("--limit", type=int, default=0, help="stop after N tuples")
    ap.add_argument("--record", default="",
                    help="dump chain-level records instead: PARAM:SIZE[:COUNT]")
    ap.add_argument("--file", default="",
                    help="only read ACDB files whose name contains this string")
    args = ap.parse_args()

    wanted = {int(x, 0) for x in args.subgraph.split(",") if x}
    files = sorted(glob.glob(os.path.join(args.acdb_dir, "*.acdb")))
    if args.file:
        files = [p for p in files if args.file in os.path.basename(p)]
    if not files:
        sys.exit("no .acdb under %s" % args.acdb_dir)
    bufs = [(os.path.basename(p), open(p, "rb").read()) for p in files]

    if args.record:
        parts = [int(x, 0) for x in args.record.split(":")]
        rows = dump_top_level_records(bufs, parts[0],
                                      parts[1] if len(parts) > 1 else 0,
                                      parts[2] if len(parts) > 2 else 0)
        for name, iid, pid, size, words, off in rows:
            print("/* %s @%#x iid=%#x param=%#010x size=%u */" % (name, off,
                                                                  iid, pid, size))
            print("\t" + ", ".join("0x%08xu" % w for w in words) + ",")
        print("# %d distinct records for param %#x size %s" % (
            len(rows), parts[0], parts[1] if len(parts) > 1 else "any"))
        return

    seen = set()
    shown = 0
    params = {}
    for path in files:
        buf = open(path, "rb").read()
        name = os.path.basename(path)
        off = buf.find(SIG_HEAD)
        while off >= 0:
            subs = set()
            for iid, pid, size, body in chain_records(buf, off):
                subs |= subgraph_ids(iid, pid, size, body, buf)
            hit = subs & wanted
            if hit:
                for iid, pid, size, body in chain_records(buf, off):
                    if pid != 0x08001003:
                        continue
                    for rec in module_tuples(size, body, buf):
                        key = (name[:12],) + rec
                        if key in seen:
                            continue
                        seen.add(key)
                        params[rec[1]] = params.get(rec[1], 0) + 1
                        shown += 1
                        if not args.limit or shown <= args.limit:
                            data = " ".join("%08x" % d for d in rec[3][:4])
                            print("%-14s sub=%s iid=%#07x param=%#010x size=%3u "
                                  "data=%s%s" % (name[:14],
                                                 ",".join(hex(s) for s in sorted(hit)),
                                                 rec[0], rec[1], rec[2], data,
                                                 "…" if len(rec[3]) > 4 else ""))
            off = buf.find(SIG_HEAD, off + 1)

    print("# total tuples: %d" % shown)
    print("# by param id: " + ", ".join("%s=%d" % (hex(k), v)
                                        for k, v in sorted(params.items(),
                                                           key=lambda kv: -kv[1])))


if __name__ == "__main__":
    main()
