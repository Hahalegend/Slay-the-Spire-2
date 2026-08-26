/* dotnet_imports.c -- the 26 imports libsts2.so needed on hardware run 3.
 *
 * The NativeAOT payload is the only module in this port that pulls a
 * substantial slice of POSIX: the .NET runtime's PAL targets Linux, and the
 * linux-bionic-arm64 RID does not narrow that much. None of these were in the
 * table, so so_resolve tainted all 26, and the run died on the first one
 * touched -- a Data Abort with fault address 0, which is what dereferencing a
 * poisoned `environ` looks like.
 *
 * Why import_gap.py did not catch them: it was run against the APK's modules
 * (engine, FMOD, Spine), which is everything that existed at the time.
 * libsts2.so is built here, so it was never in the scan. Running the tool over
 * a freshly built payload is a step the pipeline was missing.
 *
 * Grouped by how much each actually has to do, because "stub it" is the right
 * answer for most and the wrong answer for a few.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"      // fatal_error
#include "dotnet_imports.h"

// ---------------------------------------------------------------------------
// LOAD-BEARING -- these do real work and getting them wrong is a crash
// ---------------------------------------------------------------------------

// environ.
//
// A DATA symbol, not a function: the import table must hold the ADDRESS OF the
// variable, the same as __sF / stdin / stdout / stderr.
//
// ===========================================================================
// THIS WAS THE CRASH. Runs 3 through 10.
// ===========================================================================
//
// The first version of this file SNAPSHOTTED the pointer:
//
//     char **sts2_environ_ptr = NULL;
//     void dotnet_imports_init(void) { sts2_environ_ptr = environ; }
//     { "environ", (uintptr_t)&sts2_environ_ptr },
//
// newlib's setenv() REALLOCATES the environ array. The wrapper calls setenv a
// dozen times after that capture -- HOME, MESA_SHADER_CACHE_DIR,
// MESA_GLSL_CACHE_DIR, and every DOTNET_* variable -- so by the time managed
// code read it, the snapshot pointed at a freed array.
//
// .NET's managed environment enumeration then walked it and scanned each entry
// for '=', which is the loop the crash handler decoded at the fault:
//
//     ldr  x0, [x0]        ; array element
//     cbz  x0, done        ; NULL-terminated
//     mov  x21, x0         ; p = string
//   check:
//     ldrb w2, [x21]       ; <- FAULT, x21 == 0x40
//     cmp  w2, #0x3d       ; '='
//
// x21 == 0x40 was a garbage "string pointer" out of the stale array. For four
// rounds I read far == 0x40 next to x8 == 0 and reported "a field at offset
// 0x40 of a null object" -- an inference, never checked against the actual
// instruction. The base register was x21, the offset was zero, and the load
// was a byte. pvzultimate's handoff says exactly this: "disassemble the PC and
// confirm the instruction can actually fault the way the dump says."
//
// The fix is to stop snapshotting. Pointing the table at newlib's own environ
// means every read sees the current array, including after a realloc.
// ===========================================================================

// __memcpy_chk / __memset_chk / __strncpy_chk2.
//
// bionic's _FORTIFY_SOURCE variants. Clang emits these instead of the plain
// functions whenever it can compute a destination size, so they are not rare
// edge cases -- they are on hot paths all through the runtime. Unresolved,
// the first fortified memcpy in GC init is fatal.
//
// The check is a buffer-overflow guard. Performing it properly and calling
// fatal_error on violation is better than silently ignoring it: a real
// overflow here would otherwise corrupt the heap and fail somewhere unrelated.
void *sts2_memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen) {
  if (len > dstlen)
    fatal_error("__memcpy_chk overflow\n%zu into %zu", len, dstlen);
  return memcpy(dst, src, len);
}

void *sts2_memset_chk(void *dst, int c, size_t len, size_t dstlen) {
  if (len > dstlen)
    fatal_error("__memset_chk overflow\n%zu into %zu", len, dstlen);
  return memset(dst, c, len);
}

// bionic's two-size strncpy check: (dst, src, n, dstlen, srclen).
char *sts2_strncpy_chk2(char *dst, const char *src, size_t n,
                        size_t dstlen, size_t srclen) {
  (void)srclen;
  if (n > dstlen)
    fatal_error("__strncpy_chk2 overflow\n%zu into %zu", n, dstlen);
  return strncpy(dst, src, n);
}

// getpagesize. The runtime sizes several allocations off this, and 0 or a
// poisoned value would produce nonsense heap geometry rather than a clean
// failure. Horizon is 4 KB, same as everything else in this port assumes.
int sts2_getpagesize(void) { return 0x1000; }

// timegm -- UTC counterpart of mktime, used by DateTime parsing.
//
// Implemented rather than aliased because newlib's availability varies by
// build, and a link error here would cost a whole hardware cycle. The
// algorithm is the standard days-from-civil conversion; no timezone lookup, no
// DST, which is exactly what timegm means.
time_t sts2_timegm(struct tm *tm) {
  if (!tm) return (time_t)-1;

  int y = tm->tm_year + 1900;
  int m = tm->tm_mon + 1;
  const int d = tm->tm_mday;

  // shift March-based so leap day lands at the end of the year
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

  return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

// uname. .NET reports this through RuntimeInformation.OSDescription. Answering
// honestly is more useful than answering "Linux": if a crash report ever
// escapes, it should say what it actually ran on.
int sts2_uname(void *buf) {
  // struct utsname is 5 (or 6) fixed char[65] fields on bionic. Writing
  // through a local layout avoids depending on newlib exposing sys/utsname.h.
  struct { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; } *u = buf;
  if (!u) { errno = EFAULT; return -1; }
  memset(u, 0, sizeof(*u));
  strncpy(u->sysname,  "Horizon",   sizeof(u->sysname) - 1);
  strncpy(u->nodename, "switch",    sizeof(u->nodename) - 1);
  strncpy(u->release,  "1.0",       sizeof(u->release) - 1);
  strncpy(u->version,  "sts2_nx",   sizeof(u->version) - 1);
  strncpy(u->machine,  "aarch64",   sizeof(u->machine) - 1);
  return 0;
}

// _exit. Reached from the runtime's abort paths. Routing through fatal_error
// keeps the message on screen instead of the process vanishing, which is the
// difference between a report and a silent close.
void sts2_exit_now(int status) {
  fatal_error("libsts2.so called _exit(%d)\nThe .NET runtime terminated the process.", status);
}

// ---------------------------------------------------------------------------
// GRACEFUL FAILURES -- the runtime probes for these and handles absence
// ---------------------------------------------------------------------------

// epoll. .NET's SocketAsyncEngine uses it on Linux. There are no sockets to
// poll here, and returning -1/ENOSYS makes the engine fail construction rather
// than spin on a broken descriptor. Only reached if the game opens a socket at
// all, which on Switch it should not -- LAN multiplayer is not ported.
int sts2_epoll_create1(int flags) { (void)flags; errno = ENOSYS; return -1; }
int sts2_epoll_ctl(int e, int op, int fd, void *ev) {
  (void)e; (void)op; (void)fd; (void)ev; errno = ENOSYS; return -1;
}
int sts2_epoll_wait(int e, void *ev, int max, int timeout) {
  (void)e; (void)ev; (void)max; (void)timeout; errno = ENOSYS; return -1;
}

// Socket calls libnx's BSD layer may or may not export depending on how
// switch-libnx was built. Stubbed rather than aliased for the same reason as
// timegm: a link error costs a hardware cycle, and nothing in this port needs
// them to succeed.
long sts2_recvmsg(int fd, void *msg, int flags) {
  (void)fd; (void)msg; (void)flags; errno = ENOSYS; return -1;
}
long sts2_sendmsg(int fd, const void *msg, int flags) {
  (void)fd; (void)msg; (void)flags; errno = ENOSYS; return -1;
}
int sts2_getpeername(int fd, void *addr, void *len) {
  (void)fd; (void)addr; (void)len; errno = ENOTCONN; return -1;
}

// msync: nothing is file-backed, so there is never anything to flush. Success
// is the truthful answer -- the data the caller wants durable already is.
int sts2_msync(void *addr, size_t len, int flags) {
  (void)addr; (void)len; (void)flags; return 0;
}

// futimens: timestamps on the SD card are not something Horizon lets us set
// per-file this way. Reporting success keeps File.SetLastWriteTime from
// throwing on a save write.
int sts2_futimens(int fd, const void *times) { (void)fd; (void)times; return 0; }

// ---------------------------------------------------------------------------
// SINGLE-USER STUBS -- there is one user, no groups, and no process model
// ---------------------------------------------------------------------------

int sts2_sigfillset(void *set) {
  // FILLS. The first version zeroed it, with a comment reasoning that nothing
  // is delivered so an empty set is consistent -- but the name is the contract,
  // and a caller doing sigfillset() then sigismember() is asking "is this the
  // full set", to which "no" is a wrong answer rather than a conservative one.
  //
  // bionic's sigset_t is one 64-bit word on LP64, covering signals 1..64.
  if (set) *(uint64_t *)set = ~0ull;
  return 0;
}

int sts2_setgid(unsigned gid)  { (void)gid; return 0; }
int sts2_setuid(unsigned uid)  { (void)uid; return 0; }
unsigned sts2_getegid(void)    { return 0; }

int sts2_setgroups(size_t n, const unsigned *list) { (void)n; (void)list; return 0; }

int sts2_getgroups(int n, unsigned *list) {
  // POSIX: n == 0 means "how many are there". One group, id 0.
  if (n == 0) return 1;
  if (n < 1 || !list) { errno = EINVAL; return -1; }
  list[0] = 0;
  return 1;
}

int sts2_getgrouplist(const char *user, unsigned gid, unsigned *groups, int *ngroups) {
  (void)user;
  if (!ngroups) { errno = EINVAL; return -1; }
  if (*ngroups < 1 || !groups) { *ngroups = 1; return -1; }
  groups[0] = gid;
  *ngroups = 1;
  return 1;
}

int sts2_getpwnam_r(const char *name, void *pwd, char *buf, size_t buflen, void **result) {
  (void)name; (void)pwd; (void)buf; (void)buflen;
  // "No such user" is the well-defined not-found result: return 0 with a null
  // result pointer. Returning an error code instead makes callers log a
  // failure for what is simply an empty user database.
  if (result) *result = NULL;
  return 0;
}

// execve / waitid: there is no process model. .NET reaches these only through
// System.Diagnostics.Process, which nothing in a card game should touch.
int sts2_execve(const char *path, char *const argv[], char *const envp[]) {
  (void)path; (void)argv; (void)envp;
  debugPrintf("[dotnet] execve() refused -- no process model\n");
  errno = ENOSYS;
  return -1;
}

int sts2_waitid(int idtype, unsigned id, void *infop, int options) {
  (void)idtype; (void)id; (void)infop; (void)options;
  errno = ECHILD;
  return -1;
}

// ---------------------------------------------------------------------------

void dotnet_imports_init(void) {
  extern char **environ;

  // Diagnostic only -- nothing reads this. The table points at &environ
  // itself (imports.c), so the value below is a snapshot for the log and is
  // expected to change as setenv reallocates.
  int n = 0;
  if (environ) for (char **e = environ; *e && n < 512; e++) n++;
  debugPrintf("[dotnet] import shims ready; environ=%p, %d entries "
              "(the import resolves to &environ, not this value)\n",
              (void *)environ, n);
}
