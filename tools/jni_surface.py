#!/usr/bin/env python3
"""jni_surface.py -- work out what jni_fake.c actually has to answer.

There are two JNI directions and they need opposite treatment:

  Java -> native   `native` methods in Java, implemented in the .so and
                   exported as Java_<pkg>_<Class>_<method>. The wrapper CALLS
                   these (main.c resolves GodotLib.initialize/setup/step and
                   drives them directly). Nothing to implement.

  native -> Java   the engine calls GetMethodID/GetStaticMethodID on a Java
                   object and then Call*Method. These are what jni_fake.c must
                   IMPLEMENT. They are invisible in the dex's `native` flags --
                   they are ordinary Java methods -- so the only way to find
                   them is to intersect the dex method list with the string
                   table of the .so that looks them up.

Output is the second list, which is the one that costs work.
"""

import re
import subprocess
import sys
from collections import defaultdict

sys.path.insert(0, "/home/claude")
from dexparse import Dex, descriptor_to_jni, ACC_STATIC, ACC_NATIVE

# Classes whose Java side the wrapper has to stand in for. Everything else in
# the dex is either .NET-for-Android binding glue (irrelevant under NativeAOT)
# or Kotlin/AndroidX support the engine never touches.
INTERESTING = (
    "org/godotengine/godot",
    "org/fmod",
    "com/esotericsoftware",
    "com/game/sts2launcher",
)


def so_strings(path, minlen=3):
    """Printable ASCII runs from the whole file, like strings(1)."""
    with open(path, "rb") as f:
        data = f.read()
    return set(m.group().decode("ascii")
               for m in re.finditer(rb"[ -~]{%d,}" % minlen, data))


def so_dynsyms(path):
    """Exported dynamic symbols, via readelf if present."""
    try:
        out = subprocess.run(["readelf", "-W", "--dyn-syms", path],
                             capture_output=True, text=True, timeout=120).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return set()
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8:
            syms.add(parts[7])
    return syms


def main():
    dex_paths = [p for p in sys.argv[1:] if p.endswith(".dex")]
    so_path = [p for p in sys.argv[1:] if p.endswith(".so")]
    if not dex_paths or not so_path:
        print("usage: jni_surface.py <*.dex ...> <lib.so>")
        return 1
    so_path = so_path[0]

    # --- dex side: every method of every interesting class -------------------
    classes = {}
    for p in dex_paths:
        with open(p, "rb") as f:
            dex = Dex(f.read())
        for cname, cacc, methods in dex.classes():
            jni = descriptor_to_jni(cname)
            if jni.startswith(INTERESTING):
                classes[jni] = methods

    print(f"# {len(classes)} interesting classes in dex", file=sys.stderr)

    # --- so side ------------------------------------------------------------
    strings = so_strings(so_path)
    dynsyms = so_dynsyms(so_path)
    print(f"# {len(strings)} strings, {len(dynsyms)} dynsyms in {so_path}",
          file=sys.stderr)

    # Java -> native: exported bridge symbols. Informational.
    bridge = sorted(s for s in dynsyms if s.startswith("Java_"))

    # native -> Java: a class name appearing as a string means FindClass; a
    # method name + its exact signature both appearing means GetMethodID.
    # Requiring the signature too is what keeps this from matching every
    # common English word that happens to be a method name.
    needed = defaultdict(list)
    classes_referenced = []

    for cname, methods in sorted(classes.items()):
        if cname in strings:
            classes_referenced.append(cname)
        for mname, msig, macc in methods:
            if mname in ("<init>", "<clinit>"):
                continue
            if macc & ACC_NATIVE:
                continue  # other direction
            if mname in strings and msig in strings:
                kind = "static" if macc & ACC_STATIC else "instance"
                needed[cname].append((mname, msig, kind))

    print("=" * 72)
    print("JAVA -> NATIVE  (exported by the .so; the wrapper calls these)")
    print("=" * 72)
    for s in bridge:
        print(f"  {s}")
    print(f"  [{len(bridge)} total]")
    print()

    print("=" * 72)
    print("NATIVE -> JAVA  (jni_fake.c must implement these)")
    print("=" * 72)
    print(f"classes reached by FindClass: {len(classes_referenced)}")
    for c in classes_referenced:
        print(f"  {c}")
    print()

    total = 0
    for cname in sorted(needed):
        print(f"CLASS {cname}")
        for mname, msig, kind in sorted(needed[cname]):
            print(f"  {kind:8} {mname:36} {msig}")
            total += 1
        print()
    print(f"[{total} methods across {len(needed)} classes]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
