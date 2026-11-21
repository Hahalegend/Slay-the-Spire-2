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
- **Blank screen during enemy attacks & scene-baked GPU particle fix (`patches/STS2Switch/ScreenVfxPatches.cs`):**
  - With `skip_compute 1` (required to prevent NVK GM20B GPU hangs on `ParticlesShaderRD`), full-screen `BackBufferCopy` overlays sampling `hint_screen_texture, filter_linear_mipmap` read black (`vec3(0.0)`) and blank the entire screen during enemy attacks (`NRadialBlurVfx` on `ShrinkerBeetle`, `BygoneEffigy`, `CeremonialBeast`, `MechaKnight`, `Vantom`) and `DoomPower` (`NDoomOverlayVfx`).
  - Prefixes `NRadialBlurVfx._Ready`/`Activate` and `NDoomOverlayVfx._Ready`/`PlayVfx` to disable them cleanly, and installs a lightweight `SceneTree.NodeAdded` guard that disables every `BackBufferCopy` (`CopyMode = Disabled`) and every scene-baked `GpuParticles2D` (`emitting=true` in `.tscn`, such as Regent ×6, Fogmog ×3, Vantom ×52, The Insatiable ×68, Waterfall Giant ×182) before its first frame renders.
- **Selective, thread-safe serialized preloading & warm asset retention (`patches/STS2Switch/PreloadPatches.cs`):**
  - Skips `'Common'` bulk background preloading (~792 assets on Main Menu that clogged `NAssetLoader`'s FIFO queue for 90–130s when starting a run).
  - Allowlists essential run/combat/room/character/shader/material/card-selection assets + ~39 universal combat VFX scenes.
  - Caps `AssetLoadingSession.ProcessLoadingQueue()` to **1 in-flight `LoadThreadedRequest`** (instead of stock's 128-wide burst across `WorkerThreadPool`), eliminating the multi-thread `FileAccessPack::open_internal` → `RefCounted::unreference()` crash (`01795253244`) against devkitPro's non-thread-safe file-handle table while eliminating microSD seek thrashing.
  - Keeps reusable lightweight assets (`images/powers/`, `images/packed/`, `images/ui/`, `scenes/ui/`, `scenes/cards/`, `scenes/combat/`, `materials/`, `shaders/`, loaded VFX) permanently warm across room transitions (`AssetCache.UnloadMissedCacheAssets` + filtered `AssetCache.UnloadAssets`), while still unloading heavy room-specific backgrounds, encounters, events, and old monster visuals to keep RAM/VRAM lean.
  - Skips `OneTimeInitialization.PrewarmJit()` (no JIT under NativeAOT), skips the boot intro logo (`LaunchMainMenu`), and caps `NTransition` `FadeOut`/`FadeIn` to `0.25s`.
- **Frame pacing & focus crash fix (`patches/STS2Switch/PerformancePatches.cs`, `ParticlePatches.cs`):**
  - Guards `Godot.Control.GrabFocus()` against detached/invalid/unfocusable controls (including `GetFocusModeWithOverride()` for `FocusBehaviorRecursive`), fixing the mid-combat `Viewport::_gui_remove_focus_for_window` crash (`01795245857`) and eliminating `"This control can't grab focus"` backtrace SD-card stutters.
  - Keeps Godot Vulkan `VSync: Disabled` + `MaxFps: 60` so Switch `NWindow` (`swap_interval = 1`) handles display sync once without double-blocking FIFO stalls (proven 10× fewer stalls on hardware: 0.19% vs 1.99%), cuts physics to `30 Hz`, and disables root viewport AA/debanding.
  - Hides and stops `_process` on suppressed `GpuParticles2D` nodes (`ParticlePatches.cs`) and smooths main-thread finalizer draining to 8 objects/frame (32/frame on large bursts > 512) in `FinalizerSafetyPatches.cs`.
  - Keeps compact balanced `-O` AOT codegen (`~112 MB`, `LOAD span: ~135 MB`) for optimal Tegra X1 L1/L2 cache fit.
- **Tuned Switch configs (`configs/config.txt` & `configs/override.cfg`):**
  - Disables debug allocator/logging overhead (`poison_free 0`, `lock_trace 0`, `vk_verbose 0`, `dispatch_log 0`, `mem_trend 0`, `run/flush_stdout_on_print=false`, `always_track_call_stacks=false`).
  - Keeps safety/coherency guards enabled (`free_guard 1`, `skip_compute 1`, `gpu_particles 0`, `nvk_cmd_mem_uncached 1`, `nvk_mem_stream_uncached 0`, `gc_region_range_mb 384`, `thread_stack_kb 2048`).
  - Sets `resolution_height 540`, `swapchain_image_count=3`, `frame_queue_size=2`, `pipeline_cache/save_chunk_size_mb=1` (eliminating 8 MB SD write spikes), and disables MSAA/HDR 2D/Swappy with uncompressed Vulkan shader cache enabled.

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
