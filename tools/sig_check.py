#!/usr/bin/env python3
"""sig_check.py -- two more error classes a compiler would catch.

1. A prototype in a .h that disagrees with the definition in the .c.
   gcc reports "conflicting types for ...". Common when a function is edited in
   one place and not the other.

2. A call to a libc/libnx function whose system header is not included.
   Same "implicit declaration" error as decl_check.py finds for project
   headers, but for the system side. Only a curated set is checked -- the ones
   this port actually uses -- because guessing at all of libc would bury the
   real hits in noise.
"""

import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cstrip


def strip(t):
    return cstrip.strip(t)
import sys
import pathlib

SRC = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# --- 2. system symbol -> the header that declares it -------------------------
SYSTEM = {
    "malloc": "stdlib.h", "calloc": "stdlib.h", "realloc": "stdlib.h",
    "free": "stdlib.h", "atoi": "stdlib.h", "getenv": "stdlib.h",
    "setenv": "stdlib.h", "abort": "stdlib.h", "exit": "stdlib.h",
    "memalign": "malloc.h",
    "memcpy": "string.h", "memset": "string.h", "strcmp": "string.h",
    "strncmp": "string.h", "strlen": "string.h", "strdup": "string.h",
    "strchr": "string.h", "strrchr": "string.h", "strstr": "string.h",
    "strncpy": "string.h", "strcpy": "string.h",
    "printf": "stdio.h", "snprintf": "stdio.h", "fprintf": "stdio.h",
    "vsnprintf": "stdio.h", "fopen": "stdio.h", "fclose": "stdio.h",
    "fread": "stdio.h", "fwrite": "stdio.h", "fseek": "stdio.h",
    "fgets": "stdio.h", "fflush": "stdio.h", "fputs": "stdio.h",
    "va_start": "stdarg.h", "va_end": "stdarg.h", "va_arg": "stdarg.h",
    "mkdir": "sys/stat.h", "stat": "sys/stat.h",
    "opendir": "dirent.h", "readdir": "dirent.h", "closedir": "dirent.h",
    "getcwd": "unistd.h",
    "fabsf": "math.h", "sqrtf": "math.h", "floorf": "math.h",
    "isspace": "ctype.h", "isdigit": "ctype.h",
}

def _blank_keep_lines(m):
    # Replace a comment with the SAME number of newlines it spanned.
    #
    # Collapsing a 20-line block comment to one space shifts every line number
    # after it. Every message these tools have printed with a line number has
    # been wrong by however many comment lines preceded it -- which in this
    # codebase, where comments outnumber code, is a lot. Caught when a macro
    # ordering hit pointed at a line that had nothing to do with the macro.
    return "\n" * m.group(0).count("\n")


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', _blank_keep_lines, t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)

# --- 1. prototypes vs definitions -------------------------------------------
def norm(sig):
    """Collapse whitespace and pointer spacing so 'char *x' == 'char* x'."""
    sig = re.sub(r'\s+', ' ', sig).strip()
    sig = sig.replace(' *', '*').replace('* ', '*')
    # parameter names are irrelevant to the type; drop trailing identifiers
    parts = []
    for p in sig.split(','):
        p = p.strip()
        p = re.sub(r'\b[a-z_][A-Za-z0-9_]*$', '', p).strip()
        parts.append(p)
    return ','.join(parts)

protos = {}   # name -> (ret, params, header)
proto_re = re.compile(
    r'^\s*(?:extern\s+)?([A-Za-z_][A-Za-z0-9_ \*]*?)\b'
    r'([a-z_][A-Za-z0-9_]*)\s*\(([^;{]*)\)\s*;', re.M)

for h in sorted(SRC.glob("*.h")):
    for m in proto_re.finditer(strip(h.read_text(errors="replace"))):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
        protos[name] = (norm(ret), norm(params), h.name)

def_re = re.compile(
    r'^([A-Za-z_][A-Za-z0-9_ \*]*?)\b([a-z_][A-Za-z0-9_]*)\s*\(([^;{]*)\)\s*\{', re.M)

conflicts = 0
for c in sorted(SRC.glob("*.c")):
    body = strip(c.read_text(errors="replace"))
    for m in def_re.finditer(body):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
        if ret.startswith("static"): continue
        if name not in protos:       continue
        pret, pparams, ph = protos[name]
        if norm(ret) != pret or norm(params) != pparams:
            conflicts += 1
            print(f"SIGNATURE {c.name}: {name}")
            print(f"    {ph}: {pret} ( {pparams} )")
            print(f"    {c.name}: {norm(ret)} ( {norm(params)} )")

if not conflicts:
    print("no prototype/definition conflicts")
print()

# --- 2. system includes ------------------------------------------------------
missing_any = 0
for c in sorted(SRC.glob("*.c")):
    body = strip(c.read_text(errors="replace"))
    sys_inc = set(re.findall(r'#include\s+<([^>]+)>', body))
    # switch.h drags in a lot; treat it as covering libnx only
    need = {}
    for m in re.finditer(r'\b([a-z_][A-Za-z0-9_]*)\s*\(', body):
        s = m.group(1)
        if s in SYSTEM and SYSTEM[s] not in sys_inc:
            need.setdefault(SYSTEM[s], set()).add(s)
    if need:
        missing_any += 1
        print(f"{c.name}")
        for h, syms in sorted(need.items()):
            print(f"    needs <{h}>  -> {', '.join(sorted(syms))}")

if not missing_any:
    print("no missing system includes")
