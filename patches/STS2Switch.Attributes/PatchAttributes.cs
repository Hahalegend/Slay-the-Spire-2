// PatchAttributes.cs -- the static equivalent of Harmony's annotations.
//
// Harmony builds patched methods at runtime with Reflection.Emit. NativeAOT has
// no runtime IL generation, so the same declarations are consumed at BUILD time
// by tools/StS2Patcher, which weaves the calls directly into the shipped
// assembly with Mono.Cecil.
//
// This is deliberately a SUBSET of Harmony. What is supported:
//
//   [SwitchPrefix]  runs before the original body. Return void to always
//                   continue, or bool to control it -- false skips the
//                   original, exactly like Harmony.
//   [SwitchPostfix] runs after the original body, on every return path.
//   [SwitchReplace] replaces the body outright.
//
// Injected parameters, matched by name:
//
//   __instance   the target's `this` (instance methods only)
//   __result     the return value. `ref` to read or change it; a prefix that
//                returns false and sets __result is how you fully substitute
//                a return without [SwitchReplace].
//
// Any other parameter must match a target parameter by NAME and type. This is
// stricter than Harmony, which also matches positionally -- an explicit name
// mismatch is a build error rather than a wrong argument at runtime, which is
// the whole point of moving this to build time.
//
// NOT supported (and rejected by the weaver rather than silently ignored):
//   __state, __originalMethod, __args, transpilers, finalizers, reverse
//   patches, patching generic methods with open type parameters.
//
// MIT license; see LICENSE.

using System;

#nullable enable

namespace STS2Switch;

public abstract class SwitchPatchAttribute : Attribute
{
    /// <summary>
    /// Type declaring the method to patch. Null when the target was given by
    /// name instead -- see <see cref="TargetTypeName"/>.
    /// </summary>
    public Type? TargetType { get; }

    /// <summary>
    /// Fully-qualified target type name, e.g.
    /// "MegaCrit.Sts2.Core.Nodes.NGame".
    ///
    /// Preferred over <see cref="TargetType"/>. The Android launcher resolves
    /// every target this way (AccessTools.TypeByName) and it is better here for
    /// two concrete reasons:
    ///
    ///   - the patch assembly needs no compile-time reference to sts2.dll, so
    ///     it builds without the game present and the circular assembly
    ///     reference disappears;
    ///   - a type that moved between game versions fails at WEAVE time with the
    ///     name printed, rather than failing to compile with a less useful
    ///     error somewhere else.
    ///
    /// Nested types use '+', e.g. "Outer+Inner".
    /// </summary>
    public string? TargetTypeName { get; }

    /// <summary>Method name. Use ".ctor" for a constructor.</summary>
    public string MethodName { get; }

    /// <summary>
    /// Parameter types, for disambiguating overloads. Null means "the only
    /// method with this name" -- the weaver fails the build if that is
    /// ambiguous rather than picking one.
    /// </summary>
    public Type[]? ArgumentTypes { get; }

    /// <summary>
    /// When true, the build still succeeds if the target cannot be found.
    /// Use for patches that only apply to some game versions; the weaver logs
    /// a warning and skips. Default false, because a silently-dropped patch is
    /// exactly the failure mode this tool exists to prevent.
    /// </summary>
    public bool Optional { get; set; }

    /// <summary>
    /// Argument types as NAMES, for disambiguating overloads without needing a
    /// reference to the assembly that declares them.
    ///
    /// Added because SentryService.CaptureException has two overloads:
    ///
    ///     (System.Exception)
    ///     (System.Exception, System.Action`1&lt;Sentry.Scope&gt;)
    ///
    /// and picking the second with <see cref="ArgumentTypes"/> would require
    /// referencing Sentry.dll — exactly the compile-time dependency that
    /// name-based targeting exists to avoid.
    ///
    /// Use the FULL NAME as Cecil prints it, which is what the weaver's
    /// ambiguity error lists in its candidates. Generic arguments included:
    /// "System.Action`1&lt;Sentry.Scope&gt;".
    /// </summary>
    public string[]? ArgumentTypeNames { get; set; }

    /// <summary>
    /// What kind of member <see cref="MethodName"/> names. Property accessors
    /// exist because the Android launcher's PatchHelper.PatchGetter is used for
    /// PrefsSave.UploadData, and a getter is not reachable by plain method name.
    /// </summary>
    public MemberKind Kind { get; set; } = MemberKind.Method;

    protected SwitchPatchAttribute(Type targetType, string methodName, Type[]? argumentTypes = null)
    {
        TargetType = targetType;
        MethodName = methodName;
        ArgumentTypes = argumentTypes;
    }

    protected SwitchPatchAttribute(string targetTypeName, string methodName, Type[]? argumentTypes = null)
    {
        TargetTypeName = targetTypeName;
        MethodName = methodName;
        ArgumentTypes = argumentTypes;
    }
}

public enum MemberKind
{
    Method = 0,
    PropertyGetter,
    PropertySetter,
}

[AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
public sealed class SwitchPrefixAttribute : SwitchPatchAttribute
{
    public SwitchPrefixAttribute(Type targetType, string methodName, params Type[] argumentTypes)
        : base(targetType, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }

    public SwitchPrefixAttribute(string targetTypeName, string methodName, params Type[] argumentTypes)
        : base(targetTypeName, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }
}

[AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
public sealed class SwitchPostfixAttribute : SwitchPatchAttribute
{
    public SwitchPostfixAttribute(Type targetType, string methodName, params Type[] argumentTypes)
        : base(targetType, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }

    public SwitchPostfixAttribute(string targetTypeName, string methodName, params Type[] argumentTypes)
        : base(targetTypeName, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }
}

[AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
public sealed class SwitchReplaceAttribute : SwitchPatchAttribute
{
    public SwitchReplaceAttribute(Type targetType, string methodName, params Type[] argumentTypes)
        : base(targetType, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }

    public SwitchReplaceAttribute(string targetTypeName, string methodName, params Type[] argumentTypes)
        : base(targetTypeName, methodName, argumentTypes.Length == 0 ? null : argumentTypes) { }
}

/// <summary>
/// Marks a class as containing patches. The weaver only scans annotated
/// classes, so helper types can live alongside patches without being probed.
/// </summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class SwitchPatchesAttribute : Attribute
{
    /// <summary>Free-text note shown in the weave report.</summary>
    public string? Note { get; set; }
}
