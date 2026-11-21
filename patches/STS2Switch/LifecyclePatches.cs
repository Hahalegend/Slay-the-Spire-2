// LifecyclePatches.cs -- Switch port of the Android launcher's
// Patches/AppLifecyclePatches.cs.
//
// Android's onPause/onResume become the Switch's applet focus transitions.
// Guarded against EntryPointNotFoundException when paired with an older
// sts2.nro build (e.g. 1.0.0) that does not export sts2_set_game_background
// or sts2_request_quit.
//
// MIT license; see LICENSE.

using System;
using System.Runtime.InteropServices;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Applet focus <-> the game's background mode")]
public static class LifecyclePatches
{
    private static bool _nativeBackgroundMissing;

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler", "EnterBackgroundMode",
                   Optional = true)]
    public static void OnEnterBackground()
    {
        SwitchLog.Info("game entered background mode");
        TrySetBackground(1);
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler", "ExitBackgroundMode",
                   Optional = true)]
    public static void OnExitBackground()
    {
        SwitchLog.Info("game left background mode");
        TrySetBackground(0);
    }

    private static void TrySetBackground(int background)
    {
        if (_nativeBackgroundMissing)
            return;
        try
        {
            NativeSetBackground(background);
        }
        catch (EntryPointNotFoundException)
        {
            _nativeBackgroundMissing = true;
        }
    }

    // NGame.Quit: try the wrapper's clean shutdown hook; if this sts2.nro build
    // does not export sts2_request_quit, fall through to the game's own Quit()
    // so settings save and Godot exits cleanly instead of throwing.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "Quit")]
    public static bool OnQuit()
    {
        SwitchLog.Info("NGame.Quit -> wrapper shutdown");
        try
        {
            NativeRequestQuit();
            return false;
        }
        catch (EntryPointNotFoundException)
        {
            SwitchLog.Info("sts2_request_quit not exported by this sts2.nro; falling back to NGame.Quit");
            return true;
        }
    }

    [DllImport("__Internal", EntryPoint = "sts2_set_game_background")]
    private static extern void NativeSetBackground(int background);

    [DllImport("__Internal", EntryPoint = "sts2_request_quit")]
    private static extern void NativeRequestQuit();
}
