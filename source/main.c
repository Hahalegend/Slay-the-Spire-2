/* main.c -- Super Mario World Remastered Plus (Godot 4.6, Android) Switch
 * wrapper entry point.
 *
 * Loads the arm64-v8a libc++_shared.so + libgodot_android.so pair, provides a
 * minimal Android-like environment (fake JNI, libc/GLES3/EGL import table),
 * owns the EGL/GLES3 context, and drives the GodotLib native lifecycle
 * (initialize/setup/newcontext/resize/step) plus joypad/touch input from the
 * Switch pad and touchscreen.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>   // mallinfo, for the memory trend
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "config.h"
#include "lock_trace.h"
#include "nx_c11.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "jni_godot_ext.h"
#include "swkbd_shim.h"
#include "nx_input.h"
#include "app_paths.h"
#include "vulkan_shim.h"
#include "c11_probe.h"
#include "egl_shim.h"       // sts2_describe_dl_handle, for the trampoline
#include "gpu_arena.h"
#include "astc_bake.h"
#include "vk_validate.h"
#include "crash_handler.h"  // __libnx_exception_handler; no calls, see header
#include "fmod_shim.h"  // sts2_gdext_set_region
#include "audio_sink.h"
#include "patch.h"
#include "libc_shim.h"
#include "nativeaot_shim.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

// Recorded during heap setup, reported once the log is open. userAppInit runs
// before logging exists, so these cannot be printed where they are discovered.
static size_t s_heap_total = 0;
static size_t s_so_reserve = 0;
static int    s_heap_squeezed = 0;
static void *dotnet_so_base = NULL; // set once heap_so_base is known; see main()

so_module cxx_mod, game_mod;
// StS2.so (NativeAOT payload, ~7 MB per reference/nativeaot-probe/) --
// non-static: egl_shim.c's dlopen_fake/dlsym_fake reference it directly
// (see sts2_load_dotnet_module below and imports.h).
so_module dotnet_mod;
static int dotnet_mod_loaded = 0; // 0=not tried, 1=loaded, -1=tried and failed
// cached godotsharp_game_main_init address -- non-static, see imports.h;
// resolved before so_finalize(&dotnet_mod), used by egl_shim.c's dlsym_fake
// (calling so_try_find_addr_rx after finalize is a crash, see below).
// 1 once the wrapper has an EGL surface on the default NWindow. Under Vulkan
// the engine presents through wsi_switch against that same window, so the
// wrapper must neither create that surface nor swap it -- see main() and the
// frame loop.
// Readable by imports.c's shader tracker, which needs to stamp compiles with
// a frame number and cannot see the static.
int s_frames_done_public;

static int s_egl_owns_window = 0;

// The Vulkan decision, made ONCE in main() before EGL is set up, and read by
// game_thread_fn when it builds the engine's argv.
//
// It has to be one decision: main() must know before creating an EGL surface,
// and the game thread must pass the matching --rendering-driver. Probing twice
// risks the two disagreeing, which would mean either a black screen (EGL
// created, Vulkan driving) or no renderer at all.
static int s_use_vulkan = 0;

uintptr_t sts2_godotsharp_game_main_init = 0;       // what dlsym_fake hands the engine
uintptr_t sts2_godotsharp_game_main_init_real = 0;  // the payload's actual export

// ---------------------------------------------------------------------------
// Trampoline over the managed entry point.
//
// The engine dlsym()s godotsharp_game_main_init and calls it with four
// arguments we have never seen:
//
//   godot_bool godotsharp_game_main_init(void *godot_dll_handle,
//                                        void *out_managed_callbacks,
//                                        void *unmanaged_callbacks,
//                                        int32_t unmanaged_callbacks_size)
//
// The first is the one to worry about. GodotSharp wraps it in
// GodotDllImportResolver and resolves EVERY P/Invoke from managed code through
// it -- confirmed in the launcher's own ModEntry.InitializeGodotSharp, which
// does exactly `new GodotDllImportResolver(godotDllHandle).OnResolveDllImport`.
//
// If the engine obtained that handle by dlopen()ing itself, our dlopen_fake
// answered with the FAKE_DL_HANDLE sentinel rather than a real module handle,
// because the permissive fallback returns it for any name it does not
// recognise. Managed P/Invokes would then resolve against a bogus handle.
//
// That is a candidate for the crash in hand: a null static base
// (x8 = 0, read at [x8, #0x40]) reached during a string/dictionary lookup is
// what a type whose cctor failed looks like, and a cctor that P/Invokes
// through a broken resolver is a way to get one.
//
// This does not fix anything. It prints the arguments and the return value,
// which turns a guess into an observation.
// ---------------------------------------------------------------------------

typedef uint8_t (*gsmi_fn)(void *, void *, void *, int32_t);

static uint8_t sts2_gsmi_trampoline(void *godot_dll_handle,
                                    void *out_managed_callbacks,
                                    void *unmanaged_callbacks,
                                    int32_t unmanaged_callbacks_size) {
  so_module *m = so_find_module_by_addr(godot_dll_handle);
  const char *hdesc = sts2_describe_dl_handle(godot_dll_handle);
  char hnote[160];
  if (hdesc)
    snprintf(hnote, sizeof(hnote), "   <- %s", hdesc);
  else if (m)
    snprintf(hnote, sizeof(hnote), "   (inside %s)", m->name);
  else
    snprintf(hnote, sizeof(hnote), "   (not a known handle and not in any module)");

  debugPrintf(">> godotsharp_game_main_init(\n"
              "     godot_dll_handle       = %p%s\n"
              "     out_managed_callbacks  = %p\n"
              "     unmanaged_callbacks    = %p\n"
              "     unmanaged_callbacks_sz = %d)\n",
              godot_dll_handle, hnote,
              out_managed_callbacks, unmanaged_callbacks,
              (int)unmanaged_callbacks_size);
  debugFlush();

  const uint8_t ok = ((gsmi_fn)sts2_godotsharp_game_main_init_real)(
      godot_dll_handle, out_managed_callbacks,
      unmanaged_callbacks, unmanaged_callbacks_size);

  debugPrintf(">> godotsharp_game_main_init -> %d\n", (int)ok);
  debugFlush();
  return ok;
}

// reserve a slice for the .so loader; the rest is the newlib heap where the
// engine's malloc lands. libgodot_android.so's LOAD zone is ~73 MB,
// libc++_shared.so's ~1.3 MB. StS2.so's real in-memory footprint jumped
// from ~7 MB to ~24 MB (readelf -l summed LOAD MemSiz) once
// IlcTrimMetadata=false/TrimmerRootAssembly kept full reflection metadata
// for GodotSharp (needed for ScriptManagerBridge's NativeName field
// lookups, see PORT_NOTES.md) -- DOTNET_SO_SLICE/SO_HEAP_RESERVE bumped
// accordingly, with headroom for future project growth. Requires full-RAM
// mode (title override / forwarder) for the engine heap.
// GDExtension slice, sized from the real binaries rather than guessed
// (readelf -lW, LOAD vaddr span rounded up to 1 MB):
//
//   libfmod.so         2 MB      libGodotFmod.so   2 MB
//   libfmodstudio.so   2 MB      libspine_godot.so 3 MB   = 9 MB
//
// 16 MB gives ~78% headroom for FMOD/Spine version growth. Carved out of the
// SO reserve like every other module, NOT memalign'd from the newlib heap: an
// earlier version did that, and a 12 MB contiguous allocation requested at the
// moment the engine dlopen()s a GDExtension -- well after startup, from a heap
// the engine has been hammering -- is a real failure risk for no benefit.
// Sized from the REAL binaries (readelf -lW, LOAD vaddr span), not guessed:
//
//   libgodot_android.so   69 MB
//   libc++_shared.so       2 MB
//   libsts2.so           124 MB   <-- the AOT payload, measured on the first
//                                     successful ILC build
//   GDExtensions           9 MB
//                        ------
//                         204 MB
//
// The inherited 128 MB reserve with a 32 MB dotnet slice was sized for
// drmariomania_nx, whose payload is a fraction of this one. StS2's managed
// half compiles to 124 MB of LOAD segments and would not have fit at all.
#define SO_HEAP_RESERVE   (256 * 1024 * 1024)
#define CXX_SO_SLICE      (2 * 1024 * 1024)
#define DOTNET_SO_SLICE   (160 * 1024 * 1024)
#define GDEXT_SO_SLICE    (16 * 1024 * 1024)

// __nx_nv_transfermem_size is deliberately NOT defined here.
//
// I set it to 1 GB after reading gtasa_nx, which uses 1.5 GB with the note
// "All GPU bos come from this nv transfer-memory region". That is evidently
// true of their stack and NOT of ours. Run 35 versus run 32:
//
//   transfermem unset :  newlib in use 1528 MB, 0 GL_OUT_OF_MEMORY, 55781 lines
//   transfermem 1 GB  :  newlib in use 2544 MB, 14 GL_OUT_OF_MEMORY, 27286 lines
//
// In-use rose by ~1016 MB -- almost exactly the reservation -- while the GPU
// allocations carried on coming from memalign as before. So the region was
// carved out of the heap and then not used for anything, and the gigabyte came
// straight off the budget the game actually needed. It turned a run that
// reached the fight into one that ran out of memory before it.
//
// fruitninja_nx's account is the one that matches this stack: libdrm_nouveau
// backs every buffer with memalign(0x1000, size) from the general heap.
//
// Leaving the symbol undefined restores libnx's weak default, which is exactly
// the configuration run 32 used.
//
// Still READ, in the [mem] line, so the log reports what that default actually
// is -- which is worth knowing and was never visible before.
extern u32 __nx_nv_transfermem_size;


// One helper rather than a `struct stat` local per check.
//
// Two of those landed in the same scope and the second was a redeclaration
// error -- the checks were added in different rounds, each reasonable on its
// own, and neither noticed the other. A named predicate reads better at the
// call site anyway.
static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

size_t g_newlib_heap_size;   // set by __libnx_initheap, read by the [mem] line

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2) {
    so_reserve = size / 2;
    // Not enough heap to honour the full reservation. This is what applet
    // mode looks like: hbmenu launched from the album gets a few hundred MB,
    // and the modules alone need ~204 MB plus a transient copy of each file
    // while so_load parses it.
    //
    // Printed rather than silently accepted because the symptom otherwise is
    // a so_load failure on whichever module happens to be loaded when the
    // slice runs out, which points at the wrong thing entirely.
    s_heap_squeezed = 1;
  }
  s_heap_total = size;
  s_so_reserve = so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  // Published for the contiguity metric below: mallinfo's arena only covers
  // what has been sbrk'd so far, so the untouched remainder has to come from
  // here.
  g_newlib_heap_size = fake_heap_size;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsNso()) {
    if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
    if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
    if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  }
  if (sts2_own_process_handle() == INVALID_HANDLE) {
    if (!sts2_codemem_available())
      fatal_error("Neither ProcessCodeMemory nor CodeMemory (0x4B/0x4C) is available.");
    debugPrintf("[so] using CodeMemory (0x4B/0x4C) fallback backend\n");
  }
}

// Every probe is built from config.data_root rather than a bare filename.
// The bare form worked only because hbmenu sets the CWD to the NRO's folder;
// under the scan fallback in app_paths.c the CWD can be somewhere else
// entirely, and a bare stat() would then fail with a message pointing at the
// wrong problem.
static void check_probe(const char *rel, const char *hint) {
  char path[352], real_path[352];
  snprintf(path, sizeof(path), "%s/%s", config.data_root, rel);
  struct stat st;
  if (stat(app_paths_real_path(path, real_path, sizeof(real_path)), &st) < 0)
    fatal_error("Could not find\n%s\nin %s.\n\n%s", rel, config.data_root, hint);
}

static void check_data(void) {
  check_probe(SO_NAME,     "Place it next to the NRO.");
  check_probe(CXX_SO_NAME, "Place it next to the NRO.");

  // NOT assets/project.binary. That probe is inherited from drmariomania_nx,
  // where the whole game was one Android export and project.binary was its
  // entry point. StS2 is assembled from two sources and neither produces one:
  //
  //   the launcher APK  the custom engine build, GodotSharp, the mobile
  //                     shader overrides, i18n, themes -- but NO game content;
  //                     the launcher downloads that at runtime
  //   the Steam depot   the .pck and the managed assemblies
  //
  // So the meaningful probe is the .pck, which is what the engine actually
  // needs and what a user assembling the SD card is most likely to forget.
  check_probe(PCK_NAME,
              "Copy it from your Steam install. It is NOT in the launcher APK "
              "-- the launcher downloads game content from Steam at runtime.");

  // FMOD's core libraries are in assets/native/ inside the APK, not
  // lib/arm64-v8a/, which is the single easiest thing to miss when assembling
  // the SD card. Warn rather than fail: config.enable_fmod=0 is a legitimate
  // way to boot silently while debugging something else.
  if (config.enable_fmod) {
    char probe[352], real_probe[352];
    struct stat st;
    snprintf(probe, sizeof(probe), "%s/%s", config.data_root, FMOD_CORE_SO_NAME);
    if (stat(app_paths_real_path(probe, real_probe, sizeof(real_probe)), &st) < 0)
      debugPrintf("[data] %s missing -- it is in assets/native/ in the APK, "
                  "not lib/arm64-v8a/. Audio will not initialise.\n",
                  FMOD_CORE_SO_NAME);
  }

  // The two GDExtensions are looked up by prefix (fmod_shim.c), so a file
  // renamed from libGodotFmod.android... to libGodotFmod_android... still
  // loads. Report what is actually there, since a silent prefix match is
  // otherwise invisible and the difference matters when comparing logs.
  {
    char real_root[352];
    DIR *d = opendir(app_paths_real_path(config.data_root, real_root, sizeof(real_root)));
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (!strncmp(e->d_name, "libGodotFmod", 12) ||
            !strncmp(e->d_name, "libspine_godot", 14))
          debugPrintf("[data] gdextension present: %s\n", e->d_name);
      }
      closedir(d);
    }
  }
}

// The upstream port resolved the data root from the launch CWD, which already
// made it work from any folder under /switch. app_paths.c supersedes it: cwd
// is now strategy 2 of 4, behind argv[0] (exact, no probing) and ahead of a
// scan of /switch for a directory containing the engine binary. read_config()
// calls into it, so by the time main() runs config.data_root is already set.

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }

  /* config.resolution_height overrides whatever was decided above.
   *
   * It lands HERE, in set_screen_size, which is where the default is decided,
   * so both the EGL and the Vulkan path start from the requested size.
   *
   * It is NOT the last word. The Vulkan path calls nwindowGetDimensions after
   * nwindowSetDimensions and ADOPTS whatever the window actually became --
   * deliberately, because a swapchain built for a size the window does not
   * have is a failure this port has already had. So a request the hardware
   * declines is overwritten, and the log says so:
   *
   *     == NWindow returned 1280x720, not 640x360 -- adopting it ==
   *
   * That line appearing means the requested resolution did not take.
   *
   * The horizontal is computed, never configured -- the panel is 16:9 in both
   * modes and two independent values only offer ways to stretch the image. */
  if (config.resolution_height > 0) {
    int h2 = config.resolution_height;
    if (h2 < 360) h2 = 360;
    if (h2 > 1080) h2 = 1080;
    if (h2 != config.resolution_height)
      debugPrintf(">> resolution_height %d is outside 360..1080; clamped to %d\n",
                  config.resolution_height, h2);

    /* 16:9, rounded to an even width. 500 -> 888.9 -> 888; an odd width is a
     * bad idea for anything that later wants 2x2 blocks. */
    int w2 = (h2 * 16 / 9) & ~1;

    if (w2 != screen_width || h2 != screen_height)
      debugPrintf(">> render resolution %dx%d (config resolution_height %d; "
                  "the display shows %dx%d and upscales)\n",
                  w2, h2, config.resolution_height, screen_width, screen_height);
    screen_width = w2; screen_height = h2;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES3 context (mesa). Godot's android GL path expects an external
// context that is current on the thread that calls step(), so the wrapper
// owns it, exactly like the Java GLSurfaceView does on Android.
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

// ---------------------------------------------------------------------------
// Emulator GPU barrier compatibility shim (Yuzu / Eden / Sudachi)
//
// Mesa 26.2.4's nouveau_horizon_channel_create builds static cache-acquire,
// full-barrier-acquire, and syncpt fence pushbuffers that issue Maxwell3D
// (0xB197) methods (0x04A2, 0x0451, 0x00B2, 0x0369, 0x050A, 0x0509) on
// subchannel 0 before any explicit SET_OBJECT(0, 0xB197) is pushed. Real
// Tegra X1 hardware accepts this after AllocateObjectContext(0xB197), whereas
// Yuzu 1734 stubs AllocateObjectContext and leaves DmaPusher::subchannels[0]
// == nullptr, crashing on method 0x04A2 or 0x0451 (note: build_report always
// runs immediately after build_fence in channel_submit_locked, so wrapping
// build_fence covers build_report too without altering its fixed 6-word slot).
//
// On real Switch (!sts2_is_emulator()), pass straight through untouched.
// On emulators, prepend SET_OBJECT(subch=0, MaxwellB 0xB197) (2 words).
// ---------------------------------------------------------------------------

extern uint32_t __real_nouveau_horizon_gm20b_build_cache_acquire(uint32_t *out);
extern uint32_t __real_nouveau_horizon_gm20b_build_full_barrier_acquire(uint32_t *out);
extern uint32_t __real_nouveau_horizon_gm20b_build_fence(uint32_t *out, uint32_t syncpt_id, uint32_t flags);

uint32_t __wrap_nouveau_horizon_gm20b_build_cache_acquire(uint32_t *out) {
  if (sts2_is_emulator()) {
    out[0] = 0x20010000u; // INC(1 word, subch=0, method=0x0000 SET_OBJECT)
    out[1] = 0x0000B197u; // MaxwellB (3D)
    return 2u + __real_nouveau_horizon_gm20b_build_cache_acquire(out + 2);
  }
  return __real_nouveau_horizon_gm20b_build_cache_acquire(out);
}

uint32_t __wrap_nouveau_horizon_gm20b_build_full_barrier_acquire(uint32_t *out) {
  if (sts2_is_emulator()) {
    out[0] = 0x20010000u; // INC(1 word, subch=0, method=0x0000 SET_OBJECT)
    out[1] = 0x0000B197u; // MaxwellB (3D)
    return 2u + __real_nouveau_horizon_gm20b_build_full_barrier_acquire(out + 2);
  }
  return __real_nouveau_horizon_gm20b_build_full_barrier_acquire(out);
}

uint32_t __wrap_nouveau_horizon_gm20b_build_fence(uint32_t *out, uint32_t syncpt_id, uint32_t flags) {
  if (sts2_is_emulator()) {
    out[0] = 0x20010000u; // INC(1 word, subch=0, method=0x0000 SET_OBJECT)
    out[1] = 0x0000B197u; // MaxwellB (3D)
    return 2u + __real_nouveau_horizon_gm20b_build_fence(out + 2, syncpt_id, flags);
  }
  return __real_nouveau_horizon_gm20b_build_fence(out, syncpt_id, flags);
}

static int egl_setup(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY) return -1;
  if (eglInitialize(s_dpy, NULL, NULL) == EGL_FALSE) return -2;
  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) return -3;

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg;
  EGLint num = 0;
  if (eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &num) == EGL_FALSE || num < 1)
    return -4;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  nwindowSetCrop(win, 0, 0, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)win, NULL);
  if (s_surf == EGL_NO_SURFACE) return -5;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT) return -6;
  return 0;
}

// ---------------------------------------------------------------------------
// GodotLib native entry points (platform/android/java_godot_lib_jni.h)
// ---------------------------------------------------------------------------

typedef uint8_t jboolean;

static int      (*e_JNI_OnLoad)(void *vm, void *reserved);
// Godot >= 4.5: initialize(godot, asset_mgr, io, net, dirh, fileh, expansion)
static jboolean (*e_initialize)(void *env, void *cls, void *godot, void *asset_mgr,
                                void *io, void *net_utils, void *dir_handler,
                                void *file_handler, jboolean use_apk_expansion);
// Godot <= 4.4: same but with the Activity as the first object argument
static jboolean (*e_initialize44)(void *env, void *cls, void *activity, void *godot,
                                  void *asset_mgr, void *io, void *net_utils,
                                  void *dir_handler, void *file_handler,
                                  jboolean use_apk_expansion);
static int s_glue_44 = 0; // JNI glue generation of the loaded libgodot
static void     (*e_ondestroy)(void *env, void *cls);
static jboolean (*e_setup)(void *env, void *cls, void *cmdline_array, void *tts);
static void     (*e_resize)(void *env, void *cls, void *surface, int w, int h);
static void     (*e_newcontext)(void *env, void *cls, void *surface);
static jboolean (*e_step)(void *env, void *cls);
static void     (*e_key)(void *env, void *cls, int keycode, int unicode, int label, jboolean pressed, jboolean echo);
static void     (*e_joybutton)(void *env, void *cls, int device, int button, jboolean pressed);
static void     (*e_joyaxis)(void *env, void *cls, int device, int axis, float value);
static void     (*e_joyhat)(void *env, void *cls, int device, int hat_x, int hat_y);
static void     (*e_joyconnectionchanged)(void *env, void *cls, int device, jboolean connected, void *name);
// Resolved but no longer called: pointing goes through nx_input.c as MOUSE
// events (see poll_input). Kept because it costs nothing and a future game --
// or a "touch mode" toggle for handheld -- would want it back.
static void     (*e_dispatchTouchEvent)(void *env, void *cls, int ev, int pointer, int count, void *positions, jboolean double_tap);
static void     (*e_focusin)(void *env, void *cls);
static void     (*e_focusout)(void *env, void *cls);
static void     (*e_onRendererResumed)(void *env, void *cls);
static void     (*e_onRendererPaused)(void *env, void *cls);

#define G "Java_org_godotengine_godot_GodotLib_"

static void resolve_entry_points(void) {
  e_JNI_OnLoad           = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_initialize           = (void *)so_find_addr_rx(&game_mod, G "initialize");
  e_initialize44         = (void *)e_initialize;
  // initialize() gained/lost the Activity argument across engine versions;
  // hardwareKeyboardConnected only exists on the new-signature builds (4.5+),
  // so use it to pick the calling convention.
  s_glue_44 = so_try_find_addr_rx(&game_mod, G "hardwareKeyboardConnected") == 0;
  debugPrintf("== godot JNI glue: %s-style initialize ==\n", s_glue_44 ? "4.4" : "4.6");
  e_ondestroy            = (void *)so_try_find_addr_rx(&game_mod, G "ondestroy");
  e_setup                = (void *)so_find_addr_rx(&game_mod, G "setup");
  e_resize               = (void *)so_find_addr_rx(&game_mod, G "resize");
  e_newcontext           = (void *)so_find_addr_rx(&game_mod, G "newcontext");
  e_step                 = (void *)so_find_addr_rx(&game_mod, G "step");
  e_key                  = (void *)so_try_find_addr_rx(&game_mod, G "key");
  e_joybutton            = (void *)so_try_find_addr_rx(&game_mod, G "joybutton");
  e_joyaxis              = (void *)so_try_find_addr_rx(&game_mod, G "joyaxis");
  e_joyhat               = (void *)so_try_find_addr_rx(&game_mod, G "joyhat");
  e_joyconnectionchanged = (void *)so_try_find_addr_rx(&game_mod, G "joyconnectionchanged");
  e_dispatchTouchEvent   = (void *)so_try_find_addr_rx(&game_mod, G "dispatchTouchEvent");
  e_focusin              = (void *)so_try_find_addr_rx(&game_mod, G "focusin");
  e_focusout             = (void *)so_try_find_addr_rx(&game_mod, G "focusout");
  e_onRendererResumed    = (void *)so_try_find_addr_rx(&game_mod, G "onRendererResumed");
  e_onRendererPaused     = (void *)so_try_find_addr_rx(&game_mod, G "onRendererPaused");
  (void)e_key; (void)e_joyhat;
}

// ---------------------------------------------------------------------------
// input: Switch pad + touchscreen -> Godot JoyButton/JoyAxis/touch events.
// The android Java layer translates keycodes into Godot's own enums before
// crossing into native code, so we emit Godot indices directly.
// ---------------------------------------------------------------------------

#define GD_JOY_A 0
#define GD_JOY_B 1
#define GD_JOY_X 2
#define GD_JOY_Y 3
#define GD_JOY_BACK 4
#define GD_JOY_START 6
#define GD_JOY_LSTICK 7
#define GD_JOY_RSTICK 8
#define GD_JOY_L1 9
#define GD_JOY_R1 10
#define GD_JOY_DPAD_UP 11
#define GD_JOY_DPAD_DOWN 12
#define GD_JOY_DPAD_LEFT 13
#define GD_JOY_DPAD_RIGHT 14

#define GD_AXIS_LX 0
#define GD_AXIS_LY 1
#define GD_AXIS_RX 2
#define GD_AXIS_RY 3
#define GD_AXIS_LT 4
#define GD_AXIS_RT 5

static PadState pad;

// label mapping (Switch A -> Godot A, ...): with the game's ui_accept on
// Godot A and ui_back on Godot B this gives standard Switch menu behavior
// (A confirms, B backs out). Slay the Spire 2's own project.godot already
// binds gameplay actions (rotate/hold/hard-drop) to JoyButton/JoyAxis
// indices directly (ControllerSingle* actions) -- unlike the GDScript ports,
// we have the full C# source, so if a mapping needs to change it's a source
// edit in project.godot/[input], not a resource-remap trick.
static struct { u64 sw; int btn; } s_btnmap[] = {
  { HidNpadButton_A,      GD_JOY_A },        // east
  { HidNpadButton_B,      GD_JOY_B },        // south
  { HidNpadButton_X,      GD_JOY_X },        // north
  { HidNpadButton_Y,      GD_JOY_Y },        // west
  { HidNpadButton_L,      GD_JOY_L1 },
  { HidNpadButton_R,      GD_JOY_R1 },
  { HidNpadButton_StickL, GD_JOY_LSTICK },
  { HidNpadButton_StickR, GD_JOY_RSTICK },
  { HidNpadButton_Plus,   GD_JOY_START },
  { HidNpadButton_Minus,  GD_JOY_BACK },
  { HidNpadButton_Up,     GD_JOY_DPAD_UP },
  { HidNpadButton_Down,   GD_JOY_DPAD_DOWN },
  { HidNpadButton_Left,   GD_JOY_DPAD_LEFT },
  { HidNpadButton_Right,  GD_JOY_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;
static float s_prev_axis[6] = { 99, 99, 99, 99, 99, 99 }; // force initial send

static float stick_norm(s32 v) {
  float f = v / 32767.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  return f;
}

static void send_axis(void *cls, int axis, float v) {
  if (v == s_prev_axis[axis]) return;
  s_prev_axis[axis] = v;
  if (e_joyaxis) e_joyaxis(fake_env, cls, 0, axis, v);
}

static void poll_input(void) {
  void *cls = jni_activity_class();
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  // Buttons nx_pointer has claimed for the cursor are held low here, so one A
  // press does not both click and send a gamepad A. Masking the STATE rather
  // than skipping the loop matters: it makes the transition edges come out
  // right, so a button held across a cursor toggle produces a clean release
  // rather than sticking down forever.
  const u64 masked = cur & ~nx_input_masked_buttons();

  if (e_joybutton) {
    for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
      const u64 m = s_btnmap[i].sw;
      if ((masked & m) && !(s_prev_buttons & m))      e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 1);
      else if (!(masked & m) && (s_prev_buttons & m)) e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 0);
    }
  }

  // sticks: godot's android convention is Y-down-positive
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);
  // Left stick drives the cursor while it is visible; zero it here so the game
  // does not also receive it as controller navigation.
  const int lmask = nx_input_left_stick_masked();
  send_axis(cls, GD_AXIS_LX, lmask ? 0.0f : stick_norm(l.x));
  send_axis(cls, GD_AXIS_LY, lmask ? 0.0f : -stick_norm(l.y));
  send_axis(cls, GD_AXIS_RX, stick_norm(r.x));
  send_axis(cls, GD_AXIS_RY, -stick_norm(r.y));
  // ZL/ZR as digital triggers -- from `masked`, not `cur`. nx_pointer uses
  // both as one-handed tap buttons, so reading the raw state here would leak a
  // click through as a trigger pull as well.
  send_axis(cls, GD_AXIS_LT, (masked & HidNpadButton_ZL) ? 1.0f : 0.0f);
  send_axis(cls, GD_AXIS_RT, (masked & HidNpadButton_ZR) ? 1.0f : 0.0f);

  s_prev_buttons = masked;

  // Pointing (touchscreen, USB mouse, stick cursor, gyro) goes through
  // nx_pointer and comes out as MOUSE events, not touch. StS2 is a card game
  // built around hover -- tooltips, target highlighting, damage preview -- and
  // touch semantics have no hover at all. See nx_input.c.
  //
  // The inherited dispatchTouchEvent block this replaces is preserved in git
  // history; if a future game wants touch semantics, that is the shape.
  nx_input_update();
}

// ---------------------------------------------------------------------------
// game thread (owns the EGL context and the whole GodotLib lifecycle)
// ---------------------------------------------------------------------------

static Thread s_game_thread;
static volatile int s_game_running = 1;
static volatile int s_focused = 1;
static volatile int s_frames_done = 0; // step() iterations completed (watchdog)

// Steps that must complete before the watchdog will pause-and-dump. Startup
// loading runs inside a step and can legitimately take tens of seconds; 120 is
// roughly two seconds of ordinary frames, by which point a step means a frame.
#define WATCHDOG_ARM_STEPS 120

// ---------------------------------------------------------------------------
// lightweight always-on telemetry: boot phase timings + gameplay stalls,
// written to <data_root>/boot_stats.txt (one write per event; negligible cost)
// ---------------------------------------------------------------------------

static u64 s_t_boot;
static FILE *s_stats;

static void stats_open(void) {
  char p[300];
  snprintf(p, sizeof(p), "%s/boot_stats.txt",
           app_paths_is_romfs() ? config.save_root : config.data_root);
  s_stats = fopen(p, "w");
  s_t_boot = armGetSystemTick();
  if (s_stats) {
    fprintf(s_stats, "build " __DATE__ " " __TIME__ "\n");
    fflush(s_stats);
  }
}

static void stats_mark(const char *what) {
  if (!s_stats) return;
  const u64 ms = armTicksToNs(armGetSystemTick() - s_t_boot) / 1000000ull;
  fprintf(s_stats, "%7llu ms  %s\n", (unsigned long long)ms, what);
  fflush(s_stats);
}

static void game_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // bionic stack canary from tpidr_el0+0x28

  // Only if the context exists. Under Vulkan main() never called egl_setup(),
  // so s_dpy/s_surf/s_ctx are all EGL_NO_* and these would be operating on
  // nothing -- eglMakeCurrent with a null display is an error at best and a
  // fault at worst.
  if (s_egl_owns_window) {
    eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
    eglSwapInterval(s_dpy, 1);   // vsync; the Vulkan path gets this from its
                                 // swapchain present mode instead
  }

  void *cls = jni_activity_class();

  if (e_JNI_OnLoad) {
    debugPrintf(">> JNI_OnLoad...\n");
    e_JNI_OnLoad(fake_vm, NULL);
    debugPrintf(">> JNI_OnLoad ok\n");
  }

  debugPrintf(">> GodotLib.initialize...\n");
  jboolean ok;
  if (s_glue_44)
    ok = e_initialize44(fake_env, cls, jni_activity_object(),
                        jni_godot_object(), jni_assetmgr_object(),
                        jni_godot_io_object(), jni_netutils_object(),
                        jni_dirhandler_object(), jni_filehandler_object(),
                        0 /* use_apk_expansion */);
  else
    ok = e_initialize(fake_env, cls,
                      jni_godot_object(), jni_assetmgr_object(),
                      jni_godot_io_object(), jni_netutils_object(),
                      jni_dirhandler_object(), jni_filehandler_object(),
                      0 /* use_apk_expansion */);
  debugPrintf(">> GodotLib.initialize -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.initialize failed.");
  stats_mark("GodotLib.initialize");

  // Mount the content pack. Reading at offsets inside one file is much faster
  // than per-file SD path walks, which matters on boot and on every room load.
  //
  // Uses PCK_NAME so this agrees with check_data()'s preflight probe. It said
  // "game.pck" upstream while check_data now demands PCK_NAME, and the two
  // disagreeing is worse than either being wrong on its own: a correctly-named
  // pack fails preflight, and a pack renamed to satisfy preflight is then
  // silently ignored here and the game falls back to loose assets/.
  static char pck_path[352];
  char real_pck[352];
  snprintf(pck_path, sizeof(pck_path), "%s/%s", config.data_root, PCK_NAME);
  struct stat pck_st;
  const int have_pck = (stat(app_paths_real_path(pck_path, real_pck, sizeof(real_pck)), &pck_st) == 0);
  debugPrintf(">> main pack: %s (%s)\n", pck_path, have_pck ? "found" : "absent, using assets/");
  stats_mark(have_pck ? "main pack: FOUND" : "main pack: ABSENT (assets/ dir)");

  // StS2's engine ships BOTH backends (confirmed: 242 vk* symbols and a
  // libvulkan.so dlopen in libgodot_android.so), unlike drmariomania_nx's,
  // whose Makefile notes it had no Vulkan at all. So the renderer is not
  // implicit here -- it has to be chosen, and chosen twice.
  //
  //   --rendering-method  picks gl_compatibility over forward_plus/mobile,
  //                       both of which go through RenderingDevice -> Vulkan.
  //                       Overrides the project's own setting, including the
  //                       rendering_method.mobile override the Android
  //                       platform would otherwise apply.
  //   --rendering-driver  pins the driver within that method. Redundant while
  //                       gl_compatibility implies opengl3, but it costs one
  //                       argument and removes a way for an engine update to
  //                       quietly land somewhere else.
  //
  // egl_shim.c's dlopen_fake additionally refuses libvulkan.so outright, so a
  // failure of these flags degrades to Godot's own fallback rather than to a
  // null call inside the Vulkan context driver.
  const char *args[16];   // max 11 in use (method, driver, render-thread,
                          // assemblies-dir, main-pack, verbose); headroom so
                          // adding one more is not a silent stack overrun
  int nargs = 0;
  // Renderer.
  //
  // Vulkan is opt-in (config.enable_vulkan). Mesa 26.2.0's Horizon build gives
  // NVK on Maxwell plus a real wsi_switch WSI over NWindow; vulkan_shim.c
  // presents it to the engine as the Android surface extension it expects.
  //
  // The probe runs first so a driver that does not come up is one log line
  // here, rather than a failure deep inside the renderer with the GLES path
  // already discarded.
  if (s_use_vulkan) {
    // "mobile", not "forward_plus".
    //
    // forward_plus is Godot's desktop clustered renderer, and it compiles its
    // whole 3D pipeline at startup regardless of what the game draws. The
    // first Vulkan run that got this far produced 64 shader sets:
    //
    //   SDFGI (5 sets), VoxelGI (2), SSAO (4), SSIL (4), screen-space
    //   reflections (3), volumetric fog (2), FSR2 (8), SMAA (3), TAA, bokeh
    //   depth of field, subsurface scattering, cluster render/store/debug,
    //   SceneForwardClustered (the largest, ~104 KB of cache per variant)
    //
    // Slay the Spire 2 is a 2D card game. It uses none of it, and the console
    // spent minutes at 100% CPU on two cores compiling all of it before the
    // first frame -- which is what "stuck on a black screen" actually was.
    //
    // The mobile renderer drops the lot: SceneForwardMobile instead of
    // clustered, no global illumination, no screen-space effects, no temporal
    // upscaling. Still Vulkan, still RD pipeline caching.
    //
    // Configurable, because this is a judgement about THIS game rather than a
    // fact about the hardware: set `rendering_method forward_plus` to compare.
    const char *method = config.rendering_method[0] ? config.rendering_method
                                                    : "mobile";
    args[nargs++] = "--rendering-method";
    args[nargs++] = method;
    args[nargs++] = "--rendering-driver";
    args[nargs++] = "vulkan";
    debugPrintf(">> renderer: vulkan (%s)\n", method);

    // --render-thread safe, pinned.
    //
    // "separate" was tried and caused problems, so it is gone -- not a config
    // knob, not commented out, just not available. Multi-threaded rendering
    // exercises this port's thread shims harder than anything else, and the GC
    // suspension question is still measured rather than solved.
    //
    // Passed EXPLICITLY rather than simply omitted, which is what the code did
    // before the experiment. Omitting it leaves the choice to
    // rendering/driver/threads/thread_model in the game's project.godot, and
    // that is MegaCrit's desktop setting -- if it says multi-threaded, we get
    // the mode that just misbehaved, from a file inside the pck that cannot be
    // edited. "safe" is Godot's own default, so this changes nothing except
    // removing that possibility.
    //
    // It also does not help the thing it was added for: NVK compiles pipelines
    // synchronously with no async path, so no thread model spreads a single
    // vkCreateGraphicsPipelines across cores. See docs/SHADER_STUTTER.md.
    args[nargs++] = "--render-thread";
    args[nargs++] = "safe";
  } else {
    if (config.enable_vulkan)
      debugPrintf(">> renderer: vulkan requested but the probe failed;"
                  " falling back to GLES3\n");
    args[nargs++] = "--rendering-method";
    args[nargs++] = "gl_compatibility";
    args[nargs++] = "--rendering-driver";
    args[nargs++] = "opengl3";
  }

  // --game-assemblies-dir is a LAUNCHER-SPECIFIC option in this custom engine
  // build (not stock Godot -- the binary also carries
  // "STS2Mobile.ModEntry.Apply()" and "Game assembly not found, continuing in
  // launcher-only mode").
  //
  // OFF BY DEFAULT, because it takes a DIFFERENT branch from the one that
  // failed. Run 2 went down the Android default path:
  //
  //   .NET: Android platform detected. Setting api_assemblies_dir to data path
  //   dirExists(...) = 0
  //   ERROR: .NET: Assemblies not found          (gd_mono.cpp:767)
  //
  // The fix applied unconditionally below is to CREATE that directory, which
  // makes the existing path's precondition true without changing which branch
  // runs -- one variable, clean attribution.
  //
  // Set `use_cmdline_assemblies 1` in config.txt to try the launcher branch
  // instead. That needs no rebuild, so both can be tested in one sitting.
  //
  // Caveat worth knowing before spending a run on it: in the engine's rodata,
  // ".NET: Assemblies not found" (0x58770f) and ".NET: Failed to load hostfxr"
  // (0x58772a) are adjacent, which puts the hostfxr load immediately after the
  // directory check. Getting past the check may simply move the failure one
  // step -- and hostfxr is on dlopen_fake's refuse list, deliberately, since
  // CoreCLR needs a JIT we cannot provide. If that is what happens, the
  // NativeAOT path has to be reached some other way, and the [dl] log will
  // say whether it is tried at all.
  static char asm_dir[352];
  if (config.use_cmdline_assemblies) {
    snprintf(asm_dir, sizeof(asm_dir), "%s/dotnet", config.data_root);
    mkdir(asm_dir, 0777);
    args[nargs++] = "--game-assemblies-dir";
    args[nargs++] = asm_dir;
    debugPrintf(">> --game-assemblies-dir %s\n", asm_dir);
  }
  if (have_pck) {
    // Whether override.cfg exists where Godot will look for it.
  //
  // With --main-pack, Godot reads <dir containing the pck>/override.cfg, not
  // res://override.cfg -- the second string is in the binary but belongs to
  // the directory-project path, which this never takes. Getting that wrong
  // means editing a file nothing reads, and the engine says nothing either
  // way, so the wrapper says it here.
  {
    char ovr[352], real_ovr[352];
    struct stat st;
    snprintf(ovr, sizeof(ovr), "%s/override.cfg", config.data_root);

    if (stat(app_paths_real_path(ovr, real_ovr, sizeof(real_ovr)), &st) == 0) {
      debugPrintf(">> override.cfg present (%ld bytes); Godot reads it from the "
                  "pack directory\n", (long)st.st_size);
    } else if (s_use_vulkan && !app_paths_is_romfs()) {
      // Written, not merely reported, when Vulkan is driving.
      //
      // Godot's Android Vulkan backend uses Swappy (Google's Frame Pacing
      // Library, statically linked into libgodot_android.so). Swappy spawns
      // ChoreographerFilter threads that wait on an Android Choreographer
      // callback. There is no Choreographer on Switch, so they never wake --
      // and Swappy's destructor JOINS them.
      //
      // The watchdog dump caught it exactly:
      //
      //   game thread:  pthread_join <- threadWaitForExit <- _waitLoop
      //     +0x41921fc   SwappyVk_destroySwapchain
      //     +0x4196500   erase<VkSwapchainKHR, SwappyVkBase>
      //     +0x419a2c4   swappy::SwappyCommon::~SwappyCommon
      //     +0x418951c   swappy::ChoreographerFilter::~ChoreographerFilter
      //
      // Two dumps ten seconds apart were byte-identical: stuck, not slow.
      //
      // How it is reached: the single QueuePresentKHR failure makes Godot
      // recreate the swapchain, recreation tears down the Swappy state, and
      // the destructor deadlocks. Any present hiccup gets there, not just
      // this one.
      //
      // fruitninja_nx hit the same wall on Unity and byte-patched
      // Swappy::IsEnabledAndActive() to `mov w0,#0; ret` at an offset specific
      // to their engine. Godot exposes it as a project setting, so a plain
      // file does the same job with nothing hardcoded and nothing to
      // re-derive per build.
      FILE *f = fopen(ovr, "w");
      if (f) {
        fprintf(f,
          "; Written by the wrapper: this console cannot do Android frame\n"
          "; pacing. Swappy waits on a Choreographer that does not exist\n"
          "; here, and its destructor joins those threads forever.\n"
          "; Delete this file to restore Godot's default.\n"
          "\n[display]\n\n"
          "window/frame_pacing/android/enable_frame_pacing=false\n");
        fclose(f);
        debugPrintf(">> wrote %s (frame pacing disabled for Vulkan)\n", ovr);
      } else {
        debugPrintf(">> COULD NOT write %s -- Swappy will deadlock on swapchain\n"
                    ">> recreation; create it by hand\n", ovr);
      }
    } else {
      debugPrintf(">> no override.cfg at %s (optional; engine settings unchanged)\n", ovr);
    }
  }

  args[nargs++] = "--main-pack";
    args[nargs++] = pck_path;
  }
#if DEBUG_LOG
  args[nargs++] = "--verbose";
#endif
  void *cmdline = jni_new_string_array(nargs, args);

  debugPrintf(">> GodotLib.setup...\n");
  ok = e_setup(fake_env, cls, cmdline, jni_tts_object());
  debugPrintf(">> GodotLib.setup -> %d\n", (int)ok);
  if (!ok) fatal_error("GodotLib.setup (Main::setup) failed.\nCheck %s.", LOG_NAME);
  stats_mark("GodotLib.setup (project+drivers)");

  debugPrintf(">> newcontext/resize (%dx%d)...\n", screen_width, screen_height);
  e_newcontext(fake_env, cls, jni_surface_object());
  e_resize(fake_env, cls, NULL, screen_width, screen_height);

  debugPrintf(">> entering step loop\n");
  int frames = 0;
  int paused = 0;
  int announced_pad = 0;
  // adaptive CPU boost: shader compilation and level loads are CPU-bound
  // stalls on mesa/nouveau. Any slow step re-arms the boost; it drops only
  // after ~10 s of smooth frames. Boot naturally keeps it armed throughout.
  int boosted = 1; // main() starts boosted for load
  int calm_frames = 0;

  while (s_game_running && !jni_quit_requested) {
    if (!s_focused) {
      if (!paused) {
        if (e_focusout) e_focusout(fake_env, cls);
        if (e_onRendererPaused) e_onRendererPaused(fake_env, cls);
        paused = 1;
      }
      svcSleepThread(16 * 1000 * 1000);
      continue;
    }
    if (paused) {
      if (e_onRendererResumed) e_onRendererResumed(fake_env, cls);
      if (e_focusin) e_focusin(fake_env, cls);
      paused = 0;
    }

    if (announced_pad) poll_input();

    if (frames < 8) debugPrintf(">> step %d begin\n", frames + 1);
    const u64 t0 = armGetSystemTick();
    // Deferred JNI work, on the engine thread between frames.
    //
    // The dialog queue exists because showDialog/showInputDialog/
    // showFilePicker are asynchronous on Android: the Java side returns at
    // once and calls back later. Firing the callback from inside the JNI call
    // would re-enter the engine mid-call. Doing nothing hangs whatever asked.
    // So it is queued there and delivered here.
    jni_drain_dialog_queue();
    jni_rumble_tick();
    swkbd_deliver();   // keyboard results -> GodotLib.key events

    e_step(fake_env, cls);
    if (frames < 8) debugPrintf(">> step %d done\n", frames + 1);

    // Boot is over once several frames have completed: the modules are
    // loaded, the managed side is up or has failed loudly, and what follows is
    // asset loading, where per-line flushing is the dominant cost. Everything
    // that has crashed so far happened before this point, so the log keeps its
    // tail through the risky phase and its speed afterwards.
    // Memory trend.
    //
    // Run 23 died with 44 x GL_OUT_OF_MEMORY in glTexImage, then Godot's own
    // allocator returning null ("Parameter \"mem\" is null"), then a null-
    // destination write. Three crashes across three runs, each at a different
    // site, is what running out of memory looks like -- whichever allocation
    // happens to fail next is the one that crashes.
    //
    // What is NOT known is whether it is a leak or a ceiling, and those need
    // opposite fixes. Sampling used-memory over time answers it: monotonic
    // growth is a leak, a plateau at the limit is a ceiling.
    //
    // Every 300 frames (~5 s), so it costs nothing and still resolves a trend
    // over a session.
    // vk_validate on a cadence, not only on death.
    //
    // The report used to fire only from sts2_dump_all_threads -- the watchdog
    // and the crash handler. So the first run that DID NOT crash collected
    // every descriptor check and then threw the numbers away: no buffer count,
    // no BAD count, no pipeline ring, because nothing went wrong enough to
    // print them.
    //
    // A diagnostic that only reports on failure cannot tell you that things
    // are fine, which is exactly what a clean run needs to establish. Once a
    // minute at 60fps is free.
    if (config.vk_validate && frames > 0 && (frames % 3600) == 0)
      vk_validate_report("periodic");

    if ((frames % 300) == 0) {
      uint64_t total = 0, used = 0;
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

      // Cemu's QueryContiguousHeadroom, adapted: total free says nothing about
      // whether any single allocation can be served. keepcost is the top-most
      // releasable block, and heap-not-yet-extended is contiguous by
      // definition, so their sum is the largest request that can still
      // plausibly succeed.
      //
      // Our [mem] line reported fordblks -- "free 187 MB" at a crash where a
      // 20 MB request could still have failed.
      // mallinfo() WALKS THE HEAP, and a corrupted heap makes it fault.
      //
      // Run 46 crashed inside __malloc_update_mallinfo+0x40, called from
      // _mallinfo_r, on a chunk field holding 0x0000005500000001 -- the same
      // clobbered-low-word signature. newlib's own heap walker faulting is
      // proof the heap METADATA is corrupt, not just some object field.
      //
      // But it also means this diagnostic is what trips over it. Without the
      // call the corrupt chunk might never be touched and the game would keep
      // running -- until malloc or free reached it.
      //
      // It is also EXPENSIVE, which was not obvious until the disassembly was
      // read. _mallinfo_r is:
      //
      //     BL __malloc_lock
      //     BL __malloc_update_mallinfo      <- walks every chunk
      //     BL __malloc_unlock
      //
      // so it holds the GLOBAL malloc lock while walking a ~2.5 GB arena, and
      // every other thread that allocates blocks behind it. Once every 300
      // frames, that is a stall on the frame thread and on all the others --
      // worth knowing given how much of this port has been about hitching.
      //
      // The lock is also why the corruption finding is sound: the walk cannot
      // race another thread's malloc, so the bad chunk it tripped over was
      // genuinely bad rather than momentarily inconsistent.
      //
      // So: a heap-integrity canary that costs a stall always and a crash when
      // it fires. Worth having while hunting this and not otherwise.
      // `mem_trend 0` removes the stall, the detection and the trigger.
      //
      // It does NOT remove all exposure: malloc and free read neighbouring
      // chunk headers on every call, so a corrupt heap still bites eventually.
      // This walk just reaches chunks that current activity never touches.
      if (config.mem_trend) {
        struct mallinfo mi = mallinfo();
        // The pool is carved out of newlib, so mallinfo counts it as "in use"
        // and hides the split. Reporting it separately makes the two budgets
        // legible against each other, which is the comparison that matters when
        // one has to grow and the other cannot shrink.
        debugPrintf("[mem] frame %d: process %llu/%llu MB, "
                    "newlib arena %u MB, in use %u MB (of which GPU pool %zu MB), "
                    "free %u MB, largest contiguous ~%u MB, nv transfermem %u MB\n",
                    frames,
                    (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
                    (unsigned)(mi.arena >> 20), (unsigned)(mi.uordblks >> 20),
                    gpu_arena_live_bytes() >> 20,
                    (unsigned)(mi.fordblks >> 20),
                    (unsigned)((g_newlib_heap_size > (size_t)mi.arena
                                ? g_newlib_heap_size - (size_t)mi.arena : 0
                               ) + (size_t)(mi.keepcost < mi.fordblks
                                            ? mi.keepcost : mi.fordblks)) >> 20,
                    (unsigned)(__nx_nv_transfermem_size >> 20));
      }

      gpu_arena_report();
      heap_canary_report();
      jni_report();
      if (sts2_bad_lock_slots)
        debugPrintf("[lock] %llu pthread slot(s) rejected as implausible so far\n",
                    (unsigned long long)sts2_bad_lock_slots);
      sts2_report_reservation_commits();
      debugFlush();
    }

    if (frames == 8) {
      // Report the GC suspension watch once boot is over. Zero means threads
      // were always in a P/Invoke at collection time, which is the invariant
      // this port's use of .NET depends on; non-zero means any later heap
      // corruption is explained.
      sts2_gc_watch_report();
      lock_trace_report();
      sts2_c11_report();
      {
        // The heap bounds the lock guard compares against.
        //
        // A hardware run rejected 67 slots as outside this range whose values
        // were in the same region as the slots themselves. Printing the actual
        // bounds makes that checkable rather than inferred: any rejected value
        // that falls between these two numbers means the fast test is fine and
        // the problem is elsewhere; one outside them is why the kernel query
        // now runs before rejecting.
        extern char *fake_heap_start, *fake_heap_end;
        debugPrintf("[lock] newlib heap: %p .. %p (%lu MB)\n",
                    (void *)fake_heap_start, (void *)fake_heap_end,
                    (unsigned long)((fake_heap_end - fake_heap_start) >> 20));
      }
      if (!config.eager_log) debugSetEagerFlush(0);
    }
    // Present.
    //
    // ONLY when EGL owns the window. Under Vulkan the engine presents through
    // its own swapchain, which wsi_switch drives against the SAME default
    // NWindow -- there is exactly one, from nwindowGetDefault().
    //
    // Calling eglSwapBuffers as well meant two owners posting to one surface
    // every frame: Vulkan drew, EGL immediately posted its own empty buffer
    // over it, and the screen stayed black while audio and input ran fine.
    // That is what the first Vulkan hardware run looked like.
    //
    // nx_input_draw() is the GLES cursor overlay and goes with the EGL path;
    // under Vulkan it would draw into a context nothing presents.
    if (s_egl_owns_window) {
      nx_input_draw();   // cursor overlay, on top of the finished frame
      eglSwapBuffers(s_dpy, s_surf);
    }
    const u64 step_ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;

    if (step_ms > 50) { // stall (shader compile / load): burst the CPU
      calm_frames = 0;
      if (!boosted) { cpu_boost(1); boosted = 1; }
      if (step_ms > 100 && announced_pad && s_stats && frames < 100000) {
        fprintf(s_stats, "stall %4llu ms  frame %d\n", (unsigned long long)step_ms, frames);
        fflush(s_stats); // we already dropped frames; one tiny write is noise
      }
    } else if (!config.boost && boosted && ++calm_frames > 600) { // ~10 s smooth -> stock clocks
      cpu_boost(0);
      boosted = 0;
    }

    frames++;
    s_frames_done = frames;
    s_frames_done_public = frames;
    sts2_gc_watch_poll();   // silent unless a GC hijack was attempted
    if (frames == 1) stats_mark("step 1 (engine servers up)");
    if (frames == 4) stats_mark("step 4 (game scene running)");
    if (!announced_pad && frames >= 4) {
      // engine servers are up after the first steps; announce the pad once
      if (e_joyconnectionchanged) {
        void *name = jni_new_string("Nintendo Switch Controller");
        e_joyconnectionchanged(fake_env, cls, 0, 1, name);
        jni_release_local(name);
      }
      announced_pad = 1;
      debugPrintf(">> pad announced after %d frames\n", frames);
    }
  }

  debugPrintf(">> leaving step loop (running=%d quit=%d)\n", s_game_running, jni_quit_requested);
  if (e_ondestroy) e_ondestroy(fake_env, cls);
  s_game_running = 0;
}

// ---------------------------------------------------------------------------
// hang watchdog: when the game thread stops completing steps, pause it and
// dump PC/LR plus an FP-chain backtrace so the stall site lands in the log.
// Offsets are printed relative to both loaded modules and the wrapper.
// ---------------------------------------------------------------------------

int main(void); // forward-declared: the watchdog uses it as a code anchor

static void log_code_addr(const char *tag, uint64_t a) {
  // Resolve through so_util's own module list rather than the three bases this
  // used to hardcode. That list covers every module the port loads -- crucially
  // including libsts2.so, which the hardcoded version could not name, and which
  // is where every crash from run 3 onward has actually been.
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)a);
  if (m) {
    debugPrintf("[watchdog]   %s %016llx  %s+0x%llx\n", tag,
                (unsigned long long)a, m->name,
                (unsigned long long)(a - (uintptr_t)m->load_virtbase));
    return;
  }

  // The wrapper itself is not an so_util module: anchor it on a known function,
  // since module base symbols resolve to 0 under hbl.
  // sts2_wrapper_base() finds the real load base from the NRO header, so these
  // offsets go straight into `addr2line -e sts2.elf`. The 1 MB-rounded anchor
  // this replaced produced offsets that resolved to unrelated symbols -- run
  // 11's hang dump read as _eglFindDisplay, execute_cfa_program_generic and
  // __ssvfscanf_r in one stack, which is not a call chain any program has.
  const uint64_t wrap_base = (uint64_t)sts2_wrapper_base();
  if (a >= wrap_base && a < wrap_base + 0x800000)
    debugPrintf("[watchdog]   %s %016llx  sts2+0x%llx\n", tag,
                (unsigned long long)a, (unsigned long long)(a - wrap_base));
  else
    debugPrintf("[watchdog]   %s %016llx\n", tag, (unsigned long long)a);
}

// Capture while paused, print after resuming. NEVER both at once.
//
// The previous version paused the thread and then called debugPrintf a dozen
// times before resuming it. The game thread logs every resource load, so it is
// usually inside debugPrintf -- holding the log lock -- at the moment it is
// paused. The watchdog's own debugPrintf then blocks on that lock forever,
// with the thread it is waiting on suspended. Nothing recovers.
//
// That is what killed run 34. The dump was always this dangerous; it simply
// never fired, because the 45 s threshold and the focus gate kept it quiet.
// Making it fire made the latent deadlock reachable.
//
// Reading the stack does need the thread stopped. Logging does not. So the
// pause window now contains only svcGetThreadContext3 and the frame-pointer
// walk, both of which are plain memory reads that take no locks.
static void watchdog_dump_thread(Thread *t, const char *what) {
  if (R_FAILED(threadPause(t))) {
    debugPrintf("[watchdog] could not pause %s\n", what);
    return;
  }

  ThreadContext ctx;
  uint64_t frame[12];
  int nframes = 0;
  const Result rc = svcGetThreadContext3(&ctx, t->handle);

  if (R_SUCCEEDED(rc)) {
    // [fp] = next fp, [fp+8] = return address
    uint64_t fp = ctx.fp;
    const uint64_t sp = ctx.sp;
    for (int i = 0; i < 12; i++) {
      if (fp < sp || fp > sp + (16ull << 20) || (fp & 7)) break;
      const uint64_t next = *(const uint64_t *)fp;
      const uint64_t ret  = *(const uint64_t *)(fp + 8);
      if (!ret) break;
      frame[nframes++] = ret;
      if (next <= fp) break;
      fp = next;
    }
  }

  threadResume(t);   // BEFORE any logging

  if (R_FAILED(rc)) {
    debugPrintf("[watchdog] svcGetThreadContext3(%s) failed: %08x\n", what, rc);
    return;
  }

  debugPrintf("[watchdog] %s:\n", what);
  log_code_addr("PC", ctx.pc.x);
  log_code_addr("LR", ctx.lr);
  for (int i = 0; i < nframes; i++) {
    char tag[8];
    snprintf(tag, sizeof(tag), "#%d", i);
    log_code_addr(tag, frame[i]);
  }
}

// Not static: the crash handler calls it too.
//
// A single-thread dump is enough for a deterministic fault, but the crash this
// exists for is intermittent and has the shape of a race -- a live object
// (valid vtable) with one field holding 1 and the rest zeroed, read after a
// null check that 1 walks straight through. For that, the faulting thread is
// only half the evidence; what the OTHER threads were doing at the same
// instant is the other half.
//
// Safe to call from __libnx_exception_handler: the faulting thread is already
// stopped by the kernel, and this pauses each of the others in turn rather
// than relying on any lock.
void sts2_dump_all_threads(void) {
  debugPrintf("[watchdog] bases: main()=%p godot=%p libc++=%p\n",
              (void *)&main, game_mod.load_virtbase, cxx_mod.load_virtbase);

  // What the GPU was last told to run, before the thread backtraces. The
  // watchdog fires on exactly the stall that precedes every channel loss, so
  // this is the moment when "which pipeline" stops being an inference from
  // Godot's log lines and becomes a recorded fact.
  vk_validate_report("watchdog fired -- GPU state at the stall");
  watchdog_dump_thread(&s_game_thread, "game thread");

  Thread *thr[16];
  void *entry[16];
  int n = sts2_engine_threads(thr, entry, 16);
  for (int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "engine thread %d (entry godot+0x%lx)", i,
             (unsigned long)((uintptr_t)entry[i] - (uintptr_t)game_mod.load_virtbase));
    watchdog_dump_thread(thr[i], what);
  }
}

// `name` is a bare filename; the path is built from config.data_root. Passing
// the bare name straight to so_load worked only because hbmenu sets the CWD to
// the NRO's folder. Under app_paths.c's scan fallback the CWD may be anywhere,
// and the load would fail with a message naming the file rather than the
// directory it was looked for in.
static void load_module(so_module *mod, const char *name, void *base, size_t limit) {
  char path[352];
  snprintf(path, sizeof(path), "%s/%s", config.data_root, name);

  int res = so_load(mod, path, base, limit);
  if (res < 0)
    fatal_error("Could not load\n%s\nfrom %s (%d).", name, config.data_root, res);
  debugPrintf("== so_load %s ok (load_size=%u KB, load_virtbase=%p) ==\n",
              path, (unsigned)(mod->load_size >> 10), mod->load_virtbase);
}

// Lazily loads StS2.so the first time the engine itself dlopen()s it
// (egl_shim.c's dlopen_fake calls this) -- see config.h DOTNET_SO_NAME and
// PORT_NOTES.md "Ponte GodotSharp<->engine". Unlike cxx_mod/game_mod
// (mandatory, loaded eagerly in main() before this point ever runs), a
// missing StS2.so is NOT fatal: the engine just gets a failed dlopen,
// same as real Android with no working .NET runtime installed -- the
// GDScript/engine side still boots. Idempotent: safe to call repeatedly
// (returns the cached result after the first attempt).
// See the call site above. Reads dotnet_mod's .dynsym directly; safe both
// before and after so_finalize now that the tables are rebased onto
// load_virtbase (so_util.c).
static void sts2_dump_dotnet_dynsym(void) {
  if (!dotnet_mod.syms || !dotnet_mod.dynstrtab || dotnet_mod.num_syms <= 0) {
    debugPrintf("[dynsym] libsts2.so has no usable .dynsym\n");
    return;
  }

  const uintptr_t base = (uintptr_t)dotnet_mod.load_virtbase;
  int shown = 0;

  debugPrintf("[dynsym] libsts2.so: %d entries, base=%p\n"
              "[dynsym] offsets below are file offsets -- usable directly with\n"
              "[dynsym] addr2line/nm if an unstripped copy is ever rebuilt\n",
              dotnet_mod.num_syms, (void *)base);

  uintptr_t mc_start = 0, mc_stop = 0;

  for (int i = 0; i < dotnet_mod.num_syms; i++) {
    const Elf64_Sym *sym = &dotnet_mod.syms[i];
    if (sym->st_shndx == SHN_UNDEF) continue;      // imports, not exports
    if (!sym->st_value) continue;
    const char *nm = dotnet_mod.dynstrtab + sym->st_name;
    if (!nm || !*nm) continue;
    debugPrintf("[dynsym]   +0x%08lx  %6lu  %s\n",
                (unsigned long)sym->st_value,
                (unsigned long)sym->st_size, nm);
    shown++;

    // ILC brackets all compiled C# between these two linker-generated
    // symbols. They are the single most useful thing in a stripped payload:
    // a fault inside the range is the GAME's code, outside it is the runtime.
    // Run 5's crash sat squarely inside, which is what turned "some null deref
    // in libsts2.so" into "a null reference in managed code during module
    // init" -- a completely different investigation.
    if (!strcmp(nm, "__start___managedcode")) mc_start = base + sym->st_value;
    if (!strcmp(nm, "__stop___managedcode"))  mc_stop  = base + sym->st_value;
  }
  debugPrintf("[dynsym] %d defined symbols listed\n", shown);

  if (mc_start && mc_stop) {
    crash_set_managed_range(mc_start, mc_stop);
    debugPrintf("[dynsym] managed code: +0x%08lx .. +0x%08lx (%lu KB)\n",
                (unsigned long)(mc_start - base), (unsigned long)(mc_stop - base),
                (unsigned long)((mc_stop - mc_start) >> 10));
  }
}

int sts2_load_dotnet_module(void) {
  if (dotnet_mod_loaded != 0)
    return dotnet_mod_loaded > 0;

  char path[300], real_path[300];
  snprintf(path, sizeof(path), "%s/%s", config.data_root, DOTNET_SO_NAME);

  FILE *probe = fopen(app_paths_real_path(path, real_path, sizeof(real_path)), "rb");
  if (!probe) {
    debugPrintf("== %s not found at %s; .NET/C# side unavailable ==\n", DOTNET_SO_NAME, path);
    dotnet_mod_loaded = -1;
    return 0;
  }
  fclose(probe);

  nativeaot_shim_init(); // TLS + GC env vars, before any of its code runs
  // DIAGNOSTIC (temporary): confirm our own setenv/getenv actually agree
  // before blaming the runtime's own config reader for ignoring the value.
  debugPrintf("[nativeaot] sanity: DOTNET_PROCESSOR_COUNT=%s DOTNET_GCHeapHardLimit=%s\n",
              getenv("DOTNET_PROCESSOR_COUNT") ? getenv("DOTNET_PROCESSOR_COUNT") : "(null)",
              getenv("DOTNET_GCHeapHardLimit") ? getenv("DOTNET_GCHeapHardLimit") : "(null)");

  int res = so_load(&dotnet_mod, path, dotnet_so_base, DOTNET_SO_SLICE);
  if (res < 0) {
    debugPrintf("!! so_load %s failed (%d)\n", path, res);
    dotnet_mod_loaded = -1;
    return 0;
  }
  debugPrintf("== so_load %s ok (load_size=%u KB, load_virtbase=%p) ==\n",
              path, (unsigned)(dotnet_mod.load_size >> 10), dotnet_mod.load_virtbase);

  sts2_resolve_imports(&dotnet_mod);

  // Resolve the one export egl_shim.c's dlsym_fake will ever need for this
  // module BEFORE so_finalize, exactly like resolve_entry_points() does for
  // game_mod above ("resolve exports before so_finalize maps the code and
  // locks load_base out"). Confirmed on hardware (31/jul): calling
  // so_try_find_addr_rx on an already-finalized module is a Data Abort --
  // mod->syms/dynstrtab (set in so_load from mod->load_base) dangle once
  // so_finalize remaps load_base's pages to load_virtbase as code memory.
  // dlsym_fake reads this cache instead of calling so_try_find_addr_rx
  // itself, since by the time the engine actually calls dlsym (well after
  // this function returns) finalize has already happened.
  sts2_godotsharp_game_main_init_real = so_try_find_addr_rx(&dotnet_mod, "godotsharp_game_main_init");
  sts2_godotsharp_game_main_init = (uintptr_t)&sts2_gsmi_trampoline;
  if (!sts2_godotsharp_game_main_init_real) sts2_godotsharp_game_main_init = 0;
  if (!sts2_godotsharp_game_main_init)
    debugPrintf("!! %s: godotsharp_game_main_init not found\n", DOTNET_SO_NAME);

  // DIAGNOSTIC (temporary): RaiseFailFastException is NativeAOT's generic
  // fatal-error funnel (PalCreateCrashDumpIfEnabled(); abort();) -- every
  // silent abort we've seen on hardware goes through it, but it's a local
  // (non-dynamic) symbol so it's not in so_try_find_addr_rx's table (.dynsym
  // only) and sts2_abort_hook's __builtin_return_address(1/2) can't
  // recover ITS caller (no frame-pointer chain in this build). Binary-patch
  // its entry directly instead: hook_arm64 needs an absolute vaddr, found via
  // `aarch64-none-elf-nm --defined-only -n StS2.so` + confirmed with
  // objdump (see nativeaot_shim.c's sts2_raisefailfast_hook comment and
  // PORT_NOTES.md). Self-disables like patch.c's so_patch if the bytes don't
  // match -- this offset is specific to the exact StS2.so build tested on
  // 2026-08-02 (sha1 76bfb42d...); any different .NET publish invalidates it.
  {
    const uintptr_t kRaiseFailFastVaddr = 0xb79f0c;
    uint32_t *insn = (uint32_t *)((uintptr_t)dotnet_mod.load_base + kRaiseFailFastVaddr);
    if (*insn == 0xa9bf7bfdu) { // stp x29, x30, [sp, #-16]! (RaiseFailFastException's prologue)
      hook_arm64((uintptr_t)insn, (uintptr_t)sts2_raisefailfast_hook);
      debugPrintf("[nativeaot] RaiseFailFastException hooked at StS2.so+0x%lx\n",
                  (unsigned long)kRaiseFailFastVaddr);
    } else {
      debugPrintf("!! RaiseFailFastException: unexpected bytes 0x%08x at StS2.so+0x%lx, not hooking\n",
                  *insn, (unsigned long)kRaiseFailFastVaddr);
    }
  }

  // DIAGNOSTIC (temporary): now that we know the RhFailFast at
  // EnsureRuntimeInitialized is the module init callback (g_RuntimeInitializationCallback,
  // == __NativeLibraryStartup) returning nonzero -- almost certainly an
  // unhandled exception thrown by RunModuleInitializers (running every
  // static cctor: CoreLib, GodotSharp, StS2's own game code) -- hook
  // GodotSharp's own ExceptionUtils.LogUnhandledException(Exception e) to
  // capture WHICH exception. Vaddr/expected bytes specific to this exact
  // StS2.so build (sha1 76bfb42d...), same self-disabling pattern as the
  // RaiseFailFastException hook above and patch.c's so_patch.
  {
    const uintptr_t kLogUnhandledExcVaddr = 0x1103b20;
    uint32_t *insn = (uint32_t *)((uintptr_t)dotnet_mod.load_base + kLogUnhandledExcVaddr);
    if (*insn == 0xa9b77bfdu) { // stp x29, x30, [sp, #-144]! (LogUnhandledException's prologue)
      hook_arm64((uintptr_t)insn, (uintptr_t)sts2_log_unhandled_exception_hook);
      debugPrintf("[nativeaot] LogUnhandledException hooked at StS2.so+0x%lx\n",
                  (unsigned long)kLogUnhandledExcVaddr);
    } else {
      debugPrintf("!! LogUnhandledException: unexpected bytes 0x%08x at StS2.so+0x%lx, not hooking\n",
                  *insn, (unsigned long)kLogUnhandledExcVaddr);
    }
  }

  // DIAGNOSTIC (temporary): the LogUnhandledException hook never fired --
  // the actual sequence traced from Thread::EnsureRuntimeInitialized's
  // g_RuntimeInitializationCallback() is a plain native chain, no managed
  // exception involved: RhInitialize -> InitializeGC -> GCHeapUtilities::
  // InitializeGC -> InitializeGCSelector -> InitializeDefaultGC ->
  // GC_Initialize, which returns E_OUTOFMEMORY (0x8007000E) if either
  // CreateGCHandleManager() or WKS::CreateGCHeap() returns NULL -- and NO
  // mmap call (success or failure, both logged unconditionally by
  // mmap_fake) happens before the abort, so whichever of the two is
  // failing is failing on a pure validation check, not an actual OS
  // allocation attempt. Hook GC_Initialize itself (call-through
  // reimplementation, see nativeaot_shim.c) to see which one and why.
  {
    const uintptr_t kGCInitializeVaddr = 0xb44aa8;
    uint32_t *insn = (uint32_t *)((uintptr_t)dotnet_mod.load_base + kGCInitializeVaddr);
    if (*insn == 0xa9bc7bfdu) { // stp x29, x30, [sp, #-64]! (GC_Initialize's prologue)
      hook_arm64((uintptr_t)insn, (uintptr_t)sts2_gc_initialize_hook);
      debugPrintf("[nativeaot] GC_Initialize hooked at StS2.so+0x%lx\n",
                  (unsigned long)kGCInitializeVaddr);
    } else {
      debugPrintf("!! GC_Initialize: unexpected bytes 0x%08x at StS2.so+0x%lx, not hooking\n",
                  *insn, (unsigned long)kGCInitializeVaddr);
    }
  }

  // DIAGNOSTIC (temporary): GC_Initialize's own two candidates
  // (CreateGCHandleManager/WKS::CreateGCHeap) both returned non-NULL on
  // hardware -- so the E_OUTOFMEMORY must come from the *virtual* call
  // right after (WKS::GCHeap::Initialize(), dispatched through the heap
  // object's vtable, gc.cpp:49039), not from GC_Initialize's own logic.
  // That function is ~1000 bytes with several distinct failure branches
  // (compute_hard_limit/compute_memory_settings/region-size checks, then
  // the real gc_heap::initialize_gc()) -- rather than hand-porting all of
  // that, overwrite the RELOCATED POINTER at its vtable slot (a plain data
  // write: so_load's R_AARCH64_RELATIVE relocations already computed this
  // slot's value into load_base, same "patch load_base before finalize"
  // window as the other hooks, just data instead of hook_arm64 code-patch)
  // so the real function still runs, only wrapped to log its return code.
  // Vaddr found via `objdump -R StS2.so | grep <slot>` against the
  // slot address computed from `nm | grep _ZTVN3WKS6GCHeapE` (address-point
  // convention: vtable symbol + 16, + slot_index*8); specific to this
  // exact StS2.so build (sha1 76bfb42d...).
  {
    const uintptr_t kGCHeapInitVtableSlotVaddr = 0x17129b0;
    const uintptr_t kGCHeapInitRealVaddr = 0xb6e7bc;
    uint64_t *slot = (uint64_t *)((uintptr_t)dotnet_mod.load_base + kGCHeapInitVtableSlotVaddr);
    uint64_t expected = (uint64_t)(uintptr_t)dotnet_mod.load_virtbase + kGCHeapInitRealVaddr;
    if (*slot == expected) {
      *slot = (uint64_t)(uintptr_t)sts2_gcheap_initialize_hook;
      debugPrintf("[nativeaot] WKS::GCHeap::Initialize vtable slot hooked (was StS2.so+0x%lx)\n",
                  (unsigned long)kGCHeapInitRealVaddr);
    } else {
      debugPrintf("!! WKS::GCHeap::Initialize vtable slot: unexpected value 0x%llx (expected 0x%llx), not hooking\n",
                  (unsigned long long)*slot, (unsigned long long)expected);
    }
  }

  so_finalize(&dotnet_mod);
  so_flush_caches(&dotnet_mod);
  so_execute_init_array(&dotnet_mod);
  debugPrintf("== %s loaded and initialized ==\n", DOTNET_SO_NAME);

  // Dump the payload's dynamic symbol table.
  //
  // The shipped libsts2.so is fully stripped -- no .debug_info, no .symtab --
  // so addr2line and nm give nothing, and the unstripped original was built in
  // a container that no longer exists. But .dynsym SURVIVES stripping: the
  // dynamic linker needs it. On the current payload that is ~340 entries.
  //
  // Every one is an anchor. A crash address between two of them is bracketed
  // by name, which is not as good as a symbol table but is enormously better
  // than a bare offset -- and unlike the hardcoded offsets in
  // nativeaot_shim.c, it is derived at runtime and survives every rebuild.
  //
  // Sorted output would be nicer; it is left in table order because sorting
  // 340 entries here means allocating during boot, and the log is grep-able.
  sts2_dump_dotnet_dynsym();
  // The four nativeaot_shim hooks above are byte-pattern guarded and will
  // refuse to install on any payload other than the one their offsets were
  // derived from. That refusal is CORRECT and functionally harmless -- the
  // header is explicit that all four are diagnostic: GC_Initialize's is a
  // hand-port that calls the same real sub-functions, GCHeap::Initialize's is
  // a call-through, and the other two only log. The real functions run either
  // way.
  //
  // What is lost is visibility, precisely where runs 3 and 4 died. If a Data
  // Abort lands inside libsts2.so, convert the Atmosphere report's offsets
  // with the delta printed here, then:
  //   addr2line -f -C -e libsts2.so <converted addresses>
  {
    extern so_module dotnet_mod;
    debugPrintf("[nativeaot] libsts2.so virtbase=%p\n"
                "  crash-report offsets are relative to the executable segment;\n"
                "  file_offset = report_offset + (exec_base - virtbase)\n"
                "  see docs/RUN_04.md for the measured delta and a ready command\n",
                dotnet_mod.load_virtbase);
  }

  // Extra-verbose pthread_mutex_init / cond_init / create tracing
  // (nativeaot_shim.c), switched on here so it covers the managed-startup
  // window rather than the engine's constant mutex churn before it.
  //
  // NOW A CONFIG FLAG, and off by default. It was hardcoded to 1 and marked
  // "temporary", which meant it could not be turned off without a rebuild --
  // and it was never turned off.
  //
  // The cost is not the line itself. Per-line log flushing stays on until
  // frame 8 (util.c), so during boot EVERY trace line is a synchronous SD
  // card write. On the first Vulkan run that reached the main menu,
  // pthread_mutex_init alone was 4041 of 6306 lines -- 64% of the log, each
  // one an SD write, on the critical path of startup. That is a plausible
  // explanation on its own for two cores pinned and a boot that looks hung
  // when it is merely crawling.
  //
  // Set `trace_all 1` in config.txt when the managed-startup detail is
  // actually wanted.
  sts2_trace_all = config.trace_all;

  dotnet_mod_loaded = 1;
  return 1;
}

int main(void) {
  cpu_boost(1);

  // Discovery first, then the trace, then config -- in that order. The trace
  // has to come before anything that can fail on a wrong data root, because
  // every such failure presents as a missing file with no clue why.
  app_paths_log_trace();

  debugPrintf("[mem] heap %u MB, so reserve %u MB (wanted %u MB)\n",
              (unsigned)(s_heap_total >> 20), (unsigned)(s_so_reserve >> 20),
              (unsigned)(SO_HEAP_RESERVE >> 20));
  if (s_heap_squeezed) {
    debugPrintf("[mem] WARNING: the reservation was halved to fit the heap.\n"
                "[mem] The modules need ~204 MB of contiguous space plus a\n"
                "[mem] transient copy of each file while so_load parses it.\n"
                "[mem] This is what applet mode looks like -- launch the\n"
                "[mem] Homebrew Menu by holding R on a GAME rather than from\n"
                "[mem] the album, so the process gets the full memory pool.\n");
  }

  char cfg_path[352];
  snprintf(cfg_path, sizeof(cfg_path), "%s/%s", app_paths_data_root(), CONFIG_NAME);
  if (read_config(cfg_path) != 0)
    write_config(cfg_path);
  if (app_paths_is_romfs())
    read_config_overlay(DEFAULT_DATA_ROOT "/" CONFIG_NAME);

  check_syscalls();
  if (sts2_is_emulator() && !config.emu_vulkan && config.enable_vulkan) {
    debugPrintf("== emulator detected (%s): using GLES3 (Nouveau) renderer (set emu_vulkan 1 to override) ==\n",
                sts2_using_code_memory() ? "CodeMemory" : "ProcessCodeMemory");
    config.enable_vulkan = 0;
  }
  stats_open();
  check_data();
  mkdir(config.save_root, 0777);
  {
    char cache[300];
    snprintf(cache, sizeof(cache), "%s/cache", config.save_root);
    mkdir(cache, 0777);
  }
  setenv("HOME", config.save_root, 1);

  // ------------------------------------------------------------------------
  // Shader caching.
  //
  // The Android launcher's two headline engine patches -- Vulkan pipeline
  // cache persistence and canvas ubershaders -- do NOT apply here. Both are
  // Vulkan concerns (the engine's cache strings are all VK_*) and this port
  // renders through gl_compatibility. Porting them would be wasted effort.
  //
  // The underlying problem does carry over, though: first-encounter shader
  // compilation stalls. It just has a different mechanism and a different fix.
  //
  //   Godot side   rendering/shader_compiler/shader_cache/* writes to
  //                res://.godot/shader_cache, which is inside the read-only
  //                .pck and therefore unusable. The engine logs
  //                'Condition "!shader_cache_dir_valid" is true' when that
  //                happens -- if that line appears, this is why.
  //
  //   Mesa side    the GLSL-to-native disk cache. This one CAN persist,
  //                because it writes wherever we point it. Giving it a
  //                writable directory is the GL equivalent of the launcher's
  //                Vulkan pipeline cache persistence.
  //
  // MESA_SHADER_CACHE_DIR is the current name; MESA_GLSL_CACHE_DIR is the
  // pre-21.2 one. Both are set because switch-mesa's version is not known
  // here, and an unrecognised variable is ignored rather than fatal. If
  // switch-mesa was built with -Dshader-cache=disabled these are simply no-ops
  // and the stutter stays -- check for a populated cache dir after a run.
  // ------------------------------------------------------------------------
  {
    // Godot's own GLES3 shader cache lives under user://.godot/shader_cache.
    // ShaderGLES3::initialize does DirAccess::open on it and ERR_FAIL_CONDs
    // when that returns null -- which is the thirteen
    //   ERROR: Condition "d.is_null()" is true.  (shader_gles3.cpp:787)
    // lines on the first hardware run. DirAccess::open fails on a directory
    // that does not exist, and Godot does not create this one itself when the
    // parent tree is absent.
    //
    // Created here rather than left to the engine because the whole subtree is
    // ours: user:// maps to save_root (jni_fake's getDataDir).
    char gd_cache[352];
    snprintf(gd_cache, sizeof(gd_cache), "%s/.godot", config.save_root);
    mkdir(gd_cache, 0777);
    snprintf(gd_cache, sizeof(gd_cache), "%s/.godot/shader_cache", config.save_root);
    mkdir(gd_cache, 0777);

    // AND the one the GLES3 rasterizer actually asks for.
    //
    // Two different places in the engine want two different directories, and
    // only one of them was being created:
    //
    //   shader_gles3.cpp:787    user://.godot/shader_cache   (created above)
    //   rasterizer_gles3.cpp:342  user://shader_cache        (this one)
    //
    // The rasterizer does DirAccess::open("user://"), then change_dir(
    // "shader_cache"), and only calls make_dir if that fails. If the directory
    // is already there, change_dir succeeds and caching is enabled without the
    // engine having to create anything.
    //
    // Without it: "Can't create shader cache folder, no shader caching will
    // happen" on every launch, and every shader recompiled from source every
    // time -- which is the multi-second hitching when a new effect first
    // appears. This was identified in RUN_22 and then not acted on.
    snprintf(gd_cache, sizeof(gd_cache), "%s/shader_cache", config.save_root);
    mkdir(gd_cache, 0777);

    // And where user:// ACTUALLY lands while the engine's data dir is empty.
    //
    // ACCESS_USERDATA uses DirAccessUnix, not the JNI handler, so it never gets
    // save_root prepended: user://shader_cache resolves to a bare
    // "/shader_cache". Creating that too costs one directory and covers the
    // case until the data dir itself is fixed.
    // ONLY under GLES3, and clean up after the versions that did it always.
    //
    // These three live at the FILESYSTEM ROOT of the SD card -- /shader_cache
    // and /.godot/shader_cache, next to /switch and /Nintendo -- because
    // ACCESS_USERDATA uses DirAccessUnix and never gets save_root prepended.
    // That is a real GLES3 problem and this is a real fix for it.
    //
    // It is also dead weight under Vulkan, which is the default and the only
    // renderer this port ships. The GLES3 shader cache is never written, so
    // what the card gets is two empty directories in its root that no user put
    // there and nothing ever fills.
    //
    // rmdir only succeeds on an empty directory, so the cleanup cannot destroy
    // a cache that turned out to be in use -- including one left by a genuine
    // GLES3 run on the same card.
    if (!config.enable_vulkan) {
      mkdir("/shader_cache", 0777);
      mkdir("/.godot", 0777);
      mkdir("/.godot/shader_cache", 0777);
      debugPrintf("[gl] user:// fallback dirs: /shader_cache (%s), "
                  "/.godot/shader_cache (%s)\n",
                  is_dir("/shader_cache") ? "ok" : "FAILED",
                  is_dir("/.godot/shader_cache") ? "ok" : "FAILED");
    } else if (is_dir("/shader_cache") || is_dir("/.godot/shader_cache")) {
      const int a1 = rmdir("/.godot/shader_cache") == 0;
      const int a2 = rmdir("/.godot") == 0;
      const int a3 = rmdir("/shader_cache") == 0;
      debugPrintf("[gl] removed empty GLES3 fallback dirs from the SD root "
                  "(.godot/shader_cache %s, .godot %s, shader_cache %s)\n",
                  a1 ? "gone" : "kept", a2 ? "gone" : "kept",
                  a3 ? "gone" : "kept");
    }

    debugPrintf("[gl] godot shader cache dirs: %s/.godot/shader_cache, "
                "%s/shader_cache (%s)\n",
                config.save_root, config.save_root,
                is_dir(gd_cache) ? "both present" : "*** second one MISSING ***");
    debugPrintf("[gl] godot shader cache: %s\n", gd_cache);

    // Report how warm the caches are.
    //
    // Godot 4.5 has no way to precompile everything at boot -- the only
    // settings are pipeline_cache/enable and shader_compiler/shader_cache/*,
    // and neither is a "bake it all now" pass. The caches ARE the mechanism:
    // each shader variant and each pipeline compiles once EVER and then loads.
    //
    // Which makes one question worth answering at a glance: are they actually
    // persisting? Godot prints "Startup PSO cache (0.0 MiB)" whether the cache
    // is missing, empty, or was rejected for a UUID mismatch -- and 0.0 on
    // every boot means every pipeline is being rebuilt every session, which
    // looks exactly like "the stutter never gets better".
    {
      char p[352];
      struct stat st;
      long long shader_bytes = 0;
      int shader_files = 0;

      // Godot's RD shader cache: one directory per shader, files inside.
      DIR *d = opendir(gd_cache);
      if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
          if (e->d_name[0] == '.') continue;
          char sub[352];
          snprintf(sub, sizeof(sub), "%s/%s", gd_cache, e->d_name);
          DIR *d2 = opendir(sub);
          if (!d2) continue;
          struct dirent *e2;
          while ((e2 = readdir(d2))) {
            if (e2->d_name[0] == '.') continue;
            char f[704];
            snprintf(f, sizeof(f), "%s/%s", sub, e2->d_name);
            if (stat(f, &st) == 0 && S_ISREG(st.st_mode)) {
              shader_bytes += (long long)st.st_size;
              shader_files++;
            }
          }
          closedir(d2);
        }
        closedir(d);
      }

      debugPrintf("[cache] shader variants: %d file(s), %lld KB in %s\n",
                  shader_files, shader_bytes / 1024, gd_cache);

      // Godot's Vulkan pipeline cache: user://vulkan/pipelines.<uuid>.<hash>
      snprintf(p, sizeof(p), "%s/vulkan", config.save_root);
      long long pso_bytes = 0;
      int pso_files = 0;
      DIR *dv = opendir(p);
      if (dv) {
        struct dirent *e;
        while ((e = readdir(dv))) {
          if (e->d_name[0] == '.') continue;
          char f[704];
          snprintf(f, sizeof(f), "%s/%s", p, e->d_name);
          if (stat(f, &st) == 0 && S_ISREG(st.st_mode)) {
            pso_bytes += (long long)st.st_size;
            pso_files++;
          }
        }
        closedir(dv);
      }

      if (pso_files)
        debugPrintf("[cache] pipelines: %d file(s), %lld KB in %s\n",
                    pso_files, pso_bytes / 1024, p);
      else
        debugPrintf("[cache] pipelines: NONE in %s -- every pipeline will be "
                    "rebuilt this session.\n"
                    "[cache] If Godot then reports \"Startup PSO cache (0.0 "
                    "MiB)\" on the NEXT boot too,\n"
                    "[cache] the cache is not persisting and the stutter can "
                    "never thin out.\n", p);
    }

    // The path GDMono asks for on Android, verbatim from run 2's log:
    //   dirExists(2, "/.godot/mono/publish/arm64")
    //     -> "<save_root>/.godot/mono/publish/arm64" = 0
    // DirAccess::exists on it is what gates ".NET: Assemblies not found", and
    // Godot will not create the tree itself. Creating it makes the existing
    // code path's precondition true without changing which branch runs.
    //
    // It stays EMPTY. Nothing here needs to be in it -- the payload is
    // libsts2.so in data_root, loaded through dlopen, not from this directory.
    char mono_dir[352];
    snprintf(mono_dir, sizeof(mono_dir), "%s/.godot/mono", config.save_root);
    mkdir(mono_dir, 0777);
    snprintf(mono_dir, sizeof(mono_dir), "%s/.godot/mono/publish", config.save_root);
    mkdir(mono_dir, 0777);
    snprintf(mono_dir, sizeof(mono_dir), "%s/.godot/mono/publish/arm64", config.save_root);
    mkdir(mono_dir, 0777);
    debugPrintf("[dotnet] assemblies dir created: %s\n", mono_dir);

    // mkdir does NOT create parents.
    //
    // This was a single mkdir("<save>/cache/mesa"), which fails with ENOENT
    // whenever <save>/cache does not already exist -- so on a fresh card the
    // directory was never created, mesa could not write to it, and the disk
    // cache silently did nothing. Every shader was then recompiled from source
    // on every launch, which is the multi-second hitching.
    //
    // Nothing said so: the return value was discarded and the path was logged
    // as though it existed. The mono directory a few lines up creates its
    // parents step by step; this one did not.
    char shader_cache[352];
    snprintf(shader_cache, sizeof(shader_cache), "%s/cache", config.save_root);
    mkdir(shader_cache, 0777);
    snprintf(shader_cache, sizeof(shader_cache), "%s/cache/mesa", config.save_root);
    mkdir(shader_cache, 0777);

    // Verified, not assumed. A cache directory that is not there is the
    // difference between compiling shaders once and compiling them every run.
    const int cache_ok = is_dir(shader_cache);

    setenv("MESA_SHADER_CACHE_DIR", shader_cache, 1);
    setenv("MESA_GLSL_CACHE_DIR",   shader_cache, 1);

    // NVK_DEBUG, passed straight through from config.txt.
    //
    // The crash that keeps recurring is a GPU hang, not a CPU one:
    //
    //   [watchdog] game thread has not completed a step in 5 s (steps 6374)
    //   nvkmd-switch: channel 2 lost: notification={type=8 info=0 status=65535}
    //   VK_ERROR_DEVICE_LOST -> Godot CRASH_NOW (brk #1)
    //
    // type 8 is nvgpu's FIFO_ERROR_IDLE_TIMEOUT: the channel failed to go idle
    // within its timeout. The CPU stall is the SYMPTOM -- the game thread is
    // waiting on a fence the GPU will never signal -- not the cause.
    //
    // VK_EXT_device_fault is not implemented by NVK, so Godot cannot ask where
    // the GPU faulted. These options are the remaining way in, and they are
    // the driver's own rather than anything this port has to build:
    //
    //   push_sync     synchronise after every submit. Ruinous for framerate,
    //                 and exactly right here: the hang stops being "somewhere
    //                 in the last N submits" and becomes one identified submit.
    //   push_dump     dump the pushbuffer -- pair with push_sync once the
    //                 submit is known, not before.
    //   zero_memory   zero every allocation. If the hang depends on stale GPU
    //                 memory contents, this changes it; if it does not, that
    //                 is informative too.
    //   vm            VM/mapping debug.
    //   no_cbuf       disable constant buffers.
    //
    // Empty by default: push_sync alone would make the game unplayable, and a
    // diagnostic left on by accident is how the last three rounds went wrong.
    // The AOT payload's ParticlePatches reads this once. It is an environment
    // variable rather than a baked-in constant so that ONE Docker build serves
    // both behaviours, and the A/B between "mitigated" and "reproduce the
    // hang" is a config.txt edit -- the same reasoning as NVK_DEBUG below.
    setenv("STS2_GPU_PARTICLES", config.gpu_particles ? "1" : "0", 1);
    if (!config.gpu_particles)
      debugPrintf(">> GPU particle emission suppressed (gpu_particles 0); the\n"
                  ">> channel-loss mitigation. CPU particles unaffected.\n");

    if (config.nvk_debug[0]) {
      setenv("NVK_DEBUG", config.nvk_debug, 1);
      debugPrintf("== NVK_DEBUG=%s (diagnostic; expect it to be slow) ==\n",
                  config.nvk_debug);
    }

    // NVK_SWITCH_* -- CPU/GPU cache coherency on this console specifically.
    //
    // These exist because the Switch GPFIFO is a known hazard, and reading
    // libdrm_nouveau's Switch port makes plain why. On EVERY submit it appends
    // three things, not one:
    //
    //   1. the fence cmdlist, with "bit16 = gpu cache flush"
    //   2. a cache-flush cmdlist
    //   3. "a dummy NOP cmdlist with NO_PREFETCH set (used as a barrier), to
    //      make sure that all further submitted cmdlists see the effects of
    //      the previous cache flushing cmdlist"
    //
    // The GPFIFO PREFETCHES. Without that barrier, later command lists can be
    // fetched before an earlier flush lands, and the GPU executes against
    // stale memory. That is not a crash -- it is a hang, because the engine
    // sits waiting on work that will never retire.
    //
    // Which is what this port keeps hitting: ~6000 frames of correct
    // rendering, then FIFO_ERROR_IDLE_TIMEOUT. A wrong shader fails the first
    // time it runs; a coherency race fails when the timing finally lines up.
    //
    // NVK does not use libdrm_nouveau, so none of that code runs here -- but
    // the hardware behaviour it works around is the same, and NVK ships its
    // own switches for it:
    //
    //   cmd_mem_uncached    command memory mapped CPU-uncached, so a missed
    //                       flush cannot leave the GPU reading stale commands
    //   mem_stream_uncached  same for streaming/upload memory
    //   mapped_completion    completion records read through a mapping rather
    //                        than cached CPU memory
    //
    // Unlike push_sync these are plausible FIXES rather than instruments: if
    // the hang is a coherency race, uncached mappings remove it. They cost
    // bandwidth, not correctness, so leaving one on is survivable if it works.
    if (config.nvk_cmd_mem_uncached)
      setenv("NVK_SWITCH_CMD_MEM_CPU_UNCACHED", "1", 1);
    if (config.nvk_mem_stream_uncached)
      setenv("NVK_SWITCH_MEM_STREAM_CPU_UNCACHED", "1", 1);
    if (config.nvk_mapped_completion)
      setenv("NVK_SWITCH_MAPPED_COMPLETION", "1", 1);
    if (config.nvk_cmd_mem_uncached || config.nvk_mem_stream_uncached ||
        config.nvk_mapped_completion)
      debugPrintf("== NVK coherency: cmd_mem_uncached=%d mem_stream_uncached=%d "
                  "mapped_completion=%d ==\n",
                  config.nvk_cmd_mem_uncached, config.nvk_mem_stream_uncached,
                  config.nvk_mapped_completion);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "256M", 1);
    // Some mesa builds read the disable flag before the dir; clear it so a
    // stale environment cannot switch the cache off behind us.
    unsetenv("MESA_SHADER_CACHE_DISABLE");
    unsetenv("MESA_GLSL_CACHE_DISABLE");

    debugPrintf("[gl] mesa shader cache: %s (%s)\n", shader_cache,
                cache_ok ? "directory present"
                         : "*** DIRECTORY MISSING -- shaders will recompile every launch ***");
  }

  set_screen_size(config.screen_width, config.screen_height);

  // Before any engine thread exists: nxp_init() reads cursor.png and
  // pointer.cfg off the SD card, and devkitPro's file handle table is not
  // thread-safe next to the engine's workers.
  nx_input_init();

  // EGL is created ONLY when it will actually be used.
  //
  // There is one default NWindow (nwindowGetDefault()), and under Vulkan the
  // engine's swapchain presents to it through wsi_switch. An EGL window
  // surface on the same window is a second owner: it holds its own buffers and
  // posts them on every eglSwapBuffers, overwriting whatever Vulkan just
  // presented.
  //
  // That is exactly what the first Vulkan run looked like -- audio, input,
  // shader compilation and surface creation all correct, and a black screen.
  //
  // Gating only the swap would not be enough: eglCreateWindowSurface itself
  // takes buffers from the window, so under Vulkan the surface must never
  // exist.
  // Before anything asks Mesa to compile a shader.
  //
  // nvkmd-switch checks for mtx_init/cnd_init/cnd_broadcast/cnd_timedwait
  // failing, by name, in its own error strings -- so those are reachable on
  // this platform in someone's experience. Finding out here costs a few
  // milliseconds; finding out from a hang during shader compilation has cost
  // considerably more than that.
  sts2_c11_probe();

  // The bake owns the screen, so it must finish before the renderer takes it.
  //
  // startup_status uses consoleInit(NULL), which claims nwindowGetDefault().
  // The very next line hands that same window to either the Vulkan swapchain
  // or egl_setup(). There is exactly one such window, so the console has to be
  // opened and released inside this call, before either owner exists.
  //
  // Built from data_root + PCK_NAME the same way main.c:686 does it, so this
  // and check_data() cannot disagree about which file the game uses -- they
  // did once already, "game.pck" against PCK_NAME.
  {
    char bake_pck[512];
    snprintf(bake_pck, sizeof(bake_pck), "%s/%s", config.data_root, PCK_NAME);
    astc_bake_run(bake_pck);
  }

  s_use_vulkan = config.enable_vulkan && vulkan_shim_probe();
  if (!s_use_vulkan) {
    if (sts2_is_emulator()) {
      // Disable Mesa's background glthread and Gallium threaded_context workers
      // on emulators: avoids async unmarshal/u_vbuf races in
      // glDrawElementsInstancedBaseVertex and cuts thread contention across
      // emulated Switch cores. Real Switch stays on Vulkan (s_use_vulkan=1).
      setenv("mesa_glthread", "false", 1);
      setenv("MESA_GLTHREAD", "0", 1);
      setenv("MESA_SWITCH_GLTHREAD", "0", 1);
      setenv("GALLIUM_THREAD", "0", 1);
    }
    if (egl_setup() != 0)
      fatal_error("Could not create the EGL/GLES3 context.");
    s_egl_owns_window = 1;
  } else {
    // Configure the NWindow ourselves.
    //
    // egl_setup() used to be the only caller of nwindowSetDimensions (it is
    // two lines above eglCreateWindowSurface), so skipping EGL for Vulkan left
    // the window completely unconfigured -- no dimensions, no crop, no swap
    // interval. wsi_switch then built a swapchain against it and every
    // vkQueuePresentKHR failed:
    //
    //   ERROR: QueuePresentKHR failed with error: -3
    //      at: command_queue_execute_and_present
    //          (drivers/vulkan/rendering_device_driver_vulkan.cpp:2842)
    //
    // -3 is VK_ERROR_INITIALIZATION_FAILED. Godot retries presentation every
    // frame, which is the 100% CPU on a black screen: the renderer was working
    // and nothing could ever reach the display.
    //
    // The three calls are from MarathonRecomp-NX's game_window.cpp, which runs
    // Vulkan on this hardware and does exactly this before touching the
    // surface. Crop matters as much as dimensions -- an unset crop is not
    // implicitly the full surface.
    NWindow *win = nwindowGetDefault();
    if (!win) fatal_error("nwindowGetDefault() returned NULL");

    nwindowSetDimensions(win, screen_width, screen_height);
    nwindowSetCrop(win, 0, 0, screen_width, screen_height);
    nwindowSetSwapInterval(win, 1);   // vsync; EGL got this from eglSwapInterval

    // Adopt whatever the window actually became. Asking for 1280x720 does not
    // guarantee it, and a swapchain built for a size the window does not have
    // is the failure being fixed here.
    u32 nw = 0, nh = 0;
    if (R_SUCCEEDED(nwindowGetDimensions(win, &nw, &nh)) && nw && nh) {
      if ((int)nw != screen_width || (int)nh != screen_height)
        debugPrintf("== NWindow returned %ux%u, not %dx%d -- adopting it ==\n",
                    nw, nh, screen_width, screen_height);
      screen_width  = (int)nw;
      screen_height = (int)nh;
    }
    debugPrintf("== NWindow configured for Vulkan: %dx%d, crop 0,0,%d,%d, swap 1 ==\n",
                screen_width, screen_height, screen_width, screen_height);
  }

  debugPrintf("== Slay the Spire 2 Switch wrapper booting; build " __DATE__ " " __TIME__ "; data_root=%s ==\n", config.data_root);
  {
    const char *rend = s_egl_owns_window ? "GLES3" : "Vulkan";
    const char *env  = sts2_is_emulator() ? "emulator" : "Switch";
    const char *mem  = (sts2_own_process_handle() != INVALID_HANDLE)
                         ? "ProcessCodeMemory" : "CodeMemory";
    char rbuf[96];
    snprintf(rbuf, sizeof(rbuf), "renderer: %s (%s, %s)", rend, env, mem);
    if (s_egl_owns_window)
      debugPrintf("== EGL/GLES3 context created (%dx%d) [%s, %s] ==\n",
                  screen_width, screen_height, env, mem);
    else
      debugPrintf("== EGL/GLES3 NOT created: Vulkan owns the default NWindow (%dx%d) [%s, %s] ==\n",
                  screen_width, screen_height, env, mem);
    stats_mark(rbuf);
  }

  // Claim the GPU pool before any module loads -- OPTIONAL, and off by default.
  //
  // The reasoning was that the pool needs one contiguous run and so_load is
  // about to churn the heap with ~130 MB of modules and transients, so taking
  // it from a fresh heap should be strictly better.
  //
  // It is not obviously so in practice. Run 31's sweep has this active (the
  // "[gpua] pool" line appears at log line 13, before the first so_load at 17)
  // and every non-zero size died before the menu -- whereas 768 MB booted and
  // played back when the pool was created lazily. That is suggestive, not
  // conclusive, because other things changed in between.
  //
  // So it is a config flag rather than a decision: `gpu_arena_early 1` claims
  // it up front, 0 leaves it lazy as it was. One build tests both.
  if (config.gpu_arena_early) gpu_arena_preinit();

  // libc++ first so libgodot's C++ ABI imports resolve against it
  load_module(&cxx_mod, CXX_SO_NAME, heap_so_base, CXX_SO_SLICE);
  void *game_base = (char *)heap_so_base + CXX_SO_SLICE;
  size_t game_slice = heap_so_limit - CXX_SO_SLICE - DOTNET_SO_SLICE - GDEXT_SO_SLICE;
  load_module(&game_mod, SO_NAME, game_base, game_slice);
  // StS2.so is NOT loaded here: it's loaded lazily, the first time the
  // engine itself calls dlopen("libStS2.so") during its own boot
  // (GDMono::try_load_native_aot_library) -- see sts2_load_dotnet_module
  // and egl_shim.c's dlopen_fake. Just reserve its slice now, while
  // heap_so_base/heap_so_limit are known; so_finalize on cxx_mod/game_mod
  // below doesn't touch memory past their own slices, so this is safe to
  // leave unused until then.
  dotnet_so_base = (char *)game_base + game_slice;

  // Same reasoning for the GDExtensions: reserve now, while heap_so_base and
  // heap_so_limit are known, and hand the region to fmod_shim.c to sub-allocate
  // when the engine actually asks for a module.
  sts2_gdext_set_region((char *)dotnet_so_base + DOTNET_SO_SLICE, GDEXT_SO_SLICE);

  debugPrintf("== so heap: cxx %uMB | engine %uMB | dotnet %uMB | gdext %uMB "
              "(reserve %uMB) ==\n",
              (unsigned)(CXX_SO_SLICE >> 20), (unsigned)(game_slice >> 20),
              (unsigned)(DOTNET_SO_SLICE >> 20), (unsigned)(GDEXT_SO_SLICE >> 20),
              (unsigned)(heap_so_limit >> 20));

  // Must run before the engine's imports resolve: registers the AAudio table
  // that dlsym_fake serves to FMOD, and the org.fmod.* JNI surface.
  sts2_register_audio_shims();

  sts2_resolve_imports(&cxx_mod);
  sts2_resolve_imports(&game_mod);
  debugPrintf("== imports resolved ==\n");
  so_patch(&game_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();
  // same window, same reason: jni_helpers.c's dialog callbacks and the input
  // entry points have to be captured while game_mod's symbol tables are valid
  sts2_cache_engine_symbols();

  so_finalize(&cxx_mod);
  so_flush_caches(&cxx_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  debugPrintf("== so_finalize ok; running init arrays ==\n");

  jni_init();
  tls_setup_guard();
  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cxx_mod);
  so_free_temp(&game_mod);
  debugPrintf("== init arrays done ==\n");
  stats_mark("modules loaded + init arrays");

  // the game sees cwd="/" (getcwd_fake) and stray absolute writes are rebased
  // into save_root (sandbox_path); move the REAL cwd there too so any genuine
  // relative libc paths agree. The .so files were already loaded above.
  if (chdir(config.save_root) != 0)
    debugPrintf("!! chdir(%s) failed\n", config.save_root);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  if (R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2)))
    fatal_error("Could not create the game thread.");
  threadStart(&s_game_thread);

  int last_frames = -1;
  int stall_ms = 0;
  int warned = 0;   // one 5 s warning per stall, not one per tick
  while (appletMainLoop() && s_game_running) {
    AppletFocusState fs = appletGetFocusState();
    const int was_focused = s_focused;
    s_focused = (fs == AppletFocusState_InFocus);

    // On a focus transition, silence the mixer and stop the motors. An
    // unpaused FMOD mixer across a sleep transition is the classic source of a
    // burst of stale audio on wake, and a running rumble would otherwise
    // persist while the console is asleep.
    if (s_focused != was_focused) {
      audio_sink_set_paused(!s_focused);
      if (!s_focused) jni_rumble_stop();
      // Docking changes the render space; keep the cursor's clamp in step.
      // The pointer keeps its relative position across the change.
      nx_input_set_screen(screen_width, screen_height);
    }

    // Software keyboard. Runs HERE, on the appletMainLoop thread, because
    // swkbdShow blocks and the OS suspends the process while it is up --
    // doing that from the engine thread would suspend mid-frame with the EGL
    // context current. Gated on focus: a library applet transition from a
    // backgrounded process is refused.
    if (s_focused && swkbd_is_pending()) {
      // The watchdog measures wall-clock silence from the game thread, and
      // the keyboard can legitimately be up for a minute. Reset the stall
      // counter across the transition so this is not mistaken for a hang.
      swkbd_pump();
      stall_ms = 0;
      last_frames = s_frames_done;
      warned = 0;   // or the first real stall after a keyboard session is silent
    }

    // Hang watchdog.
    //
    // Run 33 hung with no GPU and minimal CPU -- a deadlock -- and this
    // produced nothing. Three reasons, all fixed here:
    //
    //  * 45 s is longer than anyone waits before pulling the power, and the
    //    dump is the only evidence a deadlock ever leaves.
    //  * it was gated on s_focused, so pressing HOME to look at the console
    //    silenced the very tool that was meant to explain the freeze.
    //  * eager flushing is turned off after frame 8, so even when it did fire
    //    the output sat in a buffer that a stuck process never flushes. Run
    //    33's log ends mid-word ("[pthread_mute") for exactly that reason.
    //
    // Now: a flushed warning at 5 s, a full dump at 15 s, and repeat dumps
    // every 15 s after that, because a second dump showing the same PC is what
    // distinguishes a deadlock from something merely slow.
    if (s_frames_done != last_frames) {
      last_frames = s_frames_done;
      stall_ms = 0;
      warned = 0;
    } else {
      stall_ms += 16;

      if (!warned && stall_ms >= 5000) {
        warned = 1;
        debugPrintf("[watchdog] game thread has not completed a step in 5 s "
                    "(steps done: %d, focused: %d)%s\n",
                    s_frames_done, s_focused,
                    config.watchdog_stall_s <= 0
                      ? " -- dump disabled (watchdog_stall_s 0)"
                      : (s_frames_done < (config.watchdog_arm_steps > 0
                                            ? config.watchdog_arm_steps
                                            : WATCHDOG_ARM_STEPS)
                         ? " -- still starting up, no dump"
                         : " -- dumping shortly"));
        debugFlush();
      }

      // NOT ARMED during startup.
      //
      // Run 34 stalled at "steps done: 2" -- inside the first loading pass,
      // where a single step legitimately runs for tens of seconds. Godot loads
      // resources within the step, and with the ETC2 background transcode
      // thread unavailable ("Cache miss (no bg)", 220 of them) each texture is
      // decoded synchronously on that thread.
      //
      // So the stall was real and also completely normal, and dumping on it
      // deadlocked the game. A watchdog that fires during legitimate work is
      // worse than none: it converts slow into dead.
      //
      // Warnings still print during startup, so a genuine early hang is
      // visible; only the pause-and-dump waits until steps are frames.
      // watchdog_stall_s 0 = no pause, no dump. The warning above still fires.
      // WATCHDOG_ARM_STEPS is now a floor, not a constant.
      //
      // The reasoning above is right and stays the default: a dump during
      // legitimate loading converts slow into dead. But 120 steps means a hang
      // during STARTUP can never be dumped, and that is exactly where this
      // port keeps stalling -- "steps done: 2", every time.
      //
      // config.watchdog_arm_steps lets it be lowered deliberately, for one
      // run, to find out where. It is the same trade the comment describes,
      // taken knowingly: the game is already not progressing, so a dump that
      // deadlocks it costs nothing that is not already lost.
      const int arm = config.watchdog_arm_steps > 0 ? config.watchdog_arm_steps
                                                    : WATCHDOG_ARM_STEPS;
      // ONE dump per session, not one per stall interval.
      //
      // Three dumps fired ten seconds apart on the last run, each pausing
      // every thread, and immediately after the third:
      //
      //   nvkmd-switch: channel 2 lost: notification={type=8 ...}
      //
      // type 8 is FIFO_ERROR_IDLE_TIMEOUT -- the GPU channel did not go idle
      // within its timeout. Pausing the submitting threads for ~30 seconds
      // with work queued is a good way to cause exactly that, so the watchdog
      // very likely killed the device it was there to observe.
      //
      // The first dump has all the evidence; the second and third only add
      // risk. A repeat stall still logs, it just does not pause anything.
      static int s_dumped;
      if (config.watchdog_stall_s > 0 &&
          !s_dumped &&
          s_frames_done >= arm &&
          stall_ms >= config.watchdog_stall_s * 1000) {
        debugPrintf("[watchdog] STALLED %d s (steps done: %d, focused: %d)\n",
                    stall_ms / 1000, s_frames_done, s_focused);
        s_dumped = 1;
      sts2_dump_all_threads();
      debugPrintf("[watchdog] that was the ONLY pause-and-dump this session.\n"
                  "[watchdog] Pausing threads starves the GPU channel, which\n"
                  "[watchdog] times out (FIFO_ERROR_IDLE_TIMEOUT) and takes the\n"
                  "[watchdog] Vulkan device with it. Later stalls log only.\n");
        // Flushed immediately: a deadlocked process never reaches the periodic
        // flush, and an unflushed dump is the same as no dump.
        debugFlush();
        stall_ms = 5000;   // re-dump on the same interval, without re-warning
      }
    }
    svcSleepThread(16 * 1000 * 1000);
  }

  s_game_running = 0;
  threadWaitForExit(&s_game_thread);
  threadClose(&s_game_thread);

  nx_input_shutdown();   // flush any queued pointer.cfg write

  if (s_ctx != EGL_NO_CONTEXT) {
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);
  }

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
