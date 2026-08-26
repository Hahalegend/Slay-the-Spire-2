#!/usr/bin/env python3
"""check_stale.py -- find files the current tree has and the shipped one does not.

WHY THIS EXISTS

This project moves between machines as a zip. Extracting a zip ADDS and
OVERWRITES; it never DELETES. So any file removed or renamed upstream stays
behind on the target, and `make` globs `source/*.c` -- which means a deleted
.c file keeps getting compiled long after it stopped being part of the build.

That cost a build cycle when `isa_dump.c` was moved to `tools/parked/`:

    source/isa_dump.c:40:10: fatal error: isa_dump.h: No such file or directory

The file was gone from the zip. It was not gone from the disk.

HOW IT WORKS

`tools/tree_manifest.txt` is regenerated whenever the project is packaged, and
lists every file that SHOULD exist. This compares the tree against it and
reports:

  STALE   on disk, not in the manifest  -- delete these, they may still build
  MISSING in the manifest, not on disk  -- extraction was incomplete

Usage:
    python3 tools/check_stale.py            # report
    python3 tools/check_stale.py --delete   # report, then remove STALE files

MIT license; see LICENSE.
"""

import pathlib
import sys

SKIP_DIRS = {".git", "build", "dist", "bin", "obj", "__pycache__", "node_modules"}
# Only these matter for correctness: a stray .md is harmless, a stray .c is not.
BUILD_INPUTS = {".c", ".h", ".cs", ".csproj"}


def walk(root: pathlib.Path):
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        yield p.relative_to(root).as_posix()


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    manifest = root / "tools" / "tree_manifest.txt"

    if not manifest.exists():
        print(f"no manifest at {manifest}")
        print("nothing to compare against; regenerate it when packaging")
        return 2

    want = {
        line.strip()
        for line in manifest.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }
    have = set(walk(root))

    stale = sorted(have - want)
    missing = sorted(want - have)

    # A stale build input is the dangerous kind: make will compile it.
    stale_build = [f for f in stale if pathlib.Path(f).suffix in BUILD_INPUTS]
    stale_other = [f for f in stale if pathlib.Path(f).suffix not in BUILD_INPUTS]

    if stale_build:
        print("STALE BUILD INPUTS -- these still compile and should not:")
        for f in stale_build:
            print(f"    {f}")
    if stale_other:
        print(f"stale non-build files ({len(stale_other)}, harmless):")
        for f in stale_other[:10]:
            print(f"    {f}")
        if len(stale_other) > 10:
            print(f"    ... and {len(stale_other) - 10} more")
    if missing:
        print("MISSING -- in the manifest but not on disk (incomplete extract?):")
        for f in missing[:20]:
            print(f"    {f}")
        if len(missing) > 20:
            print(f"    ... and {len(missing) - 20} more")

    if not stale and not missing:
        print(f"tree matches the manifest ({len(want)} files)")
        return 0

    if "--delete" in sys.argv and stale:
        for f in stale:
            (root / f).unlink()
        print(f"\ndeleted {len(stale)} stale file(s)")
        return 0

    if stale_build:
        print("\nrun with --delete to remove them, or delete by hand")
    return 1 if stale_build or missing else 0


if __name__ == "__main__":
    raise SystemExit(main())


# A reachability check was written here and removed. It flagged 242 entry
# points as "declared but never called", of which essentially all are shims
# called BY the loaded .so through the import table -- nativeaot_shim.h,
# imports.c and friends -- not by any C in this tree.
#
# This project already deleted a callorder_check at 19 false positives out of
# 19. A checker at 242 is not a stricter version of that mistake, it is the
# same one. Checkers earn their place only where the compiler cannot see AND
# the signal survives the noise.
#
# The failure that prompted it -- astc_bake_run defined, headered, configured,
# documented, and never called -- was not caused by a missing checker. It was
# caused by an edit script that printed "path fixed" unconditionally and a
# grep whose empty output went unread. The fix is in how edits are verified:
# re-read the file from disk, assert the anchor matched, and exit non-zero when
# it did not.
