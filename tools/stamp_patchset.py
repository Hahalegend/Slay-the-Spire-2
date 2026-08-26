#!/usr/bin/env python3
"""stamp_patchset.py -- write a fingerprint of the patch sources into SwitchLog.

Three hardware rounds were spent reading logs from a payload built before the
fix under test. Each time it was diagnosed by noticing an expected message was
missing, which is slow and ambiguous: absent because the code did not run, or
absent because it is not in this build?

SwitchLog prints this id on its first line, so the log answers that directly.

Run before packaging. Hand-editing the constant defeats the point -- it is
derived from the files it describes, so it cannot claim to be a build it is not.
"""

import hashlib
import pathlib
import re
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    patches = sorted((root / "patches" / "STS2Switch").glob("*.cs"))
    target = root / "patches" / "STS2Switch" / "SwitchLog.cs"

    if not patches or not target.exists():
        print("stamp: patch sources not found", file=sys.stderr)
        return 1

    h = hashlib.sha1()
    for f in patches:
        # SwitchLog itself is excluded: it CONTAINS the stamp, so including it
        # would make the hash depend on its own previous value and never settle.
        if f.name == "SwitchLog.cs":
            continue
        h.update(f.name.encode())
        h.update(f.read_bytes())
    stamp = h.hexdigest()[:8]

    s = target.read_text()
    new, n = re.subn(r'(public const string PatchSetId = ")[0-9a-f]{8}(";)',
                     r'\g<1>' + stamp + r'\g<2>', s)
    if n != 1:
        print(f"stamp: expected exactly one PatchSetId, found {n}", file=sys.stderr)
        return 1

    if new != s:
        target.write_text(new)
        print(f"stamp: patch set {stamp} (updated)")
    else:
        print(f"stamp: patch set {stamp} (unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
