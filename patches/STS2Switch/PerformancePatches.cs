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
using System.Runtime;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Switch frame pacing (single-sync via NWindow), 30Hz physics, safe GrabFocus, zero-alloc NTrail2D, lazy map SubViewport, and quiet PacketWriter")]
public static class PerformancePatches
{
    private static PropertyInfo? _warnOnGrowProp;
    private static bool _warnOnGrowResolved;

#if HAVE_GODOTSHARP
    private static FieldInfo? _trailActiveField;
    private static FieldInfo? _trailParentField;
    private static FieldInfo? _trailQueueField;
    private static FieldInfo? _trailMaxSegField;
    private static bool _trailFieldsResolved;
    private static readonly Godot.Vector2[]?[] TrailPointBuffers = new Godot.Vector2[129][];

    private static FieldInfo? _drawingViewportField;
    private static bool _drawingViewportResolved;

    // ------------------------------------------------------------------
    // 0a. Zero-allocation NTrail2D._Process replacement.
    //     Stock NTrail2D._Process allocates a Func<Vector2,Vector2> closure +
    //     LINQ Select iterator + Vector2[] and calls GetParent() per point on
    //     every frame while cards fly across the screen. Reusing a length-keyed
    //     Vector2[] buffer (copied natively by GodotSharp marshaling) eliminates
    //     all per-frame managed heap allocations on card trails.
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Vfx.Utilities.NTrail2D", "_Process", Optional = true)]
    public static bool FastTrailProcess(object __instance)
    {
        if (__instance is not Godot.Line2D line)
            return true;

        if (!_trailFieldsResolved)
        {
            var t = __instance.GetType();
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
            _trailActiveField = t.GetField("_isActive", flags);
            _trailParentField = t.GetField("_parent", flags);
            _trailQueueField = t.GetField("_pointQueue", flags);
            _trailMaxSegField = t.GetField("_maxSegments", flags);
            _trailFieldsResolved = true;
        }

        if (_trailActiveField is null || _trailParentField is null ||
            _trailQueueField is null || _trailMaxSegField is null)
            return true;

        if (_trailActiveField.GetValue(__instance) is not true)
            return false;

        if (_trailParentField.GetValue(__instance) is not Godot.Node2D parent ||
            !Godot.GodotObject.IsInstanceValid(parent))
            return false;

        if (_trailQueueField.GetValue(__instance) is not System.Collections.Generic.List<Godot.Vector2> queue)
            return false;

        int maxSeg = (int)(_trailMaxSegField.GetValue(__instance) ?? 0);
        queue.Insert(0, parent.GlobalPosition);
        if (queue.Count >= maxSeg && queue.Count > 0)
            queue.RemoveAt(queue.Count - 1);

        var parentNode = line.GetParent() as Godot.Node2D;
        if (parentNode is null || !Godot.GodotObject.IsInstanceValid(parentNode))
            return false;

        int count = queue.Count;
        Godot.Vector2[] pts;
        if ((uint)count < (uint)TrailPointBuffers.Length)
        {
            pts = TrailPointBuffers[count] ??= new Godot.Vector2[count];
        }
        else
        {
            pts = new Godot.Vector2[count];
        }

        for (int i = 0; i < count; i++)
            pts[i] = parentNode.ToLocal(queue[i]);

        line.Points = pts;
        return false;
    }

    // ------------------------------------------------------------------
    // 0b. Avoid rendering the 960x1620 MapDrawing SubViewport at 60 FPS
    //     while the player is simply browsing/scrolling the map without
    //     drawing. Enable Always during BeginLine, switch back to Once on
    //     StopDrawingLine / ClearAllLinesForPlayer.
    // ------------------------------------------------------------------
    private static Godot.SubViewport? GetDrawingViewport(object? drawingState)
    {
        if (drawingState is null)
            return null;

        if (!_drawingViewportResolved)
        {
            _drawingViewportField = drawingState.GetType().GetField(
                "drawViewport", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            _drawingViewportResolved = true;
        }

        return _drawingViewportField?.GetValue(drawingState) as Godot.SubViewport;
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Screens.Map.NMapDrawings", "BeginLine", Optional = true)]
    public static void EnableMapViewportWhileDrawing(object __arg0)
    {
        var vp = GetDrawingViewport(__arg0);
        if (vp is not null && Godot.GodotObject.IsInstanceValid(vp))
            vp.RenderTargetUpdateMode = Godot.SubViewport.UpdateMode.Always;
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Screens.Map.NMapDrawings", "StopDrawingLine", Optional = true)]
    public static void PauseMapViewportAfterDrawing(object __arg0)
    {
        var vp = GetDrawingViewport(__arg0);
        if (vp is not null && Godot.GodotObject.IsInstanceValid(vp))
            vp.RenderTargetUpdateMode = Godot.SubViewport.UpdateMode.Once;
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Screens.Map.NMapDrawings", "ClearAllLinesForPlayer", Optional = true)]
    public static void RefreshMapViewportOnceOnClear(object __arg0)
    {
        var vp = GetDrawingViewport(__arg0);
        if (vp is not null && Godot.GodotObject.IsInstanceValid(vp))
            vp.RenderTargetUpdateMode = Godot.SubViewport.UpdateMode.Once;
    }
#endif

    // ------------------------------------------------------------------
    // 0. Silence PacketWriter 6x buffer-grow warning log spam after combats.
    //    (Note: PreloadManager.LoadRoomAssets / LoadActAssets / LoadRunAssets
    //    already execute GC.Collect() at the end of every room/act load while
    //    faded out, while SustainedLowLatency below prevents blocking Gen2
    //    collections during active combat.)
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

            try
            {
                GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
            }
            catch { }

            SwitchLog.Info($"PerformancePatches ({reason}): MaxFps={targetFps}, PhysicsHz=30, SingleSync=NWindow, AA=Off, GC=SustainedLowLatency");
        }
        catch { }
#endif
    }
}
