// PreloadPatches.cs -- Fast Switch boot, selective preloading with zero-stutter
// core combat/map/reward assets, parallel VFX loading, and snappy transitions.
//
// Measured on Switch hardware (v0.111.1):
//   - Stock 'Common' + 'characters=IRONCLAD': 132.2s (792 + 698 assets, 196 serial VFX)
//   - Allowlist + core combat VFX (parallel):  14.2s (9.3x faster, 0 core combat VFX misses)

using System;
using System.Collections.Generic;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Fast Switch boot, allowlisted preloading + core combat VFX, and snappy transitions")]
public static class PreloadPatches
{
    private static FieldInfo? _missedCacheField;
    private static bool _missedCacheFieldResolved;

    // Core universal VFX fired in almost every combat, reward, or map transition.
    // Preloading ONLY these (~28 scenes, loaded in parallel) avoids mid-combat
    // main-thread load/shader stutters without spending 90+ seconds preloading
    // all ~313 rare card/event/boss VFX scenes upfront.
    private static readonly string[] CoreCombatVfxNames =
    {
        "vfx_attack_slash",
        "vfx_damage_num",
        "hit_spark_vfx",
        "vfx_card_fly",
        "card_trail_",
        "thin_slice_vfx",
        "power_applied_vfx",
        "vfx_debuff_applied",
        "vfx_buff_applied",
        "vfx_monster_death",
        "power_removed_vfx",
        "vfx_block",
        "block_spark_vfx",
        "vfx_blocked_text",
        "vfx_card_shuffle_fly",
        "vfx_block_broken",
        "vfx_card_exhaust",
        "vfx_card_power_fly",
        "map_circle_vfx",
        "map_node_select_vfx",
        "relic_inventory_flash_vfx",
        "relic_flash_vfx",
        "vfx_potion_flash",
        "uncommon_glow_vfx",
        "vfx_low_hp_border",
        "vfx_thought_bubble",
        "vfx_cross_heal",
        "vfx_heal_num",
    };

    // ------------------------------------------------------------------
    // 1. Tight allowlist preload filter in PreloadManager.LoadAssets
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.PreloadManager", "LoadAssets")]
    public static void FilterPreloadAssets(ref IEnumerable<string> __arg0, string __arg1)
    {
        if (__arg0 is null)
            return;

        string name = __arg1 ?? "";

        // Keep tiny boot essentials untouched (IntroLogo: 3, MainMenuEssentials: 5-7).
        if (name == "IntroLogo" || name == "MainMenuEssentials")
            return;

        // Skip 'Common' completely: it queues ~792 assets on the Main Menu and
        // jams NAssetLoader's single-session FIFO queue for 90-130 seconds if
        // the player starts or continues a run before it finishes.
        if (name == "Common")
        {
            SwitchLog.Info("Preload filter 'Common': skipped bulk background preload");
            __arg0 = Array.Empty<string>();
            return;
        }

        bool isRoomOrAct = !name.StartsWith("characters=", StringComparison.Ordinal);

        var kept = new List<string>();
        int total = 0;
        int keptVfx = 0;
        int skipped = 0;

        foreach (var path in __arg0)
        {
            total++;
            if (string.IsNullOrEmpty(path))
                continue;

            if (ShouldPreload(path, isRoomOrAct, out bool isCoreVfx))
            {
                kept.Add(path);
                if (isCoreVfx) keptVfx++;
            }
            else
            {
                skipped++;
            }
        }

        SwitchLog.Info($"Preload filter '{name}': {kept.Count}/{total} kept ({keptVfx} core vfx, {skipped} deferred)");
        __arg0 = kept;
    }

    private static bool ShouldPreload(string path, bool isRoomOrAct, out bool isCoreVfx)
    {
        isCoreVfx = false;

        // VFX scenes/materials/shaders: keep ONLY the ~28 universal combat/reward VFX
        // so gameplay is smooth while skipping ~285 rare card/boss/event VFX.
        if (path.Contains("/vfx/", StringComparison.OrdinalIgnoreCase))
        {
            if (path.EndsWith(".tscn", StringComparison.OrdinalIgnoreCase) && IsCoreCombatVfx(path))
            {
                isCoreVfx = true;
                return true;
            }
            return false;
        }

        // Core run, combat, map, reward, hover-tip, orb, character Spine, shader,
        // material, and combat UI assets (eliminates in-run stutter misses).
        if (path.Equals("res://scenes/run.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/screens/rewards_screen.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/screens/card_selection/card_reward_selection_screen.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/rooms/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/creature_visuals/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/cards/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/combat/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/orbs/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/relics/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/potions/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/screens/map/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/hover_tip", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/card_hover_tip", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/normal_map_point", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/boss_map_point", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/ancient_map_point", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/map_dot", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/card_reward_", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/ui/character_icons/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/packed/combat_ui/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/packed/map/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/ui/combat/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/ui/reward_screen/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://materials/cards/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://materials/transitions/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://materials/ui/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://shaders/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://debug_audio/", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        // Room/Act-specific assets (backgrounds, monsters, animations, events, rest site, rewards).
        if (isRoomOrAct)
        {
            if (path.StartsWith("res://scenes/backgrounds/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/events/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/rest_site/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/rewards/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://animations/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/rooms/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/events/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/backgrounds/", StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        // Everything else (400+ ControllerConfig glyph PNGs, menu/library/bestiary/
        // inspect/FTUE screens, individual PNG icons already in VRAM atlases) is deferred
        // to on-demand loading and retained in cache once first used.
        return false;
    }

    private static bool IsCoreCombatVfx(string path)
    {
        for (int i = 0; i < CoreCombatVfxNames.Length; i++)
        {
            if (path.Contains(CoreCombatVfxNames[i], StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Load the ~28 kept core VFX scenes in parallel instead of 1-by-1
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetLoadingSession", "IsVfxScene")]
    public static bool LoadVfxInParallel(ref bool __result)
    {
        __result = false;
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Preserve reusable on-demand assets across room transitions
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetCache", "UnloadMissedCacheAssets")]
    public static bool PreserveReusableMissedAssets(object __instance)
    {
        if (!_missedCacheFieldResolved)
        {
            _missedCacheField = __instance.GetType().GetField(
                "_missedCacheAssets",
                BindingFlags.Instance | BindingFlags.NonPublic);
            _missedCacheFieldResolved = true;
        }

        if (_missedCacheField?.GetValue(__instance) is HashSet<string> missed && missed.Count > 0)
        {
            missed.Clear();
        }

        return false;
    }

    // ------------------------------------------------------------------
    // 4. Skip JIT prewarm (no-op waste under NativeAOT)
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Helpers.OneTimeInitialization", "PrewarmJit")]
    public static bool SkipPrewarmJit()
    {
        return false;
    }

    // ------------------------------------------------------------------
    // 5. Skip intro logo animation on boot (~4.5s faster to Main Menu)
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NGame", "LaunchMainMenu")]
    public static void SkipIntroLogo(ref bool __arg0)
    {
        __arg0 = true;
    }

    // ------------------------------------------------------------------
    // 6. Snappier full-screen scene fades (0.25s cap instead of 0.8s)
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NTransition", "FadeOut")]
    public static void FastFadeOut(ref float __arg0)
    {
        if (__arg0 > 0.25f)
            __arg0 = 0.25f;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.NTransition", "FadeIn")]
    public static void FastFadeIn(ref float __arg0)
    {
        if (__arg0 > 0.25f)
            __arg0 = 0.25f;
    }
}
