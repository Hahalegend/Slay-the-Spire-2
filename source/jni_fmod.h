/* jni_fmod.h -- org.fmod.* JNI surface. See jni_fmod.c and docs/JNI_SURFACE.md.
 * MIT license; see LICENSE. */

#ifndef __JNI_FMOD_H__
#define __JNI_FMOD_H__

#include <stdarg.h>
#include <stdint.h>

typedef enum {
  JNI_VOID = 0, JNI_BOOL, JNI_INT, JNI_LONG, JNI_FLOAT, JNI_DOUBLE, JNI_OBJECT
} jni_ret_kind;

typedef struct {
  jni_ret_kind kind;
  union { int z; int i; int64_t j; float f; double d; void *l; };
} jni_ret;

// Returns 1 if the (class, method) pair was handled. Call from jni_fake.c's
// Call*Method dispatchers before their own strcmp chains -- the org/fmod/*
// classes are disjoint from the org/godotengine/* ones, so ordering between
// them does not matter.
int fmod_jni_dispatch(const char *cls, const char *name, jni_ret *out, va_list va);

int fmod_jni_static(const char *name, jni_ret *out, va_list va);
int fmod_jni_audiodevice(const char *name, jni_ret *out, va_list va);
int fmod_jni_mediacodec(const char *name, jni_ret *out, va_list va);

// Provided by jni_fake.c. Needed here for AudioDevice.write([SI)V, which hands
// us a jshortArray we have to read through.
const int16_t *jni_short_array_data(void *array_ref);

// Provided by jni_fake.c; FMOD.getAssetManager() returns the same object the
// engine already holds so both go through one AAssetManager emulation.
void *jni_assetmgr_object(void);

#endif
