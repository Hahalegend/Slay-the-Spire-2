/* nativeaot_shim.h -- OS-level shims for the .NET NativeAOT runtime
 * (compiled by Godot's Android "linux-bionic" export, see
 * reference/nativeaot-probe/) on top of the generic libc_shim/godot_shim.
 * These are for the StS2.so payload specifically (not libgodot_android.so);
 * see PORT_NOTES.md "Ponte GodotSharp<->engine". MIT license; see LICENSE. */

#ifndef __NATIVEAOT_SHIM_H__
#define __NATIVEAOT_SHIM_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

// call once, right before so_util loads StS2.so (sets up TPIDR_EL0 TLS
// for the main thread + DOTNET_GCHeapHardLimit/gcConcurrent env vars).
// Calling it more than once is harmless (idempotent).
void nativeaot_shim_init(void);

// set to 1 by main.c right after "StS2.so loaded and initialized"; gates
// extra-verbose pthread_mutex_init/cond_init/create logging in imports.c so
// it only covers NativeAOT's own runtime bootstrap, not the engine's own
// (extremely frequent) prior mutex/cond usage during earlier boot stages.
extern int sts2_trace_all;

// call from the pthread_create shim for every NEW thread the engine/runtime
// spawns, from the child thread itself before running its real entry point:
// NativeAOT reads thread-statics via tpidr_el0 + offset, so every thread
// needs its own TLS block installed the same way as the main thread's.
void nativeaot_setup_thread_tls(void);

// the rest of nativeaot_shim.c: symbols StS2.so imports (see imports.c's
// table) that have no declaration in any newlib header (either because
// newlib doesn't implement them at all, or because bionic's ABI shape for
// them differs -- see the comments next to each implementation).
#undef sigaddset
int sigaddset(sigset_t *set, int sig);
#undef sigemptyset
int sigemptyset(sigset_t *set);
// spelled out instead of using sighandler_t: that typedef exists in newlib's
// <signal.h> under glibc naming, but only when _GNU_SOURCE is defined
// before signal.h's first include in the translation unit -- true for
// imports.c, not for every file that includes this header (e.g. main.c).
void (*signal(int signum, void (*handler)(int)))(int);
// signatures below match newlib's real pthread.h/sys/signal.h declarations
// exactly (needed just to avoid a "conflicting types" error at the call
// sites in imports.c, which includes both) even though the bodies (.c)
// treat the pointee as bionic's plain-int shape, not newlib's real struct --
// see the comment on sts2_condattr_init in nativeaot_shim.c. Named
// sts2_condattr_* (not plain pthread_condattr_*) specifically so they
// DON'T shadow libsysbase.a's real, working implementation of those names --
// imports.c's own cond-var creation code needs the real one.
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int pthread_kill(pthread_t thread, int sig);
int sts2_condattr_init(pthread_condattr_t *attr);
int sts2_condattr_destroy(pthread_condattr_t *attr);
int sts2_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);
int mprotect(void *addr, size_t len, int prot);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
int __libc_current_sigrtmin(void);
int __libc_current_sigrtmax(void);
int __sched_cpucount(size_t setsize, const void *set);
char *sts2_strcpy_chk(char *dest, const char *src, size_t destlen);
char *sts2_strcat_chk(char *dest, const char *src, size_t destlen);
int asprintf(char **strp, const char *fmt, ...);
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem);
int dladdr(const void *addr, void *info);
int execv(const char *path, char *const argv[]);
int flock(int fd, int operation);
int prctl(int option, ...);
int pipe2(int pipefd[2], int flags);
uid_t geteuid(void);
int getpwuid_r(uid_t uid, void *pwd, char *buf, size_t buflen, void **result);
int getrusage(int who, void *usage);
int sysinfo(void *info);
void sts2_arc4random_buf(void *buf, size_t n);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

// diagnostics: log-then-really-terminate substitutes for "abort"/"exit"
// (both real imports of StS2.so), and a write() that mirrors fd 1/2 into
// sts2_debug.log. See nativeaot_shim.c for why this replaces --wrap
// (which we can't use -- StS2.so is a prebuilt module, not something we
// link ourselves).
__attribute__((noreturn)) void sts2_abort_hook(void);
// binary-hooked directly onto RaiseFailFastException (main.c); see comment
// above its definition in nativeaot_shim.c for why this beats unwinding.
__attribute__((noreturn)) void sts2_raisefailfast_hook(void *rec, void *ctx, unsigned int code);

// binary-hooked onto GodotSharp's own
// ExceptionUtils.LogUnhandledException(Exception e) (main.c) -- the one
// place in the whole managed call graph that has a live reference to
// whatever Exception is actually killing startup. Does NOT call through to
// the original body (see nativeaot_shim.c comment): fine here since that
// function is pure reporting, not decision-making -- the real
// fail/continue choice is made by its caller either way.
void sts2_log_unhandled_exception_hook(void *exceptionObj);

// binary-hooked onto GC_Initialize (main.c) -- traced (via addr2line/nm/
// objdump on the real StS2.so, see PORT_NOTES.md) as the function whose
// E_OUTOFMEMORY (0x8007000E) return, from CreateGCHandleManager() or
// WKS::CreateGCHeap() returning NULL, is what ultimately trips RhFailFast
// in Thread::EnsureRuntimeInitialized. Unlike the other two hooks, this one
// DOES call through -- it's a hand-ported reimplementation of GC_Initialize's
// own (short, fully disassembled) logic that calls the same two real
// sub-functions instead of skipping them, so gameplay isn't affected if
// they actually succeed; only the return-value logging is new.
uint32_t sts2_gc_initialize_hook(void *clrToGC, void **ppHeap, void **ppHandleManager, void *gcDacVars);

// installed by overwriting the RELOCATED POINTER at WKS::GCHeap's vtable
// slot for Initialize() (main.c, before so_finalize -- a plain 8-byte data
// write, not a hook_arm64 code patch, since a vtable slot is just a data
// pointer already computed by so_load's R_AARCH64_RELATIVE relocations).
// Both CreateGCHandleManager()/WKS::CreateGCHeap() (sts2_gc_initialize_hook
// above) return non-NULL on hardware, so the actual GC_Initialize failure
// must be inside the (real, unmodified) IGCHeap::Initialize() virtual call
// that follows -- this wraps it with a real call-through + return-code log
// instead of reimplementing its ~1000-byte body by hand.
uint32_t sts2_gcheap_initialize_hook(void *thisPtr);
__attribute__((noreturn)) void sts2_exit_hook(int code);
ssize_t sts2_write_hook(int fd, const void *buf, size_t count);
char *sts2_getenv_hook(const char *name);

// Reports whether the GC ever tried to hijack a cooperative-mode thread.
// Call from the frame loop periodically, and at shutdown. See nativeaot_shim.c.
void sts2_gc_watch_report(void);

// Per-frame, silent unless the hijack count moved. See nativeaot_shim.c --
// the frame-8 report alone could never catch one during gameplay.
void sts2_gc_watch_poll(void);
int sts2_pthread_key_create_hook(pthread_key_t *key, void (*destructor)(void *));

#endif
