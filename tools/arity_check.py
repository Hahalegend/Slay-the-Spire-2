#!/usr/bin/env python3
"""arity_check.py -- call sites that pass the wrong number of arguments.

The gap this fills: sig_check.py compares a HEADER prototype against its
definition, so it only sees functions that cross a file boundary. A `static`
helper has no prototype, so changing its signature and missing a caller is
invisible to it -- and that is exactly what happened with get_id():

    static FakeID *get_id(const char *cls, const char *name, const char *sig)
    ...
    return get_id(name, sig);        // j_GetFieldID, never updated

gcc: "too few arguments to function 'get_id'; expected 3, have 2".

Also flags nested block comments, which gcc reports as -Wcomment. A '/*' inside
a comment is only a warning, but a stray '*/' would END the comment early and
silently compile the rest of the prose as code, so both are worth catching.
"""

import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cstrip
import sys
import pathlib

SRC = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")


def strip_strings(t):
    return cstrip.strip(t)


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


def split_args(s):
    """Top-level comma split, respecting nesting."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def param_count(params):
    params = params.strip()
    if params in ("", "void"):
        return 0, False
    parts = split_args(params)
    variadic = parts and parts[-1] == "..."
    if variadic:
        parts = parts[:-1]
    return len(parts), variadic


def matching_paren(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


C_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "defined",
              "va_arg", "va_start", "va_copy", "va_end", "do", "else"}

problems = 0

for c in sorted(list(SRC.glob("*.c")) + list(SRC.glob("*.h"))):
    raw = c.read_text(errors="replace")

    # --- nested comment markers ---------------------------------------------
    for m in re.finditer(r'/\*.*?\*/', raw, re.S):
        if "/*" in m.group(0)[2:-2]:
            line = raw[:m.start()].count("\n") + 1
            print(f"{c.name}:{line}: '/*' within a block comment (-Wcomment)")
            problems += 1

    if c.suffix != ".c":
        continue

    body = strip_comments(strip_strings(raw))

    # --- functions defined in this file --------------------------------------
    defs = {}
    for m in re.finditer(
            r'^[A-Za-z_][A-Za-z0-9_ \*]*?\b([a-z_][A-Za-z0-9_]*)\s*\(', body, re.M):
        name = m.group(1)
        if name in C_KEYWORDS:
            continue
        op = m.end() - 1
        cp = matching_paren(body, op)
        if cp < 0:
            continue
        after = body[cp + 1:cp + 3].strip()
        if not after.startswith("{"):
            continue                       # a prototype, not a definition
        n, var = param_count(body[op + 1:cp])
        defs[name] = (n, var)

    # --- call sites ----------------------------------------------------------
    for m in re.finditer(r'\b([a-z_][A-Za-z0-9_]*)\s*\(', body):
        name = m.group(1)
        if name not in defs or name in C_KEYWORDS:
            continue
        op = m.end() - 1
        cp = matching_paren(body, op)
        if cp < 0:
            continue
        # skip the definition itself
        after = body[cp + 1:cp + 3].strip()
        if after.startswith("{"):
            continue

        args = split_args(body[op + 1:cp])

        # Skip forward declarations. `int main(void);` is not a call site, but
        # it matches the same pattern -- and split_args gives ["void"], which
        # reads as "one argument". That produced a false
        # "main() expects 0, got 1" against a perfectly good prototype.
        if args == ["void"]:
            continue
        # Same shape, general case: a prototype's arguments are `type name`
        # pairs, so every one contains a space and none is a bare identifier or
        # expression. A real call almost never looks like that.
        if args and all(" " in a and "(" not in a for a in args):
            continue

        want, variadic = defs[name]
        got = len(args)

        if (variadic and got < want) or (not variadic and got != want):
            line = body[:m.start()].count("\n") + 1
            print(f"{c.name}:{line}: {name}() expects "
                  f"{want}{'+' if variadic else ''}, got {got}")
            problems += 1

print()
print("no arity problems" if not problems else f"{problems} problem(s)")


# --- duplicate definitions --------------------------------------------------
#
# Added after a real compile failure: I appended
# __libc_current_sigrtmin/__libc_current_sigrtmax to nativeaot_shim.c without
# noticing the file already defined both, 150 lines further down. gcc reports
# "redefinition of ...", which none of the three checkers could see, because
# each only looked at calls-vs-definitions rather than definitions-vs-each-other.
#
# Two cases, both fatal and both cheap to detect:
#   same file  -> compile error
#   two files  -> link error (multiple definition), which is worse because it
#                 survives every per-file check and only fails at the very end
#                 of a long build.
def _same_branch(body, line_a, line_b):
    """True if both lines are reachable in the same preprocessor branch.

    Walks #if/#else/#endif between them: if the nesting returns to its
    starting depth without an intervening #else/#elif at that depth, the two
    are in the same branch and a redefinition is real.
    """
    lines = body.split("\n")
    lo, hi = min(line_a, line_b), max(line_a, line_b)
    depth = 0
    for l in lines[lo:hi - 1]:          # strictly between the two
        t = l.strip()
        if re.match(r"#\s*(if|ifdef|ifndef)\b", t):
            depth += 1
        elif re.match(r"#\s*endif\b", t):
            depth -= 1
        elif re.match(r"#\s*(else|elif)\b", t) and depth == 0:
            return False                # separated by #else at this level
    return True


def check_duplicate_definitions(src):
    defs = {}
    problems = 0
    for c in sorted(src.glob("*.c")):
        body = strip_comments(strip_strings(c.read_text(errors="replace")))
        seen = {}
        for m in re.finditer(
                r'^([A-Za-z_][A-Za-z0-9_ \*]*?)\b([a-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{',
                body, re.M):
            ret, name = m.group(1).strip(), m.group(2)
            if ret.startswith("static"):
                continue
            line = body[:m.start()].count("\n") + 1
            if name in seen:
                # Mutually exclusive preprocessor branches are not duplicates.
                #
                # call_once_shim.c defines call_once_report() once under
                # #if SHIM_CALL_ONCE and once under #else; only one ever
                # compiles. Flagging that would make the check cry wolf on
                # every conditionally-compiled file, and a checker that is
                # routinely wrong gets ignored -- which costs more than the
                # duplicates it would otherwise catch.
                if not _same_branch(body, seen[name], line):
                    seen[name] = line
                    continue
                print(f"{c.name}:{line}: {name}() redefined "
                      f"(first at ~{seen[name]})")
                problems += 1
            seen[name] = line
            defs.setdefault(name, set()).add(c.name)

    for name, files in sorted(defs.items()):
        if len(files) > 1:
            print(f"multiple definition of {name}(): {sorted(files)}")
            problems += 1
    return problems


# --- macro used before it is #defined, within one .c ------------------------
#
# Third ordering error of the session, and the first two checks could not see
# any of them. I added sts2_is_fake_dl_handle at the top of egl_shim.c, using
# DOTNET_DL_HANDLE, which is #defined 50 lines further down. gcc: "undeclared".
#
# Only project macros defined in the SAME .c are checked. Header macros are
# visible from the include, and system macros are not ours to reason about --
# including them would bury the real hits.
def check_macro_order(src):
    problems = 0
    for c in sorted(src.glob("*.c")):
        body = strip_comments(strip_strings(c.read_text(errors="replace")))
        defined = {}
        for m in re.finditer(r'^\s*#define\s+([A-Z][A-Z0-9_]{3,})\b', body, re.M):
            defined.setdefault(m.group(1), body[:m.start()].count("\n") + 1)
        if not defined:
            continue
        lines = body.split("\n")
        for name, dline in defined.items():
            for u in re.finditer(r'\b' + name + r'\b', body):
                uline = body[:u.start()].count("\n") + 1
                if uline >= dline:
                    continue

                text = lines[uline - 1] if uline - 1 < len(lines) else ""
                stripped = text.lstrip()

                # `#ifndef FOO` / `#define FOO` is the standard guard for a
                # macro the toolchain may already provide. Naming it first is
                # the whole point.
                if re.match(r'#\s*(ifn?def|if|elif|undef)\b', stripped):
                    continue

                # An X-macro lists its callbacks inside another macro's body;
                # those names are only expanded where the inner macros are in
                # scope, which is legitimately later.
                #
                # A continuation line ends with a backslash -- but the LAST
                # line of the body does not, which is exactly where
                # CONFIG_VARS' final CONFIG_VAR_STR(save_root) sits. So look
                # backwards as well: still inside a macro body if the previous
                # line continued.
                if text.rstrip().endswith("\\"):
                    continue
                if uline >= 2 and lines[uline - 2].rstrip().endswith("\\"):
                    continue

                print(f"{c.name}:{uline}: {name} used before its #define "
                      f"at line {dline}")
                problems += 1
                break
    return problems




# --- same-scope local redeclarations -----------------------------------------
#
# `struct stat scst;` twice in one block is a hard compile error, and it
# happened: two directory checks added in different rounds, each reasonable on
# its own, neither aware of the other.
#
# DELIBERATELY NARROW. A first attempt matched any `word word;` line, which
# treats `return x;` as a declaration of `x` and reported 51 problems on a tree
# that compiles. A checker with a 100% false-positive rate is worse than no
# checker, because it teaches you to ignore the output. This only matches
# declarations whose type is unmistakable.
_TYPE = (r'(?:struct|union|enum)\s+\w+|'
         r'(?:unsigned|signed)\s+(?:char|short|int|long|long\s+long)|'
         r'void|char|short|int|long|float|double|_Bool|bool|'
         r'\w+_t|FILE|Mutex|CondVar|RwLock|Thread|Handle|Result')

def check_local_redecl(src):
    decl = re.compile(r'^\s*(?:const\s+|volatile\s+)*(?:' + _TYPE +
                      r')\s+\*?\s*([a-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;\s*$')
    problems = 0
    for f in sorted(src.glob("*.c")):
        body = cstrip.strip(f.read_text(errors="replace"))
        depth, fn_depth, seen = 0, None, {}
        for lineno, line in enumerate(body.split("\n"), 1):
            if fn_depth is None and re.match(r'^[A-Za-z_].*\)\s*\{\s*$', line):
                fn_depth, seen = depth, {}
            m = decl.match(line)
            # Only the function's own top level, where a redeclaration is an
            # error. Inner blocks legitimately shadow.
            if m and fn_depth is not None and depth == fn_depth + 1:
                name = m.group(1)
                if name in seen:
                    print(f"{f.name}:{lineno}: '{name}' redeclared in the same "
                          f"scope (first at line {seen[name]})")
                    problems += 1
                else:
                    seen[name] = lineno
            depth += line.count("{") - line.count("}")
            if fn_depth is not None and depth <= fn_depth:
                fn_depth, seen = None, {}
    return problems

if __name__ == "__main__" and len(sys.argv) > 1:
    extra = check_duplicate_definitions(SRC)
    print("no duplicate definitions" if not extra else f"{extra} duplicate(s)")
    mo = check_macro_order(SRC)
    print("no macro ordering problems" if not mo else f"{mo} ordering problem(s)")
    rd = check_local_redecl(SRC)
    print("no same-scope redeclarations" if not rd else f"{rd} redeclaration(s)")
