// FinalizerSafetyPatches.cs -- Marshal all Godot native wrapper finalizers
// onto the Godot main thread.
//
// Crash report 01795243734_014135593a3c0000.log traced the mid-gameplay crash
// directly to the .NET GC Finalizer thread calling:
//
//   FinalizerStart -> __Finalizer.DrainQueue -> GodotObject.Finalize()
//     -> GodotObject.Dispose(false) -> godotsharp_internal_refcounted_disposed
//     -> CSharpLanguage::_instance_binding_reference_callback -> RBMap::_erase
//
// right while the Godot main thread was instantiating a new combat room
// (NCombatRoom). On Switch, concurrent native disposal from the .NET finalizer
// thread races with CSharpLanguage's RBMap, ObjectDB, and CanvasItem redraws.
//
// Critical correctness details (fixes crashes 01795260950 & 01795261261):
//   1. For GodotObject, Finalize() MUST invoke protected Dispose(false)
//      (isFinalizer = true), NEVER public Dispose() (Dispose(true), which sets
//      isFinalizer = false!). In Godot 4.5, when a RefCounted's short weak
//      GCHandle was cleared by GC and a new managed wrapper took over the live
//      native Resource/Font/StyleBox/Texture2D while the old wrapper waited in
//      the queue, calling Dispose(true) (isFinalizer = false) unconditionally
//      called unreference()/memdelete() on the live native resource, corrupting
//      dlmalloc (RichTextLabel::_draw -> malloc) and live CanvasItem nodes
//      (NMapScreen.Close -> CanvasItem::_propagate_visibility_changed).
//   2. Skip enqueueing GodotObjects whose NativeInstance is already IntPtr.Zero
//      (their Dispose(false) only removes a WeakReference from the thread-safe
//      ConcurrentDictionary in DisposablesTracker -- zero native calls).
//   3. Drain promptly (128/frame, 512/frame under bursts) instead of throttling
//      to 8/frame so deferred finalizers never linger across dozens of frames.

using System;
using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Threading;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Marshal Godot native finalizers to the Godot main thread using Dispose(false)")]
public static class FinalizerSafetyPatches
{
#if HAVE_GODOTSHARP
    private static readonly ConcurrentQueue<Godot.GodotObject> PendingGodotObjects = new();
    private static readonly object[] FalseArgs = { false };
    private static Action<Godot.GodotObject, bool>? _disposeGodotObjectBool;
    private static MethodInfo? _disposeGodotObjectMethod;
    private static bool _disposeResolved;
#endif
    private static readonly ConcurrentQueue<IDisposable> PendingOther = new();
    private static int _mainThreadId;
    private static bool _announced;

#if HAVE_GODOTSHARP
    [DynamicDependency(DynamicallyAccessedMemberTypes.NonPublicMethods, typeof(Godot.GodotObject))]
    private static void EnsureDisposeDelegate()
    {
        if (_disposeResolved)
            return;
        _disposeResolved = true;

        try
        {
            var mi = typeof(Godot.GodotObject).GetMethod(
                "Dispose",
                BindingFlags.Instance | BindingFlags.NonPublic,
                binder: null,
                types: new[] { typeof(bool) },
                modifiers: null);
            _disposeGodotObjectMethod = mi;
            if (mi is not null)
            {
                try
                {
                    _disposeGodotObjectBool = (Action<Godot.GodotObject, bool>)Delegate.CreateDelegate(
                        typeof(Action<Godot.GodotObject, bool>), mi);
                }
                catch
                {
                    // NativeAOT open-instance delegate thunk unavailable; fall back to MethodInfo.Invoke.
                    _disposeGodotObjectBool = null;
                }
            }
        }
        catch
        {
            // Handled below via diagnostic log.
        }
    }
#endif

    [SwitchPostfix("Godot.Bridge.ScriptManagerBridge", "FrameCallback")]
    public static void DrainFinalizersOnMainThread()
    {
        if (Volatile.Read(ref _mainThreadId) == 0)
        {
            Volatile.Write(ref _mainThreadId, Environment.CurrentManagedThreadId);
            if (!_announced)
            {
                _announced = true;
#if HAVE_GODOTSHARP
                EnsureDisposeDelegate();
                string mode = _disposeGodotObjectBool is not null
                    ? "Dispose(false) delegate"
                    : (_disposeGodotObjectMethod is not null ? "Dispose(false) MethodInfo" : "WARNING: Dispose(bool) unbound");
                SwitchLog.Info($"Main-thread Godot finalizer pump active ({mode})");
#else
                SwitchLog.Info("Main-thread Godot finalizer pump active");
#endif
            }
        }

#if HAVE_GODOTSHARP
        int goCount = PendingGodotObjects.Count;
        if (goCount > 0)
        {
            EnsureDisposeDelegate();
            var disposeBool = _disposeGodotObjectBool;
            var disposeMethod = _disposeGodotObjectMethod;
            int goBudget = goCount > 512 ? 512 : 128;
            int goDrained = 0;
            while (goDrained < goBudget && PendingGodotObjects.TryDequeue(out var go))
            {
                goDrained++;
                try
                {
                    if (disposeBool is not null)
                    {
                        disposeBool(go, false);
                        GC.SuppressFinalize(go);
                    }
                    else if (disposeMethod is not null)
                    {
                        disposeMethod.Invoke(go, FalseArgs);
                        GC.SuppressFinalize(go);
                    }
                }
                catch
                {
                    // Never let a stale wrapper finalizer break the engine frame loop.
                }
            }
        }
#endif

        int otherCount = PendingOther.Count;
        if (otherCount == 0)
            return;

        int otherBudget = otherCount > 512 ? 512 : 128;
        int otherDrained = 0;
        while (otherDrained < otherBudget && PendingOther.TryDequeue(out var obj))
        {
            otherDrained++;
            try
            {
                obj.Dispose();
            }
            catch
            {
                // Never let a stale wrapper disposal break the engine frame loop.
            }
        }
    }

    private static bool DeferOtherToMainThread(object __instance)
    {
        if (Environment.HasShutdownStarted)
            return false;

        int mainId = Volatile.Read(ref _mainThreadId);
        if (mainId != 0 && Environment.CurrentManagedThreadId == mainId)
            return true;

        if (__instance is IDisposable disposable)
            PendingOther.Enqueue(disposable);

        return false;
    }

    [SwitchPrefix("Godot.GodotObject", "Finalize")]
    public static bool DeferGodotObjectFinalize(object __instance)
    {
        if (Environment.HasShutdownStarted)
            return false;

        int mainId = Volatile.Read(ref _mainThreadId);
        if (mainId != 0 && Environment.CurrentManagedThreadId == mainId)
            return true;

#if HAVE_GODOTSHARP
        if (__instance is Godot.GodotObject go)
        {
            // NativePtr == IntPtr.Zero means C++ already detached/freed this
            // object; stock Dispose(false) makes zero native calls and only
            // removes the WeakReference from DisposablesTracker's
            // ConcurrentDictionary, so let it complete immediately off-thread.
            if (!Godot.GodotObject.IsInstanceValid(go))
                return true;

            PendingGodotObjects.Enqueue(go);
            return false;
        }
#endif
        return DeferOtherToMainThread(__instance);
    }

    [SwitchPrefix("Godot.Collections.Array", "Finalize")]
    public static bool DeferArrayFinalize(object __instance) => DeferOtherToMainThread(__instance);

    [SwitchPrefix("Godot.Collections.Dictionary", "Finalize")]
    public static bool DeferDictionaryFinalize(object __instance) => DeferOtherToMainThread(__instance);

    [SwitchPrefix("Godot.StringName", "Finalize")]
    public static bool DeferStringNameFinalize(object __instance) => DeferOtherToMainThread(__instance);

    [SwitchPrefix("Godot.NodePath", "Finalize")]
    public static bool DeferNodePathFinalize(object __instance) => DeferOtherToMainThread(__instance);
}
