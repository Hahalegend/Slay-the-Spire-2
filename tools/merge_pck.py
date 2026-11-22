#!/usr/bin/env python3
"""merge_pck.py -- merge a Godot .pck overlay (e.g. sts2_astc.pck) into a base
sts2.pck to produce a single compact self-contained .pck for standalone NSP RomFS.

Every entry present in the overlay replaces the matching entry in the base pack
in-place; any extra entries in the overlay are appended. Output preserves exact
Godot GDPC header version/flags/file_base, 4-byte entry alignment, and fresh MD5s.

Usage:
    python tools/merge_pck.py base.pck overlay.pck merged.pck
"""

import hashlib
import os
import struct
import sys


def read_pck(path):
    f = open(path, "rb")
    if f.read(4) != b"GDPC":
        sys.exit(f"not a .pck (no GDPC magic): {path}")
    ver, maj, mnr, patch = struct.unpack("<4I", f.read(16))
    flags, file_base = struct.unpack("<IQ", f.read(12))
    dir_off, = struct.unpack("<Q", f.read(8)) if ver >= 3 else (0,)
    f.seek(dir_off if ver >= 3 else 100)
    count, = struct.unpack("<I", f.read(4))
    entries = []
    for _ in range(count):
        n, = struct.unpack("<I", f.read(4))
        p = f.read(n).rstrip(b"\0").decode("utf-8", "replace")
        off, sz = struct.unpack("<QQ", f.read(16))
        md5 = f.read(16)
        eflags, = struct.unpack("<I", f.read(4)) if ver >= 2 else (0,)
        entries.append((p, off, sz, md5, eflags))
    return f, (ver, maj, mnr, patch, flags, file_base), entries


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: merge_pck.py <base.pck> <overlay.pck> <out.pck>")
    base_path, ovr_path, out_path = sys.argv[1:4]

    fb, hdr_b, entries_b = read_pck(base_path)
    fo, hdr_o, entries_o = read_pck(ovr_path)
    ver, maj, mnr, patch, flags, file_base = hdr_b
    ovr_base = hdr_o[5]

    ovr_map = {e[0]: e for e in entries_o}
    print(f"base:    {len(entries_b)} files ({os.path.getsize(base_path)/1024/1024:.1f} MB)")
    print(f"overlay: {len(entries_o)} files ({os.path.getsize(ovr_path)/1024/1024:.1f} MB)")

    out = open(out_path, "wb")
    out.write(b"GDPC")
    out.write(struct.pack("<4I", ver, maj, mnr, patch))
    out.write(struct.pack("<IQ", flags, file_base))
    out.write(struct.pack("<Q", 0))  # dir offset, patched below
    out.write(b"\0" * (file_base - out.tell()))

    new_entries = []
    replaced = 0
    seen = set()

    def write_entry(p, src_f, src_base, off, sz, md5, ef):
        src_f.seek(src_base + off)
        pos = out.tell() - file_base
        rem = sz
        h = hashlib.md5()
        while rem > 0:
            chunk = src_f.read(min(rem, 4 * 1024 * 1024))
            if not chunk:
                sys.exit(f"unexpected EOF reading {p}")
            out.write(chunk)
            h.update(chunk)
            rem -= len(chunk)
        pad = (-sz) % 4
        if pad:
            out.write(b"\0" * pad)
        new_entries.append((p, pos, sz, h.digest(), ef))

    for p, off, sz, md5, ef in entries_b:
        seen.add(p)
        if p in ovr_map:
            op, ooff, osz, omd5, oef = ovr_map[p]
            write_entry(op, fo, ovr_base, ooff, osz, omd5, oef)
            replaced += 1
        else:
            write_entry(p, fb, file_base, off, sz, md5, ef)

    added = 0
    for op, ooff, osz, omd5, oef in entries_o:
        if op not in seen:
            write_entry(op, fo, ovr_base, ooff, osz, omd5, oef)
            added += 1

    dir_off = out.tell()
    out.write(struct.pack("<I", len(new_entries)))
    for p, off, sz, md5, ef in new_entries:
        raw = p.encode("utf-8")
        pad = (-len(raw)) % 4
        out.write(struct.pack("<I", len(raw) + pad))
        out.write(raw + b"\0" * pad)
        out.write(struct.pack("<QQ", off, sz))
        out.write(md5)
        if ver >= 2:
            out.write(struct.pack("<I", ef))

    out.seek(32)
    out.write(struct.pack("<Q", dir_off))
    out.close()
    fb.close()
    fo.close()

    print(f"merged:  {len(new_entries)} files (replaced={replaced}, added={added}) -> "
          f"{os.path.getsize(out_path)/1024/1024:.1f} MB ({os.path.getsize(out_path):,} bytes)")


if __name__ == "__main__":
    main()
