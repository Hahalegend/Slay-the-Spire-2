#!/usr/bin/env python3
"""import_gap.py -- what the wrapper does not yet provide.

so_resolve() taints unresolved imports so they trap with a name when hit. That
works, but it finds one gap per boot on hardware. This finds all of them at
once, before the first build.

For each module the port loads, take the undefined dynamic symbols and subtract
everything the wrapper can satisfy:

  - the DynLibFunction table in imports.c
  - the AAudio table in aaudio_shim.c
  - symbols exported by another module we load (libc++_shared, libfmod,
    libfmodstudio) -- so_resolve chains across loaded modules
  - newlib/libnx symbols that resolve at link time

The remainder is real work. Grouped by likely source so the list is triageable
rather than just long.
"""

import os
import pathlib
import re
import subprocess
import sys

_SRC = pathlib.Path(__file__).resolve().parent.parent / "source"
from collections import defaultdict


def und_symbols(path):
    out = subprocess.run(["readelf", "-W", "--dyn-syms", path],
                         capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 8 and p[6] == "UND" and p[7] not in ("", "Ndx"):
            syms.add(p[7].split("@")[0])
    return syms


def def_symbols(path):
    out = subprocess.run(["readelf", "-W", "--dyn-syms", path],
                         capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 8 and p[6] != "UND" and p[3] in ("FUNC", "OBJECT"):
            syms.add(p[7].split("@")[0])
    return syms


def table_symbols(src_paths):
    """Every "name" appearing in a DynLibFunction initialiser."""
    names = set()
    for p in src_paths:
        try:
            src = open(p).read()
        except FileNotFoundError:
            continue
        # { "symbol", (uintptr_t)&fn },
        names |= set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', src))
    return names


# Symbols the devkitA64 link satisfies directly: newlib libc/libm, libnx, and
# the GL/EGL entry points pulled from mesa. Matching by prefix/known-set rather
# than by parsing the toolchain, which is not present here.
LINK_PROVIDED_PREFIXES = (
    "gl", "egl", "EGL",
)

CATEGORIES = [
    ("android/ndk", ("AAsset", "ANative", "AConfiguration", "ALooper", "AInput",
                     "AMotionEvent", "AKeyEvent", "ASensor", "AHardwareBuffer",
                     "ATrace", "AChoreographer", "APerformanceHint",
                     "android_", "__android_")),
    ("jni",         ("JNI_", "Java_")),
    ("opensl",      ("slCreate", "SL_IID")),
    ("aaudio",      ("AAudio",)),
    ("camera/media",("ACamera", "AImage", "AMedia", "AMedia_")),
    ("c++ runtime", ("_ZSt", "_ZNSt", "_ZTV", "_ZTI", "_ZTS", "__cxa", "_Unwind",
                     "__gxx", "_ZdlPv", "_Znwm", "_Znam", "_ZdaPv")),
    ("pthread",     ("pthread_", "sem_", "__pthread")),
    ("bionic-isms", ("__system_property", "__android_log", "gettid", "tgkill",
                     "prctl", "getauxval", "__errno", "arc4random",
                     "android_get", "__cmsg", "__open_2", "__read_chk",
                     "__vsnprintf_chk", "__strlen_chk", "__memcpy_chk")),
    ("fmod",        ("FMOD_", "_ZN4FMOD")),
]


def categorise(sym):
    for name, prefixes in CATEGORIES:
        if sym.startswith(prefixes):
            return name
    return "other"


def main():
    if len(sys.argv) < 2:
        print("usage: import_gap.py <libdir>")
        return 1
    d = sys.argv[1].rstrip("/")

    # The NativeAOT payload is scanned too when present. Run 3 died on 26
    # unresolved imports in it, because this tool had only ever been pointed at
    # the APK -- and libsts2.so is built here, so it was never in the scan. It
    # is also the module that pulls the most POSIX by far: the .NET PAL targets
    # Linux and linux-bionic-arm64 narrows that less than one would hope.
    #
    # Pass the payload's directory as a second argument, or drop it next to the
    # APK tree. Scan it after EVERY rebuild -- ILC's import set changes with the
    # code it compiles.
    payload = os.environ.get("AOT_PAYLOAD", f"{d}/libsts2.so")

    modules = {
        "libgodot_android.so": f"{d}/lib/arm64-v8a/libgodot_android.so",
        "libGodotFmod.so":     f"{d}/lib/arm64-v8a/libGodotFmod.android.template_release.arm64.so",
        "libspine_godot.so":   f"{d}/lib/arm64-v8a/libspine_godot.android.template_release.arm64.so",
        "libfmod.so":          f"{d}/assets/native/libfmod.so",
        "libfmodstudio.so":    f"{d}/assets/native/libfmodstudio.so",
    }
    if os.path.exists(payload):
        modules["libsts2.so (AOT payload)"] = payload
    else:
        print(f"# NOTE: AOT payload not found at {payload}\n"
              f"#       set AOT_PAYLOAD to scan it -- it is the module most\n"
              f"#       likely to need table entries.", file=sys.stderr)
    cxx = f"{d}/lib/arm64-v8a/libc++_shared.so"

    provided = table_symbols([
        # Relative to THIS script, not an absolute path.
        #
        # These were hardcoded to /home/claude/sts2_nx/source/, which is where
        # the tree happened to live when the tool was written. Point it at a
        # checkout anywhere else and it silently reads nothing, reports every
        # covered symbol as missing, and the 27 false alarms look exactly like
        # a real regression.
        str(_SRC / "imports.c"),
        str(_SRC / "aaudio_shim.c"),
        str(_SRC / "gl_imports.inc"),
        str(_SRC / "libc_shim.c"),
        str(_SRC / "godot_shim.c"),
    ])
    print(f"# wrapper import tables provide {len(provided)} symbols", file=sys.stderr)

    # Modules resolve against each other, so their exports count as provided.
    inter_module = set()
    for p in list(modules.values()) + [cxx]:
        try:
            inter_module |= def_symbols(p)
        except Exception:
            pass
    print(f"# loaded modules export {len(inter_module)} symbols", file=sys.stderr)

    for label, path in modules.items():
        try:
            und = und_symbols(path)
        except Exception as e:
            print(f"\n## {label}: unreadable ({e})")
            continue

        missing = set()
        for s in und:
            if s in provided or s in inter_module:
                continue
            if s.startswith(LINK_PROVIDED_PREFIXES):
                continue
            missing.add(s)

        print(f"\n{'='*70}")
        print(f"{label}: {len(und)} undefined, {len(missing)} not obviously covered")
        print("=" * 70)

        by_cat = defaultdict(list)
        for s in sorted(missing):
            by_cat[categorise(s)].append(s)

        for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
            syms = by_cat[cat]
            print(f"\n  [{cat}] {len(syms)}")
            for s in syms[:40]:
                print(f"    {s}")
            if len(syms) > 40:
                print(f"    ... and {len(syms)-40} more")

    return 0


if __name__ == "__main__":
    sys.exit(main())
