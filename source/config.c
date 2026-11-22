/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "app_paths.h"

// data_root is deliberately NOT persistable: it is discovered at runtime
// (app_paths.c) and a stale value in config.txt would survive a rename of the
// install directory and then point at nothing.
//
// save_root IS persistable, for the one case discovery cannot cover: keeping
// saves outside the app tree so they survive deleting and recopying it. Left
// unset it derives as <data_root>/save and follows a rename automatically.
/* Settled. Read if present, so an existing config.txt still works and a
 * manual override is still possible -- but NOT written, so a fresh
 * config.txt does not list them. The compiled defaults below are the
 * values these had after being tuned on hardware.
 *
 * They stay READABLE deliberately. Making them unknown keys would make
 * every existing config.txt emit a warning per line for settings that
 * are now simply correct. */
#define CONFIG_VARS_FIXED \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(boost); \
  CONFIG_VAR_INT(audio_backend); \
  CONFIG_VAR_INT(enable_fmod); \
  CONFIG_VAR_INT(enable_spine); \
  CONFIG_VAR_INT(use_cmdline_assemblies); \
  CONFIG_VAR_INT(eager_log); \
  CONFIG_VAR_INT(enable_vulkan); \
  CONFIG_VAR_INT(emu_vulkan); \
  CONFIG_VAR_INT(vk_verbose); \
  CONFIG_VAR_INT(astc_bake); \
  CONFIG_VAR_INT(astc_console); \
  CONFIG_VAR_INT(vk_validate); \
  CONFIG_VAR_INT(patch_loop_bounds); \
  CONFIG_VAR_INT(poison_free); \
  CONFIG_VAR_INT(original_modeldb); \
  CONFIG_VAR_INT(thread_stack_kb); \
  CONFIG_VAR_INT(gpu_arena_mb); \
  CONFIG_VAR_INT(heap_canary); \
  CONFIG_VAR_INT(no_hover_tips); \
  CONFIG_VAR_INT(mem_trend); \
  CONFIG_VAR_INT(gpu_arena_early); \
  CONFIG_VAR_INT(watchdog_stall_s); \
  CONFIG_VAR_INT(watchdog_arm_steps); \
  CONFIG_VAR_INT(virtual_cursor); \
  CONFIG_VAR_INT(trace_all); \
  CONFIG_VAR_INT(gpu_particles); \
  CONFIG_VAR_INT(astc_block); \
  CONFIG_VAR_INT(skip_compute); \
  CONFIG_VAR_INT(dispatch_log); \
  CONFIG_VAR_INT(free_guard); \
  CONFIG_VAR_INT(quarantine_mb); \
  CONFIG_VAR_INT(lock_trace); \
  CONFIG_VAR_INT(lock_trace_dump); \
  CONFIG_VAR_STR(rendering_method); \
  CONFIG_VAR_STR(nvk_debug); \
  CONFIG_VAR_INT(nvk_cmd_mem_uncached); \
  CONFIG_VAR_INT(nvk_mem_stream_uncached); \
  CONFIG_VAR_INT(nvk_mapped_completion); \
  CONFIG_VAR_INT(copy_guard); \
  CONFIG_VAR_INT(gc_region_range_mb); \
  CONFIG_VAR_STR(save_root);

/* Still worth changing per install, so still written out. */
/* The ONLY setting a generated config.txt contains.
 *
 * Everything else is either settled or a diagnostic, and both belong in
 * the binary rather than in front of a player. A config file listing
 * forty knobs invites changing one; a file with a single line says what
 * the file is for.
 *
 * The rest stay READABLE -- an existing config.txt keeps working and any
 * of them can still be set by hand for a diagnostic run. They are simply
 * never written back. */
#define CONFIG_VARS_WRITTEN \
  CONFIG_VAR_INT(resolution_height);

#define CONFIG_VARS CONFIG_VARS_WRITTEN CONFIG_VARS_FIXED

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  // Every CONFIG_VAR above returns on a match, so reaching here means the key
  // is not one we have.
  //
  // It used to be dropped in silence, which sent a hardware run after nothing:
  // `heap_trend 0` was set in place of `heap_canary 1`, and the log gave no
  // hint that the line had been ignored -- or that the real knob wants the
  // opposite value.
  debugPrintf("[config] UNKNOWN key '%s' (value '%s') -- ignored. "
              "Check the spelling against config.txt's comments.\n",
              name, value);
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config.screen_width = -1; // auto
  config.screen_height = -1;

  // StS2 fork defaults. These MUST come after the memset -- an earlier version
  // set them before it and had them zeroed straight back, which silently
  // disabled both FMOD and Spine (no audio, no characters) with nothing in the
  // log to say why.
  config.audio_backend = AUDIO_BACKEND_AUTO;
  config.enable_fmod = 1;
  config.enable_spine = 1;
  config.use_cmdline_assemblies = 0;
  config.eager_log = 0;
  config.enable_vulkan = 1;
  config.vk_verbose = 1;
  config.trace_all = 0;
  config.gpu_particles = 0;    // GPU particles hang NVK; see PARTICLES_DEVICE_LOSS.md
  config.astc_bake = 1;
  config.astc_block = 8;
  config.resolution_height = 0;  // 0 = follow the display; else 360..1080
  config.astc_console = 1;      // 1 = on-screen bar; see docs/ASTC_BAKE.md
  config.vk_validate = 0;       // descriptor/pipeline checking; see SHADER_SPIRV_ANALYSIS.md
  config.patch_loop_bounds = 1; // experiment; see SHADER_SPIRV_ANALYSIS.md
  config.skip_compute = 0;     // blunt mitigation; see config.h + PARTICLES_DEVICE_LOSS.md
  config.dispatch_log = 1;     // first 32 + every 512th; cheap and decisive
  config.free_guard = 1;
  config.poison_free = 1;    // hunting tool; see config.h
  config.quarantine_mb = 0;  // 0 = off; starves Mesa if set too high
  config.lock_trace = 1;        // recording is cheap; printing is gated
  config.lock_trace_dump = 64;  // events printed from the crash handler
  config.rendering_method[0] = 0;
  config.nvk_debug[0] = 0;          // empty = do not set NVK_DEBUG
  config.nvk_cmd_mem_uncached = 0;
  config.nvk_mem_stream_uncached = 0;
  config.nvk_mapped_completion = 0;
  config.copy_guard = 0;
  config.original_modeldb = 0;
  config.thread_stack_kb = 1024;
  config.gc_region_range_mb = 384;
  config.gpu_arena_mb = 0;
  config.heap_canary = 0;
  config.no_hover_tips = 0;
  config.mem_trend = 0;
  config.gpu_arena_early = 0;
  config.watchdog_stall_s = 0;
  config.watchdog_arm_steps = 0;   // 0 = WATCHDOG_ARM_STEPS (120); see config.h
  config.virtual_cursor = 0;

  // Snapshot, so the echo below can report what the file actually CHANGED.
  // Defaults are assigned inline above rather than by a function, so this is
  // taken here rather than recomputed -- recomputing would drift the moment a
  // default is edited in one place and not the other.
  Config defaults = config;

  // Discovered, not hardcoded: the app tree can live under any directory in
  // /switch/. See app_paths.c.
  app_paths_resolve(config.data_root, sizeof(config.data_root),
                    config.save_root, sizeof(config.save_root));

  char real_file[512];
  FILE *f = fopen(app_paths_real_path(file, real_file, sizeof(real_file)), "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // Echo everything that is NOT at its default.
  //
  // Every knob so far has been invisible unless something happened to log when
  // it acted on it -- `pointer DISABLED`, `gpua disabled`. A setting that is
  // absent, misspelled or silently unparsed looks exactly like a setting that
  // is present and working, and a whole hardware run was spent on that: the
  // hover-tip A/B produced no suppression and no way to tell whether the flag
  // was off or the patch was broken.
  //
  // Only non-defaults, so the line stays short and says something.
  {
    const Config *dp = &defaults;
    char line[512];
    int off = 0;
    #define CONFIG_VAR_INT(var) \
      if (config.var != dp->var && off < (int)sizeof(line) - 40) \
        off += snprintf(line + off, sizeof(line) - (size_t)off, "%s=%d ", #var, config.var)
    #define CONFIG_VAR_STR(var) /* paths are logged separately */
    CONFIG_VARS;
    #undef CONFIG_VAR_INT
    #undef CONFIG_VAR_STR
    debugPrintf("[config] non-default: %s\n", off ? line : "(none -- all defaults)");
  }

  // An overridden save_root is only useful if it exists; the engine's
  // DirAccess will not create it. mkdir on an existing directory is a
  // harmless EEXIST.
  char derived[288];
  snprintf(derived, sizeof(derived), "%s/%s", config.data_root, SAVE_SUBDIR);
  if (strcmp(config.save_root, derived) != 0) {
    debugPrintf("[config] save_root overridden: %s\n", config.save_root);
    mkdir(config.save_root, 0777);
  }

  return 0;
}

int read_config_overlay(const char *file) {
  char line[1024] = { 0 };
  char real_file[512];
  FILE *f = fopen(app_paths_real_path(file, real_file, sizeof(real_file)), "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue;
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);
  return 0;
}

int write_config(const char *file) {
  if (file && strncmp(file, "/romfs", 6) == 0)
    return 0; // embedded RomFS is read-only
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  // A DERIVED save_root must not be persisted. If it were, the first boot
  // would bake "<current data_root>/save" into config.txt, and renaming the
  // install directory afterwards would leave that value pointing at the old
  // folder -- saves apparently vanishing, with a config.txt that looks
  // deliberate. Only an explicit override, which by definition differs from
  // the derived value, is written back.
  char derived[288];
  snprintf(derived, sizeof(derived), "%s/%s", config.data_root, SAVE_SUBDIR);
  const int save_root_is_derived = (strcmp(config.save_root, derived) == 0);

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_STR(var) \
    if (config.var[0] && !(save_root_is_derived && !strcmp(#var, "save_root"))) \
      fprintf(f, "%s %s\n", #var, config.var)
  CONFIG_VARS_WRITTEN
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  fclose(f);
  return 0;
}
