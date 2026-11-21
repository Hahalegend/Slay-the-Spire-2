// PreloadPatches.cs -- Fast Switch boot, selective preloading with zero-stutter
// core combat/map/reward assets, parallel VFX loading, and snappy transitions.
//
// Measured on Switch hardware (v0.111.1):
//   - Stock 'Common' + 'characters=IRONCLAD': 132.2s (792 + 698 assets, 196 serial VFX)
//   - Allowlist + core combat VFX (parallel):  14.2s (9.3x faster, 0 core combat VFX misses)

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Fast Switch boot, serialized thread-safe preloading, warm cache retention, and snappy transitions")]
public static class PreloadPatches
{
    private static FieldInfo? _loadingField;
    private static FieldInfo? _toLoadField;
    private static FieldInfo? _cacheField;
    private static bool _sessionFieldsResolved;

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
        "star_gain_vfx",
        "vfx_starry_impact",
        "power_flash_vfx",
        "rare_glow_vfx",
        "vfx_card_transform",
        "vfx_attack_blunt",
        "vfx_slime_impact",
        "vfx_card_smith",
        "spike_splash_vfx",
        "fg_ground_spike_vfx",
        "bg_ground_spike_vfx",
    };

    // Tiny (<4 KB) universal combat/UI assets observed missing mid-run on Switch
    // that are not always listed in a character's RunSet. Injecting them once
    // during Embark preload (and keeping them warm via KeepReusableAssetsWarm)
    // eliminates synchronous SD-card stutters during combat and hover tips.
    private static readonly string[] ExtraWarmRunAssets =
    {
        "res://scenes/ui/card_hover_tip.tscn",
        "res://scenes/vfx/whole_screen/vfx_smoky_vignette.tscn",
        "res://scenes/vfx/vfx_heal_osty.tscn",
        "res://scenes/events/ancient_dialogue_line.tscn",
        "res://images/ui/run_history/monster.png",
        "res://images/ui/run_history/monster_outline.png",
        "res://images/ui/run_history/event.png",
        "res://images/ui/run_history/event_outline.png",
        "res://images/ui/run_history/unknown_monster.png",
        "res://images/ui/run_history/unknown_monster_outline.png",
        "res://images/ui/run_history/shop.png",
        "res://images/ui/run_history/shop_outline.png",
        "res://images/ui/run_history/rest_site.png",
        "res://images/ui/run_history/rest_site_outline.png",
        "res://images/powers/weak_power.png",
        "res://images/powers/vulnerable_power.png",
        "res://images/powers/strength_power.png",
        "res://images/powers/frail_power.png",
        "res://images/powers/dexterity_power.png",
        "res://images/powers/poison_power.png",
        "res://images/powers/shrink_power.png",
        "res://images/powers/die_for_you_power.png",
        "res://images/powers/star_next_turn_power.png",
        "res://images/packed/sprite_fonts/star_icon.png",
        "res://images/packed/sprite_fonts/ironclad_energy_icon.png",
        "res://images/packed/sprite_fonts/silent_energy_icon.png",
        "res://images/packed/sprite_fonts/defect_energy_icon.png",
        "res://images/packed/sprite_fonts/necrobinder_energy_icon.png",
        "res://images/packed/sprite_fonts/regent_energy_icon.png",
        "res://images/ui/run_history/treasure.png",
        "res://images/ui/run_history/treasure_outline.png",
        "res://images/ui/run_history/unknown_shop.png",
        "res://images/ui/run_history/unknown_shop_outline.png",
        "res://images/powers/artifact_power.png",
        "res://images/powers/retain_hand_power.png",
        "res://images/powers/constrict_power.png",
        "res://images/powers/hang_power.png",
        "res://images/powers/doom_power.png",
        "res://images/powers/thorns_power.png",
        "res://images/powers/ritual_power.png",
        "res://images/powers/barricade_power.png",
        "res://images/powers/buffer_power.png",
        "res://images/powers/focus_power.png",
        "res://images/powers/intangible_power.png",
        "res://images/powers/blur_power.png",
        "res://images/powers/block_next_turn_power.png",
        "res://images/powers/draw_cards_next_turn_power.png",
        "res://images/powers/curl_up_power.png",
        "res://images/powers/enrage_power.png",
        "res://images/powers/minion_power.png",
        "res://images/powers/regen_power.png",
        "res://images/powers/vigor_power.png",
        "res://images/powers/slippery_power.png",
        "res://images/powers/soulbound_power.png",
        "res://images/powers/hex_power.png",
        "res://images/powers/confused_power.png",
        "res://images/powers/no_draw_power.png",
        "res://images/powers/dexterity_down_power.png",
        "res://images/ui/run_history/neow.png",
        "res://images/ui/run_history/neow_outline.png",
        "res://images/ui/run_history/ancient.png",
        "res://images/ui/run_history/ancient_outline.png",
        "res://images/ui/run_history/unknown_elite.png",
        "res://images/ui/run_history/unknown_elite_outline.png",
        "res://images/ui/run_history/unknown_treasure.png",
        "res://images/ui/run_history/unknown_treasure_outline.png",
        "res://images/enchantments/glam.png",
        "res://materials/transitions/defect_transition_mat.tres",
        "res://materials/transitions/fade_transition_mat.tres",
        "res://materials/transitions/fight_transition_mat.tres",
        "res://materials/transitions/game_over_transition_mat.tres",
        "res://materials/transitions/ironclad_transition_mat.tres",
        "res://materials/transitions/necrobinder_transition_mat.tres",
        "res://materials/transitions/regent_transition_mat.tres",
        "res://materials/transitions/silent_transition_mat.tres",
        "res://scenes/ui/generic_popup.tscn",
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
        var keptSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
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
                if (keptSet.Add(path))
                {
                    kept.Add(path);
                    if (isCoreVfx) keptVfx++;
                }
            }
            else
            {
                skipped++;
            }
        }

#if HAVE_GODOTSHARP
        int injected = 0;
        if (!isRoomOrAct)
        {
            for (int i = 0; i < ExtraWarmRunAssets.Length; i++)
            {
                string extra = ExtraWarmRunAssets[i];
                if (!keptSet.Contains(extra) && Godot.ResourceLoader.Exists(extra))
                {
                    keptSet.Add(extra);
                    kept.Add(extra);
                    injected++;
                }
            }
        }
        SwitchLog.Info($"Preload filter '{name}': {kept.Count}/{total} kept ({keptVfx} core vfx, +{injected} warm, {skipped} deferred)");
#else
        SwitchLog.Info($"Preload filter '{name}': {kept.Count}/{total} kept ({keptVfx} core vfx, {skipped} deferred)");
#endif
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
        // material, power icons, and combat UI assets (eliminates in-run stutter misses).
        if (path.Equals("res://scenes/run.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/screens/main_menu.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/screens/game_over_screen.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/screens/rewards_screen.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/screens/card_selection/", StringComparison.OrdinalIgnoreCase) ||
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
            path.Equals("res://scenes/ui/ancient_name_banner.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/ui/act_banner.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.Equals("res://scenes/ui/generic_popup.tscn", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://scenes/merchant/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/packed/combat_ui/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/packed/map/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/packed/sprite_fonts/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/ui/combat/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/ui/reward_screen/", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("res://images/ui/rest_site/", StringComparison.OrdinalIgnoreCase) ||
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
                path.StartsWith("res://scenes/encounters/", StringComparison.OrdinalIgnoreCase) ||
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
    // 2. Route kept core VFX into the main loading queue AND cap concurrent
    //    ResourceLoader.LoadThreadedRequest() calls to 1 in flight at a time.
    //
    //    Why: stock ProcessLoadingQueue() fires up to 128 concurrent
    //    LoadThreadedRequest()s across Godot's WorkerThreadPool threads.
    //    Crash 01795253244 caught 3 worker threads simultaneously racing
    //    inside FileAccessPack::open_internal -> RefCounted::unreference()
    //    against devkitPro's non-thread-safe file handle table.
    //    Capping to 1 in-flight background load eliminates both the crash
    //    AND microSD multi-thread seek thrashing while staying non-blocking!
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetLoadingSession", "IsVfxScene")]
    public static bool LoadVfxThroughMainQueue(ref bool __result)
    {
        __result = false;
        return false;
    }

#if HAVE_GODOTSHARP
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetLoadingSession", "ProcessLoadingQueue")]
    public static bool SerializeThreadedLoading(object __instance)
    {
        if (!_sessionFieldsResolved)
        {
            var t = __instance.GetType();
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
            _loadingField = t.GetField("_loading", flags);
            _toLoadField = t.GetField("_toLoad", flags);
            _cacheField = t.GetField("_cache", flags);
            _sessionFieldsResolved = true;
        }

        if (_loadingField?.GetValue(__instance) is not Queue<string> loading ||
            _toLoadField?.GetValue(__instance) is not Queue<string> toLoad ||
            _cacheField?.GetValue(__instance) is not ConcurrentDictionary<string, Godot.Resource> cache)
        {
            return true;
        }

        while (loading.Count < 1 && toLoad.TryDequeue(out string? path))
        {
            if (string.IsNullOrEmpty(path) || cache.ContainsKey(path))
                continue;

            var err = Godot.ResourceLoader.LoadThreadedRequest(
                path, "", false, Godot.ResourceLoader.CacheMode.Reuse);
            if (err == Godot.Error.Ok)
                loading.Enqueue(path);
            else
                SwitchLog.Error($"PreloadPatches: LoadThreadedRequest failed ({err}) for {path}");
        }

        return false;
    }

    // Immediately kick the next serialized background load on the SAME frame
    // that CheckLoadingStatus drains _loading, eliminating 2 dead frames per asset.
    [SwitchPostfix("MegaCrit.Sts2.Core.Assets.AssetLoadingSession", "CheckLoadingStatus")]
    public static void KickNextSerializedLoad(object __instance)
    {
        SerializeThreadedLoading(__instance);
    }
#endif

    // ------------------------------------------------------------------
    // 3. Preserve reusable assets across room transitions so power icons,
    //    sprite fonts, UI panels, shaders, and combat VFX are never evicted
    //    and synchronously re-read from the Switch SD card mid-combat.
    //    Heavy room-only assets (backgrounds, encounters, old monster visuals,
    //    events) still unload normally between rooms to keep VRAM/RAM lean.
    // ------------------------------------------------------------------
    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetCache", "UnloadMissedCacheAssets")]
    public static bool PreserveReusableMissedAssets()
    {
        // Keep _missedCacheAssets intact so AssetCache.UnloadAssets also skips
        // any on-demand asset loaded during combat instead of evicting it.
        return false;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetCache", "UnloadAssets")]
    public static bool KeepReusableAssetsWarm(ref IEnumerable<string> __arg0)
    {
        if (__arg0 is null)
            return false;

        var toUnload = new List<string>();
        foreach (var path in __arg0)
        {
            if (string.IsNullOrEmpty(path))
                continue;

            // Only unload heavy room/act-specific assets that change every room.
            if (path.StartsWith("res://scenes/backgrounds/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/encounters/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/events/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://scenes/creature_visuals/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/backgrounds/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/events/", StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith("res://images/rooms/", StringComparison.OrdinalIgnoreCase))
            {
                toUnload.Add(path);
            }
        }

        if (toUnload.Count == 0)
            return false;

        __arg0 = toUnload;
        return true;
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
