using System;

namespace STS2Switch.Patches;

/// <summary>
/// A/B switch for the hover-tip system, to test whether it is what crashes.
///
/// Three crashes, all at the same instruction with the same six-frame
/// backtrace, and the object being walked resolves through its vtable's
/// typeinfo to <c>RichTextLabel</c> once and <c>Node2D</c> once -- which is
/// exactly what a hover tip is built from.
///
/// The faulting code reads a pointer from the object at +0x08 and dereferences
/// it. What is actually there is two 32-bit values: the low word is 1 in all
/// three crashes and the high word varies (0, 50, 95). So the reader expects a
/// pointer and the object holds a pair of ints -- either a type confusion, or
/// an object being touched while it is torn down.
///
/// Both readings fit hover tips: they are created and freed constantly as the
/// pointer moves over cards and powers, which is far more churn than anything
/// else on screen.
///
/// This does NOT fix anything. It answers one question -- if the crash stops
/// with hover tips suppressed, the search narrows from the whole engine to one
/// subsystem. Off by default; set `no_hover_tips 1` in config.txt.
/// </summary>
[SwitchPatches(Note = "Hover-tip A/B; diagnostic only, off by default")]
public static class HoverTipPatches
{
    // Read once. Environment.GetEnvironmentVariable goes through .NET's cached
    // copy of environ, which the wrapper populates before the runtime starts.
    private static readonly bool Disabled =
        Environment.GetEnvironmentVariable("STS2_NO_HOVER_TIPS") == "1";

    /// <summary>
    /// Skips adding a tip to the container, which leaves it empty and stops
    /// the RichTextLabel/Node2D churn without removing the container itself.
    ///
    /// Targets <c>Add</c>, not <c>_Ready</c>. The class does not override
    /// <c>_Ready</c> at all -- its methods are get_Tips, Add,
    /// LayoutResizeAndReposition and Godot's generated four. The weaver
    /// reported "no assembly contains this type", which was wrong: the type is
    /// exactly where it was expected, and only the method was missing.
    ///
    /// Returns false only when the flag is set, so a normal build is untouched.
    /// </summary>
    private static int _announced;

    [SwitchPrefix("MegaCrit.Sts2.Core.Nodes.HoverTips.NHoverTipCardContainer",
                  "Add", Optional = true)]
    public static bool SkipHoverTipAdd()
    {
        // Say it on the FIRST call either way, not only when suppressing.
        //
        // Run 42 produced no suppression line and no way to tell whether the
        // flag was off, the config key was misspelled, the patch had not woven,
        // or Add was simply never reached. Four very different problems, one
        // silence. A line on the first call collapses that to one.
        if (System.Threading.Interlocked.Exchange(ref _announced, 1) == 0)
            SwitchLog.Info(Disabled
                ? "NHoverTipCardContainer.Add reached; SUPPRESSING (no_hover_tips 1)"
                : "NHoverTipCardContainer.Add reached; passing through "
                  + "(no_hover_tips is 0)");

        return !Disabled;
    }
}
