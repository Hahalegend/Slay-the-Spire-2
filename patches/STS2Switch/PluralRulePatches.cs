// PluralRulePatches -- make SmartFormat's plural rules work under
// InvariantGlobalization.
//
// The visible symptom is raw template text on screen:
//
//   {RevealableEpochCount} {RevealableEpochCount:plural:Epoch is|Epochs are}
//   waiting to be revealed...
//
// and, on every card hover, a caught exception:
//
//   [ERROR] Localization formatting error!
//   message=Error parsing format string: IsoLangToDelegate not found for iv
//           (Parameter 'twoLetterIsoLanguageName') at 44
//   Plays this card an additional {Times:plural:time|[blue]{}[/blue] times}.
//   --------------------------------------------^
//
// `iv` is the INVARIANT culture's two-letter ISO language name.
// SmartFormat.Utilities.PluralRules keeps a table keyed by language -- "en",
// "de", "fr", "ru" and so on -- and there is no "iv", because invariant is not
// a language. So every format string containing `:plural:` throws, and the
// game falls back to printing the template.
//
// ===========================================================================
// Why the culture cannot simply be set instead
// ===========================================================================
//
// SwitchCulture.cs already does the obvious thing, and already reports why it
// is not enough. From the log:
//
//   [sts2 ERROR] culture is en-US but the two-letter code is still 'iv' --
//   InvariantGlobalization means this CANNOT report 'en', so SmartFormat
//   plural rules will still fail. See SwitchCulture.cs.
//
// `sts2.aot.props` sets PredefinedCulturesOnly=false so `new CultureInfo("en-US")`
// succeeds, and it does -- CurrentCulture.Name really is "en-US". But under
// InvariantGlobalization every culture still *behaves* invariantly, and
// TwoLetterISOLanguageName comes from that behaviour, not from the name. It
// returns "iv" whatever the culture is called.
//
// Turning InvariantGlobalization off is the other way, and it is a much bigger
// change: it needs ICU on the console, and the pvzultimate notes record ICU
// misbehaving badly there (u_tolower returning 0, which empties every string).
// Not worth it to fix pluralisation.
//
// So the fix is at the lookup instead: when SmartFormat asks for the rule for
// "iv", answer with the rule for "en".
//
// ===========================================================================
// Why this is a lie worth telling
// ===========================================================================
//
// It is a substitution, and it is only correct because this build ships
// English. A German build would pluralise wrongly and silently.
//
// That is acceptable here and would not be in general, so the guard is
// explicit: ONLY "iv" is rewritten. Any other language code passes through
// untouched, so if localisation ever reports a real language, this patch stops
// applying rather than overriding it.
//
// MIT license; see LICENSE.

using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "SmartFormat plural rules under InvariantGlobalization")]
public static class PluralRulePatches
{
    // SmartFormat.Utilities.PluralRules.GetPluralRule(string) -> PluralRuleDelegate
    //
    // public static, one string parameter -- confirmed from the metadata
    // signature (00 01 12 81cc 0e: DEFAULT, 1 arg, CLASS return, STRING param)
    // rather than assumed from the name.
    //
    // __arg0 by ref rewrites the argument in place; the original then runs and
    // does its own table lookup, just with a language it knows. Reimplementing
    // English pluralisation here instead would be more code and more to get
    // wrong.
    //
    // Returns VOID, not bool, and that is deliberate rather than incidental.
    //
    // A bool prefix advertises that it *can* skip the original. The weaver
    // then, correctly, refuses:
    //
    //   FAILED: InvariantSpeaksEnglish(String&) can skip GetPluralRule(String),
    //   which returns the reference type PluralRuleDelegate. Skipping would
    //   return null.
    //
    // It cannot see that this one always returned true. Rather than silence it
    // with a `ref PluralRuleDelegate __result` -- which would mean referencing
    // a type nested inside SmartFormat just to satisfy a branch that never
    // runs -- the signature now says what the patch actually does: it modifies
    // an argument and never takes over.
    //
    // Lives in SmartFormat.dll, not sts2.dll; the weaver searches every game
    // assembly when the type is not in the main one.
    [SwitchPrefix("SmartFormat.Utilities.PluralRules", "GetPluralRule",
                  Optional = true)]
    public static void InvariantSpeaksEnglish(ref string __arg0)
    {
        if (__arg0 == "iv")
        {
            if (!_warned)
            {
                _warned = true;
                SwitchLog.Info(
                    "SmartFormat asked for plural rules for 'iv' (invariant); " +
                    "using 'en'. Expected under InvariantGlobalization -- see " +
                    "PluralRulePatches.cs. Correct only for an English build.");
            }
            __arg0 = "en";
        }
    }

    private static bool _warned;
}
