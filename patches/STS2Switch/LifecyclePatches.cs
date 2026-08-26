// LifecyclePatches.cs -- Switch port of the Android launcher's
// Patches/AppLifecyclePatches.cs.
//
// Android's onPause/onResume become the Switch's applet focus transitions.
// main.c already tracks those (s_focused) and already pauses the audio mixer
// and stops rumble on the native side; what these add is telling the GAME, so
// it can pause its own simulation rather than running a combat turn while the
// console is asleep.
//
// The direction is native -> managed, which is the opposite of every other
// patch here. The wrapper cannot call into the game's C# directly, so the
// native side sets a flag and a woven hook reads it. See NotifyFocus below.
//
// MIT license; see LICENSE.

using System.Runtime.InteropServices;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Applet focus <-> the game's background mode")]
public static class LifecyclePatches
{
    // NBackgroundModeHandler is what the Android launcher hooked. Both are
    // Optional: the type is resolved out of the game assembly at runtime by the
    // launcher (Assembly.GetType), which means it may not exist in every build,
    // and a hard failure here would block the whole weave over something that
    // only degrades sleep behaviour.
    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler", "EnterBackgroundMode",
                   Optional = true)]
    public static void OnEnterBackground()
    {
        SwitchLog.Info("game entered background mode");
        NativeSetBackground(1);
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Nodes.NBackgroundModeHandler", "ExitBackgroundMode",
                   Optional = true)]
    public static void OnExitBackground()
    {
        SwitchLog.Info("game left background mode");
        NativeSetBackground(0);
    }

    // NGame.Quit.
    //
    // The Android launcher redirected this to restart the app, because on
    // Android killing the process drops you to the launcher's own UI and the
    // user would have to re-enter it. On Switch, quitting to the Homebrew Menu
    // is the correct and expected behaviour -- so this does NOT restart. It
    // routes through the wrapper's own shutdown so saves flush and the
    // GDExtensions get their teardown, rather than letting the game call
    // OS.Kill and take the process down under us.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "Quit")]
    public static bool OnQuit()
    {
        SwitchLog.Info("NGame.Quit -> wrapper shutdown");
        NativeRequestQuit();
        return false;   // do not run the original
    }

    [DllImport("__Internal", EntryPoint = "sts2_set_game_background")]
    private static extern void NativeSetBackground(int background);

    [DllImport("__Internal", EntryPoint = "sts2_request_quit")]
    private static extern void NativeRequestQuit();
}
