/* gpu_arena.h -- dedicated arena for libdrm_nouveau's GPU buffers.
 *
 * There is nothing to call to enable it: the linker's --wrap redirects
 * memalign/free (see the Makefile), and the arena claims page-aligned
 * allocations of 64 KB or more. `gpu_arena_mb 0` in config.txt disables it
 * entirely and restores plain newlib behaviour.
 *
 * See gpu_arena.c for why fragmentation, not exhaustion, is the problem.
 *
 * MIT license; see LICENSE. */

#ifndef __GPU_ARENA_H__
#define __GPU_ARENA_H__

// Live/peak usage and fallback count. Called periodically from the frame loop;
// the fallback count is the number that matters -- anything above zero means
// the arena was too small and newlib is being fragmented after all.
// Claim the pool early, before module loading churns the heap. Call once from
// main() after config is read. Idempotent. See gpu_arena.c.
void gpu_arena_preinit(void);

void gpu_arena_report(void);

// Canary coverage and findings. Prints nothing unless heap_canary is on. The
// "blocks checked" count is the honest measure of whether the coverage is
// meaningful -- see config.h.
void heap_canary_report(void);

// Bytes currently held by the pool. Used by the memory trend line.
size_t gpu_arena_live_bytes(void);

#endif
