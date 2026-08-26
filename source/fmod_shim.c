/* fmod_shim.c -- see fmod_shim.h. MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "imports.h"
#include "audio_sink.h"
#include "jni_fake.h"
#include "aaudio_shim.h"
#include "fmod_shim.h"

#if VERBOSE_AUDIO && DEBUG_LOG
#define fmLog(...) debugPrintf("[fmod] " __VA_ARGS__)
#else
#define fmLog(...) do {} while (0)
#endif

so_module fmod_mod;          // libGodotFmod.*.so   (GDExtension)
so_module fmod_core_mod;     // libfmod.so
so_module fmod_studio_mod;   // libfmodstudio.so
so_module spine_mod;         // libspine_godot.*.so

static int s_fmod_loaded  = 0;
static int s_spine_loaded = 0;

// Slice sizes, measured from the real binaries (readelf -lW, LOAD vaddr span
// rounded up to 1 MB) rather than guessed. The first version of this file
// reserved 40 MB for what actually spans 9 MB.
//
//   libfmod.so         2 MB      libGodotFmod.so   2 MB
//   libfmodstudio.so   2 MB      libspine_godot.so 3 MB
//
// Doubled for version headroom; the total still fits GDEXT_SO_SLICE with room
// to spare. so_util needs a contiguous reservation up front and cannot grow it.
#define FMOD_CORE_SLICE   (4 * 1024 * 1024)
#define FMOD_STUDIO_SLICE (4 * 1024 * 1024)
#define FMOD_GDEXT_SLICE  (4 * 1024 * 1024)
#define SPINE_SLICE       (4 * 1024 * 1024)

// ---------------------------------------------------------------------------
// Bump allocator over the region main() carved out of SO_HEAP_RESERVE.
//
// NOT malloc/memalign. Every other module in this port lives in that reserve,
// and the reason matters: GDExtensions are dlopen()ed by the engine well after
// startup, from a newlib heap the engine has been allocating out of for the
// whole boot. Asking for several MB contiguous at that moment is a real
// failure risk, and the failure would present as "GDExtension not found"
// rather than as an out-of-memory.
//
// Nothing is ever freed. The modules live for the process lifetime, and a
// bump pointer cannot fragment.
// ---------------------------------------------------------------------------

static char  *s_gdext_base = NULL;
static size_t s_gdext_size = 0;
static size_t s_gdext_used = 0;

void sts2_gdext_set_region(void *base, size_t size) {
  s_gdext_base = (char *)base;
  s_gdext_size = size;
  s_gdext_used = 0;
}

static void *gdext_alloc(const char *what, size_t size) {
  if (!s_gdext_base) {
    debugPrintf("[fmod] gdext region not set -- sts2_gdext_set_region() must be "
                "called from main() before any module loads\n");
    return NULL;
  }
  size = ALIGN_MEM(size, 0x1000);
  if (s_gdext_used + size > s_gdext_size) {
    debugPrintf("[fmod] gdext region exhausted loading %s: need %uKB, "
                "%uKB of %uKB left. Raise GDEXT_SO_SLICE in main.c.\n",
                what, (unsigned)(size >> 10),
                (unsigned)((s_gdext_size - s_gdext_used) >> 10),
                (unsigned)(s_gdext_size >> 10));
    return NULL;
  }
  void *p = s_gdext_base + s_gdext_used;
  s_gdext_used += size;
  return p;
}

// ---------------------------------------------------------------------------
// Staging.
//
// The order below is not arbitrary, and an earlier version of this file got it
// wrong in two ways that fail differently. It mirrors main.c's handling of
// cxx_mod/game_mod:
//
//   1. load + relocate every module in the group
//   2. resolve imports for every module
//        so_resolve_symbol walks so_list and so_lookup_export reads mod->syms
//        / mod->dynstrtab, which point into load_base and are INVALID once
//        that module is finalized. So every module in a dependency group must
//        be resolved before ANY of them is finalized. libGodotFmod's entire
//        undefined list is FMOD::Studio::* out of libfmod/libfmodstudio, so
//        this is exactly the case that matters.
//   3. cache the exports needed later, for the same reason
//   4. finalize + flush caches
//        so_finalize is what calls svcMapProcessCodeMemory. Before it runs,
//        the module's text is NOT executable.
//   5. run init arrays
//        which execute code, so they must come after 4. Running them before
//        finalize -- which the first version of this file did -- means jumping
//        into non-executable pages.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Locate a module by PREFIX, not by exact filename.
//
// The GDExtension names are long and dotted:
//
//   libGodotFmod.android.template_release.arm64.so
//
// and they do not survive every route onto an SD card intact. Upload forms,
// some archive tools and a few file managers rewrite '.' to '_', giving
// libGodotFmod_android_template_release_arm64.so -- byte-identical file, name
// the loader would not find. The template suffix also changes with the Godot
// version it was exported from, so pinning the whole string makes the port
// brittle against a rebuild of the launcher.
//
// So: try the exact name first, then fall back to the first *.so in data_root
// whose basename starts with the prefix before the first '.'. That is
// "libGodotFmod" / "libspine_godot", which is specific enough not to collide
// with anything else in the directory.
//
// This affects only the ON-DISK lookup. dlopen_fake matches the path the
// ENGINE asks for, which comes from the .gdextension inside the .pck and is
// always the canonical dotted name -- the two are independent.
// ---------------------------------------------------------------------------

static int find_module_file(const char *soname, char *out, size_t sz) {
  struct stat st;

  snprintf(out, sz, "%s/%s", config.data_root, soname);
  if (stat(out, &st) == 0) return 1;

  // prefix = up to the first '.', e.g. "libGodotFmod"
  char prefix[64];
  size_t n = 0;
  while (soname[n] && soname[n] != '.' && n < sizeof(prefix) - 1) {
    prefix[n] = soname[n];
    n++;
  }
  prefix[n] = '\0';

  DIR *d = opendir(config.data_root);
  if (!d) return 0;

  int found = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    if (len < 4 || strcmp(e->d_name + len - 3, ".so") != 0) continue;
    if (strncmp(e->d_name, prefix, n) != 0) continue;

    snprintf(out, sz, "%s/%s", config.data_root, e->d_name);
    debugPrintf("[fmod] %s not found; using %s (prefix match on \"%s\")\n",
                soname, e->d_name, prefix);
    found = 1;
    break;
  }
  closedir(d);
  return found;
}

static int stage_load(const char *soname, so_module *mod, size_t slice) {
  char path[512];
  if (!find_module_file(soname, path, sizeof(path))) {
    fmLog("no file matching %s in %s\n", soname, config.data_root);
    return -1;
  }

  void *base = gdext_alloc(soname, slice);
  if (!base) return -1;

  if (so_load(mod, path, base, slice) < 0) {
    fmLog("so_load failed: %s\n", path);
    return -1;   // the bump stays consumed; a failed load is fatal anyway
  }
  if (so_relocate(mod) < 0) {
    fmLog("so_relocate failed: %s\n", soname);
    return -1;
  }

  fmLog("staged %s at %p (%uKB slice)\n", soname, mod->load_virtbase,
        (unsigned)(slice >> 10));
  return 0;
}

static void stage_finalize(so_module *mod) {
  so_finalize(mod);
  so_flush_caches(mod);
}

// ---------------------------------------------------------------------------
// Entry symbols -- CONFIRMED, not guessed.
//
// Read straight out of the shipped binaries' dynamic symbol tables:
//
//   readelf -W --dyn-syms libGodotFmod.android.template_release.arm64.so
//     | awk '$7!="UND" && $4=="FUNC"{print $8}' | grep -iE "init|library"
//
// Each exports exactly one init-shaped symbol, so there is no ambiguity. The
// engine still reads the name from the .gdextension resource in the .pck, so
// if it ever logs "GDExtension entry point not found" the .pck and the binary
// disagree -- but the binary side is now known.
// ---------------------------------------------------------------------------

static const char *fmod_entry_symbols[]  = { "fmod_library_init", NULL };
static const char *spine_entry_symbols[] = { "spine_godot_library_init", NULL };

// ---------------------------------------------------------------------------
// JNI_OnLoad.
//
// libfmod.so EXPORTS JNI_OnLoad (confirmed: readelf shows it at .text+0xe639c)
// and that is the only way it ever obtains a JavaVM*. On Android the runtime
// calls it at load time; nothing here does, so it has to be called explicitly.
//
// Without it FMOD has no VM pointer and cannot reach Java at all -- which
// means every org/fmod/* handler in jni_fmod.c is dead code and FMOD's init
// path fails somewhere less obvious. main.c already does exactly this for the
// engine module (e_JNI_OnLoad, line ~440); this is the same treatment for the
// audio group.
//
// libfmodstudio, libGodotFmod and libspine_godot do NOT export it -- checked,
// all three report 0 -- so only the core needs this.
// ---------------------------------------------------------------------------

typedef int (*jni_onload_fn)(void *vm, void *reserved);

static void call_jni_onload(so_module *mod, const char *what) {
  jni_onload_fn f = (jni_onload_fn)so_try_find_addr_rx(mod, "JNI_OnLoad");
  if (!f) return;
  int version = f(fake_vm, NULL);
  fmLog("%s JNI_OnLoad -> 0x%x\n", what, version);
}

#define SYMCACHE_MAX 32
typedef struct { char name[64]; uintptr_t addr; } SymCacheEntry;

static SymCacheEntry fmod_symcache[SYMCACHE_MAX];
static int fmod_symcache_n = 0;
static SymCacheEntry spine_symcache[SYMCACHE_MAX];
static int spine_symcache_n = 0;

// Must run BEFORE so_finalize(mod): so_try_find_addr_rx reads mod->syms and
// mod->dynstrtab, which dangle once finalize remaps load_base. Same trap
// imports.h documents for dotnet_mod; it presents as a Data Abort at a
// plausible-looking address rather than as a null.
static void cache_symbols(so_module *mod, const char **names,
                          SymCacheEntry *cache, int *count) {
  int found = 0;
  for (int i = 0; names[i] && *count < SYMCACHE_MAX; i++) {
    uintptr_t a = so_try_find_addr_rx(mod, names[i]);
    if (a) {
      strncpy(cache[*count].name, names[i], sizeof(cache[0].name) - 1);
      cache[*count].name[sizeof(cache[0].name) - 1] = '\0';
      cache[*count].addr = a;
      (*count)++;
      found++;
      fmLog("cached entry symbol %s -> %p\n", names[i], (void *)a);
    }
  }
  if (!found)
    debugPrintf("[fmod] WARNING: no candidate entry symbol found in %s -- "
                "check the .gdextension file, see fmod_shim.c\n", mod->name);
}

int sts2_fmod_enabled(void)  { return config.enable_fmod  && FMOD_SO_NAME[0]  != '\0'; }
int sts2_spine_enabled(void) { return config.enable_spine && SPINE_SO_NAME[0] != '\0'; }

// ---------------------------------------------------------------------------

int sts2_load_fmod_module(void) {
  if (s_fmod_loaded) return 0;
  if (!sts2_fmod_enabled()) {
    fmLog("FMOD disabled by config; refusing load\n");
    return -1;
  }

  // -- 1. load + relocate the whole group -----------------------------------
  if (stage_load(FMOD_CORE_SO_NAME, &fmod_core_mod, FMOD_CORE_SLICE) < 0) {
    debugPrintf("[fmod] %s missing -- in the APK it is under assets/native/, "
                "not lib/arm64-v8a/\n", FMOD_CORE_SO_NAME);
    return -1;
  }
  if (stage_load(FMOD_STUDIO_SO_NAME, &fmod_studio_mod, FMOD_STUDIO_SLICE) < 0)
    return -1;
  if (stage_load(FMOD_SO_NAME, &fmod_mod, FMOD_GDEXT_SLICE) < 0)
    return -1;

  // -- 2. resolve, group-wide, before any finalize --------------------------
  sts2_resolve_imports(&fmod_core_mod);
  sts2_resolve_imports(&fmod_studio_mod);
  sts2_resolve_imports(&fmod_mod);

  // -- 3. cache exports while the symbol tables are still valid -------------
  cache_symbols(&fmod_mod, fmod_entry_symbols, fmod_symcache, &fmod_symcache_n);

  // -- 4. map as code -------------------------------------------------------
  stage_finalize(&fmod_core_mod);
  stage_finalize(&fmod_studio_mod);
  stage_finalize(&fmod_mod);

  // -- 5. init arrays, in dependency order ----------------------------------
  so_execute_init_array(&fmod_core_mod);
  so_execute_init_array(&fmod_studio_mod);
  so_execute_init_array(&fmod_mod);

  // -- 6. hand FMOD its JavaVM ----------------------------------------------
  // After the init arrays: JNI_OnLoad caches the pointer into state those
  // constructors set up. After finalize necessarily, since it executes code.
  call_jni_onload(&fmod_core_mod, "libfmod");

  s_fmod_loaded = 1;
  fmLog("FMOD group loaded\n");
  return 0;
}

int sts2_load_spine_module(void) {
  if (s_spine_loaded) return 0;
  if (!sts2_spine_enabled()) {
    fmLog("Spine disabled by config; refusing load\n");
    return -1;
  }

  // Spine is self-contained: NEEDED is only libc++_shared/libm/libdl/libc, and
  // its undefined list is 69 symbols, all covered by the wrapper table or
  // libc++_shared (see docs/IMPORT_GAP.md).
  if (stage_load(SPINE_SO_NAME, &spine_mod, SPINE_SLICE) < 0)
    return -1;

  sts2_resolve_imports(&spine_mod);
  cache_symbols(&spine_mod, spine_entry_symbols, spine_symcache, &spine_symcache_n);
  stage_finalize(&spine_mod);
  so_execute_init_array(&spine_mod);

  s_spine_loaded = 1;
  fmLog("Spine loaded\n");
  return 0;
}

static uintptr_t symcache_find(SymCacheEntry *cache, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (strcmp(cache[i].name, name) == 0) return cache[i].addr;
  return 0;
}

uintptr_t sts2_fmod_find_symbol(const char *name) {
  if (!s_fmod_loaded || !name) return 0;
  return symcache_find(fmod_symcache, fmod_symcache_n, name);
}

uintptr_t sts2_spine_find_symbol(const char *name) {
  if (!s_spine_loaded || !name) return 0;
  return symcache_find(spine_symcache, spine_symcache_n, name);
}

// ---------------------------------------------------------------------------
// The org.fmod.* JNI surface lives in jni_fmod.{c,h} -- three classes, derived
// from the APK rather than guessed. See docs/JNI_SURFACE.md.
// ---------------------------------------------------------------------------

void fmod_shim_register(void) {
  aaudio_shim_register();
  fmLog("FMOD shim ready (%d Hz, %d frame block); JNI in jni_fmod.c\n",
        SINK_SAMPLE_RATE, SINK_FRAMES_PER_BUF);
}
