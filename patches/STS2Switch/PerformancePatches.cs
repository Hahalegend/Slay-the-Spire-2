// PerformancePatches.cs -- Switch runtime tuning backed by hardware A/B tests
// (boot_stats 2 vs 5) plus a native-crash guard on Godot.Control.GrabFocus().
//
// Key hardware findings on Tegra X1 + NVK + wsi_switch:
//   1. main.c already configures Switch NWindow with swap_interval=1 (60 Hz
//      hardware VSync). Enabling Godot's Vulkan FIFO VSync + MaxFps=30 on top
//      caused double-waiting (110-135ms stalls every 2-3 frames, 1.99% stall
//      rate vs 0.19% with VSyncMode.Disabled + MaxFps=60, which ran 7,668
//      consecutive frames with zero >100ms stalls).
//   2. Crash report 01795245857 traced a mid-combat crash directly to
//      NControllerCardPlay.Start -> NodeUtil.TryGrabFocus -> Control.GrabFocus
//      -> Viewport::_gui_remove_focus_for_window when called on a detached /
//      freed / FocusMode.None control. Guarding Control.GrabFocus() both
//      prevents that crash and eliminates the 35-frame C# backtrace warning
//      stutters logged to SD card.
//   3. Cutting Engine.PhysicsTicksPerSecond from 60 -> 30 reduces idle 2D
//      physics CPU overhead without affecting turn-based card gameplay.

using System;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Switch frame pacing (single-sync via NWindow), 30Hz physics, safe GrabFocus, and quiet PacketWriter")]
public static class PerformancePatches
{
    private static PropertyInfo? _warnOnGrowProp;
    private static bool _warnOnGrowResolved;

    // ------------------------------------------------------------------
    // 0. Silence PacketWriter 6x buffer-grow warning log spam after combats.
    // ------------------------------------------------------------------
    [SwitchPostfix("MegaCrit.Sts2.Core.Multiplayer.Serialization.PacketWriter", ".ctor", Optional = true)]
    public static void SilencePacketWriterGrowWarnings(object __instance)
    {
        if (!_warnOnGrowResolved)
        {
            _warnOnGrowProp = __instance.GetType().GetProperty("WarnOnGrow");
            _warnOnGrowResolved = true;
        }
        try
        {
            _warnOnGrowProp?.SetValue(__instance, false);
        }
        catch { }
    }
    // ------------------------------------------------------------------
    // 1. Guard Control.GrabFocus() against detached/invalid/unfocusable controls.
    //    Fixes 01795245857 (Viewport::_gui_remove_focus_for_window crash) and
    //    eliminates "This control can't grab focus" backtrace SD-card stutters.
    // ------------------------------------------------------------------
    [SwitchPrefix("Godot.Control", "GrabFocus", Optional = true)]
    public static bool SafeGrabFocus(object __instance)
    {
#if HAVE_GODOTSHARP
        if (__instance is not Godot.Control c)
            return false;
        if (!Godot.GodotObject.IsInstanceValid(c) || c.IsQueuedForDeletion())
            return false;
        if (!c.IsInsideTree() || !c.IsVisibleInTree())
            return false;
        if (c.FocusMode == Godot.Control.FocusModeEnum.None)
            return false;
        if (c.GetFocusModeWithOverride() == Godot.Control.FocusModeEnum.None)
            return false;
        var vp = c.GetViewport();
        if (vp is null || !Godot.GodotObject.IsInstanceValid(vp) || !vp.IsInsideTree())
            return false;
#endif
        return true;
    }

    // ------------------------------------------------------------------
    // 2. Keep Godot Vulkan VSync Disabled so wsi_switch / NWindow's
    //    swap_interval=1 handles display sync once without double-blocking.
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "ApplySyncSetting")]
    public static bool SingleSyncViaNWindow()
    {
#if HAVE_GODOTSHARP
        try
        {
            Godot.DisplayServer.WindowSetVsyncMode(Godot.DisplayServer.VSyncMode.Disabled, 0);
            SwitchLog.Info("PerformancePatches: Godot VSync=Disabled (single-sync via Switch NWindow swap_interval=1)");
        }
        catch { }
#endif
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Tune MaxFps, 30 Hz physics, and root viewport AA after display init.
    // ------------------------------------------------------------------
    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NGame", "InitializeGraphicsPreferences")]
    public static void TuneGraphicsAndRuntime()
    {
        ApplySwitchRuntimeTuning("startup");
    }

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
            int targetFps = 60;
            var envFps = Environment.GetEnvironmentVariable("STS2_MAX_FPS");
            if (!string.IsNullOrEmpty(envFps) && int.TryParse(envFps, out int parsed) && parsed >= 20 && parsed <= 60)
                targetFps = parsed;

            Godot.Engine.MaxFps = targetFps;
            Godot.Engine.PhysicsTicksPerSecond = 30;
            Godot.Engine.MaxPhysicsStepsPerFrame = 2;

            if (Godot.Engine.GetMainLoop() is Godot.SceneTree tree)
            {
                ScreenVfxPatches.InstallTreeGuard(tree);
                if (tree.Root is Godot.Window root)
                {
                    root.Msaa2D = Godot.Viewport.Msaa.Disabled;
                    root.Msaa3D = Godot.Viewport.Msaa.Disabled;
                    root.ScreenSpaceAA = Godot.Viewport.ScreenSpaceAAEnum.Disabled;
                    root.UseDebanding = false;
                }
            }

            SwitchLog.Info($"PerformancePatches ({reason}): MaxFps={targetFps}, PhysicsHz=30, SingleSync=NWindow, AA=Off");
        }
        catch { }
#endif
    }
}
