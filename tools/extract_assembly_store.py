#!/usr/bin/env python3
"""extract_assembly_store.py -- pull the managed assemblies out of
libassembly-store.so.

.NET for Android packs every managed assembly into one ELF whose payload is an
"XABA" store, and by default compresses each one with LZ4
(AndroidEnableAssemblyCompression). The launcher's copy holds 260: the BCL,
GodotSharp, SteamKit2, Harmony, and the launcher's own STS2Mobile*.dll.

The game's own sts2.dll is NOT here -- the launcher downloads game content from
Steam at runtime. So why bother:

  GodotSharp.dll IS here, and it is the copy built against the launcher's
  CUSTOM Godot engine -- the same libgodot_android.so that goes on the SD card.
  The GodotSharp.dll in a Steam install is built against the DESKTOP engine.
  Those are not interchangeable: GodotSharp carries the interop surface the
  engine binds against, and a mismatch surfaces as missing methods or wrong
  vtable offsets at runtime rather than as a clean load error.

  So the AOT input set wants sts2.dll from Steam and GodotSharp.dll from HERE.

Each entry is:

    'XALZ' | uint32 descriptor_index | uint32 uncompressed_size | lz4 block

Carving on the XALZ magic rather than parsing the store index keeps this
independent of the index layout, which has changed across .NET versions.

  usage: extract_assembly_store.py <libassembly-store.so> <outdir> [name ...]

Needs python3 -m pip install lz4.
"""

import os
import re
import struct
import sys

try:
    import lz4.block
except ImportError:
    sys.exit("needs the lz4 module:  python3 -m pip install lz4")


def payload_end(data):
    """End offset of the ELF section holding the store, from the section table.

    Parsed rather than assumed: the alternative -- treating end-of-file as the
    end of the last block -- silently loses one assembly per store.
    """
    if data[:4] != b"\x7fELF" or data[4] != 2:      # ELF64 only
        return None
    try:
        e_shoff  = struct.unpack_from("<Q", data, 0x28)[0]
        e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
        e_shnum  = struct.unpack_from("<H", data, 0x3C)[0]
        e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

        sh = lambda i: e_shoff + i * e_shentsize
        str_off = struct.unpack_from("<Q", data, sh(e_shstrndx) + 0x18)[0]

        best = None
        for i in range(e_shnum):
            base = sh(i)
            name_idx = struct.unpack_from("<I", data, base)[0]
            end = data.index(b"\0", str_off + name_idx)
            name = data[str_off + name_idx:end]
            sh_type = struct.unpack_from("<I", data, base + 4)[0]
            offset  = struct.unpack_from("<Q", data, base + 0x18)[0]
            size    = struct.unpack_from("<Q", data, base + 0x20)[0]
            if name == b"payload" and sh_type == 1:   # SHT_PROGBITS
                return offset + size
            if sh_type == 1:
                best = max(best or 0, offset + size)
        return best
    except (struct.error, ValueError, IndexError):
        return None


def assembly_name(blob):
    """The assembly's OWN name, from the Module table of its ECMA-335 metadata.

    Two simpler approaches were tried and both are wrong:

      a regex for the first "*.dll" string names most assemblies after a
      DEPENDENCY, since references appear before the module's own name;

      the first entry of the #Strings heap is not the module name either --
      the heap is not ordered that way, and it yields compiler-generated
      symbols like "<>9__0_0" and type-name hashes.

    So: parse the #~ table stream, take Module (table 0x00) row 0, and follow
    its Name index into #Strings. That is by definition the assembly filename.
    """
    root = blob.find(b"BSJB")
    if root < 0:
        return None
    try:
        ver_len = struct.unpack_from("<I", blob, root + 12)[0]
        p = root + 16 + ((ver_len + 3) & ~3)
        p += 2                                        # flags
        n_streams = struct.unpack_from("<H", blob, p)[0]
        p += 2

        streams = {}
        for _ in range(n_streams):
            off, size = struct.unpack_from("<II", blob, p)
            p += 8
            e = blob.index(b"\0", p)
            streams[blob[p:e]] = (root + off, size)
            p = (e + 1 + 3) & ~3

        if b"#~" not in streams or b"#Strings" not in streams:
            return None

        tbl, _ = streams[b"#~"]
        str_off, str_size = streams[b"#Strings"]

        heap_sizes = blob[tbl + 6]
        str_wide  = bool(heap_sizes & 0x01)
        guid_wide = bool(heap_sizes & 0x02)

        valid = struct.unpack_from("<Q", blob, tbl + 8)[0]
        if not (valid & 1):                           # no Module table
            return None

        # rows array: one uint32 per set bit in `valid`
        n_tables = bin(valid).count("1")
        rows_at = tbl + 24
        # Module is table 0, so it is the first row count and the first table
        data_at = rows_at + 4 * n_tables

        # Module row: Generation(2) Name(str) Mvid(guid) EncId(guid) EncBaseId(guid)
        name_at = data_at + 2
        if str_wide:
            idx = struct.unpack_from("<I", blob, name_at)[0]
        else:
            idx = struct.unpack_from("<H", blob, name_at)[0]
        _ = guid_wide

        e = blob.index(b"\0", str_off + idx)
        name = blob[str_off + idx:e].decode("utf-8", "replace")
        if not name or "/" in name or "\\" in name:
            return None
        return name if name.lower().endswith(".dll") else name + ".dll"
    except (struct.error, ValueError, IndexError):
        return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    src, outdir = sys.argv[1], sys.argv[2]
    wanted = {w.lower() for w in sys.argv[3:]}

    data = open(src, "rb").read()
    os.makedirs(outdir, exist_ok=True)

    # Where the store data actually ends.
    #
    # The last entry's compressed block runs to the end of the ELF "payload"
    # section, NOT to the end of the file -- the section headers sit after it.
    # Feeding those extra bytes to LZ4 makes the final assembly fail to
    # decompress, which looks like a truncated or corrupt store and is neither.
    limit = payload_end(data) or len(data)

    offsets = [m.start() for m in re.finditer(b"XALZ", data)]
    print(f"XALZ entries: {len(offsets)}")

    seen, extracted, failed = set(), 0, 0

    for n, off in enumerate(offsets):
        _, idx, usize = struct.unpack_from("<4sII", data, off)
        start = off + 12
        # Each block runs to the next XALZ, or to end of file for the last.
        end = offsets[n + 1] if n + 1 < len(offsets) else limit

        try:
            blob = lz4.block.decompress(data[start:end], uncompressed_size=usize)
        except Exception:
            failed += 1
            continue

        name = assembly_name(blob) or f"assembly_{idx:03d}.dll"
        if name in seen:
            name = f"{name[:-4]}_{idx:03d}.dll"
        seen.add(name)

        if wanted and name.lower() not in wanted:
            continue

        with open(os.path.join(outdir, name), "wb") as f:
            f.write(blob)
        extracted += 1

    print(f"decompressed: {len(offsets) - failed}   failed: {failed}")
    print(f"written to {outdir}: {extracted}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
