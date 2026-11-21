# Slay the Spire 2 (v0.111.1) — Switch Compatibility & Optimization Fork Notes

This fork builds on [ChanseyIsTheBest/sts2_nx](https://github.com/ChanseyIsTheBest/sts2_nx) with fixes for Steam **v0.111.1** (and StS2Launcher Android APK **v0.3.17**), native Windows NativeAOT builds without Docker/WSL, major boot/loading/framerate optimizations, and crash fixes.

> **Legal note:** No copyrighted game assets, Steam DLLs, APKs, `sts2.pck`, `sts2_astc.pck`, `libsts2.so`, or Switch keys are included in this repository. Build from your own legally owned Steam copy of *Slay the Spire 2*.

---

## Summary of Improvements

### 1. Steam `v0.111.1` Compatibility (`patches/STS2Switch/`)
- **Sentry module-initializer crash fix (`PlatformPatches.cs`):** Steam `v0.111.1` introduced `[ModuleInitializer]` hooks in `Sentry.Godot.Internal.SentryGodotInitializer.AutoInit` / `SentryAutoInit.Init` that call `Console.WriteLine` during startup and fail-fast on Switch (`ConsolePal.EnsureInitializedCore`). Stubbed alongside `SentryBootstrap._EnterTree` and newer `SentryService` methods (`AfterGameInit`, `SetPlatformBranch`, `SetTag`, `SetExtra`, `OnLogCallback`, `DisableSentryIfModded`).
- **`ModelDb` two-phase init fix (`ModelDbPatches.cs`):** Steam `v0.111.1` switched `AbstractModel..ctor()`'s duplicate check from `ModelDb.Contains(Type)` to `ModelDb.GetByIdOrNull<AbstractModel>(id) != null`, causing `0/1660` models to construct. Added a guarded `[SwitchPrefix]` on `AbstractModel..ctor` and pre-seeded Fabricator's minions (`MONSTER.STABBOT`, `MONSTER.GUARDBOT`, `MONSTER.NOISEBOT`). Verified **1660/1660** models construct cleanly on Switch hardware.
- **Quit & Sleep/HOME safety (`LifecyclePatches.cs`):** Guarded `sts2_request_quit` and `sts2_set_game_background` P/Invokes against `EntryPointNotFoundException` when paired with prebuilt `sts2.nro` v1.0.0.
- **Mid-gameplay crash fix — Main-thread Godot finalizer pump (`FinalizerSafetyPatches.cs`):** Marshals `Finalize()` on `GodotObject`, `Collections.Array`, `Collections.Dictionary`, `StringName`, and `NodePath` from the `.NET` GC Finalizer thread onto the Godot main thread (drained per-frame in `ScriptManagerBridge.FrameCallback()`). Eliminates the native data race where background finalization called `godotsharp_internal_refcounted_disposed` → `CSharpLanguage::_instance_binding_reference_callback` → `RBMap::_erase` concurrently with main-thread room/scene creation and `CanvasItem::_redraw_callback`.

### 2. Performance, Loading & Frame-Pacing Optimizations
- **GPU 76.8 MHz throttle fix (`source/util.c` + `scripts/patch-nro-gpu-clock.py`):**
  - `ApmCpuBoostMode_FastLoad` boosts CPU to 1785 MHz while hard-throttling the Tegra X1 GPU to **76.8 MHz**. Under Godot Vulkan, any frame `> 50 ms` re-armed boost and trapped the game at ~10 FPS (and even slowed initial boot).
  - `source/util.c` now gates `cpu_boost()` behind `config.boost` (off by default).
  - For users running prebuilt `sts2.nro` v1.0.0 without devkitPro, run:
    ```bash
    python scripts/patch-nro-gpu-clock.py path/to/sts2.nro
    ```
- **Selective preloading & parallel core combat VFX (`patches/STS2Switch/PreloadPatches.cs`):**
  - Skips `'Common'` bulk background preloading (~792 assets on Main Menu that clogged `NAssetLoader`'s FIFO queue for 90–130s when starting a run).
  - Allowlists essential run/combat/room/character/shader/material assets + the ~21 universal combat VFX scenes (`vfx_attack_slash`, `vfx_damage_num`, `vfx_block`, `vfx_block_broken`, `vfx_card_fly`, `vfx_card_shuffle_fly`, etc.) and loads them **in parallel** (`AssetLoadingSession.IsVfxScene` → `false`) instead of serially one-by-one.
  - Preserves on-demand `CommonAssets` / `RunSet` assets across room transitions (`AssetCache.UnloadMissedCacheAssets`) while letting `LoadAssetSets` continue unloading old-room monsters and FTUEs cleanly.
  - Skips `OneTimeInitialization.PrewarmJit()` (no JIT under NativeAOT), skips the boot intro logo (`LaunchMainMenu`), and caps `NTransition` `FadeOut`/`FadeIn` to `0.25s`.
- **Consistent frame pacing & low-latency GC (`patches/STS2Switch/PerformancePatches.cs`, `ParticlePatches.cs`, `aot/sts2.aot.props`):**
  - Enforces `VSync: Enabled`, locked `30 FPS` pacing (`33.3 ms` frame budget, preventing `NGame.InitializeGraphicsPreferences()` from overriding `override.cfg` back to `VSync: Off` / `60 FPS`), `30 Hz` physics ticks, and root viewport AA/debanding off. (Users running `sys-clk` can still raise FPS Limit live in Settings.)
  - Sets `GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency` to suppress blocking Gen 2 GC pauses during combat.
  - Hides and stops `_process` on suppressed `GpuParticles2D` nodes (`ParticlePatches.cs`) and smooths main-thread finalizer draining to 16 objects/frame (`FinalizerSafetyPatches.cs`).
  - Enables `.NET 9` `<OptimizationPreference>Speed</OptimizationPreference>` (`-Ot`) for faster AOT codegen while keeping `LOAD span` (`143.0 MB`) safely under the `160 MB` `DOTNET_SO_SLICE`.
- **Tuned Switch configs (`configs/config.txt` & `configs/override.cfg`):**
  - Disables debug allocator/logging overhead (`poison_free 0`, `lock_trace 0`, `vk_verbose 0`, `dispatch_log 0`).
  - Keeps safety/coherency guards enabled (`free_guard 1`, `skip_compute 1`, `gpu_particles 0`, `nvk_cmd_mem_uncached 1`, `nvk_mem_stream_uncached 0`, `gc_region_range_mb 384`).
  - Sets `resolution_height 540` and disables MSAA/HDR 2D/Swappy with uncompressed Vulkan shader cache enabled.

### 3. Native Windows Build (No Docker or WSL Required)
- **`scripts/build-windows-native.ps1`** replicates `scripts/docker-entry.sh` directly on Windows using portable:
  - `.NET 9.0.317 SDK` placed in `toolchains/dotnet-9.0.317/`
  - `Android NDK r27c` placed in `toolchains/android-ndk-r27c/`
  - Python 3.10+ (`lz4`, `texture2ddecoder`, `astc-encoder-py`, `pillow` for ASTC pre-baking)
- Verifies `*.ilc.rsp` has zero `Vortice`/`SharpGen` desktop DirectX leaks, saves `dist/libsts2.so.dbg` off-SD for symbolization, and runs `tools/check_aot_so.py` automatically.

```powershell
pwsh -ExecutionPolicy Bypass -File scripts\build-windows-native.ps1 `
  -ApkPath "path\to\StS2Launcher-v0.3.17.apk" `
  -SteamDir "C:\Program Files (x86)\Steam\steamapps\common\Slay the Spire 2" `
  -OutDir "dist\sts2_nx"
```

Then copy `configs\config.txt` and `configs\override.cfg` into `dist\sts2_nx\`, copy `sts2.pck` from Steam, pre-bake ASTC 8×8 textures on PC (`python tools\bake_astc.py sts2.pck dist\sts2_nx\save\sts2_astc.pck`), and copy `dist\sts2_nx\` to `sdmc:/switch/sts2_nx/`.
