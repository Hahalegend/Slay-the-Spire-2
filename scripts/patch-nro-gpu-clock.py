#!/usr/bin/env python3
"""Patch prebuilt sts2.nro (v1.0.0) so cpu_boost() returns immediately.

ApmCpuBoostMode_FastLoad raises CPU to 1785 MHz while hard-throttling the
Switch GPU down to 76.8 MHz. On Godot Vulkan this starves rendering during
both boot and gameplay (locking frames around ~10 FPS). Patching cpu_boost()
at file offset 0xd8c90 to `ret` (0xd65f03c0) keeps full GPU clocks active
and allows sys-clk to manage clocks freely.

Usage:
    python scripts/patch-nro-gpu-clock.py path/to/sts2.nro
"""
import shutil
import struct
import sys
from pathlib import Path

CPU_BOOST_OFFSET = 0x0D8C90
EXPECTED_ORIG = 0x7100001F  # cmp w0, #0
PATCHED_RET = 0xD65F03C0    # ret


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python scripts/patch-nro-gpu-clock.py <path/to/sts2.nro>")
        return 2

    nro = Path(sys.argv[1])
    if not nro.is_file():
        print(f"Error: file not found: {nro}")
        return 1

    data = bytearray(nro.read_bytes())
    if len(data) <= CPU_BOOST_OFFSET + 4:
        print("Error: file is smaller than expected for sts2.nro v1.0.0")
        return 1

    cur = struct.unpack_from("<I", data, CPU_BOOST_OFFSET)[0]
    if cur == PATCHED_RET:
        print(f"Already patched at 0x{CPU_BOOST_OFFSET:x} (ret).")
        return 0
    if cur != EXPECTED_ORIG:
        print(
            f"Error: unexpected instruction at 0x{CPU_BOOST_OFFSET:x}: "
            f"0x{cur:08x} (expected 0x{EXPECTED_ORIG:08x}). "
            "This script targets prebuilt sts2.nro v1.0.0; if building from source, "
            "source/util.c already includes this fix."
        )
        return 1

    backup = nro.with_suffix(nro.suffix + ".orig")
    if not backup.exists():
        shutil.copy2(nro, backup)
        print(f"Saved backup: {backup}")

    struct.pack_into("<I", data, CPU_BOOST_OFFSET, PATCHED_RET)
    nro.write_bytes(data)
    print(f"Patched {nro} at 0x{CPU_BOOST_OFFSET:x}: 0x{cur:08x} -> 0x{PATCHED_RET:08x} (ret)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
