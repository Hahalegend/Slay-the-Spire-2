#!/usr/bin/env python3
"""bionic_abi.py -- imports that cross a struct-layout boundary.

The engine is compiled against BIONIC. Every shim is compiled against NEWLIB.
Where a function takes or returns a pointer to a struct the two libcs lay out
differently, passing newlib's version straight through hands the caller a
struct whose fields are at the wrong offsets.

That failure is quiet. Nothing crashes at the call; a field reads as something
else and the damage surfaces later, somewhere unrelated. Four of these have
already been found the expensive way -- one hardware run each:

    pthread_attr_t   newlib's layout written into bionic's 56-byte buffer
    sigaction        bionic puts sa_flags FIRST; the shim read the handler
                     from offset 0 and got the flags
    struct tm        tm_gmtoff/tm_zone left as whatever was on the stack
    environ          a snapshot, not &environ, so setenv's realloc dangled it

The pattern in all four: the shim was written from the newlib header on the
build machine, and bionic disagreed.

This lists every import whose signature touches one of those types, and whether
the port has a translation struct for it. It cannot verify a translation is
CORRECT -- only that one exists, or that its absence is deliberate.

Usage:  ./tools/bionic_abi.py source
"""

import re
import sys
import pathlib

# Types whose LP64 layout differs between bionic and newlib, or where the
# difference has already bitten. Value: (bionic shape, why it matters).
CROSSING = {
    "dirent": ("d_ino@0 d_off@8 d_reclen@16 d_type@18 d_name@19 (280 B)",
               "newlib has no d_off and puts d_name at +9"),
    "stat": ("st_dev@0 ... st_size@48 st_blocks@64 (128 B)",
             "field order and padding differ"),
    "tm": ("+tm_gmtoff@40 +tm_zone@48",
           "newlib's struct tm ends at tm_isdst; the extra two fields are "
           "read by bionic callers"),
    "sigaction": ("sa_flags@0 sa_handler@8",
                  "bionic puts the FLAGS first; glibc/newlib put the handler "
                  "first"),
    "pthread_attr_t": ("56 B opaque",
                       "newlib's is a different size and shape"),
    "pthread_mutex_t": ("40 B opaque", "newlib's is smaller"),
    "pthread_cond_t": ("8 B opaque",
                       "exactly 8 on LP64 -- a lazy-slot pointer fits with "
                       "NO margin"),
    "pthread_rwlock_t": ("56 B opaque", "newlib's is smaller"),
    "sem_t": ("16 B opaque", "newlib's is a pointer"),
    "jmp_buf": ("32 longs = 256 B",
                "newlib aarch64 uses 22 (_JBLEN) = 176 B; safe only while the "
                "SAME implementation does both setjmp and longjmp"),
    "FILE": ("opaque, different size",
             "never pass a newlib FILE* to a bionic caller"),
    "DIR": ("opaque", "same"),
    "statvfs": ("f_bsize@0 ... f_namemax@88", "field widths differ"),
    "rusage": ("2x timeval + 14 longs = 144 B", "we memset it"),
    "sysinfo": ("112 B", "we memset it"),
}

# Imports that touch one of the above. Each maps to the type it crosses.
WATCH = {
    "readdir": "dirent", "readdir_r": "dirent", "scandir": "dirent",
    "stat": "stat", "fstat": "stat", "lstat": "stat",
    "stat64": "stat", "fstat64": "stat", "lstat64": "stat", "fstatat": "stat",
    "localtime": "tm", "localtime_r": "tm", "gmtime": "tm", "gmtime_r": "tm",
    "mktime": "tm", "timegm": "tm", "strftime": "tm",
    "sigaction": "sigaction", "sigprocmask": "sigaction",
    "pthread_attr_init": "pthread_attr_t",
    "pthread_attr_destroy": "pthread_attr_t",
    "pthread_attr_setstacksize": "pthread_attr_t",
    "pthread_attr_getstack": "pthread_attr_t",
    "pthread_attr_setdetachstate": "pthread_attr_t",
    "pthread_getattr_np": "pthread_attr_t",
    "pthread_create": "pthread_attr_t",
    "pthread_mutex_init": "pthread_mutex_t",
    "pthread_cond_init": "pthread_cond_t",
    "pthread_cond_wait": "pthread_cond_t",
    "pthread_cond_timedwait": "pthread_cond_t",
    "pthread_rwlock_rdlock": "pthread_rwlock_t",
    "pthread_rwlock_wrlock": "pthread_rwlock_t",
    "sem_init": "sem_t", "sem_wait": "sem_t", "sem_post": "sem_t",
    "sem_destroy": "sem_t",
    "setjmp": "jmp_buf", "longjmp": "jmp_buf",
    "_setjmp": "jmp_buf", "_longjmp": "jmp_buf", "siglongjmp": "jmp_buf",
    "fopen": "FILE", "fclose": "FILE", "fread": "FILE", "fwrite": "FILE",
    "opendir": "DIR", "closedir": "DIR", "fdopendir": "DIR",
    "statvfs": "statvfs", "fstatvfs": "statvfs",
    "getrusage": "rusage", "sysinfo": "sysinfo",
}


# Pass-throughs checked by hand against bionic's aarch64 headers and found
# safe. Listed rather than removed from WATCH, so the reasoning is recorded and
# a future change to the shim re-opens the question rather than hiding it.
VERIFIED = {
    "getrusage": "we memset 144 B; bionic's struct rusage is 2 timevals + 14 "
                 "longs = 144. Exact.",
    "sysinfo":   "we memset 112 B; bionic's struct sysinfo is 112 on LP64. "
                 "Exact.",
    "setjmp":    "newlib writes 176 B into bionic's 256-byte jmp_buf -- less "
                 "than allocated, so safe. Only holds while setjmp AND longjmp "
                 "are both newlib's; mixing them would be silent breakage.",
    "longjmp":   "pairs with setjmp above.",
}


def main(src_dir):
    src = pathlib.Path(src_dir)
    imports_c = src / "imports.c"
    if not imports_c.exists():
        print(f"no imports.c under {src}")
        return 1

    body = "\n".join(p.read_text(errors="replace") for p in src.glob("*.c"))
    entry = re.compile(r'\{\s*"([a-z_0-9]+)"\s*,\s*\(uintptr_t\)&\s*([A-Za-z_0-9]+)\s*\}')
    table = {m.group(1): m.group(2) for m in entry.finditer(body)}

    # A translation exists if the port defines a bionic_<type> struct, or the
    # shim's own name marks it as translated.
    translated = set()
    for t in CROSSING:
        if re.search(r'\bstruct\s+bionic_' + re.escape(t) + r'\b', body) or \
           re.search(r'\bbionic_' + re.escape(t) + r'\b', body):
            translated.add(t)

    print(f"# {len(table)} imports; {len(translated)} bionic translations found:"
          f" {', '.join(sorted(translated)) or 'none'}\n")

    passthrough = []
    for name, target in sorted(table.items()):
        t = WATCH.get(name)
        if not t:
            continue
        # A shim of any kind is at least a place where the difference was seen.
        shimmed = target.endswith(("_fake", "_shim")) or target.startswith("sts2_")
        inert = target in ("ret0", "retm1", "proc_enosys", "net_err3",
                           "net_errsz", "net_null", "net_0")
        if inert:
            continue
        if name in VERIFIED:
            continue
        if t in translated or shimmed:
            continue
        passthrough.append((name, target, t))

    if passthrough:
        print("PASS-THROUGH across a differing layout:\n")
        for name, target, t in passthrough:
            shape, why = CROSSING[t]
            print(f"  {name}() -> {target}")
            print(f"      crosses {t}: {shape}")
            print(f"      {why}\n")
    else:
        print("no unshimmed pass-throughs across a differing layout")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "source"))
