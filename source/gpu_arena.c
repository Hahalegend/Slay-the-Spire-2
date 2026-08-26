/* gpu_arena.c -- a dedicated pool for libdrm_nouveau's GPU buffers.
 *
 * Adapted from fruitninja_nx's nx_alloc.c, whose comment describes the failure
 * this exists to prevent:
 *
 *   libdrm_nouveau backs every GPU buffer with memalign(0x1000, size). Those
 *   are large (up to ~22 MB), page-aligned, and churned constantly, so serving
 *   them from newlib's general heap SHREDS it: once fragmented, a 21 MB
 *   CONTIGUOUS page-aligned run cannot be placed even with a gigabyte free in
 *   aggregate, and memalign returns NULL -> nouveau_bo_new fails -> texStorage
 *   GL_OUT_OF_MEMORY -> incomplete FBO -> black screen.
 *
 * Two things that took hardware to learn. GL_OUT_OF_MEMORY here means the CPU
 * heap failed, not a separate GPU pool -- nouveau has no memory of its own. And
 * the problem is FRAGMENTATION, not exhaustion: freeing memory elsewhere raises
 * the ceiling but does nothing about a churning 20 MB allocator carving the
 * heap into pieces too small to serve itself.
 *
 * ===========================================================================
 * ONE STATIC POOL, deliberately
 * ===========================================================================
 *
 * An earlier version grew in chunks, to avoid having to guess the size. I
 * introduced two races in that code: __wrap_free() walked the chunk array while
 * gpua_add_chunk() mutated it, and gpua_init() raced with itself across
 * threads. Both produced heap corruption that surfaced somewhere unrelated, and
 * each cost a test cycle.
 *
 * The size is no longer a guess. Run 27 measured peak demand at 1653 MB, of
 * which ~256 MB was the GC reservation that should never have been in here
 * (mmap_fake bypasses the pool now), leaving ~1400 MB of real GPU buffers.
 *
 * With one pool, allocated once and never resized, there is no chunk array to
 * mutate and the whole class of race is gone rather than fixed case by case.
 * If the size is short, the fallback counter says so and `gpu_arena_mb` raises
 * it -- a better trade than re-introducing growth.
 *
 * ===========================================================================
 * How the interception works
 * ===========================================================================
 *
 * libdrm_nouveau and mesa are linked INTO the NRO, so their memalign calls do
 * not pass through the import table -- that only serves the .so modules we load
 * ourselves. The linker's --wrap is the only way to see them (see the Makefile).
 *
 * Only memalign and free are wrapped. fruitninja_nx wraps seven; each one is a
 * chance to break something unrelated, and nouveau_bo_new uses these two.
 *
 * `gpu_arena_mb 0` passes everything through to newlib, which is exactly the
 * pre-pool behaviour. Wrapping free() is invasive enough to deserve that.
 *
 * MIT license; see LICENSE. */

#include <malloc.h>
#include <stdio.h>    // snprintf, for the size histogram
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"

// libnx sets these when it initialises the newlib heap. Used to bound the
// canary's header read: __wrap_free sees every pointer in the process, and
// reading 32 bytes below one that did not come from us is what faulted on the
// first run with heap_canary 1.
extern char *fake_heap_start;
extern char *fake_heap_end;
#include "gpu_arena.h"
#include "lock_trace.h"

void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);
void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_memalign(size_t align, size_t size);
void  __real_free(void *p);
void *__real_realloc(void *p, size_t size);
size_t __real_malloc_usable_size(void *p);

#define GPUA_PAGE  0x1000u

// Below this much free newlib heap after the arena is reserved, warn loudly.
// 256 MB is not a tuned figure -- it is "obviously too little", chosen so the
// warning fires on a genuinely broken configuration rather than a tight one.
#define GPUA_NEWLIB_FLOOR_MB 256u

// Below this an allocation is not a GPU buffer worth pooling -- it is some
// incidental page-aligned request, and serving it here would fragment the pool
// instead of the heap. mmap_fake, the .NET TLS block and the OBB buffers all
// call __real_memalign directly to stay out regardless of size.
#define GPUA_MIN   (64u * 1024)

static uint8_t  *g_base;
static size_t    g_pages;
static uint8_t  *g_used;      // 1 byte per page
static uint32_t *g_runlen;    // run length, recorded at the run's first page
static size_t    g_hint;      // next-fit cursor
static Mutex     g_lock;      // zero-initialised is valid for libnx; see below
// 0 = untried, 1 = ready, -1 = disabled.
//
// ATOMIC, because it is read outside the lock on the fast paths in
// gpua_alloc() and __wrap_free() and written under it during init.
//
// I first reasoned that was benign -- an aligned int cannot tear, and the only
// values are 0/1/-1. ThreadSanitizer disagreed, and after two "benign" races
// in this file that both turned into heap corruption on hardware, the argument
// is not worth having. Acquire/release makes the publication of g_base and
// friends properly ordered against the readers, at no measurable cost.
static int       g_state;

static size_t    g_live_pages, g_peak_pages;
static uint64_t  g_n_alloc, g_n_free, g_n_fallback;

// Where the memory actually goes, by request size.
//
// "The GPU pool needs a gigabyte" is not actionable. Forty 22 MB atlases and
// forty thousand 64 KB buffers both come to a gigabyte and need opposite
// fixes: one is texture resolution, the other is batching or a leak.
//
// Buckets double from 64 KB, so bucket i covers [64KB << i, 64KB << (i+1)).
#define GPUA_BUCKETS 12
static uint64_t g_hist_n[GPUA_BUCKETS];    // allocations in this bucket
static uint64_t g_hist_live[GPUA_BUCKETS]; // still live, to spot what is held

// The largest live allocations, which is usually the whole story for a 2D game.
#define GPUA_TOPN 8
static size_t g_top_bytes[GPUA_TOPN];

// NEVER call debugPrintf while holding g_lock -- with one reasoned exception.
//
// Logging writes to the SD card and can allocate, which would re-enter
// __wrap_memalign and take this same lock. libnx mutexes are not recursive, so
// that is an instant self-deadlock.
//
// The exception is gpua_init_locked() below, which logs its one-time result
// with the lock held. That is safe HERE and only here, because re-entry
// requires an allocation matching this pool's filter -- page-aligned AND at
// least 64 KB -- and stdio never makes one. gpu_arena_report() snapshots and
// prints outside the lock rather than rely on that.

// Called with g_lock HELD.
//
// Locking around init matters: __wrap_memalign is called from several threads,
// and an unlocked version let two of them both see g_state == 0 and both
// allocate the pool. libnx's Mutex is a zero-initialised word, so the static
// g_lock is already valid without mutexInit -- which is what makes locking
// around init possible at all, rather than the lock coming into existence after
// the race it is meant to prevent.
static void gpua_init_locked(void) {
  if (g_state) return;   // under the lock; a plain read is fine here

  if (!config.gpu_arena_mb) {
    __atomic_store_n(&g_state, -1, __ATOMIC_RELEASE);
    debugPrintf("[gpua] disabled (gpu_arena_mb 0); GPU buffers use newlib\n");
    return;
  }

  // Taken ONCE, at first use, before the heap has been churned. Later is too
  // late: the contiguous run this needs is what fragmentation destroys.
  const size_t want  = (size_t)config.gpu_arena_mb * 1024u * 1024u;
  const size_t pages = want / GPUA_PAGE;

  uint8_t  *base   = __real_memalign(GPUA_PAGE, want);
  uint8_t  *used   = base ? __real_memalign(16, pages) : NULL;
  uint32_t *runlen = used ? __real_memalign(16, pages * sizeof(uint32_t)) : NULL;

  if (!base || !used || !runlen) {
    if (runlen) __real_free(runlen);
    if (used)   __real_free(used);
    if (base)   __real_free(base);
    __atomic_store_n(&g_state, -1, __ATOMIC_RELEASE);
    debugPrintf("[gpua] could not reserve %u MB; GPU buffers use newlib\n",
                config.gpu_arena_mb);
    return;
  }

  memset(used, 0, pages);
  memset(runlen, 0, pages * sizeof(uint32_t));

  g_base = base; g_pages = pages; g_used = used; g_runlen = runlen; g_hint = 0;

  // RELEASE: everything above must be visible to any thread that later sees 1.
  __atomic_store_n(&g_state, 1, __ATOMIC_RELEASE);

  // What the pool COST, not just what it is.
  //
  // The pool is one enormous memalign out of newlib's heap, and everything
  // else -- the engine, textures outside the filter, the managed runtime --
  // comes from what is left. Run 24 played for twenty minutes with 768 MB
  // here; the early crashes began when it went to 1500 MB, and the log never
  // showed the headroom that bought or cost.
  struct mallinfo mi = mallinfo();
  // mem_used, not `used`: `used` is already the page bitmap in this scope, and
  // shadowing it here would compile on target and mean something else entirely.
  uint64_t mem_total = 0, mem_used = 0;
  svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&mem_used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

  debugPrintf("[gpua] pool %u MB at %p (%zu pages), min alloc %u KB\n"
              "[gpua] after reserving it: process %llu/%llu MB, "
              "newlib free %u MB\n",
              config.gpu_arena_mb, (void *)base, pages, GPUA_MIN >> 10,
              (unsigned long long)(mem_used >> 20), (unsigned long long)(mem_total >> 20),
              (unsigned)(mi.fordblks >> 20));

  // Say so when the arena has eaten the heap.
  //
  // These numbers were already printed and still cost a hardware run:
  //
  //     [gpua] pool 900 MB ... newlib free 16 MB
  //     [gpua] live 97 MB, peak 97 MB of 900 MB; 28 allocs, 0 frees
  //
  // 900 MB reserved, 97 ever used, and **16 MB left for everything else** --
  // every texture, every engine object, every managed allocation. Printing the
  // figure is not the same as flagging it, and a line that reads like the
  // other status lines gets skimmed like the other status lines.
  //
  // Not clamped, because a deliberate large arena is a legitimate experiment
  // and silently shrinking it would be its own trap. Just made impossible to
  // miss.
  {
    const unsigned free_mb = (unsigned)(mi.fordblks >> 20);
    if (free_mb < GPUA_NEWLIB_FLOOR_MB) {
      debugPrintf("[gpua] *** WARNING: only %u MB left for newlib. The arena "
                  "took %u MB.\n"
                  "[gpua] *** Everything that is not a GPU buffer comes out of "
                  "that %u MB --\n"
                  "[gpua] *** textures, engine objects, the managed heap. "
                  "Allocation failures\n"
                  "[gpua] *** from here are the arena's doing, not a leak. "
                  "Lower gpu_arena_mb.\n",
                  free_mb, config.gpu_arena_mb, free_mb);
      debugFlush();
    }
  }
}

static void *gpua_alloc(size_t sz) {
  // Terminal state, checked before the lock. Once disabled the pool never
  // re-enables, so a stale read is harmless -- and without this every large
  // aligned allocation would lock for nothing when gpu_arena_mb is 0.
  if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == -1) return NULL;

  const size_t need = (sz + GPUA_PAGE - 1) / GPUA_PAGE;
  if (!need) return NULL;

  void *out = NULL;
  mutexLock(&g_lock);

  gpua_init_locked();
  if (g_state != 1 || need > g_pages) {   // under the lock
    mutexUnlock(&g_lock);
    return NULL;
  }

  // Next-fit from the hint, then wrap. Next-fit rather than first-fit because
  // these buffers churn constantly, and first-fit would re-test the same
  // crowded prefix on every allocation.
  for (int pass = 0; pass < 2 && !out; pass++) {
    size_t i   = pass ? 0 : g_hint;
    size_t end = pass ? g_hint : g_pages;

    while (i + need <= end) {
      size_t run = 0;
      while (run < need && !g_used[i + run]) run++;

      if (run == need) {
        for (size_t k = 0; k < need; k++) g_used[i + k] = 1;
        g_runlen[i] = (uint32_t)need;
        g_hint = i + need;
        if (g_hint >= g_pages) g_hint = 0;
        out = g_base + i * GPUA_PAGE;
        break;
      }
      i += run + 1;   // skip past the blocker, not one page at a time
    }
  }

  if (out) {
    g_live_pages += need;
    if (g_live_pages > g_peak_pages) g_peak_pages = g_live_pages;
    g_n_alloc++;

    unsigned b = 0;
    for (size_t t = sz >> 16; t && b < GPUA_BUCKETS - 1; t >>= 1) b++;
    g_hist_n[b]++;
    g_hist_live[b]++;

    // Insertion into a sorted top-N. Eight entries, so a linear pass is
    // cheaper than anything cleverer and cannot get the order wrong.
    for (int t = 0; t < GPUA_TOPN; t++) {
      if (sz > g_top_bytes[t]) {
        for (int k = GPUA_TOPN - 1; k > t; k--) g_top_bytes[k] = g_top_bytes[k - 1];
        g_top_bytes[t] = sz;
        break;
      }
    }
  } else {
    // Counted under the lock. Unlocked increments are lost under contention,
    // and this is the number the pool is sized by.
    g_n_fallback++;
  }

  mutexUnlock(&g_lock);
  return out;
}

// ---------------------------------------------------------------------------

void gpu_arena_preinit(void) {
  // Take the pool BEFORE anything else has used the heap.
  //
  // Lazy init sounds harmless and is not: the first large page-aligned
  // allocation happens after so_load has read and relocated ~130 MB of
  // modules and freed the transients, so by then the heap is churned and the
  // pool needs a contiguous run that may no longer exist as one piece.
  //
  // That is the likely reason boot fails at 1150 MB while combat needs more
  // than 1000: not that the memory is absent, but that no single run of it is
  // left. Claiming it first, out of a fresh heap, is free.
  //
  // Still idempotent, so the lazy path remains as a fallback if this is ever
  // not reached.
  mutexLock(&g_lock);
  gpua_init_locked();
  mutexUnlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Heap canaries -- optional, off by default, `heap_canary 1` in config.txt.
//
// The remaining crash is not exhaustion. Run 31 with the pool disabled died in
// _malloc_r+0x448 with 438 MB free and zero GL_OUT_OF_MEMORY: newlib walked a
// corrupted free list. malloc is where corruption is DETECTED, never where it
// happens -- the write that did the damage may be thousands of allocations
// earlier, which is why every crash so far has been at a different site.
//
// A canary moves detection to the free() of the block that was overrun, which
// is usually close enough to the culprit to name it.
//
// Each canaried allocation gets an extra 32 bytes: a header holding the
// requested size and a magic, and a trailing magic. Overwriting either end is
// then caught and logged with the block's size and address rather than
// crashing somewhere unrelated later.
//
// It is off by default because it changes every allocation's layout and costs
// memory. It is a debugging tool, not a shipping mode.
#define CANARY_HEAD  32u
#define CANARY_MAGIC 0x5AFEC0DEDEADBEEFull

static uint64_t g_canary_checked, g_canary_bad;

// Declared HERE, with the other counters, not next to the copy guard that
// increments it -- heap_canary_report() reads it and sits ~50 lines above
// where the guard is defined.
static uint64_t g_copy_bad;


// The user pointer must keep the alignment the caller asked for, so the header
// is placed a full `align` below it rather than a flat 32 bytes. Returning
// raw+32 from a memalign(0x1000, ...) would hand back an unaligned pointer,
// which nouveau and anything else using page-aligned buffers would then use as
// if it were aligned.
static size_t canary_offset(size_t align) {
  return align > CANARY_HEAD ? align : CANARY_HEAD;
}

// The magic is tied to the address it was written for.
//
// A plain constant is not enough. A freed canaried block leaves its magic in
// memory; when malloc reuses that memory, a NON-canaried pointer sitting 32
// bytes above the stale magic passes the check, and h[3] -- long-dead data --
// gets handed to __real_free. The canary would then corrupt the heap it exists
// to protect, intermittently, on reuse.
//
// Mixing the user pointer in means a header only ever validates for the exact
// block it was written for.
static uint64_t canary_magic_for(const void *user) {
  return CANARY_MAGIC ^ (uint64_t)(uintptr_t)user;
}

// Is it safe to READ the header at all?
//
// __wrap_free sees every pointer in the process, not just ours: malloc'd
// blocks, blocks from before the flag was read, statics. Reading 32 bytes below
// an arbitrary pointer is what crashed on the first run with heap_canary 1 --
// below the heap base, or on an unmapped page, that read faults.
static int canary_readable(const void *user) {
  const uintptr_t h = (uintptr_t)user - CANARY_HEAD;
  return h >= (uintptr_t)fake_heap_start && (uintptr_t)user <= (uintptr_t)fake_heap_end;
}

static void *canary_wrap(void *raw, size_t size, size_t align) {
  if (!raw) return NULL;
  uint8_t *user = (uint8_t *)raw + canary_offset(align);
  uint64_t *h = (uint64_t *)(user - CANARY_HEAD);
  const uint64_t magic = canary_magic_for(user);
  h[0] = magic;
  h[1] = (uint64_t)size;
  h[2] = magic;
  h[3] = (uint64_t)(uintptr_t)raw;   // what __real_free must be given
  uint64_t tail = magic;
  memcpy(user + size, &tail, sizeof tail);
  return user;
}

// Does this look canaried, and if so how big was the request? Read-only: used
// by realloc and malloc_usable_size, which must not report an overrun or free
// anything.
static int canary_peek(void *user, size_t *size_out, void **raw_out) {
  if (!canary_readable(user)) return 0;
  uint64_t *h = (uint64_t *)((uint8_t *)user - CANARY_HEAD);
  const uint64_t magic = canary_magic_for(user);
  if (h[0] != magic || h[2] != magic) return 0;
  if (size_out) *size_out = (size_t)h[1];
  if (raw_out)  *raw_out  = (void *)(uintptr_t)h[3];
  return 1;
}

// Returns the original pointer, or NULL if this does not look canaried.
static void *canary_check(void *user) {
  if (!canary_readable(user)) return NULL;
  uint64_t *h = (uint64_t *)((uint8_t *)user - CANARY_HEAD);
  const uint64_t magic = canary_magic_for(user);
  if (h[0] != magic || h[2] != magic) return NULL;

  const size_t size = (size_t)h[1];
  // A corrupted h[1] would make this read anywhere. Bound it before trusting it.
  if ((uintptr_t)user + size + 8 > (uintptr_t)fake_heap_end) {
    debugPrintf("[canary] header at %p claims size %zu, which runs past the heap "
                "-- header itself is corrupt; freeing normally\n", user, size);
    return NULL;
  }
  uint64_t tail = 0;
  memcpy(&tail, (uint8_t *)user + size, sizeof tail);

  __atomic_fetch_add(&g_canary_checked, 1, __ATOMIC_RELAXED);

  if (tail != magic) {
    __atomic_fetch_add(&g_canary_bad, 1, __ATOMIC_RELAXED);
    debugPrintf("[canary] *** OVERRUN: %zu-byte block at %p was written past its "
                "end (tail=%016llx)\n"
                "[canary] *** the culprit is whatever last wrote to this block, "
                "not the free that found it\n",
                size, user, (unsigned long long)tail);
    debugFlush();
  }

  // h[3], not h: the allocation began `align` bytes below the user pointer,
  // and handing anything else to free() corrupts the very heap this is meant
  // to protect.
  return (void *)(uintptr_t)h[3];
}

void heap_canary_report(void) {
  if (!config.heap_canary) return;
  debugPrintf("[copyguard] %llu bad copies reported\n",
              (unsigned long long)__atomic_load_n(&g_copy_bad, __ATOMIC_RELAXED));
  debugPrintf("[canary] %llu blocks checked, %llu overruns found\n",
              (unsigned long long)__atomic_load_n(&g_canary_checked, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_load_n(&g_canary_bad, __ATOMIC_RELAXED));
}

// malloc and calloc, canaried on the same terms as memalign.
//
// Until now the canary only covered memalign, which is the nouveau/page-
// aligned path. Ordinary malloc -- which is where texture data, engine objects
// and everything else actually lives -- was not covered at all, so the one
// instrument built for finding an overrun could not see the allocations most
// likely to be overrun.
//
// What made this worth extending: the crash is REPRODUCIBLE from a given save,
// and the last one branched to
//
//     pc = 80ff808080ff8080
//
// which is not an address. As bytes that is 80 80 ff 80 | 80 80 ff 80 -- two
// RGBA pixels of (128,128,255,128), the standard flat-normal value. A function
// pointer had been overwritten with texture data.
//
// A deterministic overrun is exactly what a canary catches, and it catches it
// at the free() of the block that was overrun rather than thousands of
// allocations later at some unrelated pointer use. Without malloc coverage it
// would have watched the wrong heap.
//
// Alignment: malloc must return memory aligned for any type. canary_offset(16)
// gives 32, which is a multiple of 16, so the user pointer keeps malloc's
// guarantee.
// ---------------------------------------------------------------------------
// Copy guard -- catch an overrun AT THE WRITE, with the caller's address.
//
// Adapted from fruitninja_nx's [copyguard], which exists there for the same
// reason: pointers smashed during asset loading. Its own comment records both
// the cause it was built for -- "mesa's nouveau_mm_allocate bug" -- and why it
// was later switched off:
//
//     A silently skipped copy = missing geometry/texture data, i.e. exactly a
//     black screen. Keep the plumbing, fail-open.
//
// So this one NEVER skips the copy. It logs and proceeds. A diagnostic that
// changes behaviour produces a second bug to chase, and this port has spent
// enough runs on that already.
//
// Two things it can do that theirs could not, because the canary is now on
// malloc as well as memalign:
//
//   * dst inside a canaried block, dst+n past the block's recorded size, is an
//     overrun KNOWN BEFORE IT HAPPENS -- reported with the block, the request
//     size, the overshoot and the caller.
//   * a destination that is not a plausible address at all is reported too,
//     which is the case their version handled.
//
// The crash this is for wrote (128,128,255,128) -- flat-normal texels -- over a
// function pointer. If that arrives via memcpy, this names the caller.
// `ret` is passed in rather than taken here.
//
// __builtin_return_address(1) would be the caller of the wrapper, but GCC only
// guarantees level 0; above that it depends on frame pointers and can return
// garbage or fault. The wrappers take level 0 -- which IS the real caller from
// their frame -- and hand it over.
//
// Gated on heap_canary as the very first thing, so with the flag off this
// costs one load and a branch on a function the whole process leans on.
// Its OWN flag, not heap_canary's.
//
// These went out together and the pair broke the heap immediately -- newlib
// faulted walking its own chunk metadata during GodotLib.setup, long before
// anything the canary was meant to catch. Shipping two instruments in one
// build meant the log could not say which.
//
// `copyguard` is by far the more invasive of the two: --wrap,memcpy catches
// EVERY copy in the process, including newlib's own inside malloc, realloc and
// free. This function then calls canary_peek, which reads 32 bytes below the
// destination -- a read, but a read taken while the allocator is mid-update --
// and on a bad destination calls debugPrintf, which can itself allocate. That
// last one is reentrant malloc from inside malloc.
//
// So it is off unless asked for. `heap_canary 1` alone now tests only the
// allocator change; `copy_guard 1` adds this.
static void copy_guard(const char *fn, void *dst, const void *src, size_t n,
                       void *ret) {
  static unsigned reported;
  if (!config.copy_guard) return;
  if (!n || reported >= 32) return;

  const uintptr_t d = (uintptr_t)dst;

  // Plainly impossible destinations. Deliberately conservative: their version
  // treated "low 32 bits zero" as poison and that matches legitimate
  // 4 GB-aligned addresses, which is how a guard starts causing black screens.
  if (d == 0 || d >= 0x8000000000ULL) {
    reported++;
    __atomic_fetch_add(&g_copy_bad, 1, __ATOMIC_RELAXED);
    debugPrintf("[copyguard] %s(dst=%p, n=%zu) -- dst is not a usable address. "
                "caller=%p\n", fn, dst, n, ret);
    debugFlush();
    return;
  }

  // Does this write past the end of a canaried block? Needs the canary on to
  // mean anything.
  if (!config.heap_canary) return;
  size_t blk = 0;
  if (!canary_peek(dst, &blk, NULL)) return;   // not the start of one; cheap out
  if (n <= blk) return;

  reported++;
  __atomic_fetch_add(&g_copy_bad, 1, __ATOMIC_RELAXED);
  debugPrintf("[copyguard] *** %s OVERRUNS its destination: block at %p is "
              "%zu bytes, copy is %zu (%zu past the end)\n"
              "[copyguard] *** caller=%p  src=%p  -- the copy is still "
              "performed; this is a report, not a fix\n",
              fn, dst, blk, n, n - blk, ret, src);
  debugFlush();
}

void *__wrap_memcpy(void *dst, const void *src, size_t n) {
  copy_guard("memcpy", dst, src, n, __builtin_return_address(0));
  return __real_memcpy(dst, src, n);
}

void *__wrap_memmove(void *dst, const void *src, size_t n) {
  copy_guard("memmove", dst, src, n, __builtin_return_address(0));
  return __real_memmove(dst, src, n);
}

// malloc/calloc canary -- TRAILING ONLY. The user pointer is NOT moved.
//
// The first version put a 32-byte header below the block and returned
// `raw + 32`, the same scheme memalign uses. That crashed newlib immediately:
//
//   x1 = 0x6b32332a60          pointer being freed
//   x6 = x1 - 0x10             newlib's chunk = ptr - 16
//   x0 = [x6+8] = 0x6b32332a40 "size", which is exactly x1 - 0x20
//
// `x1 - 0x20` is `raw`. Newlib had computed the chunk header at `user - 16`,
// landed inside OUR header, and read `h[3]` -- the stored raw pointer -- as a
// chunk size. Then it walked to `chunk + that` and faulted.
//
// The cause is structural, not a missed case: offsetting the user pointer only
// works if EVERY free goes through __wrap_free. `--wrap,free` catches calls to
// the symbol `free`, but newlib frees internally through `_free_r`, and
// chasing which path did it would just move the problem.
//
// So the block is not offset at all. The magic goes at the END of what newlib
// actually handed out, found via __real_malloc_usable_size, and the pointer
// returned is the one newlib returned. An unwrapped free is then simply
// correct -- it frees the real chunk and the canary is never consulted.
//
// The cost is precision: usable_size rounds up, so a write past the REQUESTED
// size but inside the rounding slack is not caught. It still catches a copy
// that runs off the end of the allocation, which is what the crash looks like.
#define CANARY_TAIL 8u

static uint64_t tail_magic_for(const void *user) {
  return CANARY_MAGIC ^ (uint64_t)(uintptr_t)user ^ 0x9E3779B97F4A7C15ull;
}

// Where the trailing magic lives, or NULL if this block cannot carry one.
static void *tail_slot(void *p) {
  if (!p) return NULL;
  const size_t usable = __real_malloc_usable_size(p);
  if (usable < CANARY_TAIL) return NULL;
  return (uint8_t *)p + usable - CANARY_TAIL;
}

// Which blocks were armed.
//
// Without a header there is no way to tell an armed block from one newlib
// allocated internally -- strdup and friends go through _malloc_r, never
// through __wrap_malloc, and their tails hold ordinary data. Checking those
// would report an "overrun" on every one and bury any real hit in noise.
//
// So arming is recorded. Open-addressed, power-of-two, no deletion tombstones
// needed because entries are cleared on free and the table is never grown.
// If it fills, arming simply stops -- the canary then covers fewer blocks
// rather than reporting rubbish about the rest.
#define CANARY_SLOTS (1u << 16)          // 64K pointers, 512 KB
static uintptr_t g_armed[CANARY_SLOTS];
static Mutex     g_armed_lock;
static uint32_t  g_armed_count;

static uint32_t armed_hash(uintptr_t v) {
  v ^= v >> 33; v *= 0xFF51AFD7ED558CCDull; v ^= v >> 33;
  return (uint32_t)v & (CANARY_SLOTS - 1);
}

// Returns 1 if inserted. Caller must hold nothing; this takes its own lock.
static int armed_insert(uintptr_t p) {
  int ok = 0;
  mutexLock(&g_armed_lock);
  if (g_armed_count < (CANARY_SLOTS * 3) / 4) {     // keep the table sparse
    uint32_t i = armed_hash(p);
    for (uint32_t n = 0; n < CANARY_SLOTS; n++, i = (i + 1) & (CANARY_SLOTS - 1)) {
      if (g_armed[i] == 0) { g_armed[i] = p; g_armed_count++; ok = 1; break; }
      if (g_armed[i] == p) { ok = 1; break; }       // already armed; re-arm
    }
  }
  mutexUnlock(&g_armed_lock);
  return ok;
}

// Returns 1 if it was armed, and removes it.
static int armed_take(uintptr_t p) {
  int found = 0;
  mutexLock(&g_armed_lock);
  uint32_t i = armed_hash(p);
  for (uint32_t n = 0; n < CANARY_SLOTS; n++, i = (i + 1) & (CANARY_SLOTS - 1)) {
    if (g_armed[i] == p) { g_armed[i] = 0; g_armed_count--; found = 1; break; }
    if (g_armed[i] == 0) break;                     // end of probe run
  }
  mutexUnlock(&g_armed_lock);
  return found;
}

static void canary_arm_tail(void *p) {
  void *slot = tail_slot(p);
  if (!slot) return;
  if (!armed_insert((uintptr_t)p)) return;          // table full: leave it alone
  const uint64_t m = tail_magic_for(p);
  memcpy(slot, &m, sizeof m);
}

// Called from __wrap_free BEFORE the block goes back. Reports and continues:
// the block is freed either way, because refusing to free it would turn a
// diagnostic into a leak.
static void canary_check_tail(void *p) {
  if (!armed_take((uintptr_t)p)) return;   // not one of ours; say nothing

  void *slot = tail_slot(p);
  if (!slot) return;
  uint64_t got = 0;
  memcpy(&got, slot, sizeof got);
  const uint64_t want = tail_magic_for(p);

  __atomic_fetch_add(&g_canary_checked, 1, __ATOMIC_RELAXED);
  if (got == want) return;

  static unsigned reported;
  if (reported++ >= 32) { __atomic_fetch_add(&g_canary_bad, 1, __ATOMIC_RELAXED); return; }
  __atomic_fetch_add(&g_canary_bad, 1, __ATOMIC_RELAXED);
  debugPrintf("[canary] *** OVERRUN: %p (usable %zu) tail=%016llx "
              "want=%016llx\n"
              "[canary] *** if this repeats on the same size, something writes "
              "past the end of that allocation\n",
              p, __real_malloc_usable_size(p),
              (unsigned long long)got, (unsigned long long)want);
  debugFlush();
}

void *__wrap_malloc(size_t size) {
  if (!config.heap_canary) return __real_malloc(size);
  if (size > SIZE_MAX - CANARY_TAIL) return NULL;

  void *p = __real_malloc(size + CANARY_TAIL);
  if (p) {
    canary_arm_tail(p);
  }
  return p;                      // newlib's own pointer, unmoved
}

void *__wrap_calloc(size_t n, size_t sz) {
  if (!config.heap_canary) return __real_calloc(n, sz);
  if (n && sz > SIZE_MAX / n) return NULL;

  const size_t size = n * sz;
  if (size > SIZE_MAX - CANARY_TAIL) return NULL;

  void *p = __real_malloc(size + CANARY_TAIL);
  if (!p) return NULL;
  memset(p, 0, size);            // calloc's contract, for the REQUESTED size
  canary_arm_tail(p);
  return p;
}

void *__wrap_memalign(size_t align, size_t size) {
  // nouveau_bo_new's signature: page-aligned and large. Everything else goes
  // straight to newlib, so this cannot disturb ordinary allocation.
  if (align >= GPUA_PAGE && size >= GPUA_MIN) {
    void *p = gpua_alloc(size);   // initialises on first call, under the lock
    if (p) return p;
  }

  // Canaries only on the newlib path. A pool block is bounded by the page
  // bitmap and cannot run into another allocation's header, so there is
  // nothing there for a canary to protect.
  if (config.heap_canary) {
    const size_t off = canary_offset(align);
    void *raw = __real_memalign(off, size + off + 8);
    return canary_wrap(raw, size, align);
  }
  return __real_memalign(align, size);
}

// Refused frees, so this is visible as a number rather than only as log lines.
uint64_t sts2_free_guard_hits;

// ---------------------------------------------------------------------------
// Free quarantine (fruitninja_nx nx_alloc.c, quar_*)
//
// A ring of recently freed blocks, poisoned and held unreusable. When a block
// finally leaves, the poison is verified and any write is reported with its
// offset and contents.
//
// Bounded three ways so it cannot become the problem: per-block size, entry
// count, and total bytes held. The last matters most here -- Mesa's buffer
// allocations come from this same newlib heap, so retention starves the GPU
// path. fruitninja hit exactly that and turned theirs off.
// ---------------------------------------------------------------------------
#define QUAR_N       4096u
#define QUAR_MAX_SZ  (1u << 20)          // per block
#define QUAR_POISON  0xDE

typedef struct { void *p; uint32_t sz; } QuarEnt;
static QuarEnt  g_quar[QUAR_N];
static unsigned g_quar_head, g_quar_tail;   // ring: head = oldest
static size_t   g_quar_bytes;
static Mutex    g_quar_lk;
uint64_t sts2_quar_hits;

// The validated release, used by BOTH the normal free path and the quarantine.
// Returns without freeing when the chunk header is corrupt: the block is
// already unusable, and faulting inside the allocator takes every thread with
// it.
//
// Declared here rather than further down because quar_check_free() below is
// its first caller. It was originally declared just above __wrap_free, which
// is 40 lines too late -- C then assumed an implicit int-returning function at
// the call site and the real declaration conflicted with it.
//
// static, because nothing outside this file calls it. That also puts it under
// decl_check, which only tracked file-scope statics and so missed this.
static void sts2_free_checked(void *p);

static void quar_check_free(QuarEnt e) {    // called with NO lock held
  const uint8_t *b = (const uint8_t *)e.p;
  for (uint32_t i = 0; i < e.sz; i++) {
    if (b[i] == QUAR_POISON) continue;

    sts2_quar_hits++;
    const uint32_t end = (i + 32 > e.sz) ? e.sz : i + 32;
    char line[3 * 36 + 4];
    int n = 0;
    for (uint32_t k = i; k < end && n < (int)sizeof(line) - 4; k++)
      n += snprintf(line + n, sizeof(line) - n, " %02x", b[k]);

    debugPrintf("[quar] WRITE-AFTER-FREE: block %p size %u, first hit at +0x%x:%s\n",
                e.p, (unsigned)e.sz, (unsigned)i, line);

    // Name the value, since the shapes this port keeps producing are small
    // integers in pointer fields and that is recognisable.
    if (i + 8 <= e.sz && ((uintptr_t)(b + i) & 7u) == 0) {
      uint64_t v;
      memcpy(&v, b + i, sizeof(v));
      char what[96];
      lock_trace_describe_value(v, what, sizeof(what));
      debugPrintf("[quar]   as a 64-bit value: %016llx -- %s\n",
                  (unsigned long long)v, what);
    }
    debugFlush();
    break;   // one report per block
  }

  // Validate before releasing, same as the main path.
  //
  // A block sitting in quarantine is exactly where a stale writer is expected
  // to land, and the header lives just below it -- so the chunk metadata is
  // MORE likely to be corrupt here than on a normal free, not less. Calling
  // __real_free directly would hand it to the allocator unchecked and fault
  // inside free() with the heap lock held, which is the crash free_guard
  // exists to prevent.
  sts2_free_checked(e.p);
}

static void quar_add(void *p, size_t sz) {
  memset(p, QUAR_POISON, sz);

  const size_t cap = (size_t)config.quarantine_mb << 20;
  QuarEnt out[8];
  int no = 0, direct = 0;

  mutexLock(&g_quar_lk);
  while (g_quar_bytes + sz > cap || (g_quar_tail + 1) % QUAR_N == g_quar_head) {
    if (no >= 8) { direct = 1; break; }   // could not make room; free p directly
    QuarEnt e = g_quar[g_quar_head];
    g_quar[g_quar_head].p = NULL;
    g_quar_head = (g_quar_head + 1) % QUAR_N;
    if (e.p) { g_quar_bytes -= e.sz; out[no++] = e; }
  }
  if (!direct) {
    g_quar[g_quar_tail].p = p;
    g_quar[g_quar_tail].sz = (uint32_t)sz;
    g_quar_tail = (g_quar_tail + 1) % QUAR_N;
    g_quar_bytes += sz;
  }
  mutexUnlock(&g_quar_lk);

  // Verification and the real free happen OUTSIDE the lock: quar_check_free
  // logs, and logging under an allocator-adjacent lock is how deadlocks start.
  for (int i = 0; i < no; i++) quar_check_free(out[i]);
  if (direct) __real_free(p);
}

// Scratch for the value description. Single-threaded use inside the guard,
// which only runs on the already-corrupt path.
static char g_desc[96];

void __wrap_free(void *p) {
  if (!p) return;

  if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == 1) {
    mutexLock(&g_lock);

    // Ownership test and release under ONE acquisition. Splitting them is what
    // let a pool pointer reach __real_free in the chunked version.
    if ((uint8_t *)p >= g_base && (uint8_t *)p < g_base + g_pages * GPUA_PAGE) {
      const size_t   idx = ((uint8_t *)p - g_base) / GPUA_PAGE;
      const uint32_t run = (idx < g_pages) ? g_runlen[idx] : 0;

      if (run) {
        for (uint32_t k = 0; k < run; k++) g_used[idx + k] = 0;
        g_runlen[idx] = 0;
        g_live_pages -= run;
        g_n_free++;

        unsigned b = 0;
        for (size_t t = ((size_t)run * GPUA_PAGE) >> 16; t && b < GPUA_BUCKETS - 1; t >>= 1) b++;
        if (g_hist_live[b]) g_hist_live[b]--;
        // Freed space is a good next candidate; rewinding keeps the cursor
        // from marching past reusable holes.
        if (idx < g_hint) g_hint = idx;
      }

      mutexUnlock(&g_lock);
      return;
    }

    mutexUnlock(&g_lock);
  }

  if (config.heap_canary) {
    // Trailing magic first: malloc/calloc blocks are NOT offset, so p is
    // already newlib's pointer and this is just a read at its far end.
    canary_check_tail(p);

    // Header-style canary, memalign only. Returns NULL for anything else,
    // including every malloc block, which now falls through to __real_free(p)
    // with the pointer newlib gave out.
    void *raw = canary_check(p);
    if (raw) { __real_free(raw); return; }
    // Not canaried: allocated before the flag was read, or by a path that does
    // not go through these wrappers. Freeing it normally is correct.
  }

  // Validate the chunk header before newlib touches it.
  //
  // Hardware crash, resolved via sts2.elf:
  //
  //   pc = sts2.nro+0xbdddcc  = _free_r
  //   str x1, [x0, #0x18]     with x0 == NULL      -- fd->bk = bk, unlink()
  //
  //   chunk at 0x47132e0bf0:
  //     +0x00 prev_size  0x0044217002329b94   garbage
  //     +0x08 size       0x0000000000000000   impossible for a live chunk
  //     +0x10 fd         NULL
  //     +0x18 bk         0x0000000000000005   a SMALL INTEGER, not a pointer
  //
  // `bk = 5` is the same shape as the `...00000001` values that have been
  // crashing this port for fifty runs: a small integer written into a field
  // that must hold a pointer. Here it lands in newlib's free-list metadata, so
  // free() dereferences it and dies.
  //
  // Checking there converts an unavoidable crash into a diagnostic. The cost
  // is a leak -- a corrupt chunk is not returned to the allocator -- and that
  // is the right trade: the block is already unusable, and the alternative is
  // a fault inside the allocator with the heap lock held, which takes every
  // other thread with it.

  // -------------------------------------------------------------------------
  // Free poisoning and quarantine.
  //
  // Ported from fruitninja_nx's nx_alloc.c, whose round-135 note describes this
  // port's situation exactly:
  //
  //   "Every boot randomises the whole layout... a stale or uninitialised
  //    pointer takes a different value every run, and whether it lands on
  //    mapped memory is a coin flip. That is why the same bug has produced a
  //    NULL klass, a bogus-but-mapped klass, an unmapped klass and a raw 1 --
  //    and why each guard held only until the shape changed."
  //
  // The shapes seen here: 0x...00000001, 0x5f00000001, 0x3200000001,
  // TextureRect+0x08 == 1, and a malloc chunk with bk == 5 and fd == NULL.
  // One bug, a different value every boot, and a guard written for each shape
  // in turn.
  //
  // POISON converts that into a constant. Freed memory reads back as
  // 0xDEDEDEDE... regardless of layout, so a stale read is nameable on sight --
  // and if the garbage is still random, reuse is RULED OUT, which is worth as
  // much as confirming it.
  //
  // QUARANTINE additionally holds freed blocks unreusable and verifies the
  // poison on release, naming the offset that was written. It is a mitigation
  // as well as a detector: the stale writer scribbles into quarantined memory
  // instead of someone's live allocation.
  //
  // Both default OFF. fruitninja disabled its quarantine because holding 64 MB
  // hostage starved Mesa's GART slabs -- nouveau buffer memory comes from this
  // same newlib heap, and that applies here too. Poison is off because this
  // build currently reaches gameplay, and anything reading freed memory (a bug,
  // but a bug that may presently go unnoticed) would start behaving
  // differently. Turn them on to hunt, not to ship.
  // -------------------------------------------------------------------------
  if (config.poison_free || config.quarantine_mb) {
    // __real_malloc_usable_size, not the plain name.
    //
    // malloc_usable_size is in the --wrap set, so the bare call would route
    // through __wrap_malloc_usable_size and report the CANARY-ADJUSTED length.
    // Poisoning that many bytes from p would run past the block's real end and
    // over the canary tail -- corrupting the check meant to detect corruption.
    const size_t us = __real_malloc_usable_size(p);
    if (us) {
      if (config.quarantine_mb && us <= QUAR_MAX_SZ) {
        quar_add(p, us);     // poisons, holds, and frees something older
        return;
      }
      if (config.poison_free) memset(p, QUAR_POISON, us);
    }
  }

  // Everything that is actually released goes through the validated path,
  // including blocks leaving quarantine.
  //
  // The quarantine lives HERE and not in sts2_free_checked, deliberately:
  // quar_check_free() calls sts2_free_checked() to release a block, so a
  // quarantine step inside that function would re-quarantine everything it
  // released and never free anything at all.
  sts2_free_checked(p);
}

static void sts2_free_checked(void *p) {
  if (!p) return;

  if (config.free_guard) {
    const uint64_t *h = (const uint64_t *)((const uint8_t *)p - 16);
    MemoryInfo mi; u32 pg;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pg, (u64)(uintptr_t)h)) &&
        mi.type != MemType_Unmapped && (mi.perm & Perm_R)) {
      // Chunk layout, established from the crash rather than assumed.
      //
      // The faulting instruction was `str x1, [x0, #0x18]` where x0 came from
      // [x4 + 0x10] -- that is `fd->bk = bk`, the unlink() step, with fd at
      // chunk+0x10 and bk at chunk+0x18. Four 8-byte fields at 0/8/16/24 with
      // an fd/bk pair is dlmalloc, which is what newlib uses here; nano-malloc
      // has no such pair and could not produce that instruction. The user
      // pointer is chunk + 2*SIZE_SZ, so the header is at p - 16.
      const uint64_t size = h[1];
      const uint64_t masked = size & ~(uint64_t)7;   // low bits are flags

      // Upper bound from the MAPPED REGION, not a magic constant.
      //
      // A chunk claiming to extend past the end of its own mapping is
      // definitively invalid, whatever the heap size happens to be. A fixed
      // 1 GB ceiling got this wrong in both directions: it would reject a
      // legitimate large allocation on a 2903 MB heap, and it let
      // 0x40000001 through because that masks to exactly 1 GB.
      const uint64_t region_end = (uint64_t)mi.addr + (uint64_t)mi.size;
      const uint64_t room = region_end - (uint64_t)(uintptr_t)h;

      // A live chunk is at least the header plus a minimum payload. The
      // crashing chunk had size == 0; `bk = 5` shows the same small-integer
      // corruption lands in size just as easily, and 5 masks to 0.
      const int bad_size = (masked < 32u) || (masked > room);

      if (bad_size) {
        sts2_free_guard_hits++;
        debugPrintf("[freeguard] REFUSING free(%p): chunk header is corrupt\n"
                    "[freeguard]   prev_size %016llx\n"
                    "[freeguard]   size      %016llx  <- invalid\n"
                    "[freeguard]   fd        %016llx\n"
                    "[freeguard]   bk        %016llx\n",
                    p, (unsigned long long)h[0], (unsigned long long)h[1],
                    (unsigned long long)h[2], (unsigned long long)h[3]);
        lock_trace_describe_value(h[3], (char *)g_desc, sizeof(g_desc));
        debugPrintf("[freeguard]   bk looks like: %s\n", g_desc);
        lock_trace_hexdump_around(p);
        debugFlush();
        return;   // leak it; see above
      }
    }
  }

  __real_free(p);
}

// realloc.
//
// A pool block is NOT a newlib block. Handing one to realloc makes newlib read
// a chunk header that was never written, and it corrupts the free list --
// which then crashes somewhere else entirely, inside _free_r, on a later and
// unrelated free.
//
// That is run 29: pc resolved to sts2.nro+0x444304 = _free_r+0x84, storing
// through a null pointer, with the heap chunk at hand full of zeros. The pool
// itself was quiet -- 93 MB, 20 allocations, no fallbacks -- so it was never
// pressure. It was this.
//
// I wrapped only memalign and free, reasoning that nouveau_bo_new uses those
// two. That is true of nouveau and irrelevant to the hazard: ANY code that
// reallocs a page-aligned block over 64 KB reaches this, and mesa has plenty
// of buffers that qualify. fruitninja_nx wraps realloc for exactly this reason,
// and its comment says so in as many words.
//
// Copy out to a real allocation and release the slot, rather than trying to
// grow in place: the pool has no size class to grow into and the copy is rare.
void *__wrap_realloc(void *p, size_t size) {
  // __real_realloc, not __real_malloc: `--wrap,X` only defines `__real_X` for
  // an X that is actually wrapped, and malloc is not one of them. newlib's
  // realloc already treats a NULL pointer as malloc, so there is nothing to
  // add here.
  if (!p) return __real_realloc(NULL, size);

  if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == 1) {
    size_t oldsz = 0;

    mutexLock(&g_lock);
    if ((uint8_t *)p >= g_base && (uint8_t *)p < g_base + g_pages * GPUA_PAGE) {
      const size_t idx = ((uint8_t *)p - g_base) / GPUA_PAGE;
      if (idx < g_pages) oldsz = (size_t)g_runlen[idx] * GPUA_PAGE;
    }
    mutexUnlock(&g_lock);

    if (oldsz) {
      // size 0 means free, per realloc's contract.
      if (!size) { __wrap_free(p); return NULL; }

      void *q = __wrap_memalign(GPUA_PAGE, size);   // may land back in the pool
      if (!q) return NULL;                          // original left intact
      memcpy(q, p, size < oldsz ? size : oldsz);
      __wrap_free(p);
      return q;
    }
  }

  // Header-canaried blocks (memalign only) are not plain newlib blocks: the
  // allocation starts `align` bytes lower. Handing the USER pointer to realloc
  // makes newlib read a header 32 bytes into the allocation.
  //
  // malloc/calloc blocks do NOT take this path -- canary_peek fails on them
  // because they carry no header -- and that is correct: they are ordinary
  // newlib blocks and __real_realloc handles them. The new block simply loses
  // its trailing magic, which is re-armed below.
  if (config.heap_canary) {
    size_t oldsz = 0;
    if (canary_peek(p, &oldsz, NULL)) {
      if (!size) { __wrap_free(p); return NULL; }
      void *q = __wrap_memalign(CANARY_HEAD, size);   // canaried again
      if (!q) return NULL;                            // original left intact
      memcpy(q, p, size < oldsz ? size : oldsz);
      __wrap_free(p);                                 // checks the old tail
      return q;
    }
  }

  // Un-arm p FIRST. __real_realloc may free the old block itself, which never
  // reaches __wrap_free and so never reaches armed_take -- leaving a stale
  // entry for an address newlib is free to hand out again. The next block at
  // that address would then be checked against a magic nobody wrote.
  if (config.heap_canary) armed_take((uintptr_t)p);

  void *q = __real_realloc(p, size);

  // Re-arm: realloc may have moved or grown the block, so the old magic is at
  // the wrong offset or gone. Without this every realloc'd block reports as an
  // overrun when freed.
  if (q && config.heap_canary) canary_arm_tail(q);
  return q;
}

// malloc_usable_size on a pool block would read a newlib chunk header that was
// never written and return nonsense -- which a caller then uses as a copy
// length. Read-only, so not the corruption in run 29, but the same class of
// hazard as realloc and three lines to close.
size_t __wrap_malloc_usable_size(void *p) {
  if (p && __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == 1) {
    size_t sz = 0;
    mutexLock(&g_lock);
    if ((uint8_t *)p >= g_base && (uint8_t *)p < g_base + g_pages * GPUA_PAGE) {
      const size_t idx = ((uint8_t *)p - g_base) / GPUA_PAGE;
      if (idx < g_pages) sz = (size_t)g_runlen[idx] * GPUA_PAGE;
    }
    mutexUnlock(&g_lock);
    if (sz) return sz;
  }

  // Canaried blocks report the REQUESTED size, not newlib's view of the raw
  // allocation -- which includes the header and tail and would invite a caller
  // to write into the canary it is meant to be protected by.
  if (p && config.heap_canary) {
    size_t sz = 0;
    if (canary_peek(p, &sz, NULL)) return sz;   // header style (memalign)

    // Trailing style: hide the magic, or a caller that sizes its writes by
    // usable_size will overwrite the canary legitimately and every free will
    // report a false overrun.
    const size_t usable = __real_malloc_usable_size(p);
    if (usable >= CANARY_TAIL) return usable - CANARY_TAIL;
  }

  return __real_malloc_usable_size(p);
}

size_t gpu_arena_live_bytes(void) {
  if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 1) return 0;
  mutexLock(&g_lock);
  const size_t n = g_live_pages * GPUA_PAGE;
  mutexUnlock(&g_lock);
  return n;
}

void gpu_arena_report(void) {
  if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 1) return;

  // Snapshot under the lock, print after releasing it. Printing while holding
  // it would break the rule above.
  size_t live, peak, total;
  uint64_t na, nf, nb;

  mutexLock(&g_lock);
  live = g_live_pages; peak = g_peak_pages; total = g_pages;
  na = g_n_alloc; nf = g_n_free; nb = g_n_fallback;
  mutexUnlock(&g_lock);

  debugPrintf("[gpua] live %zu MB, peak %zu MB of %zu MB; "
              "%llu allocs, %llu frees, %llu fell back to newlib\n",
              (live * GPUA_PAGE) >> 20, (peak * GPUA_PAGE) >> 20,
              (total * GPUA_PAGE) >> 20,
              (unsigned long long)na, (unsigned long long)nf,
              (unsigned long long)nb);

  if (nb)
    debugPrintf("[gpua] %llu fallbacks means the pool is too small and newlib "
                "is being fragmented anyway; raise gpu_arena_mb\n",
                (unsigned long long)nb);

  // The breakdown, snapshotted the same way.
  uint64_t hn[GPUA_BUCKETS], hl[GPUA_BUCKETS];
  size_t   top[GPUA_TOPN];
  mutexLock(&g_lock);
  memcpy(hn, g_hist_n, sizeof hn);
  memcpy(hl, g_hist_live, sizeof hl);
  memcpy(top, g_top_bytes, sizeof top);
  mutexUnlock(&g_lock);

  char line[256];
  int off = 0;
  for (int b = 0; b < GPUA_BUCKETS; b++) {
    if (!hn[b]) continue;
    const unsigned lo_kb = 64u << b;
    off += snprintf(line + off, sizeof(line) - (size_t)off, "%u%s:%llu/%llu ",
                    lo_kb >= 1024 ? lo_kb >> 10 : lo_kb,
                    lo_kb >= 1024 ? "M" : "K",
                    (unsigned long long)hl[b], (unsigned long long)hn[b]);
    if (off >= (int)sizeof(line) - 24) break;
  }
  debugPrintf("[gpua] by size (live/total): %s\n", off ? line : "(none)");

  debugPrintf("[gpua] largest ever: %zu %zu %zu %zu %zu %zu %zu %zu MB\n",
              top[0] >> 20, top[1] >> 20, top[2] >> 20, top[3] >> 20,
              top[4] >> 20, top[5] >> 20, top[6] >> 20, top[7] >> 20);
}
