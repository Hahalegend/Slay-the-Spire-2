/* fmod_shim.h -- FMOD and Spine GDExtension loading.
 *
 * Both ship as Android arm64 .so files. Switch is aarch64, so they load
 * through so_util exactly like libgodot_android.so does -- no rebuild, no
 * Switch-specific binary. What they need from us is an import environment:
 * libc/libm through the existing shims, AAudio or OpenSL ES for FMOD's output,
 * and (for FMOD) the org.fmod.FMOD JNI class it calls into during init.
 *
 * MIT license; see LICENSE. */

#ifndef __FMOD_SHIM_H__
#define __FMOD_SHIM_H__

#include <stdint.h>
#include <stddef.h>
#include "so_util.h"

// Hands fmod_shim the region main() carved out of SO_HEAP_RESERVE. MUST be
// called before any GDExtension loads. The modules are bump-allocated from it
// rather than malloc'd, because the engine dlopen()s them long after startup
// and a multi-MB contiguous request from the engine's own heap at that point
// is a real failure risk.
void sts2_gdext_set_region(void *base, size_t size);

extern so_module fmod_mod;
extern so_module fmod_core_mod;    // libfmod.so
extern so_module fmod_studio_mod;  // libfmodstudio.so
extern so_module spine_mod;

// Called from dlopen_fake (egl_shim.c) when the engine's GDExtension loader
// asks for a path whose basename matches FMOD_SO_NAME / SPINE_SO_NAME.
// Returns 0 on success. Loading is lazy and once-only.
int sts2_load_fmod_module(void);
int sts2_load_spine_module(void);

// Symbol lookup against the loaded GDExtension modules, for dlsym_fake.
// Reads a cache populated before so_finalize -- calling so_try_find_addr_rx
// on a finalized module is a crash, the same trap documented in imports.h
// for dotnet_mod.
uintptr_t sts2_fmod_find_symbol(const char *name);
uintptr_t sts2_spine_find_symbol(const char *name);

// 1 if the module should be loaded at all (config.enable_fmod/enable_spine).
// When 0, dlopen_fake refuses the library and the engine's GDExtension load
// fails cleanly -- which is survivable for FMOD (silent game) but not for
// Spine (no character rendering). See PORT_NOTES.md.
int sts2_fmod_enabled(void);
int sts2_spine_enabled(void);

// Registers FMOD's AAudio imports. Called once from sts2_resolve_imports.
void fmod_shim_register(void);

// The org.fmod.* JNI surface lives in jni_fmod.{c,h}.

#endif
