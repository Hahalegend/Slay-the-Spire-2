// ControllerPatches.cs -- Native Nintendo Switch button mapping & zero-alloc polling.
//
// 1. Fixes swapped X/Y face buttons on Switch:
//    The Switch wrapper (sts2.nro) maps physical Switch buttons by label:
//      A (East)  -> JoyButton.A (0) -> controller_face_button_south (Select)
//      B (South) -> JoyButton.B (1) -> controller_face_button_east  (Cancel)
//      X (North) -> JoyButton.X (2) -> was controller_face_button_west  (WRONG!)
//      Y (West)  -> JoyButton.Y (3) -> was controller_face_button_north (WRONG!)
//
//    Godot's project.binary binds controller_face_button_north to JoyButton 3
//    (Xbox Y) and controller_face_button_west to JoyButton 2 (Xbox X), so
//    pressing physical X opened the Top Panel while physical Y ended the turn!
//
//    Fix:
//      - Remap InputMap's controller_face_button_north -> JoyButton.X (2, Switch X)
//        and controller_face_button_west -> JoyButton.Y (3, Switch Y) once at boot.
//      - Swap SwitchConfig's FaceButtonSouthGlyph -> switch/a.tres and
//        FaceButtonEastGlyph -> switch/b.tres so on-screen prompts match Switch's
//        physical A=Select, B=Cancel, X=End Turn/Confirm, Y=Top Panel layout 100%.
//
// 2. Eliminates per-frame / per-button-press controller allocations:
//    - Controller.AllControllerInputs allocated a fresh 24-element StringName[]
//      every single frame (60x/sec) inside GodotControllerInputStrategy.ProcessInput().
//      Caching it once eliminates all per-frame array churn.
//    - GodotControllerInputStrategy.UpdateControllerConfig() called
//      Godot.Input.GetConnectedJoypads() on every button press, allocating a native
//      Godot.Collections.Array<int> each time. On Switch the controller is always
//      the built-in Switch controller, so once SwitchConfig is active we skip
//      redundant re-probes.

using System;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Fix Nintendo Switch X/Y button swap, align A/B glyphs, and eliminate per-frame controller allocations")]
public static class ControllerPatches
{
#if HAVE_GODOTSHARP
    private static bool s_inputMapPatched;
    private static Godot.StringName[]? s_cachedAllInputs;
    private static bool s_switchConfigLocked;
    private static FieldInfo? s_controllerConfigField;
    private static bool s_controllerConfigFieldResolved;

    // ------------------------------------------------------------------
    // 1. Remap controller_face_button_north <-> west in Godot.InputMap
    //    so physical Switch X = North (End Turn / Confirm) and
    //    physical Switch Y = West (Top Panel).
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.ControllerInput.GodotControllerInputStrategy", "Init", Optional = true)]
    public static void FixSwitchInputMapOnStrategyInit()
    {
        EnsureSwitchInputMap();
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.CommonUi.NInputManager", "_Ready", Optional = true)]
    public static void FixSwitchInputMapOnInputManagerReady()
    {
        EnsureSwitchInputMap();
    }

    private static void EnsureSwitchInputMap()
    {
        if (s_inputMapPatched)
            return;

        try
        {
            var north = new Godot.StringName("controller_face_button_north");
            var west = new Godot.StringName("controller_face_button_west");
            if (!Godot.InputMap.HasAction(north) || !Godot.InputMap.HasAction(west))
                return;

            SetActionJoyButton(north, Godot.JoyButton.X); // 2 = physical Switch X (North)
            SetActionJoyButton(west, Godot.JoyButton.Y);  // 3 = physical Switch Y (West)
            s_inputMapPatched = true;
            SwitchLog.Info("ControllerPatches: remapped Switch face buttons (A=Select, B=Cancel, X=North/EndTurn, Y=West/TopPanel)");
        }
        catch (Exception ex)
        {
            SwitchLog.Error($"ControllerPatches: failed to remap InputMap ({ex.Message})");
        }
    }

    private static void SetActionJoyButton(Godot.StringName action, Godot.JoyButton targetButton)
    {
        var events = Godot.InputMap.ActionGetEvents(action);
        bool updated = false;
        for (int i = 0; i < events.Count; i++)
        {
            if (events[i] is Godot.InputEventJoypadButton jb)
            {
                jb.ButtonIndex = targetButton;
                updated = true;
            }
        }

        if (!updated)
        {
            var jb = new Godot.InputEventJoypadButton { ButtonIndex = targetButton };
            Godot.InputMap.ActionAddEvent(action, jb);
        }
    }

    // ------------------------------------------------------------------
    // 2. Align SwitchConfig on-screen A/B glyphs with physical Switch A/B
    //    (sts2.nro maps Switch A -> South/Select, Switch B -> East/Cancel).
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.ControllerInput.ControllerConfigs.SwitchConfig",
                  "FaceButtonSouthGlyph", Kind = MemberKind.PropertyGetter, Optional = true)]
    public static bool SwitchSouthGlyphIsA(ref string __result)
    {
        __result = "res://images/atlases/controller_atlas.sprites/switch/a.tres";
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.ControllerInput.ControllerConfigs.SwitchConfig",
                  "FaceButtonEastGlyph", Kind = MemberKind.PropertyGetter, Optional = true)]
    public static bool SwitchEastGlyphIsB(ref string __result)
    {
        __result = "res://images/atlases/controller_atlas.sprites/switch/b.tres";
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Cache Controller.AllControllerInputs (avoids allocating a new
    //    24-element StringName[] 60 times/sec inside ProcessInput).
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.ControllerInput.Controller",
                  "AllControllerInputs", Kind = MemberKind.PropertyGetter, Optional = true)]
    public static bool ReuseCachedControllerInputs(ref Godot.StringName[] __result)
    {
        if (s_cachedAllInputs is not null)
        {
            __result = s_cachedAllInputs;
            return false;
        }
        return true;
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.ControllerInput.Controller",
                   "AllControllerInputs", Kind = MemberKind.PropertyGetter, Optional = true)]
    public static void CaptureCachedControllerInputs(Godot.StringName[] __result)
    {
        if (s_cachedAllInputs is null && __result is not null && __result.Length > 0)
            s_cachedAllInputs = __result;
    }

    // ------------------------------------------------------------------
    // 4. Once SwitchConfig is active, skip redundant Input.GetConnectedJoypads()
    //    native array allocations on every single button press.
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.ControllerInput.GodotControllerInputStrategy",
                  "UpdateControllerConfig", Optional = true)]
    public static bool SkipRedundantJoypadProbe(object __instance)
    {
        if (!s_switchConfigLocked)
            return true;

        if (!_IsConfigPresent(__instance))
        {
            s_switchConfigLocked = false;
            return true;
        }

        return false;
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.ControllerInput.GodotControllerInputStrategy",
                   "UpdateControllerConfig", Optional = true)]
    public static void LockOnceSwitchConfigDetected(object __instance)
    {
        if (s_switchConfigLocked)
            return;

        if (!s_controllerConfigFieldResolved)
        {
            s_controllerConfigField = __instance.GetType().GetField(
                "_controllerConfig",
                BindingFlags.Instance | BindingFlags.NonPublic);
            s_controllerConfigFieldResolved = true;
        }

        var cfg = s_controllerConfigField?.GetValue(__instance);
        if (cfg is not null && cfg.GetType().Name == "SwitchConfig")
            s_switchConfigLocked = true;
    }

    private static bool _IsConfigPresent(object instance)
    {
        if (!s_controllerConfigFieldResolved)
            return false;
        return s_controllerConfigField?.GetValue(instance) is not null;
    }
#endif
}
