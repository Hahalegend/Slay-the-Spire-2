/* aaudio_shim.h -- AAudio surface for FMOD's Android output backend.
 *
 * Differs from pvzultimate's aaudio_shim.c in intent: that one exists to fail
 * the way the game expects, and discards samples. FMOD is native C++ and does
 * check its probe, so here the stream is real -- PCM goes to audout through
 * audio_sink.
 *
 * MIT license; see LICENSE. */

#ifndef __AAUDIO_SHIM_H__
#define __AAUDIO_SHIM_H__

#include <stdint.h>
#include "so_util.h"

// Registers the AAudio entry points into the wrapper's import table.
// Called from sts2_resolve_imports (imports.c).
void aaudio_shim_register(void);

// Looks up an AAudio_* / AAudioStream* / AAudioStreamBuilder_* symbol.
// dlsym_fake consults this after the main table so a dlopen("libaaudio.so")
// handle resolves the same way a direct import does.
uintptr_t aaudio_shim_find(const char *name);

// 1 if the shim should present itself as available. Reads
// config.audio_backend: false under AUDIO_BACKEND_OPENSL so dlopen_fake can
// refuse libaaudio.so and push FMOD onto the OpenSL ES path in audio.c.
int aaudio_shim_enabled(void);

void aaudio_shim_shutdown(void);

#endif
