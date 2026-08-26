/* swkbd_shim.c -- GodotIO.showKeyboard backed by libnx swkbd.
 *
 * Why this is not a one-liner:
 *
 *   swkbdShow() runs the system keyboard as a LIBRARY APPLET. It blocks, and
 *   while it runs the calling process is suspended by the OS. Calling it from
 *   the engine thread would suspend the process mid-frame, with the EGL
 *   context current and a GL command stream possibly in flight.
 *
 *   The engine also does not want a return value. On Android, GodotEditText
 *   feeds the edited text back one character at a time through GodotLib.key(),
 *   and DisplayServer's virtual-keyboard callback fires off that. So the
 *   result has to be turned into key events, not handed back from the call.
 *
 * The flow, therefore:
 *
 *   engine thread   GodotIO.showKeyboard  -> queue a request, return at once
 *   main thread     swkbd_pump()          -> swkbdShow() blocks here; queues
 *                                            the result
 *   engine thread   swkbd_deliver()       -> diffs old vs new, emits
 *                                            backspaces then characters
 *
 * swkbd_pump() runs on the thread that owns appletMainLoop(), which is where
 * a library applet transition belongs. swkbd_deliver() runs from the frame
 * loop, on the engine thread, next to jni_drain_dialog_queue().
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "imports.h"
#include "jni_fake.h"
#include "swkbd_shim.h"

#if DEBUG_LOG
#define kbLog(...) debugPrintf("[swkbd] " __VA_ARGS__)
#else
#define kbLog(...) do {} while (0)
#endif

#define KBD_TEXT_MAX 1024

// Godot's VirtualKeyboardType, platform/android/java/.../GodotIO.java.
// Values are stable across 4.x; the mapping to swkbd presets is ours.
enum {
  GD_KEYBOARD_TYPE_DEFAULT = 0,
  GD_KEYBOARD_TYPE_MULTILINE,
  GD_KEYBOARD_TYPE_NUMBER,
  GD_KEYBOARD_TYPE_NUMBER_DECIMAL,
  GD_KEYBOARD_TYPE_PHONE,
  GD_KEYBOARD_TYPE_EMAIL_ADDRESS,
  GD_KEYBOARD_TYPE_PASSWORD,
  GD_KEYBOARD_TYPE_URL,
};

typedef struct {
  int  pending;
  int  type;
  int  max_len;
  char existing[KBD_TEXT_MAX];
} KbdRequest;

typedef struct {
  int  ready;
  int  cancelled;
  char before[KBD_TEXT_MAX];
  char after[KBD_TEXT_MAX];
} KbdResult;

static KbdRequest s_req;
static KbdResult  s_res;
static Mutex      s_lock;

// ---------------------------------------------------------------------------
// engine thread: request
// ---------------------------------------------------------------------------

void swkbd_request(const char *existing, int type, int max_len) {
  mutexLock(&s_lock);

  if (s_req.pending) {
    kbLog("request already pending, ignoring\n");
    mutexUnlock(&s_lock);
    return;
  }

  s_req.type    = type;
  s_req.max_len = (max_len > 0 && max_len < KBD_TEXT_MAX) ? max_len : KBD_TEXT_MAX - 1;
  strncpy(s_req.existing, existing ? existing : "", sizeof(s_req.existing) - 1);
  s_req.existing[sizeof(s_req.existing) - 1] = '\0';
  s_req.pending = 1;

  mutexUnlock(&s_lock);
  kbLog("queued: type %d, max %d, existing '%s'\n", type, max_len, s_req.existing);
}

int swkbd_is_pending(void) {
  mutexLock(&s_lock);
  int p = s_req.pending;
  mutexUnlock(&s_lock);
  return p;
}

// ---------------------------------------------------------------------------
// main thread: run the applet
// ---------------------------------------------------------------------------

static void configure_for_type(SwkbdConfig *cfg, int type) {
  switch (type) {
    case GD_KEYBOARD_TYPE_NUMBER:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      break;
    case GD_KEYBOARD_TYPE_NUMBER_DECIMAL:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_PHONE:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "+");
      break;
    case GD_KEYBOARD_TYPE_EMAIL_ADDRESS:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "@");
      swkbdConfigSetRightOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_PASSWORD:
      swkbdConfigMakePresetPassword(cfg);
      break;
    case GD_KEYBOARD_TYPE_URL:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "/");
      swkbdConfigSetRightOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_MULTILINE:
      // No true multiline preset; the default with a generous length is the
      // closest thing. Newlines typed into swkbd come back as '\n' and are
      // emitted as ENTER by the delivery path.
      swkbdConfigMakePresetDefault(cfg);
      break;
    default:
      swkbdConfigMakePresetDefault(cfg);
      break;
  }
}

void swkbd_pump(void) {
  KbdRequest req;

  mutexLock(&s_lock);
  if (!s_req.pending || s_res.ready) { mutexUnlock(&s_lock); return; }
  req = s_req;
  mutexUnlock(&s_lock);

  SwkbdConfig cfg;
  Result rc = swkbdCreate(&cfg, 0);
  if (R_FAILED(rc)) {
    kbLog("swkbdCreate failed: 0x%x\n", rc);
    mutexLock(&s_lock);
    s_req.pending = 0;
    s_res.ready = 1; s_res.cancelled = 1;
    mutexUnlock(&s_lock);
    return;
  }

  configure_for_type(&cfg, req.type);
  swkbdConfigSetInitialText(&cfg, req.existing);
  swkbdConfigSetStringLenMax(&cfg, (u32)req.max_len);
  swkbdConfigSetInitialCursorPos(&cfg, 1);   // cursor at end
  swkbdConfigSetBlurBackground(&cfg, 1);

  char out[KBD_TEXT_MAX];
  out[0] = '\0';

  kbLog("showing applet...\n");
  rc = swkbdShow(&cfg, out, sizeof(out));
  swkbdClose(&cfg);

  mutexLock(&s_lock);
  s_req.pending = 0;
  strncpy(s_res.before, req.existing, sizeof(s_res.before) - 1);
  s_res.before[sizeof(s_res.before) - 1] = '\0';

  if (R_SUCCEEDED(rc)) {
    strncpy(s_res.after, out, sizeof(s_res.after) - 1);
    s_res.after[sizeof(s_res.after) - 1] = '\0';
    s_res.cancelled = 0;
    kbLog("result: '%s'\n", s_res.after);
  } else {
    // User cancelled, or the applet failed. Either way the text is unchanged;
    // emitting nothing is correct.
    s_res.after[0] = '\0';
    s_res.cancelled = 1;
    kbLog("cancelled or failed: 0x%x\n", rc);
  }
  s_res.ready = 1;
  mutexUnlock(&s_lock);
}

// ---------------------------------------------------------------------------
// engine thread: turn the result into key events
// ---------------------------------------------------------------------------

// Godot's Key enum (core/os/keyboard.h). Only what the delivery path needs.
#define GD_KEY_BACKSPACE 0x04000004
#define GD_KEY_ENTER     0x04000005

// GodotLib.key. Signature per platform/android/java_godot_lib_jni.cpp:
//
//   void key(JNIEnv*, jclass, jint physical_keycode, jint unicode,
//            jint key_label, jboolean pressed, jboolean echo)
//
// ASSUMPTION, and the most likely thing in this file to be wrong: the
// key_label parameter was added in Godot 4.2 (godotengine/godot#70517). If the
// engine here predates that, this passes one argument too many -- which on
// AAPCS64 is harmless (the extra lands in an unread register) but means
// `pressed` and `echo` arrive in the wrong slots, so keys would repeat or
// never release. If typed characters come out doubled, this is why.
typedef void (*godot_key_fn)(void *env, void *cls, int physical, int unicode,
                             int label, uint8_t pressed, uint8_t echo);

static godot_key_fn s_key = NULL;
static void (*s_set_kbd_height)(void *env, void *cls, int height) = NULL;
static int s_resolved = 0;

static void resolve_key_fns(void) {
  if (s_resolved) return;
  s_resolved = 1;
#define G "Java_org_godotengine_godot_GodotLib_"
  s_key            = (godot_key_fn)sts2_engine_symbol(G "key");
  s_set_kbd_height = (void *)sts2_engine_symbol(G "setVirtualKeyboardHeight");
#undef G
  kbLog("key=%p setVirtualKeyboardHeight=%p\n", (void *)s_key, (void *)s_set_kbd_height);
}

// Minimal UTF-8 decode. swkbd returns UTF-8; GodotLib.key wants a codepoint.
static const char *utf8_next(const char *s, uint32_t *cp) {
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80)                { *cp = u[0]; return s + 1; }
  if ((u[0] & 0xE0) == 0xC0 && u[1]) {
    *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    return s + 2;
  }
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
    *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return s + 3;
  }
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
    *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
          ((uint32_t)(u[2] & 0x3F) << 6)  | (u[3] & 0x3F);
    return s + 4;
  }
  *cp = 0xFFFD;
  return s + 1;   // resync
}

static int utf8_len(const char *s) {
  int n = 0;
  uint32_t cp;
  while (*s) { s = utf8_next(s, &cp); n++; }
  return n;
}

static void emit_key(int physical, uint32_t unicode) {
  if (!s_key) return;
  void *cls = jni_activity_class();
  s_key(fake_env, cls, physical, (int)unicode, physical, 1, 0);  // press
  s_key(fake_env, cls, physical, (int)unicode, physical, 0, 0);  // release
}

void swkbd_deliver(void) {
  mutexLock(&s_lock);
  if (!s_res.ready) { mutexUnlock(&s_lock); return; }

  KbdResult res = s_res;
  s_res.ready = 0;
  mutexUnlock(&s_lock);

  resolve_key_fns();

  if (res.cancelled) {
    if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
    return;
  }

  // Find the common UTF-8 prefix, so an edit in the middle of a long string
  // does not retype the whole thing. Comparing by byte is safe: a shared byte
  // prefix of two valid UTF-8 strings ends on a character boundary unless it
  // splits a multi-byte sequence, which the resync below handles.
  size_t common = 0;
  while (res.before[common] && res.after[common] &&
         res.before[common] == res.after[common])
    common++;
  // back off to a character boundary
  while (common > 0 && ((unsigned char)res.before[common] & 0xC0) == 0x80)
    common--;

  int to_delete = utf8_len(res.before + common);
  kbLog("delivering: %d backspace(s), then '%s'\n", to_delete, res.after + common);

  for (int i = 0; i < to_delete; i++)
    emit_key(GD_KEY_BACKSPACE, 0);

  const char *p = res.after + common;
  uint32_t cp;
  while (*p) {
    p = utf8_next(p, &cp);
    if (cp == '\n' || cp == '\r') emit_key(GD_KEY_ENTER, 0);
    else                          emit_key(0, cp);
  }

  // swkbd is a fullscreen modal applet, not an inline keyboard, so it never
  // occludes part of the game view. Reporting 0 stops the engine shifting the
  // UI up to make room for a keyboard that is not there.
  if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
}

void swkbd_hide(void) {
  mutexLock(&s_lock);
  s_req.pending = 0;
  mutexUnlock(&s_lock);
  resolve_key_fns();
  if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
}
