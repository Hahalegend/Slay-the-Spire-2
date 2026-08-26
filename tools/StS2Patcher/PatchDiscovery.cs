// PatchDiscovery.cs -- reads patch declarations out of the patch assembly and
// resolves each one to a concrete MethodDefinition in a game assembly.
//
// Resolution is deliberately strict. Harmony's TargetMethod() indirection and
// its positional parameter matching both let a patch bind to something other
// than what the author meant, and on a device with no console the only symptom
// is that the game behaves as though the patch is not there. Every ambiguity
// here is a build error with the candidates printed.
//
// MIT license; see LICENSE.

using Mono.Cecil;

namespace StS2Patcher;

public enum PatchKind { Prefix, Postfix, Replace }

public enum MemberKind { Method = 0, PropertyGetter, PropertySetter }

public sealed class PatchDeclaration
{
    public required PatchKind Kind { get; init; }
    public required MethodDefinition PatchMethod { get; init; }
    public required string TargetTypeFullName { get; init; }

    /// <summary>
    /// Null when the target was named rather than typeof()'d. The weaver then
    /// has to search every game assembly for the type instead of being told
    /// which one holds it -- cheap, and it means the patch assembly needs no
    /// reference to sts2.dll.
    /// </summary>
    public string? TargetAssemblyName { get; init; }

    public required string TargetMethodName { get; init; }
    public string[]? ArgumentTypeNames { get; init; }
    public MemberKind Member { get; init; }
    public bool Optional { get; init; }

    public string Describe()
    {
        var args = ArgumentTypeNames is null ? "" : $"({string.Join(", ", ArgumentTypeNames)})";
        return $"{Kind} {TargetTypeFullName}.{TargetMethodName}{args} <- {PatchMethod.Name}";
    }
}

public sealed class PatchDiscovery
{
    private const string AttrNamespace = "STS2Switch";

    private readonly ModuleDefinition _patchModule;
    private readonly Action<string> _log;

    public PatchDiscovery(ModuleDefinition patchModule, Action<string> log)
    {
        _patchModule = patchModule;
        _log = log;
    }

    public List<PatchDeclaration> Discover()
    {
        var results = new List<PatchDeclaration>();

        foreach (var type in _patchModule.Types)
        {
            bool isPatchClass = type.CustomAttributes.Any(a =>
                a.AttributeType.Namespace == AttrNamespace &&
                a.AttributeType.Name == "SwitchPatchesAttribute");

            if (!isPatchClass) continue;

            var note = type.CustomAttributes
                .First(a => a.AttributeType.Name == "SwitchPatchesAttribute")
                .Properties.FirstOrDefault(p => p.Name == "Note").Argument.Value as string;

            _log($"[{type.Name}]{(note is null ? "" : $" -- {note}")}");

            foreach (var method in type.Methods)
            {
                foreach (var attr in method.CustomAttributes)
                {
                    if (attr.AttributeType.Namespace != AttrNamespace) continue;

                    PatchKind kind;
                    switch (attr.AttributeType.Name)
                    {
                        case "SwitchPrefixAttribute":  kind = PatchKind.Prefix;  break;
                        case "SwitchPostfixAttribute": kind = PatchKind.Postfix; break;
                        case "SwitchReplaceAttribute": kind = PatchKind.Replace; break;
                        default: continue;
                    }

                    if (!method.IsStatic)
                        throw new WeaveException(
                            $"{method.FullName} is a patch but is not static; patch methods must be static");

                    // Two constructor shapes: (Type, string, Type[]) and
                    // (string, string, Type[]). Cecil surfaces the first
                    // argument as a TypeReference or a string accordingly.
                    string targetTypeName;
                    string? targetAssembly;

                    if (attr.ConstructorArguments[0].Value is TypeReference tr)
                    {
                        targetTypeName = tr.FullName;
                        targetAssembly = ResolveAssemblyName(tr);
                    }
                    else
                    {
                        targetTypeName = (string)attr.ConstructorArguments[0].Value;
                        targetAssembly = null;   // search all assemblies
                    }

                    var methodName = (string)attr.ConstructorArguments[1].Value;

                    // `arr.Length > 0` IS REQUIRED HERE, and removing it broke
                    // eight working patches in one build.
                    //
                    // The constructor is `params Type[] argumentTypes`. A
                    // declaration that omits them does not pass null -- C#
                    // synthesises an EMPTY array, and Cecil reads that empty
                    // array straight out of the metadata. (The attribute class
                    // maps empty to null in its own constructor body, but that
                    // runs at runtime and this never instantiates the
                    // attribute.)
                    //
                    // So on THIS path an empty array cannot mean "takes no
                    // parameters" -- it is what every unspecified declaration
                    // looks like. Treating it as a specification made
                    // ModelDb.Contains, GodotFileIo.CreateDirectory,
                    // NHoverTipCardContainer.Add and five others demand a
                    // parameterless overload that does not exist.
                    string[]? argTypes = null;
                    if (attr.ConstructorArguments.Count > 2 &&
                        attr.ConstructorArguments[2].Value is CustomAttributeArgument[] arr &&
                        arr.Length > 0)
                    {
                        argTypes = arr.Select(a => ((TypeReference)a.Value).FullName).ToArray();
                    }

                    // ArgumentTypeNames wins over ArgumentTypes: it exists for
                    // the case where the typeof() form cannot be written at
                    // all, so a declaration that sets it means it.
                    var namesArg = attr.Properties
                        .FirstOrDefault(pr => pr.Name == "ArgumentTypeNames").Argument.Value;
                    // Empty IS meaningful here, unlike the params path above.
                    //
                    // ArgumentTypeNames is a named property. It has no default
                    // that produces an empty array -- writing
                    // `ArgumentTypeNames = new string[0]` is a deliberate act
                    // and can only mean "the overload that takes nothing".
                    // Absent, the property is not in attr.Properties at all and
                    // FirstOrDefault yields default(CustomAttributeNamedArgument),
                    // whose Argument.Value is null, so the type test below fails
                    // and argTypes is left alone.
                    //
                    // That asymmetry is the whole point: one path cannot express
                    // "no parameters" and the other can, so only the other drops
                    // the length check. GpuParticles2D.Restart() is reachable
                    // through this property and through nothing else.
                    if (namesArg is CustomAttributeArgument[] nameArr)
                        argTypes = nameArr.Select(a => (string)a.Value).ToArray();

                    bool optional = attr.Properties
                        .FirstOrDefault(p => p.Name == "Optional").Argument.Value is bool b && b;

                    var memberKind = MemberKind.Method;
                    if (attr.Properties.FirstOrDefault(p => p.Name == "Kind").Argument.Value is int k)
                        memberKind = (MemberKind)k;

                    results.Add(new PatchDeclaration
                    {
                        Kind = kind,
                        PatchMethod = method,
                        TargetTypeFullName = targetTypeName,
                        TargetAssemblyName = targetAssembly,
                        TargetMethodName = methodName,
                        ArgumentTypeNames = argTypes,
                        Member = memberKind,
                        Optional = optional,
                    });
                }
            }
        }

        return results;
    }

    private static string ResolveAssemblyName(TypeReference typeRef)
    {
        return typeRef.Scope switch
        {
            AssemblyNameReference anr => anr.Name,
            ModuleDefinition md       => md.Assembly.Name.Name,
            ModuleReference           => typeRef.Module.Assembly.Name.Name,
            _                         => typeRef.Module.Assembly.Name.Name,
        };
    }

    public MethodDefinition? ResolveTarget(ModuleDefinition module, PatchDeclaration decl)
    {
        var type = module.GetType(decl.TargetTypeFullName);
        if (type is null)
        {
            // Nested types use '/' in IL metadata but authors write '+' or '.'.
            var normalised = decl.TargetTypeFullName.Replace('+', '/');
            type = module.GetType(normalised);
        }
        if (type is null) return null;

        // Property accessors: PrefsSave.UploadData is patched through its
        // getter in the Android launcher, and a getter is not reachable by
        // plain method name (it is get_UploadData in metadata).
        if (decl.Member != MemberKind.Method)
        {
            var prop = type.Properties.FirstOrDefault(pr => pr.Name == decl.TargetMethodName);
            if (prop is null) return null;
            var accessor = decl.Member == MemberKind.PropertyGetter ? prop.GetMethod : prop.SetMethod;
            if (accessor is null)
                throw new WeaveException(
                    $"{decl.TargetTypeFullName}.{decl.TargetMethodName} has no " +
                    $"{(decl.Member == MemberKind.PropertyGetter ? "getter" : "setter")}");
            return accessor;
        }

        var byName = type.Methods.Where(m => m.Name == decl.TargetMethodName).ToList();
        if (byName.Count == 0) return null;

        if (decl.ArgumentTypeNames is null)
        {
            if (byName.Count > 1)
                throw new WeaveException(
                    $"{decl.TargetTypeFullName}.{decl.TargetMethodName} is ambiguous " +
                    $"({byName.Count} overloads). Specify argument types. Candidates:\n" +
                    string.Join("\n", byName.Select(m =>
                        $"      ({string.Join(", ", m.Parameters.Select(p => p.ParameterType.FullName))})")));
            return byName[0];
        }

        var match = byName.FirstOrDefault(m =>
            m.Parameters.Count == decl.ArgumentTypeNames.Length &&
            m.Parameters.Select(p => p.ParameterType.FullName)
                        .SequenceEqual(decl.ArgumentTypeNames));

        if (match is null)
            throw new WeaveException(
                $"{decl.TargetTypeFullName}.{decl.TargetMethodName} has no overload matching " +
                $"({string.Join(", ", decl.ArgumentTypeNames)}). Candidates:\n" +
                string.Join("\n", byName.Select(m =>
                    $"      ({string.Join(", ", m.Parameters.Select(p => p.ParameterType.FullName))})")));

        if (match.HasGenericParameters)
            throw new WeaveException(
                $"{match.FullName} is generic; the static weaver cannot patch open generic methods " +
                "(there is no single body to weave into). Patch a concrete caller instead.");

        return match;
    }
}
