/**
 * @file vec.h
 * @brief 2D, 3D and 4D float and integer vectors.
 *
 * Right-handed: X right, Y up, Z toward the viewer, so forward is -Z and
 * X x Y = Z. Vec3 and Vec4 share the 16-byte SIMD type; a Vec3 keeps W at 0
 * and Vec3 operations ignore it. Vec2, IVec2 and IVec3 are scalar unions.
 * Components alias as x/y/z/w, r/g/b/a, s/t/p/q and u/v.
 */

#pragma once

#include "math.h"
#include "vkr_simd.h"

// =============================================================================
// Vector Type Definitions
// =============================================================================

typedef union {
  struct {
    union {
      float32_t x, r, s, u; /**< First element: X/Red/S/U coordinate */
    };
    union {
      float32_t y, g, t, v; /**< Second element: Y/Green/T/V coordinate */
    };
  };
  float32_t elements[2]; /**< Array access to both elements */
} Vec2;

/* Vec4 storage whose W stays 0; Vec3 operations ignore it. */
typedef VKR_SIMD_F32X4 Vec3;

typedef VKR_SIMD_F32X4 Vec4;

typedef union {
  struct {
    union {
      int32_t x, r, s, u; /**< First element: X/Red/S/U coordinate */
    };
    union {
      int32_t y, g, t, v; /**< Second element: Y/Green/T/V coordinate */
    };
  };
  int32_t elements[2]; /**< Array access to all two elements */
} IVec2;

typedef union {
  struct {
    union {
      int32_t x, r, s, u; /**< First element: X/Red/S/U coordinate */
    };
    union {
      int32_t y, g, t, v; /**< Second element: Y/Green/T/V coordinate */
    };
    union {
      int32_t z, b, p, q; /**< Third element: Z/Blue/P/Q coordinate */
    };
  };
  int32_t elements[3]; /**< Array access to all three elements */
} IVec3;

typedef VKR_SIMD_I32X4 IVec4;

// =============================================================================
// Vector Constructor Functions
// =============================================================================

static INLINE Vec2 vec2_new(float32_t x, float32_t y) { return (Vec2){x, y}; }

static INLINE Vec2 vec2_zero(void) { return (Vec2){0.0f, 0.0f}; }

static INLINE Vec2 vec2_one(void) { return (Vec2){1.0f, 1.0f}; }

static INLINE Vec3 vec3_new(float32_t x, float32_t y, float32_t z) {
  return vkr_simd_set_f32x4(x, y, z, 0.0f); // Pad with 0 for W
}

static INLINE Vec3 vec3_zero(void) { return vkr_simd_set1_f32x4(0.0f); }

static INLINE Vec3 vec3_one(void) {
  return vkr_simd_set_f32x4(1.0f, 1.0f, 1.0f, 0.0f);
}

static INLINE Vec3 vec3_up(void) {
  return vkr_simd_set_f32x4(0.0f, 1.0f, 0.0f, 0.0f);
}

static INLINE Vec3 vec3_down(void) {
  return vkr_simd_set_f32x4(0.0f, -1.0f, 0.0f, 0.0f);
}

static INLINE Vec3 vec3_left(void) {
  return vkr_simd_set_f32x4(-1.0f, 0.0f, 0.0f, 0.0f);
}

static INLINE Vec3 vec3_right(void) {
  return vkr_simd_set_f32x4(1.0f, 0.0f, 0.0f, 0.0f);
}

/* -Z, the right-handed view direction. */
static INLINE Vec3 vec3_forward(void) {
  return vkr_simd_set_f32x4(0.0f, 0.0f, -1.0f, 0.0f);
}

/* +Z, toward the viewer. */
static INLINE Vec3 vec3_back(void) {
  return vkr_simd_set_f32x4(0.0f, 0.0f, 1.0f, 0.0f);
}

static INLINE Vec4 vec4_new(float32_t x, float32_t y, float32_t z,
                            float32_t w) {
  return vkr_simd_set_f32x4(x, y, z, w);
}

static INLINE Vec4 vec4_zero(void) { return vkr_simd_set1_f32x4(0.0f); }

static INLINE Vec4 vec4_one(void) { return vkr_simd_set1_f32x4(1.0f); }

static INLINE IVec2 ivec2_new(int32_t x, int32_t y) { return (IVec2){x, y}; }

static INLINE IVec2 ivec2_zero(void) { return (IVec2){0, 0}; }

static INLINE IVec3 ivec3_new(int32_t x, int32_t y, int32_t z) {
  return (IVec3){x, y, z};
}

static INLINE IVec3 ivec3_zero(void) { return (IVec3){0, 0, 0}; }

static INLINE IVec4 ivec4_new(int32_t x, int32_t y, int32_t z, int32_t w) {
  return vkr_simd_set_i32x4(x, y, z, w);
}

static INLINE IVec4 ivec4_zero(void) { return vkr_simd_set1_i32x4(0); }

// =============================================================================
// Vec2 Operations
// =============================================================================

static INLINE Vec2 vec2_add(Vec2 a, Vec2 b) {
  return (Vec2){a.x + b.x, a.y + b.y};
}

static INLINE Vec2 vec2_sub(Vec2 a, Vec2 b) {
  return (Vec2){a.x - b.x, a.y - b.y};
}

static INLINE Vec2 vec2_mul(Vec2 a, Vec2 b) {
  return (Vec2){a.x * b.x, a.y * b.y};
}

static INLINE Vec2 vec2_scale(Vec2 v, float32_t s) {
  return (Vec2){v.x * s, v.y * s};
}

static INLINE float32_t vec2_dot(Vec2 a, Vec2 b) {
  return a.x * b.x + a.y * b.y;
}

static INLINE float32_t vec2_length_squared(Vec2 v) { return vec2_dot(v, v); }

static INLINE float32_t vec2_length(Vec2 v) {
  return vkr_sqrt_f32(vec2_length_squared(v));
}

/* Returns the zero vector when the length is at most VKR_FLOAT_EPSILON. */
static INLINE Vec2 vec2_normalize(Vec2 v) {
  float32_t len_sq = vec2_length_squared(v);
  if (len_sq > VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
    float32_t inv_len = 1.0f / vkr_sqrt_f32(len_sq);
    return vec2_scale(v, inv_len);
  }
  return vec2_zero();
}

/* Divisors are not checked; zero yields IEEE infinity or NaN. */
static INLINE Vec2 vec2_div(Vec2 a, Vec2 b) {
  return (Vec2){a.x / b.x, a.y / b.y};
}

static INLINE Vec2 vec2_negate(Vec2 v) { return (Vec2){-v.x, -v.y}; }

/* True when every component differs by less than epsilon; vec3 and vec4
 * comparisons include the bound. */
static INLINE bool8_t vec2_equal(Vec2 a, Vec2 b, float32_t epsilon) {
  return vkr_abs_f32(a.x - b.x) < epsilon && vkr_abs_f32(a.y - b.y) < epsilon;
}

/* True when a component differs by more than epsilon, so a difference of
 * exactly epsilon is neither equal nor not equal. */
static INLINE bool8_t vec2_not_equal(Vec2 a, Vec2 b, float32_t epsilon) {
  return vkr_abs_f32(a.x - b.x) > epsilon || vkr_abs_f32(a.y - b.y) > epsilon;
}

// =============================================================================
// Vec3 Operations (SIMD-accelerated using Vec4 with W=0)
// =============================================================================

static INLINE Vec3 vec3_add(Vec3 a, Vec3 b) { return vkr_simd_add_f32x4(a, b); }

static INLINE Vec3 vec3_sub(Vec3 a, Vec3 b) { return vkr_simd_sub_f32x4(a, b); }

static INLINE Vec3 vec3_mul(Vec3 a, Vec3 b) { return vkr_simd_mul_f32x4(a, b); }

/* Divisors are not checked. W divides by 1, so the padding stays 0. */
static INLINE Vec3 vec3_div(Vec3 a, Vec3 b) {
  // Set W to 1 to avoid division by 0 (result W is ignored)
  Vec3 b_safe = vkr_simd_set_f32x4(b.x, b.y, b.z, 1.0f);
  return vkr_simd_div_f32x4(a, b_safe);
}

static INLINE Vec3 vec3_scale(Vec3 v, float32_t s) {
  return vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

static INLINE float32_t vec3_dot(Vec3 a, Vec3 b) {
  return vkr_simd_dot3_f32x4(a, b);
}

/* Right-hand rule; the result's W is 0. */
static INLINE Vec3 vec3_cross(Vec3 a, Vec3 b) {
  Vec3 a_yzx = vkr_simd_shuffle_f32x4(a, 1, 2, 0, 3); // (y, z, x, w)
  Vec3 b_yzx = vkr_simd_shuffle_f32x4(b, 1, 2, 0, 3); // (y, z, x, w)
  Vec3 a_zxy = vkr_simd_shuffle_f32x4(a, 2, 0, 1, 3); // (z, x, y, w)
  Vec3 b_zxy = vkr_simd_shuffle_f32x4(b, 2, 0, 1, 3); // (z, x, y, w)

  Vec3 result = vkr_simd_sub_f32x4(vkr_simd_mul_f32x4(a_yzx, b_zxy),
                                   vkr_simd_mul_f32x4(a_zxy, b_yzx));

  result.w = 0.0f;
  return result;
}

static INLINE float32_t vec3_length_squared(Vec3 v) {
  return vkr_simd_dot3_f32x4(v, v);
}

static INLINE float32_t vec3_length(Vec3 v) {
  return vkr_sqrt_f32(vec3_length_squared(v));
}

/* Returns the zero vector when the length is at most VKR_FLOAT_EPSILON. The
 * reciprocal square root is refined to float precision (vkr_simd_rsqrt_f32x4).
 */
static INLINE Vec3 vec3_normalize(Vec3 v) {
  float32_t len_sq = vec3_length_squared(v);
  if (len_sq > VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
    Vec3 result = vkr_simd_mul_f32x4(
        v, vkr_simd_rsqrt_f32x4(vkr_simd_set1_f32x4(len_sq)));
    result.w = 0.0f;
    return result;
  }
  return vec3_zero();
}

static INLINE Vec3 vec3_negate(Vec3 v) {
  return vkr_simd_sub_f32x4(vec3_zero(), v); // W stays 0
}

/* True when every component differs by at most epsilon. */
static INLINE bool8_t vec3_equal(Vec3 a, Vec3 b, float32_t epsilon) {
  return vkr_simd_compare_f32x4(a, b, VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON,
                                epsilon);
}

/* True when any component differs by more than epsilon. */
static INLINE bool8_t vec3_not_equal(Vec3 a, Vec3 b, float32_t epsilon) {
  return vkr_simd_compare_f32x4(a, b, VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON,
                                epsilon);
}

// =============================================================================
// Vec4 Operations (SIMD-optimized)
// =============================================================================

static INLINE Vec4 vec4_add(Vec4 a, Vec4 b) { return vkr_simd_add_f32x4(a, b); }

static INLINE Vec4 vec4_sub(Vec4 a, Vec4 b) { return vkr_simd_sub_f32x4(a, b); }

static INLINE Vec4 vec4_mul(Vec4 a, Vec4 b) { return vkr_simd_mul_f32x4(a, b); }

static INLINE Vec4 vec4_scale(Vec4 v, float32_t s) {
  return vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

static INLINE float32_t vec4_dot(Vec4 a, Vec4 b) {
  return vkr_simd_dot4_f32x4(a, b);
}

/* Cross product of the XYZ parts; input W is ignored and the result's W is 0.
 */
static INLINE Vec4 vec4_cross3(Vec4 a, Vec4 b) {
  Vec4 a_yzx = vkr_simd_shuffle_f32x4(a, 1, 2, 0, 3); // (y, z, x, w)
  Vec4 b_yzx = vkr_simd_shuffle_f32x4(b, 1, 2, 0, 3); // (y, z, x, w)
  Vec4 a_zxy = vkr_simd_shuffle_f32x4(a, 2, 0, 1, 3); // (z, x, y, w)
  Vec4 b_zxy = vkr_simd_shuffle_f32x4(b, 2, 0, 1, 3); // (z, x, y, w)

  Vec4 result = vkr_simd_sub_f32x4(vkr_simd_mul_f32x4(a_yzx, b_zxy),
                                   vkr_simd_mul_f32x4(a_zxy, b_yzx));
  result.w = 0.0f; // Ensure W component is 0 for 3D cross product
  return result;
}

static INLINE float32_t vec4_length_squared(Vec4 v) { return vec4_dot(v, v); }

static INLINE float32_t vec4_length(Vec4 v) {
  return vkr_sqrt_f32(vec4_length_squared(v));
}

/* Returns the zero vector when the length is at most VKR_FLOAT_EPSILON.
 * Normalizes all four components. */
static INLINE Vec4 vec4_normalize(Vec4 v) {
  float32_t len_sq = vec4_length_squared(v);
  if (len_sq > VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
    return vkr_simd_mul_f32x4(
        v, vkr_simd_rsqrt_f32x4(vkr_simd_set1_f32x4(len_sq)));
  }
  return vec4_zero();
}

/* Divisors are not checked; zero yields IEEE infinity or NaN. */
static INLINE Vec4 vec4_div(Vec4 a, Vec4 b) { return vkr_simd_div_f32x4(a, b); }

static INLINE Vec4 vec4_negate(Vec4 v) {
  return vkr_simd_sub_f32x4(vec4_zero(), v);
}

/* True when every component differs by at most epsilon. */
static INLINE bool8_t vec4_equal(Vec4 a, Vec4 b, float32_t epsilon) {
  return vkr_simd_compare_f32x4(a, b, VKR_SIMD_COMPARE_MODE_EQUAL_EPSILON,
                                epsilon);
}

/* True when any component differs by more than epsilon. */
static INLINE bool8_t vec4_not_equal(Vec4 a, Vec4 b, float32_t epsilon) {
  return vkr_simd_compare_f32x4(a, b, VKR_SIMD_COMPARE_MODE_NOT_EQUAL_EPSILON,
                                epsilon);
}

// =============================================================================
// IVec2 Operations
// =============================================================================

static INLINE IVec2 ivec2_add(IVec2 a, IVec2 b) {
  return (IVec2){a.x + b.x, a.y + b.y};
}

static INLINE IVec2 ivec2_sub(IVec2 a, IVec2 b) {
  return (IVec2){a.x - b.x, a.y - b.y};
}

static INLINE IVec2 ivec2_mul(IVec2 a, IVec2 b) {
  return (IVec2){a.x * b.x, a.y * b.y};
}

static INLINE IVec2 ivec2_scale(IVec2 v, int32_t s) {
  return (IVec2){v.x * s, v.y * s};
}

// =============================================================================
// IVec3 Operations
// =============================================================================

static INLINE IVec3 ivec3_add(IVec3 a, IVec3 b) {
  return (IVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

static INLINE IVec3 ivec3_sub(IVec3 a, IVec3 b) {
  return (IVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static INLINE IVec3 ivec3_mul(IVec3 a, IVec3 b) {
  return (IVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}

static INLINE IVec3 ivec3_scale(IVec3 v, int32_t s) {
  return (IVec3){v.x * s, v.y * s, v.z * s};
}

// =============================================================================
// IVec4 Operations (SIMD-optimized)
// =============================================================================

static INLINE IVec4 ivec4_add(IVec4 a, IVec4 b) {
  return vkr_simd_add_i32x4(a, b);
}

static INLINE IVec4 ivec4_sub(IVec4 a, IVec4 b) {
  return vkr_simd_sub_i32x4(a, b);
}

static INLINE IVec4 ivec4_mul(IVec4 a, IVec4 b) {
  return vkr_simd_mul_i32x4(a, b);
}

static INLINE IVec4 ivec4_scale(IVec4 v, int32_t s) {
  return vkr_simd_mul_i32x4(v, vkr_simd_set1_i32x4(s));
}

// =============================================================================
// Advanced Vector Operations
// =============================================================================

/* t = 0 returns a and t = 1 returns b; t outside [0, 1] extrapolates. */
static INLINE Vec2 vec2_lerp(Vec2 a, Vec2 b, float32_t t) {
  return vec2_add(a, vec2_scale(vec2_sub(b, a), t));
}

/* t = 0 returns a and t = 1 returns b; t outside [0, 1] extrapolates. */
static INLINE Vec3 vec3_lerp(Vec3 a, Vec3 b, float32_t t) {
  Vec4 t_vec = vec4_new(t, t, t, 0.0f); // Keep W at 0
  return vkr_simd_fma_f32x4(a, vkr_simd_sub_f32x4(b, a), t_vec);
}

/* t = 0 returns a and t = 1 returns b; t outside [0, 1] extrapolates. */
static INLINE Vec4 vec4_lerp(Vec4 a, Vec4 b, float32_t t) {
  Vec4 t_vec = vec4_new(t, t, t, t);
  return vkr_simd_fma_f32x4(a, vkr_simd_sub_f32x4(b, a), t_vec);
}

/* Reflects v about the unit normal n: v - 2 (v . n) n. */
static INLINE Vec3 vec3_reflect(Vec3 v, Vec3 n) {
  float32_t dot2 = 2.0f * vkr_simd_dot3_f32x4(v, n);
  return vkr_simd_sub_f32x4(v,
                            vkr_simd_mul_f32x4(n, vkr_simd_set1_f32x4(dot2)));
}

static INLINE float32_t vec2_distance(Vec2 a, Vec2 b) {
  return vec2_length(vec2_sub(a, b));
}

static INLINE float32_t vec3_distance(Vec3 a, Vec3 b) {
  return vec3_length(vec3_sub(a, b));
}

static INLINE float32_t vec4_distance(Vec4 a, Vec4 b) {
  return vec4_length(vec4_sub(a, b));
}

// =============================================================================
// Type Conversions
// =============================================================================

/* Sets W to 0. */
static INLINE Vec3 vec4_to_vec3(Vec4 v) {
  v.w = 0.0f;
  return v;
}

/* Use w = 1 for points and w = 0 for directions. */
static INLINE Vec4 vec3_to_vec4(Vec3 v, float32_t w) {
  Vec4 result = v;
  result.w = w;
  return result;
}

static INLINE Vec2 vec3_to_vec2(Vec3 v) { return (Vec2){v.x, v.y}; }

static INLINE Vec3 vec2_to_vec3(Vec2 v, float32_t z) {
  return vec3_new(v.x, v.y, z);
}

// =============================================================================
// Mutable Operations (for performance-critical code)
// =============================================================================

static INLINE void vec4_add_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_add_f32x4(a, b);
}

static INLINE void vec4_sub_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_sub_f32x4(a, b);
}

static INLINE void vec4_mul_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_mul_f32x4(a, b);
}

static INLINE void vec4_scale_mut(Vec4 *dest, Vec4 v, float32_t s) {
  *dest = vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

// =============================================================================
// FMA-Optimized Operations
// =============================================================================

/* a * b + c, rounded once where the target has fused multiply-add. */
static INLINE Vec4 vec4_muladd(Vec4 a, Vec4 b, Vec4 c) {
  return vkr_simd_fma_f32x4(c, a, b);
}

/* a * b - c, rounded after the multiply and again after the subtract. */
static INLINE Vec4 vec4_mulsub(Vec4 a, Vec4 b, Vec4 c) {
  return vkr_simd_sub_f32x4(vkr_simd_mul_f32x4(a, b), c);
}

/* a + v * scale, rounded once where the target has fused multiply-add. */
static INLINE Vec4 vec4_scaleadd(Vec4 a, Vec4 v, float32_t scale) {
  return vkr_simd_fma_f32x4(a, v, vkr_simd_set1_f32x4(scale));
}

/* Dot product of the XYZ parts; W is ignored. */
static INLINE float32_t vec4_dot3(Vec4 a, Vec4 b) {
  return vkr_simd_dot3_f32x4(a, b);
}

/* Same as vec4_length_squared. */
static INLINE float32_t vec4_length_squared_fast(Vec4 v) {
  return vkr_simd_dot4_f32x4(v, v);
}

/* Squared length of the XYZ part; W is ignored. */
static INLINE float32_t vec4_length3_squared_fast(Vec4 v) {
  return vkr_simd_dot3_f32x4(v, v);
}
