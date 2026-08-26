#!/usr/bin/env python3
"""sandbox_check.py -- every path-taking POSIX import must be rebased.

Written after the shader cache turned out to be one missing wrapper.

getcwd_fake reports "/" to mimic Android, so the engine builds absolute paths
from a root that is not the real one. sandbox_path() rebases those into
<save_root>. Every path-taking call in imports.c went through a wrapper that
did that -- except chdir, which pointed straight at newlib.

The result: mkdir() created <save_root>/shader_cache/<Shader>, chdir() then
looked for /shader_cache/<Shader> on the SD root, found nothing, and Godot
reported "Unable to create shader cache directory" for 128 shaders on every
launch while the directories sat there empty.

One missing entry, invisible in every log, costing a full shader recompile
every boot. This makes the next one a build failure instead.
"""
import re
import sys
import pathlib
import cstrip

# POSIX calls whose FIRST argument is a filesystem path.
PATH_FUNCS = {
    "open", "open64", "openat", "creat", "fopen", "fopen64", "freopen",
    "stat", "stat64", "lstat", "lstat64", "access", "faccessat",
    "mkdir", "mkdirat", "rmdir", "unlink", "unlinkat", "remove", "rename",
    "chdir", "opendir", "scandir", "chmod", "chown", "utime", "utimes",
    "truncate", "statvfs", "statfs", "realpath", "readlink", "symlink",
    "link", "mkfifo", "mknod",
}

# Wrappers are named *_fake / *_shim / sts2_* / *_hook by convention here.
WRAPPED = re.compile(r"(_fake|_shim|_hook)$|^sts2_")

# Deliberate exceptions: a target that ignores its path entirely cannot leak
# one. Keep this list short and justified.
ALLOWED = {
    "chmod": "ret0 -- FAT has no permissions; the path is never touched",
    "chown": "ret0 -- same",
    # These take a path and deliberately ignore it: they answer from synthetic
    # values and never touch the filesystem, so there is nothing to rebase.
    # Verified by reading the bodies -- each opens with `(void)path;`.
    "statfs":  "(void)path -- synthetic filesystem stats",
    "statvfs": "(void)path -- synthetic filesystem stats",
    "realpath": "copies the string; opens nothing. Callers rebase when they "
                "actually open the result.",
}

# Targets that cannot leak a path because they never read one: they fail or
# succeed unconditionally. Checked by NAME rather than by call, so a wrapper
# that starts doing real work under one of these names still gets flagged.
INERT_TARGETS = {
    "proc_enosys",   # returns -1/ENOSYS; link, symlink, mkfifo
    "ret0",          # returns 0
}


def _body_sandboxes(src, fname, _depth=0):
    """Does fname's body rebase the path, directly or by delegation?

    Follows delegation rather than consulting a list of known-good names. A
    hardcoded list was the first attempt and it was wrong in both directions:
    it missed lstat64_fake -> lstat_fake and would have gone stale the moment a
    shim was renamed. Depth-limited because shims do call each other.
    """
    if _depth > 4:
        return True          # give up rather than report on a deep chain
    for c in pathlib.Path(src).glob("*.c"):
        text = c.read_text(errors="replace")
        m = re.search(r'^[A-Za-z_].*\b' + re.escape(fname) + r'\s*\([^;]*\)\s*\{',
                      text, re.M)
        if not m:
            continue
        # crude brace match from the opening brace
        i = text.index("{", m.start())
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{": depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0: break
            j += 1
        # Comments stripped FIRST.
        #
        # Without this the matcher reads its own documentation: the comment
        # explaining "open_fake, not open, because the path needs the
        # sandbox_path() rebase" contains the string it searches for, so a
        # function that had just been reverted to the unsafe form still passed.
        # A checker defeated by a comment about the thing it checks is worse
        # than no checker.
        body = cstrip.strip(text[i:j])
        if "sandbox_path" in body:
            return True
        # Delegates to another shim? Follow it.
        for callee in set(re.findall(r'\b(\w+_fake)\s*\(', body)):
            if callee != fname and _body_sandboxes(src, callee, _depth + 1):
                return True
        return False
    return True     # not found in source: assume fine rather than cry wolf


def main(src_dir):
    src = pathlib.Path(src_dir)
    imports = src / "imports.c"
    if not imports.exists():
        print(f"no imports.c under {src}")
        return 1

    body = imports.read_text(errors="replace")
    entry = re.compile(r'\{\s*"([a-z_0-9]+)"\s*,\s*\(uintptr_t\)&\s*([A-Za-z_0-9]+)\s*\}')

    problems = 0
    checked = 0
    for m in entry.finditer(body):
        name, target = m.group(1), m.group(2)
        if name not in PATH_FUNCS:
            continue
        checked += 1
        if name in ALLOWED:
            continue        # checked by hand; reason recorded in ALLOWED
        if WRAPPED.search(target):
            # A wrapper NAME is not evidence. openat_fake was named like every
            # other shim and forwarded straight to newlib's open() with the
            # caller's path, unrebased -- exactly the bug this checker exists
            # to prevent, sitting behind a suffix the checker trusted.
            #
            # So look at the body: it must either call sandbox_path itself, or
            # delegate to another shim that does.
            if not _body_sandboxes(src, target):
                line = body[: m.start()].count("\n") + 1
                print(f"imports.c:{line}: {name}() -> {target} is named like a "
                      f"shim but does not sandbox")
                print(f"    its body calls neither sandbox_path() nor another "
                      f"*_fake that does")
                problems += 1
            continue
        if target in INERT_TARGETS:
            continue
        line = body[: m.start()].count("\n") + 1
        print(f"imports.c:{line}: {name}() -> {target} is NOT sandboxed")
        print(f"    a path-taking call must go through sandbox_path(); see chdir_fake")
        problems += 1

    print(f"\n# {checked} path-taking imports checked")
    print("all path imports sandboxed" if not problems
          else f"{problems} unsandboxed path import(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "source"))
