/* imports.c -- resolves the dynamic imports of libgodot_android.so (Godot
 * 4.6.dev4, arm64-v8a) and libc++_shared.so against newlib, host zlib, mesa
 * GLES3/EGL, and our shims. The import list is built from the actual UND
 * symbols of both libraries (reference/all_needs_wrapper.txt); C++ ABI symbols
 * (_Z.. and __cxa..) resolve module-to-module from libc++_shared's exports.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <fenv.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>   // shutdown
#include <zlib.h>
#include <switch.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include "lock_trace.h"
#include "config.h"
#include "so_util.h"
#include "error.h"       // fatal_error (sts2_assert2)
#include "util.h"
#include "libc_shim.h"
#include "godot_shim.h"
#include "egl_shim.h"
#include "jni_fake.h"
#include "nativeaot_shim.h"
#include "aaudio_shim.h"
#include "fmod_shim.h"
#include "dotnet_imports.h"

extern char **environ;   // newlib's; the "environ" import resolves to &environ

// real libc/gcc symbols whose addresses we forward verbatim
extern int   __cxa_atexit(void (*)(void *), void *, void *);
extern void  __stack_chk_fail(void);

// ---------------------------------------------------------------------------
// bionic logging
// ---------------------------------------------------------------------------

// Shader-compile tracking.
//
// The hypothesis worth testing is that the crash is caused by shaders being
// compiled DURING gameplay, rather than by anything else that happens to be
// running at the same time. That is testable for free, because Godot already
// announces every compile:
//
//     Shader cache miss for <name>            <- a real compile
//     Loading cache for shader <name>, ...    <- came from cache, no compile
//
// Everything the engine prints funnels through here, so counting them costs a
// string compare on a path that is already doing a vsnprintf.
//
// What the crash handler does with it is the point: if the last compile was
// tens of thousands of frames before the fault, compilation is not the
// trigger, and moving it earlier would achieve nothing. If it was in the last
// handful, that is a strong correlation and worth acting on.
//
// Note this distinguishes MISS from LOAD. A warm cache still prints "Loading
// cache", and if the crash follows those too, the trigger is pipeline creation
// rather than compilation -- which precompiling cannot help either.
uint32_t sts2_shader_compiles;        // cache MISS: real compilation
uint32_t sts2_shader_loads;           // cache HIT: no compilation
uint32_t sts2_last_compile_frame;
uint32_t sts2_last_load_frame;
extern int s_frames_done_public;      // set by the frame loop; 0 before it runs

static void note_shader_line(const char *msg) {
  if (!msg) return;
  // strncmp rather than strstr: these are line prefixes, and strstr over every
  // engine log line would be measurably worse for no gain.
  if (!strncmp(msg, "Shader cache miss", 17)) {
    sts2_shader_compiles++;
    sts2_last_compile_frame = (uint32_t)s_frames_done_public;
  } else if (!strncmp(msg, "Loading cache for shader", 24)) {
    sts2_shader_loads++;
    sts2_last_load_frame = (uint32_t)s_frames_done_public;
  }
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  note_shader_line(string);
  debugPrintf("[%s] %s\n", tag ? tag : "", string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// errno
// ---------------------------------------------------------------------------

static int *__errno_fake(void) { return &errno; }

// ---------------------------------------------------------------------------
// pthread: bionic's opaque types are zero-inited inline, so lazily back them
// with heap newlib objects stashed in the caller's first pointer slot.
// ---------------------------------------------------------------------------

#define BIONIC_RECURSIVE_MARK 0x8000
#define BIONIC_ERRCHECK_MARK  0x4000

// newlib error numbers -> bionic's.
//
// Both libcs are just #defines the caller compiled in. Values 1..34
// (EPERM..ERANGE) are identical, which is why most of this port works; from 35
// up they diverge completely, and a shim returning newlib's number is read by
// the engine as an entirely different error.
//
// The table used to hold two entries -- ETIMEDOUT and EDEADLK -- and be applied
// to exactly one function. Everything else that returns an error code returned
// newlib's numbering untranslated. pthread_mutex_timedlock is the clearest
// miss: it returns ETIMEDOUT for the same reason pthread_cond_timedwait does,
// and a caller checking `ret == ETIMEDOUT` against bionic's 110 never matched
// newlib's 116. A timeout then looks like an unrecognised failure, and what a
// waiting loop does with that is anyone's guess.
//
// Values below 35 fall through untouched -- translating them would be a no-op
// at best and a bug the day one of them changes.
static int to_bionic_errno(int e) {
  if (e < 35) return e;              // identical range; nothing to do
  switch (e) {
    case 45:  return 35;   // EDEADLK
    case 46:  return 37;   // ENOLCK
    case 88:  return 38;   // ENOSYS
    case 90:  return 39;   // ENOTEMPTY
    case 91:  return 36;   // ENAMETOOLONG
    case 92:  return 40;   // ELOOP
    case 108: return 88;   // ENOTSOCK
    case 109: return 89;   // EDESTADDRREQ
    case 110: return 90;   // EMSGSIZE
    case 111: return 111;  // ECONNREFUSED -- same on both, listed so the next
                           // reader does not have to check
    case 116: return 110;  // ETIMEDOUT
    case 134: return 95;   // ENOTSUP  (bionic: == EOPNOTSUPP)
    case 138: return 74;   // EBADMSG
    case 139: return 75;   // EOVERFLOW
    case 140: return 125;  // ECANCELED
    case 142: return 130;  // EOWNERDEAD
    case 143: return 131;  // ENOTRECOVERABLE
    default:  return e;    // unmapped: pass through rather than invent one
  }
}

// Bionic's pthread_mutex_t (40 B) and pthread_cond_t (48 B) are int32-array
// structs with 4-byte alignment. We stash the pointer to our real newlib
// backing object at the FIRST 8-ALIGNED offset inside that storage: exclusive
// loads (the CAS below) require natural alignment, and the struct is large
// enough that the aligned slot always fits. The static-initializer type mark
// (bionic stores it in the first int32) is read separately.
// The engine's call site, captured where it is actually meaningful.
//
// __builtin_return_address(0) inside ensure_mutex/note_bad_lock_slot_at gave
// addresses in no loaded module at all (0x22a2bea4dc, with libgodot at
// 0x338f653000 and libsts2 at 0x4f4cc60000) -- those helpers are static inline
// and the builtin has nothing dependable to read once they are folded into
// their caller.
//
// The entry shims are different: they are what the engine's import table calls
// directly, so their return address IS the engine call site. Each one records
// it here on entry, and the slot tracer reads it. Thread-local because two
// threads race through these constantly, which is the entire problem.
// Keyed on armGetTls(), NOT __thread.
//
// __thread is ELF TLS. It is not used anywhere else in this port, and the game
// thread comes from libnx's threadCreate rather than pthread_create -- so
// whether the TLS block is set up on every thread that reaches here is an
// assumption, not a fact. A fault while reading it would happen INSIDE the
// mutex path, on a diagnostic, which is the worst place to discover that.
//
// armGetTls() is a register read, valid on every thread by construction, and
// already used by lock_trace.c. Indexing a small table with it gives the same
// per-thread storage with nothing to verify.
//
// Collisions are possible and harmless: two threads sharing a bucket
// misattribute a caller address in a diagnostic. Never a fault, never a wrong
// lock.
#define LOCK_CALLER_SLOTS 64u
static uint64_t g_lock_callers[LOCK_CALLER_SLOTS];

static inline uint32_t lock_caller_bucket(void) {
  return (uint32_t)(((uintptr_t)armGetTls() >> 8) & (LOCK_CALLER_SLOTS - 1u));
}

uint64_t sts2_lock_caller_get(void) {
  return __atomic_load_n(&g_lock_callers[lock_caller_bucket()], __ATOMIC_RELAXED);
}

#define LOCK_CALLER_HERE()                                                    \
  __atomic_store_n(&g_lock_callers[lock_caller_bucket()],                     \
                   (uint64_t)(uintptr_t)__builtin_return_address(0),          \
                   __ATOMIC_RELAXED)

static inline uint64_t *lazy_slot(void *storage) {
  return (uint64_t *)(((uintptr_t)storage + 7) & ~(uintptr_t)7);
}

// Is `v` a plausible previously-stored backing-object pointer, as opposed
// to leftover garbage in the lazy slot? Confirmed on hardware (03/ago): a
// mutex+cond pair embedded in a NativeAOT-managed synchronization object
// crashed the FIRST time it was ever touched -- the cond's slot held
// 0x6f00000073 (> the old ">0x10000" threshold, so ensure_cond took the
// "already initialized" fast path and returned that garbage directly,
// without ever calling calloc()/pthread_cond_init()) even though this was
// its first use. Bionic's real static initializers are always exactly
// zero, so ANY nonzero garbage clears a magic-number threshold check; the
// only pointers WE ever actually store here come from calloc(), which is
// serviced by libnx's newlib heap (fake_heap_start/fake_heap_end, set once
// in __libnx_initheap) -- checking the value actually falls in that range
// is a real ownership test, not a guess at "big enough to be a pointer".
extern char *fake_heap_start;
extern char *fake_heap_end;

// Heap regions confirmed by svcQueryMemory, so the syscall happens once per
// region rather than once per lock operation. Eight is generous: the newlib
// heap plus whatever this port maps.
#define HEAP_REGION_CACHE 8
static struct { uint64_t start, end; } g_heap_regions[HEAP_REGION_CACHE];
static uint32_t g_heap_region_n;
// Rejected values, so this is visible rather than merely survived.
uint64_t sts2_bad_lock_slots;

int sts2_plausible_lock_ptr(uint64_t v);   // exported wrapper, below

static inline int looks_like_our_heap_ptr(uint64_t v) {
  // ALIGNMENT FIRST. The range test alone is not enough and never was.
  //
  // Everything handed out here comes from calloc, which returns memory aligned
  // for any type -- 16 bytes on aarch64, 8 guaranteed. A value with its low
  // bits set therefore CANNOT be one of ours, whatever range it falls in.
  //
  // The range test passed all four of these, because a heap based near
  // 0x5f00000000 contains 0x5f00000001:
  //
  //     run 22   0x0000003200000001
  //     run 38   0x0000000000000001
  //     run 39   0x0000005f00000001
  //     run 42   0x0000001000000001
  //
  // and the comment on this function already recorded a fifth, 0x6f00000073,
  // from the cond path. Every one has its low 32 bits set to a small integer
  // and junk above -- the shape of a 32-bit value written into a slot that is
  // then read back as a 64-bit pointer. The guard was added for exactly that
  // and then only checked the half of it that these values satisfy.
  //
  // 8 rather than 16: 8 is guaranteed by any conforming malloc, catches all
  // five observed values, and does not depend on newlib's specific alignment.
  if (v & 7u) return 0;

  // Fast accept: inside the heap libnx handed us at startup.
  if (v >= (uint64_t)(uintptr_t)fake_heap_start &&
      v <  (uint64_t)(uintptr_t)fake_heap_end)
    return 1;

  // MISS PATH -- ask the kernel before rejecting.
  //
  // fake_heap_start/fake_heap_end are set once in __libnx_initheap and never
  // move. They do not describe every page malloc can return: the heap can be
  // grown, and this port maps regions of its own. A hardware run rejected 67
  // slots as "aligned, but outside the newlib heap" whose values were
  // 0x22a4..., 0x22a8..., 0x22b5... -- the SAME region as the slots holding
  // them. Those were real pointers.
  //
  // That is the worst possible failure for this guard. Rejecting a live cond
  // means allocating a replacement and CASing it in, while any thread already
  // waiting on the original is never woken again. The check meant to prevent
  // lost wakeups was causing them.
  //
  // svcQueryMemory is authoritative but costs a syscall, and this function is
  // on the hot path -- ensure_mutex/ensure_cond call it on EVERY lock, unlock,
  // trylock, wait and signal. A syscall per lock operation would be a serious
  // regression, and it would hit precisely the objects the fast range test
  // already misses, i.e. constantly rather than rarely.
  //
  // So a verified region is remembered. svcQueryMemory reports the whole
  // containing region (addr + size), not just the page, so one syscall covers
  // a large span and every later pointer into it takes the cache path.
  for (uint32_t i = 0, n = __atomic_load_n(&g_heap_region_n, __ATOMIC_ACQUIRE);
       i < n && i < HEAP_REGION_CACHE; i++) {
    if (v >= g_heap_regions[i].start && v < g_heap_regions[i].end) return 1;
  }

  MemoryInfo mi;
  u32 pageinfo;
  if (R_FAILED(svcQueryMemory(&mi, &pageinfo, (u64)v))) return 0;
  // The MINIMUM test that answers the actual question: could calloc have
  // returned this? That is heap-typed memory we can read and write.
  //
  // No attribute filtering. The first version also excluded uncached and
  // device-mapped pages, which was conservatism aimed at nothing in
  // particular -- and the bug being fixed here is OVER-rejection. Extra
  // conditions in this function are how valid cond pointers got thrown away
  // and threads stopped being woken. Adding more of them to a fix for that
  // would be exactly the wrong direction.
  //
  // mi.attr is reported below when a rejection happens, so if an attribute
  // ever does turn out to matter, the log will say so rather than this
  // silently guessing.
  if (mi.type != MemType_Heap) return 0;
  if ((mi.perm & (Perm_R | Perm_W)) != (Perm_R | Perm_W)) return 0;

  // Remember it. Racy by design: two threads may verify the same region and
  // both append, or a reader may miss an entry another thread is still
  // publishing. Both cost one redundant syscall and nothing else -- there is
  // no wrong answer here, only a slower one, so no lock is warranted in code
  // that runs inside the lock implementation.
  const uint32_t slot_i = __atomic_fetch_add(&g_heap_region_n, 1, __ATOMIC_RELAXED);
  if (slot_i < HEAP_REGION_CACHE) {
    g_heap_regions[slot_i].start = (uint64_t)mi.addr;
    g_heap_regions[slot_i].end   = (uint64_t)mi.addr + (uint64_t)mi.size;
    __atomic_thread_fence(__ATOMIC_RELEASE);
  } else {
    // Full. Undo, so the counter cannot run away and the loop bound above
    // stays meaningful.
    __atomic_fetch_sub(&g_heap_region_n, 1, __ATOMIC_RELAXED);
  }

  return 1;
}

// A slot that holds something non-null but impossible. Counted and logged
// once, because "how often does this happen" is the difference between a
// harmless race and something writing over the engine's lock storage.
// Exported so the rwlock path in godot_shim.c uses the same test rather than
// a second copy that can drift from this one.
int sts2_plausible_lock_ptr(uint64_t v) { return looks_like_our_heap_ptr(v); }

void sts2_note_bad_lock_slot(const char *what, uint64_t v);
void sts2_note_bad_lock_slot_at(const char *what, uint64_t v, const void *at);

// `at` is the SLOT ADDRESS, and it is the whole point of this function now.
//
// ensure_cond/ensure_mutex do not merely read that slot -- when the value in
// it fails the plausibility test they CAS a heap pointer INTO it. Eight bytes,
// written into memory chosen by a heuristic.
//
// If the memory really is an uninitialised lock, that is correct. If it is
// anything else, this shim is corrupting it, eighteen times a session. The
// last run made that a live question rather than a theoretical one:
//
//   [godot] .NET: GodotPlugins initialized
//   [lock]  cond slot held 000a64657a696c61      -> "alized\n"
//
// -- the tail of the line printed immediately above it -- and four more
// holding UTF-32 pairs ('p','t'), ('b','a'), ('a','l'), ('o','i'), which is
// Godot's String (char32_t).
//
// Knowing WHERE the slot is settles it. Inside a loaded module's data, it is
// plausibly a real static lock that was never initialised. On the heap next to
// live strings, this shim is writing into someone else's memory.
static inline void note_bad_lock_slot_at(const char *what, uint64_t v, const void *at) {
  if (!v) return;                       // legitimately uninitialised

  // bionic's STATIC INITIALIZERS are not corruption.
  //
  // PTHREAD_MUTEX_INITIALIZER is 0, and the recursive and error-checking
  // variants put a small type mark in the low 32 bits. A slot holding one of
  // those is a mutex that has simply never been touched, which is the normal
  // first-use path -- counting it would have made this number meaningless from
  // the first frame, and I nearly shipped exactly that.
  const uint32_t low = (uint32_t)v;
  if ((v >> 32) == 0 &&
      (low == BIONIC_RECURSIVE_MARK || low == BIONIC_ERRCHECK_MARK))
    return;
  // Log DISTINCT values, not just the first.
  //
  // Only the first was reported before, and the count then climbed to 18
  // during gameplay with no record of what those later values were. The one
  // that did get logged -- 0101010101010101 -- is a clean memset(p, 1, 8)
  // fill, which is a pattern rather than random garbage, and it appeared at
  // startup. Whether the later ones share it is the whole question: if any of
  // them is 0x...0001, this is the same corruption as the crash that has been
  // chased for weeks, and the lock slots are simply where it is visible
  // earliest.
  //
  // Eight distinct values is enough to see whether there is one pattern or
  // several, and bounded so a steady drip cannot flood the log.
  {
    enum { KEEP = 8 };
    static uint64_t seen[KEEP];
    static int nseen;

    int known = 0;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == v) { known = 1; break; }

    if (!known && nseen < KEEP) {
      seen[nseen++] = v;
      debugPrintf("[lock] %s slot held %016llx -- not an aligned heap pointer,"
                  " treating as uninitialised.\n",
                  what, (unsigned long long)v);

      // Where is the slot this is about to be written into?
  // Report each distinct VALUE once, in full; count the rest.
  //
  // Three runs produced the same eight values, six of them in two or more
  // runs and three in all three -- 0x0101...01, UTF-32 'pt', UTF-32 '  '.
  // Stable values are not random corruption. They are FMOD carving locks out
  // of heap memory that previously held a Godot String: the hexdump around one
  // slot reads "cent cal" in UTF-32 with the slot on the "al", and another
  // value is UTF-32 'Go'.
  //
  // Every one of the seventeen was FIRST USE -- zero "WE OWNED THIS SLOT" --
  // so nothing live was ever clobbered. Rejecting the leftover and
  // initialising is the mechanism working.
  //
  // Which makes 480 log lines per run pure noise, and noise is not free: it
  // was drowning the [c11] mtx_init failure sitting at line 18 of the same
  // log, which is the one that actually mattered.
  //
  // Keyed on VALUE, not slot: slots move with ASLR every boot, values do not.
  static uint64_t seen[32];
  static uint32_t seen_n;
  static uint64_t suppressed;
  int already = 0;
  for (uint32_t i = 0; i < seen_n && i < 32; i++)
    if (seen[i] == v) { already = 1; break; }

  if (already) {
    if ((++suppressed % 64) == 0)
      debugPrintf("[lock] %llu further rejection(s) of already-reported values\n",
                  (unsigned long long)suppressed);
    sts2_bad_lock_slots++;
    return;
  }
  if (seen_n < 32) seen[seen_n++] = v;

  if (at) {
        so_module *m = so_find_module_by_addr(at);
        if (m)
          debugPrintf("[lock]   slot at %s+0x%lx (module data -- plausibly a "
                      "static lock)\n", m->name,
                      (unsigned long)((uintptr_t)at - (uintptr_t)m->load_virtbase));
        else
          debugPrintf("[lock]   slot at %p -- NOT in any loaded module. If this "
                      "is heap,\n"
                      "[lock]   the CAS below writes 8 bytes into memory that "
                      "may not be a lock.\n", at);
      }

      // Name the shapes worth recognising on sight.
      const uint32_t hi = (uint32_t)(v >> 32);
      if (v == 0x0101010101010101ULL)
        debugPrintf("[lock]   (memset fill of 0x01 -- a pattern, not garbage)\n");
      else if ((uint32_t)v == 1 && hi == 0)
        debugPrintf("[lock]   *** exactly 1 -- SAME SHAPE AS THE CRASH."
                    " See docs/RACE_HUNT.md ***\n");
      else if (hi == 0)
        debugPrintf("[lock]   (32-bit value in a 64-bit slot: %u)\n", (uint32_t)v);

      // Printable bytes mean this is text, not a lock. That single check is
      // what turned an unreadable hex counter into "GodotPlugins initialized".
      {
        char txt[9];
        int printable = 0;
        for (int i = 0; i < 8; i++) {
          unsigned char c = (unsigned char)(v >> (i * 8));
          txt[i] = (c >= 32 && c < 127) ? (char)c : '.';
          if (c >= 32 && c < 127) printable++;
        }
        txt[8] = 0;
        if (printable >= 4)
          debugPrintf("[lock]   *** looks like TEXT: \"%s\" -- this is not a "
                      "lock ***\n", txt);
      }
      debugFlush();
    }
  }
  sts2_bad_lock_slots++;

  // Everything we know about THIS slot, not just this moment.
  //
  // A rejected value on its own says the slot is wrong. The history says
  // whether we ever wrote it (a stale pointer we stored, later clobbered) or
  // never touched it (memory that was never a lock, or was reused out from
  // under one). Those need opposite fixes, and the value alone cannot tell
  // them apart.
  //
  // The hexdump matters for the same reason: the values seen on hardware were
  // Godot UTF-32 String data, and a string is recognisable from its
  // NEIGHBOURS far more readily than from eight bytes in isolation.
  if (at) {
    // DUMP FIRST, RECORD AFTER.
    //
    // The first version recorded the rejection and then dumped, so the
    // rejection was always in its own history and every slot reported
    // "1 event(s)". "NO PRIOR EVENTS" -- the whole reason the history exists,
    // the one line that distinguishes a slot we once owned from memory that
    // was never a lock -- could never print.
    //
    // The kind comes from `what` rather than being hardcoded: the header said
    // "cond" while the event said "mutex" in the same block.
    const uint8_t kind = (what && what[0] == 'c') ? LOCK_KIND_COND
                       : (what && what[0] == 'r') ? LOCK_KIND_RWLOCK
                                                  : LOCK_KIND_MUTEX;
    lock_trace_dump_slot(at, what);
    lock_trace_hexdump_around(at);
    lock_trace_record(LOCK_OP_REJECT, kind, at, v, v, sts2_lock_caller_get());
  }
}

static inline void note_bad_lock_slot(const char *what, uint64_t v) {
  note_bad_lock_slot_at(what, v, NULL);
}

void sts2_note_bad_lock_slot(const char *what, uint64_t v) {
  note_bad_lock_slot_at(what, v, NULL);
}

void sts2_note_bad_lock_slot_at(const char *what, uint64_t v, const void *at) {
  note_bad_lock_slot_at(what, v, at);
}

static pthread_mutex_t *make_mutex(int recursive) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return NULL;
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return NULL; }
  return m;
}

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  LOCK_CALLER_HERE();
  pthread_mutex_t *m = make_mutex(mutexattr && *mutexattr == 1);
  if (sts2_trace_all)
    debugPrintf("[pthread_mutex_init] uid=%p attr=%d -> m=%p\n", (void*)uid, mutexattr ? *mutexattr : -1, (void*)m);
  if (!m) return -1;
  {
    uint64_t *sl = lazy_slot(uid);
    LOCK_TRACE(LOCK_OP_INIT, LOCK_KIND_MUTEX, sl,
               __atomic_load_n(sl, __ATOMIC_RELAXED), (uint64_t)(uintptr_t)m);
    __atomic_store_n(sl, (uint64_t)(uintptr_t)m, __ATOMIC_RELEASE);
  }
  return 0;
}

// Lazy backing-object creation must be race-free: two threads touching the
// same statically-initialized bionic mutex/cond concurrently would otherwise
// each allocate an object, and the loser locks/waits on a different object
// than everyone else (lost wakeups -> boot deadlocks in WorkerThreadPool).
static pthread_mutex_t *ensure_mutex(pthread_mutex_t **uid) {
  uint64_t *slot = lazy_slot(uid);
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (looks_like_our_heap_ptr(cur)) {
    LOCK_TRACE(LOCK_OP_ENSURE_HIT, LOCK_KIND_MUTEX, slot, cur, cur);
    return (pthread_mutex_t *)(uintptr_t)cur;
  }
  note_bad_lock_slot_at("mutex", cur, slot);
  const uint32_t mark = *(volatile uint32_t *)uid; // bionic type mark, 4-aligned
  pthread_mutex_t *m = make_mutex(mark == BIONIC_RECURSIVE_MARK || mark == BIONIC_ERRCHECK_MARK);
  if (!m) return NULL;
  const uint64_t before = cur;
  if (__atomic_compare_exchange_n(slot, &cur, (uint64_t)(uintptr_t)m, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    LOCK_TRACE(LOCK_OP_CAS_WIN, LOCK_KIND_MUTEX, slot, before, (uint64_t)(uintptr_t)m);
    return m;
  }
  // Lost the race. `cur` now holds what the winner installed, which is the
  // interesting value -- recording it identifies the thread that beat us.
  LOCK_TRACE(LOCK_OP_CAS_LOSE, LOCK_KIND_MUTEX, slot, before, cur);
  pthread_mutex_destroy(m); // another thread won the race; use its object
  free(m);
  if (looks_like_our_heap_ptr(cur)) return (pthread_mutex_t *)(uintptr_t)cur;
  note_bad_lock_slot_at("mutex", cur, slot);
  return NULL;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  LOCK_CALLER_HERE();
  if (!uid) return 0;
  uint64_t *slot = lazy_slot(uid);
  uint64_t v = __atomic_exchange_n(slot, 0, __ATOMIC_ACQ_REL);
  LOCK_TRACE(LOCK_OP_DESTROY, LOCK_KIND_MUTEX, slot, v, 0);
  if (looks_like_our_heap_ptr(v)) {
    pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)v);
    free((void *)(uintptr_t)v);
  }
  return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  LOCK_CALLER_HERE();
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? to_bionic_errno(pthread_mutex_lock(m)) : -1;
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  LOCK_CALLER_HERE();
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? to_bionic_errno(pthread_mutex_trylock(m)) : -1;
}

// timedlock was NOT in the import table, so it resolved to newlib's real one,
// which would then operate on a slot holding OUR pointer -- reading it as a
// newlib mutex and writing newlib's representation over it.
//
// Nothing observed proves the engine calls it, but leaving one entry point of a
// shimmed family unrouted is the kind of gap that produces exactly the
// "something wrote a 32-bit value into pthread storage" signature being chased.
//
// The timeout is honoured approximately: libnx has no timed mutex acquire, so
// this polls. Callers use it to avoid unbounded blocking, and a coarse poll
// preserves that.
static int pthread_mutex_timedlock_fake(pthread_mutex_t **uid,
                                        const struct timespec *abstime) {
  pthread_mutex_t *m = ensure_mutex(uid);
  if (!m) return EINVAL;
  if (!abstime) return to_bionic_errno(pthread_mutex_lock(m));

  for (;;) {
    if (pthread_mutex_trylock(m) == 0) return 0;


    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > abstime->tv_sec ||
        (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
      // to_bionic_errno, not a bare ETIMEDOUT.
      //
      // ETIMEDOUT here is NEWLIB's -- 116. The caller is bionic and checks
      // against 110. `ret == ETIMEDOUT` never matched, so every timeout on a
      // timed mutex looked like an unrecognised error to whatever was waiting.
      //
      // pthread_cond_timedwait had this translated already. Its sibling did
      // not, which is the usual shape: the fix was applied where the bug was
      // observed rather than everywhere it lives.
      return to_bionic_errno(ETIMEDOUT);

    svcSleepThread(200000);   // 0.2 ms
  }
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  LOCK_CALLER_HERE();
  pthread_mutex_t *m = ensure_mutex(uid);
  return m ? pthread_mutex_unlock(m) : -1;
}

// make_mutex's cond-var counterpart. clock_id < 0 means "no condattr was
// ever provided, use whatever newlib's plain pthread_cond_init(c, NULL)
// defaults to"; >= 0 is a REAL bionic clockid_t to honor via
// pthread_condattr_setclock before creating the real backing cond.
static pthread_cond_t *make_cond(int clock_id) {
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return NULL;
  int ret;
  if (clock_id >= 0) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, (clockid_t)clock_id);
    ret = pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
  } else {
    ret = pthread_cond_init(c, NULL);
  }
  if (ret != 0) { free(c); return NULL; }
  return c;
}

// race-free lazy condvar backing, same aligned-slot scheme as ensure_mutex.
//
// CRASH FIX (03/ago): pthread_cond_init_fake used to throw away `condattr`
// entirely and always create the real backing cond with a NULL attr (=
// newlib's default clock). CoreCLR's PAL creates ALL its internal timed-wait
// condvars with pthread_condattr_setclock(CLOCK_MONOTONIC) explicitly (the
// standard cross-platform-runtime pattern, avoiding wall-clock jumps) --
// silently ignoring that meant a deadline computed via CLOCK_MONOTONIC
// arithmetic got handed to a condvar waiting against a DIFFERENT clock's
// notion of time entirely. Depending on the two clocks' actual values that
// either fires instantly forever or never fires at all; either way it
// explains "await Task.Delay(...) never resolves, no exception, nothing
// else affected" exactly -- Task.Delay's TimerQueue is backed by exactly
// this kind of clock-aware condvar wait.
static pthread_cond_t *ensure_cond_clocked(pthread_cond_t **cnd, int clock_id) {
  uint64_t *slot = lazy_slot(cnd);
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (looks_like_our_heap_ptr(cur)) {
    LOCK_TRACE(LOCK_OP_ENSURE_HIT, LOCK_KIND_COND, slot, cur, cur);
    return (pthread_cond_t *)(uintptr_t)cur;
  }
  note_bad_lock_slot_at("cond", cur, slot);
  pthread_cond_t *c = make_cond(clock_id);
  if (!c) return NULL;
  const uint64_t before = cur;
  if (__atomic_compare_exchange_n(slot, &cur, (uint64_t)(uintptr_t)c, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    LOCK_TRACE(LOCK_OP_CAS_WIN, LOCK_KIND_COND, slot, before, (uint64_t)(uintptr_t)c);
    return c;
  }
  LOCK_TRACE(LOCK_OP_CAS_LOSE, LOCK_KIND_COND, slot, before, cur);
  pthread_cond_destroy(c); // another thread won the race
  free(c);
  if (looks_like_our_heap_ptr(cur)) return (pthread_cond_t *)(uintptr_t)cur;
  note_bad_lock_slot_at("cond", cur, slot);
  return NULL;
}
// callers with no condattr on hand (a cond touched before/without an
// explicit pthread_cond_init, i.e. bionic's static-initializer case): no
// clock preference was ever expressed, so there's nothing to honor.
static pthread_cond_t *ensure_cond(pthread_cond_t **cnd) {
  return ensure_cond_clocked(cnd, -1);
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  LOCK_CALLER_HERE();
  if (!cnd) return -1;
  // POSIX pthread_cond_init() is called on UNINITIALIZED storage (e.g.
  // malloc(0x58) in .NET's SystemNative_LowLevelMonitor_Create). With
  // poison_free=0, that memory frequently holds a stale 8-byte-aligned heap
  // pointer from a prior allocation. Calling ensure_cond_clocked() without
  // overwriting the slot first caused looks_like_our_heap_ptr() to accept the
  // stale pointer as an existing condvar, corrupting live heap objects on wait/
  // signal and crashing inside free() on pthread_cond_destroy().
  // Mirror pthread_mutex_init_fake(): unconditionally allocate and store a
  // fresh backing condvar with the requested clock_id.
  int clock_id = condattr ? *condattr : -1;
  pthread_cond_t *c = make_cond(clock_id);
  if (sts2_trace_all)
    debugPrintf("[pthread_cond_init] cnd=%p clock=%d -> c=%p\n", (void*)cnd, clock_id, (void*)c);
  if (!c) return -1;
  {
    uint64_t *sl = lazy_slot(cnd);
    LOCK_TRACE(LOCK_OP_INIT, LOCK_KIND_COND, sl,
               __atomic_load_n(sl, __ATOMIC_RELAXED), (uint64_t)(uintptr_t)c);
    __atomic_store_n(sl, (uint64_t)(uintptr_t)c, __ATOMIC_RELEASE);
  }
  return 0;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  LOCK_CALLER_HERE();
  pthread_cond_t *c = ensure_cond(cnd);
  return c ? to_bionic_errno(pthread_cond_broadcast(c)) : -1;
}
static int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  LOCK_CALLER_HERE();
  pthread_cond_t *c = ensure_cond(cnd);
  return c ? to_bionic_errno(pthread_cond_signal(c)) : -1;
}
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  LOCK_CALLER_HERE();
  if (!cnd) return 0;
  uint64_t *slot = lazy_slot(cnd);
  uint64_t v = __atomic_exchange_n(slot, 0, __ATOMIC_ACQ_REL);
  // Recorded even when v is implausible. A destroy of a slot that never held
  // one of ours is itself evidence: it means the storage was reused between
  // init and destroy, which is the shape of the bug being chased.
  LOCK_TRACE(LOCK_OP_DESTROY, LOCK_KIND_COND, slot, v, 0);
  if (looks_like_our_heap_ptr(v)) {
    pthread_cond_destroy((pthread_cond_t *)(uintptr_t)v);
    free((void *)(uintptr_t)v);
  }
  return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  LOCK_CALLER_HERE();
  pthread_cond_t *c = ensure_cond(cnd);
  pthread_mutex_t *m = ensure_mutex(mtx);
  if (!c || !m) return -1;
  return to_bionic_errno(pthread_cond_wait(c, m));
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  LOCK_CALLER_HERE();
  pthread_cond_t *c = ensure_cond(cnd);
  pthread_mutex_t *m = ensure_mutex(mtx);
  if (!c || !m) return -1;
  return to_bionic_errno(pthread_cond_timedwait(c, m, t));
}

// waiters must block until the winner FINISHES the init routine (the old
// version let them race past while init was still running)
static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  int prev = __sync_val_compare_and_swap(once_control, 0, 1);
  if (prev == 0) {
    (*init_routine)();
    __sync_synchronize();
    *once_control = 2;
    return 0;
  }
  while (*once_control != 2)
    svcSleepThread(100 * 1000); // 0.1 ms
  return 0;
}

static int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
static int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// new engine threads need tpidr_el0 pointing at a stack-guard block first.
// each engine thread is also registered so the hang watchdog can dump it.
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

#define MAX_GAME_THREADS 64
static struct { Thread *thr; void *entry; int alive; } s_game_threads[MAX_GAME_THREADS];
static Mutex s_game_threads_lock;

int sts2_engine_threads(Thread **out_thr, void **out_entry, int max) {
  int n = 0;
  mutexLock(&s_game_threads_lock);
  for (int i = 0; i < MAX_GAME_THREADS && n < max; i++) {
    if (s_game_threads[i].alive && s_game_threads[i].thr) {
      out_thr[n] = s_game_threads[i].thr;
      out_entry[n] = s_game_threads[i].entry;
      n++;
    }
  }
  mutexUnlock(&s_game_threads_lock);
  return n;
}

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  // Every new thread needs its own NativeAOT TLS block installed before
  // running real code (nativeaot_shim.h) -- NativeAOT reads thread-statics
  // via tpidr_el0 + offset, and this was never being called here, only for
  // the main thread (nativeaot_shim_init). Confirmed on hardware (02/ago):
  // a thread StS2.so itself spawned (GC finalizer/server thread, per
  // so_load's pthread_create log right after "WKS::GCHeap::Initialize() ->
  // 0x00000000") crashed with a Data Abort at address 0 a few frames deep
  // inside StS2.so -- a thread-static read through an uninitialized
  // (garbage/zero) tpidr_el0 block matches exactly. Harmless for Godot's
  // own worker threads too: our libc/pthread shim doesn't use tpidr_el0 for
  // its own TLS (the main thread already overwrites it via
  // nativeaot_shim_init with no ill effect on debugPrintf/errno/etc.).
  nativeaot_setup_thread_tls();

  int slot = -1;
  mutexLock(&s_game_threads_lock);
  for (int i = 0; i < MAX_GAME_THREADS; i++) {
    if (!s_game_threads[i].alive) {
      s_game_threads[i].thr = threadGetSelf();
      s_game_threads[i].entry = (void *)ts.entry;
      s_game_threads[i].alive = 1;
      slot = i;
      break;
    }
  }
  mutexUnlock(&s_game_threads_lock);

  void *ret = ts.entry(ts.arg);

  if (slot >= 0) {
    mutexLock(&s_game_threads_lock);
    s_game_threads[slot].alive = 0;
    s_game_threads[slot].thr = NULL;
    mutexUnlock(&s_game_threads_lock);
  }
  return ret;
}

// Minimum stack for any engine thread.
//
// bionic's default is 1 MB and newlib's is a small fraction of that, so a
// caller that never touches pthread_attr_setstacksize still expects far more
// room than it gets here. Godot recurses deeply on the resource-loading and
// WorkerThreadPool threads -- scene instantiation walks a node tree, and each
// level costs a frame.
#define STS2_MIN_THREAD_STACK (1024 * 1024)

// Defined with the pthread_attr shims further down; forward-declared because
// pthread_create_fake is above them and C needs the declaration first.
size_t sts2_attr_stacksize(const void *attr);

static int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;

  // The attr used to be DISCARDED -- `(void)attr` and pthread_create(..., NULL,
  // ...). pthread_attr_setstacksize is in the import table, so the engine could
  // ask for a big stack, be told it succeeded, and get newlib's default.
  //
  // Run 21 crashed on it:
  //
  //   libgodot_android.so+0x3b57eac  f9005fff   -> str xzr, [sp, #0xb8]
  //   SP 00000045bd9fb7f0  <unreadable>
  //
  // A store to its OWN stack, faulting, with SP unmapped: a stack overflow,
  // not a bad pointer. It happened while loading scene resources, which is the
  // deepest recursion the engine does.
  //
  // The caller's attr is already in NEWLIB layout.
  //
  // pthread_attr_init and pthread_attr_setstacksize are in the import table
  // pointing at newlib's real implementations, so when the engine initialises
  // an attr and sets a stack size, newlib is what writes it -- into storage the
  // engine sized to bionic's struct, which is larger. Reading it at bionic's
  // offsets would return garbage; newlib's own getter is the correct reader.
  // config.thread_stack_kb, so the floor can be changed or disabled from
  // config.txt without a rebuild. Set 0 to restore the previous behaviour of
  // letting newlib choose -- the stack floor is the newest variable in the
  // wrapper, and run 22 produced a new engine crash, so being able to rule it
  // in or out in one boot is worth the knob.
  pthread_attr_t local;
  int have_attr = (pthread_attr_init(&local) == 0);
  size_t want = 0;

  if (config.thread_stack_kb) {
    want = (size_t)config.thread_stack_kb * 1024;

    if (attr) {
      // sts2_attr_stacksize, NOT newlib's getter. The attr shims above now own
      // the caller's storage and keep a tagged {tag, stacksize} in it, so
      // newlib's accessor would read our tag as its own fields and return
      // nonsense. Changing the representation means changing every reader.
      const size_t asked = sts2_attr_stacksize(attr);
      if (asked > want && asked <= 64u * 1024 * 1024)
        want = asked;   // honour a larger request; the floor is only a floor
    }
  }
  // want == 0 means pass NULL below: EXACTLY the pre-fix behaviour, so
  // `thread_stack_kb 0` is a clean A/B rather than a slightly different third
  // thing.

  if (have_attr && want) pthread_attr_setstacksize(&local, want);

  int rc = pthread_create(thread, (have_attr && want) ? &local : NULL,
                          thread_trampoline, ts);
  if (have_attr) pthread_attr_destroy(&local);

  if (rc != 0) {
    // Out of memory for that stack: retry at the default rather than fail the
    // thread outright, since a smaller stack usually still works.
    debugPrintf("[pthread_create] %zu KB stack refused (rc=%d), retrying default\n",
                want >> 10, rc);
    rc = pthread_create(thread, NULL, thread_trampoline, ts);
  }

  if (sts2_trace_all)
    debugPrintf("[pthread_create] entry=%p arg=%p stack=%zuKB -> rc=%d thread=%p\n",
                entry, arg, want >> 10, rc, thread ? (void*)*thread : NULL);
  return rc;
}

static int pthread_setschedparam_fake(pthread_t t, int policy, const void *param) {
  (void)t; (void)policy; (void)param; return 0;
}
static int pthread_setname_np_fake(pthread_t t, const char *name) {
  (void)t; (void)name; return 0;
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

// sched_yield.
//
// svcSleepThread(0) is NOT a general yield -- it is
// YieldType_WithoutCoreMigration, which only offers the CPU to threads already
// on this core. A thread waiting on another core never runs, so a spin-wait
// that yields between attempts can spin forever against a holder that is
// scheduled elsewhere.
//
// pvzultimate's HANDOFF lists this under Horizon traps and is specific about
// why it matters here: "every .NET spin-wait bottoms out there". The runtime
// spins in exactly this shape while waiting for GC and for lock acquisition.
//
// -2 is YieldType_ToAnyThread. The periodic real sleep is deliberate and also
// theirs: a yield only OFFERS the CPU, and under sustained contention offering
// it is not always enough to make progress.
static int sched_yield_fake(void) {
  static unsigned n;
  if ((++n & 0xFF) == 0)
    svcSleepThread(100000ULL);   // 0.1 ms every 256th call
  else
    svcSleepThread(-2LL);        // YieldType_ToAnyThread
  return 0;
}

static int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

static void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

// bionic struct stat conversion for the fd variant (path variant lives in
// libc_shim as stat_fake)
struct bionic_timespec64 { int64_t tv_sec; int64_t tv_nsec; };
// pthread_attr_t: never let newlib write into the caller's storage.
//
// bionic's pthread_attr_t is 56 bytes; newlib's is a different struct of
// unknown size. Passing the engine's 56-byte buffer to newlib's
// pthread_attr_init means newlib writes ITS layout there -- fine if newlib's is
// smaller, stack corruption if it is larger, and nobody has measured which.
//
// The engine uses attrs for exactly one thing here: carrying a stack size into
// pthread_create. So store just that, tagged, in the first 16 bytes -- well
// inside bionic's 56 -- and keep newlib entirely out of it.
// pthread_create_fake already builds its own newlib attr from the size, so
// nothing downstream needs newlib's representation.
#define BIONIC_ATTR_TAG 0x41545452u   /* "ATTR" */

struct sts2_attr {
  uint32_t tag;
  uint32_t _pad;
  uint64_t stacksize;
  uint64_t stackaddr;   // 0 = "not known"; see pthread_getattr_np_fake
};

static int pthread_attr_init_fake(void *attr) {
  if (!attr) return EINVAL;
  struct sts2_attr *a = attr;
  a->tag = BIONIC_ATTR_TAG;
  a->_pad = 0;
  a->stacksize = 0;                    // 0 = "unset", pthread_create picks
  a->stackaddr = 0;                    // must be written: the caller's buffer
                                       // is 56 bytes of whatever was there
  return 0;
}

static int pthread_attr_destroy_fake(void *attr) {
  if (attr) ((struct sts2_attr *)attr)->tag = 0;
  return 0;
}

static int pthread_attr_setstacksize_fake(void *attr, size_t sz) {
  if (!attr) return EINVAL;
  struct sts2_attr *a = attr;
  if (a->tag != BIONIC_ATTR_TAG) return EINVAL;
  a->stacksize = sz;
  return 0;
}

// pthread_attr_getstacksize is deliberately NOT provided.
//
// It was written alongside pthread_attr_getstack and never registered, so the
// compiler reported it unused. Checked before deleting rather than after:
// nothing imports it -- not libgodot_android.so, not libc++_shared.so, and not
// the AOT payload, which imports pthread_attr_getstack (base AND size) and
// gets it from pthread_attr_getstack_fake below.
//
// Removed rather than registered. A shim nothing calls is a shim nobody
// maintains, and the next person to read the table should not have to work out
// which of two near-identical accessors is the live one.

// These two were passing through to newlib as well. Now that the storage holds
// OUR tagged representation, newlib's setdetachstate would overwrite the tag
// and getstack would report our tag as an address. Both had to move with the
// representation.
static int pthread_attr_getstack_fake(void *attr, void **addr, size_t *sz) {
  if (!attr || !addr || !sz) return EINVAL;
  const struct sts2_attr *a = attr;
  if (a->tag != BIONIC_ATTR_TAG) return EINVAL;
  *addr = (void *)(uintptr_t)a->stackaddr;
  *sz = (size_t)a->stacksize;
  return 0;
}

// pthread_getattr_np -- the REAL stack bounds of a running thread.
//
// This used to pass straight through to newlib, and that was a hole big enough
// to matter. The import table is mixed: pthread_attr_init writes OUR tagged
// sts2_attr into the caller's storage, while pthread_getattr_np wrote NEWLIB's
// layout into the same storage. pthread_attr_getstack then found no tag,
// returned EINVAL, and -- this is the part that matters -- **left *addr and
// *sz untouched**.
//
// Only ONE module imports pthread_getattr_np: libsts2.so, the NativeAOT
// payload. libgodot_android.so does not. NativeAOT uses it for one thing:
// finding a thread's stack bounds so the GC knows what range to scan.
//
// So the runtime asked where the stack was, got EINVAL and two uninitialised
// locals, and scanned whatever they happened to contain. A GC scanning the
// wrong range treats arbitrary memory as object references -- and rewrites
// them during compaction. That is pointer-sized damage, in any module, at an
// address decided by whatever was on the stack at the time: deterministic for
// a given save, indifferent to the renderer, and never near the code that
// caused it.
//
// Even with a matching tag the old getstack returned `*addr = NULL`, so the
// best case was a stack range starting at address 0.
//
// The fix keeps the tagged representation as the single owner of caller
// storage: ask newlib, in newlib's own layout, in a LOCAL attr -- then write
// the answer out in ours.
static int pthread_getattr_np_fake(pthread_t th, void *attr) {
  if (!attr) return EINVAL;
  struct sts2_attr *a = attr;

  pthread_attr_t local;
  void  *base = NULL;
  size_t size = 0;

  const int ok = (pthread_getattr_np(th, &local) == 0) &&
                 (pthread_attr_getstack(&local, &base, &size) == 0);
  if (ok) pthread_attr_destroy(&local);

  a->tag       = BIONIC_ATTR_TAG;
  a->_pad      = 0;
  a->stacksize = ok ? (uint64_t)size : 0;
  a->stackaddr = ok ? (uint64_t)(uintptr_t)base : 0;

  if (!ok) {
    static int warned;
    if (!warned) {
      warned = 1;
      debugPrintf("[pthread] getattr_np could not read the stack bounds. "
                  "NativeAOT uses these\n"
                  "[pthread] to bound GC stack scanning -- see "
                  "pthread_getattr_np_fake.\n");
      debugFlush();
    }
    return EINVAL;
  }

  static int logged;
  if (logged < 4) {
    logged++;
    debugPrintf("[pthread] getattr_np: stack %p + %zu KB\n", base, size >> 10);
  }
  return 0;
}

static int pthread_attr_setdetachstate_fake(void *attr, int state) {
  (void)state;                   // threads here are joined or detached by libnx
  if (!attr) return EINVAL;
  return ((struct sts2_attr *)attr)->tag == BIONIC_ATTR_TAG ? 0 : EINVAL;
}

// Reads the size back out of an attr the engine handed to pthread_create.
// Returns 0 when the attr is not one of ours, so the caller falls back to its
// own default rather than trusting a stray value.
size_t sts2_attr_stacksize(const void *attr) {
  if (!attr) return 0;
  const struct sts2_attr *a = attr;
  return a->tag == BIONIC_ATTR_TAG ? (size_t)a->stacksize : 0;
}

// bionic's struct tm has two fields newlib may not write.
//
//   int tm_sec..tm_isdst;   // 9 ints, both libcs
//   long tm_gmtoff;         // bionic
//   const char *tm_zone;    // bionic
//
// If newlib stops after the nine ints, the engine reads tm_gmtoff and tm_zone
// as whatever was on the stack -- and tm_zone is a pointer it may dereference,
// for instance formatting %Z. Zeroing the whole bionic-sized struct first turns
// an uninitialised pointer into a null one.
//
// libgodot imports gmtime_r, localtime_r and strftime, so this is a live path,
// not a hypothetical.
struct bionic_tm {
  int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  int64_t tm_gmtoff;
  const char *tm_zone;
};

static void *fill_bionic_tm(const struct tm *in, void *out) {
  struct bionic_tm *b = out;
  if (!in || !b) return NULL;
  memset(b, 0, sizeof(*b));           // tm_gmtoff and tm_zone default to zero
  b->tm_sec = in->tm_sec;   b->tm_min  = in->tm_min;   b->tm_hour = in->tm_hour;
  b->tm_mday = in->tm_mday; b->tm_mon  = in->tm_mon;   b->tm_year = in->tm_year;
  b->tm_wday = in->tm_wday; b->tm_yday = in->tm_yday;  b->tm_isdst = in->tm_isdst;
  return b;
}

static void *localtime_r_fake(const time_t *t, void *out) {
  struct tm tmp;
  if (!t || !out || !localtime_r(t, &tmp)) return NULL;
  return fill_bionic_tm(&tmp, out);
}

static void *gmtime_r_fake(const time_t *t, void *out) {
  struct tm tmp;
  if (!t || !out || !gmtime_r(t, &tmp)) return NULL;
  return fill_bionic_tm(&tmp, out);
}

struct bionic_stat64 {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec64 st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

static void fill_bionic_stat(const struct stat *in, void *st) {
  struct bionic_stat64 *out = st;
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino;
  out->st_mode = in->st_mode; out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size;
  out->st_blksize = in->st_blksize; out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

static int fstat_fake(int fd, void *st) {
  struct stat in;

  // /dev/urandom is synthetic: open_fake hands back URANDOM_FD and read_fake
  // serves it from Horizon's csrng, but newlib knows nothing about that
  // descriptor, so a real fstat() on it returns EBADF.
  //
  // That matters because a caller validating the fd treats failure as "this
  // descriptor is unusable" and gives up. Hardware run 12:
  //
  //   System.Security.Cryptography.CryptographicException
  //     at System.Guid.NewGuid()
  //     at SentryService..cctor()
  //
  // Guid.NewGuid() failing takes far more with it than Sentry -- it is reached
  // from SaveManager.SaveSettings() in the same trace.
  //
  // Answered as a character device, then run through fill_bionic_stat like
  // every other path so the layout conversion stays in one place.
  if (sts2_is_urandom_fd(fd)) {
    memset(&in, 0, sizeof(in));
    in.st_mode = S_IFCHR | 0666;
    in.st_nlink = 1;
    fill_bionic_stat(&in, st);
    return 0;
  }

  if (fstat(fd, &in) != 0) return -1;
  fill_bionic_stat(&in, st);
  return 0;
}
static int lstat_fake(const char *path, void *st) {
  struct stat in;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  if (stat(path, &in) != 0) return -1; // no symlinks on fatfs: lstat == stat
  fill_bionic_stat(&in, st);
  return 0;
}
// bionic aarch64 has no 32/64 stat split (off_t/ino_t are already 64-bit),
// so unlike glibc, stat64/fstat64/lseek64 are just the plain calls under a
// different name -- StS2.so's NativeAOT runtime imports the "64" names
// (see reference/nativeaot-probe/), the engine imports the plain ones; both
// resolve to the exact same bionic-shaped implementation.
static int stat64_fake(const char *path, void *st) { return stat_fake(path, st); }
static int fstat64_fake(int fd, void *st) { return fstat_fake(fd, st); }
static int lstat64_fake(const char *path, void *st) { return lstat_fake(path, st); }
static off_t lseek64_fake(int fd, off_t off, int whence) { return lseek_fake(fd, off, whence); }
// devkitA64 newlib has no real pread; emulate with save/seek/read/restore
// (not atomic against a concurrent seek on the same fd from another thread,
// same caveat reference/nativeaot-celeste64/crypto_stubs.c calls out).
static ssize_t pread_fake(int fd, void *buf, size_t count, off_t offset) {
  off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved == (off_t)-1) return -1;
  if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
  ssize_t got = read(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  return got;
}
// same save/seek/write/restore pattern as pread_fake, for the write side.
static ssize_t pwrite_fake(int fd, const void *buf, size_t count, off_t offset) {
  off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved == (off_t)-1) return -1;
  if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
  ssize_t wrote = write(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  return wrote;
}
// fadvise/fallocate are pure performance hints on real Linux (readahead,
// disk-space preallocation) -- our FS has no equivalent, so a no-op success
// is exactly as correct as any real implementation would be from the
// caller's perspective (both just optimize; neither changes observable
// behavior).
static int posix_fadvise64_fake(int fd, off_t offset, off_t len, int advice) {
  (void)fd; (void)offset; (void)len; (void)advice;
  return 0;
}
static int fallocate_fake(int fd, int mode, off_t offset, off_t len) {
  (void)fd; (void)mode; (void)offset; (void)len;
  return 0;
}

// bionic statfs (arm64): report a large amount of free space so the engine's
// free-disk-space check passes.
struct bionic_statfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree;
  uint64_t f_fsid;
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static int statfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = 0x1000;
  s->f_frsize = 0x1000;
  s->f_blocks = (4ull * 1024 * 1024 * 1024) / 0x1000; // 4 GiB total
  s->f_bfree = s->f_bavail = (2ull * 1024 * 1024 * 1024) / 0x1000; // 2 GiB free
  s->f_namelen = 255;
  return 0;
}
static int fstatfs_fake(int fd, void *buf) { (void)fd; return statfs_fake(NULL, buf); }

// minimal mmap/munmap: Switch newlib has no real mmap. Anonymous maps become
// page-aligned heap; file maps read the range into heap. munmap frees it.
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_FIXED
#define MAP_FIXED 0x10
#endif
// Shared by libgodot_android.so (occasional file-backed mapping -- the
// original behavior below) AND, once loaded, StS2.so's NativeAOT GC
// (reserve a big range with one mmap, then "commit" sub-ranges via
// MAP_FIXED, and later munmap sub-ranges of that same block -- a pattern
// plain alloc-per-call + free-on-unmap cannot support: freeing a pointer
// that isn't the head of its allocation corrupts the malloc heap). Model:
// reference/nativeaot-celeste64/nativeaot_shims.c, same fix, same reasoning,
// validated on hardware there. The two behaviors don't conflict: a
// non-MAP_FIXED call still behaves exactly as before (fresh, zeroed,
// page-aligned block), which is all the GDScript engine ever does.
// Largest mmap reservation seen, so mprotect (nativeaot_shim.c) can report
// commits WITHIN it. See sts2_report_reservation_commits().
// Defined by the linker's --wrap; see gpu_arena.c.
void *__real_memalign(size_t align, size_t size);

uintptr_t sts2_biggest_map_base;
size_t    sts2_biggest_map_len;

static void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)prot;
  if (len == 0) { debugPrintf("[mmap] len=0 -> FAILED\n"); return (void *)-1; } // MAP_FAILED
  if (flags & MAP_FIXED) {
    // "Commit": addr must already be a live mapping from an earlier
    // non-fixed call. POSIX anonymous-mmap semantics require committed
    // pages to read as zero; restore that (our munmap below is a no-op, so
    // a sub-range can otherwise still hold stale data from before it was
    // "decommitted").
    if (!addr) { debugPrintf("[mmap] MAP_FIXED addr=NULL len=0x%lx -> FAILED\n", (unsigned long)len); return (void *)-1; }
    memset(addr, 0, len);
    debugPrintf("[mmap] MAP_FIXED addr=%p len=0x%lx -> OK\n", addr, (unsigned long)len);
    return addr;
  }
  // A "reservation" here is a REAL allocation.
  //
  // The .NET GC reserves address space with PROT_NONE and commits subranges
  // later; on a platform with demand paging that costs nothing. This mmap is
  // malloc-backed, so it costs the whole length immediately, and munmap below
  // is a no-op so it is never returned.
  //
  // Run 23: a single 1024 MB mmap from DOTNET_GCRegionRange consumed a physical
  // gigabyte and the engine then failed texture uploads with GL_OUT_OF_MEMORY.
  // Anything this large is worth seeing in the log without VERBOSE_IO, because
  // the symptom appears much later and in a completely different subsystem.
  if (len >= (16u << 20))
    debugPrintf("[mmap] LARGE: %lu MB (flags=0x%x) -- this is a real allocation, "
                "not a reservation\n", (unsigned long)(len >> 20), flags);

  // __real_memalign, NOT memalign.
  //
  // With --wrap in play (Makefile), a plain memalign() here goes through
  // __wrap_memalign and lands in the GPU arena -- it is page-aligned and large,
  // which is exactly the arena's filter. Run 27 showed the consequence: the
  // GC's 256 MB reservation was sitting inside the arena, so "[gpua] peak
  // 1653 MB" was not GPU demand and the arena hit its chunk limit serving
  // allocations that were never GPU buffers.
  //
  // It also put mprotect'd pages inside arena chunks, which is at best
  // confusing and at worst a permissions surprise.
  //
  // The arena exists to keep GPU buffers away from everything else; that only
  // works if everything else stays out of it.
  void *p = __real_memalign(0x1000, len);

  // Remember the largest reservation, so mprotect below can report commits
  // WITHIN it rather than leaving the number to be reconstructed from the log.
  //
  // Run 25 was a regression caused by getting exactly that reconstruction
  // wrong: I totalled MAP_FIXED commits (14 MB), ignored the mprotect commits
  // in the same output (178.9 MB of distinct pages), and cut the GC's
  // reservation below what it needs. Both numbers were on screen together.
  if (p && len >= (64u << 20) && len > sts2_biggest_map_len) {
    sts2_biggest_map_base = (uintptr_t)p;
    sts2_biggest_map_len  = len;
  }
  if (!p) {
    debugPrintf("[mmap] len=0x%lx flags=0x%x -> FAILED (alloc)\n", (unsigned long)len, flags);
    return (void *)-1; // MAP_FAILED
  }
  if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    lseek(fd, off, SEEK_SET);
    ssize_t got = read(fd, p, len);
    (void)got;
    lseek(fd, cur, SEEK_SET);
  } else {
    memset(p, 0, len);
  }
  debugPrintf("[mmap] len=0x%lx flags=0x%x -> %p\n", (unsigned long)len, flags, p);
  return p;
}
static int munmap_fake(void *addr, size_t len) {
  // Do NOT free(): see the mmap_fake comment above -- a GC-style caller
  // unmaps sub-ranges of a larger allocation, which free() cannot handle.
  // This does mean memory the GDScript engine mmap'd is no longer released
  // on munmap (a leak, not a corruption) -- the same tradeoff Celeste64
  // already validated as the safer of the two failure modes on hardware.
  (void)addr; (void)len;
  return 0;
}
static void *mmap64_fake(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
  return mmap_fake(addr, len, prot, flags, fd, (off_t)off);
}
static int madvise_fake(void *addr, size_t len, int adv) {
  (void)addr; (void)len; (void)adv; return 0;
}

// ---------------------------------------------------------------------------
// networking: stubbed (no multiplayer / editor debugger on this port)
// ---------------------------------------------------------------------------

static int   net_err3(long a, long b, long c) { (void)a;(void)b;(void)c; errno = ENOSYS; return -1; }

// send/recv return ssize_t, NOT int, and on AArch64 that difference is not
// cosmetic.
//
// A function declared to return `int` writes only w0; the upper half of x0 is
// left holding whatever was there. A caller that reads the result as ssize_t --
// which every send/recv caller does -- therefore gets `garbage << 32 | -1`,
// and `if (n < 0)` is FALSE unless the garbage happens to be all ones. The
// error is then read as an enormous positive byte count.
//
// That is the exact shape of value this port has been chasing: a small
// intended result in the low 32 bits with unrelated bytes above it. These four
// are not proven to be the source -- networking may never be called here -- but
// they are a real instance of the mechanism, found by checking every import's
// return width against libc rather than by waiting for another crash.
static ssize_t net_errsz(long a, long b, long c) {
  (void)a; (void)b; (void)c;
  errno = ENOSYS;
  return -1;
}
static void *net_null(void) { return NULL; }
static int   net_0(void) { return 0; }
static int   gethostname_fake(char *n, size_t l) { if (n && l) n[0] = 0; return -1; }
static int   getaddrinfo_fake(const char *a, const char *b, const void *c, void **res) {
  (void)a; (void)b; (void)c; if (res) *res = NULL; return -4; // EAI_FAIL
}
static void  freeaddrinfo_fake(void *ai) { (void)ai; }
static const char *gai_strerror_fake(int e) { (void)e; return "EAI_FAIL"; }

// process control: single-process system, none of these can work
static int proc_enosys(void) { errno = ENOSYS; return -1; }
static int getuid_fake(void) { return 0; }

// ---------------------------------------------------------------------------
// OpenSLES: real minimal implementation over audout (see audio.c)
// ---------------------------------------------------------------------------

extern uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                               uint32_t numInterfaces, const void *pInterfaceIds,
                               const uint8_t *pInterfaceRequired);
extern const void *SL_IID_ENGINE, *SL_IID_PLAY, *SL_IID_RECORD,
                  *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_ANDROIDCONFIGURATION,
                  *SL_IID_BUFFERQUEUE, *SL_IID_EFFECTSEND, *SL_IID_ENVIRONMENTALREVERB;

// ---------------------------------------------------------------------------
// zstd trace hooks (zstd itself is built into libgodot; tracing is optional
// and weakly bound -- report "no tracing")
// ---------------------------------------------------------------------------

static unsigned long long zstd_trace_begin(void) { return 0; }
static void zstd_trace_end(void) {}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// gap shims (see tools/import_gap.py)
// ---------------------------------------------------------------------------

// bionic's assert. Signature: __assert2(file, line, function, message).
// newlib has __assert_func with a different argument order, so this cannot be
// aliased -- it needs its own wrapper.
static void sts2_assert2(const char *file, int line, const char *func, const char *msg) {
  fatal_error("assert failed\n%s:%d\n%s\n%s",
              file ? file : "?", line, func ? func : "?", msg ? msg : "?");
}

// FMOD raises its mixer thread's priority. Horizon priorities are per-thread
// and set at creation; there is no post-hoc nice() equivalent, and the audio
// threads FMOD creates already come through the pthread shim, which assigns a
// priority there. Accept and ignore rather than fail -- FMOD does not check
// the return, but returning -1 would set errno and some callers log it.
static int sts2_setpriority(int which, int who, int prio) {
  (void)which; (void)who; (void)prio;
  return 0;
}

static int sts2_getpriority(int which, int who) {
  (void)which; (void)who;
  return 0;   // "normal"
}

static const DynLibFunction dynlib_functions[] = {
  // --- OpenSLES (audio.c) ---
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_EFFECTSEND", (uintptr_t)&SL_IID_EFFECTSEND },
  { "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB },

  // --- bionic runtime / logging ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&stdin_fake },
  { "stdout", (uintptr_t)&stdout_fake },
  { "stderr", (uintptr_t)&stderr_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__register_atfork", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "abort", (uintptr_t)&sts2_abort_hook }, // logs caller before really aborting -- see nativeaot_shim.c
  { "exit", (uintptr_t)&sts2_exit_hook },   // ditto
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "getenv", (uintptr_t)&sts2_getenv_hook }, // logs every name queried -- see nativeaot_shim.c
  { "setenv", (uintptr_t)&setenv },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "raise", (uintptr_t)&raise },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "kill", (uintptr_t)&proc_enosys },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "gettid", (uintptr_t)&gettid_fake2 },

  // --- dynamic loader ---
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },

  // --- setjmp/longjmp ---
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "madvise", (uintptr_t)&madvise_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strncat", (uintptr_t)&strncat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strnlen", (uintptr_t)&strnlen },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "atoi", (uintptr_t)&atoi },
  { "atol", (uintptr_t)&atol },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "tolower", (uintptr_t)&tolower },
  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "lldiv", (uintptr_t)&lldiv },

  // --- ctype ---
  { "isalnum", (uintptr_t)&isalnum },
  { "islower", (uintptr_t)&islower },
  { "isupper", (uintptr_t)&isupper },
  { "isspace", (uintptr_t)&isspace },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "isdigit_l", (uintptr_t)&isdigit_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l_fake },
  { "islower_l", (uintptr_t)&islower_l_fake },
  { "isupper_l", (uintptr_t)&isupper_l_fake },
  { "tolower_l", (uintptr_t)&tolower_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },

  // --- wide char / wctype ---
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "getwc", (uintptr_t)&getwc },
  { "fputwc", (uintptr_t)&fputwc },
  { "ungetwc", (uintptr_t)&ungetwc },

  // --- locale ---
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },

  // --- printf / scanf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "vprintf", (uintptr_t)&vprintf_fake },
  { "putchar", (uintptr_t)&putchar_fake },
  { "puts", (uintptr_t)&puts_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "perror", (uintptr_t)&perror_fake },

  // --- stdio over fake __sF + buffered fopen ---
  { "fopen", (uintptr_t)&fopen_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "ftello", (uintptr_t)&ftello_fake },
  { "fgetpos", (uintptr_t)&fgetpos_fake },
  { "fsetpos", (uintptr_t)&fsetpos_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof_fake },
  { "fgetc", (uintptr_t)&getc_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "rewind", (uintptr_t)&rewind_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fcntl", (uintptr_t)&fcntl },
  { "fsync", (uintptr_t)&fsync },
  { "fstat", (uintptr_t)&fstat_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "fstatfs", (uintptr_t)&fstatfs_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "posix_fadvise64", (uintptr_t)&posix_fadvise64_fake },
  { "fallocate", (uintptr_t)&fallocate_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "unlink", (uintptr_t)&unlink_fake },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "rmdir", (uintptr_t)&rmdir_fake },
  { "lseek", (uintptr_t)&lseek_fake },
  { "open", (uintptr_t)&open_fake },
  { "__open_2", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake },
  { "read", (uintptr_t)&read_fake },
  { "getrandom",  (uintptr_t)&sts2_getrandom },
  { "getentropy", (uintptr_t)&sts2_getentropy },
  { "write", (uintptr_t)&sts2_write_hook }, // mirrors fd 1/2 into sts2_debug.log -- see nativeaot_shim.c
  { "access", (uintptr_t)&access_fake },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "chdir", (uintptr_t)&chdir_fake },   // rebased like every other path call
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "readlink", (uintptr_t)&readlink_fake },
  { "realpath", (uintptr_t)&realpath_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "closedir", (uintptr_t)&closedir_fake },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "ftruncate64", (uintptr_t)&ftruncate }, // bionic aarch64: no 32/64 split, same as stat64/fstat64
  { "truncate", (uintptr_t)&truncate_fake },
  { "chmod", (uintptr_t)&ret0 },
  { "fchmod", (uintptr_t)&ret0 },
  { "fchmodat", (uintptr_t)&fchmodat_fake },
  { "utimensat", (uintptr_t)&utimensat_fake },
  { "link", (uintptr_t)&proc_enosys },
  { "symlink", (uintptr_t)&proc_enosys },
  { "mkfifo", (uintptr_t)&proc_enosys },
  { "mkstemp", (uintptr_t)&mkstemp_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "isatty", (uintptr_t)&isatty_fake },
  { "ioctl", (uintptr_t)&ret0 },
  { "poll", (uintptr_t)&ret0 },
  { "select", (uintptr_t)&proc_enosys },
  { "sendfile", (uintptr_t)&proc_enosys },
  { "dup2", (uintptr_t)&proc_enosys },
  { "pipe", (uintptr_t)&proc_enosys },
  { "popen", (uintptr_t)&net_null },
  { "pclose", (uintptr_t)&proc_enosys },

  // --- math ---
  { "acos", (uintptr_t)&acos }, { "acosf", (uintptr_t)&acosf },
  { "acosh", (uintptr_t)&acosh }, { "acoshf", (uintptr_t)&acoshf },
  { "asin", (uintptr_t)&asin }, { "asinf", (uintptr_t)&asinf },
  { "asinh", (uintptr_t)&asinh }, { "asinhf", (uintptr_t)&asinhf },
  { "atan", (uintptr_t)&atan }, { "atanf", (uintptr_t)&atanf },
  { "atanh", (uintptr_t)&atanh }, { "atanhf", (uintptr_t)&atanhf },
  { "atan2", (uintptr_t)&atan2 }, { "atan2f", (uintptr_t)&atan2f },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "cos", (uintptr_t)&cos }, { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh }, { "coshf", (uintptr_t)&coshf },
  { "sin", (uintptr_t)&sin }, { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh }, { "sinhf", (uintptr_t)&sinhf },
  { "tan", (uintptr_t)&tan }, { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh }, { "tanhf", (uintptr_t)&tanhf },
  { "ceil", (uintptr_t)&ceil }, { "ceilf", (uintptr_t)&ceilf },
  { "floor", (uintptr_t)&floor }, { "floorf", (uintptr_t)&floorf },
  { "sqrt", (uintptr_t)&sqrt }, { "sqrtf", (uintptr_t)&sqrtf },
  { "exp", (uintptr_t)&exp }, { "expf", (uintptr_t)&expf },
  { "exp2", (uintptr_t)&exp2 }, { "exp2f", (uintptr_t)&exp2f },
  { "pow", (uintptr_t)&pow }, { "powf", (uintptr_t)&powf },
  { "log", (uintptr_t)&log }, { "logf", (uintptr_t)&logf },
  { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 }, { "log2f", (uintptr_t)&log2f },
  { "log10l", (uintptr_t)&log10l_fake },
  { "hypot", (uintptr_t)&hypot }, { "hypotf", (uintptr_t)&hypotf },
  { "fmod", (uintptr_t)&fmod }, { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "ldexp", (uintptr_t)&ldexp }, { "ldexpf", (uintptr_t)&ldexpf },
  { "ilogb", (uintptr_t)&ilogb },
  { "nextafter", (uintptr_t)&nextafter },
  { "remainder", (uintptr_t)&remainder },
  { "lrintf", (uintptr_t)&lrintf },
  { "lroundf", (uintptr_t)&lroundf },
  { "modf", (uintptr_t)&modf }, { "modff", (uintptr_t)&modff },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },

  // --- fenv ---
  { "fegetenv", (uintptr_t)&fegetenv },
  { "fesetenv", (uintptr_t)&fesetenv },
  { "fesetround", (uintptr_t)&fesetround },

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r_fake },
  { "localtime_r", (uintptr_t)&localtime_r_fake },
  { "mktime", (uintptr_t)&mktime },
  { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // --- syscalls / process ---
  { "getpid", (uintptr_t)&getpid },
  { "getuid", (uintptr_t)&getuid_fake },
  { "getpwuid", (uintptr_t)&ret0 },
  { "getrlimit", (uintptr_t)&getrlimit_fake },
  { "fork", (uintptr_t)&proc_enosys },
  { "waitpid", (uintptr_t)&proc_enosys },
  { "execvp", (uintptr_t)&proc_enosys },
  { "setsid", (uintptr_t)&proc_enosys },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_gettid_np", (uintptr_t)&pthread_gettid_np_fake },
  { "pthread_key_create", (uintptr_t)&sts2_pthread_key_create_hook }, // temporary trace, see nativeaot_shim.c
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // --- POSIX semaphores (pointer-indirected; libc_shim) ---
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- scheduling ---
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_getaffinity", (uintptr_t)&sched_getaffinity_fake },
  { "sched_setaffinity", (uintptr_t)&sched_setaffinity_fake },

  // --- networking (stubbed) ---
  { "socket", (uintptr_t)&proc_enosys },
  { "bind", (uintptr_t)&net_err3 },
  { "listen", (uintptr_t)&net_err3 },
  { "accept", (uintptr_t)&net_err3 },
  { "connect", (uintptr_t)&net_err3 },
  { "send", (uintptr_t)&net_errsz },
  { "sendto", (uintptr_t)&net_errsz },
  { "recv", (uintptr_t)&net_errsz },
  { "recvfrom", (uintptr_t)&net_errsz },
  { "setsockopt", (uintptr_t)&net_err3 },
  { "getsockopt", (uintptr_t)&net_err3 },
  { "getsockname", (uintptr_t)&net_err3 },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&net_err3 },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "if_indextoname", (uintptr_t)&net_null },
  { "if_nametoindex", (uintptr_t)&net_0 },
  { "inet_pton", (uintptr_t)&net_0 },
  { "in6addr_any", (uintptr_t)&in6addr_any_fake },

  // --- zlib (host -lz) ---
  { "adler32", (uintptr_t)&adler32 },
  { "crc32", (uintptr_t)&crc32 },
  { "compress", (uintptr_t)&compress },
  { "compress2", (uintptr_t)&compress2 },
  { "compressBound", (uintptr_t)&compressBound },
  { "uncompress", (uintptr_t)&uncompress },
  { "deflate", (uintptr_t)&deflate },
  { "deflateBound", (uintptr_t)&deflateBound },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit_", (uintptr_t)&deflateInit_ },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateReset2", (uintptr_t)&inflateReset2 },

  // --- zstd trace hooks ---
  { "ZSTD_trace_compress_begin", (uintptr_t)&zstd_trace_begin },
  { "ZSTD_trace_compress_end", (uintptr_t)&zstd_trace_end },
  { "ZSTD_trace_decompress_begin", (uintptr_t)&zstd_trace_begin },
  { "ZSTD_trace_decompress_end", (uintptr_t)&zstd_trace_end },

  // --- Android NDK: asset manager (godot_shim) ---
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },

  // --- Android NDK: looper / native window ---
  { "ALooper_acquire", (uintptr_t)&ret0 },
  { "ALooper_release", (uintptr_t)&ret0 },
  { "ALooper_prepare", (uintptr_t)&ret0 },
  { "ALooper_pollOnce", (uintptr_t)&retm1 },
  { "ALooper_wake", (uintptr_t)&ret0 },
  { "ANativeWindow_acquire", (uintptr_t)&ret0 },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },

  // --- Android NDK: camera2 / media (stubbed; CameraServer stays empty) ---
  { "ACameraManager_create", (uintptr_t)&ret0 },
  { "ACameraManager_delete", (uintptr_t)&ret0 },
  { "ACameraManager_getCameraIdList", (uintptr_t)&retm1 },
  { "ACameraManager_deleteCameraIdList", (uintptr_t)&ret0 },
  { "ACameraManager_getCameraCharacteristics", (uintptr_t)&retm1 },
  { "ACameraManager_openCamera", (uintptr_t)&retm1 },
  { "ACameraMetadata_free", (uintptr_t)&ret0 },
  { "ACameraMetadata_getConstEntry", (uintptr_t)&retm1 },
  { "ACameraDevice_close", (uintptr_t)&ret0 },
  { "ACameraDevice_createCaptureRequest", (uintptr_t)&retm1 },
  { "ACameraDevice_createCaptureSession", (uintptr_t)&retm1 },
  { "ACameraCaptureSession_close", (uintptr_t)&ret0 },
  { "ACameraCaptureSession_setRepeatingRequest", (uintptr_t)&retm1 },
  { "ACameraCaptureSession_stopRepeating", (uintptr_t)&retm1 },
  { "ACameraOutputTarget_create", (uintptr_t)&retm1 },
  { "ACameraOutputTarget_free", (uintptr_t)&ret0 },
  { "ACaptureRequest_addTarget", (uintptr_t)&retm1 },
  { "ACaptureRequest_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutput_create", (uintptr_t)&retm1 },
  { "ACaptureSessionOutput_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutputContainer_create", (uintptr_t)&retm1 },
  { "ACaptureSessionOutputContainer_free", (uintptr_t)&ret0 },
  { "ACaptureSessionOutputContainer_add", (uintptr_t)&retm1 },
  { "AImageReader_new", (uintptr_t)&retm1 },
  { "AImageReader_delete", (uintptr_t)&ret0 },
  { "AImageReader_getWindow", (uintptr_t)&retm1 },
  { "AImageReader_setImageListener", (uintptr_t)&retm1 },
  { "AImageReader_acquireNextImage", (uintptr_t)&retm1 },
  { "AImage_delete", (uintptr_t)&ret0 },
  { "AImage_getPlaneData", (uintptr_t)&retm1 },
  { "AImage_getPlanePixelStride", (uintptr_t)&retm1 },
  { "AImage_getPlaneRowStride", (uintptr_t)&retm1 },

  // --- EGL (mesa; the wrapper owns the context) ---
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },

  // --- GLES3 core (mesa), generated from the .so's UND list ---
#include "gl_imports.inc"

  // --- StS2.so (NativeAOT payload): symbols the engine's own imports
  // above don't cover. List captured empirically against a real build
  // (reference/nativeaot-probe/undefined_symbols.txt); see PORT_NOTES.md
  // "Ponte GodotSharp<->engine" for how StS2.so actually gets loaded
  // (dlopen_fake in egl_shim.c) -- this table just needs its OWN undefined
  // symbols resolved once so_util walks its relocations, same as any other
  // module. Implementations in nativeaot_shim.c unless noted otherwise.
  { "__libc_current_sigrtmin", (uintptr_t)&__libc_current_sigrtmin },
  { "__libc_current_sigrtmax", (uintptr_t)&__libc_current_sigrtmax },
  { "__sched_cpucount", (uintptr_t)&__sched_cpucount },
  { "__strcpy_chk", (uintptr_t)&sts2_strcpy_chk },
  { "__strcat_chk", (uintptr_t)&sts2_strcat_chk },
  { "asprintf", (uintptr_t)&asprintf },
  { "clock_nanosleep", (uintptr_t)&clock_nanosleep },
  { "dladdr", (uintptr_t)&dladdr },
  { "execv", (uintptr_t)&execv },
  { "flock", (uintptr_t)&flock },
  { "prctl", (uintptr_t)&prctl },
  { "pipe2", (uintptr_t)&pipe2 },
  { "geteuid", (uintptr_t)&geteuid },
  { "getpwuid_r", (uintptr_t)&getpwuid_r },
  { "getrusage", (uintptr_t)&getrusage },
  { "sysinfo", (uintptr_t)&sysinfo },
  { "getline", (uintptr_t)&getline },
  { "mprotect", (uintptr_t)&mprotect },
  { "mlock", (uintptr_t)&mlock },
  { "munlock", (uintptr_t)&munlock },
  { "mmap64", (uintptr_t)&mmap64_fake },
  { "stat64", (uintptr_t)&stat64_fake },
  { "fstat64", (uintptr_t)&fstat64_fake },
  { "lstat64", (uintptr_t)&lstat64_fake },
  { "lseek64", (uintptr_t)&lseek64_fake },
  { "pread", (uintptr_t)&pread_fake },
  { "pwrite", (uintptr_t)&pwrite_fake },
  { "sigaddset", (uintptr_t)&sigaddset },
  { "sigemptyset", (uintptr_t)&sigemptyset },
  { "signal", (uintptr_t)&signal },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask },
  { "pthread_kill", (uintptr_t)&pthread_kill },
  { "pthread_getattr_np", (uintptr_t)&pthread_getattr_np_fake }, // tagged attr; see above
  { "pthread_condattr_init", (uintptr_t)&sts2_condattr_init },
  { "pthread_condattr_destroy", (uintptr_t)&sts2_condattr_destroy },
  { "pthread_condattr_setclock", (uintptr_t)&sts2_condattr_setclock },
  // direct passthroughs to the real newlib functions -- same precedent as
  // pthread_attr_init/destroy/setstacksize a few hundred lines up (bionic's
  // pthread_attr_t is only ever touched through these calls, so as long as
  // they're ALL the real implementation consistently, ABI layout doesn't
  // matter). sched_getcpu/strtok_r/arc4random_buf are plain real newlib
  // functions with no bionic-shape concerns at all.
  { "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "sched_getcpu", (uintptr_t)&sched_getcpu },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "arc4random_buf", (uintptr_t)&sts2_arc4random_buf }, // NOT the real newlib one -- see nativeaot_shim.c

  // --- gaps found by tools/import_gap.py (docs/IMPORT_GAP.md) ---
  //
  // Every module's undefined list was diffed against this table; these four
  // were the only GLOBAL (non-weak) symbols left over. The weak ones -- the
  // _ZTH* thread-local inits and memfd_create -- are correctly left at 0 by
  // so_resolve, which the upstream code already handles.
  { "__assert2",   (uintptr_t)&sts2_assert2 },   // bionic; libfmod.so
  { "setpriority", (uintptr_t)&sts2_setpriority },// libfmod.so, libfmodstudio.so
  { "getpriority", (uintptr_t)&sts2_getpriority },// pairs with the above

  // Both reported as unresolved on the first hardware run, in modules that
  // only load late. import_gap.py had listed them and I judged them covered by
  // the link -- shutdown by libnx's BSD sockets, nan by newlib's libm. Neither
  // assumption held: the table is the only resolution path for a loaded
  // module, so "the toolchain provides it" is irrelevant unless it is HERE.
  { "shutdown", (uintptr_t)&shutdown },   // libfmod.so
  { "nan",      (uintptr_t)&nan },        // libspine_godot.so

  // --- the 26 libsts2.so needed on run 3 (see dotnet_imports.c) ---
  //
  // environ is a DATA symbol: the table holds the address OF the variable,
  // like __sF/stdin above, not its value.
  //
  // &environ, NOT a snapshot. newlib's setenv() reallocates the array, and the
  // wrapper calls setenv a dozen times during startup, so a captured pointer
  // dangles by the time managed code reads it. That was the crash in runs 3
  // through 10 -- see dotnet_imports.c.
  { "environ",        (uintptr_t)&environ },

  { "__memcpy_chk",   (uintptr_t)&sts2_memcpy_chk },
  { "__memset_chk",   (uintptr_t)&sts2_memset_chk },
  { "__strncpy_chk2", (uintptr_t)&sts2_strncpy_chk2 },
  { "getpagesize",    (uintptr_t)&sts2_getpagesize },
  { "timegm",         (uintptr_t)&sts2_timegm },
  { "uname",          (uintptr_t)&sts2_uname },
  { "_exit",          (uintptr_t)&sts2_exit_now },

  { "epoll_create1",  (uintptr_t)&sts2_epoll_create1 },
  { "epoll_ctl",      (uintptr_t)&sts2_epoll_ctl },
  { "epoll_wait",     (uintptr_t)&sts2_epoll_wait },
  { "recvmsg",        (uintptr_t)&sts2_recvmsg },
  { "sendmsg",        (uintptr_t)&sts2_sendmsg },
  { "getpeername",    (uintptr_t)&sts2_getpeername },
  { "msync",          (uintptr_t)&sts2_msync },
  { "futimens",       (uintptr_t)&sts2_futimens },

  { "sigfillset",     (uintptr_t)&sts2_sigfillset },
  { "sigdelset",      (uintptr_t)&sigdelset_fake },
  { "sigismember",    (uintptr_t)&sigismember_fake },
  { "setgid",         (uintptr_t)&sts2_setgid },
  { "setuid",         (uintptr_t)&sts2_setuid },
  { "getegid",        (uintptr_t)&sts2_getegid },
  { "setgroups",      (uintptr_t)&sts2_setgroups },
  { "getgroups",      (uintptr_t)&sts2_getgroups },
  { "getgrouplist",   (uintptr_t)&sts2_getgrouplist },
  { "getpwnam_r",     (uintptr_t)&sts2_getpwnam_r },
  { "execve",         (uintptr_t)&sts2_execve },
  { "waitid",         (uintptr_t)&sts2_waitid },
};

static const size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void sts2_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, (DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, 1);
}

// One-time registration of the FMOD/AAudio surfaces. Separate from
// sts2_resolve_imports because that runs once per module (engine, then each
// GDExtension) while this must run exactly once.
// game_mod is defined in main.c; declared here rather than in a header to keep
// the dependency one-way (imports.c already knows about the engine module).
extern so_module game_mod;

// game_mod IS finalized (main.c, right after resolve_entry_points), so
// so_try_find_addr_rx on it afterwards is a use-after-free -- mod->syms and
// mod->dynstrtab point into load_base, which finalize remaps. That is the same
// trap imports.h documents for dotnet_mod, and it presents as a Data Abort at
// a plausible-looking address rather than as a null.
//
// So symbols are cached BEFORE finalize. sts2_cache_engine_symbols() is called
// from resolve_entry_points(); sts2_engine_symbol() only reads the cache.
#define ENGINE_SYMCACHE_MAX 16
typedef struct { const char *name; uintptr_t addr; } EngineSym;

static EngineSym engine_symcache[ENGINE_SYMCACHE_MAX];
static int engine_symcache_n = 0;

// Names jni_helpers.c needs lazily. All three are in the exported dynsym list
// (docs/jni_surface_godot.txt); a zero here means the engine build differs
// from the one that was analysed.
static const char *engine_cached_names[] = {
  "Java_org_godotengine_godot_GodotLib_dialogCallback",
  "Java_org_godotengine_godot_GodotLib_inputDialogCallback",
  "Java_org_godotengine_godot_GodotLib_filePickerCallback",
  "Java_org_godotengine_godot_GodotLib_setVirtualKeyboardHeight",
  "Java_org_godotengine_godot_GodotLib_key",
  "Java_org_godotengine_godot_GodotLib_joybutton",
  "Java_org_godotengine_godot_GodotLib_joyaxis",
  "Java_org_godotengine_godot_GodotLib_joyconnectionchanged",
  // nx_input.c: every pointing device is funnelled through this, so a missing
  // entry here means all pointer input silently does nothing.
  "Java_org_godotengine_godot_GodotLib_dispatchMouseEvent",
  "Java_org_godotengine_godot_GodotLib_dispatchTouchEvent",
  "Java_org_godotengine_godot_GodotLib_magnify",
  "Java_org_godotengine_godot_GodotLib_pan",
  NULL
};

void sts2_cache_engine_symbols(void) {
  if (engine_symcache_n) return;
  for (int i = 0; engine_cached_names[i] && engine_symcache_n < ENGINE_SYMCACHE_MAX; i++) {
    uintptr_t a = so_try_find_addr_rx(&game_mod, engine_cached_names[i]);
    engine_symcache[engine_symcache_n].name = engine_cached_names[i];
    engine_symcache[engine_symcache_n].addr = a;
    engine_symcache_n++;
    // Not necessarily a fault. Confirmed against this engine build: it
    // exports filePickerCallback but NOT dialogCallback or
    // inputDialogCallback, so those two are expected to be absent and
    // jni_post_dialog_cancel simply has nothing to invoke for them. Logged at
    // a lower key so a genuinely surprising absence still stands out.
    if (!a) debugPrintf("[imports] engine symbol absent: %s\n", engine_cached_names[i]);
  }
}

uintptr_t sts2_engine_symbol(const char *name) {
  if (!name) return 0;
  for (int i = 0; i < engine_symcache_n; i++)
    if (!strcmp(engine_symcache[i].name, name)) return engine_symcache[i].addr;
  debugPrintf("[imports] sts2_engine_symbol(%s): not in cache -- add it to "
              "engine_cached_names, it cannot be resolved after so_finalize\n", name);
  return 0;
}

void sts2_register_audio_shims(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  dotnet_imports_init();   // environ must be live before libsts2.so resolves
  fmod_shim_register();
}

// generic import lookup for dlsym_fake (egl_shim routes unknown names here)
uintptr_t sts2_find_import(const char *name) {
  DynLibFunction *f = so_find_import((DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, name);
  if (f) return f->func;

  // AAudio lives in its own table so the whole surface can be switched off by
  // config.audio_backend without editing the main one.
  return aaudio_shim_find(name);
}
