// SPDX-License-Identifier: MIT
// Copyright 2026 Joshua Warren

/* Q6_K matrix times fp32 vector, for the tied output head of the staged Qwen decode.
 *
 * The head tensor is stored as Q6_K (6.5625 bits per weight). The host dequantizes it to fp32 and reads 2.03 GB every
 * generated step, which on the M2 is at the memory ceiling (95.6 GB/s). This kernel reads the Q6_K blocks (417 MB) and
 * dequantizes in registers, with an fp32 activation vector.
 *
 * Layout (ggml block_q6_K, 210 bytes per 256 weights): ql[128] low nibbles, qh[64] high 2 bits, scales[16] int8,
 * d fp16. A weight is d * scales[g] * (q - 32), q the 6-bit value. A row is K/256 consecutive blocks.
 *
 * q6k_dequant_row computes exactly that product ((d * scale) * q, fp32), so it equals the fp32 head in use today.
 * q6k_matvec_f32 factors the group scale out of the sum: per 16-weight group it accumulates q * x and multiplies the
 * group sum by d * scale. That is a different fp32 rounding order from dequantize-then-multiply and from a BLAS sgemv, so
 * logits differ from the fp32 head at fp32 rounding and are not bit-identical. The activations stay fp32 (no int8
 * activation quantization).
 *
 * Threads split the rows evenly; the caller pins them (tools/pcore_mask.py). Build: cc -O3 -pthread -shared -fPIC.
 * On aarch64 the NEON path is used unless Q6K_NO_NEON is defined; elsewhere a scalar path with double accumulation runs.
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && !defined(Q6K_NO_NEON)
#include <arm_neon.h>
#define Q6K_NEON 1
#else
#define Q6K_NEON 0
#endif

#define QK_K 256
#define BLOCK_BYTES 210

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {                       // subnormal half: normalize
            int shift = 0;
            while (!(man & 0x400u)) { man <<= 1; shift++; }
            man &= 0x3FFu;
            bits = sign | ((uint32_t)(127 - 15 + 1 - shift) << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    }
    float out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

int q6k_has_neon(void) { return Q6K_NEON; }

void q6k_dequant_row(const uint8_t *w, size_t k, float *y) {
    for (size_t b = 0; b < k / QK_K; b++) {
        const uint8_t *ql = w + b * BLOCK_BYTES;
        const uint8_t *qh = ql + 128;
        const int8_t *sc = (const int8_t *)(ql + 192);
        uint16_t dh;
        memcpy(&dh, ql + 208, sizeof dh);
        const float d = half_to_float(dh);
        float *out = y + b * QK_K;
        for (int half = 0; half < 2; half++) {
            for (int l = 0; l < 32; l++) {
                const int is = l / 16;
                const int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                const int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                const int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                const int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                out[l + 0] = d * (float)sc[is + 0] * (float)q1;
                out[l + 32] = d * (float)sc[is + 2] * (float)q2;
                out[l + 64] = d * (float)sc[is + 4] * (float)q3;
                out[l + 96] = d * (float)sc[is + 6] * (float)q4;
            }
            out += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
}

#if Q6K_NEON
static inline float32x4_t group16(uint8x16_t u, const float *x, float32x4_t acc, float dsc) {
    const int8x16_t q = vsubq_s8(vreinterpretq_s8_u8(u), vdupq_n_s8(32));
    const int16x8_t lo = vmovl_s8(vget_low_s8(q));
    const int16x8_t hi = vmovl_high_s8(q);
    const float32x4_t q0 = vcvtq_f32_s32(vmovl_s16(vget_low_s16(lo)));
    const float32x4_t q1 = vcvtq_f32_s32(vmovl_high_s16(lo));
    const float32x4_t q2 = vcvtq_f32_s32(vmovl_s16(vget_low_s16(hi)));
    const float32x4_t q3 = vcvtq_f32_s32(vmovl_high_s16(hi));
    float32x4_t s = vmulq_f32(q0, vld1q_f32(x));
    s = vfmaq_f32(s, q1, vld1q_f32(x + 4));
    s = vfmaq_f32(s, q2, vld1q_f32(x + 8));
    s = vfmaq_f32(s, q3, vld1q_f32(x + 12));
    return vfmaq_n_f32(acc, s, dsc);
}

static float row_dot(const uint8_t *w, size_t nb, const float *x) {
    float32x4_t acc = vdupq_n_f32(0.0f);
    const uint8x16_t m4 = vdupq_n_u8(0x0F), m3 = vdupq_n_u8(0x03);
    for (size_t b = 0; b < nb; b++) {
        const uint8_t *ql = w + b * BLOCK_BYTES;
        const uint8_t *qh = ql + 128;
        const int8_t *sc = (const int8_t *)(ql + 192);
        uint16_t dh;
        memcpy(&dh, ql + 208, sizeof dh);
        const float d = half_to_float(dh);
        const float *xb = x + b * QK_K;
        for (int half = 0; half < 2; half++) {
            const uint8x16_t ql0 = vld1q_u8(ql), ql1 = vld1q_u8(ql + 16), ql2 = vld1q_u8(ql + 32), ql3 = vld1q_u8(ql + 48);
            const uint8x16_t qh0 = vld1q_u8(qh), qh1 = vld1q_u8(qh + 16);
            // l = 0..15 (scales 0,2,4,6), then l = 16..31 (scales 1,3,5,7); positions l, l+32, l+64, l+96.
            const uint8x16_t a0 = vorrq_u8(vandq_u8(ql0, m4), vshlq_n_u8(vandq_u8(qh0, m3), 4));
            const uint8x16_t a1 = vorrq_u8(vandq_u8(ql2, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(qh0, 2), m3), 4));
            const uint8x16_t a2 = vorrq_u8(vshrq_n_u8(ql0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(qh0, 4), m3), 4));
            const uint8x16_t a3 = vorrq_u8(vshrq_n_u8(ql2, 4), vshlq_n_u8(vshrq_n_u8(qh0, 6), 4));
            const uint8x16_t b0 = vorrq_u8(vandq_u8(ql1, m4), vshlq_n_u8(vandq_u8(qh1, m3), 4));
            const uint8x16_t b1 = vorrq_u8(vandq_u8(ql3, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(qh1, 2), m3), 4));
            const uint8x16_t b2 = vorrq_u8(vshrq_n_u8(ql1, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(qh1, 4), m3), 4));
            const uint8x16_t b3 = vorrq_u8(vshrq_n_u8(ql3, 4), vshlq_n_u8(vshrq_n_u8(qh1, 6), 4));
            acc = group16(a0, xb + 0, acc, d * (float)sc[0]);
            acc = group16(a1, xb + 32, acc, d * (float)sc[2]);
            acc = group16(a2, xb + 64, acc, d * (float)sc[4]);
            acc = group16(a3, xb + 96, acc, d * (float)sc[6]);
            acc = group16(b0, xb + 16, acc, d * (float)sc[1]);
            acc = group16(b1, xb + 48, acc, d * (float)sc[3]);
            acc = group16(b2, xb + 80, acc, d * (float)sc[5]);
            acc = group16(b3, xb + 112, acc, d * (float)sc[7]);
            xb += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
    return vaddvq_f32(acc);
}
#else
// Scalar path for hosts without NEON: dequantize a block, accumulate in double.
static float row_dot(const uint8_t *w, size_t nb, const float *x) {
    float tmp[QK_K];
    double acc = 0.0;
    for (size_t b = 0; b < nb; b++) {
        q6k_dequant_row(w + b * BLOCK_BYTES, QK_K, tmp);
        for (int i = 0; i < QK_K; i++) acc += (double)tmp[i] * (double)x[b * QK_K + i];
    }
    return (float)acc;
}
#endif

struct job {
    const uint8_t *w;
    size_t row0, row1, nb;
    const float *x;
    float *y;
};

static void *run_job(void *arg) {
    const struct job *j = (const struct job *)arg;
    for (size_t r = j->row0; r < j->row1; r++) j->y[r] = row_dot(j->w + r * j->nb * BLOCK_BYTES, j->nb, j->x);
    return NULL;
}

// y[r] = sum_k W[r][k] * x[k] for r in [0, rows); W is rows x (k / 256) Q6_K blocks, k a multiple of 256.
void q6k_matvec_f32(const uint8_t *w, size_t rows, size_t k, const float *x, float *y, int nthreads) {
    const size_t nb = k / QK_K;
    if (nthreads < 1) nthreads = 1;
    if ((size_t)nthreads > rows) nthreads = (int)(rows ? rows : 1);
    struct job jobs[64];
    pthread_t threads[64];
    if (nthreads > 64) nthreads = 64;
    for (int t = 0; t < nthreads; t++) {
        jobs[t].w = w;
        jobs[t].nb = nb;
        jobs[t].x = x;
        jobs[t].y = y;
        jobs[t].row0 = rows * (size_t)t / (size_t)nthreads;
        jobs[t].row1 = rows * (size_t)(t + 1) / (size_t)nthreads;
    }
    for (int t = 1; t < nthreads; t++) pthread_create(&threads[t], NULL, run_job, &jobs[t]);
    run_job(&jobs[0]);
    for (int t = 1; t < nthreads; t++) pthread_join(threads[t], NULL);
}

#ifdef Q6K_TEST_MAIN
static void *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(*n ? *n : 1);
    if (fread(buf, 1, *n, f) != *n) { perror("read"); exit(2); }
    fclose(f);
    return buf;
}

// q6k_test blocks.bin rows k x.bin y.bin dq_row dq.bin nthreads
int main(int argc, char **argv) {
    if (argc != 9) { fprintf(stderr, "usage: %s blocks rows k x y dq_row dq nthreads\n", argv[0]); return 64; }
    size_t nbytes, nx;
    uint8_t *w = slurp(argv[1], &nbytes);
    const size_t rows = strtoull(argv[2], NULL, 10), k = strtoull(argv[3], NULL, 10);
    float *x = slurp(argv[4], &nx);
    if (nbytes != rows * (k / QK_K) * BLOCK_BYTES || nx != k * sizeof(float)) { fprintf(stderr, "size mismatch\n"); return 65; }
    float *y = malloc(rows * sizeof(float));
    q6k_matvec_f32(w, rows, k, x, y, atoi(argv[8]));
    FILE *f = fopen(argv[5], "wb");
    fwrite(y, sizeof(float), rows, f);
    fclose(f);
    const size_t r = strtoull(argv[6], NULL, 10);
    float *dq = malloc(k * sizeof(float));
    q6k_dequant_row(w + r * (k / QK_K) * BLOCK_BYTES, k, dq);
    f = fopen(argv[7], "wb");
    fwrite(dq, sizeof(float), k, f);
    fclose(f);
    printf("neon=%d\n", q6k_has_neon());
    return 0;
}
#endif
