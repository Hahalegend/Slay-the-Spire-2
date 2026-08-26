/* astc_bake.c -- on-device ASTC conversion of sts2.pck's textures.
 *
 * WHY ON DEVICE
 * =============
 * tools/astc_convert.py already does this on a PC, and AstcOverlay.cs already
 * loads the result. The gap is that the output cannot be shipped: a converted
 * pck contains MegaCrit's art. Converting on the user's own console from the
 * user's own copy is the only distributable form of the same fix.
 *
 * WHAT IT IS FIXING
 * =================
 * sts2.pck is a desktop export. By count:
 *
 *     RGBA8       1532   459 Mpx   uncompressed
 *     RGB8         707   473 Mpx   uncompressed
 *     BPTC (BC7)   683   701 Mpx   PC-only, expanded to RGBA8 at load
 *     DXT5/DXT1    539   229 Mpx   PC-only, expanded to RGBA8 at load
 *
 * ~7 GB of texture data on a console with ~3.2 GB. ASTC 8x8 is 0.25 B/px and
 * Tegra X1 samples it in hardware: the same textures come to ~444 MB.
 *
 * WHAT THIS FILE DOES TODAY
 * =========================
 * The pck plumbing and the census, and nothing it cannot do correctly:
 *
 *   - parses the GDPC container and its file table
 *   - identifies every CompressedTexture2D (GST2) and reads its real format
 *   - reports the totals, on screen and in the log
 *
 * It does NOT encode yet. The encoder and the four decoders it needs are the
 * next step, and writing a half-encoder that silently produced wrong pixels
 * would be worse than reporting honestly that it is not there. The census is
 * independently useful: it confirms on hardware that the format distribution
 * matches what the desktop script measured, using this pck rather than a
 * developer's copy.
 *
 * Gated on config.astc_bake (default 0).
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <switch.h>   /* appletSetCpuBoostMode */

#include "astc_bake.h"
#include "astc_decode.h"
#include "astc_encode.h"
#include "startup_status.h"
#include "util.h"
#include "config.h"

/* Godot Image::Format ids, as they appear in this pck. */
#define F_L8   0
#define F_LA8  1
#define F_R8   2
#define F_RG8  3
#define F_RGB8 4
#define F_RGBA8 5
#define F_DXT1 17
#define F_DXT3 18
#define F_DXT5 19
#define F_BPTC_RGBA 22
#define F_ASTC_4x4 35
#define F_ASTC_8x8 37

#define FORMAT_MASK ((1u << 20) - 1u)   /* Godot's own mask */

/* CompressedTexture2D layout, from the shipped files rather than the docs:
 * "GST2" magic, then a fixed header, then the image record at IMG_OFF and its
 * payload at DATA_OFF. */
/* Verified against a real converted pck, not guessed. A 500x500 ASTC 8x8
 * texture is ceil(500/8)^2 * 16 + 52 = 63556 bytes, which is exactly its
 * directory size -- so the payload starts at 52 and the image record at 36.
 *
 * The first version of this file had 32 and 56, from reading the struct rather
 * than measuring a file. At 32 the width and height both read as 0, and every
 * texture would have been silently "skipped (w=0 h=0)" -- a census reporting
 * zero convertible textures, with no error anywhere. */
#define IMG_OFF  36
#define DATA_OFF 52

static const char *fmt_name(uint32_t f) {
  switch (f) {
    case F_L8: return "L8";
    case F_LA8: return "LA8";
    case F_R8: return "R8";
    case F_RG8: return "RG8";
    case F_RGB8: return "RGB8";
    case F_RGBA8: return "RGBA8";
    case F_DXT1: return "DXT1";
    case F_DXT3: return "DXT3";
    case F_DXT5: return "DXT5";
    case F_BPTC_RGBA: return "BPTC";
    case F_ASTC_4x4: return "ASTC4x4";
    case F_ASTC_8x8: return "ASTC8x8";
    default: return "other";
  }
}

/* Would converting this format gain anything? ASTC input is already done, and
 * a format we cannot decode is not a candidate however large it is. */
static int is_candidate(uint32_t f) {
  switch (f) {
    case F_L8: case F_LA8: case F_R8: case F_RG8: case F_RGB8: case F_RGBA8:
    case F_DXT1: case F_DXT3: case F_DXT5: case F_BPTC_RGBA:
      return 1;
    default:
      return 0;
  }
}

/* ONE entry at a time, read straight from the file.
 *
 * The first version materialised the whole directory: 15658 entries of a
 * 528-byte struct is 8.2 MB, and the conversion pass allocated a second one --
 * 16.5 MB of spike during boot, on the console whose memory pressure is the
 * entire reason this feature exists.
 *
 * It also crashed. The fault landed between the scan finishing and the census
 * printing, which is exactly where those buffers are freed, and the crash
 * handler then looped 11,647 times without ever reporting a cause.
 *
 * Streaming removes the allocation rather than explaining it. The directory is
 * walked twice -- once to count, once to convert -- which costs two sequential
 * reads of a few hundred KB and nothing else. */
typedef struct {
  FILE    *f;
  uint64_t file_base;
  uint64_t dir_pos;      /* first byte of the first directory record */
  uint32_t count;
  uint32_t ver;
} pck_reader;

typedef struct {
  char     path[512];
  uint64_t off;          /* absolute, file_base already added */
  uint64_t size;
} pck_entry;

/* Reads the header and leaves the reader positioned at the directory. */
static int pck_open(const char *path, pck_reader *r) {
  memset(r, 0, sizeof(*r));
  r->f = fopen(path, "rb");
  if (!r->f) { debugPrintf("[astc] cannot open %s\n", path); return 0; }

  char magic[4];
  if (fread(magic, 1, 4, r->f) != 4 || memcmp(magic, "GDPC", 4)) {
    debugPrintf("[astc] not a .pck (no GDPC magic)\n");
    fclose(r->f); r->f = NULL; return 0;
  }

  uint32_t maj, mnr, patch, flags;
  if (fread(&r->ver, 4, 1, r->f) != 1 || fread(&maj, 4, 1, r->f) != 1 ||
      fread(&mnr, 4, 1, r->f) != 1 || fread(&patch, 4, 1, r->f) != 1 ||
      fread(&flags, 4, 1, r->f) != 1 || fread(&r->file_base, 8, 1, r->f) != 1) {
    fclose(r->f); r->f = NULL; return 0;
  }

  uint64_t dir_off = 100;
  if (r->ver >= 3) {
    if (fread(&dir_off, 8, 1, r->f) != 1) { fclose(r->f); r->f = NULL; return 0; }
  }
  if (fseek(r->f, (long)dir_off, SEEK_SET) != 0) { fclose(r->f); r->f = NULL; return 0; }
  if (fread(&r->count, 4, 1, r->f) != 1) { fclose(r->f); r->f = NULL; return 0; }

  if (r->count == 0 || r->count > 200000) {
    debugPrintf("[astc] implausible entry count %u -- refusing\n", r->count);
    fclose(r->f); r->f = NULL; return 0;
  }
  r->dir_pos = (uint64_t)ftell(r->f);
  debugPrintf("[astc] pck v%u godot %u.%u.%u, %u entries\n",
              r->ver, maj, mnr, patch, r->count);
  return 1;
}

static void pck_rewind(pck_reader *r) { fseek(r->f, (long)r->dir_pos, SEEK_SET); }
static void pck_close(pck_reader *r) { if (r->f) fclose(r->f); r->f = NULL; }

/* Reads the next directory record from wherever the file currently is.
 * Callers that seek away to read payload must restore the position first --
 * pck_tell/pck_seek exist for exactly that. */
static int pck_next(pck_reader *r, pck_entry *e) {
  uint32_t nlen = 0;
  if (fread(&nlen, 4, 1, r->f) != 1) return 0;
  if (nlen == 0 || nlen >= sizeof(e->path)) {
    debugPrintf("[astc] bad name length %u -- stopping the walk\n", nlen);
    return 0;
  }
  if (fread(e->path, 1, nlen, r->f) != nlen) return 0;
  e->path[nlen] = 0;

  uint8_t md5[16];
  if (fread(&e->off, 8, 1, r->f) != 1 || fread(&e->size, 8, 1, r->f) != 1 ||
      fread(md5, 1, 16, r->f) != 16) return 0;
  if (r->ver >= 2) { uint32_t ef; if (fread(&ef, 4, 1, r->f) != 1) return 0; }

  e->off += r->file_base;
  return 1;
}

static long pck_tell(pck_reader *r) { return ftell(r->f); }
static void pck_seek(pck_reader *r, long p) { fseek(r->f, p, SEEK_SET); }

static int ends_with(const char *s, const char *suffix) {
  const size_t n = strlen(s), m = strlen(suffix);
  return n >= m && !strcmp(s + n - m, suffix);
}

/* Godot writes the directory name field padded to a 4-byte multiple. Matching
 * that matters: Godot's own reader trusts the length and would otherwise read
 * a name that runs into the next record's offset. */
static uint32_t padded_name_len(const char *p) {
  const uint32_t n = (uint32_t)strlen(p);
  return (n + 3u) & ~3u;
}


void astc_bake_run(const char *pck_path) {
  if (!config.astc_bake) return;

  /* Already built? The whole point is that this happens once.
   *
   * CHECK EVERY PLACE THE LOADER ACCEPTS, not just the one we write to.
   * AstcOverlay.cs mounts the first of three candidates it finds:
   *
   *     user://sts2_astc.pck            <- save_root; main.c:1627 maps
   *                                        user:// to it via jni_fake's
   *                                        getDataDir
   *     user://default/1/sts2_astc.pck  <- beside settings.save
   *     res://sts2_astc.pck             <- beside sts2.pck
   *
   * Checking only the first would mean a pck placed in either of the other
   * two -- by hand, or converted on a PC with tools/astc_convert.py -- gets
   * loaded by the engine AND rebuilt from scratch by us. Twenty minutes of
   * conversion to produce a file that was already there and already in use.
   *
   * The lists must match, so this one names the same three and says which it
   * found. */
  char out_path[512];
  snprintf(out_path, sizeof(out_path), "%s/sts2_astc.pck", config.save_root);

  {
    char cand[3][512];
    snprintf(cand[0], sizeof(cand[0]), "%s/sts2_astc.pck", config.save_root);
    snprintf(cand[1], sizeof(cand[1]), "%s/default/1/sts2_astc.pck", config.save_root);
    snprintf(cand[2], sizeof(cand[2]), "%s/sts2_astc.pck", config.data_root);

    for (int c = 0; c < 3; c++) {
      FILE *probe = fopen(cand[c], "rb");
      if (!probe) continue;

      /* Size it, and sanity-check the magic. A zero-byte or truncated file
       * from an interrupted bake would otherwise skip the rebuild forever
       * while the engine failed to mount it -- the worst of both. */
      char magic[4] = {0};
      const size_t got = fread(magic, 1, 4, probe);
      fseek(probe, 0, SEEK_END);
      const long sz = ftell(probe);
      fclose(probe);

      if (got != 4 || memcmp(magic, "GDPC", 4) || sz < 1024) {
        debugPrintf("[astc] %s exists but is not a usable pck "
                    "(%ld bytes, magic %.4s) -- ignoring it\n",
                    cand[c], sz, got == 4 ? magic : "????");
        continue;
      }

      debugPrintf("[astc] found %s (%ld MB) -- skipping the bake.\n"
                  "[astc] Delete it to rebuild.\n", cand[c], sz >> 20);
      return;
    }
  }

  FILE *f = fopen(pck_path, "rb");
  if (!f) {
    debugPrintf("[astc] cannot open %s -- nothing to convert\n", pck_path);
    return;
  }

  /* ONE pass. Open the pck, convert, tally as we go.
   *
   * There used to be two: a census walk over all 15658 entries, then a rewind
   * and a second walk to convert. That was wrong in three ways.
   *
   * The census cost a full directory walk with a seek per texture -- tens of
   * seconds of SD I/O -- to produce numbers that were only logged.
   *
   * The progress bar reported the SCAN, which is not the work. The user sees a
   * bar run to 100%, then nothing on screen for the minutes that actually
   * matter. That was the reported symptom, and it is a design fault rather
   * than a display bug.
   *
   * And it opened and closed the console TWICE. The crash lands on the first
   * consoleExit, so the wasted pass was also carrying the failure.
   *
   * The same numbers are still reported -- they are just counted from the
   * conversion instead of from a separate walk that had to agree with it. */
  pck_reader r;
  if (!pck_open(pck_path, &r)) return;

  const unsigned bx = (config.astc_block == 4) ? 4u
                    : (config.astc_block == 6) ? 6u : 8u;
  if (!astc_encode_init(bx, bx)) {
    debugPrintf("[astc] encoder unavailable -- nothing written\n");
    pck_close(&r);
    return;
  }

  char tmp_path[512];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
  remove(tmp_path);

  FILE *out = fopen(tmp_path, "wb");
  if (!out) {
    debugPrintf("[astc] cannot create %s\n", tmp_path);
    pck_close(&r); astc_encode_shutdown(); return;
  }

  /* Header first, dir_off patched once known. Replicated byte-for-byte from a
   * pck the desktop script produced and the engine accepted: 40 bytes of
   * fields, 72 zero bytes, payload from 112. */
  unsigned char head[112];
  memset(head, 0, sizeof(head));
  memcpy(head, "GDPC", 4);
  const uint32_t pack_ver = 3, g_maj = 4, g_min = 5, g_pat = 1, pflags = 2;
  const uint64_t fbase = 112;
  memcpy(head + 4,  &pack_ver, 4);  memcpy(head + 8,  &g_maj, 4);
  memcpy(head + 12, &g_min, 4);     memcpy(head + 16, &g_pat, 4);
  memcpy(head + 20, &pflags, 4);    memcpy(head + 24, &fbase, 8);
  fwrite(head, 1, sizeof(head), out);

  uint32_t  cap = 4096, done = 0;
  uint64_t *ooff = (uint64_t *)malloc(cap * sizeof(uint64_t));
  uint64_t *osz  = (uint64_t *)malloc(cap * sizeof(uint64_t));
  uint32_t *opth = (uint32_t *)malloc(cap * sizeof(uint32_t));
  size_t    pool_cap = 1 << 20, pool_len = 0;
  char     *pool = (char *)malloc(pool_cap);
  if (!ooff || !osz || !opth || !pool) {
    debugPrintf("[astc] out of memory for the output directory\n");
    free(ooff); free(osz); free(opth); free(pool);
    fclose(out); pck_close(&r); astc_encode_shutdown(); return;
  }

  uint32_t cnt[64];  memset(cnt, 0, sizeof(cnt));
  uint64_t px[64];   memset(px, 0, sizeof(px));
  uint32_t textures = 0, skipped_mips = 0, failed = 0;
  uint64_t in_bytes = 0, out_bytes = 0;
  uint8_t *blob = NULL; size_t blob_cap = 0;
  pck_entry e;

  /* CPU boost, for this loop and nothing else.
   *
   * ApmCpuBoostMode_FastLoad takes the A57s from 1020 MHz to 1785 MHz -- about
   * 1.75x, and ASTC encoding is exactly the pure-CPU work that converts into
   * wall-clock directly.
   *
   * IT ALSO THROTTLES THE GPU TO MINIMUM. libnx's own comment on the enum says
   * so: "Boost CPU. Additionally, throttle GPU to minimum." That is free here
   * -- nothing renders during the bake, the progress console is a CPU blit --
   * but it is the reason this must not outlive the loop. Leaving it on would
   * hand the engine a GPU pinned at its lowest clock, and after everything
   * this port has been through with the GPU, that would be attributed to
   * anything but a boost flag set during a texture conversion.
   *
   * Restored immediately after the loop, which is airtight because the loop
   * contains no `return` -- every failure path inside it is `continue` or
   * `break`.
   *
   * Nintendo intends this for short loading periods. A multi-minute bake is
   * longer than that, so it will run warm and drain battery faster. For a
   * one-time conversion that is the right trade; for anything recurring it
   * would not be. */
  const int boosted = R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad));
  debugPrintf("[astc] CPU boost %s (GPU throttled to minimum while it lasts)\n",
              boosted ? "ON -- 1020 -> 1785 MHz" : "REFUSED; encoding at 1020 MHz");

  startup_status_begin("Converting textures to ASTC...");

  for (uint32_t i = 0; i < r.count; i++) {
    if (!pck_next(&r, &e)) break;
    const long dir_next = pck_tell(&r);

    /* The percentage tracks directory position, which is honest but uneven:
     * only ~3.5k of 15658 entries are textures, so the bar sprints through
     * scripts and crawls through art. Showing the converted count alongside it
     * gives the one number that reflects the actual work. */
    {
      char title[64];
      snprintf(title, sizeof(title), "Converting to ASTC... (%u done)", done);
      startup_status_update(title, e.path,
                            (int)(((uint64_t)i * 100) / r.count));
    }

    if (!ends_with(e.path, ".ctex") || e.size < DATA_OFF ||
        e.size > (64u << 20)) continue;

    if (e.size > blob_cap) {
      uint8_t *nb = (uint8_t *)realloc(blob, (size_t)e.size);
      if (!nb) { failed++; continue; }
      blob = nb; blob_cap = (size_t)e.size;
    }
    pck_seek(&r, (long)e.off);
    const int got = (fread(blob, 1, (size_t)e.size, r.f) == (size_t)e.size);
    pck_seek(&r, dir_next);
    if (!got) { failed++; continue; }
    if (memcmp(blob, "GST2", 4)) continue;

    uint32_t data_format, w16h16, mips, raw_fmt;
    memcpy(&data_format, blob + IMG_OFF, 4);
    memcpy(&w16h16, blob + IMG_OFF + 4, 4);
    memcpy(&mips, blob + IMG_OFF + 8, 4);
    memcpy(&raw_fmt, blob + IMG_OFF + 12, 4);
    const unsigned w = w16h16 & 0xFFFFu, h = (w16h16 >> 16) & 0xFFFFu;
    const uint32_t fmt = raw_fmt & FORMAT_MASK;

    /* Census, counted from the work itself rather than a separate walk that
     * then had to agree with it. */
    textures++;
    cnt[fmt < 64 ? fmt : 63]++;
    px[fmt < 64 ? fmt : 63] += (uint64_t)w * h;

    if (!w || !h) continue;
    if (mips != 0) { skipped_mips++; continue; }
    if (!is_candidate(fmt)) continue;

    const char *why = NULL;
    uint8_t *rgba = astc_decode_to_rgba(blob + DATA_OFF,
                                        (size_t)e.size - DATA_OFF,
                                        data_format, fmt, w, h, &why);
    if (!rgba) {
      if (failed < 16) debugPrintf("[astc] %s: %s\n", e.path, why ? why : "?");
      failed++; continue;
    }

    const size_t csize = astc_encode_size(w, h, bx, bx);
    uint8_t *comp = (uint8_t *)malloc(csize);
    if (!comp || !astc_encode_rgba(rgba, w, h, comp, csize)) {
      free(rgba); free(comp); failed++; continue;
    }
    free(rgba);

    const uint32_t d_image = 0;
    const uint32_t new_fmt = (bx == 4) ? F_ASTC_4x4 : F_ASTC_8x8;
    memcpy(blob + IMG_OFF, &d_image, 4);
    const uint32_t merged = (raw_fmt & ~FORMAT_MASK) | new_fmt;
    memcpy(blob + IMG_OFF + 12, &merged, 4);

    if (done == cap) {
      const uint32_t nc = cap * 2;
      uint64_t *a1 = (uint64_t *)realloc(ooff, nc * sizeof(uint64_t));
      uint64_t *a2 = (uint64_t *)realloc(osz,  nc * sizeof(uint64_t));
      uint32_t *a3 = (uint32_t *)realloc(opth, nc * sizeof(uint32_t));
      if (!a1 || !a2 || !a3) { free(comp); failed++; break; }
      ooff = a1; osz = a2; opth = a3; cap = nc;
    }
    const size_t plen = strlen(e.path) + 1;
    if (pool_len + plen > pool_cap) {
      const size_t np = pool_cap * 2 + plen;
      char *pp = (char *)realloc(pool, np);
      if (!pp) { free(comp); failed++; break; }
      pool = pp; pool_cap = np;
    }
    memcpy(pool + pool_len, e.path, plen);
    opth[done] = (uint32_t)pool_len;
    pool_len += plen;

    ooff[done] = (uint64_t)ftell(out) - fbase;
    osz[done]  = (uint64_t)DATA_OFF + csize;
    fwrite(blob, 1, DATA_OFF, out);
    fwrite(comp, 1, csize, out);
    free(comp);

    in_bytes  += (uint64_t)w * h * 4u;
    out_bytes += osz[done];
    done++;
  }
  free(blob);

  /* Off, before anything else can run. Not at the end of the function, and not
   * on the caller's good behaviour: the next thing main() does is hand the
   * window to the renderer. */
  if (boosted) {
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    debugPrintf("[astc] CPU boost OFF (normal clocks restored)\n");
  }

  /* Directory last, then patch dir_off. */
  const uint64_t dir_at = (uint64_t)ftell(out);
  fwrite(&done, 4, 1, out);
  for (uint32_t i = 0; i < done; i++) {
    const char *path = pool + opth[i];
    const uint32_t raw = (uint32_t)strlen(path);
    const uint32_t plen = padded_name_len(path);
    unsigned char md5[16]; memset(md5, 0, sizeof(md5));
    const uint32_t eflags = 0;
    fwrite(&plen, 4, 1, out);
    fwrite(path, 1, raw, out);
    for (uint32_t k = raw; k < plen; k++) fputc(0, out);
    fwrite(&ooff[i], 8, 1, out);
    fwrite(&osz[i], 8, 1, out);
    fwrite(md5, 1, 16, out);
    fwrite(&eflags, 4, 1, out);
  }
  fseek(out, 32, SEEK_SET);
  fwrite(&dir_at, 8, 1, out);

  fclose(out);
  pck_close(&r);
  free(ooff); free(osz); free(opth); free(pool);
  astc_encode_shutdown();
  startup_status_end();

  if (done == 0) {
    debugPrintf("[astc] converted nothing -- leaving no pck behind\n");
    remove(tmp_path);
    return;
  }
  remove(out_path);
  if (rename(tmp_path, out_path) != 0) {
    debugPrintf("[astc] could not rename %s -> %s; the bake is wasted\n",
                tmp_path, out_path);
    return;
  }

  debugPrintf("[astc] === what was in %s ===\n", pck_path);
  debugPrintf("[astc] %u CompressedTexture2D of %u entries\n", textures, r.count);
  for (uint32_t k = 0; k < 64; k++) {
    if (!cnt[k]) continue;
    debugPrintf("[astc]   %-8s %5u   %6llu Mpx\n",
                fmt_name(k), cnt[k], (unsigned long long)(px[k] / 1000000));
  }
  if (skipped_mips)
    debugPrintf("[astc] %u texture(s) have mipmaps and would need a mip loop\n",
                skipped_mips);

  debugPrintf("[astc] wrote %s: %u textures, %llu MB -> %llu MB (%u failed)\n",
              out_path, done,
              (unsigned long long)(in_bytes >> 20),
              (unsigned long long)(out_bytes >> 20), failed);
}
