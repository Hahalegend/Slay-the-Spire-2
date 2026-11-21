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
// Fix:
//   1. Intercept Finalize() on GodotObject, Collections.Array,
//      Collections.Dictionary, StringName, and NodePath. When called from any
//      non-main thread (i.e. the .NET Finalizer thread), enqueue the object
//      into a lock-free ConcurrentQueue<IDisposable> instead of calling into
//      libgodot_android.so off-thread.
//   2. Drain the queue on the Godot main thread every frame inside
//      Godot.Bridge.ScriptManagerBridge.FrameCallback() with a per-frame cap
//      so cleanup never causes frame spikes or native data races.

using System;
using System.Collections.Concurrent;
using System.Threading;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Marshal Godot native finalizers to the Godot main thread")]
public static class FinalizerSafetyPatches
{
    private static readonly ConcurrentQueue<IDisposable> PendingDispose = new();
    private static int _mainThreadId;
    private static bool _announced;

    [SwitchPostfix("Godot.Bridge.ScriptManagerBridge", "FrameCallback")]
    public static void DrainFinalizersOnMainThread()
    {
        if (Volatile.Read(ref _mainThreadId) == 0)
        {
            Volatile.Write(ref _mainThreadId, Environment.CurrentManagedThreadId);
            if (!_announced)
            {
                _announced = true;
                SwitchLog.Info("Main-thread Godot finalizer pump active");
            }
        }

        int count = PendingDispose.Count;
        if (count == 0)
            return;

        // Spread cleanup across frames during normal play, but ramp up if a
        // big room/act unload queued a burst so memory never backs up.
        int budget = count > 256 ? 256 : 64;
        int drained = 0;
        while (drained < budget && PendingDispose.TryDequeue(out var obj))
        {
            drained++;
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

    private static bool DeferToMainThread(object __instance)
    {
        if (Environment.HasShutdownStarted)
            return false;

        int mainId = Volatile.Read(ref _mainThreadId);
        if (mainId != 0 && Environment.CurrentManagedThreadId == mainId)
            return true;

        if (__instance is IDisposable disposable)
            PendingDispose.Enqueue(disposable);

        return false;
    }

    [SwitchPrefix("Godot.GodotObject", "Finalize")]
    public static bool DeferGodotObjectFinalize(object __instance) => DeferToMainThread(__instance);

    [SwitchPrefix("Godot.Collections.Array", "Finalize")]
    public static bool DeferArrayFinalize(object __instance) => DeferToMainThread(__instance);

    [SwitchPrefix("Godot.Collections.Dictionary", "Finalize")]
    public static bool DeferDictionaryFinalize(object __instance) => DeferToMainThread(__instance);

    [SwitchPrefix("Godot.StringName", "Finalize")]
    public static bool DeferStringNameFinalize(object __instance) => DeferToMainThread(__instance);

    [SwitchPrefix("Godot.NodePath", "Finalize")]
    public static bool DeferNodePathFinalize(object __instance) => DeferToMainThread(__instance);
}
