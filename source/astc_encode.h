/* astc_encode.h -- see astc_encode.cpp. MIT license; see LICENSE. */
#ifndef STS2_ASTC_ENCODE_H
#define STS2_ASTC_ENCODE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int    astc_encode_init(unsigned block_x, unsigned block_y);
size_t astc_encode_size(unsigned w, unsigned h, unsigned bx, unsigned by);
int    astc_encode_rgba(const uint8_t *rgba, unsigned w, unsigned h,
                        uint8_t *out, size_t out_size);
void   astc_encode_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
