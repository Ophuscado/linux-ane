// SPDX-License-Identifier: MIT
// Copyright 2026 Joshua Warren

/* Strided fp16 surface pack and unpack for the staged H13/H14 decodes.
 *
 * An ANE port is an (n, c, h, w) fp16 surface with byte strides (c * plane, plane, row, 2): the last axis is contiguous, rows are `row` bytes apart, channels `plane` bytes apart.
 * The lab decoders built a surface with numpy: zero a buffer of the port's size, assign the converted dense array through a strided view, copy the buffer to bytes, copy those bytes
 * into the prepared input buffer (pack); and copy the output buffer to bytes, view it and copy the strided view to a dense array (unpack). That is four passes over the port size per input and two per output.
 *
 * surface_pack_f32 / surface_pack_f16 write the dense array straight into a surface buffer whose padding is already zero (a prepared buffer is zero at creation and this function only ever writes the strided
 * positions, so the padding stays zero). surface_unpack copies the strided rows into a dense fp16 array. fp32 to fp16 conversion is round to nearest even, the same as numpy's astype(float16);
 * on aarch64 it uses the NEON FCVTN instruction, elsewhere the scalar _Float16 conversion of the compiler. No allocation, no threads, no writes outside the n*c*h rows of w fp16 values.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Byte offset of row (in, ic, ih) of a surface, or SIZE_MAX when the arithmetic overflows or the row of w fp16 values would end beyond `size`.
static inline size_t row_offset(size_t in, size_t ic, size_t ih, size_t c, size_t plane, size_t row, size_t w, size_t size) {
    size_t a, b, d, off;
    if (__builtin_mul_overflow(in * c + ic, plane, &a) || __builtin_mul_overflow(ih, row, &b) || __builtin_add_overflow(a, b, &off) || __builtin_mul_overflow(w, (size_t)2, &d)) return SIZE_MAX;
    if (off > size || d > size - off) return SIZE_MAX;
    return off;
}

static inline uint16_t f32_to_f16_bits(float x) {
    _Float16 h = (_Float16)x;
    uint16_t b;
    memcpy(&b, &h, sizeof b);
    return b;
}

// dense: n*c*h*w fp32 in row-major order. dst: the surface buffer. plane, row: byte strides of the c and h axes (n stride is c * plane).
// Returns 0, or -1 when a row would end beyond `dst_size` (nothing is written beyond it).
int surface_pack_f32(uint8_t *dst, size_t dst_size, const float *dense, size_t n, size_t c, size_t h, size_t w, size_t plane, size_t row) {
    for (size_t in = 0; in < n; in++)
        for (size_t ic = 0; ic < c; ic++)
            for (size_t ih = 0; ih < h; ih++) {
                const size_t off = row_offset(in, ic, ih, c, plane, row, w, dst_size);
                if (off == SIZE_MAX) return -1;
                const float *s = dense + ((in * c + ic) * h + ih) * w;
                uint16_t *d = (uint16_t *)(void *)(dst + off);
                size_t x = 0;
#if defined(__aarch64__)
                for (; x + 8 <= w; x += 8) {
                    const float16x8_t v = vcombine_f16(vcvt_f16_f32(vld1q_f32(s + x)), vcvt_f16_f32(vld1q_f32(s + x + 4)));
                    vst1q_u16(d + x, vreinterpretq_u16_f16(v));
                }
#endif
                for (; x < w; x++) d[x] = f32_to_f16_bits(s[x]);
            }
    return 0;
}

int surface_pack_f16(uint8_t *dst, size_t dst_size, const uint16_t *dense, size_t n, size_t c, size_t h, size_t w, size_t plane, size_t row) {
    for (size_t in = 0; in < n; in++)
        for (size_t ic = 0; ic < c; ic++)
            for (size_t ih = 0; ih < h; ih++) {
                const size_t off = row_offset(in, ic, ih, c, plane, row, w, dst_size);
                if (off == SIZE_MAX) return -1;
                memcpy(dst + off, dense + ((in * c + ic) * h + ih) * w, w * 2);
            }
    return 0;
}

// src: the surface buffer; dense: n*c*h*w fp16 out. Returns 0, or -1 when a row would end beyond `src_size`.
int surface_unpack(const uint8_t *src, size_t src_size, uint16_t *dense, size_t n, size_t c, size_t h, size_t w, size_t plane, size_t row) {
    for (size_t in = 0; in < n; in++)
        for (size_t ic = 0; ic < c; ic++)
            for (size_t ih = 0; ih < h; ih++) {
                const size_t off = row_offset(in, ic, ih, c, plane, row, w, src_size);
                if (off == SIZE_MAX) return -1;
                memcpy(dense + ((in * c + ic) * h + ih) * w, src + off, w * 2);
            }
    return 0;
}

int surface_pack_has_neon(void) {
#if defined(__aarch64__)
    return 1;
#else
    return 0;
#endif
}
