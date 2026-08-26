/* spirv_patch.h -- see spirv_patch.c for what this does and why.
 * MIT license; see LICENSE. */
#ifndef STS2_SPIRV_PATCH_H
#define STS2_SPIRV_PATCH_H

#include <stdint.h>
#include <stddef.h>

/* Rewrites, in place, the bound of every loop whose trip count is loaded from
 * a StorageBuffer/Uniform, pointing it at an existing OpConstant 0.
 * `code` must be a WRITABLE copy of the module. Returns the number of sites
 * patched; 0 means nothing matched and the module is untouched. */
uint32_t spirv_neutralize_ssbo_loop_bounds(uint32_t *code, size_t words);

#endif
