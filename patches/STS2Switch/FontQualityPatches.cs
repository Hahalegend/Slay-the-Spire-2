// FontQualityPatches.cs -- Enable trilinear glyph mipmaps on loaded FontFile
// resources once at load time for razor-sharp 720p card & UI text.
//
// Slay the Spire 2 authors its UI at 1920x1080 and scales it down to 1280x720
// (0.667x), with cards in hand scaled down even further (~0.55x). Stock .fontdata
// imports have GenerateMipmaps=false, which causes downscaled glyph shimmering
// and softer edges. Enabling GenerateMipmaps=true once per unique FontFile
// (paired with trilinear mipmaps in override.cfg) makes scaled card text much
// crisper with zero layout changes and zero per-frame overhead.

using System;
using System.Collections.Generic;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Enable glyph mipmaps once per FontFile for crisp scaled 720p card text")]
public static class FontQualityPatches
{
#if HAVE_GODOTSHARP
    private static readonly HashSet<IntPtr> ConfiguredFonts = new();
    private static readonly Godot.StringName LabelFontProp = new("font");
    private static readonly Godot.StringName LabelThemeType = new("Label");
    private static readonly Godot.StringName RichNormalFontProp = new("normal_font");
    private static readonly Godot.StringName RichBoldFontProp = new("bold_font");
    private static readonly Godot.StringName RichItalicsFontProp = new("italics_font");
    private static readonly Godot.StringName RichThemeType = new("RichTextLabel");
    private static int _configuredCount;

    private static void ConfigureFont(Godot.Font? font)
    {
        if (font is null || !Godot.GodotObject.IsInstanceValid(font))
            return;

        if (font is Godot.FontFile ff)
        {
            IntPtr ptr = ff.NativeInstance;
            if (ptr == IntPtr.Zero || !ConfiguredFonts.Add(ptr))
                return;

            try
            {
                if (!ff.GenerateMipmaps)
                {
                    ff.GenerateMipmaps = true;
                    _configuredCount++;
                    if (_configuredCount == 1 || _configuredCount == 4)
                        SwitchLog.Info($"FontQualityPatches: enabled glyph mipmaps on {_configuredCount} FontFile(s)");
                }
            }
            catch { }
        }
        else if (font is Godot.FontVariation fv)
        {
            IntPtr ptr = fv.NativeInstance;
            if (ptr == IntPtr.Zero || !ConfiguredFonts.Add(ptr))
                return;
            try { ConfigureFont(fv.BaseFont); } catch { }
        }
    }

    [SwitchPostfix("MegaCrit.Sts2.Core.Localization.Fonts.FontManager", "GetFontForLanguage", Optional = true)]
    public static void EnhanceLoadedLanguageFont(Godot.Font? __result)
    {
        ConfigureFont(__result);
    }

    [SwitchPostfix("MegaCrit.Sts2.addons.mega_text.MegaLabel", "RefreshFont", Optional = true)]
    public static void EnhanceMegaLabelFont(object __instance)
    {
        if (__instance is not Godot.Control ctrl || !Godot.GodotObject.IsInstanceValid(ctrl))
            return;
        try { ConfigureFont(ctrl.GetThemeFont(LabelFontProp, LabelThemeType)); } catch { }
    }

    [SwitchPostfix("MegaCrit.Sts2.addons.mega_text.MegaRichTextLabel", "RefreshFont", Optional = true)]
    public static void EnhanceMegaRichTextFont(object __instance)
    {
        if (__instance is not Godot.Control ctrl || !Godot.GodotObject.IsInstanceValid(ctrl))
            return;
        try
        {
            ConfigureFont(ctrl.GetThemeFont(RichNormalFontProp, RichThemeType));
            ConfigureFont(ctrl.GetThemeFont(RichBoldFontProp, RichThemeType));
            ConfigureFont(ctrl.GetThemeFont(RichItalicsFontProp, RichThemeType));
        }
        catch { }
    }
#endif
}
