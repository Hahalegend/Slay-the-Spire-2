/* aaudio_shim.c -- see aaudio_shim.h.
 *
 * FMOD's Android output backend (FMOD_OUTPUTTYPE_AAUDIO) drives a stream one
 * of two ways, and which one it picks depends on how the game initialised
 * FMOD -- not on anything we control. Both are implemented:
 *
 *   blocking write. FMOD's mixer thread calls AAudioStream_write() and
 *     expects it to block for roughly the duration of the audio it consumed.
 *     audio_sink_client_write() gives that for free: the ring backpressures against
 *     the feeder, which is paced by audout. No sleep() needed, and the pacing
 *     is the device's clock rather than a guess.
 *
 *   data callback. FMOD registers a callback and expects the system to pull.
 *     There is no system here, so a puller thread does it: fill a scratch
 *     buffer through the callback, convert, push to the sink, repeat. The
 *     sink's blocking write paces the loop, so the callback fires at the same
 *     rate a real device would fire it at.
 *
 * Format conversion happens on the way out. AAudio's contract is request-then-
 * query: the caller asks for a rate/format/channel count, and must read back
 * what it actually got. We always report 48 kHz stereo s16 because that is
 * what audout is, and let FMOD resample internally -- FMOD is good at that and
 * we would only be adding a second resampler.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "audio_sink.h"
#include "aaudio_shim.h"

#if VERBOSE_AUDIO && DEBUG_LOG
#define aaLog(...) debugPrintf("[aaudio] " __VA_ARGS__)
#else
#define aaLog(...) do {} while (0)
#endif

// --- AAudio constants (NDK aaudio/AAudio.h) ---------------------------------
#define AAUDIO_OK                          0
#define AAUDIO_ERROR_NULL                (-882)
#define AAUDIO_ERROR_INVALID_STATE       (-895)
#define AAUDIO_ERROR_UNAVAILABLE         (-878)

#define AAUDIO_FORMAT_INVALID            (-1)
#define AAUDIO_FORMAT_UNSPECIFIED          0
#define AAUDIO_FORMAT_PCM_I16              1
#define AAUDIO_FORMAT_PCM_FLOAT            2
#define AAUDIO_FORMAT_PCM_I24_PACKED       3
#define AAUDIO_FORMAT_PCM_I32              4

#define AAUDIO_STREAM_STATE_UNINITIALIZED  0
#define AAUDIO_STREAM_STATE_OPEN           1
#define AAUDIO_STREAM_STATE_STARTING       2
#define AAUDIO_STREAM_STATE_STARTED        3
#define AAUDIO_STREAM_STATE_PAUSING        4
#define AAUDIO_STREAM_STATE_PAUSED         5
#define AAUDIO_STREAM_STATE_FLUSHING       6
#define AAUDIO_STREAM_STATE_FLUSHED        7
#define AAUDIO_STREAM_STATE_STOPPING       8
#define AAUDIO_STREAM_STATE_STOPPED        9
#define AAUDIO_STREAM_STATE_CLOSING       10
#define AAUDIO_STREAM_STATE_CLOSED        11

#define AAUDIO_DIRECTION_OUTPUT            0
#define AAUDIO_DIRECTION_INPUT             1

#define AAUDIO_SHARING_MODE_EXCLUSIVE      0
#define AAUDIO_SHARING_MODE_SHARED         1

#define AAUDIO_PERFORMANCE_MODE_NONE        10
#define AAUDIO_PERFORMANCE_MODE_POWER_SAVING 11
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12

#define AAUDIO_CALLBACK_RESULT_CONTINUE    0
#define AAUDIO_CALLBACK_RESULT_STOP        1

typedef int32_t (*AAudioDataCallback)(void *stream, void *userData,
                                      void *audioData, int32_t numFrames);
typedef void (*AAudioErrorCallback)(void *stream, void *userData, int32_t error);

typedef struct {
  int32_t sample_rate;
  int32_t channels;
  int32_t format;
  int32_t direction;
  int32_t sharing_mode;
  int32_t performance_mode;
  int32_t buffer_capacity;
  int32_t frames_per_burst;
  AAudioDataCallback  data_cb;
  void               *data_cb_user;
  AAudioErrorCallback error_cb;
  void               *error_cb_user;
} AAudioBuilder;

typedef struct {
  AAudioBuilder cfg;        // resolved (post-open) configuration
  int      sink_client;     // audio_sink client id
  int32_t  state;
  int32_t  buffer_size;
  int64_t  frames_read;
  int32_t  xrun_count;

  // data-callback drive model
  Thread   puller;
  int      puller_running;
  int      puller_started;
  void    *scratch;         // frames_per_burst * channels * max sample size
  int16_t *convert;         // frames_per_burst * SINK_CHANNELS s16
} AAudioStream;

static AAudioStream *s_active = NULL;   // audout is single-instance

// --- format conversion -------------------------------------------------------
// Source is whatever the caller asked for; destination is always interleaved
// stereo s16 because that is what the sink takes. Mono is duplicated rather
// than centre-panned at -3 dB: FMOD has already done its own panning, so a
// mono stream here means a mono mix, and halving it would just be quieter.

static inline int16_t clamp_s16(float v) {
  if (v >= 1.0f)  return 32767;
  if (v <= -1.0f) return -32768;
  return (int16_t)(v * 32767.0f);
}

static void convert_to_sink(const void *src, int32_t frames,
                            int32_t src_channels, int32_t src_format,
                            int16_t *dst) {
  if (src_format == AAUDIO_FORMAT_PCM_FLOAT) {
    const float *f = (const float *)src;
    if (src_channels == 1) {
      for (int32_t i = 0; i < frames; i++) {
        int16_t v = clamp_s16(f[i]);
        dst[i * 2 + 0] = v;
        dst[i * 2 + 1] = v;
      }
    } else {
      for (int32_t i = 0; i < frames; i++) {
        dst[i * 2 + 0] = clamp_s16(f[i * src_channels + 0]);
        dst[i * 2 + 1] = clamp_s16(f[i * src_channels + 1]);
      }
    }
  } else if (src_format == AAUDIO_FORMAT_PCM_I32) {
    const int32_t *s = (const int32_t *)src;
    if (src_channels == 1) {
      for (int32_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(s[i] >> 16);
        dst[i * 2 + 0] = v;
        dst[i * 2 + 1] = v;
      }
    } else {
      for (int32_t i = 0; i < frames; i++) {
        dst[i * 2 + 0] = (int16_t)(s[i * src_channels + 0] >> 16);
        dst[i * 2 + 1] = (int16_t)(s[i * src_channels + 1] >> 16);
      }
    }
  } else { // PCM_I16 and anything unrecognised
    const int16_t *s = (const int16_t *)src;
    if (src_channels == 1) {
      for (int32_t i = 0; i < frames; i++) {
        dst[i * 2 + 0] = s[i];
        dst[i * 2 + 1] = s[i];
      }
    } else if (src_channels == 2) {
      memcpy(dst, s, (size_t)frames * 2 * sizeof(int16_t));
    } else {
      for (int32_t i = 0; i < frames; i++) {
        dst[i * 2 + 0] = s[i * src_channels + 0];
        dst[i * 2 + 1] = s[i * src_channels + 1];
      }
    }
  }
}

static int32_t bytes_per_sample(int32_t format) {
  switch (format) {
    case AAUDIO_FORMAT_PCM_FLOAT: return 4;
    case AAUDIO_FORMAT_PCM_I32:   return 4;
    case AAUDIO_FORMAT_PCM_I16:   return 2;
    default:                      return 2;
  }
}

// --- data-callback puller ----------------------------------------------------

static void puller_thread(void *arg) {
  AAudioStream *st = (AAudioStream *)arg;
  const int32_t burst = st->cfg.frames_per_burst;

  while (st->puller_running) {
    if (st->state != AAUDIO_STREAM_STATE_STARTED) {
      svcSleepThread(5000000ULL); // 5 ms
      continue;
    }

    int32_t r = st->cfg.data_cb((void *)st, st->cfg.data_cb_user,
                                st->scratch, burst);
    if (r == AAUDIO_CALLBACK_RESULT_STOP) {
      aaLog("data callback requested stop\n");
      st->state = AAUDIO_STREAM_STATE_STOPPING;
      break;
    }

    convert_to_sink(st->scratch, burst, st->cfg.channels, st->cfg.format,
                    st->convert);

    // Blocking: this is what paces the callback at device rate.
    audio_sink_client_write(st->sink_client, st->convert, burst, SINK_CHANNELS);
    st->frames_read += burst;
  }

  st->state = AAUDIO_STREAM_STATE_STOPPED;
}

// --- builder -----------------------------------------------------------------

static int32_t nx_AAudio_createStreamBuilder(AAudioBuilder **out) {
  if (!out) return AAUDIO_ERROR_NULL;

  AAudioBuilder *b = (AAudioBuilder *)calloc(1, sizeof(AAudioBuilder));
  if (!b) return AAUDIO_ERROR_UNAVAILABLE;

  b->sample_rate      = SINK_SAMPLE_RATE;
  b->channels         = SINK_CHANNELS;
  b->format           = AAUDIO_FORMAT_PCM_I16;
  b->direction        = AAUDIO_DIRECTION_OUTPUT;
  b->sharing_mode     = AAUDIO_SHARING_MODE_SHARED;
  b->performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
  b->frames_per_burst = SINK_FRAMES_PER_BUF;
  b->buffer_capacity  = SINK_FRAMES_PER_BUF * SINK_NUM_BUFFERS;

  *out = b;
  aaLog("createStreamBuilder -> %p\n", (void *)b);
  return AAUDIO_OK;
}

#define BUILDER_SETTER(name, field, type) \
  static void nx_AAudioStreamBuilder_##name(AAudioBuilder *b, type v) { \
    if (b) b->field = (typeof(b->field))v; }

BUILDER_SETTER(setSampleRate,      sample_rate,      int32_t)
BUILDER_SETTER(setChannelCount,    channels,         int32_t)
BUILDER_SETTER(setFormat,          format,           int32_t)
BUILDER_SETTER(setSharingMode,     sharing_mode,     int32_t)
BUILDER_SETTER(setDirection,       direction,        int32_t)
BUILDER_SETTER(setPerformanceMode, performance_mode, int32_t)
BUILDER_SETTER(setBufferCapacityInFrames, buffer_capacity, int32_t)
BUILDER_SETTER(setFramesPerDataCallback,  frames_per_burst, int32_t)

// Ignored entirely: these describe routing/ducking policy on Android and have
// no analogue on audout. Present so FMOD's symbol resolution succeeds.
static void nx_AAudioStreamBuilder_setDeviceId(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setUsage(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setContentType(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setInputPreset(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setAllowedCapturePolicy(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setSessionId(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void nx_AAudioStreamBuilder_setPrivacySensitive(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }

static void nx_AAudioStreamBuilder_setDataCallback(AAudioBuilder *b,
                                                   AAudioDataCallback cb,
                                                   void *user) {
  if (!b) return;
  b->data_cb = cb;
  b->data_cb_user = user;
  aaLog("setDataCallback %p\n", (void *)cb);
}

static void nx_AAudioStreamBuilder_setErrorCallback(AAudioBuilder *b,
                                                    AAudioErrorCallback cb,
                                                    void *user) {
  if (!b) return;
  b->error_cb = cb;
  b->error_cb_user = user;
}

static int32_t nx_AAudioStreamBuilder_openStream(AAudioBuilder *b, void **out) {
  if (!b || !out) return AAUDIO_ERROR_NULL;

  if (b->direction == AAUDIO_DIRECTION_INPUT) {
    aaLog("openStream: input direction refused (no capture device)\n");
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  if (s_active) {
    aaLog("openStream: a stream is already open\n");
    return AAUDIO_ERROR_UNAVAILABLE;
  }

  AAudioStream *st = (AAudioStream *)calloc(1, sizeof(AAudioStream));
  if (!st) return AAUDIO_ERROR_UNAVAILABLE;

  st->sink_client = audio_sink_client_open("fmod-aaudio");
  if (st->sink_client < 0) {
    aaLog("openStream: no sink client available\n");
    free(st);
    return AAUDIO_ERROR_UNAVAILABLE;
  }

  st->cfg = *b;

  // Resolve to what we can actually deliver. The caller is required by the
  // AAudio contract to query these back rather than assume its request stuck.
  st->cfg.sample_rate = SINK_SAMPLE_RATE;
  if (st->cfg.channels < 1 || st->cfg.channels > 8) st->cfg.channels = SINK_CHANNELS;
  if (st->cfg.format == AAUDIO_FORMAT_UNSPECIFIED ||
      st->cfg.format == AAUDIO_FORMAT_INVALID)
    st->cfg.format = AAUDIO_FORMAT_PCM_I16;
  if (st->cfg.frames_per_burst <= 0) st->cfg.frames_per_burst = SINK_FRAMES_PER_BUF;

  st->buffer_size = st->cfg.buffer_capacity;
  st->state = AAUDIO_STREAM_STATE_OPEN;

  if (st->cfg.data_cb) {
    size_t scratch_bytes = (size_t)st->cfg.frames_per_burst *
                           (size_t)st->cfg.channels *
                           (size_t)bytes_per_sample(st->cfg.format);
    st->scratch = malloc(scratch_bytes);
    st->convert = (int16_t *)malloc((size_t)st->cfg.frames_per_burst * SINK_FRAME_BYTES);
    if (!st->scratch || !st->convert) {
      free(st->scratch); free(st->convert);
      audio_sink_client_close(st->sink_client);
      free(st);
      return AAUDIO_ERROR_UNAVAILABLE;
    }
    memset(st->scratch, 0, scratch_bytes);
  }

  s_active = st;
  *out = st;
  aaLog("openStream ok: %ld Hz, %ld ch, fmt %ld, burst %ld, %s\n",
        (long)st->cfg.sample_rate, (long)st->cfg.channels, (long)st->cfg.format,
        (long)st->cfg.frames_per_burst, st->cfg.data_cb ? "callback" : "blocking write");
  return AAUDIO_OK;
}

static int32_t nx_AAudioStreamBuilder_delete(AAudioBuilder *b) {
  free(b);
  return AAUDIO_OK;
}

// --- stream ------------------------------------------------------------------

static int32_t nx_AAudioStream_requestStart(AAudioStream *st) {
  if (!st) return AAUDIO_ERROR_NULL;

  st->state = AAUDIO_STREAM_STATE_STARTED;
  audio_sink_set_paused(0);

  if (st->cfg.data_cb && !st->puller_started) {
    st->puller_running = 1;
    Result rc = threadCreate(&st->puller, puller_thread, st, NULL, 0x10000, 0x2D, -2);
    if (R_FAILED(rc)) {
      st->puller_running = 0;
      aaLog("puller threadCreate failed: 0x%x\n", rc);
      return AAUDIO_ERROR_UNAVAILABLE;
    }
    if (R_FAILED(threadStart(&st->puller))) {
      threadClose(&st->puller);
      st->puller_running = 0;
      return AAUDIO_ERROR_UNAVAILABLE;
    }
    st->puller_started = 1;
  }

  aaLog("requestStart\n");
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_requestPause(AAudioStream *st) {
  if (!st) return AAUDIO_ERROR_NULL;
  st->state = AAUDIO_STREAM_STATE_PAUSED;
  audio_sink_set_paused(1);
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_requestFlush(AAudioStream *st) {
  if (!st) return AAUDIO_ERROR_NULL;
  st->state = AAUDIO_STREAM_STATE_FLUSHED;
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_requestStop(AAudioStream *st) {
  if (!st) return AAUDIO_ERROR_NULL;
  st->state = AAUDIO_STREAM_STATE_STOPPED;
  audio_sink_set_paused(1);
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_close(AAudioStream *st) {
  if (!st) return AAUDIO_ERROR_NULL;

  if (st->puller_started) {
    st->puller_running = 0;
    threadWaitForExit(&st->puller);
    threadClose(&st->puller);
    st->puller_started = 0;
  }

  st->state = AAUDIO_STREAM_STATE_CLOSED;
  free(st->scratch);
  free(st->convert);
  audio_sink_client_close(st->sink_client);
  if (s_active == st) s_active = NULL;
  free(st);
  aaLog("close\n");
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_write(AAudioStream *st, const void *buf,
                                     int32_t frames, int64_t timeout_ns) {
  (void)timeout_ns;
  if (!st || !buf) return AAUDIO_ERROR_NULL;
  if (st->state != AAUDIO_STREAM_STATE_STARTED) return AAUDIO_ERROR_INVALID_STATE;
  if (frames <= 0) return 0;

  // Convert in chunks so a large write does not need a matching scratch alloc.
  enum { CHUNK = 512 };
  static int16_t chunk_buf[CHUNK * SINK_CHANNELS];

  int32_t done = 0;
  const uint8_t *src = (const uint8_t *)buf;
  const int32_t src_frame_bytes = st->cfg.channels * bytes_per_sample(st->cfg.format);

  while (done < frames) {
    int32_t n = frames - done;
    if (n > CHUNK) n = CHUNK;

    convert_to_sink(src + (size_t)done * src_frame_bytes, n,
                    st->cfg.channels, st->cfg.format, chunk_buf);

    int wrote = audio_sink_client_write(st->sink_client, chunk_buf, n, SINK_CHANNELS);
    done += wrote;
    if (wrote < n) break; // sink shut down under us
  }

  st->frames_read += done;
  return done;
}

static int32_t nx_AAudioStream_getState(AAudioStream *st) {
  return st ? st->state : AAUDIO_STREAM_STATE_UNINITIALIZED;
}

static int32_t nx_AAudioStream_waitForStateChange(AAudioStream *st, int32_t input,
                                                  int32_t *next, int64_t timeout_ns) {
  (void)input; (void)timeout_ns;
  if (!st || !next) return AAUDIO_ERROR_NULL;
  *next = st->state;
  return AAUDIO_OK;
}

static int32_t nx_AAudioStream_setBufferSizeInFrames(AAudioStream *st, int32_t n) {
  if (!st) return AAUDIO_ERROR_NULL;
  if (n > st->cfg.buffer_capacity) n = st->cfg.buffer_capacity;
  st->buffer_size = n;
  return n;
}

static int32_t nx_AAudioStream_getBufferSizeInFrames(AAudioStream *st) {
  return st ? st->buffer_size : 0;
}

static int32_t nx_AAudioStream_getSampleRate(AAudioStream *st) {
  return st ? st->cfg.sample_rate : SINK_SAMPLE_RATE;
}
static int32_t nx_AAudioStream_getChannelCount(AAudioStream *st) {
  return st ? st->cfg.channels : SINK_CHANNELS;
}
static int32_t nx_AAudioStream_getSamplesPerFrame(AAudioStream *st) {
  return st ? st->cfg.channels : SINK_CHANNELS;
}
static int32_t nx_AAudioStream_getFormat(AAudioStream *st) {
  return st ? st->cfg.format : AAUDIO_FORMAT_PCM_I16;
}
static int32_t nx_AAudioStream_getFramesPerBurst(AAudioStream *st) {
  return st ? st->cfg.frames_per_burst : SINK_FRAMES_PER_BUF;
}
static int32_t nx_AAudioStream_getBufferCapacityInFrames(AAudioStream *st) {
  return st ? st->cfg.buffer_capacity : SINK_FRAMES_PER_BUF * SINK_NUM_BUFFERS;
}
static int32_t nx_AAudioStream_getPerformanceMode(AAudioStream *st) {
  return st ? st->cfg.performance_mode : AAUDIO_PERFORMANCE_MODE_NONE;
}
static int32_t nx_AAudioStream_getSharingMode(AAudioStream *st) {
  return st ? st->cfg.sharing_mode : AAUDIO_SHARING_MODE_SHARED;
}
static int32_t nx_AAudioStream_getDirection(AAudioStream *st) {
  return st ? st->cfg.direction : AAUDIO_DIRECTION_OUTPUT;
}
static int32_t nx_AAudioStream_getDeviceId(AAudioStream *st) { (void)st; return 0; }
static int32_t nx_AAudioStream_getSessionId(AAudioStream *st) { (void)st; return 0; }
static int32_t nx_AAudioStream_getUsage(AAudioStream *st) { (void)st; return 0; }
static int32_t nx_AAudioStream_getContentType(AAudioStream *st) { (void)st; return 0; }
static int32_t nx_AAudioStream_getAllowedCapturePolicy(AAudioStream *st) { (void)st; return 0; }

static int32_t nx_AAudioStream_getXRunCount(AAudioStream *st) {
  return st ? st->xrun_count : 0;
}

static int32_t nx_AAudioStream_getBufferSizeInFramesQueued(AAudioStream *st) {
  return st ? audio_sink_client_queued(st->sink_client) : 0;
}

static int64_t nx_AAudioStream_getFramesWritten(AAudioStream *st) {
  (void)st;
  return audio_sink_frames_written();
}

static int64_t nx_AAudioStream_getFramesRead(AAudioStream *st) {
  return st ? st->frames_read : 0;
}

static int32_t nx_AAudioStream_getTimestamp(AAudioStream *st, int32_t clockid,
                                            int64_t *framePosition,
                                            int64_t *timeNanoseconds) {
  (void)clockid;
  if (!st || !framePosition || !timeNanoseconds) return AAUDIO_ERROR_NULL;
  *framePosition = audio_sink_frames_written();
  *timeNanoseconds = (int64_t)armTicksToNs(armGetSystemTick());
  return AAUDIO_OK;
}

static const char *nx_AAudio_convertResultToText(int32_t r) {
  return r == AAUDIO_OK ? "AAUDIO_OK" : "AAUDIO_ERROR";
}
static const char *nx_AAudio_convertStreamStateToText(int32_t s) {
  switch (s) {
    case AAUDIO_STREAM_STATE_OPEN:    return "OPEN";
    case AAUDIO_STREAM_STATE_STARTED: return "STARTED";
    case AAUDIO_STREAM_STATE_PAUSED:  return "PAUSED";
    case AAUDIO_STREAM_STATE_STOPPED: return "STOPPED";
    case AAUDIO_STREAM_STATE_CLOSED:  return "CLOSED";
    default: return "UNKNOWN";
  }
}

// --- registration ------------------------------------------------------------

static const DynLibFunction aaudio_functions[] = {
  { "AAudio_createStreamBuilder",  (uintptr_t)&nx_AAudio_createStreamBuilder },
  { "AAudio_convertResultToText",  (uintptr_t)&nx_AAudio_convertResultToText },
  { "AAudio_convertStreamStateToText", (uintptr_t)&nx_AAudio_convertStreamStateToText },

  { "AAudioStreamBuilder_setSampleRate",      (uintptr_t)&nx_AAudioStreamBuilder_setSampleRate },
  { "AAudioStreamBuilder_setChannelCount",    (uintptr_t)&nx_AAudioStreamBuilder_setChannelCount },
  { "AAudioStreamBuilder_setDeviceId",        (uintptr_t)&nx_AAudioStreamBuilder_setDeviceId },
  { "AAudioStreamBuilder_setFormat",          (uintptr_t)&nx_AAudioStreamBuilder_setFormat },
  { "AAudioStreamBuilder_setSharingMode",     (uintptr_t)&nx_AAudioStreamBuilder_setSharingMode },
  { "AAudioStreamBuilder_setDirection",       (uintptr_t)&nx_AAudioStreamBuilder_setDirection },
  { "AAudioStreamBuilder_setPerformanceMode", (uintptr_t)&nx_AAudioStreamBuilder_setPerformanceMode },
  { "AAudioStreamBuilder_setBufferCapacityInFrames", (uintptr_t)&nx_AAudioStreamBuilder_setBufferCapacityInFrames },
  { "AAudioStreamBuilder_setFramesPerDataCallback",  (uintptr_t)&nx_AAudioStreamBuilder_setFramesPerDataCallback },
  { "AAudioStreamBuilder_setUsage",           (uintptr_t)&nx_AAudioStreamBuilder_setUsage },
  { "AAudioStreamBuilder_setContentType",     (uintptr_t)&nx_AAudioStreamBuilder_setContentType },
  { "AAudioStreamBuilder_setInputPreset",     (uintptr_t)&nx_AAudioStreamBuilder_setInputPreset },
  { "AAudioStreamBuilder_setAllowedCapturePolicy", (uintptr_t)&nx_AAudioStreamBuilder_setAllowedCapturePolicy },
  { "AAudioStreamBuilder_setSessionId",       (uintptr_t)&nx_AAudioStreamBuilder_setSessionId },
  { "AAudioStreamBuilder_setPrivacySensitive",(uintptr_t)&nx_AAudioStreamBuilder_setPrivacySensitive },
  { "AAudioStreamBuilder_setDataCallback",    (uintptr_t)&nx_AAudioStreamBuilder_setDataCallback },
  { "AAudioStreamBuilder_setErrorCallback",   (uintptr_t)&nx_AAudioStreamBuilder_setErrorCallback },
  { "AAudioStreamBuilder_openStream",         (uintptr_t)&nx_AAudioStreamBuilder_openStream },
  { "AAudioStreamBuilder_delete",             (uintptr_t)&nx_AAudioStreamBuilder_delete },

  { "AAudioStream_requestStart",  (uintptr_t)&nx_AAudioStream_requestStart },
  { "AAudioStream_requestPause",  (uintptr_t)&nx_AAudioStream_requestPause },
  { "AAudioStream_requestFlush",  (uintptr_t)&nx_AAudioStream_requestFlush },
  { "AAudioStream_requestStop",   (uintptr_t)&nx_AAudioStream_requestStop },
  { "AAudioStream_close",         (uintptr_t)&nx_AAudioStream_close },
  { "AAudioStream_write",         (uintptr_t)&nx_AAudioStream_write },
  { "AAudioStream_getState",      (uintptr_t)&nx_AAudioStream_getState },
  { "AAudioStream_waitForStateChange", (uintptr_t)&nx_AAudioStream_waitForStateChange },
  { "AAudioStream_setBufferSizeInFrames", (uintptr_t)&nx_AAudioStream_setBufferSizeInFrames },
  { "AAudioStream_getBufferSizeInFrames", (uintptr_t)&nx_AAudioStream_getBufferSizeInFrames },
  { "AAudioStream_getSampleRate",     (uintptr_t)&nx_AAudioStream_getSampleRate },
  { "AAudioStream_getChannelCount",   (uintptr_t)&nx_AAudioStream_getChannelCount },
  { "AAudioStream_getSamplesPerFrame",(uintptr_t)&nx_AAudioStream_getSamplesPerFrame },
  { "AAudioStream_getFormat",         (uintptr_t)&nx_AAudioStream_getFormat },
  { "AAudioStream_getFramesPerBurst", (uintptr_t)&nx_AAudioStream_getFramesPerBurst },
  { "AAudioStream_getBufferCapacityInFrames", (uintptr_t)&nx_AAudioStream_getBufferCapacityInFrames },
  { "AAudioStream_getPerformanceMode",(uintptr_t)&nx_AAudioStream_getPerformanceMode },
  { "AAudioStream_getSharingMode",    (uintptr_t)&nx_AAudioStream_getSharingMode },
  { "AAudioStream_getDirection",      (uintptr_t)&nx_AAudioStream_getDirection },
  { "AAudioStream_getDeviceId",       (uintptr_t)&nx_AAudioStream_getDeviceId },
  { "AAudioStream_getSessionId",      (uintptr_t)&nx_AAudioStream_getSessionId },
  { "AAudioStream_getUsage",          (uintptr_t)&nx_AAudioStream_getUsage },
  { "AAudioStream_getContentType",    (uintptr_t)&nx_AAudioStream_getContentType },
  { "AAudioStream_getAllowedCapturePolicy", (uintptr_t)&nx_AAudioStream_getAllowedCapturePolicy },
  { "AAudioStream_getXRunCount",      (uintptr_t)&nx_AAudioStream_getXRunCount },
  { "AAudioStream_getFramesWritten",  (uintptr_t)&nx_AAudioStream_getFramesWritten },
  { "AAudioStream_getFramesRead",     (uintptr_t)&nx_AAudioStream_getFramesRead },
  { "AAudioStream_getTimestamp",      (uintptr_t)&nx_AAudioStream_getTimestamp },
};

static const size_t aaudio_numfunctions =
  sizeof(aaudio_functions) / sizeof(aaudio_functions[0]);

int aaudio_shim_enabled(void) {
  return config.audio_backend != AUDIO_BACKEND_OPENSL;
}

void aaudio_shim_register(void) {
  aaLog("registered %zu entry points (%s)\n", aaudio_numfunctions,
        aaudio_shim_enabled() ? "enabled" : "disabled by config");
}

uintptr_t aaudio_shim_find(const char *name) {
  if (!name || !aaudio_shim_enabled()) return 0;
  DynLibFunction *f = so_find_import((DynLibFunction *)aaudio_functions,
                                     (int)aaudio_numfunctions, name);
  return f ? f->func : 0;
}

void aaudio_shim_shutdown(void) {
  if (s_active) nx_AAudioStream_close(s_active);
}
