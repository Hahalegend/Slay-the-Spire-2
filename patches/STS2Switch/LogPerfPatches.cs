// LogPerfPatches.cs -- Suppress repetitive combat/gameplay SD-card log spam
// while preserving all boot diagnostics, room transitions, [sts2] notices,
// and error messages.
//
// In Slay the Spire 2, every card played, enemy move, card rarity roll,
// reward pickup, and asset cache miss writes a formatted line to godot.log
// on the Switch microSD card and allocates interpolated strings on the main
// thread. Silencing hot gameplay log noise eliminates unnecessary microSD
// writes and GC churn during combat.

using System;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Silence hot combat/gameplay SD-card log spam while preserving startup & errors")]
public static class LogPerfPatches
{
    private static bool _pastStartup;

    [SwitchPrefix("MegaCrit.Sts2.Core.Logging.Logger", "Debug", Optional = true)]
    public static bool SkipDebugLog() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Logging.Logger", "VeryDebug", Optional = true)]
    public static bool SkipVeryDebugLog() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Logging.Logger", "Info", Optional = true)]
    public static bool FilterNoisyGameplayInfo(string __arg0)
    {
        if (string.IsNullOrEmpty(__arg0))
            return false;

        if (!_pastStartup)
        {
            if (__arg0.Contains("[Startup] Time to main menu", StringComparison.Ordinal))
                _pastStartup = true;
            return true;
        }

        // Suppress per-card, per-move, per-roll, per-reward, and routine save/input spam.
        if (__arg0.StartsWith("Player ", StringComparison.Ordinal) ||
            __arg0.StartsWith("Monster ", StringComparison.Ordinal) ||
            __arg0.StartsWith("Card rarity:", StringComparison.Ordinal) ||
            __arg0.StartsWith("Wrote ", StringComparison.Ordinal) ||
            __arg0.StartsWith("CONTROLLER DETECTED", StringComparison.Ordinal) ||
            __arg0.StartsWith("MOUSE DETECTED", StringComparison.Ordinal) ||
            __arg0.StartsWith("Max ascension changed", StringComparison.Ordinal))
        {
            return false;
        }

        return true;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Logging.Logger", "Warn", Optional = true)]
    public static bool FilterNoisyAssetMissWarn(string __arg0)
    {
        if (!_pastStartup || string.IsNullOrEmpty(__arg0))
            return true;

        // Avoid synchronous SD-card log writes whenever a deferred asset loads on demand.
        if (__arg0.StartsWith("Asset not cached:", StringComparison.Ordinal))
            return false;

        return true;
    }
}
