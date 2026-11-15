/* vxu.h: the integer half of the vocabulary, the partner of vx.h and in its form: one name, two bodies, AVX2 on the host, a lane model beside it. Packed BGRA pixels are handled here as what they are, 16 pixels to a
 * vector (two registers of 8), by byte-wise arithmetic on 32-bit lanes, as the card did it: the kernels are the card's, unchanged. */
#ifndef MF_VXU_H
#define MF_VXU_H

#include "vx.h"

#if defined(VX_NATIVE)

typedef struct { __m256i lo, hi; } vxu;

VX_INL vxu vxu_of(__m256i lo, __m256i hi) { const vxu r = {lo, hi}; return r; }
VX_INL vxu vxu_zero(void) { return vxu_of(_mm256_setzero_si256(), _mm256_setzero_si256()); }
VX_INL vxu vxu_load(const uint32_t *p) { return vxu_of(_mm256_load_si256((const __m256i *)p), _mm256_load_si256((const __m256i *)(p + 8))); }
VX_INL vxu vxu_loadu(const uint32_t *p) { return vxu_of(_mm256_loadu_si256((const __m256i *)p), _mm256_loadu_si256((const __m256i *)(p + 8))); }
VX_INL void vxu_store(uint32_t *p, vxu v) { _mm256_store_si256((__m256i *)p, v.lo); _mm256_store_si256((__m256i *)(p + 8), v.hi); }
#if defined(VX_STREAM)
VX_INL void vxu_store_nr(uint32_t *p, vxu v) { _mm256_stream_si256((__m256i *)p, v.lo); _mm256_stream_si256((__m256i *)(p + 8), v.hi); }
#else
VX_INL void vxu_store_nr(uint32_t *p, vxu v) { vxu_store(p, v); }
#endif
VX_INL vxu vxu_bcast(const uint32_t *p) { const __m256i v = _mm256_set1_epi32((int)*p); return vxu_of(v, v); }
VX_INL vxu vxu_and(vxu a, vxu b) { return vxu_of(_mm256_and_si256(a.lo, b.lo), _mm256_and_si256(a.hi, b.hi)); }
VX_INL vxu vxu_or(vxu a, vxu b) { return vxu_of(_mm256_or_si256(a.lo, b.lo), _mm256_or_si256(a.hi, b.hi)); }
VX_INL vxu vxu_xor(vxu a, vxu b) { return vxu_of(_mm256_xor_si256(a.lo, b.lo), _mm256_xor_si256(a.hi, b.hi)); }
VX_INL vxu vxu_add(vxu a, vxu b) { return vxu_of(_mm256_add_epi32(a.lo, b.lo), _mm256_add_epi32(a.hi, b.hi)); }
VX_INL vxu vxu_sub(vxu a, vxu b) { return vxu_of(_mm256_sub_epi32(a.lo, b.lo), _mm256_sub_epi32(a.hi, b.hi)); }
VX_INL vxu vxu_mull(vxu a, vxu b) { return vxu_of(_mm256_mullo_epi32(a.lo, b.lo), _mm256_mullo_epi32(a.hi, b.hi)); }
VX_INL vxu vxu_and_s(vxu a, const uint32_t *s) { return vxu_and(a, vxu_bcast(s)); }
VX_INL vxu vxu_add_s(vxu a, const uint32_t *s) { return vxu_add(a, vxu_bcast(s)); }
VX_INL vxu vxu_srl_(vxu v, int n) { return vxu_of(_mm256_srli_epi32(v.lo, n), _mm256_srli_epi32(v.hi, n)); }
VX_INL vxu vxu_sra_(vxu v, int n) { return vxu_of(_mm256_srai_epi32(v.lo, n), _mm256_srai_epi32(v.hi, n)); }
VX_INL vxu vxu_sll_(vxu v, int n) { return vxu_of(_mm256_slli_epi32(v.lo, n), _mm256_slli_epi32(v.hi, n)); }
#define vxu_srl(v, n) vxu_srl_((v), (n))
#define vxu_sra(v, n) vxu_sra_((v), (n))
#define vxu_sll(v, n) vxu_sll_((v), (n))
VX_INL vxu vxu_min_s(vxu a, vxu b) { return vxu_of(_mm256_min_epi32(a.lo, b.lo), _mm256_min_epi32(a.hi, b.hi)); }
VX_INL vxu vxu_max_s(vxu a, vxu b) { return vxu_of(_mm256_max_epi32(a.lo, b.lo), _mm256_max_epi32(a.hi, b.hi)); }
/* base[ix[lane]], base a pointer to 32-bit words: vpgatherdd. */
VX_INL vxu vxu_gather(const uint32_t *base, vxu ix) { return vxu_of(_mm256_i32gather_epi32((const int *)base, ix.lo, 4), _mm256_i32gather_epi32((const int *)base, ix.hi, 4)); }
/* Lanes of k from b, the rest from a. */
VX_INL vxu vxu_blend(vxk k, vxu a, vxu b)
{
    return vxu_of(_mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(a.lo), _mm256_castsi256_ps(b.lo), vx_k8(k))), _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(a.hi), _mm256_castsi256_ps(b.hi), vx_k8((unsigned)k >> 8))));
}
/* Whole numbers to F32 lanes, exact below 2^24, and back. */
VX_INL vxf vxu_to_f(vxu v) { return vx_of(_mm256_cvtepi32_ps(v.lo), _mm256_cvtepi32_ps(v.hi)); }
VX_INL vxu vxu_from_f(vxf v) { return vxu_of(_mm256_cvtps_epi32(v.lo), _mm256_cvtps_epi32(v.hi)); }
/* Lane i takes v[ix[i] & 15]. */
VX_INL __m256i vxu_pick8(vxu v, __m256i ix)
{
    return _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(_mm256_permutevar8x32_epi32(v.lo, ix)), _mm256_castsi256_ps(_mm256_permutevar8x32_epi32(v.hi, ix)), _mm256_castsi256_ps(_mm256_slli_epi32(ix, 28))));
}
VX_INL vxu vxu_permi(vxu v, vxidx ix) { return vxu_of(vxu_pick8(v, ix.lo), vxu_pick8(v, ix.hi)); }

#else /* the lane model */

typedef struct { uint32_t l[16]; } vxu;

static inline vxu vxu_zero(void) { vxu r; memset(r.l, 0, sizeof r.l); return r; }
static inline vxu vxu_load(const uint32_t *p) { vxu r; memcpy(r.l, p, sizeof r.l); return r; }
static inline vxu vxu_loadu(const uint32_t *p) { return vxu_load(p); }
static inline void vxu_store_nr(uint32_t *p, vxu v) { memcpy(p, v.l, sizeof v.l); }
static inline void vxu_store(uint32_t *p, vxu v) { memcpy(p, v.l, sizeof v.l); }
static inline vxu vxu_bcast(const uint32_t *p) { vxu r; VX_LANE_LOOP r.l[i] = *p; return r; }
static inline vxu vxu_and(vxu a, vxu b) { VX_LANE_LOOP a.l[i] &= b.l[i]; return a; }
static inline vxu vxu_or(vxu a, vxu b) { VX_LANE_LOOP a.l[i] |= b.l[i]; return a; }
static inline vxu vxu_xor(vxu a, vxu b) { VX_LANE_LOOP a.l[i] ^= b.l[i]; return a; }
static inline vxu vxu_add(vxu a, vxu b) { VX_LANE_LOOP a.l[i] += b.l[i]; return a; }
static inline vxu vxu_sub(vxu a, vxu b) { VX_LANE_LOOP a.l[i] -= b.l[i]; return a; }
static inline vxu vxu_mull(vxu a, vxu b) { VX_LANE_LOOP a.l[i] *= b.l[i]; return a; }
static inline vxu vxu_and_s(vxu a, const uint32_t *s) { VX_LANE_LOOP a.l[i] &= *s; return a; }
static inline vxu vxu_add_s(vxu a, const uint32_t *s) { VX_LANE_LOOP a.l[i] += *s; return a; }
static inline vxu vxu_srl_model(vxu a, int n) { VX_LANE_LOOP a.l[i] >>= n; return a; }
static inline vxu vxu_sll_model(vxu a, int n) { VX_LANE_LOOP a.l[i] <<= n; return a; }
#define vxu_srl(v, n) vxu_srl_model((v), (n))
static inline vxu vxu_sra_model(vxu a, int n) { VX_LANE_LOOP a.l[i] = (uint32_t)((int32_t)a.l[i] >> n); return a; }
#define vxu_sra(v, n) vxu_sra_model((v), (n))
static inline vxu vxu_min_s(vxu a, vxu b) { VX_LANE_LOOP a.l[i] = (int32_t)a.l[i] < (int32_t)b.l[i] ? a.l[i] : b.l[i]; return a; }
static inline vxu vxu_max_s(vxu a, vxu b) { VX_LANE_LOOP a.l[i] = (int32_t)a.l[i] > (int32_t)b.l[i] ? a.l[i] : b.l[i]; return a; }
static inline vxu vxu_gather(const uint32_t *base, vxu ix) { vxu r; VX_LANE_LOOP r.l[i] = base[(int32_t)ix.l[i]]; return r; }
#define vxu_sll(v, n) vxu_sll_model((v), (n))
static inline vxu vxu_blend(vxk k, vxu a, vxu b) { VX_LANE_LOOP if ((k >> i) & 1) a.l[i] = b.l[i]; return a; }
static inline vxf vxu_to_f(vxu v) { vxf r; VX_LANE_LOOP r.l[i] = (float)(int32_t)v.l[i]; return r; }
static inline vxu vxu_from_f(vxf v) { vxu r; VX_LANE_LOOP r.l[i] = (uint32_t)(int32_t)lrintf(v.l[i]); return r; }

static inline vxu vxu_permi(vxu v, vxidx ix) { vxu r; VX_LANE_LOOP r.l[i] = v.l[ix.l[i] & 15]; return r; }

#endif

#endif
