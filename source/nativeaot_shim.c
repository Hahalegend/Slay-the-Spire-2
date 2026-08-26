/* nativeaot_shim.c -- see nativeaot_shim.h.
 *
 * These implementations follow the same conservative "what does StS2.so
 * actually import" approach as the rest of the port: the exact symbol list
 * was captured empirically (nm -D --undefined-only against a real, built
 * StS2.so -- reference/nativeaot-probe/undefined_symbols.txt), not copied
 * wholesale from reference/nativeaot-celeste64/ (a different game, a
 * different RID -- linux-arm64/glibc-shaped vs our linux-bionic-arm64 --
 * so its symbol *list* doesn't transfer, only the *technique* for the ones
 * that do overlap). Untested on hardware; see PORT_NOTES.md roadmap.
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "nativeaot_shim.h"
#include "imports.h" // extern so_module dotnet_mod; (dso_offset below)

// ---------------------------------------------------------------------------
// abort/exit/write logging hooks.
//
// "abort" and "exit" are both in StS2.so's own undefined-symbol list
// (reference/nativeaot-probe-430/undefined_symbols.txt) -- they resolved
// straight to the real newlib functions with no logging (imports.c), which
// is exactly how a hard NativeAOT-side failure (unhandled exception,
// FailFast, GC init failure, ...) can terminate the whole process without
// leaving a single line in sts2_debug.log: no Data Abort means no
// Atmosphère crash report either, matching a "closes silently" hardware
// report (31/jul) after godotsharp_game_main_init started running.
// We can't reproduce Celeste64's --wrap=RaiseFailFastException /
// --wrap=abort trick (those need to be linker flags at the link stage that
// PRODUCES StS2.so; we only load a prebuilt StS2.so via so_util, we
// never link it) -- but "abort"/"exit" are plain imports like any other
// libc symbol in our table, so a real substitute function works exactly
// like every other _fake shim in this codebase, no wrapping needed.
// ---------------------------------------------------------------------------

// caller is an absolute runtime address; StS2.so's own load_virtbase
// varies every boot (depends on available RAM at the time -- see
// SO_HEAP_RESERVE/dotnet_so_base in main.c), so print it as an OFFSET into
// StS2.so (usable directly with `addr2line -e StS2.so`) instead of
// making whoever reads the log do that subtraction by hand.
static uintptr_t dso_offset(void *caller) {
  uintptr_t base = (uintptr_t)dotnet_mod.load_virtbase;
  uintptr_t addr = (uintptr_t)caller;
  return (base && addr >= base) ? addr - base : addr; // fall back to raw if outside StS2.so
}

__attribute__((noreturn)) void sts2_abort_hook(void) {
  // Level 0 is always RaiseFailFastException itself (a generic funnel
  // called from all over the runtime on any fatal condition) -- not useful
  // by itself, see PORT_NOTES.md. Levels 1/2 need RaiseFailFastException's
  // OWN caller to have kept a standard AAPCS64 frame-pointer chain (X29);
  // not guaranteed for clang-compiled code, but cheap to try -- worst case
  // these print garbage, GCC docs consider level 1 reasonably safe, 2 more
  // speculative. __builtin_return_address takes a compile-time-constant
  // level, so each has to be spelled out (no loop).
  void *lvl0 = __builtin_return_address(0);
  void *lvl1 = __builtin_return_address(1);
  void *lvl2 = __builtin_return_address(2);
  debugPrintf("[nativeaot] abort() called! stack (StS2.so, addr2line -e StS2.so):\n"
              "  [0] 0x%lx (always RaiseFailFastException)\n"
              "  [1] 0x%lx\n"
              "  [2] 0x%lx\n",
              (unsigned long)dso_offset(lvl0), (unsigned long)dso_offset(lvl1), (unsigned long)dso_offset(lvl2));
  debugFlush();
  abort();
}

// Levels 1/2 above are unreliable (both always print 0x0 on hardware --
// clang's NativeAOT build doesn't keep a frame-pointer chain), so the real
// caller of RaiseFailFastException can't be recovered by unwinding from
// abort(). Instead this hijacks RaiseFailFastException itself: hook_arm64
// (main.c, before so_finalize(&dotnet_mod), same "expect these exact bytes
// first" pattern as patch.c's so_patch) overwrites its first instruction
// with a branch here. BR doesn't touch X30, so __builtin_return_address(0)
// reads the link register exactly as the ORIGINAL caller set it up for its
// `bl RaiseFailFastException` -- reliable regardless of frame pointers,
// unlike sts2_abort_hook's levels 1/2. rec/ctx/code are RaiseFailFastException's
// own (PEXCEPTION_RECORD*, PCONTEXT*, uint32_t) args, untouched since BR is
// a plain jump; rec's first 4 bytes are ExceptionCode in PAL's Windows-style
// EXCEPTION_RECORD layout.
__attribute__((noreturn)) void sts2_raisefailfast_hook(void *rec, void *ctx, unsigned int code) {
  void *caller = __builtin_return_address(0);
  unsigned int exceptionCode = rec ? *(unsigned int *)rec : 0;
  (void)ctx;
  debugPrintf("[nativeaot] RaiseFailFastException! code=%u exceptionCode=0x%08x\n"
              "  caller=StS2.so+0x%lx (addr2line -e StS2.so)\n",
              code, exceptionCode, (unsigned long)dso_offset(caller));
  sts2_abort_hook();
}

// GodotSharp_Godot_NativeInterop_ExceptionUtils__LogUnhandledException(Exception e)
// in StS2.so is the one place the actual failing Exception object is a
// live argument (x0 at entry -- confirmed via objdump: its first real
// instruction after the prologue is `str x0, [x29, #16]`, the standard
// AAPCS64 "spill first arg" pattern). Every NativeAOT object starts with an
// 8-byte MethodTable/EEType pointer at offset 0; ilc emits one distinctly
// named `_ZTV<N><MangledTypeName>` rodata symbol per type used as that
// EEType (confirmed empirically: __NativeLibraryStartup's own
// pre-allocated-OOM-exception setup does
// `adr x0, _ZTV39S_P_CoreLib_System_OutOfMemoryException; bl RhpNewFast`,
// i.e. that symbol IS literally the EEType passed to the allocator). So:
// logging *(void**)exceptionObj as a StS2.so-relative offset, then
// `nm --defined-only StS2.so | grep <offset>` against `_ZTV*` symbols,
// identifies the exact exception type without needing to touch the
// object's other fields (whose layout isn't a stable ABI to hand-decode).
void sts2_log_unhandled_exception_hook(void *exceptionObj) {
  void *eetype = exceptionObj ? *(void **)exceptionObj : NULL;
  debugPrintf("[nativeaot] LogUnhandledException! obj=StS2.so+0x%lx eetype=StS2.so+0x%lx\n"
              "  (nm --defined-only -C StS2.so | grep <eetype offset> against _ZTV*)\n",
              (unsigned long)dso_offset(exceptionObj), (unsigned long)dso_offset(eetype));
}

// Hand-ported from GC_Initialize's own disassembly (offsets are static
// vaddrs in StS2.so, specific to this exact build -- see PORT_NOTES.md,
// re-derive with nm/objdump -R/addr2line after every StS2.so rebuild):
//   CreateGCHandleManager()      0xa204c4  (void* -> NULL on failure)
//   WKS::CreateGCHeap()          0xa211ec  (void* -> NULL on failure)
//   WKS::PopulateDacVars(GcDacVars*)              0xa4d420
//   PopulateHandleTableDacVars(GcDacVars*)         0xa55430
// GC_Initialize itself: if CreateGCHandleManager() is NULL, return
// E_OUTOFMEMORY immediately; otherwise always call both PopulateDacVars
// functions, then return E_OUTOFMEMORY if CreateGCHeap() was NULL, else
// store both pointers into *ppHandleManager/*ppHeap and return S_OK (0).
//
// CRASH FIX (02/ago): the original ALSO writes two globals that this hand-
// port initially missed -- g_gc_heap_type (set to 1/WKS as soon as
// handleMgr succeeds) and, more importantly, g_theGCHeap (set to the heap
// pointer once heap creation succeeds too). Confirmed on hardware: nothing
// else in the runtime reads our ppHeap out-param for later global lookups
// -- e.g. HndCreateHandle (handletable.cpp) reads g_theGCHeap directly via
// its own GOT slot and derefs it unconditionally, so leaving it NULL/BSS-
// zero crashed with a Data Abort at address 0 the first time any code
// created a GC handle (which happens almost immediately, inside
// StartupCodeHelpers::InitializeModules -- i.e. running the very first
// static cctor). Addresses found via `objdump -R StS2.so` on the GOT
// slots GC_Initialize's own code uses for these two globals, resolved
// against `nm` -- specific to each exact StS2.so build (shifts on every
// recompile; re-derive with the same technique, see PORT_NOTES.md).
typedef void *(*sts2_noarg_ptr_fn)(void);
typedef void (*sts2_dacvars_fn)(void *);

uint32_t sts2_gc_initialize_hook(void *clrToGC, void **ppHeap, void **ppHandleManager, void *gcDacVars) {
  (void)clrToGC;
  uintptr_t base = (uintptr_t)dotnet_mod.load_virtbase;
  void *handleMgr = ((sts2_noarg_ptr_fn)(base + 0xb44b38))();
  debugPrintf("[nativeaot] CreateGCHandleManager() -> %p\n", handleMgr);
  if (!handleMgr)
    return 0x8007000E; // E_OUTOFMEMORY

  *(uint32_t *)(base + 0x1a5ece0) = 1; // g_gc_heap_type = WKS

  void *heap = ((sts2_noarg_ptr_fn)(base + 0xb45860))();
  debugPrintf("[nativeaot] WKS::CreateGCHeap() -> %p\n", heap);

  ((sts2_dacvars_fn)(base + 0xb71a94))(gcDacVars);
  ((sts2_dacvars_fn)(base + 0xb79aa4))(gcDacVars);

  if (!heap)
    return 0x8007000E; // E_OUTOFMEMORY

  *(void **)(base + 0x1a5ecb0) = heap; // g_theGCHeap = heap
  *ppHandleManager = handleMgr;
  *ppHeap = heap;
  return 0; // S_OK
}

// Real call-through wrapper for WKS::GCHeap::Initialize() -- see header
// comment. Its own disassembly (gc.cpp:49039, ~1000 bytes) shows SEVERAL
// distinct failure branches, each setting a recognizable HRESULT before a
// shared epilogue: 0x8013200D (gc_heap::compute_hard_limit() false),
// 0x8013200E (compute_memory_settings-derived check), 0x8013200F
// (GetGCRegionSize()'s top bits set), 0x8007000E/E_OUTOFMEMORY (region
// size not a power of 2, OR region_size*19 > region_range, OR
// gc_heap::initialize_gc() itself failing, OR a `new` failing) -- so
// whichever HRESULT this logs narrows things down immediately, without
// having to hand-port that whole function's arithmetic into C.
//
// Vaddr is specific to the exact StS2.so build (shifts on every
// recompile, same as every other hardcoded offset in this file/main.c --
// re-derive with nm/objdump -R/addr2line, see PORT_NOTES.md). Confirmed on
// hardware (02/ago): forgetting to update this ONE offset after a rebuild
// caused an Instruction Abort (branching into whatever garbage now lives
// at the OLD build's address) -- so_finalize's vtable-slot hook itself
// pointed at the right place, but the function IT calls through to here
// didn't get updated in the same pass.
typedef uint32_t (*sts2_gcheap_init_real_fn)(void *);

uint32_t sts2_gcheap_initialize_hook(void *thisPtr) {
  uintptr_t base = (uintptr_t)dotnet_mod.load_virtbase;
  uint32_t result = ((sts2_gcheap_init_real_fn)(base + 0xb6e7bc))(thisPtr);
  debugPrintf("[nativeaot] WKS::GCHeap::Initialize() -> 0x%08x\n", result);
  return result;
}

__attribute__((noreturn)) void sts2_exit_hook(int code) {
  void *caller = __builtin_return_address(0);
  debugPrintf("[nativeaot] exit(%d) called! caller=StS2.so+0x%lx (addr2line -e StS2.so)\n",
              code, (unsigned long)dso_offset(caller));
  debugFlush();
  exit(code);
}

// Diagnostic (temporary): log every name the runtime probes via getenv.
// NativeAOT's RhConfig reads its own settings as DOTNET_<Name> env vars
// (same convention as GCHeapHardLimit/gcConcurrent/PROCESSOR_COUNT above);
// the LAST name logged before an abort() is a strong hint at which
// subsystem was initializing when it failed -- __builtin_return_address
// beyond level 0 came back NULL (frame pointers not preserved in the AOT
// compiler's output), so this is the next-best lead available without a
// real unwinder.
char *sts2_getenv_hook(const char *name) {
  if (!name) return NULL;

  char *value = getenv(name);

  // COMPlus_ is the runtime's older prefix for the same configuration knobs,
  // and some paths try it FIRST. Everything above is set with the DOTNET_
  // spelling, so without this aliasing those paths see "unset" and fall back
  // to inferring values -- which for GCRegionRange means inferring from our
  // invented memory figures, the thing that setting it was meant to stop.
  //
  // From pvzultimate's getenv_fake, which answers both spellings from one
  // table rather than duplicating every entry.
  if (!value && !strncmp(name, "COMPlus_", 8)) {
    char alt[160];
    snprintf(alt, sizeof(alt), "DOTNET_%s", name + 8);
    value = getenv(alt);
    if (value) {
      debugPrintf("[getenv] %s = %s (via %s)\n", name, value, alt);
      return value;
    }
  }

  debugPrintf("[getenv] %s = %s\n", name, value ? value : "(unset)");
  return value;
}

// fd 1/2 writes normally only go out over nxlink (util.c's initNxLink,
// DEBUG_LOG-gated) -- useless if nothing is listening, which is the common
// case for a plain hbmenu launch. .NET prints unhandled-exception text to
// stderr right before the abort/exit that the hooks above now catch; mirror
// fd 1/2 into sts2_debug.log too so that text isn't lost. Other fds
// (asset files, saves) are untouched -- passed straight to the real write.
ssize_t sts2_write_hook(int fd, const void *buf, size_t count) {
  if ((fd == 1 || fd == 2) && buf && count)
    debugPrintf("[fd%d] %.*s\n", fd, (int)count, (const char *)buf);
  return write(fd, buf, count);
}

// ---------------------------------------------------------------------------
// TLS: NativeAOT (linux-bionic-arm64, same codegen as linux-arm64 for this)
// reads thread-statics via TPIDR_EL0 + offset. Horizon OS doesn't use that
// register for anything of its own, so we're free to park a TLS block there.
// Model: reference/nativeaot-celeste64/nativeaot_shims.c (setup_aot_tls),
// confirmed workable on real hardware for the same runtime family.
// ---------------------------------------------------------------------------

void *__real_memalign(size_t align, size_t size);  // linker --wrap; see gpu_arena.c

#define AOT_TLS_SIZE 65536 // 64 KB, same size Celeste64 validated on hardware

static void install_tls_block(void *block) {
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(block));
}

void nativeaot_setup_thread_tls(void) {
  // __real_memalign: this is 4096-aligned and exactly 64 KB, which is the GPU
  // arena's filter to the byte. It is a TLS block, not a GPU buffer, and the
  // arena is only useful if what goes in it is actually GPU buffers.
  void *block = __real_memalign(4096, AOT_TLS_SIZE);
  if (!block) {
    debugPrintf("[nativeaot] FATAL: could not allocate %d bytes for thread TLS\n", AOT_TLS_SIZE);
    return;
  }
  memset(block, 0, AOT_TLS_SIZE);
  install_tls_block(block);
}

static int s_init_done = 0;
int sts2_trace_all = 0;

void nativeaot_shim_init(void) {
  if (s_init_done) return;
  s_init_done = 1;

  nativeaot_setup_thread_tls(); // main thread

  // GC heap hard limit.
  //
  // CORRECTION (02/ago, after getenv tracing pinpointed this): earlier
  // comments here argued our mmap_fake doesn't share Celeste64's "reserve
  // ~100x upfront" multiplier because it's a plain aligned_alloc per call
  // with no blowup logic of ITS OWN -- true, but irrelevant: the multiplier
  // isn't the ALLOCATOR's doing, it's the .NET 8 GC's OWN behavior (region-
  // based GC reserves a large upfront virtual range sized off the heap hard
  // limit, independent of which PAL/allocator services the mmap call).
  // Celeste64 uses the exact same "mmap == aligned_alloc" strategy we do,
  // so the same multiplier applies here too -- raising this to 128 MB
  // (cheap insurance, allegedly) was backwards: a getenv trace (this
  // session) showed DOTNET_GCHeapHardLimit correctly read as 134217728,
  // immediately followed by the silent abort with no further getenv calls
  // at all -- consistent with the GC's own upfront reservation mmap
  // failing right after config parsing finishes, because aligned_alloc()
  // can't satisfy a request ~100x larger than 128 MB. Reverted to
  // Celeste64's own hardware-proven value.
  //
  // LEFT AS-IS, and the units are no longer ambiguous: this is 352 MB.
  //
  // The comment above settles it. A previous attempt set "8000000" and the
  // getenv trace showed the runtime reading 134217728 -- which is 0x8000000
  // exactly. DOTNET_ values are parsed as HEXADECIMAL, so this literal is
  // 0x16000000 = 352 MB, and the old trailing "// 16 MB" was simply wrong.
  //
  // That mattered: an earlier note elsewhere concluded the value was decimal
  // (15.3 MB) because only 14 MB was ever committed. 14 MB is this game's
  // working set, comfortably under a 352 MB cap -- it says nothing about where
  // the cap is. Both notes are corrected.
  //
  // The "~100x reservation" failure described above does not apply now:
  // DOTNET_GCRegionRange is set explicitly a few lines down, which bounds the
  // upfront reserve instead of letting the GC size it from this limit.
  setenv("DOTNET_GCHeapHardLimit", "16000000", 1); // 0x16000000 = 352 MB

  // Skip CPU-count autodetection. NOT actually the cause of the silent
  // abort (that theory came from misreading an address that landed
  // exactly on a function boundary -- see PORT_NOTES.md, "InitializeCurrent-
  // ProcessCpuCount was a false lead"; a later getenv trace confirmed this
  // value IS read successfully and PalInit() moves well past this point).
  // Left in anyway -- bypassing sched_getaffinity/CPU_COUNT entirely via
  // the documented DOTNET_PROCESSOR_COUNT config knob is harmless and one
  // less thing to worry about. "3" matches sched_getaffinity_fake's own
  // reported mask (godot_shim.c) for consistency, not an independently
  // measured core count.
  setenv("DOTNET_PROCESSOR_COUNT", "3", 1);

  // ---------------------------------------------------------------------
  // The rest of this block is adopted from pvzultimate's runtime_glue.c,
  // which runs the same NativeAOT runtime on the same platform and reached
  // each of these the hard way. Its comments are the evidence; this is a
  // summary of why each one is here.
  // ---------------------------------------------------------------------

  // Concurrent GC suspends threads via pthread_kill, which Horizon cannot
  // deliver. Stop-the-world only avoids ever needing that signal.
  setenv("DOTNET_gcConcurrent", "0", 1);

  // Workstation GC: one heap, no server GC thread pool. One fewer set of
  // threads inside the suspension problem.
  setenv("DOTNET_gcServer", "0", 1);

  // TimeZoneInfo.
  //
  // pvzultimate hit a memcpy(NULL, src, 0x100000) reached from managed code
  // immediately after the runtime probed this variable, found it unset, and
  // went looking for the Android tzdata blob -- which does not exist here, so
  // the buffer it copies into is null.
  //
  // Our own crash is a null dereference in managed code during startup. Not
  // the same shape, but the same class: a platform query that cannot succeed
  // returning null into code that does not check. Setting this makes the
  // runtime skip the database and use UTC, which is the honest answer for a
  // console that exposes no timezone configuration to us.
  //
  // Read at RUN TIME, unlike the globalization switch, which NativeAOT bakes
  // in at compile time and therefore ignores from here.
  setenv("DOTNET_SYSTEM_TIMEZONE_INVARIANT", "1", 1);

  // Belt to InvariantGlobalization's braces (sts2.aot.props sets that at
  // compile time). Harmless if the baked-in switch already won.
  setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1);

  // ZERO, not one. Adopted from pvzultimate as "1" in run 7, which is right
  // for a game that never names a culture and wrong for this one.
  //
  // Run 11, verbatim:
  //   CultureNotFoundException: Only the invariant culture is supported in
  //   globalization-invariant mode. en-US is an invalid culture identifier.
  //     at NullPlatformUtilStrategy.GetThreeLetterLanguageCode()
  //     at LocManager..ctor()
  //
  // With PREDEFINED_CULTURES_ONLY set, `new CultureInfo("en-US")` throws in
  // invariant mode. Cleared, it succeeds and behaves invariantly -- a
  // CultureInfo that exists and does nothing locale-specific, which is the
  // honest offer from a console with no ICU.
  //
  // sts2.aot.props sets the matching MSBuild property; this covers the runtime
  // read as well, since the two are separate switches.
  setenv("DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY", "0", 1);

  // Region reservation.
  //
  // With regions (default since .NET 7) the GC reserves a contiguous address
  // range up front, sized INDEPENDENTLY of the hard limit -- inferred from how
  // much RAM it believes the machine has, which on this platform means numbers
  // our own sysconf/sysinfo shims invented.
  //
  // This matters here specifically: run 6 died immediately after a long run of
  // 64 KB region commits. Pinning the reservation removes the GC's dependency
  // on our fabricated memory figures. Hex; 0x40000000 = 1 GB.
  // NOT 1 GB.
  //
  // This was adopted from pvzultimate in run 7 as 0x40000000. On a platform
  // where mmap is malloc-backed, a "reservation" is a real allocation:
  // imports.c's mmap_fake calls memalign(0x1000, len), and run 23's log shows
  // exactly one 1024 MB mmap. A gigabyte of physical memory, permanently, for
  // address space the GC only reserves.
  //
  // It is also oversized on its own terms. GCHeapHardLimit below is 0x16000000
  // (369 MB), so the range reserved was nearly three times a heap that can
  // never grow into it.
  //
  // The cost showed up as 44 x GL_OUT_OF_MEMORY in glTexImage: the gigabyte
  // came straight out of the texture budget, and the engine crashed on
  // whichever allocation happened to fail next.
  //
  // Sized from the hard limit with headroom, and configurable so it can be
  // tuned without a rebuild -- builds are expensive and this is exactly the
  // kind of number that wants measuring.
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%X", (unsigned)config.gc_region_range_mb << 20);
    setenv("DOTNET_GCRegionRange", buf, 1);
    // The pair has to be consistent, and nothing was checking.
    //
    // DOTNET_ values are hexadecimal, so the hard limit below parses as
    // 0x16000000 = 352 MB. A region reserve smaller than that is a
    // configuration the GC cannot satisfy: it may commit up to the limit but
    // has nowhere to commit into.
    const char *hl = getenv("DOTNET_GCHeapHardLimit");
    const unsigned long limit_mb =
        hl ? (unsigned long)(strtoull(hl, NULL, 16) >> 20) : 0;

    debugPrintf("[gc] region range %u MB, heap hard limit %lu MB (0x%s)%s\n",
                config.gc_region_range_mb, limit_mb, hl ? hl : "unset",
                (limit_mb && config.gc_region_range_mb < limit_mb)
                  ? "  *** RANGE IS SMALLER THAN THE LIMIT ***" : "");
  }

  // Gen0 size. A small gen0 provokes collections during startup, which is
  // exactly when the managed heap is least settled. Hex; 0x4000000 = 64 MB.
  setenv("DOTNET_GCgen0size", "4000000", 1);

  // W^X.
  //
  // Off, the runtime expects one address it can both write to and execute
  // from: install thunk stubs, run them, write more into the same pages.
  // Horizon grants a range RW or RX and never both, so that pattern faults
  // however carefully the permissions are juggled underneath it.
  //
  // On, the runtime keeps the write and execute views apart itself, which is
  // the model this platform can satisfy. pvzultimate enabled this "having
  // watched the alternative fail".
  // W^X is OFF, and this is a deliberate change from the pvzultimate settings
  // it was copied from in run 7.
  //
  // W^X makes the runtime double-map executable memory: one RW view and one RX
  // view of the same pages. Our mmap is malloc-backed and cannot do that -- it
  // would return two independent buffers, and writes through one would never
  // appear in the other.
  //
  // As it happens the payload does not implement it: "WriteXorExecute" does not
  // appear in libsts2.so and it imports neither memfd_create nor shm_open, so
  // the variable is inert. Setting it was harmless and also unchecked, which is
  // how the 1 GB GCRegionRange arrived from the same table and cost several
  // rounds. Copying settings between ports without checking they apply is the
  // pattern worth dropping, not just this one value.
  setenv("DOTNET_EnableWriteXorExecute", "0", 1);

  // Read by ModelDbPatches.InitPrefix. Lets the stock ModelDb.Init run instead
  // of our two-phase replacement, so "is our patch causing the missing content
  // or is the content genuinely absent" is one config edit rather than a
  // rebuild. See docs/RUN_15.md.
  // Read by HoverTipPatches. Hover tips are the strongest correlate of the
  // repeating RichTextLabel/Node2D crash; this turns them off so one run can
  // rule them in or out. See docs/RUN_39.md.
  if (config.no_hover_tips)
    setenv("STS2_NO_HOVER_TIPS", "1", 1);

  if (config.original_modeldb)
    setenv("STS2_ORIGINAL_MODELDB", "1", 1);

  debugPrintf("[nativeaot] shim init: TLS installed, GC env vars set\n");
}

// ---------------------------------------------------------------------------
// signals: nothing on Horizon OS delivers POSIX signals to our threads, so
// every one of these is a "pretend it worked, do nothing" stub -- same
// posture as the existing sigaction entry this file's sibling shims use
// elsewhere in the wrapper.
// ---------------------------------------------------------------------------

#undef sigaddset
int sigaddset(sigset_t *set, int sig) {
  if (set) *set |= (1u << sig);
  return 0;
}

#undef sigemptyset
int sigemptyset(sigset_t *set) {
  if (set) *set = 0;
  return 0;
}

void (*signal(int signum, void (*handler)(int)))(int) {
  (void)signum; (void)handler;
  return NULL; // "no previous handler", never fails
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set; (void)oldset;
  return 0;
}

// PalHijack (NativeAOT's GC thread-suspend path) signals a thread via
// pthread_kill() and waits for an ack that Horizon OS will never deliver --
// on Celeste64 this hung and then FailFast-aborted. We can't intercept
// PalHijack itself here (no --wrap at this link stage; StS2.so is a
// prebuilt module so_util loads, not something we link ourselves), so the
// next best thing is making pthread_kill fail immediately: PalHijack should
// treat that as "hijack unsupported" and fall back to cooperative
// suspension (a GC poll point) instead of blocking on an ack.
// DOTNET_gcConcurrent=0 above should mean this path is rarely hit either way.
// ---------------------------------------------------------------------------
// The GC suspension watch. Adopted from pvzultimate, whose runtime_glue.c
// calls this "the instrument for the project's go/no-go question".
//
// NativeAOT suspends managed threads for GC by sending SIGRTMIN via
// pthread_kill and reading the target's register context in the handler
// (PalHijack). Horizon delivers no signals, so if that ever fires we cannot
// honour it.
//
// Do NOT "solve" this the way IL2CPP ports solve the analogous Boehm problem.
// Boehm's stop-the-world is acknowledgement-driven and Boehm is conservative
// and non-moving, so posting the ack the undelivered handler would have posted
// works -- a thread that keeps running can at worst cause a missed reference.
// .NET's GC COMPACTS. A thread running through compaction holds stale pointers
// and you get heap corruption that surfaces later as unrelated crashes. There
// is also nothing to fake: PalHijack needs the target's registers, and no
// semaphore stands in for that.
//
// The viable approach is to arrange that no managed thread is ever in
// cooperative mode at GC time -- a thread blocked inside our shim is in a
// P/Invoke, which the runtime already treats as safe and does not hijack.
// This does not enforce that. It MEASURES it.
//
// Why this matters for the crash we currently have: a null dereference in
// managed code, sixteen managed frames deep, immediately after a long run of
// GC region commits, is exactly the shape stale pointers after an unhonoured
// suspension would produce. pvzultimate reports its own count stayed at zero,
// so the invariant can hold -- but that was a different game with different
// threads, and it has to be measured here rather than assumed.
// ---------------------------------------------------------------------------

static unsigned g_hijack_attempts;
static unsigned g_other_signals;

// __libc_current_sigrtmin / __libc_current_sigrtmax are defined further down
// this file already (returning 32 / 64). I added a second pair returning 34,
// which is what pvzultimate uses -- a redefinition error, and the duplicate
// was mine.
//
// The existing value stands, because the number itself does not matter:
// the runtime calls __libc_current_sigrtmin() to LEARN what SIGRTMIN is and
// then passes that value to pthread_kill, so as long as both go through the
// same function the comparison is self-consistent whatever it returns.
//
// The one way that reasoning fails is if the runtime hardcodes a value instead
// of asking. So the test below is a RANGE rather than equality, and it logs
// the actual signal number -- a hijack arriving on an unexpected signal then
// shows up as a log line rather than as silence.
int __libc_current_sigrtmin(void);
int __libc_current_sigrtmax(void);

int pthread_kill(pthread_t thread, int sig) {
  // Any realtime signal is a hijack candidate. PalHijack is the only thing
  // that sends one, and being slightly over-inclusive costs a log line while
  // being under-inclusive costs the entire measurement.
  if (sig >= __libc_current_sigrtmin() && sig <= __libc_current_sigrtmax()) {
    g_hijack_attempts++;
    if (g_hijack_attempts <= 8 || (g_hijack_attempts % 256) == 0) {
      debugPrintf("[gc] *** PalHijack attempt #%u on thread %p (sig %d, SIGRTMIN=%d) ***\n"
                  "[gc]     That thread was in cooperative mode at GC time, so the\n"
                  "[gc]     invariant this port depends on does NOT hold. The GC\n"
                  "[gc]     compacts; a thread that keeps running through that holds\n"
                  "[gc]     stale pointers, and the corruption surfaces later as an\n"
                  "[gc]     unrelated crash.\n",
                  g_hijack_attempts, (void *)thread, sig, __libc_current_sigrtmin());
    }
    // Deliberately nothing else. Returning 0 lets the runtime proceed as if
    // the signal were queued: if the invariant holds this never runs, and if
    // it does, faking success is no worse than faking an ack while keeping the
    // failure visible.
    return 0;
  }

  g_other_signals++;
  return 0;
}

// Report only when the count has MOVED since last time.
//
// The frame-8 report was the only one, which meant a hijack during gameplay --
// the only place it can matter -- was never reported at all. Called every
// frame now; silent unless something changed, so it costs a compare.
//
// This is the instrument for whether the port's core assumption holds: that no
// managed thread is ever in cooperative mode at GC time. A non-zero count here
// means the GC compacted while a thread kept running, and any object that
// looks live but has stale contents -- which is exactly the crash being
// chased -- is explained by it.
void sts2_gc_watch_poll(void) {
  static unsigned last;
  if (g_hijack_attempts == last) return;
  last = g_hijack_attempts;
  debugPrintf("[gc] *** hijack count changed: %u *** the cooperative-mode\n"
              "[gc]     invariant is BROKEN; heap contents cannot be trusted\n"
              "[gc]     from here on\n", g_hijack_attempts);
  debugFlush();
}

void sts2_gc_watch_report(void) {
  if (g_hijack_attempts == 0)
    debugPrintf("[gc] hijack attempts: 0 -- cooperative-mode invariant HELD "
                "(%u other signals ignored)\n", g_other_signals);
  else
    debugPrintf("[gc] hijack attempts: %u -- invariant BROKEN. Any managed heap\n"
                "[gc]     corruption after this point is explained by it.\n",
                g_hijack_attempts);
}

// ---------------------------------------------------------------------------
// pthread_condattr_*: bionic's pthread_condattr_t is a plain 32-bit int
// (unlike newlib's opaque struct), same situation as pthread_mutexattr_t
// elsewhere in this wrapper (imports.c) -- so this follows that exact
// precedent (int-based fake for StS2.so's OWN direct calls to these
// names, no passthrough to newlib's real pthread_condattr_* which would
// read/write past a bionic-sized int).
//
// Named sts2_condattr_* rather than the real pthread_condattr_* names:
// libsysbase.a (devkitA64) has ITS OWN real, working implementation of
// these -- confirmed via `nm` on the .a directly -- so defining plain
// "pthread_condattr_init" etc here would silently shadow (strong symbol
// beats weak/archive) the real one everywhere in this binary, including
// imports.c's OWN cond-var creation code (make_cond), which genuinely
// needs the REAL implementation to make pthread_condattr_setclock(
// CLOCK_MONOTONIC) actually take effect on the backing cond object (see
// make_cond's comment, imports.c) -- CoreCLR's PAL creates every one of
// its internal timed-wait condvars that way, and our cond_init_fake used
// to silently ignore the requested clock entirely, explaining a
// Task.Delay-based game timer that hangs forever with no exception.
int sts2_condattr_init(pthread_condattr_t *attr) { if (attr) *(int *)attr = 0; return 0; }
int sts2_condattr_destroy(pthread_condattr_t *attr) { (void)attr; return 0; }
// Logged because this is the ONLY place any shim stores a bare 32-bit 1 into
// memory the caller owns -- CLOCK_MONOTONIC is 1.
//
// The crash being chased is exactly that shape. Twice now, in two different
// libraries, a pointer field has been found holding a pointer-shaped high half
// with 1 in the low word:
//
//   libgodot_android.so   ldr x8, [x21, #0x8]    TextureRect+8
//   libfmodstudio.so      ldr x0, [x20, #0x8]    object+8
//
// Both at offset +8. Both "a live pointer with a 32-bit value stored over its
// low word", which is what the crash handler says unprompted.
//
// That does NOT make this the cause -- a condattr is the caller's own field
// and writing 1 into it is correct. But it is the only candidate of that exact
// shape in code this port owns, and one line of logging either puts it in the
// frame or rules it out. If this never fires, it is not this.
int sts2_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id) {
  if (attr) {
    static int logged;
    if (logged < 8) {
      logged++;
      debugPrintf("[condattr] setclock(attr=%p, clock=%d) -- 32-bit store of "
                  "%d at that address\n", (void *)attr, (int)clock_id,
                  (int)clock_id);
    }
    *(int *)attr = (int)clock_id;
  }
  return 0;
}

// pthread_attr_getstack/setdetachstate are NOT here: they're direct
// passthroughs to the real newlib functions in imports.c's table, same as
// the pre-existing pthread_attr_init/destroy/setstacksize entries (bionic's
// pthread_attr_t is treated as opaque and only ever touched through these
// functions, so ABI layout doesn't matter as long as every attr operation
// goes through the same real implementation consistently).

// pthread_getattr_np retrieves attrs (mainly stack bounds) of an ALREADY
// RUNNING thread, for GC stack-scanning. Threads here are libnx Thread
// objects wrapped by our own pthread_create (imports.c), not raw newlib
// pthreads, so there's no real attr to hand back. ENOSYS (matching
// reference/nativeaot-celeste64's stub) should make the GC fall back to
// whatever it does when precise native-stack bounds aren't available.
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
  (void)thread; (void)attr;
  errno = ENOSYS;
  return ENOSYS;
}

// ---------------------------------------------------------------------------
// memory: GC heap management. mmap/munmap in imports.c were written for the
// GDScript engine's occasional file-backed mapping, not for a GC doing
// reserve-then-commit-sub-ranges -- see the edits to mmap_fake/munmap_fake
// there (same file, so a single "mmap"/"munmap" table entry serves both
// libgodot_android.so and StS2.so correctly). mprotect is new: NativeAOT's
// GC calls it to toggle page protection on its heap segments.
// ---------------------------------------------------------------------------

// Distinct pages committed inside the largest reservation.
//
// A page re-protected ten times must count once. Summing lengths inflates the
// answer, and that is exactly the mistake that produced run 25: I read the GC's
// MAP_FIXED total (14 MB), missed the mprotect commits (178.9 MB of distinct
// pages) sitting in the same output, and cut the reservation below what the GC
// needs. Reporting the real number removes the reconstruction step.
static uint8_t *s_commit_bitmap;
static size_t   s_commit_pages;

void sts2_report_reservation_commits(void) {
  if (!sts2_biggest_map_len || !s_commit_bitmap) return;
  size_t n = 0;
  for (size_t i = 0; i < s_commit_pages; i++) n += s_commit_bitmap[i];
  debugPrintf("[mem] largest reservation %zu MB, committed %zu MB "
              "(distinct pages -- size it from THIS, not from log arithmetic)\n",
              sts2_biggest_map_len >> 20, (n * 0x1000) >> 20);
}

int mprotect(void *addr, size_t len, int prot) {
  if (sts2_biggest_map_len && (prot & 3) == 3) {
    const uintptr_t a = (uintptr_t)addr;
    if (a >= sts2_biggest_map_base
        && a < sts2_biggest_map_base + sts2_biggest_map_len) {
      if (!s_commit_bitmap) {
        s_commit_pages  = sts2_biggest_map_len / 0x1000;
        s_commit_bitmap = calloc(1, s_commit_pages);
      }
      if (s_commit_bitmap) {
        size_t i = (a - sts2_biggest_map_base) / 0x1000;
        for (size_t k = 0; k < len / 0x1000 && i + k < s_commit_pages; k++)
          s_commit_bitmap[i + k] = 1;
      }
    }
  }
  // glibc/bionic PROT_* bit values (both agree: R=1, W=2, X=4).
  uintptr_t page_addr = (uintptr_t)addr & ~0xFFFULL;
  size_t page_len = (((uintptr_t)addr + len) - page_addr + 0xFFF) & ~0xFFFULL;

  u32 nx_perm = 0;
  if (prot & 1) nx_perm |= 1; // Perm_R
  if (prot & 2) nx_perm |= 2; // Perm_W
  // Perm_X (4) is rejected by svcSetMemoryPermission for heap memory --
  // NativeAOT is fully ahead-of-time compiled, so nothing should ever need
  // to mark GC-heap pages executable; if this assumption is wrong, hardware
  // testing will surface it as a very specific PROT_EXEC mprotect failure.

  Result rc = svcSetMemoryPermission((void *)page_addr, page_len, nx_perm);
  if (R_SUCCEEDED(rc)) {
    // SUCCESS is traced only under trace_all. The GC commits regions in 64 KB
    // steps, so this fires hundreds of times on the boot path -- 601 lines in
    // the first run to reach the main menu -- and per-line flushing is still
    // on that early, making each one a synchronous SD write.
    //
    // Failures below are logged unconditionally, which is the half that ever
    // mattered: a successful mprotect is the expected case and says nothing.
    if (sts2_trace_all)
      debugPrintf("[mprotect] %p len=0x%lx prot=%d -> OK\n", addr, (unsigned long)len, prot);
    return 0;
  }

  // NRO code/data pages are CodeMutable-typed; svcSetMemoryPermission only
  // works on Heap-typed memory, so this fails for anything that isn't part
  // of a GC-owned mmap block. If the page already has the requested
  // permission, treat that as success instead of a hard failure.
  MemoryInfo info; u32 pageinfo;
  Result qrc = svcQueryMemory(&info, &pageinfo, page_addr);
  if (R_SUCCEEDED(qrc) && (info.perm & nx_perm) == nx_perm)
    return 0;

  debugPrintf("[nativeaot] mprotect(%p, 0x%lx, %d) failed: svc=0x%x\n",
              addr, (unsigned long)len, prot, rc);
  errno = EACCES;
  return -1;
}

int mlock(const void *addr, size_t len) { debugPrintf("[mlock] %p len=0x%lx -> OK\n", addr, (unsigned long)len); return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

// Diagnostic (temporary): PalInit()'s literal last step
// (src/coreclr/nativeaot/Runtime/unix/PalRedhawkUnix.cpp) is
// pthread_key_create(&key, RuntimeThreadShutdown) -- everything traced so
// far (getenv, sysconf, mmap, mlock) accounts for PalInit() up to here with
// nothing unusual; this is the one call in that function we hadn't
// instrumented yet (a direct real-newlib passthrough, imports.c).
int sts2_pthread_key_create_hook(pthread_key_t *key, void (*destructor)(void *)) {
  int rc = pthread_key_create(key, destructor);
  debugPrintf("[pthread_key_create] destructor=%p -> rc=%d\n", (void *)destructor, rc);
  return rc;
}

// ---------------------------------------------------------------------------
// misc POSIX odds and ends the runtime probes at startup or during normal
// operation; none of these are exotic, all are "no-op / minimal-viable"
// exactly like their siblings already in godot_shim.c/libc_shim.c.
// ---------------------------------------------------------------------------

int __libc_current_sigrtmin(void) { return 32; }
int __libc_current_sigrtmax(void) { return 64; }

int __sched_cpucount(size_t setsize, const void *set) {
  (void)setsize; (void)set;
  return 4; // matches sched_getaffinity_fake's reported CPU count elsewhere
}

// named sts2_strcpy_chk, not __strcpy_chk: GCC recognizes __strcpy_chk as
// a fortify-source builtin and refuses to let code take its address
// ("built-in function must be directly called") -- the import table (see
// imports.c) still exports it to StS2.so under the real name "__strcpy_chk",
// only the C-level symbol backing that table entry is renamed.
char *sts2_strcpy_chk(char *dest, const char *src, size_t destlen) {
  // A real fortified strcpy aborts on overflow; we can't detect that here
  // (destlen is the COMPILER's static estimate of dest's size, not
  // something we can double check at runtime any better than plain strcpy
  // already does), so this is only as safe as an ordinary strcpy.
  (void)destlen;
  return strcpy(dest, src);
}

// same GCC-builtin-name issue as sts2_strcpy_chk above, same caveat.
char *sts2_strcat_chk(char *dest, const char *src, size_t destlen) {
  (void)destlen;
  return strcat(dest, src);
}

int asprintf(char **strp, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) { *strp = NULL; return -1; }

  *strp = malloc((size_t)needed + 1);
  if (!*strp) return -1;

  va_start(args, fmt);
  int written = vsnprintf(*strp, (size_t)needed + 1, fmt, args);
  va_end(args);
  return written;
}

// bionic/Linux kernel ABI constant (NOT newlib's own TIMER_ABSTIME, which is
// 4 -- same "the CALLER's ABI numbering wins" issue as BIONIC_RLIMIT_AS).
#define BIONIC_TIMER_ABSTIME 1
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem) {
  if (!(flags & BIONIC_TIMER_ABSTIME))
    return nanosleep(req, rem);
  // CRASH FIX (03/ago): flags was silently ignored here, so an absolute
  // deadline (TIMER_ABSTIME -- the common "sleep until this exact instant"
  // pattern precise periodic timers use, e.g. a game tick driving pill
  // drops) got passed straight to a RELATIVE nanosleep(). CLOCK_MONOTONIC/
  // CLOCK_REALTIME deadlines are huge (seconds since boot/epoch), so the
  // thread slept for that many seconds instead of until that instant --
  // effectively forever on a human timescale. Convert to a relative
  // duration by subtracting the current time first, same clock the caller
  // asked for.
  struct timespec now;
  if (clock_gettime(clock_id, &now) != 0)
    return -1;
  struct timespec rel;
  rel.tv_sec = req->tv_sec - now.tv_sec;
  rel.tv_nsec = req->tv_nsec - now.tv_nsec;
  if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec -= 1; }
  if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; } // deadline already passed
  return nanosleep(&rel, NULL); // rem is unspecified for TIMER_ABSTIME per POSIX
}

int dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; } // "not found", same as dlsym_fake's failure posture

int execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int flock(int fd, int operation) { (void)fd; (void)operation; return 0; }
int prctl(int option, ...) { (void)option; return 0; }
int pipe2(int pipefd[2], int flags) { (void)pipefd; (void)flags; errno = ENOSYS; return -1; }

uid_t geteuid(void) { return 0; }

int getpwuid_r(uid_t uid, void *pwd, char *buf, size_t buflen, void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}

int getrusage(int who, void *usage) {
  (void)who;
  if (usage) memset(usage, 0, 144); // aarch64 struct rusage: 2x timeval + 16 longs
  return 0;
}

// newlib's real arc4random_buf (stdlib.h) calls getentropy() -> _getentropy_r(),
// which devkitA64's libnx C runtime does not implement (link error: "undefined
// reference to _getentropy_r") -- go straight to the Switch's own hardware
// CSRNG instead, same source libc_shim.c already uses for /dev/*random* and
// mbedtls entropy (randomGet(), from <switch/kernel/random.h>).
void sts2_arc4random_buf(void *buf, size_t n) {
  randomGet(buf, n);
}

int sysinfo(void *info) {
  if (!info) return 0;
  memset(info, 0, 112); // aarch64 struct sysinfo, POSIX-standard shape
  unsigned long *p = (unsigned long *)info;
  p[4] = 268435456; // totalram: report 256 MB, matches the conservative GC heap limit above
  p[5] = 268435456; // freeram
  *(unsigned int *)((char *)info + 104) = 1; // mem_unit = 1 byte
  return 0;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
  extern ssize_t __getline(char **, size_t *, FILE *); // devkitA64 newlib provides this, just not under the POSIX name
  return __getline(lineptr, n, stream);
}
