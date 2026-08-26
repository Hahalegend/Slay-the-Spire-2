// SavePatches.cs -- Switch port of the Android launcher's
// Patches/SaveDiagnosticPatches.cs.
//
// ===========================================================================
// The original is a transpiler. This is not.
// ===========================================================================
//
// The Android version rewrites ProgressSaveManager.LoadProgress's IL, walking
// the instruction stream for the callvirt to MigrationManager.LoadSave and
// splicing in a Dup + Call to log the returned object. The static weaver
// rejects transpilers outright, and correctly: pattern-matching an opcode
// sequence at build time against an assembly that changes every game patch is
// exactly the brittleness this pipeline exists to remove.
//
// What it was FOR still matters though. Its comment says it plainly: "trace
// why the game creates a fresh default save instead of loading the pulled
// one." A save that silently does not load, and gets overwritten by a fresh
// default on the next write, is the worst class of bug this port can ship.
//
// So the intent is kept and the mechanism is not. A postfix on LoadProgress
// reports the outcome, which answers the same question from outside the method
// rather than from inside it. Less precise -- it cannot say WHICH of the two
// call sites produced the default -- but it survives a game update, which the
// transpiler would not.
//
// ===========================================================================
// Where saves live
// ===========================================================================
//
// GodotIO.getDataDir() returns config.save_root (jni_fake.c), so user:// maps
// to <data_root>/save, and app_paths.c derives data_root at runtime from
// argv[0] / cwd / a scan of /switch. Renaming the install directory therefore
// moves the saves with it -- /switch/sts2_nx and /switch/slaythespire2 both
// work, and neither is compiled in anywhere.
//
// MIT license; see LICENSE.

using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Save load diagnostics; transpiler replaced with a postfix")]
public static class SavePatches
{
    // ProgressSaveManager.LoadProgress.
    //
    // __result is the loaded progress object. Null, or an object that is
    // reference-equal to a freshly constructed default, means the load did not
    // find anything -- which on Switch usually means user:// resolved
    // somewhere unexpected rather than the save being corrupt.
    [SwitchPostfix("MegaCrit.Sts2.Core.Saves.Managers.ProgressSaveManager", "LoadProgress",
                   Optional = true)]
    public static void AfterLoadProgress(ref object __result)
    {
        if (__result is null)
        {
            SwitchLog.Error("LoadProgress returned null -- a fresh default will be "
                          + "written and any existing save overwritten. Check that "
                          + "user:// resolves under <data_root>/save.");
        }
        else
        {
            SwitchLog.Info($"LoadProgress -> {__result.GetType().Name}");
        }
    }

    // SaveManager.ConstructDefault.
    //
    // Not suppressed -- a genuinely new player needs it. Logged, because a call
    // to it immediately after a LoadProgress that should have succeeded is the
    // signature of the exact failure above, and the two lines together say more
    // than either does alone.
    [SwitchPostfix("MegaCrit.Sts2.Core.Saves.SaveManager", "ConstructDefault",
                   Optional = true)]
    public static void AfterConstructDefault()
    {
        SwitchLog.Info("SaveManager.ConstructDefault -- new save being created");
    }

    // SaveManager.InitSettingsData.
    //
    // The Android launcher patched this to force mobile-appropriate defaults.
    // Here it is only logged: the console's own settings (resolution, VSync)
    // are already suppressed in DisplayPatches, and forcing anything else would
    // overwrite a returning player's preferences on every boot.
    [SwitchPostfix("MegaCrit.Sts2.Core.Saves.SaveManager", "InitSettingsData",
                   Optional = true)]
    public static void AfterInitSettings()
    {
        SwitchLog.Info("SaveManager.InitSettingsData complete");
    }
}
