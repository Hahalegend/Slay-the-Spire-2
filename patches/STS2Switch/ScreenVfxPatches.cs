// ScreenVfxPatches.cs -- Eliminates blank/black screen flashes during enemy
// attacks and removes wasted full-viewport BackBufferCopy + scene-baked
// GpuParticles2D draw passes on Switch NVK.
//
// ROOT CAUSE OF BLANK SCREEN DURING ENEMY ATTACKS
// ===============================================
// With `skip_compute 1` (required to prevent NVK GM20B channel-loss hangs on
// ParticlesShaderRD compute dispatches), Godot 4.5 Forward Mobile shaders that
// sample `hint_screen_texture, filter_linear_mipmap` behind a BackBufferCopy
// read an empty black screen texture (vec3(0.0)):
//
//   1. NRadialBlurVfx (scenes/rooms/combat_room.tscn -> shaders/radial_blur.gdshader)
//      Full-screen 1920x1080 ColorRect triggered during enemy attacks by
//      ShrinkerBeetle (ShrinkMove), BygoneEffigy (SlashMove), CeremonialBeast
//      (PlowMove), MechaKnight (HeavyCleaveMove), and Vantom (DismemberMove).
//      Turning it visible blanks the entire combat screen to solid black!
//
//   2. NDoomOverlayVfx (scenes/vfx/doom_overlay_vfx.tscn -> shaders/doom_overlay.gdshader)
//      Same BackBufferCopy + 1920x1080 ColorRect + hint_screen_texture mipmap pattern.
//
//   3. Attack / impact / orb BackBufferCopy nodes (vfx_heavy_blunt, vfx_scream,
//      vfx_spooky_scream, vfx_hyperbeam, vfx_large_magic_missile, vfx_starry_impact,
//      vfx_grand_finale_impact, glass_orb, etc.) whose only child is a suppressed
//      GPUParticles2D distortion emitter -- yet an active BackBufferCopy still
//      forces a full-viewport GPU copy pass on GM20B every frame it is alive.
//
//   4. Scene-baked GPUParticles2D (emitting=true in .tscn without C# set_Emitting/
//      Restart calls: e.g. regent.tscn x6, fogmog.tscn x3, merchant_room.tscn x4,
//      vantom_boss x52, the_insatiable_boss x68, waterfall_giant_boss x182).
//      Disabling them via SceneTree.NodeAdded eliminates all wasted particle
//      draw calls and storage-buffer binds before their very first frame.

using System;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Disable broken full-screen BackBufferCopy overlays (fixes blank screen on enemy attacks) and scene-baked GpuParticles2D")]
public static class ScreenVfxPatches
{
#if HAVE_GODOTSHARP
    private static readonly bool s_allowGpuParticles =
        Environment.GetEnvironmentVariable("STS2_GPU_PARTICLES") == "1";

    private static bool s_treeGuardInstalled;
    private static int s_disabledBackBuffers;
    private static int s_disabledGpuParticles;

    // ------------------------------------------------------------------
    // 1. Suppress NRadialBlurVfx (fixes black screen flash on enemy attacks)
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Vfx.NRadialBlurVfx", "_Ready", Optional = true)]
    public static bool DisableRadialBlurReady(object __instance)
    {
        DisableBackBufferNode(__instance);
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Vfx.NRadialBlurVfx", "Activate", Optional = true)]
    public static bool DisableRadialBlurActivate(object __instance)
    {
        DisableBackBufferNode(__instance);
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Suppress NDoomOverlayVfx (same BackBufferCopy + mipmap screen_texture)
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Vfx.NDoomOverlayVfx", "_Ready", Optional = true)]
    public static bool DisableDoomOverlayReady(object __instance)
    {
        DisableBackBufferNode(__instance);
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Vfx.NDoomOverlayVfx", "PlayVfx", Optional = true)]
    public static bool DisableDoomOverlayPlay(object __instance)
    {
        DisableBackBufferNode(__instance);
        return false;
    }

    // ------------------------------------------------------------------
    // 3. SceneTree.NodeAdded guard: disables EVERY BackBufferCopy and every
    //    scene-baked GpuParticles2D the instant it enters the tree.
    // ------------------------------------------------------------------
    internal static void InstallTreeGuard(Godot.SceneTree tree)
    {
        if (s_treeGuardInstalled)
            return;

        s_treeGuardInstalled = true;
        tree.NodeAdded += OnNodeAdded;

        // Sweep any nodes already in the tree (e.g. main_menu_bg.tscn particles).
        if (tree.Root is not null)
            SweepSubtree(tree.Root);

        SwitchLog.Info(
            $"ScreenVfxPatches: SceneTree guard active (initial sweep: {s_disabledBackBuffers} BackBufferCopy, {s_disabledGpuParticles} GpuParticles2D disabled)");
    }

    private static void SweepSubtree(Godot.Node node)
    {
        OnNodeAdded(node);
        int count = node.GetChildCount();
        for (int i = 0; i < count; i++)
        {
            var child = node.GetChild(i);
            if (child is not null)
                SweepSubtree(child);
        }
    }

    private static void OnNodeAdded(Godot.Node node)
    {
        if (node is null || !Godot.GodotObject.IsInstanceValid(node))
            return;

        try
        {
            if (node is Godot.BackBufferCopy bbc)
            {
                bbc.CopyMode = Godot.BackBufferCopy.CopyModeEnum.Disabled;
                bbc.Visible = false;
                bbc.SetProcess(false);
                bbc.SetPhysicsProcess(false);
                s_disabledBackBuffers++;
                return;
            }

            if (!s_allowGpuParticles && node is Godot.GpuParticles2D p)
            {
                p.Emitting = false;
                p.Visible = false;
                p.SetProcess(false);
                p.SetPhysicsProcess(false);
                s_disabledGpuParticles++;
            }
        }
        catch
        {
        }
    }

    private static void DisableBackBufferNode(object instance)
    {
        if (instance is Godot.BackBufferCopy bbc)
        {
            bbc.CopyMode = Godot.BackBufferCopy.CopyModeEnum.Disabled;
            bbc.Visible = false;
            bbc.SetProcess(false);
            bbc.SetPhysicsProcess(false);
        }
    }
#endif
}
