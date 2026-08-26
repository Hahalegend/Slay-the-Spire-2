# NativeAOT payload

## Why this is harder than it was for Dr. Mario Mania

That port ran `dotnet publish` on the game's own `.csproj`. There is no
`.csproj` here — the input is a directory of already-compiled assemblies from
your Steam copy. ILC does not care (it consumes IL, not C#), but three things
that MSBuild normally handles now have to be arranged by hand:

1. **A project for ILC to run under.** `sts2.aot.csproj` is a shim: it
   produces no code of its own and references the game assemblies as raw
   `<Reference>` items.
2. **Trimmer roots and reflection metadata.** With source you annotate
   declarations. Without it, everything goes in `rd.xml`, and the failure mode
   is quiet — Godot logs a missing class rather than throwing.
3. **Version drift.** Every game update changes the assemblies. The weave step
   (`tools/StS2Patcher`) fails loudly on a moved method, which is the point.

## Pipeline

```
Steam depot  ──►  raw assemblies  ──►  StS2Patcher  ──►  ILC  ──►  libsts2.so
                                       (Cecil weave)     (AOT)
```

## Step 1 — get the assemblies

Two sources, and mixing them up is subtle enough to be worth stating.

### sts2.dll — from your Steam install

The managed assemblies live under the game's `.godot/mono/` tree or next to the
executable depending on export settings.

It is **not** in the launcher APK. The launcher downloads game content from
Steam at runtime, so `libassembly-store.so` holds only the launcher's own
`STS2Mobile*.dll`, SteamKit2, Harmony and the BCL — verified by extracting all
260 entries.

### GodotSharp.dll — from the launcher APK, not Steam

```sh
python3 tools/extract_assembly_store.py \
  libassembly-store.so ./extracted GodotSharp.dll
```

The Steam install has a `GodotSharp.dll` too, and it is the **wrong one**. It is
built against the desktop engine; the `libgodot_android.so` going on the SD card
is the launcher's custom Android build. GodotSharp carries the interop surface
the engine binds against, so a mismatch shows up as missing methods or wrong
vtable offsets at runtime rather than as a clean load error — which is a
miserable thing to diagnose from a console with no debugger.

Take `GodotSharp.dll` from the store, `sts2.dll` from Steam.

Do not commit either anywhere. They belong to Mega Crit and to the launcher
author respectively, and the permission this project operates under covers
running them, not redistributing them.

## Step 2 — find out what the .pck actually references

Before writing `rd.xml` entries, check the real names rather than the
plausible ones. Assembly names, the GDExtension entry symbols, and the
NativeAOT library name the engine probes for are all things this repo has
guessed at and marked as such:

```sh
# GDExtension entry symbols -- correct config.h FMOD_SO_NAME/SPINE_SO_NAME
# and fmod_shim.c's entry symbol lists from this
unzip -p sts2.pck '*.gdextension' | grep -E 'entry_symbol|android'

# what the engine probes for the AOT payload -- correct DOTNET_SO_NAME
readelf -p .rodata libgodot_android.so | grep -iE '\.so|aot'

# assembly names, for rd.xml
ls *.dll
```

## Step 3 — weave

```sh
dotnet run --project ../tools/StS2Patcher -- \
  --game    ./assemblies-raw \
  --patches ./STS2Switch.dll \
  --out     ./assemblies-patched \
  --strict
```

Non-zero exit means a patch did not apply. Do not proceed past a failure —
a dropped patch is a game that boots and then behaves wrongly in a way that is
hard to attribute later.

## Step 4 — compile

```sh
export PATH="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
dotnet publish aot/sts2.aot.csproj -c Release -r linux-bionic-arm64 --self-contained
```

Output goes to `bin/Release/net9.0/linux-bionic-arm64/publish/`. Rename to
match `DOTNET_SO_NAME` from `config.h` and drop it in `/switch/sts2_nx/`.

## Step 5 — the part that will actually take the time

The first build will be large and will probably not boot. Work in this order,
because each step gives a signal the next one depends on:

1. **Root everything.** `rd.xml` as shipped is deliberately broad. Confirm the
   payload loads and `godotsharp_game_main_init` is reached (the wrapper logs
   it). Do not trim anything yet.
2. **Get to a menu.** Missing-class errors in the Godot log map to types that
   need rooting. Godot names them as it fails to find them, which is the most
   useful diagnostic in the whole pipeline — read the log, add the type, repeat.
3. **Get through a run.** Serialization and FMOD/Spine callback delegates
   surface here, not at boot. Saves that load empty rather than failing are
   almost always an STJ rooting problem.
4. **Only then trim.** Narrow `sts2` from `Required All` to specific
   namespaces, one at a time, re-testing each.

Expect step 2 to be the long one. It is the price of not having the source,
and it is the single biggest difference between this port and the Dr. Mario
one.

## Known-unknowns in this directory

Marked here so they are not mistaken for verified facts:

- Assembly name `sts2` is assumed. Correct from step 2.
- ~~ILC ignores `<Assembly>` entries it cannot resolve, so a wrong guess is
  harmless.~~ **This was wrong.** ILC treats an `<Assembly Name="X">` naming
  something absent from the compilation as a hard error: `Failed to load
  assembly 'X'`. `GodotSharpEditor`, `System.Text.Json`, `FmodSharp`,
  `fmod_godot`, `spine-godot` and `SpineGodotSharp` were all named in `rd.xml`
  and each failed the build in turn.

  The roots are now generated from the real reference set
  (`scripts/docker-entry.sh` -> `rd.generated.xml`), so this class of mistake
  cannot recur.

- ILC's rd.xml support is a **subset** of the .NET Native directives:
  `<Assembly>` and `<Type>` only. `<Namespace>` is rejected outright with
  "Namespace is not a supported Runtime Directive".
- Whether StS2 uses source-generated or reflection-based `System.Text.Json` is
  unknown. If source-generated, the STJ block in `rd.xml` is dead weight.
