/* vx.h: every 16-lane vector operation the kernels use, one name each, two bodies. The names and their meaning are those of MVEX-OpticalFLow's vx.h, where the first body was Knights Corner's MVEX; here it is the host's.
 *
 *   __AVX2__ and __FMA__ (and no VX_MODEL): the native body. A 16-lane vector is two 256-bit registers, lanes 0 .. 7 and 8 .. 15; every operation is its AVX2 or FMA3 instruction on each half, a fused multiply-add stays one
 *     rounding (vfmadd), and what MVEX did with mask registers is done with a 16-bit number expanded to lane masks.
 *   VX_MODEL, or a compiler without the two: a lane-by-lane model, fmaf for every fused multiply-add, one IEEE operation per lane. vxf is a plain struct.
 *
 * The same kernel source compiles to both, so the model's output is the reference for the native body's, every lane, and the gate (out/ofgate) holds them to it. The struct makes vector operators a compile error in both:
 * `a + b` on vxf does not build, so no kernel can leave the vocabulary.
 *
 * Alignment contract, kept from the card: a plain load or store is 64 B aligned unless the name says u (the native body needs 32, the planes give 64).
 * Assumes round-to-nearest-even. The agreement of the two bodies is gated per run, not assumed. */
#ifndef VT_VX_H
#define VT_VX_H

#include <stdint.h>

#define VX_LANES 16
#define VX_BYTES 64
#define VX_ALIGNED __attribute__((aligned(VX_BYTES)))

/* Compare predicates: the numbers are vcmpps's own (EQ_OQ, LT_OS, LE_OS). */
#define VX_CMP_EQ 0
#define VX_CMP_LT 1
#define VX_CMP_LE 2

/* Rounding to a whole value: 0 nearest (even), 1 down. */
#define VX_RND_NEAREST 0
#define VX_RND_DOWN 1

#if defined(__AVX2__) && defined(__FMA__) && !defined(VX_MODEL)

#include <immintrin.h>

#define VX_NATIVE 1
#define VX_INL static inline __attribute__((always_inline))

typedef struct { __m256 lo, hi; } vxf;
typedef struct { __m256i lo, hi; } vxidx;
typedef unsigned short vxk;

#define VX_ALL ((vxk)0xffff)

VX_INL vxf vx_of(__m256 lo, __m256 hi) { const vxf r = {lo, hi}; return r; }
VX_INL vxf vx_zero(void) { return vx_of(_mm256_setzero_ps(), _mm256_setzero_ps()); }
VX_INL vxf vx_load(const float *p) { return vx_of(_mm256_load_ps(p), _mm256_load_ps(p + 8)); }
VX_INL void vx_store(float *p, vxf v) { _mm256_store_ps(p, v.lo); _mm256_store_ps(p + 8, v.hi); }
/* The card wrote these lines with no read-for-ownership. Here a plain store: the next stage reads the line from this core's cache (VX_STREAM builds the streaming form, for measuring the two against each other). */
#if defined(VX_STREAM)
VX_INL void vx_store_nr(float *p, vxf v) { _mm256_stream_ps(p, v.lo); _mm256_stream_ps(p + 8, v.hi); }
#else
VX_INL void vx_store_nr(float *p, vxf v) { _mm256_store_ps(p, v.lo); _mm256_store_ps(p + 8, v.hi); }
#endif
/* 16 floats to 16 bytes, rounded to nearest even, saturated to 0 .. 255: the clamp first, so no lane can overflow the conversion. */
VX_INL void vx_store_u8(uint8_t *p, vxf v)
{
    const __m256 z = _mm256_setzero_ps(), top = _mm256_set1_ps(255.0f);
    const __m256i a = _mm256_cvtps_epi32(_mm256_min_ps(_mm256_max_ps(v.lo, z), top)), b = _mm256_cvtps_epi32(_mm256_min_ps(_mm256_max_ps(v.hi, z), top));
    /* packus works in 128-bit halves: a0-3 b0-3 | a4-7 b4-7; the quadwords back in order 0 2 1 3. */
    const __m256i w = _mm256_permute4x64_epi64(_mm256_packus_epi32(a, b), 0xD8);
    _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(_mm256_castsi256_si128(w), _mm256_extracti128_si256(w, 1)));
}
VX_INL vxf vx_bcast(const float *p) { const __m256 v = _mm256_broadcast_ss(p); return vx_of(v, v); }
VX_INL vxf vx_loadu(const float *p) { return vx_of(_mm256_loadu_ps(p), _mm256_loadu_ps(p + 8)); }
/* Lanes n .. n + 15 of a row held in memory: where the card slid two registers by valignd, the host loads from the address itself, which costs it nothing. */
VX_INL vxf vx_window(const float *p, int n) { return vx_loadu(p + n); }
/* 16 bytes to 16 floats, 0.0 .. 255.0, exact. */
VX_INL vxf vx_load_u8(const uint8_t *p)
{
    return vx_of(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)p))), _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(p + 8)))));
}
VX_INL vxf vx_add(vxf a, vxf b) { return vx_of(_mm256_add_ps(a.lo, b.lo), _mm256_add_ps(a.hi, b.hi)); }
VX_INL vxf vx_sub(vxf a, vxf b) { return vx_of(_mm256_sub_ps(a.lo, b.lo), _mm256_sub_ps(a.hi, b.hi)); }
VX_INL vxf vx_mul(vxf a, vxf b) { return vx_of(_mm256_mul_ps(a.lo, b.lo), _mm256_mul_ps(a.hi, b.hi)); }
/* a * b + c, one rounding: FMA3. */
VX_INL vxf vx_fma(vxf a, vxf b, vxf c) { return vx_of(_mm256_fmadd_ps(a.lo, b.lo, c.lo), _mm256_fmadd_ps(a.hi, b.hi, c.hi)); }
/* c - a * b, one rounding. */
VX_INL vxf vx_fnma(vxf a, vxf b, vxf c) { return vx_of(_mm256_fnmadd_ps(a.lo, b.lo, c.lo), _mm256_fnmadd_ps(a.hi, b.hi, c.hi)); }
VX_INL vxf vx_fma_s(vxf a, const float *s, vxf c) { const __m256 v = _mm256_broadcast_ss(s); return vx_of(_mm256_fmadd_ps(a.lo, v, c.lo), _mm256_fmadd_ps(a.hi, v, c.hi)); }
VX_INL vxf vx_mul_s(vxf a, const float *s) { const __m256 v = _mm256_broadcast_ss(s); return vx_of(_mm256_mul_ps(a.lo, v), _mm256_mul_ps(a.hi, v)); }
VX_INL vxf vx_min(vxf a, vxf b) { return vx_of(_mm256_min_ps(a.lo, b.lo), _mm256_min_ps(a.hi, b.hi)); }
VX_INL vxf vx_max(vxf a, vxf b) { return vx_of(_mm256_max_ps(a.lo, b.lo), _mm256_max_ps(a.hi, b.hi)); }
/* The sign bit cleared. The mask the card took from memory is a constant here. */
VX_INL vxf vx_abs(vxf a, const uint32_t *mask7f)
{
    (void)mask7f;
    const __m256 sign = _mm256_set1_ps(-0.0f);
    return vx_of(_mm256_andnot_ps(sign, a.lo), _mm256_andnot_ps(sign, a.hi));
}
/* Lanes m .. m + 7 of the 16 lanes y:x, m = 0 .. 7 a constant: vpalignr slides inside each 128-bit half, so the half that crosses over (x's upper, y's lower) is brought beside them first. 0, 1 or 2 instructions. */
#define VX_MID8(x, y) _mm256_permute2f128_ps((x), (y), 0x21)
#define VX_CAT8(x, y, m)                                                                                                                                              \
    ((m) == 0 ? (x)                                                                                                                                                   \
     : (m) == 4 ? VX_MID8((x), (y))                                                                                                                                   \
     : (m) < 4 ? _mm256_castsi256_ps(_mm256_alignr_epi8(_mm256_castps_si256(VX_MID8((x), (y))), _mm256_castps_si256(x), ((m) & 3) * 4))                              \
               : _mm256_castsi256_ps(_mm256_alignr_epi8(_mm256_castps_si256(y), _mm256_castps_si256(VX_MID8((x), (y))), ((m) & 3) * 4)))
/* The 32 lanes hi:lo, shifted down by n lanes, the low 16 kept. n = 0 .. 15, a compile-time constant. */
#define vx_alignr(H, L, n)                                                                                                                                           \
    __extension__({                                                                                                                                                  \
        const vxf vx_h_ = (H), vx_l_ = (L);                                                                                                                          \
        (n) < 8 ? vx_of(VX_CAT8(vx_l_.lo, vx_l_.hi, (n) & 7), VX_CAT8(vx_l_.hi, vx_h_.lo, (n) & 7)) : vx_of(VX_CAT8(vx_l_.hi, vx_h_.lo, (n) & 7), VX_CAT8(vx_h_.lo, vx_h_.hi, (n) & 7)); \
    })
/* Eight bits of a mask to eight lanes of all ones or none: what a mask register was. A constant mask folds to a constant. */
VX_INL __m256 vx_k8(unsigned k)
{
    const __m256i bit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    return _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32((int)(k & 0xffu)), bit), bit));
}
/* Lanes of k from b, the rest from a. */
VX_INL vxf vx_blend(vxk k, vxf a, vxf b) { return vx_of(_mm256_blendv_ps(a.lo, b.lo, vx_k8(k)), _mm256_blendv_ps(a.hi, b.hi, vx_k8((unsigned)k >> 8))); }
#define vx_cmp(a, b, pred)                                                                                                                                           \
    __extension__({                                                                                                                                                  \
        const vxf vx_a_ = (a), vx_b_ = (b);                                                                                                                          \
        (vxk)((unsigned)_mm256_movemask_ps(_mm256_cmp_ps(vx_a_.lo, vx_b_.lo, (pred))) | (unsigned)_mm256_movemask_ps(_mm256_cmp_ps(vx_a_.hi, vx_b_.hi, (pred))) << 8); \
    })
#define VX_ROUND_IMM(mode) ((mode) == VX_RND_DOWN ? (_MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC) : (_MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC))
#define vx_round(v, mode)                                                                                                                                            \
    __extension__({                                                                                                                                                  \
        const vxf vx_r_ = (v);                                                                                                                                       \
        vx_of(_mm256_round_ps(vx_r_.lo, VX_ROUND_IMM(mode)), _mm256_round_ps(vx_r_.hi, VX_ROUND_IMM(mode)));                                                         \
    })
/* The pairs i^1, i^2, i^4, i^8, in that order: the model's tree, so a sum that rounds, rounds the same. */
VX_INL float vx_hsum(vxf v)
{
    const __m256 a = _mm256_add_ps(v.lo, _mm256_movehdup_ps(v.lo)), b = _mm256_add_ps(v.hi, _mm256_movehdup_ps(v.hi));
    const __m256 c = _mm256_add_ps(a, _mm256_permute_ps(a, 0x4E)), d = _mm256_add_ps(b, _mm256_permute_ps(b, 0x4E));
    const __m128 e = _mm_add_ss(_mm256_castps256_ps128(c), _mm256_extractf128_ps(c, 1)), f = _mm_add_ss(_mm256_castps256_ps128(d), _mm256_extractf128_ps(d, 1));
    return _mm_cvtss_f32(_mm_add_ss(e, f));
}
VX_INL float vx_hmin(vxf v)
{
    const __m256 m = _mm256_min_ps(v.lo, v.hi);
    __m128 x = _mm_min_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
    x = _mm_min_ps(x, _mm_movehl_ps(x, x));
    return _mm_cvtss_f32(_mm_min_ss(x, _mm_movehdup_ps(x)));
}
VX_INL float vx_hmax(vxf v)
{
    const __m256 m = _mm256_max_ps(v.lo, v.hi);
    __m128 x = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
    x = _mm_max_ps(x, _mm_movehl_ps(x, x));
    return _mm_cvtss_f32(_mm_max_ss(x, _mm_movehdup_ps(x)));
}
/* An index table held in two registers, for a run of permutes. */
VX_INL vxidx vx_idx(const int32_t *idx) { const vxidx r = {_mm256_load_si256((const __m256i *)idx), _mm256_load_si256((const __m256i *)(idx + 8))}; return r; }
/* Eight lanes, each v[ix & 15]: vpermps reads 3 bits of an index, so both halves are permuted and bit 3, moved to the sign, picks between them. */
VX_INL __m256 vx_pick8(vxf v, __m256i ix)
{
    return _mm256_blendv_ps(_mm256_permutevar8x32_ps(v.lo, ix), _mm256_permutevar8x32_ps(v.hi, ix), _mm256_castsi256_ps(_mm256_slli_epi32(ix, 28)));
}
VX_INL vxf vx_permi(vxf v, vxidx ix) { return vx_of(vx_pick8(v, ix.lo), vx_pick8(v, ix.hi)); }
VX_INL vxf vx_perm(vxf v, const int32_t *idx) { return vx_permi(v, vx_idx(idx)); }
/* Lanes of k take v[ix[i]]; the rest keep dst. */
VX_INL vxf vx_perm_merge(vxf dst, vxk k, vxf v, vxidx ix) { return vx_blend(k, dst, vx_permi(v, ix)); }
/* Whole-valued lanes to int32. */
VX_INL vxidx vx_to_idx(vxf v) { const vxidx r = {_mm256_cvtps_epi32(v.lo), _mm256_cvtps_epi32(v.hi)}; return r; }
/* base[ix[lane]]: AVX2 has the gather the card lacked. */
VX_INL vxf vx_gather(const float *base, vxidx ix) { return vx_of(_mm256_i32gather_ps(base, ix.lo, 4), _mm256_i32gather_ps(base, ix.hi, 4)); }
VX_INL vxf vx_mul_u8(vxf a, const uint8_t *p) { return vx_mul(a, vx_load_u8(p)); }
/* The card's software prefetch: nothing here, this CPU's own prefetchers follow five rows. */
VX_INL void vx_prefetch0(const void *p) { (void)p; }
VX_INL void vx_prefetch1(const void *p) { (void)p; }
/* Division and square root by Newton from the bits, as on the card, so a kernel that used them stays bit-exact against the model. */
VX_INL vxf vx_bits_rsub(vxf x, const uint32_t *k)
{
    const __m256i c = _mm256_set1_epi32((int)*k);
    return vx_of(_mm256_castsi256_ps(_mm256_sub_epi32(c, _mm256_castps_si256(x.lo))), _mm256_castsi256_ps(_mm256_sub_epi32(c, _mm256_castps_si256(x.hi))));
}
VX_INL vxf vx_bits_shr1(vxf x) { return vx_of(_mm256_castsi256_ps(_mm256_srli_epi32(_mm256_castps_si256(x.lo), 1)), _mm256_castsi256_ps(_mm256_srli_epi32(_mm256_castps_si256(x.hi), 1))); }

#else /* the lane model */

#include <math.h>
#include <string.h>

typedef struct { float l[16]; } vxf;
typedef unsigned short vxk;

#define VX_ALL ((vxk)0xffff)
#define VX_LANE_LOOP for (int i = 0; i < 16; i++)

static inline vxf vx_zero(void) { vxf r; VX_LANE_LOOP r.l[i] = 0.0f; return r; }
static inline vxf vx_load(const float *p) { vxf r; memcpy(r.l, p, sizeof r.l); return r; }
static inline void vx_store(float *p, vxf v) { memcpy(p, v.l, sizeof v.l); }
static inline void vx_store_nr(float *p, vxf v) { memcpy(p, v.l, sizeof v.l); }
static inline void vx_store_u8(uint8_t *p, vxf v) { VX_LANE_LOOP { const float s = rintf(v.l[i]); p[i] = s <= 0.0f ? 0u : s >= 255.0f ? 255u : (uint8_t)s; } }
static inline vxf vx_bcast(const float *p) { vxf r; VX_LANE_LOOP r.l[i] = *p; return r; }
static inline vxf vx_loadu(const float *p) { return vx_load(p); }
static inline vxf vx_load_u8(const uint8_t *p) { vxf r; VX_LANE_LOOP r.l[i] = (float)p[i]; return r; }
static inline vxf vx_add(vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = a.l[i] + b.l[i]; return r; }
static inline vxf vx_sub(vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = a.l[i] - b.l[i]; return r; }
static inline vxf vx_mul(vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = a.l[i] * b.l[i]; return r; }
static inline vxf vx_fma(vxf a, vxf b, vxf c) { vxf r; VX_LANE_LOOP r.l[i] = fmaf(a.l[i], b.l[i], c.l[i]); return r; }
static inline vxf vx_fnma(vxf a, vxf b, vxf c) { vxf r; VX_LANE_LOOP r.l[i] = fmaf(-a.l[i], b.l[i], c.l[i]); return r; }
static inline vxf vx_fma_s(vxf a, const float *s, vxf c) { vxf r; VX_LANE_LOOP r.l[i] = fmaf(a.l[i], *s, c.l[i]); return r; }
static inline vxf vx_mul_s(vxf a, const float *s) { vxf r; VX_LANE_LOOP r.l[i] = a.l[i] * *s; return r; }
static inline vxf vx_min(vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = fminf(a.l[i], b.l[i]); return r; }
static inline vxf vx_max(vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = fmaxf(a.l[i], b.l[i]); return r; }
static inline vxf vx_abs(vxf a, const uint32_t *mask7f) { (void)mask7f; vxf r; VX_LANE_LOOP r.l[i] = fabsf(a.l[i]); return r; }
static inline vxf vx_alignr_model(vxf hi, vxf lo, int n)
{
    vxf r;
    VX_LANE_LOOP r.l[i] = i + n < 16 ? lo.l[i + n] : hi.l[i + n - 16];
    return r;
}
#define vx_alignr(hi, lo, n) vx_alignr_model((hi), (lo), (n))
static inline vxf vx_perm(vxf v, const int32_t *idx) { vxf r; VX_LANE_LOOP r.l[i] = v.l[idx[i] & 15]; return r; }
static inline vxf vx_blend(vxk k, vxf a, vxf b) { vxf r; VX_LANE_LOOP r.l[i] = ((k >> i) & 1) ? b.l[i] : a.l[i]; return r; }
static inline vxk vx_cmp_model(vxf a, vxf b, int pred)
{
    unsigned k = 0;
    VX_LANE_LOOP {
        const int t = pred == VX_CMP_EQ ? a.l[i] == b.l[i] : pred == VX_CMP_LT ? a.l[i] < b.l[i] : a.l[i] <= b.l[i];
        k |= (unsigned)t << i;
    }
    return (vxk)k;
}
#define vx_cmp(a, b, pred) vx_cmp_model((a), (b), (pred))
static inline vxf vx_round_model(vxf v, int mode)
{
    vxf r;
    VX_LANE_LOOP r.l[i] = mode == VX_RND_DOWN ? floorf(v.l[i]) : rintf(v.l[i]);
    return r;
}
#define vx_round(v, mode) vx_round_model((v), (mode))
static inline float vx_hsum(vxf v)
{
    for (int step = 1; step <= 8; step <<= 1) {
        vxf t;
        VX_LANE_LOOP t.l[i] = v.l[i] + v.l[i ^ step];
        v = t;
    }
    return v.l[0];
}
typedef struct { int32_t l[16]; } vxidx;
static inline vxidx vx_idx(const int32_t *idx) { vxidx r; memcpy(r.l, idx, sizeof r.l); return r; }
static inline vxf vx_permi(vxf v, vxidx ix) { vxf r; VX_LANE_LOOP r.l[i] = v.l[ix.l[i] & 15]; return r; }
static inline vxf vx_perm_merge(vxf dst, vxk k, vxf v, vxidx ix) { vxf r; VX_LANE_LOOP r.l[i] = ((k >> i) & 1) ? v.l[ix.l[i] & 15] : dst.l[i]; return r; }
static inline vxidx vx_to_idx(vxf v) { vxidx r; VX_LANE_LOOP r.l[i] = (int32_t)lrintf(v.l[i]); return r; }
static inline vxf vx_gather(const float *base, vxidx ix) { vxf r; VX_LANE_LOOP r.l[i] = base[ix.l[i]]; return r; }
static inline vxf vx_mul_u8(vxf a, const uint8_t *p) { vxf r; VX_LANE_LOOP r.l[i] = a.l[i] * (float)p[i]; return r; }
static inline float vx_hmin(vxf v)
{
    for (int step = 1; step <= 8; step <<= 1) {
        vxf t;
        VX_LANE_LOOP t.l[i] = fminf(v.l[i], v.l[i ^ step]);
        v = t;
    }
    return v.l[0];
}
static inline float vx_hmax(vxf v)
{
    for (int step = 1; step <= 8; step <<= 1) {
        vxf t;
        VX_LANE_LOOP t.l[i] = fmaxf(v.l[i], v.l[i ^ step]);
        v = t;
    }
    return v.l[0];
}
static inline void vx_prefetch0(const void *p) { (void)p; }
static inline void vx_prefetch1(const void *p) { (void)p; }
static inline vxf vx_bits_rsub(vxf x, const uint32_t *k) { vxf r; VX_LANE_LOOP { uint32_t b; memcpy(&b, &x.l[i], 4); b = *k - b; memcpy(&r.l[i], &b, 4); } return r; }
static inline vxf vx_bits_shr1(vxf x) { vxf r; VX_LANE_LOOP { uint32_t b; memcpy(&b, &x.l[i], 4); b >>= 1; memcpy(&r.l[i], &b, 4); } return r; }

/* Lanes n .. n + 15 of a row held in memory. */
static inline vxf vx_window(const float *p, int n) { vxf r; VX_LANE_LOOP r.l[i] = p[n + i]; return r; }

#endif

/* 1 / x and 1 / sqrt(x), x > 0: on both sides the same fused operations from the same bits. */
static inline vxf vx_rcp(vxf x, const float *one)
{
    static const uint32_t K VX_ALIGNED = 0x7EF311C7u;
    vxf y = vx_bits_rsub(x, &K);
    const vxf o = vx_bcast(one);
    for (int i = 0; i < 3; i++)
        y = vx_fma(y, vx_fnma(x, y, o), y);
    return y;
}
static inline vxf vx_sqrt(vxf x, const float *half, const float *three_halves)
{
    static const uint32_t K VX_ALIGNED = 0x5F375A86u;
    vxf y = vx_bits_rsub(vx_bits_shr1(x), &K);
    const vxf h = vx_bcast(half), t = vx_bcast(three_halves);
    const vxf hx = vx_mul(x, h);
    for (int i = 0; i < 3; i++)
        y = vx_mul(y, vx_fnma(hx, vx_mul(y, y), t));
    return vx_mul(x, y);
}

#endif
