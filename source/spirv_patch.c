/* spirv_patch.c -- neutralise SSBO-sourced loop bounds in flight.
 *
 * WHAT THIS DOES
 * ==============
 * Godot's particle process shader contains exactly two loops, in every one of
 * the 38 variants on the card:
 *
 *     for (uint i = 0; i < FRAME.attractor_count; i++)
 *     for (uint i = 0; i < FRAME.collider_count;  i++)
 *
 * The trip count is LOADED FROM A STORAGE BUFFER and the compare is UNSIGNED.
 * A wrong count is therefore not an error -- it is a four-billion-iteration
 * loop, which is a hung GPU. That fits both observed failures: with constant
 * buffers the channel idle-times out (type=8); with NVK_DEBUG=no_cbuf the same
 * walk reads out of bounds and faults (type=31 at 0xCF200000).
 *
 * sts2.dll references NO attractor or collider types at all, so both counts
 * SHOULD be zero and both loops SHOULD run zero iterations.
 *
 * This rewrites the second operand of each such compare to point at an
 * existing OpConstant 0, making the test `0 < 0` -- false, always.
 *
 * WHY THIS IS A CLEAN EXPERIMENT, NOT A BLIND FIX
 * ===============================================
 * If the counts really are zero, this changes NOTHING observable and the crash
 * persists -- which rules the loops out and points at codegen.
 * If the crash stops, the counts were garbage, and the bug is in what fills
 * that buffer (descriptors/upload), not in the shader and not in NAK.
 *
 * Either result eliminates half the remaining search space, which no config
 * flag tried so far has managed.
 *
 * WHY IT IS SAFE TO DO IN FLIGHT
 * ==============================
 * OpULessThan is 5 words: [wc|op][type][result][a][b]. Only `b` changes, so
 * the word count, every id, and every offset in the module are untouched. No
 * rebuild, no relocation, no size change. Verified on the real crash variant:
 * 4405 instructions before and after, ending at exactly 19125/19125 words.
 *
 * The transform refuses to fire unless ALL of the following hold, and logs
 * every site it touches:
 *   - the compare is the condition of the OpBranchConditional that belongs to
 *     an OpLoopMerge (not just any comparison in the module)
 *   - operand b is an OpLoad, of an OpAccessChain, based on an OpVariable
 *     whose storage class is StorageBuffer or Uniform
 *   - an OpConstant 0 already exists with the operand's exact result type
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "spirv_patch.h"
#include "util.h"

#define OP_VARIABLE            59
#define OP_CONSTANT            43
#define OP_LOAD                61
#define OP_ACCESS_CHAIN        65
#define OP_ULESSTHAN          176
#define OP_SLESSTHAN          177
#define OP_LOOP_MERGE         246
#define OP_BRANCH_CONDITIONAL 250

#define SC_UNIFORM         2
#define SC_STORAGE_BUFFER 12

#define SPIRV_MAGIC 0x07230203u
#define MAX_ID      65536u
#define SCAN_WINDOW 128u        /* instructions from LoopMerge to its branch */

/* Small side tables, indexed by SPIR-V id. Allocated by the caller's stack is
 * too much (4 x 256 KB), so these are static: shader creation is single
 * threaded in Godot's pipeline path, and this is a diagnostic build option. */
static uint32_t s_res_at[MAX_ID];    /* id -> word offset of defining inst */
static uint32_t s_load_ptr[MAX_ID];  /* OpLoad result -> pointer id        */
static uint32_t s_ac_base[MAX_ID];   /* OpAccessChain result -> base id    */
static uint32_t s_var_sc[MAX_ID];    /* OpVariable result -> storage class */
static uint32_t s_rtype[MAX_ID];     /* id -> result type id               */
static uint32_t s_zero_of[MAX_ID];   /* type id -> id of an OpConstant 0   */

uint32_t spirv_neutralize_ssbo_loop_bounds(uint32_t *code, size_t words) {
  if (!code || words < 6 || code[0] != SPIRV_MAGIC) return 0;

  memset(s_res_at, 0, sizeof(s_res_at));
  memset(s_load_ptr, 0, sizeof(s_load_ptr));
  memset(s_ac_base, 0, sizeof(s_ac_base));
  memset(s_var_sc, 0, sizeof(s_var_sc));
  memset(s_rtype, 0, sizeof(s_rtype));
  memset(s_zero_of, 0, sizeof(s_zero_of));

  /* ---- pass 1: definitions ---- */
  for (size_t i = 5; i < words; ) {
    const uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (wc == 0 || i + wc > words) return 0;      /* malformed: touch nothing */

    if (op == OP_LOAD && wc >= 4 && code[i+2] < MAX_ID) {
      s_rtype[code[i+2]] = code[i+1];
      s_load_ptr[code[i+2]] = code[i+3];
      s_res_at[code[i+2]] = (uint32_t)i;
    } else if (op == OP_ACCESS_CHAIN && wc >= 4 && code[i+2] < MAX_ID) {
      s_ac_base[code[i+2]] = code[i+3];
      s_res_at[code[i+2]] = (uint32_t)i;
    } else if (op == OP_VARIABLE && wc >= 4 && code[i+2] < MAX_ID) {
      s_var_sc[code[i+2]] = code[i+3];
    } else if (op == OP_CONSTANT && wc >= 4 && code[i+2] < MAX_ID) {
      s_rtype[code[i+2]] = code[i+1];
      s_res_at[code[i+2]] = (uint32_t)i;
      /* Only single-word constants; a 64-bit 0 is wc>=5 and not wanted here. */
      if (wc == 4 && code[i+3] == 0 && code[i+1] < MAX_ID && !s_zero_of[code[i+1]])
        s_zero_of[code[i+1]] = code[i+2];
    } else if ((op == OP_ULESSTHAN || op == OP_SLESSTHAN) && wc >= 5 &&
               code[i+2] < MAX_ID) {
      s_res_at[code[i+2]] = (uint32_t)i;
    }
    i += wc;
  }

  /* ---- pass 2: loop headers ---- */
  uint32_t patched = 0;
  for (size_t i = 5; i < words; ) {
    const uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (wc == 0 || i + wc > words) break;
    if (op != OP_LOOP_MERGE) { i += wc; continue; }

    size_t j = i; uint32_t steps = 0;
    while (j < words && steps < SCAN_WINDOW) {
      const uint32_t jwc = code[j] >> 16, jop = code[j] & 0xFFFF;
      if (jwc == 0 || j + jwc > words) break;

      if (jop == OP_BRANCH_CONDITIONAL && jwc >= 4) {
        const uint32_t cond = code[j+1];
        if (cond >= MAX_ID) break;
        const uint32_t ci = s_res_at[cond];
        if (!ci) break;

        const uint32_t cop = code[ci] & 0xFFFF;
        if (cop != OP_ULESSTHAN && cop != OP_SLESSTHAN) break;

        const uint32_t b = code[ci+4];
        if (b >= MAX_ID) break;
        const uint32_t ptr = s_load_ptr[b];              /* b from an OpLoad? */
        if (!ptr || ptr >= MAX_ID) break;
        const uint32_t base = s_ac_base[ptr];            /* ...of an AccessChain? */
        if (!base || base >= MAX_ID) break;
        const uint32_t sc = s_var_sc[base];              /* ...on a buffer? */
        if (sc != SC_STORAGE_BUFFER && sc != SC_UNIFORM) break;

        const uint32_t at = s_rtype[code[ci+3]];         /* type of operand a */
        if (!at || at >= MAX_ID) break;
        const uint32_t zero = s_zero_of[at];
        if (!zero) {
          debugPrintf("[spv] loop bound at word %u is buffer-sourced, but this\n"
                      "[spv] module has no OpConstant 0 of type %%%u -- left alone\n",
                      ci, at);
          break;
        }

        debugPrintf("[spv] loop bound neutralised: cmp@%u  %%%u (SSBO load) -> "
                    "%%%u (const 0)\n", ci, b, zero);
        code[ci+4] = zero;
        patched++;
        break;
      }
      j += jwc; steps++;
    }
    i += wc;
  }
  return patched;
}
