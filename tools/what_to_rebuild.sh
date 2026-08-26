#!/usr/bin/env bash
# what_to_rebuild.sh -- which artifact does a change land in?
#
# This port has TWO independently built outputs and it is not obvious from a
# diff which one a change affects. Getting it wrong wastes a full hardware
# cycle: the log looks unchanged and the natural conclusion is "the fix did not
# work" rather than "the fix is not on the card".
#
# The split is decided by what each build actually reads:
#
#   sts2.nro     Makefile: SOURCES := source
#                -> source/**, Makefile
#
#   libsts2.so   scripts/docker-entry.sh: builds patches/STS2Switch.csproj,
#                runs StS2Patcher over /in, then ILC with aot/
#                -> patches/**, tools/StS2Patcher/**, aot/**
#
# Everything else -- docs/, tools/*.py -- affects neither.
#
# Usage:  ./tools/what_to_rebuild.sh
#
# MIT license; see LICENSE.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

NRO="$ROOT/sts2.nro"
PAYLOAD=""
for c in "$ROOT/dist/libsts2.so" "$ROOT/libsts2.so"; do
    [ -f "$c" ] && { PAYLOAD="$c"; break; }
done

# newest mtime under a set of paths, or 0
newest() {
    local n=0 f
    for f in $(find "$@" -type f \
                 ! -path '*/__pycache__/*' ! -name '*.o' ! -name '*.d' \
                 2>/dev/null); do
        local t
        t=$(stat -c %Y "$f" 2>/dev/null) || continue
        [ "$t" -gt "$n" ] && n=$t
    done
    echo "$n"
}

when() { [ "$1" = 0 ] && echo "never" || date -d "@$1" '+%Y-%m-%d %H:%M'; }

SRC_T=$(newest source Makefile)
PATCH_T=$(newest patches aot tools/StS2Patcher)
NRO_T=$([ -f "$NRO" ] && stat -c %Y "$NRO" || echo 0)
PAY_T=$([ -n "$PAYLOAD" ] && stat -c %Y "$PAYLOAD" || echo 0)

echo
echo "  newest source/ + Makefile   $(when "$SRC_T")"
echo "  newest patches/ + aot/      $(when "$PATCH_T")"
echo "  sts2.nro                    $(when "$NRO_T")"
echo "  libsts2.so                  $(when "$PAY_T")"
echo

need_nro=0
need_aot=0
[ "$SRC_T"   -gt "$NRO_T" ] && need_nro=1
[ "$PATCH_T" -gt "$PAY_T" ] && need_aot=1

# A missing artifact counts as stale rather than as "up to date". The opposite
# would report a clean tree for a build that has never run.
[ "$NRO_T" = 0 ] && need_nro=1
[ "$PAY_T" = 0 ] && need_aot=1

if [ "$need_nro" = 1 ]; then
    echo "  REBUILD THE NRO:"
    echo "      make"
    find source Makefile -type f -newer "$NRO" 2>/dev/null | sed 's/^/          /' | head -12
    echo
fi

if [ "$need_aot" = 1 ]; then
    echo "  REBUILD THE AOT PAYLOAD:"
    echo "      docker build -f scripts/Dockerfile.aot -t sts2nx-aot ."
    echo "      docker run --rm -it -v \"\$PWD/aot/assemblies-raw:/in:ro\" \\"
    echo "                          -v \"\$PWD/dist:/out\" sts2nx-aot"
    if [ -n "$PAYLOAD" ]; then
        find patches aot tools/StS2Patcher -type f -newer "$PAYLOAD" 2>/dev/null \
            | sed 's/^/          /' | head -12
    fi
    echo
fi

[ "$need_nro" = 0 ] && [ "$need_aot" = 0 ] && echo "  Both artifacts are up to date."
echo
