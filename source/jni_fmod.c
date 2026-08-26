/* jni_fmod.c -- org.fmod.* JNI surface.
 *
 * Derived, not guessed. The method set below is the intersection of
 * classes.dex (StS2Launcher-v0.3.14) with the string table of
 * assets/native/libfmod.so -- i.e. every Java method FMOD's native side
 * actually looks up. See docs/JNI_SURFACE.md for the derivation.
 *
 * Three classes matter:
 *
 *   org/fmod/FMOD          static; init/teardown and the device queries FMOD
 *                          sizes its mixer from.
 *   org/fmod/AudioDevice   instance; the AudioTrack fallback path. FMOD calls
 *                          init(rate,ch,fmt,bufsize) then pushes s16 through
 *                          write([S,len). This is the LOWEST-priority backend
 *                          and we want FMOD on AAudio instead -- but if AAudio
 *                          and OpenSL both fail, this is what it falls to, and
 *                          it is cheap to support because audio_sink already
 *                          takes exactly this format.
 *   org/fmod/MediaCodec    instance; hardware audio decode via Android's
 *                          MediaCodec. There is no such thing here. init()
 *                          returns false, which makes FMOD fall back to its
 *                          own Vorbis/FADPCM decoders -- which is what we want
 *                          regardless, since those are pure CPU and portable.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "audio_sink.h"
#include "aaudio_shim.h"
#include "jni_fake.h"   // jni_assetmgr_object
#include "jni_fmod.h"

#if VERBOSE_AUDIO && DEBUG_LOG
#define fjLog(...) debugPrintf("[fmod-jni] " __VA_ARGS__)
#else
#define fjLog(...) do {} while (0)
#endif

// --- org/fmod/FMOD (all static) ----------------------------------------------

int fmod_jni_static(const char *name, jni_ret *out, va_list va) {
  if (!name || !out) return 0;

  // init(Landroid/content/Context;)V -- caches the AssetManager on Android.
  // The wrapper serves assets from the SD card through godot_shim.c's
  // AAssetManager emulation, so there is nothing to cache.
  if (!strcmp(name, "init")) {
    fjLog("FMOD.init() -- no-op\n");
    out->kind = JNI_VOID;
    return 1;
  }

  if (!strcmp(name, "close")) {
    fjLog("FMOD.close()\n");
    out->kind = JNI_VOID;
    return 1;
  }

  // checkInit()Z -- FMOD refuses to initialise if this is false.
  if (!strcmp(name, "checkInit")) {
    out->kind = JNI_BOOL;
    out->z = 1;
    return 1;
  }

  // These two size FMOD's mixer buffers. They MUST agree with audio_sink.h:
  // FMOD computes its latency budget from them, so a mismatch shows up as an
  // underrun pattern that looks like a device fault and is not one.
  if (!strcmp(name, "getOutputSampleRate")) {
    out->kind = JNI_INT;
    out->i = SINK_SAMPLE_RATE;
    return 1;
  }
  if (!strcmp(name, "getOutputBlockSize")) {
    out->kind = JNI_INT;
    out->i = SINK_FRAMES_PER_BUF;
    return 1;
  }

  // supportsAAudio()Z -- gates whether FMOD even tries to dlopen libaaudio.so.
  // Follows config.audio_backend so AUDIO_BACKEND_OPENSL can push FMOD onto
  // the OpenSL path without editing code.
  if (!strcmp(name, "supportsAAudio")) {
    out->kind = JNI_BOOL;
    out->z = aaudio_shim_enabled();
    fjLog("supportsAAudio -> %d\n", out->z);
    return 1;
  }

  // supportsLowLatency()Z -- audout is a fixed-latency device with no
  // fast-mixer concept. Claiming support makes FMOD request buffer sizes we
  // cannot honour, and it will not renegotiate after the fact.
  if (!strcmp(name, "supportsLowLatency")) {
    out->kind = JNI_BOOL;
    out->z = 0;
    return 1;
  }

  // getAssetManager()Landroid/content/res/AssetManager; -- hand back the same
  // object jni_fake already vends to the engine, so FMOD's asset reads go
  // through the one AAssetManager emulation rather than a second path.
  if (!strcmp(name, "getAssetManager")) {
    out->kind = JNI_OBJECT;
    out->l = jni_assetmgr_object();
    return 1;
  }

  // getAudioDevices(I)[Landroid/media/AudioDeviceInfo; -- device enumeration
  // for FMOD's output-device list. One fixed device; an empty array is safer
  // than a fabricated AudioDeviceInfo, because FMOD only walks the array to
  // find a preferred device and falls back to the default when it finds none.
  if (!strcmp(name, "getAudioDevices")) {
    (void)va_arg(va, int);
    out->kind = JNI_OBJECT;
    out->l = NULL;
    return 1;
  }

  // fileDescriptorFromUri(Ljava/lang/String;)I -- content:// URI resolution.
  // No content provider here; -1 makes FMOD use the plain path API instead.
  if (!strcmp(name, "fileDescriptorFromUri")) {
    (void)va_arg(va, void *);
    out->kind = JNI_INT;
    out->i = -1;
    return 1;
  }

  return 0;
}

// --- org/fmod/AudioDevice (instance) -----------------------------------------
//
// FMOD's AudioTrack fallback. Reached only if AAudio and OpenSL both fail.
// Supported because the format it pushes -- interleaved s16 -- is exactly what
// audio_sink takes, so it costs almost nothing to have a third working path.

static int  s_dev_open = 0;
static int  s_dev_channels = SINK_CHANNELS;
static int  s_dev_rate = SINK_SAMPLE_RATE;
static int  s_dev_client = -1;

int fmod_jni_audiodevice(const char *name, jni_ret *out, va_list va) {
  if (!name || !out) return 0;

  // init(IIII)Z -- (sampleRate, channels, format, bufferSizeFrames)
  if (!strcmp(name, "init")) {
    int rate     = va_arg(va, int);
    int channels = va_arg(va, int);
    (void)va_arg(va, int);            // format; always s16 in practice
    (void)va_arg(va, int);            // buffer size; audio_sink fixes this

    s_dev_rate = rate;
    s_dev_channels = channels < 1 ? SINK_CHANNELS : channels;

    s_dev_client = audio_sink_client_open("fmod-audiodevice");
    s_dev_open = (s_dev_client >= 0);

    fjLog("AudioDevice.init(%d Hz, %d ch) -> %d\n", rate, channels, s_dev_open);
    out->kind = JNI_BOOL;
    out->z = s_dev_open;
    return 1;
  }

  // write([SI)V -- (short[] buffer, int lengthInShorts)
  if (!strcmp(name, "write")) {
    void *arr = va_arg(va, void *);
    int   len = va_arg(va, int);

    if (s_dev_open && arr && len > 0) {
      const int16_t *pcm = jni_short_array_data(arr);
      if (pcm) {
        int frames = len / (s_dev_channels > 0 ? s_dev_channels : 1);
        audio_sink_client_write(s_dev_client, pcm, frames, s_dev_channels);
      }
    }
    out->kind = JNI_VOID;
    return 1;
  }

  if (!strcmp(name, "close")) {
    if (s_dev_open) {
      audio_sink_client_close(s_dev_client);
      s_dev_open = 0;
      s_dev_client = -1;
    }
    fjLog("AudioDevice.close()\n");
    out->kind = JNI_VOID;
    return 1;
  }

  return 0;
}

// --- org/fmod/MediaCodec (instance) ------------------------------------------
//
// Hardware audio decode. Deliberately refused: init() returning false makes
// FMOD use its own Vorbis/FADPCM decoders, which are pure CPU and portable.
// The remaining methods are answered anyway so that a caller which ignores
// init()'s result gets consistent nonsense rather than a null deref.

int fmod_jni_mediacodec(const char *name, jni_ret *out, va_list va) {
  if (!name || !out) return 0;

  if (!strcmp(name, "init")) {
    (void)va_arg(va, int64_t);
    fjLog("MediaCodec.init() -> false (forcing FMOD's own decoders)\n");
    out->kind = JNI_BOOL;
    out->z = 0;
    return 1;
  }

  if (!strcmp(name, "read")) {
    (void)va_arg(va, void *);
    (void)va_arg(va, int);
    out->kind = JNI_INT;
    out->i = 0;                       // 0 bytes read == end of stream
    return 1;
  }

  if (!strcmp(name, "seek")) {
    (void)va_arg(va, int);
    out->kind = JNI_VOID;
    return 1;
  }

  if (!strcmp(name, "release")) { out->kind = JNI_VOID; return 1; }

  if (!strcmp(name, "getChannelCount")) { out->kind = JNI_INT;  out->i = 0; return 1; }
  if (!strcmp(name, "getSampleRate"))   { out->kind = JNI_INT;  out->i = 0; return 1; }
  if (!strcmp(name, "getLength"))       { out->kind = JNI_LONG; out->j = 0; return 1; }

  return 0;
}

// --- dispatch ----------------------------------------------------------------

int fmod_jni_dispatch(const char *cls, const char *name, jni_ret *out, va_list va) {
  if (!cls) return 0;

  if (!strcmp(cls, "org/fmod/FMOD"))
    return fmod_jni_static(name, out, va);
  if (!strcmp(cls, "org/fmod/AudioDevice"))
    return fmod_jni_audiodevice(name, out, va);
  if (!strncmp(cls, "org/fmod/MediaCodec", 19))
    return fmod_jni_mediacodec(name, out, va);

  return 0;
}
