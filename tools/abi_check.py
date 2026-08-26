#!/usr/bin/env python3
"""abi_check.py -- catch return-width mismatches in the import table.

On AArch64 a function declared to return `int` writes only w0; the upper half
of x0 is left holding whatever was there. If the real libc function returns a
64-bit type and the caller reads all 64 bits, it gets

    garbage << 32 | our_value

which is why `if (n < 0)` can be false for a shim that returned -1, and why a
shim returning 1 can be read as a pointer like 0x0000003400000001.

Five crashes in this port have had exactly that shape -- a small intended value
in the low word with unrelated bytes above it -- across three different modules.
That does not prove a width mismatch caused any of them, but it is the mechanism
that produces the shape, so the table is worth checking mechanically instead of
by eye.

Found on its first run: send, sendto, recv and recvfrom all pointed at a stub
returning `int` where libc returns `ssize_t`.
"""

import pathlib
import re
import sys

# Real return type on LP64, for functions where it is 64-bit.
WIDE = {
    "read": "ssize_t", "write": "ssize_t", "pread": "ssize_t", "pwrite": "ssize_t",
    "readv": "ssize_t", "writev": "ssize_t", "recv": "ssize_t", "send": "ssize_t",
    "recvfrom": "ssize_t", "sendto": "ssize_t", "recvmsg": "ssize_t",
    "sendmsg": "ssize_t", "lseek": "off_t", "lseek64": "off64_t",
    "ftell": "long", "ftello": "off_t", "sysconf": "long", "pathconf": "long",
    "fpathconf": "long", "strtol": "long", "strtoul": "unsigned long",
    "strtoll": "long long", "strtoull": "unsigned long long", "labs": "long",
    "time": "time_t", "mktime": "time_t", "fread": "size_t", "fwrite": "size_t",
    "strlen": "size_t", "strnlen": "size_t", "strspn": "size_t",
    "strcspn": "size_t", "confstr": "size_t", "malloc_usable_size": "size_t",
    "getauxval": "unsigned long", "telldir": "long", "random": "long",
}

# Functions returning a pointer. A 32-bit shim here is the same bug, worse.
PTR = {
    "malloc", "calloc", "realloc", "memalign", "aligned_alloc", "strdup",
    "strndup", "memcpy", "memmove", "memset", "memchr", "strcpy", "strncpy",
    "strcat", "strncat", "strchr", "strrchr", "strstr", "strpbrk", "strtok",
    "strtok_r", "getenv", "setlocale", "fopen", "fdopen", "freopen", "opendir",
    "fdopendir", "readdir", "dlopen", "dlsym", "dlerror", "mmap", "sbrk",
    "signal", "bsearch", "gmtime", "localtime", "strerror", "getcwd",
    "realpath", "fgets", "__errno", "__errno_location", "pthread_getspecific",
    "stpcpy", "stpncpy", "memrchr",
}

NARROW = {"int", "unsigned", "unsigned int", "uint32_t", "int32_t", "short",
          "char", "signed int", "_Bool", "bool"}


def main():
    src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "source")
    raw = {f.name: f.read_text(errors="replace") for f in src.glob("*.c")}

    entries, rettype = [], {}
    for name, body in raw.items():
        for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*\(uintptr_t\)\s*&?(\w+)\s*\}', body):
            entries.append((m.group(1), m.group(2), name))
        for m in re.finditer(
                r'^\s*(?:static\s+)?((?:const\s+|unsigned\s+|signed\s+)*'
                r'[A-Za-z_]\w*(?:\s*\*+)?)\s+(\w+)\s*\([^;{]*\)\s*\{', body, re.M):
            rettype[m.group(2)] = " ".join(m.group(1).split())

    problems = 0
    for name, shim, where in sorted(set(entries)):
        rt = rettype.get(shim)
        if rt is None:            # points at the real libc function
            continue
        if name in PTR and "*" not in rt:
            print(f"{where}: {name} -> {shim} returns '{rt}', libc returns a pointer")
            problems += 1
        elif name in WIDE and rt in NARROW:
            print(f"{where}: {name} -> {shim} returns '{rt}', "
                  f"libc returns '{WIDE[name]}' (64-bit)")
            problems += 1

    print(f"{len(set(entries))} import entries checked; "
          + ("no return-width mismatches" if not problems else f"{problems} mismatch(es)"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
