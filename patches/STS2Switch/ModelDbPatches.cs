// ModelDbPatches.cs -- Switch port of the Android launcher's
// Patches/ModelDbInitPatch.cs.
//
// ===========================================================================
// This is the one patch that could not be transliterated.
// ===========================================================================
//
// ModelDb.Init() builds the game's entire content database -- cards, relics,
// monsters, everything -- by reflecting over ModelDb.AllAbstractModelSubtypes
// and constructing one instance of each. On Android it crashed on circular
// dependencies between model types, so the launcher replaced it with a
// two-phase version:
//
//   phase 1  RuntimeHelpers.GetUninitializedObject(type) for every type, and
//            register it, so cross-type references resolve during phase 2
//   phase 2  RunClassConstructor + ctor.Invoke on those pre-allocated objects
//
// The problem for this port is HOW phase 2 was scoped. The Android version
// creates a Harmony instance IN THE MIDDLE OF THE ALGORITHM, patches
// ModelDb.Contains to return false so constructors do not short-circuit, runs
// phase 2, then Unpatches:
//
//     var harmony = new Harmony("com.sts2mobile.modeldb");
//     harmony.Patch(containsMethod, new HarmonyMethod(containsPrefix));
//     ... phase 2 ...
//     harmony.Unpatch(containsMethod, containsPrefix);
//
// Runtime patching is exactly what NativeAOT removes, so that cannot survive.
//
// The translation is straightforward once you notice the dynamic patch was
// only ever a scoping device. ContainsPrefix already gates on a static flag:
//
//     if (_suppressContains) { __result = false; return false; }
//     return true;
//
// So the prefix is woven into ModelDb.Contains PERMANENTLY, and InitPrefix
// just sets and clears the flag. Identical behaviour, no runtime patching --
// and marginally cheaper, since it drops a patch/unpatch cycle around the
// hottest loop in startup.
//
// ===========================================================================
// AOT consequences -- read aot/rd.xml alongside this
// ===========================================================================
//
// This patch is why the Models namespace must be rooted. It needs, per type:
//
//   - the type to EXIST after trimming (it is only ever reached reflectively,
//     via AllAbstractModelSubtypes, so the trimmer sees no reference to it)
//   - metadata for GetUninitializedObject
//   - an invokable parameterless constructor for ctor.Invoke
//
// A trimmed-away model type does not throw. It silently drops out of
// AllAbstractModelSubtypes, and the game boots with a smaller content
// database -- fewer cards, missing relics -- which is a horrible thing to
// debug from a menu. The phase-1/phase-2 counters logged below are the
// early-warning signal: compare them against the desktop build.
//
// MIT license; see LICENSE.

using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.CompilerServices;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "ModelDb two-phase init; static translation of the Harmony original")]
public static class ModelDbPatches
{
    private static bool _suppressContains;
    private static MethodInfo? _cachedGetId;
    private static FieldInfo? _cachedIdField;

    private const BindingFlags All =
        BindingFlags.Public | BindingFlags.NonPublic |
        BindingFlags.Instance | BindingFlags.Static;

    /// <summary>
    /// Woven permanently, unlike the Android original which patched this only
    /// for the duration of phase 2. Gated on the flag, so outside phase 2 it
    /// returns true immediately and the original runs unchanged.
    /// </summary>
    [SwitchPrefix("MegaCrit.Sts2.Core.Models.ModelDb", "Contains")]
    public static bool ContainsPrefix(ref bool __result)
    {
        if (_suppressContains)
        {
            __result = false;
            return false;
        }
        return true;
    }

    /// <summary>
    /// v0.111.1 changed AbstractModel..ctor() from checking ModelDb.Contains(Type)
    /// to calling ModelDb.GetByIdOrNull&lt;AbstractModel&gt;(id) before assigning
    /// &lt;Id&gt;k__BackingField. During phase 2, skip the duplicate check while
    /// still setting &lt;Id&gt;k__BackingField and keeping GetByIdOrNull working
    /// for cross-model lookups inside model constructors.
    /// </summary>
    [SwitchPrefix("MegaCrit.Sts2.Core.Models.AbstractModel", ".ctor")]
    public static bool AbstractModelCtorPrefix(object __instance)
    {
        if (!_suppressContains) return true;
        if (_cachedGetId is not null && _cachedIdField is not null)
        {
            var id = _cachedGetId.Invoke(null, new object[] { __instance.GetType() });
            _cachedIdField.SetValue(__instance, id);
            return false;
        }
        return true;
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Models.ModelDb", "Init")]
    public static bool InitPrefix()
    {
        SwitchCulture.Ensure();

        // Escape hatch: run the ORIGINAL ModelDb.Init instead.
        //
        // This replacement exists because the Android launcher's own
        // ModelDbInitPatch does, and that patch exists because the original
        // crashed on circular dependencies between model types. Whether StS2
        // on Switch actually hits that is an assumption nobody has tested.
        //
        // Run 15 showed the content database is incomplete:
        //
        //   KeyNotFoundException: The given key 'ENCOUNTER.FLYCONID_NORMAL'
        //   was not present in the dictionary.
        //     at ModelDb.Get[T]()
        //     at Acts.Overgrowth.GenerateAllEncounters()
        //     at AssetSets..cctor()
        //
        // Either the two-phase construction below is missing types, or the
        // content genuinely is not there. Returning true here runs the stock
        // Init and answers that in one hardware run instead of a rebuild --
        // set `original_modeldb 1` in config.txt.
        if (Environment.GetEnvironmentVariable("STS2_ORIGINAL_MODELDB") == "1")
        {
            SwitchLog.Info("ModelDb: running the ORIGINAL Init (STS2_ORIGINAL_MODELDB=1)");
            return true;
        }

        SwitchLog.Info("ModelDb.Init: running two-phase replacement");

        // Resolved by name because this assembly does not reference the game.
        var modelDbType = Type.GetType("MegaCrit.Sts2.Core.Models.ModelDb, sts2", false)
                          ?? FindType("MegaCrit.Sts2.Core.Models.ModelDb");
        if (modelDbType is null)
        {
            SwitchLog.Error("ModelDb type not found at runtime -- falling through to the "
                          + "original Init(), which is expected to crash on circular refs");
            return true;
        }

        var allSubtypesProp = modelDbType.GetProperty("AllAbstractModelSubtypes", All);
        var types = (Type[])allSubtypesProp!.GetValue(null)!;

        var getIdMethod = modelDbType.GetMethod("GetId", All, null, new[] { typeof(Type) }, null);
        var contentById = modelDbType.GetField("_contentById", BindingFlags.NonPublic | BindingFlags.Static)!
                                     .GetValue(null)!;
        var setItem = contentById.GetType().GetMethod("set_Item")!;

        // -- phase 1 ----------------------------------------------------------
        var objects = new Dictionary<Type, object>(types.Length);
        int preRegistered = 0;

        // A failed class constructor is PERMANENTLY POISONED.
        //
        // Run 19 retried the ten failures and recovered none -- "round 2: 0
        // recovered, 10 still failing". That is not evidence the dependency is
        // absent; it is how .NET works. Once a cctor throws, the type is marked
        // failed and every later access rethrows the CACHED
        // TypeInitializationException. The cctor never runs again.
        //
        // So retrying cannot work, and the only fix is to not fail the first
        // time. The failures are all one shape:
        //
        //   FlyconidNormal   wants MONSTER.LEAF_SLIME_M
        //   BowlbugsNormal   wants MONSTER.BOWLBUG_EGG
        //   Darv             wants RELIC.ASTROLABE
        //   ... 10 in total, wanting 9 distinct MONSTER.* / RELIC.* keys
        //
        // Encounters reference monsters; events reference relics. Nothing in
        // the list runs the other way. Registering the referenced kinds first
        // should break every one of these.
        //
        // -- pass A: ids only ---------------------------------------------
        //
        // Split out so the log says WHICH call triggers the class constructor.
        // GetUninitializedObject is documented not to run the *instance*
        // constructor, but creating an instance still initialises the type; if
        // GetId alone is safe, the ids can all be resolved before anything is
        // allocated, and ordering becomes possible. If pass A fails for the
        // same ten, ordering cannot help and this says so plainly.
        var idOf = new Dictionary<Type, object>(types.Length);
        int idFailures = 0;

        foreach (var type in types)
        {
            try
            {
                idOf[type] = getIdMethod!.Invoke(null, new object[] { type })!;
            }
            catch (Exception ex)
            {
                idFailures++;
                if (idFailures <= 4)
                {
                    var inner = ex;
                    while (inner.InnerException is not null) inner = inner.InnerException;
                    SwitchLog.Error($"phase 1a: GetId({type.Name}): "
                                  + $"{inner.GetType().Name}: {inner.Message}");
                }
            }
        }

        SwitchLog.Info($"ModelDb phase 1a: {idOf.Count}/{types.Length} ids resolved"
                     + (idFailures > 0
                        ? $", {idFailures} failed -- GetId TRIGGERS cctors, ordering cannot help"
                        : " -- GetId is cctor-safe, ordering is viable"));

        // Pass A resolved all 1624 ids with zero failures, which proves by
        // elimination that GetUninitializedObject -- not GetId -- is what runs
        // the class constructor. Allocation order is therefore the whole game,
        // and an unknown edge costs exactly the type that discovers it,
        // because a thrown cctor is cached and can never be retried.

        // What kinds exist. Printed because the leaf set below was guessed from
        // one run's failures (MONSTER, RELIC) and the next run promptly found
        // CARD and POTION. Guessing twice is enough; this lists the taxonomy.
        var kinds = new Dictionary<string, int>();
        foreach (var kv in idOf)
        {
            var t = kv.Value?.ToString();
            var dot = t?.IndexOf('.') ?? -1;
            var k = dot > 0 ? t!.Substring(0, dot) : "(none)";
            kinds[k] = kinds.TryGetValue(k, out var c) ? c + 1 : 1;
        }
        var kindList = new List<string>();
        foreach (var kv in kinds) kindList.Add($"{kv.Key}={kv.Value}");
        kindList.Sort();
        SwitchLog.Info("ModelDb kinds: " + string.Join(" ", kindList));

        // Reverse map, so a missing key can be turned back into a type.
        var typeOfId = new Dictionary<string, Type>(idOf.Count);
        foreach (var kv in idOf)
        {
            var k = kv.Value?.ToString();
            if (k is not null) typeOfId[k] = kv.Key;
        }

        var registered = new HashSet<Type>();
        var lastError = new Dictionary<Type, Exception>();

        // First failure per type wins: the on-demand path below can touch a
        // type twice, and the ORIGINAL exception is the one that names the
        // real unmet dependency. A later "cached cctor" rethrow would say the
        // same thing less usefully.
        void lastErrorInit(Type t, Exception e)
        {
            if (!lastError.ContainsKey(t)) lastError[t] = e;
        }

        void Register(Type type)
        {
            if (!registered.Add(type)) return;
            var id = idOf[type];
            var model = RuntimeHelpers.GetUninitializedObject(type);
            setItem.Invoke(contentById, new[] { id, model });
            objects[type] = model;
            preRegistered++;
        }

        // Learned dependency order, persisted across boots.
        //
        // Hardcoding the ids hardware has happened to report covers exactly
        // what has already broken, which is the wrong shape of fix: bosses,
        // events, potions and every kind not yet exercised would each cost
        // their own hardware round to discover.
        //
        // Instead, every dependency discovered on demand is written to
        // user://modeldb_order.txt and registered FIRST on the next boot. An
        // edge is therefore paid for once, ever -- the first run that touches
        // that content loses one type, and no run after it does.
        //
        // It also survives a game update: new content produces new edges, they
        // are learned the same way, and no source change is needed. The seed
        // below is only a head start from what runs 19 and 20 already told us,
        // so the first boot on a fresh card is not needlessly worse.
        // Two sets in one file. Ids beginning '!' are types that have FAILED
        // before and should be registered LAST.
        //
        // Learning one dependency per boot is too slow when a type has
        // several. Run 22 recorded MONSTER.STABBOT for Fabricator; run 23 got
        // past it and failed on MONSTER.GUARDBOT. Fabricator is a summoner and
        // references a whole family of bots, so at one per boot it converges in
        // as many restarts as it has minions.
        //
        // Deferring a known-failing type to the very end of the pass means
        // every other model -- including all its minions -- is already
        // registered when its constructor finally runs. One boot instead of
        // several, and it generalises to any type with many dependencies.
        var learned = new List<string>(Seed);
        var deferred = new HashSet<string>();

        foreach (var id in ReadLearnedOrder())
        {
            if (id.StartsWith("!")) deferred.Add(id.Substring(1));
            else if (!learned.Contains(id)) learned.Add(id);
        }

        int learnedHits = 0;
        foreach (var id in learned)
        {
            if (!typeOfId.TryGetValue(id, out var st)) continue;
            try { Register(st); learnedHits++; }
            catch (Exception ex) { lastErrorInit(st, ex); }
        }
        SwitchLog.Info($"ModelDb: {learnedHits}/{learned.Count} learned dependencies "
                     + "registered up front");

        // Primitives, then the rest, then anything known to have failed.
        // Stable within each group.
        bool IsDeferred(Type t) =>
            idOf.TryGetValue(t, out var d) && d?.ToString() is string ds && deferred.Contains(ds);

        var ordered = new List<Type>(types.Length);
        foreach (var type in types)
            if (!IsDeferred(type) && idOf.TryGetValue(type, out var a) && IsLeafKind(a))
                ordered.Add(type);
        foreach (var type in types)
            if (!IsDeferred(type) && (!idOf.TryGetValue(type, out var b) || !IsLeafKind(b)))
                ordered.Add(type);
        foreach (var type in types)
            if (IsDeferred(type)) ordered.Add(type);

        if (deferred.Count > 0)
            SwitchLog.Info($"ModelDb: {deferred.Count} previously-failing type(s) "
                         + "deferred to the end of the pass");

        foreach (var type in ordered)
        {
            try
            {
                Register(type);
            }
            catch (Exception ex)
            {
                lastErrorInit(type, ex);

                // Register the thing it was missing, right now. The type that
                // discovered the edge is already poisoned and unrecoverable,
                // but everything else that needs the same dependency is saved
                // -- LEAF_SLIME_S was wanted by two types in run 19.
                var inner = ex;
                while (inner.InnerException is not null) inner = inner.InnerException;
                var key = ExtractMissingKey(inner.Message);
                if (key is not null && typeOfId.TryGetValue(key, out var dep)
                    && !registered.Contains(dep))
                {
                    try
                    {
                        Register(dep);
                        if (!learned.Contains(key)) learned.Add(key);
                        SwitchLog.Info($"  recovered dependency {key} on demand "
                                     + "(will be registered first next boot)");
                    }
                    catch { /* its own dependency is missing too; the next pass sees it */ }
                }
            }
        }

        foreach (var kv in lastError)
        {
            var inner = kv.Value;
            while (inner.InnerException is not null) inner = inner.InnerException;
            var ownId = idOf.TryGetValue(kv.Key, out var oid) ? oid?.ToString() : "?";
            SwitchLog.Error($"phase 1: {kv.Key.Name} ({ownId}): "
                          + $"{inner.GetType().Name}: {inner.Message}");

            // Its dependency goes into the learned set even when recovery
            // failed above -- knowing the edge is what matters, and next boot
            // registers it before anything can trip over it.
            var k = ExtractMissingKey(inner.Message);
            if (k is not null && !learned.Contains(k)) learned.Add(k);

            // And the type ITSELF is deferred next boot. This is the part that
            // converges a multi-dependency type in one restart rather than one
            // restart per dependency.
            if (ownId is not null) deferred.Add(ownId);
        }

        WriteLearnedOrder(learned, deferred);

        if (lastError.Count > 0)
        {
            // Worth stating plainly, because the remedy is unusual: these
            // models are missing FOR THIS SESSION ONLY. Their dependencies are
            // now in modeldb_order.txt and get registered first next boot, so
            // a restart fixes it without any rebuild.
            //
            // It matters because a missing model is not a visible error later
            // -- run 20 softlocked the start of a run on RELIC.CAULDRON with
            // no crash and working input.
            SwitchLog.Error($"{lastError.Count} model(s) missing this session. "
                          + "Their dependencies have been recorded; RESTART the game "
                          + "and they will be registered first.");
        }

        SwitchLog.Info($"ModelDb phase 1: {preRegistered}/{types.Length} pre-registered");

        // -- phase 2 ----------------------------------------------------------
        // The flag stands in for the Android version's temporary Harmony patch.
        _cachedGetId = getIdMethod;
        var abstractModelType = Type.GetType("MegaCrit.Sts2.Core.Models.AbstractModel, sts2", false)
                                ?? FindType("MegaCrit.Sts2.Core.Models.AbstractModel");
        _cachedIdField = abstractModelType?.GetField("<Id>k__BackingField", All);
        _suppressContains = true;

        int constructed = 0;
        int noCtor = 0;
        var failed = new List<Type>();

        try
        {
            foreach (var type in types)
            {
                if (!objects.TryGetValue(type, out var instance)) continue;

                try
                {
                    // ILC flags both of these (IL2059 on RunClassConstructor,
                    // IL2065 on GetConstructor): the Type comes from
                    // AllAbstractModelSubtypes at runtime, so the trimmer
                    // cannot prove the metadata survives.
                    //
                    // The suppressions are honest rather than cosmetic: the
                    // whole assembly is rooted (the container generates
                    // rd.generated.xml with sts2 as an assembly root, which is
                    // broader than any namespace list), so the metadata IS
                    // present. What the trimmer cannot do is prove it from the
                    // call site.
                    //
                    // Each call is individually guarded anyway, because
                    // "should be present" is not "is present": a missing
                    // constructor gives null rather than an exception, and a
                    // failed cctor must not take the whole loop down.
                    RunCctorSafe(type);

                    var ctor = GetParameterlessCtor(type);
                    if (ctor is null)
                    {
                        // Not fatal on its own -- some model types legitimately
                        // have no parameterless constructor -- but worth
                        // counting, because a SUDDEN rise here between builds
                        // means trimming, not content changes.
                        noCtor++;
                        continue;
                    }

                    ctor.Invoke(instance, null);
                    constructed++;
                }
                catch (Exception ex)
                {
                    failed.Add(type);
                    var inner = ex;
                    while (inner.InnerException is not null) inner = inner.InnerException;
                    SwitchLog.Error($"phase 2: {type.Name}: {inner.GetType().Name}: {inner.Message}");
                }
            }
        }
        finally
        {
            // try/finally, unlike the Android original's straight-line clear.
            // If phase 2 throws with the flag still set, ModelDb.Contains lies
            // for the rest of the process -- every later lookup says "not
            // registered" and the game rebuilds content it already has.
            _suppressContains = false;
        }

        if (failed.Count > 0)
        {
            SwitchLog.Error($"ModelDb: {failed.Count}/{types.Length} types failed to construct");
            foreach (var t in failed) SwitchLog.Error($"  {t.FullName}");
        }

        // The number that matters for AOT triage. If this is lower than the
        // desktop build's, types were trimmed away and the content database is
        // incomplete -- see aot/rd.xml.
        SwitchLog.Info($"ModelDb phase 2: {constructed}/{types.Length} constructed"
                     + (noCtor > 0 ? $", {noCtor} without a parameterless ctor" : ""));
        return false;
    }

    // The two reflection calls ILC warns about, isolated so the suppressions
    // sit on the smallest possible surface rather than over the whole loop.

    [UnconditionalSuppressMessage("Trimming", "IL2059",
        Justification = "sts2 is rooted as a whole assembly in rd.generated.xml, " +
                        "so cctor metadata is present; the trimmer cannot prove " +
                        "it because the Type comes from a runtime enumeration. " +
                        "Guarded: a throwing cctor is caught by the caller.")]
    private static void RunCctorSafe(Type type)
    {
        RuntimeHelpers.RunClassConstructor(type.TypeHandle);
    }

    [UnconditionalSuppressMessage("Trimming", "IL2065",
        Justification = "Same rooting argument as RunCctorSafe. Returns null " +
                        "rather than throwing when the constructor is absent, " +
                        "which the caller counts instead of treating as fatal.")]
    private static ConstructorInfo? GetParameterlessCtor(Type type)
    {
        return type.GetConstructor(
            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance,
            null, Type.EmptyTypes, null);
    }

    private static Type? FindType(string fullName)
    {
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            var t = asm.GetType(fullName, false);
            if (t is not null) return t;
        }
        return null;
    }

    // Monsters and relics are what the failing encounters and events reference.
    // Registering them first is a heuristic, but a well-founded one: every
    // dependency observed on hardware runs that direction and none run back.
    // Head start from runs 19 and 20, so a fresh card is not needlessly worse
    // on its first boot. Everything beyond this is learned at runtime.
    private static readonly string[] Seed =
    {
        "MONSTER.ZAPBOT", "MONSTER.STABBOT", "MONSTER.GUARDBOT", "MONSTER.NOISEBOT",
        "CARD.DISINTEGRATION", "POTION.FLEX_POTION",
        "MONSTER.LEAF_SLIME_S", "MONSTER.LEAF_SLIME_M", "MONSTER.BOWLBUG_EGG",
        "MONSTER.CALCIFIED_CULTIST", "MONSTER.AXE_RUBY_RAIDER",
        "RELIC.ASTROLABE", "RELIC.DAUGHTER_OF_THE_WIND", "RELIC.FAKE_ANCHOR",
    };

    // "user://modeldb_order.txt", opened through GODOT's FileAccess.
    //
    // The first version used Godot.OS.GetUserDataDir() with System.IO, and run
    // 22 showed why that cannot work here:
    //
    //   ModelDb: could not write learned order:
    //   Could not find file '/modeldb_order.txt'
    //
    // On this Android build GetUserDataDir() returns "/", so the path resolved
    // to the filesystem root. Godot's own user:// handling is what maps to the
    // save directory -- it goes through the JNI DirectoryAccessHandler, which
    // is the same path the game's saves use and is proven working.
    //
    // So the rule is: for anything under user://, use Godot's FileAccess, not
    // System.IO. System.IO only sees paths the wrapper's sandbox rebases, and
    // Godot's virtual paths are not among them.
    private const string LearnedOrderVfsPath = "user://modeldb_order.txt";

    private static List<string> ReadLearnedOrder()
    {
        var result = new List<string>();
#if HAVE_GODOTSHARP
        try
        {
            if (!Godot.FileAccess.FileExists(LearnedOrderVfsPath)) return result;

            using var f = Godot.FileAccess.Open(LearnedOrderVfsPath,
                                                Godot.FileAccess.ModeFlags.Read);
            if (f is null) return result;

            var text = f.GetAsText();
            foreach (var line in text.Split('\n'))
            {
                var t = line.Trim();
                // '#' starts a comment so the file explains itself to anyone
                // who opens it on the SD card.
                if (t.Length > 0 && !t.StartsWith("#")) result.Add(t);
            }
        }
        catch (Exception ex)
        {
            SwitchLog.Info("ModelDb: could not read learned order: " + ex.Message);
        }
#endif
        return result;
    }

    private static void WriteLearnedOrder(List<string> ids, HashSet<string> deferred)
    {
#if HAVE_GODOTSHARP
        try
        {

            var lines = new List<string>
            {
                "# ModelDb dependency order, learned at runtime by STS2Switch.",
                "#",
                "# Each id is a model whose class constructor another model needs",
                "# BEFORE that other model can be allocated. Allocating a type runs",
                "# its cctor, and a cctor that throws is poisoned permanently, so",
                "# the order has to be right the first time.",
                "#",
                "# Entries are appended as they are discovered. Deleting this file",
                "# is safe: it will be relearned, at the cost of a few models on the",
                "# next boot.",
            };
            lines.AddRange(ids);
            lines.Add("#");
            lines.Add("# '!' marks a type that FAILED and should be registered LAST,");
            lines.Add("# after everything it might reference already exists.");
            foreach (var d in deferred) lines.Add("!" + d);

            using var f = Godot.FileAccess.Open(LearnedOrderVfsPath,
                                                Godot.FileAccess.ModeFlags.Write);
            if (f is null)
            {
                SwitchLog.Info("ModelDb: could not open " + LearnedOrderVfsPath
                             + " for write: " + Godot.FileAccess.GetOpenError());
                return;
            }
            f.StoreString(string.Join("\n", lines) + "\n");
            SwitchLog.Info($"ModelDb: learned order saved ({ids.Count} ids)");
        }
        catch (Exception ex)
        {
            SwitchLog.Info("ModelDb: could not write learned order: " + ex.Message);
        }
#endif
    }

    // Primitive kinds: things other models reference, that reference nothing
    // themselves. MONSTER and RELIC came from run 19's failures; CARD and
    // POTION from run 20's. The "ModelDb kinds:" line now lists every kind
    // present, so this can be widened from evidence rather than guesswork.
    private static bool IsLeafKind(object id)
    {
        var s = id?.ToString();
        if (s is null) return false;
        // Widened from the taxonomy the game actually reports, rather than
        // guessed a third time. Run 21:
        //
        //   ACHIEVEMENT=9 ACT=5 AFFLICTION=10 BADGE=2 CARD=584 CARD_POOL=13
        //   CHARACTER=8 ENCHANTMENT=24 ENCOUNTER=96 EVENT=69 MODIFIER=17
        //   MONSTER=126 ORB=6 POTION=65 POTION_POOL=10 POWER=273 RELIC=297
        //   RELIC_POOL=9 SINGLETON=1
        //
        // Primitives are the things other models are built FROM. The
        // composites -- ACT, ENCOUNTER, EVENT, the *_POOLs, CHARACTER,
        // ACHIEVEMENT, BADGE, SINGLETON -- are what reference them, and every
        // dependency observed on hardware runs that direction.
        return s.StartsWith("CARD.")        || s.StartsWith("MONSTER.")
            || s.StartsWith("POTION.")      || s.StartsWith("RELIC.")
            || s.StartsWith("POWER.")       || s.StartsWith("AFFLICTION.")
            || s.StartsWith("ENCHANTMENT.") || s.StartsWith("ORB.")
            || s.StartsWith("MODIFIER.");
    }

    // "The given key 'MONSTER.ZAPBOT' was not present in the dictionary."
    //
    // Parsing an exception message is fragile in general -- it is culture
    // dependent and not a contract. It is acceptable here because the payload
    // runs with InvariantGlobalization, so the message is always the invariant
    // English one, and because a parse miss costs a missed recovery rather
    // than a wrong action.
    private static string? ExtractMissingKey(string? message)
    {
        if (message is null) return null;
        int a = message.IndexOf('\'');
        if (a < 0) return null;
        int b = message.IndexOf('\'', a + 1);
        return b > a ? message.Substring(a + 1, b - a - 1) : null;
    }

    // ---------------------------------------------------------------------
    // Ordering breadcrumbs.
    //
    // The failure is:
    //
    //   KeyNotFoundException: 'ENCOUNTER.FLYCONID_NORMAL'
    //     at ModelDb.Get[T]()
    //     at Acts.Overgrowth.GenerateAllEncounters()
    //     at AssetSets..cctor()
    //     at PreloadManager.<LoadLogoAnimation>d__7.MoveNext()
    //
    // Two possibilities, and they need completely different fixes:
    //
    //   ORDER    AssetSets' static constructor queries ModelDb BEFORE
    //            ModelDb.Init has populated it. A cctor runs on first touch of
    //            the type, so its timing is not controlled by startup order --
    //            and PreloadManager.LoadLogoAnimation is very early.
    //
    //   CONTENT  Init ran, and this specific entry is genuinely absent.
    //
    // A log line either side of Init, plus one on the cctor, distinguishes them
    // in a single run. Both prefixes return true, so nothing behaves
    // differently -- this only timestamps the sequence.
    // ---------------------------------------------------------------------

    [SwitchPostfix("MegaCrit.Sts2.Core.Models.ModelDb", "Init", Optional = true)]
    public static void InitFinished()
    {
        SwitchLog.Info("ModelDb.Init RETURNED");
    }

    [SwitchPrefix("MegaCrit.Sts2.Core.Assets.AssetSets", ".cctor", Optional = true)]
    public static bool AssetSetsCctorEntered()
    {
        SwitchLog.Info("AssetSets..cctor ENTERED "
                     + "(if this precedes 'ModelDb.Init RETURNED', the bug is ORDER, not content)");
        return true;
    }
}
