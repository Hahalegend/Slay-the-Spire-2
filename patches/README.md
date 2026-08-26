# Patches

Build-time replacement for the Android launcher's Harmony patches.

## Supported subset

```csharp
[SwitchPrefix(typeof(Target), "Method")]
public static bool Example(SomeType __instance, ref int __result, int namedArg)
{
    __result = 42;
    return false;   // skip the original; return void or true to continue
}
```

| Injected | Meaning |
|---|---|
| `__instance` | the target's `this` (instance methods only) |
| A prefix that only OBSERVES should return **void**, not `bool`. A `bool`
prefix declares that it *can* skip the original, so for any target returning a
reference type the weaver requires a `ref __result` — otherwise skipping hands
back null, and for a `Task` that is an await on null. A void prefix cannot
skip, needs no `__result`, and the original always runs.

`__arg0`, `__arg1`, ... bind to the target's parameters **by position**,
for targets whose parameter names are unknown — a stack trace gives you the
position but not the name, and a wrong name is a hard weave failure rather
than a skip. Types must match (or be `object`).

`__result`   | the return value; `ref` to change it |
| anything else | must match a target parameter **by name** |

Name matching is stricter than Harmony's, which also matches positionally. A
mismatch is a build error rather than a wrong argument at runtime.

**Not supported**, and rejected with a build error rather than ignored:
`__state`, `__args`, `__originalMethod`, transpilers, finalizers, reverse
patches, open generic methods.

`__state` is the likely migration blocker — if a patch ported from the Android
launcher needs to pass data from prefix to postfix, carry it in a static field
(single-threaded call sites only) or restructure into a single `[SwitchReplace]`.

## Porting the Android patch set

| Android launcher | Here |
|---|---|
| `TouchInputPatches` | Drop. The wrapper feeds touch through Godot's input path already; undocked touchscreen needs no patch. |
| `UiScalePatches` | Rework for TV/handheld docking transitions rather than DPI. |
| `MobileLayoutPatches`, `MerchantLayoutPatches`, `CardRewardPatches`, `EventLayoutPatches` | Re-evaluate. These reflowed for a phone aspect ratio; the Switch is 16:9 like the desktop target, so most are probably unnecessary. |
| `PlatformPatches` | Port. Steam calls still cannot succeed. |
| `AppLifecyclePatches` | Port. Android pause/resume becomes applet focus. |
| `LanMultiplayerPatcher` | Port largely as-is; libnx has BSD sockets and UDP broadcast. |
| `ModelDbInitPatch`, `SaveDiagnosticPatches` | Port, but check for `__state` first. |
| `EarlyAccessDisclaimerPatches` | Port as-is. |
| `ModLoaderPatches` | Probably drop — no filesystem writes to a mods directory on a read-only SD layout. |
| `SettingsPatches`, `CombatBackgroundPatches` | Re-evaluate against Switch perf. |

## Targets are real

Every target name in this directory comes from the Android launcher's own
patch set (`src/STS2Mobile/Patches/`), not from guesswork. That source resolves
types by name via `AccessTools.TypeByName`, so the fully-qualified names were
sitting there in string literals:

```
MegaCrit.Sts2.Core.Nodes.NGame
MegaCrit.Sts2.Core.Models.ModelDb
MegaCrit.Sts2.Core.Saves.PrefsSave
MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler
MegaCrit.Sts2.Core.Nodes.Screens.Settings.NResolutionDropdown
...
```

Targets here are given by **name** for the same reason: this assembly needs no
reference to `sts2.dll`, so it builds without the game present and the circular
assembly reference disappears. A renamed type then fails at weave time with the
name printed, which is strictly better than a compile error somewhere else.

## Workflow

Run the weaver, read what it reports, correct, repeat:

```sh
dotnet run --project ../tools/StS2Patcher -- \
  --game ../aot/assemblies-raw --patches ./bin/Release/net9.0/STS2Switch.dll \
  --out ../aot/assemblies-patched --strict
```
