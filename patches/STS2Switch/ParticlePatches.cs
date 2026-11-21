// ParticlePatches.cs -- keep GPU particles off this GPU, until NVK stops
// hanging on them.
//
// THE EVIDENCE
// ============
// Two device-loss crashes, and then a third under NVK_DEBUG=push_sync, which
// makes every submit synchronous so the failure lands on the submit that
// caused it:
//
//     [INFO] Monster VANTOM performing move DISMEMBER_MOVE
//     Loading cache for shader ParticlesShaderRD, variant 0
//     [watchdog] game thread has not completed a step in 5 s
//     nvkmd-switch: channel 2 lost: notification={type=8 info=0 status=65535}
//     nvkmd-switch: channel wait idle failed: device lost
//
// The particle dispatch goes in, the GPU never returns to idle, the channel
// times out, and Godot's CRASH_NOW ends the process. ParticlesShaderRD is
// Godot's GPU-particle COMPUTE shader; a compute dispatch that never finishes
// is exactly what this looks like from the outside.
//
// Whether the underlying fault is NAK's Maxwell codegen or something the
// shader reads from uninitialised memory is not settled and does not need to
// be for this mitigation: no dispatch, no hang.
//
// THE SHAPE OF THE PATCH
// ======================
// The game uses BOTH GpuParticles2D and CpuParticles2D (both appear in
// sts2.dll's TypeRefs, with set_Emitting / Restart / set_OneShot on each).
// CPU particles never touch ParticlesShaderRD and keep working; this touches
// only the GPU kind.
//
// Two entry points start emission from script, and both are covered:
//   set_Emitting(true)  -- the property the combat code sets
//   Restart()           -- re-triggers a one-shot emitter
//
// KNOWN GAP: a GpuParticles2D saved with emitting=true in its scene file
// starts emitting when it enters the tree, without either call. The logs say
// that path is not the trigger here -- the game survives 80+ seconds of scene
// loading and dies specifically on combat-move VFX, which is script-triggered.
// If a hang ever occurs with this patch active, that gap is the first place to
// look, and the fix would have to move into the engine side.
//
// RUNTIME-SWITCHABLE, one AOT build for both behaviours:
// config.txt `gpu_particles 1` -> wrapper sets STS2_GPU_PARTICLES=1 -> the
// prefixes pass through untouched. Absent or 0, emission is suppressed. The
// A/B is a config edit, not a rebuild -- the same reasoning as NVK_DEBUG.
//
// MIT license; see LICENSE.

using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "GPU particles hang NVK on GM20B; see PARTICLES_DEVICE_LOSS.md")]
public static class ParticlePatches
{
#if HAVE_GODOTSHARP
    // Read once. getenv goes through the wrapper's environment shim, and the
    // value cannot change after boot -- main.c sets it before the engine
    // starts.
    private static readonly bool s_allowGpuParticles =
        System.Environment.GetEnvironmentVariable("STS2_GPU_PARTICLES") == "1";

    private static bool s_announced;

    private static void AnnounceOnce()
    {
        if (s_announced) return;
        s_announced = true;
        SwitchLog.Info(s_allowGpuParticles
            ? "GpuParticles2D: ENABLED by config (gpu_particles 1) -- if the GPU "
              + "channel dies mid-combat, this is the first thing to turn off"
            : "GpuParticles2D: emission suppressed (NVK channel-loss mitigation); "
              + "set gpu_particles 1 in config.txt to test the real thing");
    }

    // set_Emitting: swallow `true`, pass `false` through. Letting explicit
    // stops through matters for one-shots the scene may have started -- the
    // patched world should still be able to turn things OFF.
    [SwitchPrefix("Godot.GpuParticles2D", "Emitting",
                  Kind = MemberKind.PropertySetter, Optional = true)]
    public static bool SuppressGpuEmission(object __instance, bool __arg0)
    {
        AnnounceOnce();
        if (s_allowGpuParticles) return true;   // run the real setter
        if (__arg0 && __instance is Godot.GpuParticles2D p)
        {
            p.Visible = false;
            p.SetProcess(false);
            p.SetPhysicsProcess(false);
        }
        return !__arg0;                         // false: only when turning ON
    }

    [SwitchPrefix("Godot.GpuParticles2D", "Restart",
                  ArgumentTypeNames = new string[0], Optional = true)]
    public static bool SuppressGpuRestart(object __instance)
    {
        AnnounceOnce();
        if (!s_allowGpuParticles && __instance is Godot.GpuParticles2D p)
        {
            p.Visible = false;
            p.SetProcess(false);
            p.SetPhysicsProcess(false);
        }
        return s_allowGpuParticles;
    }

    [SwitchPrefix("Godot.GpuParticles2D", "Restart",
                  ArgumentTypeNames = new[] { "System.Boolean" }, Optional = true)]
    public static bool SuppressGpuRestartKeepSeed(object __instance, bool __arg0)
    {
        AnnounceOnce();
        if (!s_allowGpuParticles && __instance is Godot.GpuParticles2D p)
        {
            p.Visible = false;
            p.SetProcess(false);
            p.SetPhysicsProcess(false);
        }
        return s_allowGpuParticles;
    }
#endif
}
