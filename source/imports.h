/* imports.h -- libgodot_android.so import resolution. MIT license; see LICENSE. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

void sts2_resolve_imports(so_module *mod);

// registers the FMOD/AAudio surfaces exactly once; call before loading the engine
void sts2_register_audio_shims(void);

// Resolves a symbol in the engine module (libgodot_android.so). Used by
// jni_helpers.c for the GodotLib.*Callback entry points, which have to be
// looked up lazily because jni_fake is initialised before the module loads.
// MUST be called before so_finalize(&game_mod) -- see imports.c. main.c calls
// it from resolve_entry_points().
void sts2_cache_engine_symbols(void);

// Reads the cache populated above. Returns 0 (with a log line) for anything
// not on the cached list.
uintptr_t sts2_engine_symbol(const char *name);

// look up a name in the wrapper's import table (0 if absent);
// used by dlsym_fake so dlopen'd modules see the same environment
uintptr_t sts2_find_import(const char *name);

// engine threads created through the pthread shim (for the hang watchdog)
#include <switch.h>
int sts2_engine_threads(Thread **out_thr, void **out_entry, int max);

// StS2.so (NativeAOT payload) -- lazily loaded the first time the engine
// dlopen()s it (egl_shim.c's dlopen_fake calls sts2_load_dotnet_module;
// dlsym_fake resolves symbols against dotnet_mod on success). See config.h
// DOTNET_SO_NAME and PORT_NOTES.md "Ponte GodotSharp<->engine".
extern so_module dotnet_mod;
int sts2_load_dotnet_module(void);
// resolved before so_finalize(&dotnet_mod) -- calling so_try_find_addr_rx
// on dotnet_mod AFTER it's finalized is a crash (load_base, which
// mod->syms/dynstrtab point into, is no longer valid; confirmed on hardware,
// see PORT_NOTES.md). dlsym_fake (egl_shim.c) must read this cache, not
// call so_try_find_addr_rx on dotnet_mod itself.
// What dlsym_fake hands the engine: a trampoline in main.c that logs the four
// arguments and calls through. See the comment there.
// Distinct pages committed inside the largest mmap reservation. Reported
// alongside the memory trend so the GC's reservation can be sized from a
// measured number instead of one reconstructed from log lines. See imports.c.
void sts2_report_reservation_commits(void);

// Shared lock-slot validation. A pointer we handed out is heap-resident AND
// aligned; anything else in a pthread storage slot is someone else's 32-bit
// write. Used by the mutex/cond paths in imports.c and the rwlock path in
// godot_shim.c, so both apply the same test.
int  sts2_plausible_lock_ptr(uint64_t v);
void sts2_note_bad_lock_slot(const char *what, uint64_t v);

// Same, plus the SLOT ADDRESS. Prefer this: the address is what distinguishes
// "an uninitialised static lock" from "memory that is not a lock at all and is
// about to be written into". See imports.c.
void sts2_note_bad_lock_slot_at(const char *what, uint64_t v, const void *at);
extern uint64_t sts2_bad_lock_slots;

// Largest mmap reservation, published by mmap_fake so mprotect can attribute
// commits to it. Both in imports.c.
extern uintptr_t sts2_biggest_map_base;
extern size_t    sts2_biggest_map_len;

// Pause and dump every thread -- game thread plus engine workers. Used by the
// stall watchdog and by the crash handler; see main.c.
void sts2_dump_all_threads(void);

// Shader activity, for correlating the crash against compilation. A MISS is a
// real compile; a LOAD came from cache and did none. See imports.c.
extern uint32_t sts2_shader_compiles;
extern uint32_t sts2_shader_loads;
extern uint32_t sts2_last_compile_frame;
extern uint32_t sts2_last_load_frame;

extern uintptr_t sts2_godotsharp_game_main_init;
extern uintptr_t sts2_godotsharp_game_main_init_real;

#endif
