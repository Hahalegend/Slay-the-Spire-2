/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "jni_fake.h"
#include "app_paths.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle, buffered (see debugFlush)

// 1 until the frame loop is proven healthy; see debugPrintf. Starts on so the
// riskiest phase -- module load, GC init, managed startup -- is never lost to
// a Data Abort truncating the buffer.
static int s_eager_flush = 1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line (the engine logs thousands of lines).
void userAppInit(void) {
  initNxLink();
  // Discovered, not compile-time: the app tree can be any directory under
  // /switch/. app_paths resolves from argv[0] / cwd / a scan, and caches.
  s_log = fopen(app_paths_log_file(), "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  if (s_log) {
    fputs("== sts2 log open ==\n", s_log);
    fflush(s_log);
  }
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;

  if (s_log) {
    va_start(list, text);
    vfprintf(s_log, text, list);
    va_end(list);
    // Eager flush during BOOT ONLY.
    //
    // The note below is right that per-line flushing made asset loading slow,
    // and it is right that debugFlush() covers a clean abort/exit. What it
    // could not cover is exactly what has happened on all three hardware runs
    // so far: an unhandled Data Abort, where the buffered tail is lost and the
    // log stops mid-word at the most interesting moment.
    //
    // So the tradeoff is resolved by phase instead of globally. Flushing stays
    // on until main() has proven the frame loop is running (debugSetEagerFlush(0)
    // after a few steps), which covers module loading, GC init and everything
    // else that has failed so far, and costs nothing once the game is past it
    // -- asset loading happens after that point, not before.
    //
    // Set `eager_log 1` in config.txt to keep it on permanently.
    if (s_eager_flush) fflush(s_log);

    // NOT flushed per line anymore (03/ago): with the game actually
    // booting and running now, asset loading alone emits hundreds of
    // these (VERBOSE_IO's whole point), each one a synchronous SD-card
    // write -- confirmed as the dominant cause of "loading is slow" on
    // hardware. A real crash is still covered: debugFlush() (below) is
    // called from the abort/exit/RaiseFailFastException hooks
    // (nativeaot_shim.c) right before they actually terminate, and
    // userAppExit()'s fclose() flushes on any clean exit. Only an
    // unhandled hardware fault (Data Abort etc, which Atmosphère's own
    // crash report already covers with full register/stack state) can
    // lose the last buffered lines now.
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
#endif
  return 0;
}

void debugSetEagerFlush(int on) {
#if DEBUG_LOG
  /* INSIDE the guard, with the variable it writes.
   *
   * s_eager_flush is declared under #if DEBUG_LOG, and this assignment was
   * outside it -- so the file compiled only while logging was ON. Flipping
   * DEBUG_LOG to 0, which the comment above its definition explicitly tells
   * you to do once the game is playable, broke the build:
   *
   *     util.c:114: error: 's_eager_flush' undeclared
   *
   * A switch that cannot be switched is not a switch. */
  s_eager_flush = on ? 1 : 0;
  if (s_log) {
    fflush(s_log);
    fprintf(s_log, "== per-line log flushing %s ==\n", on ? "ON" : "OFF (boot complete)");
    fflush(s_log);
  }
#else
  (void)on;
#endif
}

void debugFlush(void) {
#if DEBUG_LOG
  if (s_log) fflush(s_log);
#endif
}

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

// ApmCpuBoostMode_FastLoad raises CPU to 1785 MHz but hard-throttles the
// Tegra X1 GPU down to 76.8 MHz, which starves Godot's Vulkan renderer during
// both boot and gameplay. Only activate when explicitly enabled (`boost 1`).
void cpu_boost(int on) {
  if (on && !config.boost)
    return;
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }


// ---------------------------------------------------------------------------
// Logging from managed code.
//
// STS2Switch.SwitchLog P/Invokes this so patched game code lands in the same
// log as the wrapper. Interleaving the two is what makes a boot failure
// readable -- a managed error next to the native call that caused it.
//
// Exported (not static) so the NativeAOT payload's "__Internal" DllImport
// resolves it: the payload is loaded into this process image, so the dynamic
// linker finds it without a real shared library.
// ---------------------------------------------------------------------------

void sts2_managed_log(int level, const char *message) {
  if (!message) return;
  debugPrintf("[managed%s] %s\n", level ? " ERROR" : "", message);
}

// ---------------------------------------------------------------------------
// Native side of patches/STS2Switch/LifecyclePatches.cs.
//
// sts2_set_game_background tells the wrapper the GAME has entered its own
// background mode. That is not the same as applet focus loss: the game can
// background itself (a modal, an alt-tab equivalent) while the console is
// still focused. main.c already pauses the mixer on focus loss; this is the
// other direction and the two are deliberately independent.
// ---------------------------------------------------------------------------

static volatile int s_game_background = 0;

void sts2_set_game_background(int background) {
  s_game_background = background ? 1 : 0;
}

int sts2_game_is_background(void) { return s_game_background; }

// NGame.Quit routes here rather than calling OS.Kill, so the frame loop can
// unwind normally: saves flush, GDExtensions get their teardown, and
// nx_input_shutdown writes pointer.cfg. Reuses jni_quit_requested, which
// main.c already polls, instead of adding a second quit path that could
// disagree with it.
void sts2_request_quit(void) {
  jni_quit_requested = 1;
}


// ---------------------------------------------------------------------------
// The wrapper's own load base.
//
// log_code_addr and the crash handler both need to turn a wrapper address into
// an offset usable with `addr2line -e sts2.elf`. The existing anchor is
//
//     ((uintptr_t)&main) & ~0xFFFFF
//
// which is main's address rounded down to 1 MB -- not the module base, so
// every "sts2+0x..." the watchdog has printed resolves to the wrong symbol.
// Run 11's hang dump showed the consequence: one thread's stack read as
// _eglFindDisplay, execute_cfa_program_generic and __ssvfscanf_r together,
// which is not a call chain any program has.
//
// hbloader maps the NRO with its header at the module base, and that header
// carries "NRO0" at offset 0x10. Scanning back a page at a time from a known
// code address finds it exactly. Reads are svcQueryMemory-guarded, because
// walking off the front of the mapping is otherwise a fault inside the very
// code meant to explain faults.
// ---------------------------------------------------------------------------

#define NRO_MAGIC 0x304F524E   /* "NRO0" */

uintptr_t sts2_wrapper_base(void) {
  static uintptr_t cached;
  if (cached) return cached;

  if (envIsNso()) {
    MemoryInfo mi;
    u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&sts2_wrapper_base))) {
      cached = (uintptr_t)mi.addr;
      return cached;
    }
  }

  uintptr_t p = ((uintptr_t)&sts2_wrapper_base) & ~0xFFFULL;
  for (int i = 0; i < 8192; i++, p -= 0x1000) {   // up to 32 MB back
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, p))) break;
    if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) break;
    if (*(const volatile uint32_t *)(p + 0x10) == NRO_MAGIC) {
      cached = p;
      return cached;
    }
  }

  // Fall back to the old anchor rather than returning nothing. It is wrong,
  // but it is wrong the way every previous log was wrong, which is better than
  // a zero that makes every address look like a module offset.
  cached = ((uintptr_t)&sts2_wrapper_base) & ~0xFFFFFULL;
  debugPrintf("[base] NRO header not found; falling back to the 1MB anchor "
              "(%p) -- wrapper offsets will NOT match sts2.elf\n", (void *)cached);
  return cached;
}
