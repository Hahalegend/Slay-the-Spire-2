/* astc_decode.c -- get any texture in this pck to RGBA8.
 *
 * The pck stores textures four ways and each needs a different path. The
 * formats and their counts come from measuring sts2.pck, not from guessing
 * what a Godot export contains:
 *
 *     RGBA8 / RGB8 / LA8 / L8 / R8   raw, just widen to 4 channels
 *     DXT1 / DXT3 / DXT5             bcdec
 *     BPTC (BC7)                     bcdec
 *     PNG / WebP payloads            stb_image / libwebp
 *
 * CHANNEL ORDER
 * =============
 * bcdec writes RGBA directly, which is worth stating because the desktop
 * script's decoder (texture2ddecoder) returns BGRA and needs a swap. Copying
 * that swap here would break exactly the S3TC and BPTC textures -- the large
 * art -- while leaving the raw and PNG ones correct. The result looks
 * SELECTIVELY wrong rather than obviously wrong, which is the hardest kind to
 * notice: a red health bar and gold text next to a blue-skinned Ironclad.
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BCDEC_IMPLEMENTATION
#include "thirdparty/bcdec.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "thirdparty/stb_image.h"

#include <webp/decode.h>

#include "astc_decode.h"
#include "util.h"

/* Godot Image::Format ids present in this pck. */
#define F_L8 0
#define F_LA8 1
#define F_R8 2
#define F_RG8 3
#define F_RGB8 4
#define F_RGBA8 5
#define F_DXT1 17
#define F_DXT3 18
#define F_DXT5 19
#define F_BPTC_RGBA 22

/* CompressedTexture2D data_format */
#define D_IMAGE 0
#define D_PNG   1
#define D_WEBP  2
#define D_BASIS 3

/* Generic BCn walk. bcdec's per-block functions all take (src, dst, stride),
 * so only the block size and the callback differ between BC1/2/3/7. */
typedef void (*bc_fn)(const void *src, void *dst, int stride);

static void bc_walk(const uint8_t *src, size_t src_len, uint8_t *dst,
                    unsigned w, unsigned h, unsigned block_bytes, bc_fn fn) {
  const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t need = (size_t)bw * bh * block_bytes;
  if (src_len < need) return;                    /* caller checks the output */

  for (unsigned by = 0; by < bh; by++) {
    for (unsigned bx = 0; bx < bw; bx++) {
      /* A block at the right or bottom edge can hang past the image on a
       * non-multiple-of-4 texture. Decode into scratch and copy the part that
       * exists, rather than letting bcdec write outside dst. */
      uint8_t tile[4 * 4 * 4];
      fn(src + ((size_t)by * bw + bx) * block_bytes, tile, 4 * 4);

      for (unsigned y = 0; y < 4; y++) {
        const unsigned py = by * 4 + y;
        if (py >= h) break;
        const unsigned run = (bx * 4 + 4 <= w) ? 4 : (w - bx * 4);
        memcpy(dst + ((size_t)py * w + bx * 4) * 4, tile + y * 16, run * 4);
      }
    }
  }
}

uint8_t *astc_decode_to_rgba(const uint8_t *blob, size_t blob_len,
                             uint32_t data_format, uint32_t fmt,
                             unsigned w, unsigned h, const char **why) {
  *why = NULL;
  const size_t px = (size_t)w * h;
  if (!px) { *why = "zero dimension"; return NULL; }

  /* PNG and WebP payloads are LENGTH-PREFIXED: Godot writes a u32 size then
   * that many bytes. Handing the prefix to the decoder fails on most textures,
   * which is how the desktop script found it. */
  if (data_format == D_PNG || data_format == D_WEBP) {
    if (blob_len < 4) { *why = "truncated payload"; return NULL; }
    uint32_t n;
    memcpy(&n, blob, 4);
    if (n == 0 || (size_t)n > blob_len - 4) { *why = "bad payload length"; return NULL; }

    const uint8_t *img = blob + 4;
    uint8_t *out = NULL;

    if (data_format == D_PNG) {
      int iw = 0, ih = 0, ic = 0;
      out = stbi_load_from_memory(img, (int)n, &iw, &ih, &ic, 4);
      if (!out) { *why = "png decode failed"; return NULL; }
      if ((unsigned)iw != w || (unsigned)ih != h) {
        stbi_image_free(out);
        *why = "png size mismatch";       /* no resampler here; skip it */
        return NULL;
      }
      return out;                          /* stb malloc'd; caller frees */
    }

    int iw = 0, ih = 0;
    out = WebPDecodeRGBA(img, n, &iw, &ih);
    if (!out) { *why = "webp decode failed"; return NULL; }
    if ((unsigned)iw != w || (unsigned)ih != h) {
      WebPFree(out);
      *why = "webp size mismatch";
      return NULL;
    }
    /* WebPDecodeRGBA uses its own allocator; hand back a malloc'd copy so the
     * caller has ONE free() to call rather than a per-format one. */
    uint8_t *copy = (uint8_t *)malloc(px * 4);
    if (copy) memcpy(copy, out, px * 4);
    WebPFree(out);
    if (!copy) { *why = "out of memory"; return NULL; }
    return copy;
  }

  if (data_format == D_BASIS) { *why = "basis needs a transcoder"; return NULL; }

  uint8_t *dst = (uint8_t *)malloc(px * 4);
  if (!dst) { *why = "out of memory"; return NULL; }

  switch (fmt) {
    case F_RGBA8:
      if (blob_len < px * 4) { free(dst); *why = "short RGBA8"; return NULL; }
      memcpy(dst, blob, px * 4);
      return dst;

    case F_RGB8:
      if (blob_len < px * 3) { free(dst); *why = "short RGB8"; return NULL; }
      for (size_t i = 0; i < px; i++) {
        dst[i * 4 + 0] = blob[i * 3 + 0];
        dst[i * 4 + 1] = blob[i * 3 + 1];
        dst[i * 4 + 2] = blob[i * 3 + 2];
        dst[i * 4 + 3] = 255;
      }
      return dst;

    case F_LA8:
      if (blob_len < px * 2) { free(dst); *why = "short LA8"; return NULL; }
      for (size_t i = 0; i < px; i++) {
        const uint8_t l = blob[i * 2], a = blob[i * 2 + 1];
        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = l;
        dst[i * 4 + 3] = a;
      }
      return dst;

    case F_RG8:
      if (blob_len < px * 2) { free(dst); *why = "short RG8"; return NULL; }
      for (size_t i = 0; i < px; i++) {
        dst[i * 4 + 0] = blob[i * 2 + 0];
        dst[i * 4 + 1] = blob[i * 2 + 1];
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
      }
      return dst;

    case F_L8:
    case F_R8:
      if (blob_len < px) { free(dst); *why = "short L8/R8"; return NULL; }
      for (size_t i = 0; i < px; i++) {
        const uint8_t v = blob[i];
        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = v;
        dst[i * 4 + 3] = 255;
      }
      return dst;

    case F_DXT1:
      memset(dst, 0, px * 4);
      bc_walk(blob, blob_len, dst, w, h, 8,  bcdec_bc1);
      return dst;
    case F_DXT3:
      memset(dst, 0, px * 4);
      bc_walk(blob, blob_len, dst, w, h, 16, bcdec_bc2);
      return dst;
    case F_DXT5:
      memset(dst, 0, px * 4);
      bc_walk(blob, blob_len, dst, w, h, 16, bcdec_bc3);
      return dst;
    case F_BPTC_RGBA:
      memset(dst, 0, px * 4);
      bc_walk(blob, blob_len, dst, w, h, 16, bcdec_bc7);
      return dst;

    default:
      free(dst);
      *why = "format not convertible";     /* already ASTC/ETC, or unknown */
      return NULL;
  }
}
