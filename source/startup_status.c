/* startup_status.c -- console progress overlay for work that happens before
 * the engine starts.
 *
 * Modelled on fruitninja_nx's error.c, which uses the libnx console for its
 * first-boot asset build. Same reasoning applies here and one constraint is
 * sharper:
 *
 *   THE CONSOLE AND THE RENDERER SHARE ONE WINDOW.
 *
 * There is exactly one default NWindow (nwindowGetDefault()). consoleInit()
 * takes it, and under Vulkan main() hands the same window to the swapchain at
 * `s_use_vulkan = config.enable_vulkan && vulkan_shim_probe()` (main.c:1923).
 * So everything here must finish, and startup_status_end() must run, BEFORE
 * that line. Fruit Ninja gets this for free because its console closes before
 * EGL setup; here it has to be deliberate.
 *
 * Every update is also written to the log, so a run that ends badly still
 * leaves the full trace in sts2_debug.log rather than only on a screen nobody
 * captured.
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/iosupport.h>

#include "startup_status.h"
#include "util.h"
#include "config.h"

static int  s_console_up;
static char s_last[192];

/* stdout's device, saved across the console.
 *
 * userAppInit calls initNxLink(), which points stdout at a SOCKET via
 * nxlinkStdio(). consoleInit then takes stdout for itself, and libnx's
 * consoleExit does not put it back -- it points stdout at the null device.
 *
 * So the console silently destroys nxlink output for the rest of the process.
 * Saving the pointer and restoring it is two lines and removes an interaction
 * nobody would look for. */
static const devoptab_t *s_saved_stdout;

static void draw(const char *title, const char *detail, int percent) {
  printf("\x1b[2J\x1b[H");                 /* clear + home */
  printf("\n\n  Slay the Spire 2 -- first boot\n");
  printf("  -----------------------------\n\n");
  printf("  %s\n\n", title ? title : "");

  if (percent >= 0) {
    if (percent > 100) percent = 100;
    char bar[41];
    const int fill = percent * 40 / 100;
    for (int i = 0; i < 40; i++) bar[i] = (i < fill) ? '#' : '-';
    bar[40] = 0;
    printf("  [%s] %3d%%\n\n", bar, percent);
  }

  /* The detail line is where the current file goes. Truncated rather than
   * wrapped: a 200-character res:// path would push the bar off screen, and
   * the tail of a path identifies it better than the head. */
  if (detail && *detail) {
    const size_t n = strlen(detail);
    printf("  %s\n\n", n > 60 ? detail + (n - 60) : detail);
  }

  printf("  This runs once. Please wait...\n");
  consoleUpdate(NULL);
}

void startup_status_begin(const char *title) {
  // Log-only mode still logs. The point of the switch is to remove the
  // console, not the progress reporting -- a bisect that also silences the
  // output tells you nothing about the run you just made.
  if (!config.astc_console) {
    debugPrintf("[status] %s (console disabled, astc_console 0)\n",
                title ? title : "");
    return;
  }
  if (!s_console_up) {
    s_saved_stdout = devoptab_list[STD_OUT];
    consoleInit(NULL);
    s_console_up = 1;
  }
  s_last[0] = 0;
  debugPrintf("[status] %s\n", title ? title : "(begin)");
  draw(title, NULL, -1);
}

void startup_status_update(const char *title, const char *detail, int percent) {
  if (!config.astc_console) {
    // Throttled, because at 15658 entries an unthrottled line per item buries
    // everything else in the boot log.
    static int last_pct = -1;
    if (percent != last_pct) {
      last_pct = percent;
      debugPrintf("[status] %s %d%%\n", title ? title : "", percent);
    }
    return;
  }
  if (!s_console_up) return;

  /* Log only when the line actually changes. At 3485 textures an unconditional
   * log line per item is 3485 lines of noise that push the interesting parts
   * of the boot out of a tail. */
  char one[192];
  snprintf(one, sizeof(one), "%s | %s | %d%%",
           title ? title : "", detail ? detail : "", percent);
  if (strcmp(one, s_last)) {
    strncpy(s_last, one, sizeof(s_last) - 1);
    s_last[sizeof(s_last) - 1] = 0;
    debugPrintf("[status] %s\n", one);
  }

  draw(title, detail, percent);
}

void startup_status_end(void) {
  if (!s_console_up) return;
  consoleExit(NULL);
  if (s_saved_stdout) devoptab_list[STD_OUT] = s_saved_stdout;
  s_console_up = 0;
  debugPrintf("[status] console released (the renderer can take the window)\n");
}

int startup_status_active(void) { return s_console_up; }
