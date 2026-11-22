/* app_paths.h -- discovers the app tree under /switch/. See app_paths.c.
 *
 * The port runs from ANY directory under /switch/ -- /switch/sts2_nx,
 * /switch/slaythespire2, /switch/sls2nx. Nothing downstream may assume a
 * particular name.
 *
 * MIT license; see LICENSE. */

#ifndef __APP_PATHS_H__
#define __APP_PATHS_H__

#include <stddef.h>

// All three resolve on first use and cache. Safe to call from userAppInit,
// before the log exists.
const char *app_paths_data_root(void);   // e.g. "/switch/slaythespire2"
const char *app_paths_save_root(void);   // "<data_root>/save", created if absent
const char *app_paths_log_file(void);    // "<data_root>/sts2_debug.log"

// Fills config.data_root / config.save_root.
void app_paths_resolve(char *data_root, size_t data_sz,
                       char *save_root, size_t save_sz);

// Discovery runs before the log is open, so its trace is buffered. Call once
// after logging is up. Without this a wrong data root is nearly undiagnosable:
// every later failure presents as a missing file with no clue why.
void app_paths_log_trace(void);

// 1 when running from embedded NSP RomFS (/romfs -> romfs:/), 0 for loose SD install.
int app_paths_is_romfs(void);

// Translates virtual POSIX "/romfs" / "/romfs/..." paths into newlib "romfs:/..."
// paths for direct fopen/stat/opendir calls; returns `path` unchanged otherwise.
const char *app_paths_real_path(const char *path, char *buf, size_t sz);

#endif
