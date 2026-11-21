#!/usr/bin/env python3
"""
patch-nro-cond-init.py -- Binary-patch prebuilt sts2.nro v1.0.0 so
pthread_cond_init_fake() clears its uninitialized slot before calling
ensure_cond_clocked().

Root cause (symbolized from Switch crash reports 01795266119 & 01795266430):
  .NET's SystemNative_LowLevelMonitor_Create allocates 88 bytes via malloc()
  (uninitialized!) and calls pthread_cond_init(&monitor->Condition, &attr).
  With poison_free=0, malloc() frequently returns memory containing a stale
  8-byte-aligned heap pointer at offset +0x28. In prebuilt sts2.nro v1.0.0,
  pthread_cond_init_fake() called ensure_cond_clocked() without zeroing
  *lazy_slot(cnd) first, so looks_like_our_heap_ptr() treated the stale heap
  pointer as an already-initialized condvar -- corrupting live heap memory on
  cond_wait/signal and crashing in free() on SystemNative_LowLevelMonitor_Destroy
  (ThreadWaitInfo.Finalize -> pthread_cond_destroy_fake -> sts2+0xbeb50).

Patch (16 bytes inside pthread_cond_init_fake @ 0xbf0e0..0xbf0ec):
  Replaces the 4 diagnostic LOCK_CALLER_HERE() instructions with:
    cbz  x21, +16               (0xb4000095)  ; if (!cnd) skip
    add  x0, x21, #7            (0x91001ea0)  ; (uintptr_t)cnd + 7
    and  x0, x0, #-8            (0x927df000)  ; lazy_slot(cnd)
    stlr xzr, [x0]              (0xc89ffc1f)  ; atomic release-store 0
  so ensure_cond_clocked() always sees 0 and allocates a fresh condvar with the
  requested clock_id.

Usage:
  python scripts/patch-nro-cond-init.py path/to/sts2.nro
"""

import struct
import sys
from pathlib import Path

# Original 4 LOCK_CALLER_HERE() instructions inside pthread_cond_init_fake:
#   mrs  x0, TPIDRRO_EL0   (0xd53bd060)
#   ubfx x0, x0, #8, #6    (0xd3483400)
#   add  x0, x2, x0, lsl#3 (0x8b000c40)
#   str  x30, [x0]         (0xf900001e)
ORIG_INSNS = struct.pack("<IIII", 0xD53BD060, 0xD3483400, 0x8B000C40, 0xF900001E)

# Replacement 4 instructions: zero *lazy_slot(cnd) atomically if cnd != NULL.
#   cbz  x21, +16          (0xb4000095)
#   add  x0, x21, #7       (0x91001ea0)
#   and  x0, x0, #-8       (0x927df000)
#   stlr xzr, [x0]         (0xc89ffc1f)
PATCH_INSNS = struct.pack("<IIII", 0xB4000095, 0x91001EA0, 0x927DF000, 0xC89FFC1F)


def find_pthread_cond_init_fake(data: bytes) -> int:
    s_off = data.find(b"pthread_cond_init\x00", 0xC20000)
    if s_off == -1:
        raise RuntimeError("Could not find 'pthread_cond_init' string in NRO .rodata")
    t_off = data.find(struct.pack("<Q", s_off), 0x16C8000)
    if t_off == -1:
        raise RuntimeError("Could not find 'pthread_cond_init' entry in NRO import table")
    fn_addr = struct.unpack_from("<Q", data, t_off + 8)[0]
    return int(fn_addr)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python scripts/patch-nro-cond-init.py <path/to/sts2.nro>", file=sys.stderr)
        return 2

    nro_path = Path(sys.argv[1])
    data = bytearray(nro_path.read_bytes())
    if data[0x10:0x14] != b"NRO0":
        raise RuntimeError(f"{nro_path} is not a valid NRO0 binary")

    fn_addr = find_pthread_cond_init_fake(data)
    patch_off = fn_addr + 0x20  # 0xbf0c0 + 0x20 = 0xbf0e0
    cur = bytes(data[patch_off : patch_off + 16])

    if cur == PATCH_INSNS:
        print(f"[OK] {nro_path} already patched at 0x{patch_off:x} (pthread_cond_init_fake @ 0x{fn_addr:x})")
        return 0

    if cur != ORIG_INSNS:
        raise RuntimeError(
            f"Unexpected bytes at 0x{patch_off:x} (pthread_cond_init_fake @ 0x{fn_addr:x}): "
            f"got {cur.hex()}, expected {ORIG_INSNS.hex()}"
        )

    data[patch_off : patch_off + 16] = PATCH_INSNS
    nro_path.write_bytes(data)
    print(f"[OK] Patched pthread_cond_init_fake @ 0x{fn_addr:x} (zero-slot at 0x{patch_off:x}) in {nro_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
