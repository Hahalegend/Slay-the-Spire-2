// Program.cs -- StS2Patcher entry point.
//
//   StS2Patcher --game <dir> --patches <STS2Switch.dll> --out <dir> [--strict]
//
// Reads every [SwitchPatches] class out of the patch assembly, resolves each
// declared target against the game assemblies in --game, weaves the calls in,
// and writes the rewritten assemblies to --out. Assemblies with no patches are
// copied through unchanged so --out is a complete, ILC-ready input set.
//
// On the circular reference this creates: the rewritten sts2.dll calls into
// STS2Switch.dll, which references sts2.dll. C# cannot express that, but it is
// legal IL and the runtime resolves it fine -- and ILC in particular works on
// the whole closure at once, so the cycle never has to be broken. If it ever
// does become a problem, the fix is to merge the patch types INTO sts2.dll
// rather than to restructure the patches.
//
// MIT license; see LICENSE.

using Mono.Cecil;

namespace StS2Patcher;

public static class Program
{
    /// <summary>
    /// Prints every type whose full name contains <paramref name="pattern"/>
    /// (case-insensitive), with its methods. For finding a patch target that
    /// has been renamed between game versions.
    /// </summary>
    private static int ListTypes(string gameDir, string pattern)
    {
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(gameDir);
        var rp = new ReaderParameters { AssemblyResolver = resolver, ReadSymbols = false };

        int hits = 0;
        foreach (var path in Directory.GetFiles(gameDir, "*.dll").OrderBy(f => f))
        {
            ModuleDefinition module;
            try { module = ModuleDefinition.ReadModule(path, rp); }
            catch { continue; }

            using (module)
            {
                foreach (var type in module.GetTypes())
                {
                    if (type.FullName.IndexOf(pattern, StringComparison.OrdinalIgnoreCase) < 0)
                        continue;

                    hits++;
                    Console.WriteLine($"\n{type.FullName}");
                    Console.WriteLine($"    [{Path.GetFileName(path)}]");

                    foreach (var m in type.Methods.OrderBy(m => m.Name))
                    {
                        var ps = string.Join(", ", m.Parameters.Select(
                            x => $"{x.ParameterType.Name} {x.Name}"));
                        var kind = m.IsStatic ? "static " : "";
                        Console.WriteLine($"      {kind}{m.ReturnType.Name} {m.Name}({ps})");
                    }

                    foreach (var pr in type.Properties.OrderBy(x => x.Name))
                        Console.WriteLine($"      property {pr.PropertyType.Name} {pr.Name}"
                            + $" {{{(pr.GetMethod is not null ? " get;" : "")}"
                            + $"{(pr.SetMethod is not null ? " set;" : "")} }}");
                }
            }
        }

        Console.WriteLine($"\n{hits} type(s) matching \"{pattern}\"");
        if (hits == 0)
            Console.WriteLine("Try a shorter pattern -- e.g. \"ModelDb\" rather than the full namespace.");
        return 0;
    }

    /// <summary>
    /// Dumps exactly the GodotSharp API needed to write the NativeAOT entry
    /// point, and nothing else.
    ///
    /// Godot's SDK normally contributes GodotPlugins.Game.Main, which carries
    /// [UnmanagedCallersOnly(EntryPoint = "godotsharp_game_main_init")]. That
    /// assembly is generated only when building with PublishAot under
    /// Godot.NET.Sdk, so it exists in neither the shipped game nor the Android
    /// launcher, and this pipeline has to supply an equivalent.
    ///
    /// Writing it from memory is how you get a payload that compiles and then
    /// does nothing. This prints the real signatures instead: the bootstrap
    /// types, and any method taking an 'outManagedCallbacks' parameter, which
    /// is the one that varies most across Godot versions.
    /// </summary>
    private static int BootstrapInfo(string gameDir)
    {
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(gameDir);
        var rp = new ReaderParameters { AssemblyResolver = resolver, ReadSymbols = false };

        string[] wantTypes =
        {
            "GodotDllImportResolver",
            "ScriptManagerBridge",
            "ManagedCallbacks",
            "GodotPlugins",
        };

        Console.WriteLine("=== bootstrap types ===");
        foreach (var path in Directory.GetFiles(gameDir, "*.dll").OrderBy(f => f))
        {
            ModuleDefinition module;
            try { module = ModuleDefinition.ReadModule(path, rp); } catch { continue; }

            using (module)
            {
                foreach (var type in module.GetTypes())
                {
                    if (!wantTypes.Any(w => type.Name.Contains(w, StringComparison.Ordinal)))
                        continue;

                    Console.WriteLine($"\n{type.FullName}   [{Path.GetFileName(path)}]");
                    foreach (var m in type.Methods.Where(m => m.IsPublic || m.IsAssembly)
                                                  .OrderBy(m => m.Name))
                    {
                        var ps = string.Join(", ", m.Parameters.Select(
                            x => $"{x.ParameterType.FullName} {x.Name}"));
                        Console.WriteLine($"   {(m.IsStatic ? "static " : "")}{m.ReturnType.FullName} {m.Name}({ps})");
                    }
                }
            }
        }

        Console.WriteLine("\n=== methods taking 'outManagedCallbacks' ===");
        foreach (var path in Directory.GetFiles(gameDir, "*.dll").OrderBy(f => f))
        {
            ModuleDefinition module;
            try { module = ModuleDefinition.ReadModule(path, rp); } catch { continue; }

            using (module)
            {
                foreach (var type in module.GetTypes())
                foreach (var m in type.Methods)
                {
                    if (!m.Parameters.Any(x => x.Name is "outManagedCallbacks")) continue;
                    var ps = string.Join(", ", m.Parameters.Select(
                        x => $"{x.ParameterType.FullName} {x.Name}"));
                    Console.WriteLine($"   [{Path.GetFileName(path)}] {(m.IsStatic ? "static " : "")}" +
                                      $"{m.ReturnType.FullName} {type.FullName}.{m.Name}({ps})");
                }
            }
        }

        Console.WriteLine("\n=== NativeFuncs bootstrap entry points ===");
        foreach (var path in Directory.GetFiles(gameDir, "*.dll").OrderBy(f => f))
        {
            ModuleDefinition module;
            try { module = ModuleDefinition.ReadModule(path, rp); } catch { continue; }

            using (module)
            {
                foreach (var type in module.GetTypes())
                {
                    if (type.Name != "NativeFuncs") continue;
                    foreach (var m in type.Methods.Where(m =>
                                 m.Name.Contains("Initialize", StringComparison.Ordinal)))
                    {
                        var ps = string.Join(", ", m.Parameters.Select(
                            x => $"{x.ParameterType.FullName} {x.Name}"));
                        Console.WriteLine($"   [{Path.GetFileName(path)}] {(m.IsStatic ? "static " : "")}" +
                                          $"{m.ReturnType.FullName} {type.FullName}.{m.Name}({ps})");
                    }
                }
            }
        }

        return 0;
    }

    public static int Main(string[] args)
    {
        try
        {
            return Run(args);
        }
        catch (Exception ex)
        {
            // Exit 3, distinct from 1 (weave failures) and 2 (bad arguments),
            // so docker-entry.sh does not report a crash as "a patch target
            // could not be resolved".
            Console.Error.WriteLine();
            Console.Error.WriteLine($"StS2Patcher crashed: {ex.GetType().Name}: {ex.Message}");
            Console.Error.WriteLine(ex.StackTrace);
            return 3;
        }
    }

    private static int Run(string[] args)
    {
        string? gameDir = null, patchAsm = null, outDir = null, listPattern = null;
        bool strict = false, bootstrapInfo = false;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--game":    gameDir  = args[++i]; break;
                case "--patches": patchAsm = args[++i]; break;
                case "--out":     outDir   = args[++i]; break;
                // Escalates Optional patches to required. Off by default.
                case "--strict":  strict   = true;      break;
                case "--list-types": listPattern = args[++i]; break;
                case "--bootstrap-info": bootstrapInfo = true; break;
                default:
                    Console.Error.WriteLine($"unknown argument: {args[i]}");
                    return 2;
            }
        }

        // --list-types is a lookup mode, not a build mode. When a patch target
        // fails to resolve, the useful next question is "what is it called
        // now?", and answering that by guessing costs a full weave per
        // attempt. This dumps the real types and their methods so the name can
        // be read off directly.
        if (listPattern is not null)
        {
            if (gameDir is null)
            {
                Console.Error.WriteLine("--list-types also needs --game <dir>");
                return 2;
            }
            return ListTypes(gameDir, listPattern);
        }

        if (bootstrapInfo)
        {
            if (gameDir is null)
            {
                Console.Error.WriteLine("--bootstrap-info also needs --game <dir>");
                return 2;
            }
            return BootstrapInfo(gameDir);
        }

        if (gameDir is null || patchAsm is null || outDir is null)
        {
            Console.Error.WriteLine(
                "usage: StS2Patcher --game <dir> --patches <STS2Switch.dll> --out <dir> [--strict]");
            return 2;
        }

        Directory.CreateDirectory(outDir);

        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(gameDir);
        resolver.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(patchAsm))!);

        var readerParams = new ReaderParameters
        {
            AssemblyResolver = resolver,
            ReadWrite = false,
            ReadSymbols = false,   // shipped assemblies have no portable PDBs
        };

        using var patchModule = ModuleDefinition.ReadModule(patchAsm, readerParams);

        var gameAssemblies = Directory.GetFiles(gameDir, "*.dll")
                                      .OrderBy(f => f)
                                      .ToList();

        Console.WriteLine($"patch assembly : {Path.GetFileName(patchAsm)}");
        Console.WriteLine($"game assemblies: {gameAssemblies.Count} in {gameDir}");
        Console.WriteLine();

        var discovery = new PatchDiscovery(patchModule, Console.WriteLine);
        var declared = discovery.Discover();

        // Every type name seen, so an unresolved target can suggest the right
        // namespace instead of only reporting that the wrong one is absent.
        var seenTypes = new List<string>();
        // Method names per type, so an unresolved method can list what the type
        // actually declares instead of sending the search after the namespace.
        var seenMethods = new Dictionary<string, List<string>>();

        if (declared.Count == 0)
        {
            Console.Error.WriteLine("no [SwitchPatches] classes found -- nothing to do");
            return 1;
        }

        Console.WriteLine($"{declared.Count} patch declaration(s)");
        Console.WriteLine();

        int applied = 0, skipped = 0, failed = 0;
        var matched = new HashSet<PatchDeclaration>();
        var errored = new HashSet<PatchDeclaration>();

        foreach (var asmPath in gameAssemblies)
        {
            var name = Path.GetFileName(asmPath);
            var outPath = Path.Combine(outDir, name);

            // The patch assembly itself is copied, not patched.
            if (Path.GetFullPath(asmPath) == Path.GetFullPath(patchAsm))
            {
                File.Copy(asmPath, outPath, overwrite: true);
                continue;
            }

            ModuleDefinition module;
            try
            {
                module = ModuleDefinition.ReadModule(asmPath, readerParams);
                foreach (var t in module.GetTypes())
                {
                    seenTypes.Add(t.FullName);
                    seenMethods[t.FullName] =
                        t.Methods.Select(m => m.Name).Distinct().ToList();
                }
            }
            catch (Exception ex)
            {
                // Native or resource-only files sitting in the same directory.
                Console.WriteLine($"  {name}: not a managed assembly ({ex.GetType().Name}), copied as-is");
                File.Copy(asmPath, outPath, overwrite: true);
                continue;
            }

            // Declared outside the using so the copy fallback can run after
            // Cecil has released the source file.
            bool wovenHere = false;
            bool needsCopy = false;

            using (module)
            {
                // A null TargetAssemblyName means the patch named its target
                // rather than typeof()'ing it, so we do not know which assembly
                // holds it -- try this one and let ResolveTarget say no. The
                // per-declaration "did anything match anywhere" check below is
                // what turns a never-matched name into a build failure.
                var forThisModule = declared
                    .Where(d => d.TargetAssemblyName is null ||
                                d.TargetAssemblyName == module.Assembly.Name.Name)
                    .ToList();

                if (forThisModule.Count == 0)
                {
                    File.Copy(asmPath, outPath, overwrite: true);
                    continue;
                }

                Console.WriteLine($"  {name}");

                var weaver = new MethodWeaver(module, Console.WriteLine);
                var grouped = new Dictionary<MethodDefinition, PatchSet>();

                foreach (var decl in forThisModule)
                {
                    MethodDefinition? target;
                    try
                    {
                        target = discovery.ResolveTarget(module, decl);
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"    FAILED {decl.Describe()}: {ex.Message}");
                        failed++;
                        // Record it, or the unmatched-name sweep below reports
                        // the SAME declaration a second time as "no assembly
                        // contains this type" -- which is both wrong (the type
                        // was found; the overload was ambiguous) and doubles
                        // the failure count.
                        errored.Add(decl);
                        continue;
                    }

                    if (target is null)
                    {
                        // Not in THIS assembly. For a name-targeted patch that
                        // is expected -- it is only a failure if no assembly
                        // matched, which the unmatched sweep after the loop
                        // reports once rather than once per assembly.
                        if (decl.TargetAssemblyName is null) continue;

                        if (decl.Optional && !strict)
                        {
                            Console.WriteLine($"    skipped (optional, not found) {decl.Describe()}");
                            skipped++;
                        }
                        else
                        {
                            Console.WriteLine($"    FAILED {decl.Describe()}: target not found");
                            failed++;
                        }
                        continue;
                    }

                    matched.Add(decl);

                    if (!grouped.TryGetValue(target, out var set))
                        grouped[target] = set = new PatchSet();

                    switch (decl.Kind)
                    {
                        case PatchKind.Prefix:  set.Prefixes.Add(decl.PatchMethod);  break;
                        case PatchKind.Postfix: set.Postfixes.Add(decl.PatchMethod); break;
                        case PatchKind.Replace:
                            if (set.Replace is not null)
                            {
                                Console.WriteLine($"    FAILED {decl.Describe()}: target already replaced");
                                failed++;
                            }
                            else set.Replace = decl.PatchMethod;
                            break;
                    }
                }

                foreach (var (target, set) in grouped)
                {
                    if (set.IsEmpty) continue;
                    if (set.Replace is not null && (set.Prefixes.Count > 0 || set.Postfixes.Count > 0))
                    {
                        Console.WriteLine($"    FAILED {target.FullName}: cannot combine replace with prefix/postfix");
                        failed++;
                        continue;
                    }

                    Console.WriteLine($"    {target.FullName}");
                    try
                    {
                        weaver.Weave(target, set);
                        applied++;
                        wovenHere = true;
                    }
                    catch (WeaveException ex)
                    {
                        Console.WriteLine($"    FAILED: {ex.Message}");
                        failed++;
                    }
                }

                // Only REWRITE a module we actually changed.
                //
                // The earlier version wrote every module it could read. That
                // was harmless-looking but wrong twice over: it rewrites ~200
                // assemblies to change two, and Cecil cannot write a
                // mixed-mode assembly it can happily read -- which crashed the
                // whole run on an assembly that had nothing to do with any
                // patch.
                //
                // The "nothing to do" fast path above never fired because
                // every patch here is name-targeted, so TargetAssemblyName is
                // null and forThisModule is never empty. The real test is
                // whether a weave succeeded, which is what wovenHere records.
                if (wovenHere)
                {
                    try
                    {
                        module.Write(outPath);
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"    FAILED to write {name}: {ex.Message}");
                        failed++;
                    }
                }
                else
                {
                    needsCopy = true;
                }
            }

            // Outside the using: Cecil has released the source file.
            if (needsCopy) File.Copy(asmPath, outPath, overwrite: true);
        }

        // The weave creates references FROM the game assemblies INTO the patch
        // assembly, so the patch assembly and anything it needs must ship in
        // the output set. Otherwise ILC reports "Failed to load assembly
        // 'STS2Switch'" -- a dangling reference this tool created itself.
        //
        // The existing copy-through above only fires when the patch assembly
        // happens to live inside --game, which it does not: it is built
        // separately and passed via --patches.
        var patchDir = Path.GetDirectoryName(Path.GetFullPath(patchAsm))!;
        int carried = 0;
        foreach (var dep in Directory.GetFiles(patchDir, "*.dll"))
        {
            var target = Path.Combine(outDir, Path.GetFileName(dep));
            if (File.Exists(target)) continue;      // a game assembly of the same name wins
            File.Copy(dep, target, overwrite: true);
            Console.WriteLine($"  carried into output: {Path.GetFileName(dep)}");
            carried++;
        }
        if (carried == 0)
            Console.WriteLine($"  WARNING: nothing carried from {patchDir}; " +
                              "the woven references will not resolve");

        // Name-targeted patches that matched nothing anywhere. This is the
        // check that keeps a renamed type from silently dropping a patch --
        // exactly the failure the Android launcher's PatchHelper logs and then
        // continues past ("FAILED {type}.{method}: method not found").
        foreach (var decl in declared)
        {
            if (decl.TargetAssemblyName is not null) continue;
            if (matched.Contains(decl)) continue;
            if (errored.Contains(decl)) continue;   // already reported, with a better reason

            // Near misses. "No assembly contains this type" is true and
            // unhelpful; the usual cause is a wrong namespace and the right one
            // is sitting in the assembly.
            //
            // Matching on "." alone missed the first real case and printed
            // nothing at all, which reads exactly like "there are no
            // candidates". Cecil writes nested types as Outer/Inner, and a type
            // in the global namespace has no separator at all, so the simple
            // name is whatever follows the last '.' OR '/'.
            static string Simple(string n)
            {
                int i = n.LastIndexOfAny(new[] { '.', '/' });
                return i < 0 ? n : n[(i + 1)..];
            }

            // Was the TYPE found, or only the method missing? Those are very
            // different problems and the message conflated them: it reported
            // "no assembly contains this type" for
            // NHoverTipCardContainer._Ready, when the type is exactly where it
            // was expected and only _Ready does not exist -- the class never
            // overrides it. That cost a build and sent the search after a
            // namespace that was already right.
            var typeExists = seenTypes.Contains(decl.TargetTypeFullName);

            var shortName = Simple(decl.TargetTypeFullName);
            var exact = seenTypes.Where(n => Simple(n) == shortName
                                             && n != decl.TargetTypeFullName)
                                 .Distinct().Take(5).ToList();
            // A PREFIX stem, not a truncation of the end. Chopping the last
            // four characters off NHoverTipCardContainer gives
            // NHoverTipCardCont, which matches almost nothing; the useful stem
            // is the front. The leading N is this codebase's Godot-node
            // convention and carries no information, so it comes off first:
            // NHoverTipCardContainer -> HoverTip, which finds NHoverTipContainer,
            // HoverTipPanel and anything else in the family.
            var bare = shortName.Length > 1 && shortName[0] == 'N' && char.IsUpper(shortName[1])
                ? shortName[1..] : shortName;
            var stem = bare.Length > 8 ? bare[..8] : bare;
            // Ranked, not just filtered. An unranked Take(8) returned eight
            // Settings-screen tooltips in assembly order -- NFastModeHoverTip,
            // NMsaaHoverTip and friends -- while the one actually being looked
            // for sat further down the same list, invisible.
            //
            // A name that STARTS with the stem is a far better guess than one
            // that merely contains it: NHoverTipCardContainer over
            // NFastModeHoverTip. Ordering by that, then by how close the length
            // is to the target, puts the likely answer first.
            var fuzzy = exact.Count > 0
                ? new List<string>()
                : seenTypes
                    .Where(n => Simple(n).Contains(stem, StringComparison.OrdinalIgnoreCase))
                    .Distinct()
                    .OrderByDescending(n => Simple(n).StartsWith(stem, StringComparison.OrdinalIgnoreCase))
                    .ThenBy(n => Math.Abs(Simple(n).Length - shortName.Length))
                    .Take(12)
                    .ToList();
            var nearMisses = exact.Count > 0 ? exact : fuzzy;

            // --strict escalates Optional to required. Useful for a release
            // build, where knowing that a patch silently dropped is worth a
            // failed build -- but it is NOT the right default, and it was
            // being passed by default, which made every Optional marking
            // useless and reported a genuinely-absent type as a hard failure.
            if (decl.Optional && !strict)
            {
                Console.WriteLine(typeExists
                    ? $"  skipped (optional; type found, no method "
                      + $"'{decl.TargetMethodName}') {decl.Describe()}"
                    : $"  skipped (optional, no assembly had it) {decl.Describe()}");
                skipped++;
            }
            else
            {
                Console.WriteLine(typeExists
                    ? $"  FAILED {decl.Describe()}: the type exists but has no "
                      + $"method '{decl.TargetMethodName}'"
                    : $"  FAILED {decl.Describe()}: no assembly contains this type");
                failed++;
            }

            if (typeExists)
            {
                var methods = seenMethods.TryGetValue(decl.TargetTypeFullName, out var ms)
                    ? ms : new List<string>();
                Console.WriteLine($"      {decl.TargetTypeFullName} declares: "
                                  + (methods.Count > 0
                                     ? string.Join(", ", methods.Take(15))
                                     : "(none recorded)"));
            }
            else if (nearMisses.Count > 0)
            {
                Console.WriteLine(exact.Count > 0
                    ? "      same class name, different namespace:"
                    : $"      nothing named '{shortName}'; types containing '{stem}':");
                foreach (var m in nearMisses)
                    Console.WriteLine($"        {m}");
            }
            else
            {
                // Say it. Printing nothing looks identical to not having
                // looked, and that ambiguity cost a build.
                Console.WriteLine($"      no type resembling '{shortName}' in any of "
                                  + $"{seenTypes.Distinct().Count()} types across the game "
                                  + "assemblies");
            }
        }

        Console.WriteLine();
        Console.WriteLine($"applied {applied}, skipped {skipped}, failed {failed}");
        Console.WriteLine($"output: {outDir}");

        // Any failure is fatal. A patch that silently did not apply is the
        // exact bug class this tool exists to make impossible -- with Harmony
        // it would have been a log line at startup on a device with no console.
        return failed > 0 ? 1 : 0;
    }
}
