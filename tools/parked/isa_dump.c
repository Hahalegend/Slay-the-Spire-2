/* isa_dump.c -- capture NAK's compiled Maxwell code at pipeline creation.
 *
 * WHY THIS FILE EXISTS
 * ====================
 * The GPU dies executing specific shader variants (idle-timeout with cbufs, an
 * MMU fault at 0xCF200000 without), and the fault is not data-dependent. The
 * remaining suspect is NAK's Maxwell codegen -- but the compiled ISA turned
 * out to exist NOWHERE on disk. Both the Mesa disk cache and the VkPipelineCache
 * blob on this fork store serialized NIR; NAK runs again at every pipeline
 * creation, and its output lives only in memory.
 *
 * (That finding also explains the per-boot compile stutter: "Loading cache"
 * was never loading machine code, only the IR in front of the backend.)
 *
 * So the ISA is captured where it is born. VK_KHR_pipeline_executable_properties
 * is implemented by this NVK (15 entry points in libvulkan.a), and with
 * pipelineExecutableInfo enabled plus CAPTURE_INTERNAL_REPRESENTATIONS on each
 * pipeline, the driver hands back its internal representations -- for NVK that
 * includes NAK's own shader listing. The variant that kills the channel is
 * CREATED successfully (only execution hangs), so its code reaches the SD card
 * before the crash.
 *
 * Wiring (vulkan_shim.c):
 *   vkCreateDevice            + feature struct chained, + extension enabled
 *   vkCreate*Pipelines        | CAPTURE bits OR'd into every create-info
 *   after each create         properties + internal representations -> files
 *
 * Everything is gated on config.dump_isa (default 0): a dump run writes tens
 * of MB and slows creation, so it is a diagnostic you choose, like push_sync.
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <vulkan/vulkan.h>

#include "isa_dump.h"
#include "util.h"
#include "config.h"

static PFN_vkGetPipelineExecutablePropertiesKHR              g_props;
static PFN_vkGetPipelineExecutableInternalRepresentationsKHR g_reps;
static uint32_t g_pipe_n;      // running index, ties files to creation order
static int      g_dir_ready;

void isa_dump_resolve(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
  if (!config.dump_isa) return;
  g_props = (PFN_vkGetPipelineExecutablePropertiesKHR)
      gdpa(dev, "vkGetPipelineExecutablePropertiesKHR");
  g_reps  = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)
      gdpa(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
  debugPrintf("[isa] executable-properties entry points: props=%d reps=%d\n",
              g_props != NULL, g_reps != NULL);
}

static FILE *isa_open(uint32_t pipe_idx, uint32_t exe_idx, const char *kind,
                      const char *ext) {
  char path[512];
  if (!g_dir_ready) {
    snprintf(path, sizeof(path), "%s/isa", config.save_root);
    mkdir(path, 0777);
    g_dir_ready = 1;
  }
  snprintf(path, sizeof(path), "%s/isa/p%04u_e%u_%s.%s",
           config.save_root, pipe_idx, exe_idx, kind, ext);
  return fopen(path, "wb");
}

void isa_dump_pipeline(VkDevice dev, VkPipeline pipe, const char *origin) {
  if (!config.dump_isa || !g_props || !g_reps || pipe == VK_NULL_HANDLE) return;

  const uint32_t idx = g_pipe_n++;

  VkPipelineInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
                           .pipeline = pipe };
  uint32_t n_exe = 0;
  if (g_props(dev, &pi, &n_exe, NULL) != VK_SUCCESS || n_exe == 0) return;
  if (n_exe > 8) n_exe = 8;

  VkPipelineExecutablePropertiesKHR props[8];
  memset(props, 0, sizeof(props));
  for (uint32_t i = 0; i < n_exe; i++)
    props[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
  if (g_props(dev, &pi, &n_exe, props) != VK_SUCCESS) return;

  for (uint32_t e = 0; e < n_exe; e++) {
    VkPipelineExecutableInfoKHR ei = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
      .pipeline = pipe, .executableIndex = e };

    uint32_t n_rep = 0;
    if (g_reps(dev, &ei, &n_rep, NULL) != VK_SUCCESS || n_rep == 0) continue;
    if (n_rep > 8) n_rep = 8;

    // Two-call with real payloads. The driver owns nothing here: we allocate
    // pData ourselves per the spec, sized by a NULL-pData query first.
    VkPipelineExecutableInternalRepresentationKHR reps[8];
    memset(reps, 0, sizeof(reps));
    for (uint32_t r = 0; r < n_rep; r++)
      reps[r].sType =
        VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
    if (g_reps(dev, &ei, &n_rep, reps) != VK_SUCCESS) continue;

    for (uint32_t r = 0; r < n_rep; r++) {
      if (!reps[r].dataSize) continue;
      reps[r].pData = malloc(reps[r].dataSize);
      if (!reps[r].pData) { reps[r].dataSize = 0; }
    }
    if (g_reps(dev, &ei, &n_rep, reps) != VK_SUCCESS) {
      for (uint32_t r = 0; r < n_rep; r++) free(reps[r].pData);
      continue;
    }

    for (uint32_t r = 0; r < n_rep; r++) {
      if (!reps[r].pData || !reps[r].dataSize) { free(reps[r].pData); continue; }
      FILE *f = isa_open(idx, e, reps[r].name[0] ? reps[r].name : "rep",
                         reps[r].isText ? "txt" : "bin");
      if (f) {
        fprintf(f, "%s pipeline #%u  exe %u \"%s\" (%s)\n"
                   "rep \"%s\" %s, %zu bytes\n----\n",
                origin, idx, e, props[e].name, props[e].description,
                reps[r].name, reps[r].isText ? "text" : "binary",
                (size_t)reps[r].dataSize);
        fwrite(reps[r].pData, 1, reps[r].dataSize, f);
        fclose(f);
      }
      free(reps[r].pData);
    }
  }

  if ((idx & 31u) == 0)
    debugPrintf("[isa] dumped pipeline #%u (%s)\n", idx, origin);
}
