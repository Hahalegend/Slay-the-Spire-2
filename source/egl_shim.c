/* egl_shim.c -- dlopen/dlsym bridge to mesa EGL/GLES. SDL's Android backend
 * dlopen()s libEGL.so and resolves egl* by name; we answer with mesa.
 * MIT license; see LICENSE. */

#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"      // debugPrintf
#include "egl_shim.h"
#include "aaudio_shim.h"
#include "fmod_shim.h"
#include "so_util.h"    // so_get_list, so_try_find_addr_rx (main-program dlsym)
#include "imports.h"
#include "vulkan_shim.h"

#define FAKE_DL_HANDLE ((void *)0xE61D1B)


typedef void (*generic_func)(void);
typedef struct { const char *name; generic_func fn; } EglEntry;

#define E(sym) { #sym, (generic_func)sym }

// core EGL entry points (eglGetProcAddress only returns extension/GL functions)
static const EglEntry egl_table[] = {
  E(eglGetError),
  E(eglGetDisplay),
  E(eglInitialize),
  E(eglTerminate),
  E(eglQueryString),
  E(eglGetConfigs),
  E(eglChooseConfig),
  E(eglGetConfigAttrib),
  E(eglCreateWindowSurface),
  E(eglCreatePbufferSurface),
  E(eglCreatePixmapSurface),
  E(eglDestroySurface),
  E(eglQuerySurface),
  E(eglBindAPI),
  E(eglQueryAPI),
  E(eglWaitClient),
  E(eglReleaseThread),
  E(eglCreatePbufferFromClientBuffer),
  E(eglSurfaceAttrib),
  E(eglBindTexImage),
  E(eglReleaseTexImage),
  E(eglSwapInterval),
  E(eglCreateContext),
  E(eglDestroyContext),
  E(eglMakeCurrent),
  E(eglGetCurrentContext),
  E(eglGetCurrentSurface),
  E(eglGetCurrentDisplay),
  E(eglQueryContext),
  E(eglWaitGL),
  E(eglWaitNative),
  E(eglSwapBuffers),
  E(eglCopyBuffers),
  E(eglGetProcAddress),
};

// Distinct from FAKE_DL_HANDLE: real handle to the actual loaded StS2.so
// module (so dlsym_fake below can tell the two apart and look symbols up in
// the right place). See config.h DOTNET_SO_NAME, imports.h, main.c
// sts2_load_dotnet_module -- this is the "ponte" from PORT_NOTES.md.
#define DOTNET_DL_HANDLE ((void *)&dotnet_mod)

// Match by suffix, not exact string: Godot 4.3's try_load_native_aot_library
// (no Android-specific branch there, see config.h) passes
// GodotSharpDirs::get_api_assemblies_dir().path_join("StS2.so") -- some
// directory prefix we don't replicate + the bare name. We don't care WHAT
// directory the engine thinks this should live in; we serve our own copy
// (sts2_load_dotnet_module) regardless, as long as the request is
// clearly for StS2.so and not some unrelated dlopen call.
static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && strcmp(s + (ls - lx), suffix) == 0;
}

// Asked by main.c's entry-point trampoline: what IS this handle?
//
// A description rather than a bool, because the sentinels are not equivalent
// and a yes/no answer would have been misleading. DOTNET_DL_HANDLE is
// &dotnet_mod -- a pointer to a real structure that dlsym_fake services
// properly. FAKE_DL_HANDLE is the catch-all the permissive fallback returns
// for any name dlopen_fake did not recognise, and it is the only one that
// signals a genuine problem when it turns up somewhere load-bearing.
//
// Defined here, below every #define it references. The first version sat at
// the top of the file and could not see DOTNET_DL_HANDLE, which is declared
// 50 lines further down.
const char *sts2_describe_dl_handle(const void *h) {
  if (!h)                     return "NULL";
  if (h == FAKE_DL_HANDLE)    return "FAKE_DL_HANDLE -- dlopen_fake did not "
                                     "recognise the name; this is the catch-all";
  if (h == MAIN_PROG_DL_HANDLE) return "MAIN_PROG_DL_HANDLE (dlopen(NULL); "
                                       "searches every loaded module)";
  if (h == VULKAN_DL_HANDLE)    return "VULKAN_DL_HANDLE (static Mesa NVK)";
  if (h == DOTNET_DL_HANDLE)  return "DOTNET_DL_HANDLE (&dotnet_mod, real)";
  if (h == FMOD_DL_HANDLE)    return "FMOD_DL_HANDLE";
  if (h == SPINE_DL_HANDLE)   return "SPINE_DL_HANDLE";
  if (h == AAUDIO_DL_HANDLE)  return "AAUDIO_DL_HANDLE";
  return NULL;   // not one of ours; caller resolves it against the module list
}

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;

  // dlopen(NULL) -- the MAIN PROGRAM handle.
  //
  // This is not a corner case: Godot's own gd_mono.cpp does
  //
  //     godot_dll_handle = dlopen(nullptr, RTLD_NOW);
  //
  // and passes the result to godotsharp_game_main_init, where GodotSharp wraps
  // it in GodotDllImportResolver and resolves every DllImport("__Internal")
  // through it.
  //
  // Returning FAKE_DL_HANDLE here -- which is what the catch-all at the bottom
  // of this function did -- meant those P/Invokes resolved against 0xE61D1B.
  // NativeLibrary.GetExport on that fails; if the failure lands inside a class
  // constructor it becomes a TypeInitializationException and leaves the type's
  // statics null, which is exactly the [x8, #0x40] with x8 == 0 that runs 5
  // through 7 died on.
  //
  // Confirmed on hardware by the entry-point trampoline (main.c):
  //     godot_dll_handle = 0xe61d1b   <- FAKE_DL_HANDLE
  //
  // "The main program and everything loaded into it" is what dlopen(NULL)
  // means, so this handle searches every so_util module. Whether the symbols
  // GodotSharp wants are actually there is a separate question -- this engine
  // exports no godotsharp_* at all, so the real interop is the unmanaged
  // callbacks table -- but a miss now fails visibly instead of resolving to a
  // GL function pointer.
  if (!filename) {
    debugPrintf("[dl] dlopen(NULL) -> main-program handle\n");
    return MAIN_PROG_DL_HANDLE;
  }

  // libvulkan.so: Mesa is linked STATICALLY into the .nro, so there is nothing
  // to load -- the symbols are already here. Hand back a sentinel and let
  // dlsym resolve through vulkan_shim.
  //
  // Gated on config.enable_vulkan so the GLES3 path, which currently works,
  // stays reachable without a rebuild. Refusing when disabled is what makes
  // the engine fall back to GLES instead of half-initialising Vulkan.
  if (ends_with(filename, "libvulkan.so") || ends_with(filename, "libvulkan.so.1")) {
    if (!config.enable_vulkan) {
      debugPrintf("[dl] refusing %s (enable_vulkan=0)\n", filename);
      return NULL;
    }
    debugPrintf("[dl] %s -> static Mesa Vulkan\n", filename);
    return VULKAN_DL_HANDLE;
  }

  // Log EVERY request. On the first hardware run the .NET module failed with
  // "Assemblies not found" and libsts2.so was never probed -- and because the
  // permissive fallback below answers any unknown name with a valid-looking
  // handle, a NativeAOT probe under a name other than DOTNET_SO_NAME would
  // have silently succeeded at dlopen and then failed at dlsym, invisibly.
  //
  // DOTNET_SO_NAME is a guess ("libsts2.so"); the real name comes from the
  // project's assembly name, which this port has never seen. Rather than guess
  // again, this prints what the engine actually asks for.
  debugPrintf("[dl] dlopen(\"%s\")\n", filename);

  if (ends_with(filename, DOTNET_SO_NAME))
    return sts2_load_dotnet_module() ? DOTNET_DL_HANDLE : NULL;

  // GDExtensions. The engine dlopen()s these from paths inside the .pck; match
  // by basename like the dotnet payload, so whatever directory prefix the
  // engine computed is ignored.
  if (ends_with(filename, FMOD_SO_NAME))
    return sts2_load_fmod_module() == 0 ? FMOD_DL_HANDLE : NULL;
  if (ends_with(filename, SPINE_SO_NAME))
    return sts2_load_spine_module() == 0 ? SPINE_DL_HANDLE : NULL;

  // FMOD dlopen()s its own backends by name rather than linking them
  // (confirmed: libfmod.so has no AAudio UND symbols but its string table is
  // full of AAudioStreamBuilder_*). Answering here is what makes the AAudio
  // shim reachable at all.
  //
  // Returning NULL under AUDIO_BACKEND_OPENSL is a deliberate refusal that
  // pushes FMOD onto OpenSL ES. That is safe HERE because FMOD is native C++
  // and checks the probe -- unlike the pvzultimate case, where a refused
  // libaaudio became a DllNotFoundException thrown out of managed code.
  if (ends_with(filename, "libaaudio.so"))
    return aaudio_shim_enabled() ? AAUDIO_DL_HANDLE : NULL;

  // ------------------------------------------------------------------------
  // Libraries we must REFUSE rather than pretend to have.
  //
  // The permissive default below hands back a valid-looking handle for any
  // unknown name, and dlsym_fake then returns NULL for each symbol. For a
  // library the engine only probes that is fine. For one it COMMITS to on a
  // successful dlopen it is the worst possible answer: it selects a backend
  // and then calls a null function pointer.
  //
  // libvulkan is the one that matters. StS2's engine has BOTH backends
  // compiled in -- 242 vk* symbols and a libvulkan.so dlopen, unlike
  // drmariomania_nx's engine whose Makefile notes "no Vulkan imports found".
  // The wrapper renders GLES3 through mesa/nouveau and has no Vulkan at all,
  // so Vulkan must fail at the dlopen, which is where Godot's own fallback
  // path is. main() also passes --rendering-method/--rendering-driver, and
  // this is the belt to that's braces: if the flags ever stop applying across
  // an engine update, this still keeps the failure clean and legible instead
  // of a null call inside RenderingContextDriverVulkan.
  //
  // openxr_loader: same shape, no headset, nothing to service.
  // camera2ndk / mediandk: their NDK entry points are already ret0/retm1
  //   stubs in imports.c, so a successful dlopen here would be consistent --
  //   but refusing is better, because the engine skips the whole subsystem
  //   rather than initialising one built on stubs.
  // coreclr / hostfxr / monosgen: the Mono/CoreCLR path. The AOT payload
  //   replaces it entirely; letting a probe succeed could send GDMono down
  //   the JIT branch, which cannot work here.
  static const char *refuse[] = {
    // libvulkan.so is NO LONGER refused -- see the dlopen handler below.
    // Mesa 26.2.0's Horizon build provides a real NVK + wsi_switch stack, so
    // the reason for the refusal (nothing behind it) no longer holds.
    "libopenxr_loader.so",
    "libcamera2ndk.so", "libmediandk.so",
    "libcoreclr.so", "libhostfxr.so", "libmonosgen-2.0.so",
    NULL
  };
  for (int i = 0; refuse[i]; i++) {
    if (ends_with(filename, refuse[i])) {
      debugPrintf("[dl] refusing %s (unavailable by design)\n", refuse[i]);
      return NULL;
    }
  }

  return FAKE_DL_HANDLE; // any GL/EGL library (or NULL) maps to the bridge
}

void *dlsym_fake(void *handle, const char *symbol) {
  if (!symbol)
    return NULL;

  if (handle == VULKAN_DL_HANDLE)
    return vulkan_shim_find(symbol);

  if (handle == MAIN_PROG_DL_HANDLE) {
    // Every loaded module, then the wrapper's own import table -- the honest
    // reading of "the main program and everything in it".
    //
    // so_try_find_addr_rx is safe on finalized modules now: so_finalize
    // rebases syms/dynstrtab onto load_virtbase (so_util.c). Before that fix
    // this would have been a Data Abort.
    for (so_module *m = so_get_list(); m; m = m->next) {
      uintptr_t a = so_try_find_addr_rx(m, symbol);
      if (a) return (void *)a;
    }

    uintptr_t a = sts2_find_import(symbol);
    if (a) return (void *)a;

    // Log the misses. GodotSharp resolving a P/Invoke to NULL is how a cctor
    // ends up throwing, and a named symbol here is the difference between
    // "something failed in managed startup" and knowing which import it was.
    static int misses;
    if (misses < 40) {
      misses++;
      debugPrintf("[dl] dlsym(main-program, \"%s\") -> NOT FOUND\n", symbol);
    }
    return NULL;
  }
  if (handle == DOTNET_DL_HANDLE) {
    // Do NOT call so_try_find_addr_rx(&dotnet_mod, ...) here: by the time
    // the engine calls dlsym, sts2_load_dotnet_module() has already run
    // so_finalize(&dotnet_mod), and mod->syms/dynstrtab (set from
    // mod->load_base in so_load) dangle once so_finalize remaps that
    // memory to load_virtbase as code memory -- confirmed on hardware
    // (Data Abort, fault address 0, inside so_try_find_addr_rx). The one
    // symbol dotnet_mod ever needs to answer was already resolved BEFORE
    // finalize, in sts2_load_dotnet_module (main.c); serve it from that
    // cache instead.
    if (strcmp(symbol, "godotsharp_game_main_init") == 0)
      return sts2_godotsharp_game_main_init ? (void *)sts2_godotsharp_game_main_init : NULL;
    return NULL;
  }
  // Same finalize hazard as dotnet_mod above: fmod_shim.c caches the entry
  // symbols before so_finalize and serves them from that cache.
  if (handle == FMOD_DL_HANDLE)  return (void *)sts2_fmod_find_symbol(symbol);
  if (handle == SPINE_DL_HANDLE) return (void *)sts2_spine_find_symbol(symbol);

  if (handle == AAUDIO_DL_HANDLE) {
    uintptr_t a = aaudio_shim_find(symbol);
    return a ? (void *)a : NULL;
  }

  // The NativeAOT entry point. Logged specifically because seeing it asked for
  // -- on ANY handle -- proves try_load_native_aot_library was reached, which
  // is the open question after the first run.
  if (symbol && !strcmp(symbol, "godotsharp_game_main_init"))
    debugPrintf("[dl] dlsym(godotsharp_game_main_init) on handle %p\n", handle);

  for (unsigned i = 0; i < sizeof(egl_table) / sizeof(*egl_table); i++) {
    if (strcmp(symbol, egl_table[i].name) == 0)
      return (void *)egl_table[i].fn;
  }
  // anything the wrapper already provides via the import table (Godot dlopens
  // itself/other libs and dlsyms plain libc/GL names)
  uintptr_t imp = sts2_find_import(symbol);
  if (imp)
    return (void *)imp;
  return (void *)eglGetProcAddress(symbol); // GL entry points + EGL extensions
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

char *dlerror_fake(void) { return NULL; }
