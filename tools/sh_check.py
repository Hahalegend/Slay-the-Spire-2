#!/usr/bin/env python3
"""sh_check.py -- catch shell bugs that only fire at runtime.

Two classes, both of which have cost a build cycle:

  * `$VAR` under `set -u` where VAR is never assigned and not guarded with
    `:-`. This fails only when that branch runs, which for a Docker build means
    after several minutes of restore and weave:

        /src/scripts/docker-entry.sh: line 109: STS2_STRICT: unbound variable

  * a syntax error, which `bash -n` finds instantly and nobody runs by hand.

Quoting matters and a naive scan gets it wrong: a first version flagged
`\\$ANDROID_NDK` inside an escaped error string and `$p` inside sed's
single-quoted `'/x/,$p'`. Neither is a shell expansion. This tracks
single-quote, double-quote and backslash state so those do not appear.
"""

import pathlib
import re
import subprocess
import sys


def expansions(text):
    """(line, name) for every $VAR / ${VAR} the shell would expand.

    Line by line, deliberately. A character-by-character state machine across
    the whole file is the obvious approach and it is what I wrote first: it
    drifted, decided it was inside a single-quoted string from line 80 onward,
    and reported the file clean while the very bug it was written for sat at
    line 110.

    Per line, the only state that carries is a heredoc body, which is tracked
    explicitly. Everything else -- comments, single quotes -- resolves within
    the line. A multi-line single-quoted string would produce a false positive
    rather than a miss, which is the right way round for a checker.
    """
    out = []
    heredoc = None
    for lineno, raw in enumerate(text.split("\n"), 1):
        if heredoc is not None:
            if raw.strip() == heredoc:
                heredoc = None
            continue
        m = re.search(r"<<-?\s*'?([A-Za-z_]\w*)'?", raw)
        if m:
            heredoc = m.group(1)
            continue

        line = re.sub(r'\\.', '', raw)          # \$VAR is literal
        line = re.sub(r"'[^']*'", "''", line)    # no expansion in single quotes
        h = line.find("#")                       # comment, if # starts a word
        while h > 0 and line[h - 1] not in " \t":
            h = line.find("#", h + 1)
        if h == 0 or h > 0:
            line = line[:h]

        for m in re.finditer(r'\$\{?([A-Za-z_]\w*)', line):
            if re.match(r'\$\{[A-Za-z_]\w*\s*:?[-=?+]', line[m.start():]):
                continue                          # guarded
            out.append((lineno, m.group(1)))
    return out


def check(path):
    text = path.read_text()
    problems = 0

    r = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{path.name}: syntax error\n{r.stderr.strip()}")
        return 1

    if "set -u" not in text and "set -eu" not in text and "set -euo" not in text:
        return 0

    assigned = set(re.findall(r'^\s*(?:export\s+)?([A-Za-z_]\w*)=', text, re.M))
    assigned |= set(re.findall(r'^\s*(?:local|declare|readonly)\s+([A-Za-z_]\w*)', text, re.M))
    assigned |= set(re.findall(r'\bfor\s+([A-Za-z_]\w*)\s+in\b', text))
    assigned |= set(re.findall(r'\bread\s+(?:-\w+\s+)*([A-Za-z_]\w*)', text))
    # Set by the shell or the environment, not by the script.
    known = {"PATH", "HOME", "PWD", "OLDPWD", "IFS", "BASH_SOURCE", "FUNCNAME",
             "LINENO", "RANDOM", "SECONDS", "HOSTNAME", "USER", "SHELL", "TERM",
             "PS1", "PS2", "BASH", "BASHPID", "PPID", "UID", "EUID", "REPLY"}

    for line, name in expansions(text):
        if name in assigned or name in known:
            continue
        print(f"{path.name}:{line}: ${name} is never assigned and not guarded "
              f"-- with set -u this aborts when that line runs")
        problems += 1
    return problems


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    scripts = sorted(root.glob("scripts/*.sh")) or sorted(root.glob("*.sh"))
    total = sum(check(p) for p in scripts)
    print(f"{len(scripts)} script(s) checked; "
          + ("all clean" if not total else f"{total} problem(s)"))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
