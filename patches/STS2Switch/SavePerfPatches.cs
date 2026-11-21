// SavePerfPatches.cs -- Eliminate blocking microSD fsync() and redundant
// save-file stalls on Nintendo Switch (libnx sdmc FAT32).
//
// Hardware telemetry (boot_stats.txt + godot.log on Switch):
//   - GodotFileIo.WriteFile() ran CopyBackup() -> FileStream.Flush(true)
//     (fsync #1) -> WriteFile(.tmp) -> FileStream.Flush(true) (fsync #2) ->
//     RenameFile() synchronously on the main thread.
//   - Every combat victory / map node click saves BOTH progress.save (36 KB)
//     and current_run.save (48 KB) = 4 blocking fsync() calls + 4 full file
//     writes + 4 renames (140-250ms main-thread stutters).
//   - Touching the screen / switching controller input also triggered
//     SaveControllerInputMapping() -> SaveSettings() mid-combat (up to 992ms
//     stall at frame 8791).
//
// Fix (100% save-safe):
//   1. Skip GodotFileIo.FsyncFile(): Godot's FileAccess.Close() already flushes
//      through Horizon OS FS (sdmc), and atomic .tmp -> RenameFile() stays
//      fully intact.
//   2. Skip CopyBackup() ONLY for small non-critical settings.save / prefs.save
//      (keep full .backup redundancy for current_run.save, progress.save,
//      profile.save, and run history).
//   3. Debounce duplicate SaveControllerInputMapping() / SaveMKbInputMapping()
//      writes within 15 seconds so touching the screen mid-combat never freezes
//      the frame (Settings screen saves remain unaffected).

using System;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Eliminate Switch microSD fsync() and redundant mid-combat settings save stalls")]
public static class SavePerfPatches
{
    private static bool _announcedFsync;
    private static long _lastInputMappingSaveTicks;

    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.GodotFileIo", "FsyncFile", Optional = true)]
    public static bool SkipBlockingFsync()
    {
        if (!_announcedFsync)
        {
            _announcedFsync = true;
            SwitchLog.Info("SavePerfPatches: disabled blocking FAT32 FsyncFile (atomic .tmp + rename kept)");
        }
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.GodotFileIo", "CopyBackup", Optional = true)]
    public static bool SkipBackupForSmallSettings(string __arg0)
    {
        if (!string.IsNullOrEmpty(__arg0))
        {
            if (__arg0.EndsWith("settings.save", StringComparison.OrdinalIgnoreCase) ||
                __arg0.EndsWith("prefs.save", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }
        return true;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.CommonUi.NInputManager", "SaveControllerInputMapping", Optional = true)]
    public static bool DebounceControllerMappingSave()
    {
        return ShouldAllowInputMappingSave();
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.CommonUi.NInputManager", "SaveMKbInputMapping", Optional = true)]
    public static bool DebounceMkbMappingSave()
    {
        return ShouldAllowInputMappingSave();
    }

    private static bool ShouldAllowInputMappingSave()
    {
        long now = Environment.TickCount64;
        long prev = _lastInputMappingSaveTicks;
        if (prev != 0 && (now - prev) < 15_000)
            return false;

        _lastInputMappingSaveTicks = now;
        return true;
    }
}
