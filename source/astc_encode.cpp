/* astc_encode.cpp -- the thinnest possible C wrapper over astcenc.
 *
 * astcenc is C++ and the rest of this port is C, so the boundary lives here
 * rather than spreading `extern "C"` through 22 vendored files.
 *
 * THREADS
 * =======
 * astcenc has its own worker model: astcenc_context_alloc takes a thread count
 * and astcenc_compress_image must then be called once per thread index with
 * the same context and image. That is used directly rather than encoding one
 * texture per core, because a single 4096x4096 texture would otherwise pin one
 * core for its whole encode while the others idled -- and the largest textures
 * are exactly the ones that dominate the total.
 *
 * The Switch gives userland three of four A57 cores by default; core 3 is the
 * system's. ASTC_THREADS is 3.
 *
 * QUALITY
 * =======
 * ASTCENC_PRE_FAST. This is a one-off boot cost measured in minutes on a
 * 1 GHz A57, and the input is already lossy for two thirds of the textures
 * (a BC7 decode, not the original art), so spending 4x the time chasing
 * quality the source no longer has is a poor trade.
 *
 * MIT license; see LICENSE. */

#include <cstring>
#include <cstdint>

#include "thirdparty/astcenc/astcenc.h"

extern "C" {
#include "util.h"
}

/* debugPrintf takes `char *`, not `const char *`.
 *
 * C lets a string literal through with at most a warning. C++ does not -- this
 * is the one file in the project that is C++, so it is the one file where the
 * project's own logging signature is a hard error. Casting here is smaller
 * than changing a signature every other file already depends on. */
/* The FORMAT only. An earlier version cast __VA_ARGS__ whole, which makes the
 * argument list a comma expression: every value after the format string was
 * evaluated and discarded, and the log would have printed garbage for each
 * %u. -Wunused-value caught it; nothing at runtime would have. */
#define LOG(fmt, ...) debugPrintf(const_cast<char *>(fmt), ##__VA_ARGS__)

#define ASTC_THREADS 3

namespace {

struct Encoder {
  astcenc_context *ctx = nullptr;
  unsigned bx = 0, by = 0;
  bool ok = false;
};

Encoder g_enc;

}  // namespace

extern "C" int astc_encode_init(unsigned block_x, unsigned block_y) {
  if (g_enc.ok && g_enc.bx == block_x && g_enc.by == block_y) return 1;
  if (g_enc.ctx) { astcenc_context_free(g_enc.ctx); g_enc.ctx = nullptr; }
  g_enc.ok = false;

  astcenc_config cfg{};
  // LDR_SRGB matches what the desktop script used. Getting this wrong does not
  // fail -- it shifts every colour, subtly, on every texture at once.
  astcenc_error e = astcenc_config_init(ASTCENC_PRF_LDR_SRGB,
                                        block_x, block_y, 1,
                                        ASTCENC_PRE_FAST, 0, &cfg);
  if (e != ASTCENC_SUCCESS) {
    LOG("[astc] config_init(%ux%u) failed: %s\n",
                block_x, block_y, astcenc_get_error_string(e));
    return 0;
  }

  e = astcenc_context_alloc(&cfg, ASTC_THREADS, &g_enc.ctx, nullptr);
  if (e != ASTCENC_SUCCESS) {
    LOG("[astc] context_alloc failed: %s\n", astcenc_get_error_string(e));
    return 0;
  }

  g_enc.bx = block_x; g_enc.by = block_y; g_enc.ok = true;
  LOG("[astc] encoder ready: %ux%u LDR_SRGB, fast, %d threads\n",
              block_x, block_y, ASTC_THREADS);
  return 1;
}

extern "C" size_t astc_encode_size(unsigned w, unsigned h,
                                   unsigned block_x, unsigned block_y) {
  const unsigned bw = (w + block_x - 1) / block_x;
  const unsigned bh = (h + block_y - 1) / block_y;
  return (size_t)bw * bh * 16;   /* every ASTC block is 16 bytes */
}

/* rgba: w*h*4 bytes. out: astc_encode_size() bytes. Returns 1 on success. */
extern "C" int astc_encode_rgba(const uint8_t *rgba, unsigned w, unsigned h,
                                uint8_t *out, size_t out_size) {
  if (!g_enc.ok || !rgba || !out) return 0;

  void *planes[1];
  planes[0] = (void *)rgba;

  astcenc_image img{};
  img.dim_x = w;
  img.dim_y = h;
  img.dim_z = 1;
  img.data_type = ASTCENC_TYPE_U8;
  img.data = planes;

  const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G,
                            ASTCENC_SWZ_B, ASTCENC_SWZ_A};

  // Reset before each image. Without it the second and subsequent encodes on
  // the same context return ASTCENC_ERR_BAD_CONTEXT, which would look like
  // "the encoder broke after one texture".
  astcenc_compress_reset(g_enc.ctx);

  for (int t = 0; t < ASTC_THREADS; t++) {
    const astcenc_error e =
        astcenc_compress_image(g_enc.ctx, &img, &swz, out, out_size, t);
    if (e != ASTCENC_SUCCESS) {
      LOG("[astc] compress %ux%u failed on thread %d: %s\n",
                  w, h, t, astcenc_get_error_string(e));
      return 0;
    }
  }
  return 1;
}

extern "C" void astc_encode_shutdown(void) {
  if (g_enc.ctx) { astcenc_context_free(g_enc.ctx); g_enc.ctx = nullptr; }
  g_enc.ok = false;
}
