// AudioPerfPatches.cs -- Eliminate synchronous microSD warning spam when
// SfxCmd.PlayDamage() passes 'EnemyImpact_Intensity' to enemy hurt FMOD events
// whose FMOD Studio definition omits that parameter.
//
// Hardware telemetry (godot.log on Switch):
//   - SfxCmd.PlayDamage(MonsterModel, int) unconditionally adds
//     {"EnemyImpact_Intensity", intensity} before calling NAudioManager.PlayOneShot().
//   - Enemies like Chomper (13x in one fight), Thieving Hopper (5x), and
//     Hunter Killer lack 'EnemyImpact_Intensity' on their hurt event, triggering
//     repeated synchronous `WARNING: FMOD parameter 'EnemyImpact_Intensity' not
//     found on event '...'` writes to godot.log on the Switch microSD card.
//   - Stripping 'EnemyImpact_Intensity' for those specific events plays the
//     exact same hurt SFX at full volume with zero warning spam.

using System;
using System.Collections.Generic;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Strip missing EnemyImpact_Intensity parameter from FMOD hurt events that lack it")]
public static class AudioPerfPatches
{
    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.Audio.NAudioManager", "PlayOneShot",
                  ArgumentTypeNames = new[]
                  {
                      "System.String",
                      "System.Collections.Generic.Dictionary`2<System.String,System.Single>",
                      "System.Single",
                  },
                  Optional = true)]
    public static void StripMissingEnemyImpactParam(string __arg0, Dictionary<string, float>? __arg1)
    {
        if (__arg1 is null || __arg1.Count == 0 || string.IsNullOrEmpty(__arg0))
            return;

        if (!__arg1.ContainsKey("EnemyImpact_Intensity"))
            return;

        if (__arg0.Contains("chomper_hurt", StringComparison.OrdinalIgnoreCase) ||
            __arg0.Contains("thieving_hopper_hurt", StringComparison.OrdinalIgnoreCase) ||
            __arg0.Contains("hunter_killer_hurt", StringComparison.OrdinalIgnoreCase))
        {
            __arg1.Remove("EnemyImpact_Intensity");
        }
    }
}
