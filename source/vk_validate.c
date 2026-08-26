/* vk_validate.c -- catch a bad descriptor before the GPU faults on it.
 *
 * WHY THESE THREE, AND NOT MORE
 * =============================
 * The SPIR-V analysis split the failures rather than unifying them:
 *
 *   ParticlesShaderRD  2 loops, bounds read from an SSBO   -> type=8  HANG
 *   CanvasShaderRD     ZERO loops, in all 36 variants      -> type=31 PAGE FAULT
 *
 * A shader with no loops cannot spin. So the canvas death is not a runaway
 * loop -- it is a read through a wrong address, and the fault record says
 * exactly which one: 0xCF200000.
 *
 * A wrong address in a shader with no loops comes from a descriptor. That is
 * what this file watches, and it watches it on the CPU side where the values
 * are still inspectable, before they are handed to the GPU.
 *
 *   1. buffer sizes        vkCreateBuffer          handle -> size
 *   2. descriptor writes   vkUpdateDescriptorSets  offset+range vs that size
 *   3. pipeline binds      vkCmdBindPipeline       ring of the last N
 *
 * (2) is the point. Godot binds buffer ranges constantly; if any write says
 * "offset 0, range 0xFFFFFFFF" into a 4 KB buffer, or an offset past the end,
 * the GPU reads memory nobody mapped and faults. Vulkan has no validation
 * layer on this platform, so nothing would otherwise notice.
 *
 * (3) exists because "which pipeline was running when it died" has been
 * inferred from Godot's own log lines for this entire investigation. The ring
 * makes it a fact.
 *
 * All gated on config.vk_validate (default 0).
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#include "vk_validate.h"
#include "util.h"
#include "config.h"
#include "spirv_patch.h"

/* ---- buffer size table -------------------------------------------------
 * Open-addressed, fixed capacity, never resized. A miss simply means "size
 * unknown", and an unknown size disables the range check for that write
 * rather than inventing a bound to compare against. */
#define BUFTAB_BITS 12
#define BUFTAB_SIZE (1u << BUFTAB_BITS)
#define BUFTAB_MASK (BUFTAB_SIZE - 1u)

static struct { uint64_t handle; uint64_t size; } s_buf[BUFTAB_SIZE];
static uint32_t s_buf_live, s_buf_evicted;

static inline uint32_t buf_hash(uint64_t h) {
  h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 29;
  return (uint32_t)h & BUFTAB_MASK;
}

static void buf_put(VkBuffer b, uint64_t size) {
  const uint64_t h = (uint64_t)b;
  uint32_t i = buf_hash(h);
  for (uint32_t n = 0; n < 64; n++, i = (i + 1) & BUFTAB_MASK) {
    if (s_buf[i].handle == 0 || s_buf[i].handle == h) {
      if (s_buf[i].handle == 0) s_buf_live++;
      s_buf[i].handle = h; s_buf[i].size = size;
      return;
    }
  }
  s_buf_evicted++;   /* table pressure is worth knowing about, not fatal */
}

static uint64_t buf_get(VkBuffer b) {
  const uint64_t h = (uint64_t)b;
  uint32_t i = buf_hash(h);
  for (uint32_t n = 0; n < 64; n++, i = (i + 1) & BUFTAB_MASK) {
    if (s_buf[i].handle == h) return s_buf[i].size;
    if (s_buf[i].handle == 0) return 0;
  }
  return 0;
}

void vk_validate_note_buffer(VkBuffer b, uint64_t size) {
  if (config.vk_validate) buf_put(b, size);
}

/* ---- descriptor write checking ---------------------------------------- */
static uint32_t s_writes, s_bad;

void vk_validate_descriptor_writes(uint32_t n, const VkWriteDescriptorSet *w) {
  if (!config.vk_validate || !w) return;

  for (uint32_t i = 0; i < n; i++) {
    if (!w[i].pBufferInfo) continue;
    const VkDescriptorType t = w[i].descriptorType;
    if (t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
        t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
        t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
      continue;

    for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
      const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[k];
      s_writes++;
      if (bi->buffer == VK_NULL_HANDLE) continue;

      const uint64_t size = buf_get(bi->buffer);
      if (!size) continue;                 /* unknown: do not guess a bound */

      const int off_past  = bi->offset > size;
      const int whole     = bi->range == VK_WHOLE_SIZE;
      const int end_past  = !whole && (bi->offset + bi->range > size);

      if (off_past || end_past) {
        if (s_bad < 32)
          debugPrintf("[vkval] BAD DESCRIPTOR: binding %u type %u -> buffer %p\n"
                      "[vkval]   offset %llu range %llu, but the buffer is %llu "
                      "bytes\n"
                      "[vkval]   the GPU would read %llu bytes past the end\n",
                      w[i].dstBinding, (unsigned)t, (void *)bi->buffer,
                      (unsigned long long)bi->offset,
                      (unsigned long long)bi->range,
                      (unsigned long long)size,
                      (unsigned long long)((bi->offset + (whole ? 0 : bi->range))
                                           - size));
        s_bad++;
      }
    }
  }
}

/* ---- pipeline bind ring ------------------------------------------------ */
#define RING 16
static struct { uint64_t pipe; uint32_t bind_point; uint32_t seq; } s_ring[RING];
static uint32_t s_ring_n;

void vk_validate_note_bind(VkPipelineBindPoint bp, VkPipeline p) {
  if (!config.vk_validate) return;
  const uint32_t i = s_ring_n % RING;
  s_ring[i].pipe = (uint64_t)p;
  s_ring[i].bind_point = (uint32_t)bp;
  s_ring[i].seq = s_ring_n;
  s_ring_n++;
}

void vk_validate_report(const char *why) {
  if (!config.vk_validate) return;

  debugPrintf("[vkval] === %s ===\n", why);
  debugPrintf("[vkval] buffers tracked %u (evicted %u), descriptor writes "
              "checked %u, BAD %u\n",
              s_buf_live, s_buf_evicted, s_writes, s_bad);

  // Zero writes and zero buffers is NOT a clean bill of health -- it means the
  // interception is not on the path the engine uses, and the check never ran.
  // Those two states printed identically for three rounds.
  if (!s_writes && !s_buf_live)
    debugPrintf("[vkval] NOTHING WAS SEEN. Not \"no problems found\" -- the\n"
                "[vkval] hooks are not on the path this engine resolves. Check\n"
                "[vkval] the [vk] GIPA list for how it asks.\n");
  else if (!s_bad)
    debugPrintf("[vkval] every descriptor checked was in bounds\n");

  if (!s_ring_n) { debugPrintf("[vkval] no pipeline binds recorded\n"); return; }

  /* Most recent first: the last one bound is the one that was running. */
  debugPrintf("[vkval] last pipeline binds, newest first:\n");
  const uint32_t count = s_ring_n < RING ? s_ring_n : RING;
  for (uint32_t k = 0; k < count; k++) {
    const uint32_t i = (s_ring_n - 1 - k) % RING;
    debugPrintf("[vkval]   #%u  %s  pipeline %p\n",
                s_ring[i].seq,
                s_ring[i].bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                  ? "COMPUTE " : "GRAPHICS",
                (void *)(uintptr_t)s_ring[i].pipe);
  }
}

/* ---- link-time interception ---------------------------------------------
 *
 * WHY --wrap AND NOT vkGetDeviceProcAddr
 *
 * The first version of this hooked vkGetDeviceProcAddr. On hardware it
 * reported, at the stall:
 *
 *     [vkval] buffers tracked 0, descriptor writes checked 0, BAD 0
 *     [vkval] no pipeline binds recorded
 *
 * and never printed its HOOKED line. Godot does not resolve these through
 * vkGetDeviceProcAddr -- Mesa is linked statically INTO the NRO, so the calls
 * bind directly to the library's symbols at link time and no proc-addr shim is
 * ever consulted.
 *
 * --wrap operates at that same link step, so it catches every call however it
 * was resolved. The four symbols are confirmed global text in libvulkan.a:
 *
 *     T vkCmdBindPipeline   T vkCreateBuffer
 *     T vkCreateShaderModule  T vkUpdateDescriptorSets
 *
 * This is the same mechanism already used for malloc/free, and for the same
 * reason: the code we want to observe is inside the NRO, not behind a loader.
 * ------------------------------------------------------------------------ */

extern VkResult __real_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *,
                                      const VkAllocationCallbacks *, VkBuffer *);
extern void __real_vkUpdateDescriptorSets(VkDevice, uint32_t,
                                          const VkWriteDescriptorSet *,
                                          uint32_t, const VkCopyDescriptorSet *);
extern void __real_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint,
                                     VkPipeline);
extern VkResult __real_vkCreateShaderModule(VkDevice,
                                            const VkShaderModuleCreateInfo *,
                                            const VkAllocationCallbacks *,
                                            VkShaderModule *);

static void vkval_announce(void) {
  static int done;
  if (done || !config.vk_validate) return;
  done = 1;
  debugPrintf("[vkval] link-time interception ACTIVE (--wrap)\n");
}

VkResult __wrap_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci,
                               const VkAllocationCallbacks *a, VkBuffer *out) {
  const VkResult r = __real_vkCreateBuffer(d, ci, a, out);
  if (r == VK_SUCCESS && ci && out) { vkval_announce(); vk_validate_note_buffer(*out, ci->size); }
  return r;
}

void __wrap_vkUpdateDescriptorSets(VkDevice d, uint32_t nw,
                                   const VkWriteDescriptorSet *w,
                                   uint32_t nc, const VkCopyDescriptorSet *c) {
  vkval_announce();
  vk_validate_descriptor_writes(nw, w);   /* check while the values are ours */
  __real_vkUpdateDescriptorSets(d, nw, w, nc, c);
}

void __wrap_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                              VkPipeline p) {
  vk_validate_note_bind(bp, p);
  __real_vkCmdBindPipeline(cb, bp, p);
}

VkResult __wrap_vkCreateShaderModule(VkDevice d,
                                     const VkShaderModuleCreateInfo *ci,
                                     const VkAllocationCallbacks *a,
                                     VkShaderModule *out) {
  if (!config.patch_loop_bounds || !ci || !ci->pCode || ci->codeSize < 24)
    return __real_vkCreateShaderModule(d, ci, a, out);

  uint32_t *copy = (uint32_t *)malloc(ci->codeSize);
  if (!copy) return __real_vkCreateShaderModule(d, ci, a, out);
  memcpy(copy, ci->pCode, ci->codeSize);

  static int announced;
  if (!announced) {
    announced = 1;
    debugPrintf("[spv] shader-module interception ACTIVE (--wrap), "
                "patch_loop_bounds=%d\n", config.patch_loop_bounds);
  }

  if (spirv_neutralize_ssbo_loop_bounds(copy, ci->codeSize / 4) == 0) {
    free(copy);
    return __real_vkCreateShaderModule(d, ci, a, out);
  }

  VkShaderModuleCreateInfo patched = *ci;
  patched.pCode = copy;
  const VkResult r = __real_vkCreateShaderModule(d, &patched, a, out);
  free(copy);           /* the driver copies the code during create */
  return r;
}
