// DisplayPatches.cs -- Switch port of the Android launcher's UiScalePatches.cs
// and SettingsPatches.cs.
//
// The Android versions existed to undo phone-density scaling and to hide
// settings that make no sense without a window manager. The Switch needs less
// of the first and more of the second:
//
//   scaling   NOT wanted. The console is 16:9 at 1280x720 or 1920x1080, the
//             same aspect the desktop build targets, and getScaledDensity
//             already returns 1.0 from jni_fake's call_float. The Android
//             reflow patches (MobileLayoutPatches, MerchantLayoutPatches,
//             CardRewardPatches, EventLayoutPatches) are therefore NOT ported.
//             If something looks wrong at 720p it is a resolution problem, not
//             an aspect problem, and reflowing would make it worse.
//
//   settings  MORE wanted. Resolution and VSync are chosen by the system here
//             and the game must not think it can change them. Docking swaps
//             the render size out from under the game with no user action.
//
// MIT license; see LICENSE.

using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Settings that the console owns, not the game")]
public static class DisplayPatches
{
    // NResolutionDropdown: there is no resolution to choose. Docked and
    // handheld are decided by the dock, and main.c drives GodotLib.resize
    // across the transition.
    //
    // Suppressing PopulateDropdownItems rather than the whole control leaves an
    // empty dropdown rather than a missing one, which is ugly. RefreshEnabled
    // is prefixed too so it renders greyed out -- the honest presentation.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Screens.Settings.NResolutionDropdown",
                  "PopulateDropdownItems", Optional = true)]
    public static bool NoResolutionItems() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Screens.Settings.NResolutionDropdown",
                  "RefreshEnabled", Optional = true)]
    public static bool ResolutionAlwaysDisabled() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Screens.Settings.NResolutionDropdown",
                  "OnDropdownItemSelected", Optional = true)]
    public static bool IgnoreResolutionSelection() => false;

    // NVSyncPaginator.GetVSyncString. VSync is not the game's to set: the
    // wrapper owns the EGL surface and eglSwapBuffers paces the frame. The
    // Android launcher replaced the string too, for the same reason.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Screens.Settings.NVSyncPaginator",
                  "GetVSyncString", Optional = true)]
    public static bool FixedVSyncString(ref string __result)
    {
        __result = "On";
        return false;
    }

    // NGlobalUi / NMainMenu OnWindowChange fire on a resolution change. On
    // Switch that is the docking transition, which is real and DOES need
    // handling -- so these are NOT suppressed. Logged instead, because docking
    // mid-combat is the most likely source of a layout bug and knowing exactly
    // when it happened is worth a line in the log.
    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.CommonUi.NGlobalUi", "OnWindowChange",
                   Optional = true)]
    public static void GlobalUiWindowChanged()
    {
        SwitchLog.Info("NGlobalUi.OnWindowChange (docking transition)");
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.Screens.MainMenu.NMainMenu", "OnWindowChange",
                   Optional = true)]
    public static void MainMenuWindowChanged()
    {
        SwitchLog.Info("NMainMenu.OnWindowChange (docking transition)");
    }

    // SettingsSave.WindowSize -- report the console's actual size, always.
    //
    // The game stores a window size in settings.save and applies it at
    // startup. On this console that value came from somewhere else entirely:
    //
    //   [INFO] Applying display settings...
    //     WINDOW_SIZE: (1920, 1080)          <- from the save
    //   >> newcontext/resize (1280x720)      <- the actual NWindow, handheld
    //
    // Under GLES the mismatch was harmless: the EGL surface was fixed at the
    // real size and the game's number was just a value it carried around.
    // Under Vulkan the swapchain must match the surface, so a game that
    // believes it is 1920x1080 while the window is 1280x720 is a real problem.
    //
    // Patched through the GETTER rather than by writing the save. Writing it
    // would overwrite a returning player's file with console-specific values
    // that are wrong the moment they dock, and wrong again on a desktop copy of
    // the same save. A getter override is true at the moment it is asked and
    // never persists.
    //
    // DisplayServer.WindowGetSize() rather than a P/Invoke into the wrapper:
    // the engine already knows, because main.c drives GodotLib.resize with the
    // real NWindow dimensions, including across docking. One source of truth,
    // and it stays correct when the dock changes it.
    //
    // Metadata name is get_WindowSize, hence MemberKind.PropertyGetter -- the
    // same reason PlatformPatches needs it for PrefsSave.UploadData.
    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.SettingsSave", "WindowSize",
                  Kind = MemberKind.PropertyGetter, Optional = true)]
    //
    // Guarded like SwitchLog.cs, and for the reason stated there: referencing
    // Godot types unconditionally turns a missing GodotSharp reference into a
    // hard compile error, which is a confusing way to discover the assembly
    // path was wrong. Without it the getter is simply not patched, and the
    // game keeps its stored size -- the behaviour before this patch existed.
#if HAVE_GODOTSHARP
    public static bool NativeWindowSize(ref Godot.Vector2I __result)
    {
        try
        {
            var size = Godot.DisplayServer.WindowGetSize();
            if (size.X > 0 && size.Y > 0)
            {
                __result = size;
                return false;
            }
        }
        catch
        {
            // DisplayServer is not available this early in some paths. Fall
            // through to the stored value rather than returning a zero size,
            // which would be worse than the mismatch this exists to fix.
        }
        return true;   // let the original getter run
    }
#else
    public static bool NativeWindowSize() => true;
#endif

    // SettingsSave.ResizeWindows -- there is no window manager. A resizable
    // window is not a thing the console can offer, and NWindowResizeTickbox
    // exists in the settings UI to toggle it.
    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.SettingsSave", "ResizeWindows",
                  Kind = MemberKind.PropertyGetter, Optional = true)]
    public static bool NoWindowResize(ref bool __result)
    {
        __result = false;
        return false;
    }
}
