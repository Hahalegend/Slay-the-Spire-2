// PerformancePatches.cs -- Enforce consistent Switch frame pacing, lower
// physics/GC overhead, and prevent desktop settings.save defaults from
// overriding Switch-tuned VSync / FPS caps.
//
// Why this matters on Tegra X1 (4x Cortex-A57 @ 1.02 GHz, GM20B @ 384 MHz):
//   - NGame.InitializeGraphicsPreferences() runs after override.cfg and overwrote
//     override.cfg's VSync=On and MaxFps=30 with VSync=Off and MaxFps=60.
//     Uncapped/60fps VSync-Off saturates the GPU & NVK queue, causing wild
//     16ms -> 60ms frame-time swings (visible stutter/judder).
//   - Locking to VSync=Enabled + 30 FPS gives every frame a generous 33.3ms
//     budget so Spine animations, card drags, and UI updates stay dead-flat.
//     (Players running sys-clk can still raise FPS Limit live in Settings.)
//   - Cutting Engine.PhysicsTicksPerSecond from 60 -> 30 halves idle 2D physics
//     CPU overhead in a turn-based card game.
//   - Setting GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency keeps
//     blocking Gen 2 GC sweeps out of active combat (PreloadManager's explicit
//     GC.Collect() still cleans up cleanly during room fade-outs).

using System;
using System.Runtime;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Consistent Switch frame pacing, 30Hz physics, and low-latency GC")]
public static class PerformancePatches
{
    private static bool _gcTuned;

    // ------------------------------------------------------------------
    // 1. Always keep VSync Enabled on Switch (prevents NVK queue saturation
    //    and tearing/judder when settings.save has VSync=Off).
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "ApplySyncSetting")]
    public static bool ForceVsyncOn()
    {
#if HAVE_GODOTSHARP
        try
        {
            Godot.DisplayServer.WindowSetVsyncMode(Godot.DisplayServer.VSyncMode.Enabled, 0);
            SwitchLog.Info("PerformancePatches: enforced VSync=Enabled");
        }
        catch { }
#endif
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Enforce Switch frame pacing, 30 Hz physics, and low-latency GC
    //    right after NGame.InitializeGraphicsPreferences() runs.
    // ------------------------------------------------------------------
    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NGame", "InitializeGraphicsPreferences")]
    public static void TuneGraphicsAndRuntime()
    {
        ApplySwitchRuntimeTuning("startup");
    }

    // Re-apply 30 FPS cap when returning from sleep/HOME (ExitBackgroundMode
    // restores SettingsSave.FpsLimit which may be 60/0).
    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler", "ExitBackgroundMode",
                   Optional = true)]
    public static void ReapplyAfterBackground()
    {
        ApplySwitchRuntimeTuning("resume");
    }

    private static void ApplySwitchRuntimeTuning(string reason)
    {
#if HAVE_GODOTSHARP
        try
        {
            // Default to locked 30 FPS for rock-solid 33.3ms frame pacing on
            // stock Switch clocks (can be overridden via STS2_MAX_FPS env or
            // live via in-game Settings -> FPS Limit when using sys-clk).
            int targetFps = 30;
            var envFps = Environment.GetEnvironmentVariable("STS2_MAX_FPS");
            if (!string.IsNullOrEmpty(envFps) && int.TryParse(envFps, out int parsed) && parsed >= 20 && parsed <= 60)
                targetFps = parsed;

            Godot.Engine.MaxFps = targetFps;
            Godot.Engine.PhysicsTicksPerSecond = 30;
            Godot.Engine.MaxPhysicsStepsPerFrame = 2;

            // Ensure root viewport never burns GM20B fillrate on MSAA / SSAA / debanding.
            if (Godot.Engine.GetMainLoop() is Godot.SceneTree tree && tree.Root is Godot.Window root)
            {
                root.Msaa2D = Godot.Viewport.Msaa.Disabled;
                root.Msaa3D = Godot.Viewport.Msaa.Disabled;
                root.ScreenSpaceAA = Godot.Viewport.ScreenSpaceAAEnum.Disabled;
                root.UseDebanding = false;
            }

            SwitchLog.Info($"PerformancePatches ({reason}): MaxFps={targetFps}, PhysicsHz=30, VSync=Enabled, AA=Off");
        }
        catch { }
#endif

        if (!_gcTuned)
        {
            _gcTuned = true;
            try
            {
                GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
                SwitchLog.Info("PerformancePatches: GCSettings.LatencyMode = SustainedLowLatency");
            }
            catch { }
        }
    }
}
