/* startup_status.h -- see startup_status.c.
 *
 * MUST be finished (startup_status_end) before main.c hands the default
 * NWindow to the renderer at main.c:1923. There is only one window.
 *
 * MIT license; see LICENSE. */
#ifndef STS2_STARTUP_STATUS_H
#define STS2_STARTUP_STATUS_H

void startup_status_begin(const char *title);
void startup_status_update(const char *title, const char *detail, int percent);
void startup_status_end(void);
int  startup_status_active(void);

#endif
