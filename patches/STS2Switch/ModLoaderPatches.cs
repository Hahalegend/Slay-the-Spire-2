// ModLoaderPatches.cs -- Switch port of the Android launcher's
// Patches/ModLoaderPatches.cs.
//
// ModManager.Initialize scans a mods directory and loads assemblies out of it.
// That cannot work here for a reason that has nothing to do with the SD card
// being writable: NativeAOT has no assembly loader. There is no JIT to compile
// a mod's IL, and Assembly.LoadFrom throws PlatformNotSupportedException.
//
// So this is not a policy choice, it is a hard platform limit -- worth stating
// plainly because "mods do not work" reads like something that could be fixed
// with a config flag, and it cannot.
//
// MIT license; see LICENSE.

using System.Threading.Tasks;
using STS2Switch;

namespace STS2Switch.Patches;

[SwitchPatches(Note = "Mod loading: nothing patched, see the comment below")]
public static class ModLoaderPatches
{
    // CONFIRMED via --list-types:
    //   static Task Initialize(IModManagerFileIo, ModSettings, SemanticVersion)
    //
    // Static, three parameters, and returns Task. The Task is what matters: a
    // skipping prefix that does not set __result leaves it null, and
    // ModManager.Initialize is awaited -- so suppressing mod loading would
    // itself crash the game at startup.
    //
    // Task.CompletedTask, not Task.FromResult: the target's return type is the
    // non-generic Task and the weaver stores straight into a local of that
    // exact type.
    // ===========================================================================
    // DISABLED. Skipping Initialize broke ModelDb.
    // ===========================================================================
    //
    // Returning Task.CompletedTask without running Initialize meant ModManager
    // never marked itself finished, and the game guards on that:
    //
    //   InvalidOperationException: ModManager is not finished initializing!
    //   ReflectionHelper.ModTypes cannot be called before that.
    //     at ReflectionHelper.get_ModTypes()
    //     at ModelDb.get_AllAbstractModelSubtypes()
    //     at STS2Switch.Patches.ModelDbPatches.InitPrefix()
    //     at ModelDb.Init()
    //     at OneTimeInitialization.ExecuteEssential()
    //
    // Two of our own patches in conflict: ModelDbPatches enumerates the model
    // types through ReflectionHelper, which refuses to answer until ModManager
    // says it is done.
    //
    // The premise was also wrong. "NativeAOT has no assembly loader" is true,
    // but Initialize only reaches an assembly load if it actually FINDS a mod.
    // With no mods directory it enumerates nothing, completes, and sets the
    // flag the rest of the game waits on. Suppressing it removed the flag and
    // prevented nothing.
    //
    // Left in place rather than deleted, because "we tried skipping this and it
    // broke ModelDb" is the useful part -- and if a mods directory ever does
    // appear on the SD card, the assembly load underneath becomes real and this
    // is where the narrower fix belongs (let Initialize run, suppress only the
    // load).
    //
    // [SwitchPrefix("MegaCrit.Sts2.Core.Modding.ModManager", "Initialize", Optional = true)]
    // public static bool SkipModLoading(ref Task __result)
    // {
    //     __result = Task.CompletedTask;
    //     return false;
    // }
}
