using System;
// PlatformPatches.cs -- Switch port of the Android launcher's
// Patches/PlatformPatches.cs.
//
// Target names are REAL, taken from the launcher source rather than invented.
// Targets are given by NAME so this assembly needs no reference to sts2.dll:
// it builds without the game present, and a renamed type fails at weave time
// with the name printed instead of failing to compile.
//
// MIT license; see LICENSE.

using System.Threading.Tasks;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Desktop-only platform features that cannot work on Switch")]
public static class PlatformPatches
{
    // NGame.InitializePlatform brings up Steam. libsteam_api.so is stubbed in
    // the wrapper's import table, so the P/Invokes return safe defaults -- but
    // the managed init path still awaits a callback that will never arrive.
    //
    // NOT an ownership bypass: ownership is established when the depot is
    // downloaded on a machine that IS signed in. By the time these assemblies
    // reach the SD card that has already happened. What this removes is a
    // runtime call that cannot succeed on a device with no Steam client.
    // SwitchCulture.Ensure() is called from several early entry points because
    // the weave offers no "before everything" hook and the culture must be set
    // before LocManager's constructor. It is idempotent, so whichever runs
    // first wins and the rest are free.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "InitializePlatform")]
    public static bool InitializePlatform(ref Task<bool> __result)
    {
        SwitchCulture.Ensure();
        AstcOverlay.Ensure();

        __result = Task.FromResult(true);
        return false;
    }

    // OsDebugInfo.LogSystemInfo queries desktop-shaped system information.
    //
    // Returns Task, NOT void (confirmed via --list-types). A skipping prefix
    // that does not set __result leaves the Task local at its default of null,
    // and the caller awaits it -- NullReferenceException at startup, from a
    // patch whose entire purpose is to suppress a log line.
    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.OsDebugInfo", "LogSystemInfo", Optional = true)]
    public static bool SkipSystemInfo(ref Task __result)
    {
        __result = Task.CompletedTask;
        return false;
    }

    // SentryService.
    //
    // Patching Initialize alone was not enough. Run 11 caught the real path:
    //
    //   at MegaCrit.Sts2.Core.Debug.SentryService..cctor() + 0x43c
    //   at ClassConstructorRunner.EnsureClassConstructorRun(...)
    //   --- End of inner exception stack trace ---
    //   at ClassConstructorRunner.CheckStaticClassConstructionReturnNonGCStaticBase(...)
    //   at MegaCrit.Sts2.Core.Debug.SentryService.Shutdown() + 0x138
    //
    // The STATIC CONSTRUCTOR throws, because res://addons/sentry/sentry.gdextension
    // declares no android.arm64 library and never loaded. Any static member
    // access triggers it -- Shutdown() did here, and skipping Initialize does
    // nothing about that, because a cctor runs on first touch of the type, not
    // on the method we happened to patch.
    //
    // So the cctor itself is replaced. Its statics are then left at their
    // default values, which is fine precisely because every method that would
    // read them is also suppressed below.
    [SwitchReplace("MegaCrit.Sts2.Core.Debug.SentryService", ".cctor", Optional = true)]
    public static void NoSentryCctor() { }

    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "Initialize", Optional = true)]
    public static bool SkipSentryInit() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "Shutdown", Optional = true)]
    public static bool SkipSentryShutdown() => false;

    // CaptureException has two overloads, and the weaver refused to guess --
    // correctly. Disambiguated by NAME rather than typeof(), because the second
    // takes Action<Sentry.Scope> and typeof'ing it would mean referencing
    // Sentry.dll, which is the compile-time dependency name-based targeting
    // exists to avoid. The strings are exactly what the ambiguity error listed
    // as candidates.
    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "CaptureException",
                  ArgumentTypeNames = new[] { "System.Exception" }, Optional = true)]
    public static bool SkipSentryCapture1(Exception __arg0)
    {
        // Already bound to this signature, with the exception right there. It
        // was being discarded. The game calls CaptureException for its own
        // error reporting -- run 14 reached it from SaveManager.SaveSettings --
        // so this is a free window onto whatever the game considers wrong,
        // without patching anything new.
        SwitchLog.Error("game reported: " + (__arg0?.ToString() ?? "(null)"));
        return false;   // still skipped; Sentry itself cannot work here
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "CaptureException",
                  ArgumentTypeNames = new[] { "System.Exception", "System.Action`1<Sentry.Scope>" },
                  Optional = true)]
    public static bool SkipSentryCapture2(Exception __arg0)
    {
        SwitchLog.Error("game reported: " + (__arg0?.ToString() ?? "(null)"));
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "CaptureMessage", Optional = true)]
    public static bool SkipSentryMessage() => false;

    [SwitchPrefix("MegaCrit.Sts2.Core.Debug.SentryService", "AddBreadcrumb", Optional = true)]
    public static bool SkipSentryBreadcrumb() => false;

    // PrefsSave.UploadData -- telemetry opt-in. Patched through the GETTER,
    // which is why MemberKind exists: in metadata this is get_UploadData and
    // is not reachable by plain method name.
    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.PrefsSave", "UploadData",
                  Kind = MemberKind.PropertyGetter, Optional = true)]
    public static bool NoTelemetry(ref bool __result)
    {
        __result = false;
        return false;
    }

    // GodotFileIo.CreateDirectory.
    //
    // NullPlatformUtilStrategy's constructor calls CreateDirectory(".") and "."
    // is not a valid absolute Godot path. The Android launcher hit this and
    // godot_shim.c has the same "://" expectation, so it reproduces here.
    //
    // CONFIRMED via --list-types: MegaCrit.Sts2.Core.Saves.GodotFileIo, one
    // CreateDirectory overload, and GetFullPath(String) is public.
    //
    // The launcher wrote __instance.GetFullPath(directoryPath) directly. This
    // assembly has no compile-time reference to sts2.dll, so the call is made
    // reflectively instead -- the MethodInfo is cached, so the reflection cost
    // is one lookup for the process, not one per call.
    //
    // Doing it this way rather than testing the raw argument for "://" matters:
    // GetFullPath resolves a relative path against SaveDir, so a legitimately
    // relative path becomes a valid absolute one and is NOT skipped. The
    // string test would have skipped those too, silently failing to create
    // directories the game expects.
    [SwitchPrefix("MegaCrit.Sts2.Core.Saves.GodotFileIo", "CreateDirectory")]
    public static bool SkipRelativeCreateDirectory(object __instance, string directoryPath)
    {
        if (directoryPath is null) return false;

        var full = ResolveFullPath(__instance, directoryPath);

        // GetFullPath failed, or produced something without a Godot scheme.
        // "." is the case this exists for.
        if (full is null || !full.Contains("://"))
        {
            SwitchLog.Info($"CreateDirectory('{directoryPath}') skipped: "
                         + $"resolves to '{full ?? "<null>"}', not a Godot path");
            return false;
        }
        return true;
    }

    private static System.Reflection.MethodInfo? _getFullPath;
    private static bool _getFullPathResolved;

    private static string? ResolveFullPath(object instance, string path)
    {
        if (!_getFullPathResolved)
        {
            _getFullPathResolved = true;
            _getFullPath = instance?.GetType().GetMethod(
                "GetFullPath",
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic
                    | System.Reflection.BindingFlags.Instance,
                null, new[] { typeof(string) }, null);

            if (_getFullPath is null)
                SwitchLog.Error("GodotFileIo.GetFullPath(string) not found; "
                              + "CreateDirectory will fall back to the raw argument");
        }

        if (_getFullPath is null) return path;

        try { return _getFullPath.Invoke(instance, new object[] { path }) as string; }
        catch { return null; }
    }

    // ---------------------------------------------------------------------
    // Startup exception capture.
    //
    // The game catches whatever GameStartup throws and logs only the HANDLER's
    // stack:
    //
    //   [ERROR] Encountered error on game startup! Attempting to show error dialog
    //      at NGame.<GameStartupWrapper>d__114.MoveNext()
    //      ... pure async plumbing ...
    //      at <unknown>
    //
    // The exception itself -- type, message, the frame that actually threw --
    // is never printed. Run 16 failed here with nothing to go on.
    //
    // GameStartupError takes the exception, so a prefix bound to that parameter
    // by NAME can print it before the dialog goes up. The weaver matches prefix
    // parameters to target parameters by name, so this needs the target's real
    // parameter name; both plausible spellings are declared Optional and
    // whichever matches wins. A miss is reported as a skip, not a failure.
    //
    // Returns true: the original still runs and the dialog still appears. This
    // only adds the message.
    // ---------------------------------------------------------------------

    // GameStartupError() takes NO parameters -- the weave said so:
    //
    //   Task NGame::GameStartupError()
    //   FAILED: LogStartupError(Exception) wants __arg0 but it has 0 parameter(s)
    //
    // The exception is captured inside the async state machine, not passed in,
    // so there is nothing here to bind to. This marks the path and no more;
    // the exception itself is caught by the two patches below.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "GameStartupError", Optional = true)]
    // VOID, not bool. A bool prefix *can* skip the original, so the weaver
    // requires a `ref __result` for any target returning a reference type --
    // otherwise skipping would hand back null, and for a Task-returning method
    // that is an await on null. Correct of it to insist, even though this one
    // always returned true.
    //
    // A void prefix cannot skip, so no __result is needed and the original
    // always runs. That is all these breadcrumbs ever wanted.
    public static void LogStartupError()
    {
        SwitchLog.Error("NGame.GameStartupError entered -- startup threw");
    }

    // TaskHelper.LogTaskExceptions(Task) is the game's OWN exception logger --
    // it appears in the traces of every exception we have managed to read. A
    // prefix on it gets the faulted Task, and therefore the exception, without
    // needing to know any parameter names.
    // Argument types pinned up front. If TaskHelper has a Task<T> overload as
    // well, an unqualified target is "ambiguous (2 overloads)" and the weave
    // fails -- which is correct behaviour but costs a build cycle to discover.
    // The stack traces show LogTaskExceptions(Task task).
    [SwitchPrefix("MegaCrit.Sts2.Core.Helpers.TaskHelper", "LogTaskExceptions",
                  ArgumentTypeNames = new[] { "System.Threading.Tasks.Task" },
                  Optional = true)]
    public static void LogTaskException(System.Threading.Tasks.Task __arg0)
    {
        // Void for the same reason as LogStartupError: LogTaskExceptions
        // returns Task, and a skipping prefix would return null into an await.
        try
        {
            var ex = __arg0?.Exception;
            if (ex is not null)
                SwitchLog.Error("TASK FAULTED: " + ex.ToString());
        }
        catch { }
    }
}
