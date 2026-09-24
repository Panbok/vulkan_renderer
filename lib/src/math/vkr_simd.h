/**
 * @file vkr_simd.h
 * @brief Four-lane float and int32 vectors over ARM NEON, x86 SSE with FMA,
 * or a scalar fallback, selected at compile time.
 *
 * VKR_SIMD_F32X4 and VKR_SIMD_I32X4 are 16-byte aligned unions whose lanes
 * alias as x/y/z/w, r/g/b/a, s/t/p/q and elements[]; .neon or .sse exposes the
 * native register. The declarations below are followed by one implementation
 * block per target.
 */

#pragma once

#include "defines.h"
#include "vkr_math.h"
#include "vkr_pch.h"

// =============================================================================
// SIMD Type Definitions
// =============================================================================

typedef VKR_SIMD_ALIGN union {
#if VKR_SIMD_ARM_NEON
  float32x4_t neon; /**< Native ARM NEON vector register */
#elif VKR_SIMD_X86_AVX
  __m128 sse; /**< Native x86 SSE vector register */
#endif

  union {
    struct {
      union {
        float32_t x, r, s; /**< First element: X/Red/S coordinate */
      };
      union {
        float32_t y, g, t; /**< Second element: Y/Green/T coordinate */
      };
      union {
        float32_t z, b, p; /**< Third element: Z/Blue/P coordinate */
      };
      union {
        float32_t w, a, q; /**< Fourth element: W/Alpha/Q coordinate */
      };
    };
  };
  float32_t elements[4]; /**< Array access to all four elements */
} VKR_SIMD_F32X4;

typedef VKR_SIMD_ALIGN union {
#if VKR_SIMD_ARM_NEON
  int32x4_t neon; /**< Native ARM NEON integer vector register */
#elif VKR_SIMD_X86_AVX
  __m128i sse; /**< Native x86 SSE integer vector register */
#endif
  union {
    struct {
      union {
        int32_t x, r, s; /**< First element: X/Red/S coordinate */
      };
      union {
        int32_t y, g, t; /**< Second element: Y/Green/T coordinate */
      };
      union {
        int32_t z, b, p; /**< Third element: Z/Blue/P coordinate */
      };
      union {
        int32_t w, a, q; /**< Fourth element: W/Alpha/Q coordinate */
      };
    };
  };
  int32_t elements[4]; /**< Array access to all four elements */
} VKR_SIMD_I32X4;

// =============================================================================
// General-Purpose SIMD Operations
// =============================================================================

// =============================================================================
// SIMD Operations for float32_t vectors
// =============================================================================

/* Loads four floats from any alignment. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_load_f32x4(const float32_t *ptr);

/* Stores four floats to any alignment. */
vkr_internal INLINE void vkr_simd_store_f32x4(float32_t *ptr, VKR_SIMD_F32X4 v);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set_f32x4(float32_t x, float32_t y,
                                                      float32_t z, float32_t w);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set1_f32x4(float32_t value);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_add_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sub_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_mul_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

/* Divisors are not checked; zero yields IEEE infinity or NaN. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_div_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

/* Negative lanes yield NaN. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sqrt_f32x4(VKR_SIMD_F32X4 v);

/* Refined to float precision on every target. Zero and negative lanes give
 * target-dependent results, so callers guard them. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_rsqrt_f32x4(VKR_SIMD_F32X4 v);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_min_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_max_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b);

/* a + b * c. Rounded once on NEON and x86. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fma_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c);

/* a - b * c. Rounded once on NEON and x86. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fms_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c);

/* -(a + b * c). Rounded once on NEON and x86. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnma_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c);

/* -(a - b * c). Rounded once on NEON and x86. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnms_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c);

vkr_internal INLINE float32_t vkr_simd_hadd_f32x4(VKR_SIMD_F32X4 v);

vkr_internal INLINE float32_t vkr_simd_dot_f32x4(VKR_SIMD_F32X4 a,
                                                 VKR_SIMD_F32X4 b);

/* Dot product of the XYZ lanes; W is ignored. */
vkr_internal INLINE float32_t vkr_simd_dot3_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b);

/* Same as vkr_simd_dot_f32x4. */
vkr_internal INLINE float32_t vkr_simd_dot4_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b);

/* Lane indices in [0, 3]; they may be run-time values. */
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_shuffle_f32x4(VKR_SIMD_F32X4 v,
                                                          int32_t x, int32_t y,
                                                          int32_t z, int32_t w);

typedef enum VkrSimdCompareMode {
  VKR_SIMD_COMPARE_MODE_ABSOLUTE_DIFFERENCE = 0,
  VKR_SIMD_COMPARE_MODE_RELATIVE_DIFFERENCE = 1,
  VKR_SIMD_COMPARE_MODE_EQUAL = 2,
  VKR_SIMD_COMPARE_MODE_NOT_EQUAL = 3,
  VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON = 4,
  VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON = 5,
} VkrSimdCompareMode;

/* ABSOLUTE_DIFFERENCE: the 4D length of a - b is at most |epsilon|.
 * RELATIVE_DIFFERENCE: that length is at most |epsilon| times the longer input,
 * or at most |epsilon| when both inputs are that short. EQUAL and NOT_EQUAL
 * compare lanes exactly. EQUAL_EPSILON: every lane differs by at most
 * |epsilon|; NOT_EQUAL_EPSILON: any lane differs by more. */
vkr_internal INLINE bool8_t vkr_simd_compare_f32x4(VKR_SIMD_F32X4 a,
                                                   VKR_SIMD_F32X4 b,
                                                   VkrSimdCompareMode mode,
                                                   float32_t epsilon);

// =============================================================================
// SIMD Operations for int32_t vectors
// =============================================================================

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set_i32x4(int32_t x, int32_t y,
                                                      int32_t z, int32_t w);

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set1_i32x4(int32_t value);

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_add_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b);

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_sub_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b);

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_mul_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b);

// =============================================================================
// SIMD Operations Scatter-
// =============================================================================

/* Writes lane i of v to lane indices[i]. Out-of-range indices are skipped,
 * unwritten lanes are 0, and a repeated index keeps the last write. */
vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_scatter_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices);

/* Lane i reads v[indices[i]]; out-of-range indices read 0. */
vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_gather_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices);

// =============================================================================
// Platform-specific implementations
// =============================================================================

// Include platform-specific static INLINE implementations
#if VKR_SIMD_ARM_NEON

// ARM NEON static INLINE implementations
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_load_f32x4(const float32_t *ptr) {
  VKR_SIMD_F32X4 result;
  result.neon = vld1q_f32(ptr);
  return result;
}

vkr_internal INLINE void vkr_simd_store_f32x4(float32_t *ptr,
                                              VKR_SIMD_F32X4 v) {
  vst1q_f32(ptr, v.neon);
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set_f32x4(float32_t x, float32_t y,
                                                      float32_t z,
                                                      float32_t w) {
  VKR_SIMD_F32X4 result;
  result.neon = (float32x4_t){x, y, z, w};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set1_f32x4(float32_t value) {
  VKR_SIMD_F32X4 result;
  result.neon = vdupq_n_f32(value);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_add_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vaddq_f32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sub_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vsubq_f32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_mul_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vmulq_f32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_div_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vdivq_f32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result;
  result.neon = vsqrtq_f32(v.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_rsqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result;
  // vrsqrteq_f32 is only an eight-bit estimate, so each Newton-Raphson step
  // roughly doubles the correct bits. One step stops near sixteen bits, which
  // leaves vec3_normalize() short of unit length by about 6e-6 relative --
  // large enough to fail a 1e-5 unit-length check. Two steps reach float
  // precision.
  result.neon = vrsqrteq_f32(v.neon);
  result.neon = vmulq_f32(
      result.neon, vrsqrtsq_f32(vmulq_f32(v.neon, result.neon), result.neon));
  result.neon = vmulq_f32(
      result.neon, vrsqrtsq_f32(vmulq_f32(v.neon, result.neon), result.neon));
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_min_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vminq_f32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_max_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.neon = vmaxq_f32(a.neon, b.neon);
  return result;
}

// ARM NEON FMA implementations
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fma_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.neon = vfmaq_f32(a.neon, b.neon, c.neon); // a + (b * c)
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fms_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.neon = vfmsq_f32(a.neon, b.neon, c.neon); // a - (b * c)
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnma_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  // -(a + b * c) = -a - (b * c)
  result.neon = vfmsq_f32(vnegq_f32(a.neon), b.neon, c.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnms_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  // -(a - b * c) = -a + (b * c)
  result.neon = vfmaq_f32(vnegq_f32(a.neon), b.neon, c.neon);
  return result;
}

vkr_internal INLINE float32_t vkr_simd_hadd_f32x4(VKR_SIMD_F32X4 v) {
  return vaddvq_f32(v.neon);
}

// Optimized dot product using FMA and pairwise operations
vkr_internal INLINE float32_t vkr_simd_dot_f32x4(VKR_SIMD_F32X4 a,
                                                 VKR_SIMD_F32X4 b) {
  // Multiply the vectors element-wise
  float32x4_t prod = vmulq_f32(a.neon, b.neon);
  // Sum all elements of the result vector
  return vaddvq_f32(prod);
}

// 3D dot product (optimized for vec3 stored in vec4)
vkr_internal INLINE float32_t vkr_simd_dot3_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  float32x4_t prod = vmulq_f32(a.neon, b.neon);
  float32x2_t sum = vpadd_f32(vget_low_f32(prod), vget_low_f32(prod));
  return vget_lane_f32(sum, 0) + vgetq_lane_f32(prod, 2);
}

// 4D dot product (alias for clarity)
vkr_internal INLINE float32_t vkr_simd_dot4_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  return vkr_simd_dot_f32x4(a, b);
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_shuffle_f32x4(VKR_SIMD_F32X4 v,
                                                          int32_t x, int32_t y,
                                                          int32_t z,
                                                          int32_t w) {
  assert(x >= 0 && x < 4);
  assert(y >= 0 && y < 4);
  assert(z >= 0 && z < 4);
  assert(w >= 0 && w < 4);
  // ARM NEON doesn't have arbitrary shuffle, so we use element access
  VKR_SIMD_F32X4 result;
  result.elements[0] = v.elements[x];
  result.elements[1] = v.elements[y];
  result.elements[2] = v.elements[z];
  result.elements[3] = v.elements[w];
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set_i32x4(int32_t x, int32_t y,
                                                      int32_t z, int32_t w) {
  VKR_SIMD_I32X4 result;
  result.neon = (int32x4_t){x, y, z, w};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set1_i32x4(int32_t value) {
  VKR_SIMD_I32X4 result;
  result.neon = vdupq_n_s32(value);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_add_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.neon = vaddq_s32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_sub_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.neon = vsubq_s32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_mul_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.neon = vmulq_s32(a.neon, b.neon);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_scatter_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  // ARM NEON doesn't have direct scatter, so we use element access
  // This creates a result vector where each element from v is placed at the
  // position specified by the corresponding index (with bounds checking)
  VKR_SIMD_F32X4 result = vkr_simd_set1_f32x4(0.0f); // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_gather_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  // ARM NEON doesn't have direct gather, so we use element access
  // This gathers elements from v using indices to specify which elements to
  // pick
  VKR_SIMD_F32X4 result;

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[i] = v.elements[idx];
    } else {
      result.elements[i] = 0.0f; // Safety fallback for out-of-bounds indices
    }
  }
  return result;
}

vkr_internal INLINE bool8_t vkr_simd_compare_f32x4(VKR_SIMD_F32X4 a,
                                                   VKR_SIMD_F32X4 b,
                                                   VkrSimdCompareMode mode,
                                                   float32_t epsilon) {
  const float32_t abs_epsilon = vkr_abs_f32(epsilon);
  const float32_t epsilon_sq = abs_epsilon * abs_epsilon;

  switch (mode) {
  case VKR_SIMD_COMPARE_MODE_ABSOLUTE_DIFFERENCE: {
    float32x4_t diff = vsubq_f32(a.neon, b.neon);
    float32_t diff_len_sq = vaddvq_f32(vmulq_f32(diff, diff));
    return diff_len_sq <= epsilon_sq ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_RELATIVE_DIFFERENCE: {
    float32x4_t diff = vsubq_f32(a.neon, b.neon);
    float32_t diff_len_sq = vaddvq_f32(vmulq_f32(diff, diff));
    float32_t len_a_sq = vaddvq_f32(vmulq_f32(a.neon, a.neon));
    float32_t len_b_sq = vaddvq_f32(vmulq_f32(b.neon, b.neon));
    float32_t max_len_sq = vkr_max_f32(len_a_sq, len_b_sq);
    if (max_len_sq <= epsilon_sq) {
      return diff_len_sq <= epsilon_sq ? true_v : false_v;
    }
    float32_t threshold = epsilon_sq * max_len_sq;
    return diff_len_sq <= threshold ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_EQUAL: {
    uint32x4_t cmp = vceqq_f32(a.neon, b.neon);
    uint32_t mask[4];
    vst1q_u32(mask, cmp);
    uint32_t all_equal = mask[0] & mask[1] & mask[2] & mask[3];
    return all_equal == 0xFFFFFFFFu ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL: {
    uint32x4_t cmp = vceqq_f32(a.neon, b.neon);
    uint32_t mask[4];
    vst1q_u32(mask, cmp);
    uint32_t combined = mask[0] & mask[1] & mask[2] & mask[3];
    return combined == 0xFFFFFFFFu ? false_v : true_v;
  }
  case VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON: {
    float32x4_t abs_diff = vabsq_f32(vsubq_f32(a.neon, b.neon));
    float32x4_t eps_vec = vdupq_n_f32(abs_epsilon);
    uint32x4_t cmp = vcleq_f32(abs_diff, eps_vec);
    uint32_t mask[4];
    vst1q_u32(mask, cmp);
    uint32_t all_equal = mask[0] & mask[1] & mask[2] & mask[3];
    return all_equal == 0xFFFFFFFFu ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON: {
    float32x4_t abs_diff = vabsq_f32(vsubq_f32(a.neon, b.neon));
    float32x4_t eps_vec = vdupq_n_f32(abs_epsilon);
    uint32x4_t cmp = vcgtq_f32(abs_diff, eps_vec);
    uint32_t mask[4];
    vst1q_u32(mask, cmp);
    uint32_t any_greater = mask[0] | mask[1] | mask[2] | mask[3];
    return any_greater ? true_v : false_v;
  }
  default:
    return false_v;
  }
}

#elif defined(VKR_SIMD_X86_AVX)

vkr_internal INLINE float32_t vkr_sse_sum_f32x4(__m128 v) {
  float32_t temp[4];
  _mm_storeu_ps(temp, v);
  return temp[0] + temp[1] + temp[2] + temp[3];
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_load_f32x4(const float32_t *ptr) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_loadu_ps(ptr);
  return result;
}

vkr_internal INLINE void vkr_simd_store_f32x4(float32_t *ptr,
                                              VKR_SIMD_F32X4 v) {
  _mm_storeu_ps(ptr, v.sse);
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set_f32x4(float32_t x, float32_t y,
                                                      float32_t z,
                                                      float32_t w) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_set_ps(w, z, y, x);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set1_f32x4(float32_t value) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_set1_ps(value);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_add_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_add_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sub_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_sub_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_mul_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_mul_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_div_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_div_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_sqrt_ps(v.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_rsqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result;
  // _mm_rsqrt_ps starts at twelve bits, so a single Newton-Raphson step already
  // reaches float precision. The NEON path needs two steps from its weaker
  // eight-bit estimate to land in the same place.
  const __m128 estimate = _mm_rsqrt_ps(v.sse);
  const __m128 half = _mm_set1_ps(0.5f);
  const __m128 three_halves = _mm_set1_ps(1.5f);
  result.sse = _mm_mul_ps(
      estimate,
      _mm_sub_ps(three_halves, _mm_mul_ps(_mm_mul_ps(half, v.sse),
                                          _mm_mul_ps(estimate, estimate))));
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_min_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_min_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_max_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_max_ps(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fma_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_fmadd_ps(b.sse, c.sse, a.sse); // a + (b * c)
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fms_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_fnmadd_ps(b.sse, c.sse, a.sse); // a - (b * c)
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnma_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_fnmsub_ps(b.sse, c.sse, a.sse); // -(a + b * c)
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnms_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result;
  result.sse = _mm_fmsub_ps(b.sse, c.sse, a.sse); // -(a - b * c)
  return result;
}

vkr_internal INLINE float32_t vkr_simd_hadd_f32x4(VKR_SIMD_F32X4 v) {
  // _mm_hadd_ps(a, b) gives [a0+a1, a2+a3, b0+b1, b2+b3]
  // So _mm_hadd_ps(v, v) gives [v0+v1, v2+v3, v0+v1, v2+v3]
  // We need to extract and add the first two elements
  __m128 hadd_result = _mm_hadd_ps(v.sse, v.sse);
  return _mm_cvtss_f32(hadd_result) +
         _mm_cvtss_f32(
             _mm_shuffle_ps(hadd_result, hadd_result, _MM_SHUFFLE(1, 1, 1, 1)));
}

vkr_internal INLINE float32_t vkr_simd_dot_f32x4(VKR_SIMD_F32X4 a,
                                                 VKR_SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0xFF));
}

vkr_internal INLINE float32_t vkr_simd_dot3_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0x71));
}

vkr_internal INLINE float32_t vkr_simd_dot4_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0xFF));
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_shuffle_f32x4(VKR_SIMD_F32X4 v,
                                                          int32_t x, int32_t y,
                                                          int32_t z,
                                                          int32_t w) {
  // _mm_shuffle_ps requires a compile-time constant for the control mask,
  // so we must do the shuffle manually for variable indices.
  VKR_SIMD_F32X4 result;
  float tmp[4];
  _mm_storeu_ps(tmp, v.sse);
  float out[4];
  out[0] = tmp[x];
  out[1] = tmp[y];
  out[2] = tmp[z];
  out[3] = tmp[w];
  result.sse = _mm_loadu_ps(out);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set_i32x4(int32_t x, int32_t y,
                                                      int32_t z, int32_t w) {
  VKR_SIMD_I32X4 result;
  result.sse = _mm_set_epi32(w, z, y, x);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set1_i32x4(int32_t value) {
  VKR_SIMD_I32X4 result;
  result.sse = _mm_set1_epi32(value);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_add_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.sse = _mm_add_epi32(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_sub_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.sse = _mm_sub_epi32(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_mul_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result;
  result.sse = _mm_mullo_epi32(a.sse, b.sse);
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_scatter_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  VKR_SIMD_F32X4 result = vkr_simd_set1_f32x4(0.0f); // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_gather_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  VKR_SIMD_F32X4 result;

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[i] = v.elements[idx];
    } else {
      result.elements[i] = 0.0f; // Safety fallback for out-of-bounds indices
    }
  }
  return result;
}

vkr_internal INLINE bool8_t vkr_simd_compare_f32x4(VKR_SIMD_F32X4 a,
                                                   VKR_SIMD_F32X4 b,
                                                   VkrSimdCompareMode mode,
                                                   float32_t epsilon) {
  const float32_t abs_epsilon = vkr_abs_f32(epsilon);
  const float32_t epsilon_sq = abs_epsilon * abs_epsilon;

  switch (mode) {
  case VKR_SIMD_COMPARE_MODE_ABSOLUTE_DIFFERENCE: {
    __m128 diff = _mm_sub_ps(a.sse, b.sse);
    float32_t diff_len_sq = vkr_sse_sum_f32x4(_mm_mul_ps(diff, diff));
    return diff_len_sq <= epsilon_sq ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_RELATIVE_DIFFERENCE: {
    __m128 diff = _mm_sub_ps(a.sse, b.sse);
    float32_t diff_len_sq = vkr_sse_sum_f32x4(_mm_mul_ps(diff, diff));
    float32_t len_a_sq = vkr_sse_sum_f32x4(_mm_mul_ps(a.sse, a.sse));
    float32_t len_b_sq = vkr_sse_sum_f32x4(_mm_mul_ps(b.sse, b.sse));
    float32_t max_len_sq = vkr_max_f32(len_a_sq, len_b_sq);
    if (max_len_sq <= epsilon_sq) {
      return diff_len_sq <= epsilon_sq ? true_v : false_v;
    }
    float32_t threshold = epsilon_sq * max_len_sq;
    return diff_len_sq <= threshold ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_EQUAL: {
    int mask = _mm_movemask_ps(_mm_cmpeq_ps(a.sse, b.sse));
    return mask == 0xF ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL: {
    int mask = _mm_movemask_ps(_mm_cmpeq_ps(a.sse, b.sse));
    return mask == 0xF ? false_v : true_v;
  }
  case VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON: {
    const __m128 sign_mask = _mm_set1_ps(-0.0f);
    __m128 abs_diff = _mm_andnot_ps(sign_mask, _mm_sub_ps(a.sse, b.sse));
    __m128 eps_vec = _mm_set1_ps(abs_epsilon);
    int mask = _mm_movemask_ps(_mm_cmple_ps(abs_diff, eps_vec));
    return mask == 0xF ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON: {
    const __m128 sign_mask = _mm_set1_ps(-0.0f);
    __m128 abs_diff = _mm_andnot_ps(sign_mask, _mm_sub_ps(a.sse, b.sse));
    __m128 eps_vec = _mm_set1_ps(abs_epsilon);
    int mask = _mm_movemask_ps(_mm_cmpgt_ps(abs_diff, eps_vec));
    return mask ? true_v : false_v;
  }
  default:
    return false_v;
  }
}
#else
// Fallback scalar implementations
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_load_f32x4(const float32_t *ptr) {
  VKR_SIMD_F32X4 result = {{ptr[0], ptr[1], ptr[2], ptr[3]}};
  return result;
}

vkr_internal INLINE void vkr_simd_store_f32x4(float32_t *ptr,
                                              VKR_SIMD_F32X4 v) {
  ptr[0] = v.x;
  ptr[1] = v.y;
  ptr[2] = v.z;
  ptr[3] = v.w;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set_f32x4(float32_t x, float32_t y,
                                                      float32_t z,
                                                      float32_t w) {
  VKR_SIMD_F32X4 result = {{x, y, z, w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_set1_f32x4(float32_t value) {
  VKR_SIMD_F32X4 result = {{value, value, value, value}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_add_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sub_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_mul_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_div_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_sqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result = {{vkr_sqrt_f32(v.x), vkr_sqrt_f32(v.y),
                            vkr_sqrt_f32(v.z), vkr_sqrt_f32(v.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_rsqrt_f32x4(VKR_SIMD_F32X4 v) {
  VKR_SIMD_F32X4 result = {{1.0f / vkr_sqrt_f32(v.x), 1.0f / vkr_sqrt_f32(v.y),
                            1.0f / vkr_sqrt_f32(v.z),
                            1.0f / vkr_sqrt_f32(v.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_min_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {
      {Min(a.x, b.x), Min(a.y, b.y), Min(a.z, b.z), Min(a.w, b.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_max_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b) {
  VKR_SIMD_F32X4 result = {
      {Max(a.x, b.x), Max(a.y, b.y), Max(a.z, b.z), Max(a.w, b.w)}};
  return result;
}

// Fallback FMA implementations
vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fma_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result = {{a.x + (b.x * c.x), a.y + (b.y * c.y),
                            a.z + (b.z * c.z), a.w + (b.w * c.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fms_f32x4(VKR_SIMD_F32X4 a,
                                                      VKR_SIMD_F32X4 b,
                                                      VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result = {{a.x - (b.x * c.x), a.y - (b.y * c.y),
                            a.z - (b.z * c.z), a.w - (b.w * c.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnma_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result = {{-(a.x + b.x * c.x), -(a.y + b.y * c.y),
                            -(a.z + b.z * c.z), -(a.w + b.w * c.w)}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_fnms_f32x4(VKR_SIMD_F32X4 a,
                                                       VKR_SIMD_F32X4 b,
                                                       VKR_SIMD_F32X4 c) {
  VKR_SIMD_F32X4 result = {{-(a.x - b.x * c.x), -(a.y - b.y * c.y),
                            -(a.z - b.z * c.z), -(a.w - b.w * c.w)}};
  return result;
}

vkr_internal INLINE float32_t vkr_simd_hadd_f32x4(VKR_SIMD_F32X4 v) {
  return v.x + v.y + v.z + v.w;
}

// Optimized fallback dot product - hint to compiler for FMA
vkr_internal INLINE float32_t vkr_simd_dot_f32x4(VKR_SIMD_F32X4 a,
                                                 VKR_SIMD_F32X4 b) {
  // Structure to encourage compiler FMA generation
  float result = a.x * b.x;
  result += a.y * b.y; // Compiler may generate FMA here
  result += a.z * b.z; // And here
  result += a.w * b.w; // And here
  return result;
}

// Fallback 3D dot product
vkr_internal INLINE float32_t vkr_simd_dot3_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  float result = a.x * b.x;
  result += a.y * b.y;
  result += a.z * b.z;
  // Ignore w component
  return result;
}

// Fallback 4D dot product (alias)
vkr_internal INLINE float32_t vkr_simd_dot4_f32x4(VKR_SIMD_F32X4 a,
                                                  VKR_SIMD_F32X4 b) {
  return vkr_simd_dot_f32x4(a, b);
}

vkr_internal INLINE VKR_SIMD_F32X4 vkr_simd_shuffle_f32x4(VKR_SIMD_F32X4 v,
                                                          int32_t x, int32_t y,
                                                          int32_t z,
                                                          int32_t w) {
  assert(x >= 0 && x < 4);
  assert(y >= 0 && y < 4);
  assert(z >= 0 && z < 4);
  assert(w >= 0 && w < 4);
  VKR_SIMD_F32X4 result = {
      {v.elements[x], v.elements[y], v.elements[z], v.elements[w]}};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set_i32x4(int32_t x, int32_t y,
                                                      int32_t z, int32_t w) {
  VKR_SIMD_I32X4 result = {{x, y, z, w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_set1_i32x4(int32_t value) {
  VKR_SIMD_I32X4 result = {{value, value, value, value}};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_add_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result = {{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_sub_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result = {{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_I32X4 vkr_simd_mul_i32x4(VKR_SIMD_I32X4 a,
                                                      VKR_SIMD_I32X4 b) {
  VKR_SIMD_I32X4 result = {{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}};
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_scatter_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  // Scalar fallback for scatter operation
  VKR_SIMD_F32X4 result = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

vkr_internal INLINE VKR_SIMD_F32X4
vkr_simd_gather_f32x4(VKR_SIMD_F32X4 v, VKR_SIMD_I32X4 indices) {
  // Scalar fallback for gather operation
  VKR_SIMD_F32X4 result;

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[i] = v.elements[idx];
    } else {
      result.elements[i] = 0.0f; // Safety fallback for out-of-bounds indices
    }
  }
  return result;
}

vkr_internal INLINE bool8_t vkr_simd_compare_f32x4(VKR_SIMD_F32X4 a,
                                                   VKR_SIMD_F32X4 b,
                                                   VkrSimdCompareMode mode,
                                                   float32_t epsilon) {
  const float32_t abs_epsilon = vkr_abs_f32(epsilon);
  const float32_t epsilon_sq = abs_epsilon * abs_epsilon;
  const float32_t diff_x = a.x - b.x;
  const float32_t diff_y = a.y - b.y;
  const float32_t diff_z = a.z - b.z;
  const float32_t diff_w = a.w - b.w;
  const float32_t diff_len_sq =
      diff_x * diff_x + diff_y * diff_y + diff_z * diff_z + diff_w * diff_w;
  const float32_t abs_diff_x = vkr_abs_f32(diff_x);
  const float32_t abs_diff_y = vkr_abs_f32(diff_y);
  const float32_t abs_diff_z = vkr_abs_f32(diff_z);
  const float32_t abs_diff_w = vkr_abs_f32(diff_w);

  switch (mode) {
  case VKR_SIMD_COMPARE_MODE_ABSOLUTE_DIFFERENCE:
    return diff_len_sq <= epsilon_sq ? true_v : false_v;
  case VKR_SIMD_COMPARE_MODE_RELATIVE_DIFFERENCE: {
    const float32_t len_a_sq = a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w;
    const float32_t len_b_sq = b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w;
    const float32_t max_len_sq = vkr_max_f32(len_a_sq, len_b_sq);
    if (max_len_sq <= epsilon_sq) {
      return diff_len_sq <= epsilon_sq ? true_v : false_v;
    }
    const float32_t threshold = epsilon_sq * max_len_sq;
    return diff_len_sq <= threshold ? true_v : false_v;
  }
  case VKR_SIMD_COMPARE_MODE_EQUAL:
    return (diff_x == 0.0f && diff_y == 0.0f && diff_z == 0.0f &&
            diff_w == 0.0f)
               ? true_v
               : false_v;
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL:
    return (diff_x != 0.0f || diff_y != 0.0f || diff_z != 0.0f ||
            diff_w != 0.0f)
               ? true_v
               : false_v;
  case VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON:
    return (abs_diff_x <= abs_epsilon && abs_diff_y <= abs_epsilon &&
            abs_diff_z <= abs_epsilon && abs_diff_w <= abs_epsilon)
               ? true_v
               : false_v;
  case VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON:
    return (abs_diff_x > abs_epsilon || abs_diff_y > abs_epsilon ||
            abs_diff_z > abs_epsilon || abs_diff_w > abs_epsilon)
               ? true_v
               : false_v;
  default:
    return false_v;
  }
}
#endif
