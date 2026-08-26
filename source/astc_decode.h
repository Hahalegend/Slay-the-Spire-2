/* astc_decode.h -- see astc_decode.c. MIT license; see LICENSE. */
#ifndef STS2_ASTC_DECODE_H
#define STS2_ASTC_DECODE_H
#include <stdint.h>
#include <stddef.h>
/* Returns a malloc'd w*h*4 RGBA8 buffer, or NULL with *why set to a reason
 * suitable for logging. Caller frees. */
uint8_t *astc_decode_to_rgba(const uint8_t *blob, size_t blob_len,
                             uint32_t data_format, uint32_t fmt,
                             unsigned w, unsigned h, const char **why);
#endif
