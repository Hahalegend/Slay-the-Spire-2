/* config.h -- Slay the Spire 2 Switch wrapper configuration.
 *
 * Forked from sts2_nx (MIT). Differences from that port are noted
 * inline; the structure is kept close so fixes can move between the two.
 *
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// libgodot_android.so is fopen()'d relative to the NRO's directory (the homebrew
// CWD), so keep it a bare filename: place the NRO next to it in /switch/sts2_nx/.
#define SO_NAME "libgodot_android.so"
#define CXX_SO_NAME "libc++_shared.so"

// NativeAOT game payload.
//
// UNLIKE sts2_nx: there DOTNET_SO_NAME came from the game's own
// project.godot assembly_name because we built it from source. Here the name is
// whatever ILC emits for the shipped assembly set (see aot/README.md); it is NOT
// read out of a project.godot we control. dlopen_fake matches by suffix
// (egl_shim.c) so the engine's computed directory prefix is ignored -- but the
// basename still has to line up.
//
// StS2's Android export is Godot 4.5, which DOES have the Android branch from
// godotengine/godot#97908 ("lib" + assembly_name + ".so") -- the opposite of the
// 4.3 situation the Dr. Mario port documented. Verify against your binary:
//   readelf -p .rodata libgodot_android.so | grep -i 'aot'
#define DOTNET_SO_NAME "libsts2.so"

// GDExtension native payloads, loaded through so_util alongside the engine.
// These are the Android arm64 builds; Switch is aarch64 so they load directly.
// Confirmed against StS2Launcher-v0.3.14's lib/arm64-v8a/ -- these are the
// real filenames, not guesses.
#define FMOD_SO_NAME  "libGodotFmod.android.template_release.arm64.so"
#define SPINE_SO_NAME "libspine_godot.android.template_release.arm64.so"

// libGodotFmod is a thin wrapper: its whole UND list is FMOD::Studio::* C++
// symbols resolved from these two. They must load FIRST and export to it.
// In the APK they live in assets/native/, not lib/ -- the launcher extracts
// them at runtime -- so they are easy to miss when assembling the SD card.
#define FMOD_CORE_SO_NAME   "libfmod.so"
#define FMOD_STUDIO_SO_NAME "libfmodstudio.so"

// The game's content pack, from the Steam depot. NOT from the launcher APK:
// that ships the engine and the shader overrides but no game content.
#define PCK_NAME "sts2.pck"

#define CONFIG_NAME "config.txt"
#define LOG_NAME "sts2_debug.log"

// LAST-RESORT FALLBACK ONLY. The app tree is DISCOVERED at runtime -- it may
// live in any directory under /switch/ (/switch/sts2_nx, /switch/slaythespire2,
// /switch/sls2nx, ...). app_paths.c resolves it from argv[0], then the cwd,
// then by scanning /switch/ for a directory containing libgodot_android.so.
//
// Use app_paths_data_root() / app_paths_save_root(), or config.data_root /
// config.save_root, which app_paths fills. Do not hardcode either.
#define DEFAULT_DATA_ROOT "/switch/sts2_nx"

// The save directory is always <data_root>/save, so it follows the app tree
// automatically and a renamed install keeps its saves.
#define SAVE_SUBDIR "save"

// Master debug switch. ON for bring-up -- flip to 0 only once the game is
// confirmed playable on hardware, the way sts2_nx did on 03/ago.
#define DEBUG_LOG 0
// Per-file-operation logging (open/stat/access/fopen). Very noisy and slow
// (one fflush per line during asset loading); requires DEBUG_LOG too.
#define VERBOSE_IO 0
// Audio backend tracing. Separate from VERBOSE_IO because FMOD bring-up needs
// it without the asset-load noise.
#define VERBOSE_AUDIO 1

extern int screen_width;
extern int screen_height;

// locale reported to the engine via GodotIO.getLocale
#define DEVICE_LOCALE "en_US"

// --- audio backend selection -------------------------------------------------
// CONFIRMED from the APK (docs/JNI_SURFACE.md): libfmod.so has no AAudio UND
// symbols but its string table is full of AAudioStreamBuilder_* names -- it
// dlopen()s libaaudio.so and dlsym()s each entry point. So the shim has to be
// reachable through dlsym_fake, which is how imports.c routes it.
//
// Separately, libgodot_android.so has libOpenSLES.so in NEEDED and no AAudio
// at all: the ENGINE's own audio driver is OpenSL ES regardless of this
// setting. Both can run at once; audio_sink.c mixes them.
//
//   AUDIO_BACKEND_AUTO    let FMOD probe; AAudio wins if aaudio_shim reports OK
//   AUDIO_BACKEND_OPENSL  refuse libaaudio.so in dlopen_fake, force audio.c
//   AUDIO_BACKEND_AAUDIO  force AAudio
//
// If you hit the failure mode pvzultimate documented -- a refused libaaudio
// becoming a hard DllNotFoundException out of managed code instead of a
// graceful fallback -- switch to AAUDIO, not OPENSL. FMOD is native C++ and
// does check the probe, so OPENSL should be safe here, but that is an
// assumption until it runs.
#define AUDIO_BACKEND_AUTO   0
#define AUDIO_BACKEND_OPENSL 1
#define AUDIO_BACKEND_AAUDIO 2

typedef struct {
  int screen_width;   // -1 = auto (1080p docked / 720p handheld)
  int screen_height;
  int boost;          // 0 = adaptive CPU boost (default); 1 = always boosted
  int audio_backend;  // AUDIO_BACKEND_*
  int enable_fmod;    // 0 = stub the FMOD GDExtension entirely (silent, boots)
  int enable_spine;   // 0 = stub the Spine GDExtension entirely (no characters)

  // 0 (default): let GDMono use its Android default assemblies path, which
  //   main() pre-creates so the existence check passes without changing branch.
  // 1: pass --game-assemblies-dir instead -- the launcher-specific branch of
  //   this custom engine build. See the comment in main() before changing.
  int use_cmdline_assemblies;

  // 1 = keep per-line log flushing on forever, not just through boot. Slower
  // asset loading, but nothing is ever lost to a Data Abort truncating the
  // buffer. Worth setting while chasing a crash that happens after boot.
  int eager_log;

  // 1 = let the engine load Vulkan (Mesa NVK, linked statically) and pass
  //   --rendering-driver vulkan. 0 = refuse libvulkan.so so the engine falls
  //   back to GLES3, which is the path that currently works end to end.
  //
  // Off by default deliberately: the GLES3 renderer boots, draws and plays,
  // and an unproven renderer should not become the default until it has run.
  int enable_vulkan;

  // Log every GetInstanceProcAddr miss and the full extension list. Noisy;
  // useful exactly once, when finding out what the engine asks for.
  int vk_verbose;

  // 1 = trace every pthread_mutex_init / cond_init / thread create during
  // managed startup. Off by default: with per-line flushing on until frame 8,
  // each line is a synchronous SD write, and this one call dominated 64% of a
  // boot log. Turn on only when chasing a startup lock problem.
  int trace_all;

  // 1 (default) = record every pthread lazy-slot transition into an in-memory
  // ring. The ring is only PRINTED on a rejected slot or a crash: logging each
  // transition live would cost an SD write per event and shift the timing of
  // the use-after-free being chased. Set 0 to disable recording entirely.
  // 1 (default) = validate a chunk header before handing the pointer to
  // newlib's free(). A corrupt header is logged and the block leaked rather
  // than faulting inside the allocator with the heap lock held, which takes
  // every other thread down with it.
  // 1 = let GpuParticles2D emit. Default 0: GPU particle dispatches hang the
  // NVK channel on this hardware (see docs/PARTICLES_DEVICE_LOSS.md), so the
  // AOT payload suppresses emission unless this says otherwise. The value
  // travels as the STS2_GPU_PARTICLES environment variable; the patch reads it
  // once at first use. CPU particles are unaffected either way.
  int gpu_particles;

  // 1 = drop every compute dispatch (vkCmdDispatch / vkCmdDispatchIndirect)
  // on the floor. The particle compute shader hangs the GPU as compiled by
  // NAK for GM20B, it is not data-dependent (zero_memory changed nothing),
  // and it dispatches regardless of Emitting -- so the C#-level gate cannot
  // reach it. This one can. Blunt: release SPIR-V carries no names, so "just
  // the particle pipeline" is not identifiable here. The bet is that a 2D
  // canvas game issues no other compute the player would miss; dispatch_log
  // is how that bet gets checked.
  // 1 = rewrite SSBO-sourced loop bounds to constant 0 at vkCreateShaderModule.
  // Godot's particle shader loops `i < FRAME.attractor_count/collider_count`
  // with an UNSIGNED compare on a value read from memory; a wrong count is a
  // 4-billion-iteration hang, not an error. The game references no attractor
  // or collider types, so both counts should already be 0 -- which makes this
  // a clean experiment: no change = the loops were innocent and it is codegen;
  // crash gone = the counts were garbage and the bug is in what fills that
  // buffer. See docs/SHADER_SPIRV_ANALYSIS.md.
  // 1 = CPU-side descriptor and pipeline checking. Tracks vkCreateBuffer sizes,
  // validates every buffer descriptor's offset+range against the real buffer,
  // and keeps a ring of the last 16 pipeline binds to name what was running
  // when the channel died. There is no Vulkan validation layer on this
  // platform, so nothing else would catch a descriptor pointing past the end
  // of its buffer -- which is what an MMU page fault in a LOOP-FREE canvas
  // shader looks like. See docs/SHADER_SPIRV_ANALYSIS.md.
  // 1 = scan sts2.pck's textures at boot and report the format census.
  // Eventually: re-encode them as ASTC into <save>/sts2_astc.pck, which
  // AstcOverlay.cs already knows how to mount. Today it only counts, because
  // the encoder is not vendored yet -- see docs/ASTC_BAKE.md.
  // Costs one full read of the pck on boot, so it is off by default.
  int astc_bake;

  // ASTC block size: 8 (0.25 B/px, ~444 MB), 6 (0.44, better on gradients),
  // 4 (1.0, matches BC7's size). A config knob rather than a constant because
  // a card that looks banded should be a re-bake, not a rebuild.
  int astc_block;

  // 1 = show the on-screen progress console during the bake. 0 = log only.
  //
  // consoleInit/consoleExit is the newest thing in the boot path and the bake
  // has now crashed twice immediately after consoleExit returns. Turning it
  // off separates "the bake is broken" from "the console is broken" without a
  // rebuild -- the same reason nvk_debug is a string and not a #define.
  int astc_console;

  int vk_validate;

  int patch_loop_bounds;

  int skip_compute;

  // 1 = log the first 32 compute dispatches and every 512th after, with group
  // counts. Cheap; answers "how much compute does this game actually issue".
  int dispatch_log;

  int free_guard;

  // 1 = memset every freed block to 0xDE. Freed memory then reads back the
  // same value every boot instead of whatever the randomised layout leaves, so
  // a stale read is nameable on sight -- and if the garbage is still random,
  // reuse is ruled out. Costs a memset per free. OFF by default: this build
  // reaches gameplay, and anything relying on freed contents would change
  // behaviour.
  int poison_free;

  // MB of freed memory to hold unreusable and verify on release. 0 = off.
  // Detects AND mitigates: the stale writer hits quarantined memory rather
  // than a live allocation. Retention starves Mesa, whose buffers come from
  // this same heap -- fruitninja_nx disabled theirs for exactly that. Try 16.
  int quarantine_mb;

  int lock_trace;

  // How many recent slot transitions the crash handler prints. 0 = none.
  int lock_trace_dump;

  // Godot rendering method under Vulkan: "mobile" (default) or "forward_plus".
  // forward_plus compiles the entire clustered 3D pipeline at startup -- SDFGI,
  // VoxelGI, SSAO/SSIL, SSR, volumetric fog, FSR2, SMAA, TAA -- none of which a
  // 2D card game draws, and all of which cost minutes of shader compilation
  // before the first frame.
  char rendering_method[32];

  // Passed to NVK as NVK_DEBUG. Comma-separated; the useful one for a hung
  // GPU channel is "push_sync", which makes every submit synchronous so the
  // hang localises to one submit. Also: push_dump, zero_memory, vm, no_cbuf.
  // Empty by default -- push_sync alone makes the game unplayable.
  char nvk_debug[64];

  // NVK_SWITCH_* cache-coherency switches. See main() -- the Switch GPFIFO
  // prefetches, and a command list fetched before an earlier cache flush
  // lands makes the GPU run against stale memory, which hangs rather than
  // faults. Candidates for the FIFO_ERROR_IDLE_TIMEOUT crash.
  //
  // Try cmd_mem_uncached first: command memory is the one the GPFIFO
  // prefetches from.
  int nvk_cmd_mem_uncached;
  int nvk_mem_stream_uncached;
  int nvk_mapped_completion;

  // memcpy/memmove destination reporting. SEPARATE from heap_canary because
  // --wrap,memcpy intercepts newlib's own copies inside the allocator, which
  // is a much bigger blast radius than canarying allocations. Needs
  // heap_canary too for the overrun check to mean anything.
  int copy_guard;

  // 1 = run the stock ModelDb.Init instead of STS2Switch's two-phase
  // replacement. A/B test for the incomplete content database seen in run 15.
  int original_modeldb;

  // Minimum stack for engine threads, in KB. 1024 matches bionic's default,
  // which is what the engine was compiled expecting; newlib's is far smaller
  // and run 21 overflowed it during scene resource loading.
  //
  // 0 = do not set a stack size at all (newlib's default), which restores the
  // pre-fix behaviour for an A/B test.
  int thread_stack_kb;

  // DOTNET_GCRegionRange, in MB. The .NET GC reserves this much address space
  // up front -- and because mmap_fake is malloc-backed (imports.c), a
  // reservation is a REAL allocation. 1024 cost a physical gigabyte and showed
  // up as GL_OUT_OF_MEMORY during texture upload.
  //
  // 384, so the region reserve is at least the heap hard limit.
  //
  // DOTNET_ values are HEXADECIMAL, so DOTNET_GCHeapHardLimit "16000000" is
  // 0x16000000 = 352 MB, not 15.3 MB. At 256 the GC was reserving less region
  // space than the limit it had been told it could commit into -- it could
  // never reach the limit, and region bookkeeping is sized from the range.
  //
  // CORRECTION: an earlier note here concluded the limit was decimal, on the
  // grounds that only 14 MB was ever committed. That was unsound -- 14 MB is
  // simply this game's working set, comfortably under a 352 MB cap, and says
  // nothing about where the cap is. Raising the range is the safe direction:
  // it makes the pair consistent without touching a limit that is
  // hardware-proven and that previously aborted the runtime when changed.
  //
  // 256, from the automatic accounting rather than log arithmetic:
  //
  //   [mem] largest reservation 384 MB, committed 114 MB (distinct pages)
  //
  // 114 MB is the GC's real working set. The 178.9 MB that misled me before
  // was every RW mprotect in the process, most of it nothing to do with the
  // GC. 256 leaves 142 MB of headroom over the measurement.
  //
  // History worth keeping: 128 starved it (run 25, managed heap corruption
  // before the menu), 384 was comfortable but wasteful.
  //
  // The 128 came from a measurement error of mine. I totalled the GC's
  // MAP_FIXED commits (14 MB) and concluded the 384 MB reservation was 25x
  // oversized -- while ignoring the mprotect(PROT_READ|WRITE) commits in the
  // same output, which come to 178.9 MB of DISTINCT pages. Both numbers were
  // printed side by side; I used the smaller one.
  //
  // 128 MB is below what the GC actually commits, so it was being starved.
  //
  // The 14 MB figure did establish one thing that still holds:
  // DOTNET_GCHeapHardLimit "16000000" is read as DECIMAL (15.3 MB), not hex.
  // That is a separate cap from this reservation.
  int gc_region_range_mb;

  // Size of the dedicated GPU-buffer arena, in MB. 0 disables it and sends
  // libdrm_nouveau's allocations back to newlib's general heap.
  //
  // 1024, raised from 768 on measurement. Run 24 ended at:
  //
  //   [gpua] live 757 MB, peak 763 MB of 768 MB;
  //          1587 allocs, 984 frees, 639 fell back to newlib
  //
  // Full, with 639 GPU allocations pushed back into newlib -- which is exactly
  // the fragmentation the arena exists to prevent, so the arena was doing
  // nothing for the last stretch of the session.
  //
  // 0 -- OFF BY DEFAULT. The pool is a net loss and the evidence is unambiguous.
  //
  // Run 31 swept four sizes on hardware:
  //
  //   gpu_arena_mb 0     boots, plays, reaches battle    55781 log lines
  //   gpu_arena_mb 768   dies before the menu             5134
  //   gpu_arena_mb 1000  dies before the menu             5058
  //   gpu_arena_mb 1100  dies before the menu             5058
  //
  // Any pool kills boot, and the size barely moves the line count, so it is
  // the pool existing rather than the pool being too small or too large.
  //
  // It was also never needed. It was built to stop fragmentation-driven
  // GL_OUT_OF_MEMORY during texture upload -- and with the pool off there are
  // ZERO GL_OUT_OF_MEMORY, with 438 MB free at the crash. What actually fixed
  // the exhaustion was DOTNET_GCRegionRange: 1024 MB of malloc-backed
  // "reservation" cut to 256, of which only 3 MB is committed. The pool was
  // built on top of a problem the GC fix had already solved, and I did not
  // re-check the premise afterwards.
  //
  // Left in rather than deleted: --wrap still routes through it, it passes
  // straight through at 0, and it is the only tool that can attribute GPU
  // allocations if texture memory ever becomes the constraint again.
  int gpu_arena_mb;

  // 1 = add canaries, to catch buffer overruns at the free() of the block that
  // was overrun rather than in a later, unrelated malloc.
  //
  // COVERAGE IS PARTIAL, and worth knowing before spending a run on it: only
  // memalign goes through a wrapper, so only memalign blocks are canaried.
  // Plain malloc is not wrapped, so a small malloc'd block that gets overrun
  // is invisible to this.
  //
  // malloc is deliberately not wrapped. --wrap intercepts the PUBLIC free, but
  // newlib's internals free through _free_r -- stdio's fclose releasing a
  // buffer that __smakebuf malloc'd, for instance. A canaried malloc block
  // freed that way would hand newlib a pointer 32 bytes past the real
  // allocation, which is the corruption this is meant to find rather than
  // cause.
  //
  // So: useful if the overrun is in a GPU buffer or another large aligned
  // allocation, which is where the churn is. Read the "[canary] N blocks
  // checked" line -- if N is small next to the allocation volume, the coverage
  // is not meaningful and the answer is elsewhere.
  //
  // Costs 32+ bytes per allocation. A debugging tool, not a shipping mode.
  int heap_canary;

  // 1 = suppress the hover-tip container. Diagnostic only: it answers whether
  // the repeating crash lives in that subsystem, at the cost of losing tooltips.
  int no_hover_tips;

  // 1 = the periodic [mem] line, which calls mallinfo(). mallinfo WALKS THE
  // HEAP, so on a corrupted heap it faults -- run 46 died inside
  // __malloc_update_mallinfo. That makes it a useful integrity canary and a
  // crash trigger at the same time. 0 removes both.
  int mem_trend;

  // 1 = claim the GPU pool before module loading; 0 = lazily, on the first
  // large aligned allocation, which is what the wrapper did up to run 30.
  //
  // Only matters when gpu_arena_mb is non-zero. Exists because early claiming
  // is plausible reasoning that hardware has not confirmed. See main.c.
  int gpu_arena_early;

  // Seconds of no completed step before the watchdog PAUSES the game thread
  // and dumps its stack. 0 disables the pause-and-dump entirely.
  //
  // DEFAULT 0. The dump deadlocked run 34, and while the cause is fixed
  // (capture while paused, log after resuming), the fix is unproven on
  // hardware. Anything that suspends the game thread is a poor thing to have
  // running while trying to establish whether the game itself is at fault --
  // a tool that can kill the run makes every result ambiguous.
  //
  // The 5-second warning still prints, flushed, regardless of this setting.
  // That is the part with no risk: it observes and never suspends anything, so
  // a stall is still visible in the log even with the dump off.
  //
  // Set to 30 to re-enable once the game is reaching the fight reliably and a
  // stack is worth the risk.
  int watchdog_stall_s;

  // Steps that must complete before watchdog_stall_s can pause-and-dump.
  // 0 = use the built-in WATCHDOG_ARM_STEPS (120), which is deliberately high
  // so a dump cannot fire during normal loading -- see main.c.
  //
  // Set to 1 ONLY to diagnose a startup hang, together with watchdog_stall_s.
  // Pausing threads mid-load has deadlocked the game before; that is an
  // acceptable trade when it is already not progressing, and a bad one
  // otherwise.
  int watchdog_arm_steps;



  // 1 = TOUCHSCREEN input, delivered as Godot pointer events. 0 = off.
  //
  // There is no virtual cursor any more, under any condition. The game has
  // full controller support, so a stick-driven pointer was never needed to
  // play it -- and it cost four buttons the game uses ('+', '-', L, R) to
  // summon something a player did not ask for. Docked play is controller play.
  //
  // The name is kept because it is what config.txt has always called this
  // setting, and renaming it would silently ignore every existing config.
  // What it means now is narrower than it was: touch, and nothing else.
  //
  // The gamepad path is independent: turning this off removes touch input,
  // not any controller input.
  int virtual_cursor;

  // Vertical render resolution, 360..1080. 0 = follow the display (720
  // handheld, 1080 docked), which is the default and what every build before
  // this one did.
  //
  // The horizontal is COMPUTED as height * 16 / 9, not configurable. The panel
  // is 16:9 in both modes, and letting the two be set independently only
  // offers ways to get a stretched image.
  //
  // Lower than the panel is the point: the Switch upscales the result for
  // free, and a 2D game at 540p costs roughly half the fill rate of 720p.
  // Higher than the panel renders detail the display then throws away.
  //
  // Rounded to an even width -- 16:9 does not divide evenly for every height
  // (500 -> 888.9), and an odd width is a bad idea for anything that later
  // wants 2x2 blocks.
  // 1 = allow Vulkan (Mesa NVK) on Switch emulators. Default 0: emulators
  // (yuzu/ruzu/eden) lack NAK's IMUL (cbuf) instruction and NVK custom MME
  // support, so emulators default to Mesa Gallium Nouveau GLES3 while real
  // Switch hardware continues using Vulkan (enable_vulkan=1) untouched.
  int emu_vulkan;

  int resolution_height;
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int read_config_overlay(const char *file);
int write_config(const char *file);

#endif
