/* nx_input.c -- routes nx_pointer's events into Godot's input path.
 *
 * nx_pointer.{c,h} is vendored UNCHANGED from the pvzultimate lineage so
 * upstream fixes can still be pulled in. Everything StS2-specific lives here.
 *
 * ===========================================================================
 * Why mouse events and not touch
 * ===========================================================================
 *
 * The inherited main.c fed the touchscreen straight to
 * GodotLib.dispatchTouchEvent. That is right for a game designed around
 * touch. StS2 is a mouse-driven card game, and the difference that matters is
 * HOVER: card tooltips, target highlighting and damage previews all fire on
 * mouse-over. A touchscreen has no hover, so touch semantics lose them --
 * the game is playable but reads as broken.
 *
 * So every pointing device is funnelled to dispatchMouseEvent instead, and
 * hover is synthesised (see below).
 *
 * ===========================================================================
 * Hover synthesis
 * ===========================================================================
 *
 * nxp_update() only pushes an event when a tap phase is active:
 *
 *     if      ( tap && !s_tap_prev) phase = NXP_DOWN;
 *     else if ( tap &&  s_tap_prev) phase = NXP_MOVE;
 *     else if (!tap &&  s_tap_prev) phase = NXP_UP;
 *     if (phase) push(...);
 *
 * Move the cursor with no button held and nothing is emitted. Correct for a
 * touch game; useless here.
 *
 * Rather than patch the vendored file, this reads nxp_cursor_pos() every frame
 * and emits ACTION_HOVER_MOVE when the position changed and no button is
 * down. The public API already exposes exactly what is needed, so the vendored
 * module stays byte-identical to upstream.
 *
 * ===========================================================================
 * Device arbitration
 * ===========================================================================
 *
 * nx_pointer takes the left stick and the A button for the cursor. main.c also
 * sends both to the game as joyaxis/joybutton. Without arbitration one stick
 * push both moves the cursor and drives the game's controller navigation, and
 * one A press both clicks and sends a gamepad A.
 *
 * So while the cursor is visible, this suppresses left-stick axes and the A /
 * ZL / ZR buttons from the gamepad path. Everything else -- d-pad, B/X/Y,
 * shoulders, right stick -- still reaches the game, so StS2's own controller
 * support (it is Steam Deck Verified, so it very likely has some) keeps
 * working alongside the cursor.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "app_paths.h"
#include "util.h"
#include "imports.h"
#include "jni_fake.h"
#include "nx_pointer.h"
#include "nx_input.h"

#if DEBUG_LOG
#define inLog(...) debugPrintf("[input] " __VA_ARGS__)
#else
#define inLog(...) do {} while (0)
#endif

// Android MotionEvent action constants, as Godot's Android input path expects.
#define AMOTION_DOWN        0
#define AMOTION_UP          1
#define AMOTION_MOVE        2
#define AMOTION_HOVER_MOVE  7
#define AMOTION_SCROLL      8

// MotionEvent button masks.
#define ABUTTON_PRIMARY     1
#define ABUTTON_SECONDARY   2
#define ABUTTON_TERTIARY    4

// GodotLib.dispatchMouseEvent, per platform/android/java_godot_lib_jni.cpp:
//
//   void dispatchMouseEvent(JNIEnv*, jclass, jint event_type, jint button_mask,
//                           jfloat x, jfloat y, jboolean double_click,
//                           jboolean source_mouse_relative,
//                           jfloat pressure, jfloat tilt_x, jfloat tilt_y)
//
// ASSUMPTION, and the likeliest thing here to be wrong: pressure/tilt_x/tilt_y
// were added in Godot 4.2 (godotengine/godot#80231). On an older engine this
// passes three arguments too many. Under AAPCS64 the extras land in unread
// float registers, so it is survivable -- but if clicks register at the wrong
// place or double_click never fires, drop the last three and rebuild.
typedef void (*godot_mouse_fn)(void *env, void *cls, int event_type,
                               int button_mask, float x, float y,
                               uint8_t double_click, uint8_t relative,
                               float pressure, float tilt_x, float tilt_y);

static godot_mouse_fn s_mouse = NULL;
static int  s_resolved = 0;

static float s_last_x = -1.0f, s_last_y = -1.0f;
static int   s_buttons = 0;          // current mask
static int   s_initialised = 0;

// Cursor art and the settings file live next to the NRO.
static char s_data_dir[288];

static void nx_input_log(const char *msg) {
#if DEBUG_LOG
  debugPrintf("%s", msg);
#else
  (void)msg;
#endif
}

// ---------------------------------------------------------------------------
// Locked file I/O for the pointer's settings.
//
// nx_pointer.h is explicit that so-loader ports must serialise newlib file
// calls: devkitPro's handle table is not thread-safe and the engine's workers
// hammer it. The port's own fopen_fake is NOT a fit -- it is unlocked, and it
// sandboxes paths, which would rewrite the absolute sdmc path the pointer
// wants. So a small dedicated pair.
//
// nxp_init() reads cursor.png before the engine spawns threads, so only the
// deferred settings save (3 s after the last change) actually races. One mutex
// covers both.
// ---------------------------------------------------------------------------

static Mutex s_file_lock;

static FILE *locked_fopen(const char *path, const char *mode) {
  mutexLock(&s_file_lock);
  FILE *f = fopen(path, mode);
  mutexUnlock(&s_file_lock);
  return f;
}

static int locked_fclose(FILE *f) {
  mutexLock(&s_file_lock);
  int r = fclose(f);
  mutexUnlock(&s_file_lock);
  return r;
}

// ---------------------------------------------------------------------------

static void resolve_mouse_fn(void) {
  if (s_resolved) return;
  s_resolved = 1;
  s_mouse = (godot_mouse_fn)sts2_engine_symbol(
      "Java_org_godotengine_godot_GodotLib_dispatchMouseEvent");
  if (!s_mouse)
    debugPrintf("[input] dispatchMouseEvent NOT FOUND -- pointer input will "
                "do nothing. Add it to engine_cached_names in imports.c if it "
                "is missing from the cache rather than from the engine.\n");
  else
    inLog("dispatchMouseEvent resolved at %p\n", (void *)s_mouse);
}

static void send_mouse(int action, int mask, float x, float y, int dbl) {
  if (!s_mouse) return;
  s_mouse(fake_env, jni_activity_class(), action, mask, x, y,
          (uint8_t)dbl, 0 /* not relative */, 1.0f, 0.0f, 0.0f);
}

void nx_input_init(void) {
  if (s_initialised) return;

  snprintf(s_data_dir, sizeof(s_data_dir), "%s",
           app_paths_is_romfs() ? config.save_root : config.data_root);

  NxpConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.screen_w    = screen_width;
  cfg.screen_h    = screen_height;
  cfg.panel_w     = 1280;          // the handheld touch panel, always
  cfg.panel_h     = 720;
  cfg.data_dir    = s_data_dir;
  cfg.cursor_id   = 8;
  cfg.max_touch_slots = 8;
  cfg.stick_speed = 18.0f;         // a card game wants a brisk cursor; the
                                   // user can retune with d-pad up/down and it
                                   // persists to pointer.cfg
  cfg.mouse_sens  = 1.0f;
  cfg.log         = nx_input_log;
  cfg.fopen_fn    = locked_fopen;
  cfg.fclose_fn   = locked_fclose;

  // virtual_cursor 0 skips the pointer layer ENTIRELY -- no init, no draw, no
  // synthesised mouse events. The gamepad path in main.c's poll_input() is
  // separate and unaffected, so the game gets clean controller input.
  //
  // Gating here rather than inside nx_pointer.c on purpose: that file is
  // vendored unchanged from the pvzultimate lineage and md5-verified, and a
  // local edit would have to be re-applied every time it is refreshed
  // upstream.
  //
  // Note nx_pointer also arbitrates A / ZL / ZR / left-stick while the cursor
  // is visible. With it off those go straight to the gamepad handler, which is
  // what a controller-only player wants.
  if (!config.virtual_cursor) {
    s_initialised = 0;
    inLog("pointer DISABLED (virtual_cursor 0); controller input only\n");
    return;
  }

  nxp_init(&cfg);
  s_initialised = 1;
  inLog("touch input initialised (%dx%d); no virtual cursor\n",
        screen_width, screen_height);
}

void nx_input_set_screen(int w, int h) {
  if (!s_initialised) return;
  nxp_set_screen(w, h);
}

int nx_input_cursor_visible(void) {
  return s_initialised && nxp_cursor_visible();
}

// ---------------------------------------------------------------------------
// per-frame
// ---------------------------------------------------------------------------

void nx_input_update(void) {
  if (!s_initialised) return;
  resolve_mouse_fn();

  nxp_update();

  NxpEvent ev[24];
  const int n = nxp_poll(ev, 24);

  int any_button_event = 0;

  for (int i = 0; i < n; i++) {
    switch (ev[i].phase) {
      case NXP_DOWN:
        s_buttons |= ABUTTON_PRIMARY;
        send_mouse(AMOTION_DOWN, s_buttons, ev[i].x, ev[i].y, 0);
        any_button_event = 1;
        break;

      case NXP_MOVE:
        // Drag: a move with the button still held.
        send_mouse(AMOTION_MOVE, s_buttons, ev[i].x, ev[i].y, 0);
        any_button_event = 1;
        break;

      case NXP_UP:
        send_mouse(AMOTION_UP, s_buttons, ev[i].x, ev[i].y, 0);
        s_buttons &= ~ABUTTON_PRIMARY;
        any_button_event = 1;
        break;

      default:
        break;
    }
    s_last_x = ev[i].x;
    s_last_y = ev[i].y;
  }

  // Hover. Only when the cursor is visible, nothing is held, and the position
  // actually moved -- an unconditional hover every frame would flood the
  // engine's input queue with duplicates and make Godot's own
  // mouse-motion-relative accumulate garbage.
  if (!any_button_event && s_buttons == 0 && nxp_cursor_visible()) {
    float x, y;
    nxp_cursor_pos(&x, &y);
    if (fabsf(x - s_last_x) > 0.5f || fabsf(y - s_last_y) > 0.5f) {
      send_mouse(AMOTION_HOVER_MOVE, 0, x, y, 0);
      s_last_x = x;
      s_last_y = y;
    }
  }
}

void nx_input_draw(void) {
  if (s_initialised) nxp_draw();
}

void nx_input_shutdown(void) {
  if (s_initialised) nxp_save_settings();
}

// ---------------------------------------------------------------------------
// Arbitration, consumed by main.c's poll_input().
//
// While the cursor is up, nx_pointer owns the left stick and A/ZL/ZR. Sending
// them to the game as well would mean one stick push both moves the cursor and
// drives controller navigation, and one A press both clicks and sends a
// gamepad A.
// ---------------------------------------------------------------------------

uint64_t nx_input_masked_buttons(void) {
  if (!nx_input_cursor_visible()) return 0;
  return HidNpadButton_A | HidNpadButton_ZL | HidNpadButton_ZR
       | HidNpadButton_Plus | HidNpadButton_Minus;
}

int nx_input_left_stick_masked(void) {
  return nx_input_cursor_visible();
}
