#!/usr/bin/env python3
"""check_aot_so.py -- verify libsts2.so is the right kind of file.

ILC succeeding and producing a .so is not the same as producing a USABLE one.
Two things decide whether the engine can do anything with it, and neither is
visible from the build log:

  architecture   the RID is linux-bionic-arm64, but a misconfigured build can
                 emit a host-architecture binary that loads nowhere near a
                 Switch.

  exports        the engine dlopen()s the payload and dlsym()s
                 godotsharp_game_main_init. NativeLib=Shared is what makes ILC
                 export [UnmanagedCallersOnly(EntryPoint=...)] methods; without
                 it you get a valid .so with none of them, and the failure
                 appears on hardware as "the game does nothing" with no log.

Parses the ELF directly rather than shelling out to readelf, so it runs
anywhere Python does.

  usage: check_aot_so.py <libsts2.so>
"""

import struct
import sys

EM_AARCH64 = 183
ET_DYN = 3

# Symbols the engine looks for. The first is mandatory; the rest are the wider
# GodotSharp bootstrap surface and are informational.
REQUIRED = ["godotsharp_game_main_init"]
INTERESTING = [
    "godotsharp_game_main_init",
    "godotsharp_editor_main_init",
    "godotsharp_dllimport_resolver",
]


def read_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not an ELF file")
    if data[4] != 2:
        raise SystemExit(f"{path}: not ELF64")

    e_type = struct.unpack_from("<H", data, 0x10)[0]
    e_machine = struct.unpack_from("<H", data, 0x12)[0]
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    def sh(i):
        return e_shoff + i * e_shentsize

    shstr_off = struct.unpack_from("<Q", data, sh(e_shstrndx) + 0x18)[0]

    sections = {}
    for i in range(e_shnum):
        base = sh(i)
        name_idx = struct.unpack_from("<I", data, base)[0]
        end = data.index(b"\0", shstr_off + name_idx)
        name = data[shstr_off + name_idx:end].decode()
        sections[name] = {
            "type":   struct.unpack_from("<I", data, base + 0x04)[0],
            "offset": struct.unpack_from("<Q", data, base + 0x18)[0],
            "size":   struct.unpack_from("<Q", data, base + 0x20)[0],
            "link":   struct.unpack_from("<I", data, base + 0x28)[0],
            "entsize":struct.unpack_from("<Q", data, base + 0x38)[0],
        }

    return data, e_type, e_machine, sections, sh


def dynamic_symbols(data, sections, sh):
    """Defined (non-UND) symbols from .dynsym."""
    if ".dynsym" not in sections or ".dynstr" not in sections:
        return []

    sym = sections[".dynsym"]
    strtab = sections[".dynstr"]
    out = []

    count = sym["size"] // 24 if sym["entsize"] == 0 else sym["size"] // sym["entsize"]
    for i in range(count):
        base = sym["offset"] + i * 24
        st_name = struct.unpack_from("<I", data, base)[0]
        st_info = data[base + 4]
        st_shndx = struct.unpack_from("<H", data, base + 6)[0]
        if st_name == 0 or st_shndx == 0:      # unnamed or undefined
            continue
        end = data.index(b"\0", strtab["offset"] + st_name)
        name = data[strtab["offset"] + st_name:end].decode("utf-8", "replace")
        out.append((name, st_info & 0xF))
    return out


def load_span(data):
    """Contiguous virtual span of the LOAD segments, and their total file size."""
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]

    lo, hi, filesz = None, 0, 0
    for i in range(e_phnum):
        b = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, b)[0] != 1:      # PT_LOAD
            continue
        vaddr = struct.unpack_from("<Q", data, b + 0x10)[0]
        fsz = struct.unpack_from("<Q", data, b + 0x20)[0]
        msz = struct.unpack_from("<Q", data, b + 0x28)[0]
        lo = vaddr if lo is None else min(lo, vaddr)
        hi = max(hi, vaddr + msz)
        filesz += fsz
    return (hi - (lo or 0)), filesz


def needed_libs(data, sections):
    if ".dynamic" not in sections or ".dynstr" not in sections:
        return []
    dyn = sections[".dynamic"]
    strtab = sections[".dynstr"]
    libs = []
    for i in range(dyn["size"] // 16):
        base = dyn["offset"] + i * 16
        tag, val = struct.unpack_from("<qQ", data, base)
        if tag == 0:
            break
        if tag == 1:                            # DT_NEEDED
            end = data.index(b"\0", strtab["offset"] + val)
            libs.append(data[strtab["offset"] + val:end].decode())
    return libs


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = sys.argv[1]
    data, e_type, e_machine, sections, sh = read_elf(path)

    print(f"file      : {path}")
    print(f"size      : {len(data):,} bytes")

    ok = True

    arch = "aarch64" if e_machine == EM_AARCH64 else f"UNEXPECTED (e_machine={e_machine})"
    print(f"arch      : {arch}")
    if e_machine != EM_AARCH64:
        print("            ^ the Switch is aarch64. Check RuntimeIdentifier is")
        print("              linux-bionic-arm64, not the host RID.")
        ok = False

    kind = "shared object" if e_type == ET_DYN else f"UNEXPECTED (e_type={e_type})"
    print(f"type      : {kind}")
    if e_type != ET_DYN:
        print("            ^ expected a shared object. Check NativeLib=Shared")
        print("              in aot/sts2.aot.csproj.")
        ok = False

    # LOAD span is what the wrapper must reserve; file size is what so_load
    # transiently mallocs. They are very different numbers and both matter.
    span, filesz = load_span(data)
    print(f"LOAD span : {span/1048576:.1f} MB   (wrapper DOTNET_SO_SLICE must exceed this)")
    print(f"mapped    : {filesz/1048576:.1f} MB of file, {(len(data)-filesz)/1048576:.1f} MB debug/symbols")
    if span > 160 * 1048576:
        print("            ^ EXCEEDS the 160 MB DOTNET_SO_SLICE in source/main.c.")
        print("              Raise it and SO_HEAP_RESERVE to match.")
        ok = False

    libs = needed_libs(data, sections)
    print(f"NEEDED    : {', '.join(libs) if libs else '(none)'}")

    syms = dynamic_symbols(data, sections, sh)
    names = {n for n, _ in syms}
    print(f"exports   : {len(names)} defined dynamic symbols")

    print()
    for want in INTERESTING:
        mark = "OK  " if want in names else "--  "
        print(f"  {mark}{want}")

    print()
    missing = [r for r in REQUIRED if r not in names]
    if missing:
        ok = False
        print("MISSING REQUIRED EXPORTS:", ", ".join(missing))
        print()
        print("The engine dlsym()s godotsharp_game_main_init after loading this")
        print("library. Without it the payload loads and nothing happens, with")
        print("nothing in the log to say why.")
        print()
        print("Most likely causes, in order:")
        print("  1. NativeLib=Shared missing from aot/sts2.aot.csproj -- ILC only")
        print("     emits [UnmanagedCallersOnly] exports for a shared library.")
        print("  2. GodotSharp not rooted, so the bootstrap was trimmed away.")
        print("     Check the generated rd.generated.xml names it.")
        # Show what DID get exported, as a hint.
        candidates = sorted(n for n in names if "godot" in n.lower() or "sharp" in n.lower())
        if candidates:
            print()
            print("  Godot-ish symbols that ARE exported:")
            for c in candidates[:20]:
                print(f"    {c}")

    print()
    print("RESULT: usable" if ok else "RESULT: NOT usable")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
