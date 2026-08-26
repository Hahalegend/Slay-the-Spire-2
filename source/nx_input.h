/* nx_input.h -- nx_pointer -> Godot mouse events. See nx_input.c.
 *
 * nx_pointer.{c,h} is vendored unchanged; this is the StS2-specific glue.
 * MIT license; see LICENSE. */

#ifndef __NX_INPUT_H__
#define __NX_INPUT_H__

#include <stdint.h>

// Call once, EARLY -- before the engine spawns threads. nxp_init() reads
// cursor.png off the SD card and that must not race the engine's workers.
void nx_input_init(void);

// Once per frame on the engine thread, from poll_input().
void nx_input_update(void);

// Draws the cursor. Must be called with the engine's GL context current, right
// before eglSwapBuffers, so it lands on top of the finished frame. Saves and
// restores all GL state itself.
void nx_input_draw(void);

// Docking swaps 1280x720 <-> 1920x1080. Cheap no-op when unchanged.
void nx_input_set_screen(int w, int h);

// Flushes the pointer's settings file.
void nx_input_shutdown(void);

int nx_input_cursor_visible(void);

// --- arbitration -------------------------------------------------------------
// While the cursor is up, nx_pointer owns these inputs; main.c's poll_input()
// must not also forward them to the game. See nx_input.c.

// Buttons to suppress from the joybutton path (0 when the cursor is hidden).
uint64_t nx_input_masked_buttons(void);

// 1 when the left stick is driving the cursor and must not also be sent as
// joyaxis LX/LY.
int nx_input_left_stick_masked(void);

#endif
