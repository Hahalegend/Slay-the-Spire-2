// MethodWeaver.cs -- injects prefix/postfix/replace calls into a target method.
//
// Shape produced for a non-void method with both a prefix and a postfix:
//
//     <prefix args>
//     call     Prefix
//     brfalse  EPILOGUE          // only when the prefix returns bool
//     ... original body ...      // every `ret` rewritten to:
//                                //     stloc __result ; br EPILOGUE
//   EPILOGUE:
//     <postfix args>
//     call     Postfix
//     ldloc    __result
//     ret
//
// Two details that are easy to get wrong and are worth stating:
//
//   Instructions are MUTATED in place, never replaced. Cecil does not rewrite
//   branch operands when you swap an Instruction object out, so an existing
//   `br` pointing at a `ret` we replaced would keep pointing at an orphan. By
//   changing the opcode and operand of the existing object, every branch that
//   targeted the old `ret` now targets the `stloc` that took its place, which
//   is exactly where control should land.
//
//   A `ret` inside a protected region has to leave via `leave`, not `br`.
//   Roslyn does not emit that shape (it always branches to a `ret` outside the
//   handler), but hand-written or obfuscated IL can, and getting it wrong
//   produces an assembly that fails verification only at load.
//
// MIT license; see LICENSE.

using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

namespace StS2Patcher;

public sealed class WeaveException : Exception
{
    public WeaveException(string message) : base(message) { }
}

public sealed class MethodWeaver
{
    private readonly ModuleDefinition _targetModule;
    private readonly Action<string> _log;

    public MethodWeaver(ModuleDefinition targetModule, Action<string> log)
    {
        _targetModule = targetModule;
        _log = log;
    }

    public void Weave(MethodDefinition target, PatchSet patches)
    {
        if (!target.HasBody)
            throw new WeaveException($"{target.FullName} has no body (abstract, extern, or already stripped)");

        if (patches.Replace is not null)
        {
            WeaveReplace(target, patches.Replace);
            return;
        }

        var body = target.Body;
        body.SimplifyMacros();   // br.s -> br, ldloc.0 -> ldloc; avoids short-branch overflow

        var il = body.GetILProcessor();

        // __result local, when the target returns something.
        VariableDefinition? resultVar = null;
        bool isVoid = target.ReturnType.FullName == "System.Void";
        if (!isVoid)
        {
            resultVar = new VariableDefinition(target.ReturnType);
            body.Variables.Add(resultVar);
            body.InitLocals = true;
        }

        // --- epilogue -----------------------------------------------------
        // Built first so the ret-rewrite below has a branch target. The final
        // `ret` is a fresh instruction; the original rets all become branches.
        var epilogue = new List<Instruction>();
        foreach (var postfix in patches.Postfixes)
            epilogue.AddRange(BuildCall(target, postfix, resultVar));

        if (!isVoid)
            epilogue.Add(Instruction.Create(OpCodes.Ldloc, resultVar));
        epilogue.Add(Instruction.Create(OpCodes.Ret));

        var epilogueStart = epilogue[0];

        // --- rewrite every existing `ret` ---------------------------------
        // Snapshot first: we are about to insert into the same collection.
        var rets = body.Instructions.Where(i => i.OpCode == OpCodes.Ret).ToList();

        foreach (var ret in rets)
        {
            bool inProtectedRegion = IsInProtectedRegion(body, ret);
            var branchOp = inProtectedRegion ? OpCodes.Leave : OpCodes.Br;

            if (isVoid)
            {
                // Mutate in place so inbound branches still resolve.
                ret.OpCode = branchOp;
                ret.Operand = epilogueStart;
            }
            else
            {
                ret.OpCode = OpCodes.Stloc;
                ret.Operand = resultVar;
                il.InsertAfter(ret, Instruction.Create(branchOp, epilogueStart));
            }

            if (inProtectedRegion)
                _log($"    note: return inside a protected region, emitted `leave` ({target.Name})");
        }

        foreach (var instr in epilogue)
            body.Instructions.Add(instr);

        // --- prefixes -----------------------------------------------------
        // Prepended in reverse so declaration order is execution order.
        foreach (var prefix in Enumerable.Reverse(patches.Prefixes))
        {
            var call = BuildCall(target, prefix, resultVar).ToList();

            // Validate the prefix's own return type FIRST, as a standalone
            // check. An earlier version wove this together with the flow
            // emission using an if/else chain and then had a second condition
            // inserted into the middle of it, which orphaned the `else` onto
            // the wrong `if` -- so every bool-returning prefix was rejected
            // with "must return void or bool, not System.Boolean". Keeping the
            // validation separate makes that shape of mistake impossible.
            bool controlsFlow  = prefix.ReturnType.FullName == "System.Boolean";
            bool prefixIsVoid  = prefix.ReturnType.FullName == "System.Void";

            if (!controlsFlow && !prefixIsVoid)
                throw new WeaveException(
                    $"prefix {prefix.FullName} must return void or bool, not " +
                    $"{prefix.ReturnType.FullName}");

            if (controlsFlow)
            {
                call.Add(Instruction.Create(OpCodes.Brfalse, epilogueStart));

                // A prefix that can skip the original leaves __result at its
                // default. For a value type that is usually fine (0 / false).
                // For a REFERENCE type it is null, which the caller then
                // dereferences -- and for a Task it is awaited, which throws.
                //
                // Invisible at build time, and at runtime it looks like a
                // random startup crash. Refuse it rather than warn.
                if (!isVoid && IsReferenceType(target.ReturnType))
                {
                    bool anySetsResult = patches.Prefixes.Any(pf =>
                        pf.Parameters.Any(x => x.Name == "__result" && x.ParameterType.IsByReference));

                    if (!anySetsResult)
                        throw new WeaveException(
                            $"{prefix.FullName} can skip {target.FullName}, which returns the " +
                            $"reference type {target.ReturnType.FullName}. Skipping would return " +
                            $"null. Add a `ref {target.ReturnType.Name} __result` parameter and " +
                            $"assign it (Task.CompletedTask, Task.FromResult(...), etc).");
                }
            }

            var anchor = body.Instructions[0];
            foreach (var instr in call)
            {
                il.InsertBefore(anchor, instr);
            }
        }

        body.OptimizeMacros();
        _log($"    woven: {patches.Prefixes.Count} prefix, {patches.Postfixes.Count} postfix");
    }

    private void WeaveReplace(MethodDefinition target, MethodDefinition replacement)
    {
        if (replacement.ReturnType.FullName != target.ReturnType.FullName)
            throw new WeaveException(
                $"replace {replacement.FullName} returns {replacement.ReturnType.FullName} " +
                $"but {target.FullName} returns {target.ReturnType.FullName}");

        var body = target.Body;
        body.Instructions.Clear();
        body.Variables.Clear();
        body.ExceptionHandlers.Clear();

        var il = body.GetILProcessor();
        foreach (var instr in BuildCall(target, replacement, null))
            body.Instructions.Add(instr);

        body.Instructions.Add(Instruction.Create(OpCodes.Ret));
        body.OptimizeMacros();
        _log($"    replaced body with {replacement.FullName}");
    }

    /// <summary>
    /// Emits the argument loads for a patch method, matching its parameters to
    /// the target by name. Unmatched names are a build error.
    /// </summary>
    private IEnumerable<Instruction> BuildCall(MethodDefinition target,
                                               MethodDefinition patch,
                                               VariableDefinition? resultVar)
    {
        var instrs = new List<Instruction>();

        foreach (var p in patch.Parameters)
        {
            switch (p.Name)
            {
                case "__instance":
                    if (target.IsStatic)
                        throw new WeaveException(
                            $"{patch.FullName} wants __instance but {target.FullName} is static");
                    instrs.Add(Instruction.Create(OpCodes.Ldarg_0));
                    break;

                case "__result":
                    if (resultVar is null)
                        throw new WeaveException(
                            $"{patch.FullName} wants __result but {target.FullName} returns void");
                    instrs.Add(Instruction.Create(
                        p.ParameterType.IsByReference ? OpCodes.Ldloca : OpCodes.Ldloc, resultVar));
                    break;

                case "__state":
                case "__args":
                case "__originalMethod":
                    throw new WeaveException(
                        $"{patch.FullName} uses {p.Name}, which the static weaver does not support. " +
                        "See patches/README.md for the supported subset.");

                default:
                {
                    ParameterDefinition? targetParam;

                    // __arg0, __arg1, ... bind BY POSITION.
                    //
                    // Name binding needs the target's parameter names, which we
                    // do not have for methods we have never seen the source of.
                    // Guessing produces a hard weave failure, and declaring two
                    // spellings does not help: the one that guesses wrong fails
                    // rather than skipping, taking the build with it.
                    //
                    // Position is knowable from a stack trace alone, which is
                    // usually all there is.
                    if (p.Name is not null && p.Name.StartsWith("__arg") &&
                        int.TryParse(p.Name.AsSpan(5), out int argIndex))
                    {
                        if (argIndex < 0 || argIndex >= target.Parameters.Count)
                            throw new WeaveException(
                                $"{patch.FullName} wants {p.Name} but {target.FullName} has " +
                                $"{target.Parameters.Count} parameter(s)");
                        targetParam = target.Parameters[argIndex];

                        // The types still have to line up: a positional bind
                        // that silently loads the wrong type would corrupt the
                        // call rather than fail it.
                        var want = p.ParameterType.IsByReference
                            ? ((ByReferenceType)p.ParameterType).ElementType
                            : p.ParameterType;
                        if (want.FullName != "System.Object" &&
                            want.FullName != targetParam.ParameterType.FullName)
                            throw new WeaveException(
                                $"{patch.FullName} {p.Name} is {want.FullName} but " +
                                $"{target.FullName} parameter {argIndex} is " +
                                $"{targetParam.ParameterType.FullName}");
                    }
                    else
                    {
                        targetParam = target.Parameters.FirstOrDefault(tp => tp.Name == p.Name);
                    }

                    if (targetParam is null)
                        throw new WeaveException(
                            $"{patch.FullName} parameter '{p.Name}' does not name a parameter of " +
                            $"{target.FullName} (has: {string.Join(", ", target.Parameters.Select(x => x.Name))}). " +
                            $"Use __arg0/__arg1/... to bind by position instead.");

                    // Ldarg/Ldarga with a ParameterDefinition operand: Cecil
                    // derives the argument slot from the parameter's Sequence,
                    // which already accounts for `this` occupying slot 0 on
                    // instance methods. Computing the index by hand here is the
                    // classic off-by-one in this kind of tool.
                    var op = p.ParameterType.IsByReference ? OpCodes.Ldarga : OpCodes.Ldarg;
                    instrs.Add(Instruction.Create(op, targetParam));
                    break;
                }
            }
        }

        instrs.Add(Instruction.Create(OpCodes.Call, _targetModule.ImportReference(patch)));
        return instrs;
    }

    /// <summary>
    /// True for anything whose default is null. Deliberately treats an
    /// unresolvable type as a reference type: failing the build on a
    /// value type is a nuisance, returning a null Task is a crash.
    /// </summary>
    private static bool IsReferenceType(TypeReference t)
    {
        if (t.IsValueType) return false;
        if (t.IsPointer || t.IsByReference) return false;
        if (t.IsGenericParameter) return true;

        try
        {
            var def = t.Resolve();
            return def is null || !def.IsValueType;
        }
        catch
        {
            return true;
        }
    }

    private static bool IsInProtectedRegion(MethodBody body, Instruction instr)
    {
        if (!body.HasExceptionHandlers) return false;

        int Offset(Instruction i) => body.Instructions.IndexOf(i);
        int pos = Offset(instr);

        foreach (var h in body.ExceptionHandlers)
        {
            if (h.TryStart is not null && h.TryEnd is not null &&
                pos >= Offset(h.TryStart) && pos < Offset(h.TryEnd))
                return true;
            if (h.HandlerStart is not null && h.HandlerEnd is not null &&
                pos >= Offset(h.HandlerStart) && pos < Offset(h.HandlerEnd))
                return true;
        }
        return false;
    }
}

/// <summary>All patches resolved against a single target method.</summary>
public sealed class PatchSet
{
    public List<MethodDefinition> Prefixes { get; } = new();
    public List<MethodDefinition> Postfixes { get; } = new();
    public MethodDefinition? Replace { get; set; }

    public bool IsEmpty => Prefixes.Count == 0 && Postfixes.Count == 0 && Replace is null;
}
