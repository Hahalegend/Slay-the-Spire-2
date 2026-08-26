#!/usr/bin/env python3
"""decl_check.py -- find calls to project functions whose header is not included.

Not a compiler. It catches exactly one error class, which is the one that keeps
turning up when a file gains new code: calling a function declared in one of
this project's own headers without including that header. gcc reports it as
"implicit declaration of function", one file per build.

Method: build a map of {symbol -> header} from the project's .h files, then for
each .c work out which project headers it includes and flag any call to a
symbol whose home header is absent.

Deliberately ignores libc/libnx/GL symbols -- those come from system headers
this script does not parse, and guessing at them would bury the real hits.
"""

import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cstrip
import sys
import pathlib

SRC = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")


def check_cross_file_undeclared(src):
    header_syms = set()
    for h in src.glob("*.h"):
        body = cstrip.strip(h.read_text(errors="replace"))
        for m in re.finditer(r'\b([a-z_][A-Za-z0-9_]*)\s*\(', body):
            header_syms.add(m.group(1))

    defined_in = {}
    for c in sorted(src.glob("*.c")):
        body = cstrip.strip(c.read_text(errors="replace"))
        for m in re.finditer(
                r'^([A-Za-z_][A-Za-z0-9_ \*]*?)\b([a-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{',
                body, re.M):
            if m.group(1).strip().startswith("static"):
                continue
            defined_in[m.group(2)] = c.name

    problems = 0
    for c in sorted(src.glob("*.c")):
        body = cstrip.strip(c.read_text(errors="replace"))

        # A .c may declare an extern itself instead of pulling in a header --
        # imports.c does exactly that for the OpenSL entry points. That IS a
        # declaration, so it is not an implicit one, and flagging it made the
        # check cry wolf on its first real run.
        local_externs = set(re.findall(
            r'\bextern\b[^;{()]*?\b([a-z_][A-Za-z0-9_]*)\s*\(', body))

        for m in re.finditer(r'\b([a-z_][A-Za-z0-9_]{3,})\s*\(', body):
            name = m.group(1)
            home = defined_in.get(name)
            if not home or home == c.name:
                continue
            if name in header_syms or name in local_externs:
                continue
            line = body[:m.start()].count("\n") + 1
            print(f"{c.name}:{line}: {name}() is defined in {home} but declared "
                  f"in no header (implicit declaration)")
            problems += 1
            break
    return problems




# Symbols declared in each project header.
decl_re = re.compile(
    r'^\s*(?:extern\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_ \*]*?'
    r'\b([a-z_][A-Za-z0-9_]*)\s*\(', re.M)

home = {}          # symbol -> header filename
for h in sorted(SRC.glob("*.h")):
    text = h.read_text(errors="replace")
    # strip block comments so commented-out prototypes do not count
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//.*', '', text)
    for m in decl_re.finditer(text):
        sym = m.group(1)
        if sym in ("if", "for", "while", "switch", "return", "sizeof", "defined"):
            continue
        home.setdefault(sym, h.name)

print(f"# {len(home)} symbols declared across {len(list(SRC.glob('*.h')))} headers\n")

problems = 0
for c in sorted(SRC.glob("*.c")):
    text = c.read_text(errors="replace")
    body = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    body = re.sub(r'//.*', '', body)

    included = set(re.findall(r'#include\s+"([^"]+)"', body))
    # a .c implicitly has its own declarations
    own = c.with_suffix(".h").name

    # symbols defined in this .c itself do not need a header
    defined_here = set(re.findall(
        r'^\s*(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_ \*]*?'
        r'\b([a-z_][A-Za-z0-9_]*)\s*\([^;]*?\)\s*\{', body, re.M))

    missing = {}
    for m in re.finditer(r'\b([a-z_][A-Za-z0-9_]*)\s*\(', body):
        sym = m.group(1)
        if sym not in home:            continue
        if sym in defined_here:        continue
        h = home[sym]
        if h in included or h == own:  continue
        missing.setdefault(h, set()).add(sym)

    if missing:
        problems += 1
        print(f"{c.name}")
        for h, syms in sorted(missing.items()):
            print(f'    needs #include "{h}"  -> {", ".join(sorted(syms))}')

if not problems:
    print("no missing project includes")

# The cross-file check runs BEFORE the exit, not after it. Appending a second
# __main__ block below sys.exit(0) meant it never executed -- it reported
# "clean" by never running at all, which is the worst kind of green.
problems += check_cross_file_undeclared(SRC)

# Unterminated string literals. Cheap, and a compile error every time.
_str_bad = 0
for _f in sorted(list(SRC.glob("*.c")) + list(SRC.glob("*.h"))):
    for _ln, _txt in cstrip.find_unterminated_strings(_f.read_text(errors="replace")):
        print(f"{_f.name}:{_ln}: unterminated string literal: {_txt[:70]}")
        _str_bad += 1
if not _str_bad:
    print("no unterminated string literals")
problems += _str_bad

# A file-scope static used ABOVE the line that declares it.
#
# C needs the declaration first, and the compiler does say so -- but it says it
# at the USE, naming a variable that "does not exist" and helpfully suggesting a
# similarly-spelled one:
#
#   gpu_arena.c:417: 'g_copy_bad' undeclared; did you mean 'g_canary_bad'?
#
# g_copy_bad was declared beside the guard that increments it and read by a
# reporter fifty lines higher. The suggestion was a DIFFERENT counter that
# would have compiled cleanly and reported the wrong number.
#
# Only file-scope `static <type> name;` counts -- locals and parameters shadow
# legitimately and are not this mistake.
_ubd = 0
for _c in sorted(SRC.glob("*.c")):
    _lines = cstrip.strip(_c.read_text(errors="replace")).split("\n")
    _decl = {}
    for _i, _l in enumerate(_lines, 1):
        if _l[:1] in " \t":
            continue                       # indented => not file scope
        _m = re.match(r'static\s+[A-Za-z_][\w \*]*?\b(\w+)\s*(?:=[^;]*)?;\s*$', _l)
        if _m and _m.group(1) not in _decl:
            _decl[_m.group(1)] = _i
    for _name, _dl in _decl.items():
        _pat = re.compile(r'\b' + re.escape(_name) + r'\b')
        for _i, _l in enumerate(_lines[:_dl - 1], 1):
            if _pat.search(_l):
                print(f"{_c.name}:{_i}: uses '{_name}', declared later at line {_dl}")
                _ubd += 1
                break
if not _ubd:
    print("no file-scope statics used before declaration")
problems += _ubd

sys.exit(0)


# --- symbols defined in one .c, used in another, declared in NO header -------
#
# The gap this file had. decl_check matched uses against symbols DECLARED in
# project headers, so it could say "you used X, include its header" -- but a
# symbol declared nowhere at all was invisible, and that is the classic C
# implicit-declaration error:
#
#   jni_fake.c:685: implicit declaration of function 'sts2_rename_replacing'
#
# I had added the definition to godot_shim.c and the include to jni_fake.c, but
# the edit adding the DECLARATION to godot_shim.h silently did not apply. Every
# checker passed and the compiler did not.
#
# Under C99 this is an error; under older dialects it is a warning and the
# compiler invents `int f()`, which then mismatches the real signature and
# corrupts arguments at the call site. Worth catching either way.
