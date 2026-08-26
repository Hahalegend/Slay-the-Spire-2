using System;
using Godot;

namespace STS2Switch;

/// <summary>
/// Loads an ASTC texture overlay pack over the shipped one, if present.
///
/// sts2.pck is a desktop export: 1532 textures are raw RGBA8, 707 are raw RGB8,
/// and the remaining 1222 are S3TC or BPTC, which Tegra X1's GLES driver cannot
/// sample -- so mesa expands every one of those to RGBA8 at load. That comes to
/// roughly 7 GB of texture data on a console with about 3.2 GB, which is the
/// real reason a three-enemy fight runs out of memory.
///
/// tools/astc_convert.py re-encodes them as ASTC 8x8 (0.25 bytes/pixel, sampled
/// in hardware on this GPU), which brings the same textures to about 444 MB.
///
/// The output is an OVERLAY -- only the converted textures -- and
/// <c>replace_files: true</c> makes it shadow the originals inside the untouched
/// sts2.pck. That is deliberate: it is a third of the size, the original is
/// never rewritten, and an interrupted or bad conversion cannot leave an
/// install that will not boot. Delete the overlay and everything is back to
/// stock.
/// </summary>
public static class AstcOverlay
{
    // user:// resolves through Godot's own path handling, which is the same
    // route the saves take. See the note in ModelDbPatches about System.IO not
    // reaching Godot's virtual paths.
    // Several plausible spots, because "put it next to settings.save" is
    // ambiguous advice that cost a hardware run: settings.save lives in a
    // PROFILE directory (user://default/1/), while user:// itself is the save
    // root one level up. Both are reasonable places to look, so look in both
    // and say which was used.
    private static readonly string[] Candidates =
    {
        "user://sts2_astc.pck",             // the save root -- <sd>/switch/<app>/save/
        "user://default/1/sts2_astc.pck",   // beside settings.save
        "res://sts2_astc.pck",              // beside sts2.pck
    };

    private static int _done;

    public static void Ensure()
    {
        if (System.Threading.Interlocked.Exchange(ref _done, 1) != 0) return;

        try
        {
            string? found = null;   // nullable: CS8600 otherwise
            foreach (var c in Candidates)
            {
                if (Godot.FileAccess.FileExists(c)) { found = c; break; }
            }

            if (found is null)
            {
                // Naming every path checked, so a misplaced file is a one-line
                // diagnosis instead of another hardware round.
                SwitchLog.Info("no ASTC overlay found; using the shipped desktop "
                             + "textures (expect high texture memory). Looked in: "
                             + string.Join(", ", Candidates));
                return;
            }

            // replace_files: true is the whole mechanism. Without it the pack
            // loads and every path already present in sts2.pck keeps pointing
            // at the original, so nothing changes and nothing says why.
            bool ok = ProjectSettings.LoadResourcePack(found, replaceFiles: true);
            SwitchLog.Info(ok
                ? $"ASTC overlay loaded from {found}; textures now come from it"
                : $"ASTC overlay at {found} FAILED to load; stock textures in use");
        }
        catch (Exception ex)
        {
            // Never fatal: without the overlay the game is exactly as it was.
            SwitchLog.Error("ASTC overlay: " + ex.Message);
        }
    }
}
