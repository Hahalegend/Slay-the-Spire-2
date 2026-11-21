#!/usr/bin/env python3
"""patch_aot_threadpool.py -- Prevent .NET 9 PortableThreadPool worker exit leaks on Switch.

On devkitPro/libnx, pthread_detach is unimplemented (ENOSYS) and threadClose()
is only invoked by pthread_join(). Because .NET 9's PortableThreadPool creates
detached worker threads that time out after 20 seconds of inactivity
(DetermineThreadPoolThreadTimeoutMs = 20000, DetermineThreadsToKeepAlive = 0),
every 20-second idle cycle exits all worker threads without joining them,
leaking 1 MB stack + 64 KB TLS + 1 Horizon kernel Thread handle & TLS slot per
worker until pthread_create fails after ~50 minutes (crash 01795290236).

This post-link binary patcher resolves the three PortableThreadPool worker-exit
policy methods from libsts2.so.dbg and patches them in libsts2.so:
  - DetermineThreadPoolThreadTimeoutMs() -> return -1 (Timeout.Infinite)
  - DetermineThreadsToKeepAlive()        -> return -1 (keep all workers alive)
  - WorkerThread.ShouldExitWorker()      -> return false (0)

Paired with ThreadPool.SetMinThreads(4, 4) / SetMaxThreads(4, 4) in
SwitchCulture.cs, the pool keeps at most 4 worker threads alive for the entire
session with zero thread-exit leaks and zero mid-gameplay pthread_create churn.
"""

import struct
import sys

TARGETS = {
    "S_P_CoreLib_System_Threading_PortableThreadPool__DetermineThreadPoolThreadTimeoutMs": (
        0x12800000,  # mov w0, #-1
        0xD65F03C0,  # ret
        "return -1 (Timeout.Infinite)",
    ),
    "S_P_CoreLib_System_Threading_PortableThreadPool_WorkerThread__DetermineThreadsToKeepAlive": (
        0x12800000,  # mov w0, #-1
        0xD65F03C0,  # ret
        "return -1 (keep all workers alive)",
    ),
    "S_P_CoreLib_System_Threading_PortableThreadPool_WorkerThread__ShouldExitWorker": (
        0x52800000,  # mov w0, #0
        0xD65F03C0,  # ret
        "return false (never exit worker)",
    ),
}


def find_symbols(dbg_path):
    with open(dbg_path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{dbg_path}: not an ELF64 file")

    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    shstr_off = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

    symtab_hdr = None
    for i in range(e_shnum):
        sh = data[e_shoff + i * e_shentsize : e_shoff + (i + 1) * e_shentsize]
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from(
            "<IIQQQQI", sh, 0
        )
        name = data[shstr_off + sh_name : shstr_off + sh_name + 64].split(b"\x00")[0]
        if name == b".symtab":
            symtab_hdr = (sh_offset, sh_size, sh_link)
            break

    if not symtab_hdr:
        raise SystemExit(f"{dbg_path}: missing .symtab section")

    sym_off, sym_sz, str_idx = symtab_hdr
    sh_str = data[e_shoff + str_idx * e_shentsize : e_shoff + (str_idx + 1) * e_shentsize]
    str_off, _ = struct.unpack_from("<QQ", sh_str, 0x18)

    found = {}
    for off in range(sym_off, sym_off + sym_sz, 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, off
        )
        if st_value == 0:
            continue
        end = data.find(b"\x00", str_off + st_name)
        name = data[str_off + st_name : end].decode("utf-8", "replace")
        if name in TARGETS:
            found[name] = (st_value, st_size)

    return found


def vaddr_to_file_offset(so_bytes, vaddr):
    e_phoff = struct.unpack_from("<Q", so_bytes, 0x20)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", so_bytes, 0x36)
    for i in range(e_phnum):
        ph = so_bytes[e_phoff + i * e_phentsize : e_phoff + (i + 1) * e_phentsize]
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<IIQQQQQ", ph, 0
        )
        if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    return None


def patch_so(so_path, syms):
    with open(so_path, "rb") as f:
        so = bytearray(f.read())

    for name, (insn0, insn1, desc) in TARGETS.items():
        if name not in syms:
            raise SystemExit(f"Symbol {name} not found in .dbg file")
        vaddr, sz = syms[name]
        if sz < 8:
            raise SystemExit(f"Symbol {name} size {sz} < 8 bytes")
        foff = vaddr_to_file_offset(so, vaddr)
        if foff is None:
            raise SystemExit(f"Could not map vaddr 0x{vaddr:x} for {name} in {so_path}")

        cur0, cur1 = struct.unpack_from("<II", so, foff)
        struct.pack_into("<II", so, foff, insn0, insn1)
        print(
            f"  [patched] {name} @ vaddr=0x{vaddr:x} (file+0x{foff:x}): "
            f"{cur0:08x} {cur1:08x} -> {insn0:08x} {insn1:08x} ({desc})"
        )

    with open(so_path, "wb") as f:
        f.write(so)


def main():
    if len(sys.argv) < 3:
        print("usage: patch_aot_threadpool.py <libsts2.so.dbg> <libsts2.so> [<libsts2_copy2.so> ...]")
        return 2

    dbg_path = sys.argv[1]
    so_paths = sys.argv[2:]
    syms = find_symbols(dbg_path)
    missing = [k for k in TARGETS if k not in syms]
    if missing:
        raise SystemExit(f"Missing symbols in {dbg_path}: {', '.join(missing)}")

    for sp in so_paths:
        print(f"Patching PortableThreadPool worker keep-alive in {sp}:")
        patch_so(sp, syms)

    return 0


if __name__ == "__main__":
    sys.exit(main())
