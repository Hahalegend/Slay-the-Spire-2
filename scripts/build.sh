#!/usr/bin/env bash
# build.sh -- end-to-end.
#
#   1. weave the patch set into the game assemblies (Mono.Cecil)
#   2. NativeAOT-compile them to libsts2.so (ILC)
#   3. build the wrapper NRO (devkitA64)
#   4. assemble the SD layout
#
# Any stage can run alone: ./build.sh weave|aot|nro|stage
# No argument runs all four.
#
# The inherited drmariomania_nx scripts/ was not forked: build-godot.sh built
# an engine from source we do not have, and make-bootstrap-pck.py packed a
# project.godot that does not exist here. This replaces both.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RAW="${GAME_ASSEMBLIES:-$ROOT/aot/assemblies-raw}"
PATCHED="$ROOT/aot/assemblies-patched"
APK_DIR="${APK_DIR:-$ROOT/apk}"
OUT="$ROOT/dist"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

need_dir() { [ -d "$1" ] || die "$2"; }

# ---------------------------------------------------------------- 1. weave --
do_weave() {
  say "weaving patches"
  check_xml
  python3 "$ROOT/tools/stamp_patchset.py" "$ROOT" 2>/dev/null || true
  python3 "$ROOT/tools/sh_check.py" "$ROOT" 2>/dev/null || true
  python3 "$ROOT/tools/abi_check.py" "$ROOT/source" 2>/dev/null || true
  python3 "$ROOT/tools/sandbox_check.py" "$ROOT/source" || die "unsandboxed path import"
  python3 "$ROOT/tools/bionic_abi.py" "$ROOT/source" 2>/dev/null || true
  python3 "$ROOT/tools/arity_check.py" "$ROOT/source" 2>/dev/null || true
  need_dir "$RAW" "no game assemblies at $RAW.
Extract them from your own Steam install (aot/README.md step 1), or point
GAME_ASSEMBLIES at them."

  command -v dotnet >/dev/null || die ".NET 9 SDK not found on PATH"

  dotnet build "$ROOT/patches/STS2Switch/STS2Switch.csproj" -c Release \
    -p:GameAssemblyDir="$RAW"

  local patch_dll
  patch_dll="$(find "$ROOT/patches/STS2Switch/bin/Release" -name STS2Switch.dll | head -1)"
  [ -n "$patch_dll" ] || die "STS2Switch.dll was not produced"

  rm -rf "$PATCHED"

  # --strict escalates Optional patches to required. It is NOT passed by
  # default, because passing it unconditionally is the same as not having
  # Optional at all -- which is what happened: a patch deliberately marked
  # Optional, for a type that might not exist, failed the build anyway.
  #
  # Non-optional patches still fail without it. --strict only changes what
  # happens to the ones that were declared as "may be absent".
  #
  # STS2_STRICT=--strict for a release build, where a silently dropped patch is
  # worth failing over.
  # That is the entire reason this is a build step rather than runtime Harmony.
  # The Android launcher's PatchHelper logs "FAILED {type}.{method}: method not
  # found" and carries on, and the result is a game that boots and then behaves
  # wrongly on a device with no console to read.
  dotnet run --project "$ROOT/tools/StS2Patcher" -c Release -- \
    --game "$RAW" --patches "$patch_dll" --out "$PATCHED" ${STS2_STRICT:-}
}

# ------------------------------------------------------------------ 2. aot --
do_aot() {
  say "NativeAOT compile"
  check_xml
  need_dir "$PATCHED" "no patched assemblies -- run './build.sh weave' first"

  command -v clang >/dev/null || die "clang not on PATH.
ILC needs the Android NDK's clang for the final native link:
  export PATH=\"\$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:\$PATH\""

  # linux-bionic-arm64, NOT linux-arm64. bionic gives a libc surface so_util
  # and libc_shim can satisfy; the glibc-shaped RID does not, and its symbol
  # list does not transfer (see nativeaot_shim.c).
  dotnet publish "$ROOT/aot/sts2.aot.csproj" -c Release \
    -r linux-bionic-arm64 --self-contained \
    -p:PatchedAssemblyDir="$PATCHED"

  local so
  so="$(find "$ROOT/aot/bin" -name '*.so' -path '*publish*' | head -1)"
  [ -n "$so" ] || die "ILC produced no .so -- check the publish output above"
  echo "ILC output: $so"

  # Scan the payload's imports against the wrapper's table. Run 3 lost a
  # hardware cycle to 26 unresolved imports in exactly this file, because the
  # gap analysis had only ever been pointed at the APK. ILC's import set
  # changes with the code it compiles, so this belongs in the build, not in a
  # one-off investigation.
  if [ -d "$APK_DIR" ]; then
    say "checking payload imports"
    AOT_PAYLOAD="$so" python3 "$ROOT/tools/import_gap.py" "$APK_DIR" \
      | sed -n '/AOT payload/,$p' || true
    echo
    echo "Anything listed above under [other] or [bionic-isms] needs an entry"
    echo "in imports.c before this will run. so_resolve taints what it cannot"
    echo "resolve, so a missing one is a Data Abort, not a link error."
  fi
}

# ------------------------------------------------------------------ 3. nro --
do_nro() {
  say "building the wrapper"
  [ -n "${DEVKITPRO:-}" ] || die "DEVKITPRO not set:
  export DEVKITPRO=/opt/devkitpro"
  make -C "$ROOT" -j"$(nproc 2>/dev/null || echo 4)"
}

# ---------------------------------------------------------------- 4. stage --
do_stage() {
  say "assembling the SD layout"
  mkdir -p "$OUT"

  [ -f "$ROOT/sts2.nro" ] && cp "$ROOT/sts2.nro" "$OUT/"

  if [ -d "$APK_DIR" ]; then
    local lib="$APK_DIR/lib/arm64-v8a"
    for f in libgodot_android.so libc++_shared.so \
             libGodotFmod.android.template_release.arm64.so \
             libspine_godot.android.template_release.arm64.so; do
      if [ -f "$lib/$f" ]; then cp "$lib/$f" "$OUT/"; else echo "  MISSING: $f"; fi
    done

    # These two are under assets/native/, NOT lib/arm64-v8a/ -- the launcher
    # extracts them at runtime. Single easiest thing to miss when assembling
    # by hand, which is most of why this script exists.
    for f in libfmod.so libfmodstudio.so; do
      if [ -f "$APK_DIR/assets/native/$f" ]; then
        cp "$APK_DIR/assets/native/$f" "$OUT/"
      else
        echo "  MISSING: $f  (assets/native/ in the APK, not lib/)"
      fi
    done

    [ -d "$APK_DIR/assets" ] && cp -r "$APK_DIR/assets" "$OUT/"
  else
    echo "  APK_DIR unset or absent; skipping engine/GDExtension staging"
  fi

  local so
  so="$(find "$ROOT/aot/bin" -name '*.so' -path '*publish*' 2>/dev/null | head -1)"
  [ -n "$so" ] && cp "$so" "$OUT/libsts2.so"

  # override.cfg sits NEXT TO THE PCK, not in assets/.
  #
  # Godot has two code paths and takes exactly one. With --main-pack, which is
  # what the wrapper passes:
  #
  #   _load_settings_text(p_main_pack.get_base_dir().path_join("override.cfg"))
  #
  # so it reads <dir containing sts2.pck>/override.cfg. The "res://override.cfg"
  # string also in the binary belongs to the directory-project path, which we
  # never take. Both strings are present in libgodot_android.so; only one is
  # reachable here.
  #
  # Copied only if absent: it is meant to be edited on the card one setting at
  # a time, and clobbering those edits every build would defeat the point.
  if [ -f "$ROOT/dist_template/override.cfg" ]; then
    if [ -f "$OUT/override.cfg" ]; then
      echo "  keeping your existing override.cfg"
    else
      cp "$ROOT/dist_template/override.cfg" "$OUT/override.cfg"
      echo "  staged override.cfg next to the pck (all settings commented out)"
    fi
  fi

  say "staged in $OUT"
  echo "Still needed, from your own Steam install:"
  echo "  sts2.pck   -- the launcher APK carries no game content"
  echo
  echo "Then copy $OUT/* into /switch/<any-name>/ on the SD card."
  echo "The directory name does not matter; app_paths.c discovers it."
}

case "${1:-all}" in
  weave) do_weave ;;
  aot)   do_aot ;;
  nro)   do_nro ;;
  stage) do_stage ;;
  all)   do_weave; do_aot; do_nro; do_stage ;;
  *)     die "usage: build.sh [weave|aot|nro|stage]" ;;
esac
