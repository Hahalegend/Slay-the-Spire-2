// AudioPerfPatches.cs -- Eliminate FMOD parameter warnings, missing/renamed
// monster SFX lookup stalls, and Act-transition FMOD invalid-handle backtraces.
//
// Hardware telemetry (godot.log & godot2026-11-22T03.44.04.log on Switch):
//   1. SfxCmd.PlayDamage(MonsterModel, int) unconditionally passes
//      {"EnemyImpact_Intensity", intensity} to NAudioManager.PlayOneShot().
//      Only generic impact events under "event:/sfx/enemy/enemy_impact_enemy_size/"
//      define 'EnemyImpact_Intensity'; custom monster hurt events (chomper_hurt,
//      thieving_hopper_hurt_hover, hunter_killer_hurt, byrdonis_hurt) do not,
//      triggering synchronous SD-card warning writes on every hit.
//   2. Several monsters were renamed in C# after the FMOD banks were built
//      (e.g. BowlbugSilk -> workbug_silk, BowlbugNectar -> workbug_goop,
//      Myte -> mite, PhantasmalGardener -> phantasmal_gardeners,
//      TwoTailedRat -> two_tail_rats, MechaKnight -> mechaknight,
//      TrackerRubyRaider cast -> buff), while a few monsters have no _die or
//      _attack event in Master.strings.bank (eye_with_teeth_die,
//      living_shield_attack, *_ruby_raider_cast). Remapping the renamed paths
//      restores their real FMOD sound effects, and skipping nonexistent paths
//      eliminates "cannot find sfx path:" log stalls.
//   3. On every Act transition (Act 1 -> Act 2 -> Act 3),
//      NRunMusicController.UpdateMusic() called LoadActBank() (which unloads
//      the previous Act's FMOD bank, invalidating current_music) BEFORE
//      _proxy.Call("update_music") stopped and released current_music, dumping
//      two `ERROR: [FMOD ERROR] An invalid object handle was used (36)` +
//      12-frame C# backtraces. Stopping current_music before LoadActBank()
//      unloads the old bank eliminates both FMOD errors cleanly.

using System;
using System.Collections.Generic;
using System.Reflection;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Fix Act-transition FMOD invalid-handle errors, remap renamed monster SFX, and strip missing FMOD params")]
public static class AudioPerfPatches
{
    private static readonly Dictionary<string, string> RemappedSfxPaths = new(StringComparer.OrdinalIgnoreCase)
    {
        ["event:/sfx/enemy/enemy_attacks/bowlbug_silk/bowlbug_silk_attack"] =
            "event:/sfx/enemy/enemy_attacks/workbug_silk/workbug_silk_attack",
        ["event:/sfx/enemy/enemy_attacks/bowlbug_silk/bowlbug_silk_cast"] =
            "event:/sfx/enemy/enemy_attacks/workbug_silk/workbug_silk_buff",
        ["event:/sfx/enemy/enemy_attacks/bowlbug_nectar/bowlbug_nectar_attack"] =
            "event:/sfx/enemy/enemy_attacks/workbug_goop/workbug_goop_attack",
        ["event:/sfx/enemy/enemy_attacks/bowlbug_nectar/bowlbug_nectar_cast"] =
            "event:/sfx/enemy/enemy_attacks/workbug_goop/workbug_goop_buff",
        ["event:/sfx/enemy/enemy_attacks/myte/myte_attack"] =
            "event:/sfx/enemy/enemy_attacks/mite/mite_attack",
        ["event:/sfx/enemy/enemy_attacks/myte/myte_cast"] =
            "event:/sfx/enemy/enemy_attacks/mite/mite_cast",
        ["event:/sfx/enemy/enemy_attacks/phantasmal_gardener/phantasmal_gardener_attack"] =
            "event:/sfx/enemy/enemy_attacks/phantasmal_gardeners/phantasmal_gardeners_attack_bite",
        ["event:/sfx/enemy/enemy_attacks/phantasmal_gardener/phantasmal_gardener_cast"] =
            "event:/sfx/enemy/enemy_attacks/phantasmal_gardeners/phantasmal_gardeners_buff",
        ["event:/sfx/enemy/enemy_attacks/two_tailed_rat/two_tailed_rat_attack"] =
            "event:/sfx/enemy/enemy_attacks/two_tail_rats/two_tail_rats_attack_bite",
        ["event:/sfx/enemy/enemy_attacks/mecha_knight/mecha_knight_attack"] =
            "event:/sfx/enemy/enemy_attacks/mechaknight/mechaknight_heavy_attack",
        ["event:/sfx/enemy/enemy_attacks/mecha_knight/mecha_knight_cast"] =
            "event:/sfx/enemy/enemy_attacks/mechaknight/mechaknight_buff",
        ["event:/sfx/enemy/enemy_attacks/tracker_ruby_raider/tracker_ruby_raider_cast"] =
            "event:/sfx/enemy/enemy_attacks/tracker_ruby_raider/tracker_ruby_raider_buff",
    };

    private static readonly HashSet<string> NonexistentSfxPaths = new(StringComparer.OrdinalIgnoreCase)
    {
        "event:/sfx/enemy/enemy_attacks/eye_with_teeth/eye_with_teeth_die",
        "event:/sfx/enemy/enemy_attacks/eye_with_teeth/eye_with_teeth_cast",
        "event:/sfx/enemy/enemy_attacks/living_shield/living_shield_attack",
        "event:/sfx/enemy/enemy_attacks/living_shield/living_shield_cast",
        "event:/sfx/enemy/enemy_attacks/living_shield/living_shield_die",
        "event:/sfx/enemy/enemy_attacks/brute_ruby_raider/brute_ruby_raider_cast",
        "event:/sfx/enemy/enemy_attacks/assassin_ruby_raider/assassin_ruby_raider_cast",
        "event:/sfx/enemy/enemy_attacks/axe_ruby_raider/axe_ruby_raider_cast",
        "event:/sfx/enemy/enemy_attacks/crossbow_ruby_raider/crossbow_ruby_raider_cast",
        "event:/sfx/enemy/enemy_attacks/flail_knight/flail_knight_attack",
        "event:/sfx/enemy/enemy_attacks/spectral_knight/spectral_knight_attack",
        "event:/sfx/enemy/enemy_attacks/globe_head/globe_head_attack",
    };

#if HAVE_GODOTSHARP
    private static readonly Godot.StringName StopMusicMethod = new("stop_music");
    private static FieldInfo? _proxyField;
    private static FieldInfo? _currentTrackField;
    private static bool _musicFieldsResolved;
#endif

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Audio.NAudioManager", "PlayOneShot",
                  ArgumentTypeNames = new[]
                  {
                      "System.String",
                      "System.Collections.Generic.Dictionary`2<System.String,System.Single>",
                      "System.Single",
                  },
                  Optional = true)]
    public static bool FilterAndRemapOneShot(ref string __arg0, Dictionary<string, float>? __arg1)
    {
        if (string.IsNullOrEmpty(__arg0))
            return false;

        if (RemappedSfxPaths.TryGetValue(__arg0, out string? mapped))
        {
            __arg0 = mapped;
        }
        else if (NonexistentSfxPaths.Contains(__arg0))
        {
            return false;
        }

        if (__arg1 is not null && __arg1.Count > 0 && __arg1.ContainsKey("EnemyImpact_Intensity"))
        {
            // Only generic enemy_impact_enemy_size events define EnemyImpact_Intensity.
            if (!__arg0.StartsWith("event:/sfx/enemy/enemy_impact_enemy_size/", StringComparison.Ordinal))
            {
                __arg1.Remove("EnemyImpact_Intensity");
            }
        }

        return true;
    }

    // Stop and release current_music BEFORE LoadActBank unloads the old Act's
    // FMOD bank, preventing FMOD_ERR_INVALID_HANDLE (36) in FmodEvent::stop/release.
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Audio.NRunMusicController", "LoadActBank", Optional = true)]
    public static void StopOldMusicBeforeActBankUnload(object __instance)
    {
#if HAVE_GODOTSHARP
        try
        {
            if (!_musicFieldsResolved)
            {
                var t = __instance.GetType();
                const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                _proxyField = t.GetField("_proxy", flags);
                _currentTrackField = t.GetField("_currentTrack", flags);
                _musicFieldsResolved = true;
            }

            if (_currentTrackField?.GetValue(__instance) is string cur && !string.IsNullOrEmpty(cur))
            {
                if (_proxyField?.GetValue(__instance) is Godot.Node proxy && Godot.GodotObject.IsInstanceValid(proxy))
                {
                    proxy.Call(StopMusicMethod);
                }
            }
        }
        catch
        {
        }
#endif
    }
}
