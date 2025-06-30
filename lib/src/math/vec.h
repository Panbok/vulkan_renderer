// clang-format off

/**
 * @file vec.h
 * @brief Comprehensive vector mathematics library for 2D, 3D, and 4D operations.
 *
 * This file provides a complete set of vector types and operations optimized for
 * graphics programming, game development, and scientific computation. The library
 * supports float and integer vectors with semantic aliasing for different use cases.
 *
 * Vector Types and Dimensions:
 * - Vec2: 2D floating-point vectors (texture coordinates, 2D positions)
 * - Vec3: 3D floating-point vectors (positions, directions, normals, colors)
 * - Vec4: 4D floating-point vectors (homogeneous coordinates, RGBA colors)
 * - IVec2/IVec3/IVec4: Integer vectors for indices, masks, bit operations
 *
 * SIMD Optimization:
 * - Vec4 operations leverage hardware SIMD instructions (ARM NEON, x86 SSE)
 * - FMA (Fused Multiply-Add) operations for improved precision and performance
 * - Scalar fallbacks ensure compatibility across all platforms
 * - 16-byte alignment for optimal memory access patterns
 *
 * Memory Layout and Semantic Aliasing:
 * All vector types use union-based structures to provide multiple semantic
 * access patterns for the same underlying data:
 *
 * Vec3 Memory Layout (internally Vec4 with W=0):
 * +------------------+ <-- 16-byte SIMD aligned structure
 * | union { x,r,s,u} |  [0] First component (mathematical/color/texture/UV)
 * +------------------+
 * | union { y,g,t,v} |  [4] Second component (mathematical/color/texture/UV)
 * +------------------+
 * | union { z,b,p,q} |  [8] Third component (mathematical/color/texture/coord)
 * +------------------+
 * | w = 0            | [12] Fourth component (always 0, ignored for Vec3)
 * +------------------+
 *
 * Semantic Access Patterns:
 * - Mathematical: v.x, v.y, v.z, v.w (position vectors, direction vectors)
 * - Color: v.r, v.g, v.b, v.a (red, green, blue, alpha channels)
 * - Texture: v.s, v.t, v.p, v.q (texture coordinates, 3D/cube textures)
 * - UV/ST: v.u, v.v (2D texture mapping coordinates)
 * - Array: v.elements[0-3] (indexed access for loops and algorithms)
 *
 * Union Memory Sharing:
 * Each union ensures that all aliases refer to the SAME memory location:
 * ```c
 * Vec3 color = vec3_new(0.8f, 0.2f, 0.1f);
 * // These are ALL equivalent - same memory, different names:
 * assert(color.x == color.r && color.r == color.s && color.s == color.u);
 * assert(color.y == color.g && color.g == color.t && color.t == color.v);
 * assert(color.z == color.b && color.b == color.p && color.p == color.q);
 * assert(color.elements[0] == color.x);
 * assert(color.elements[1] == color.y);
 * assert(color.elements[2] == color.z);
 * ```
 *
 * Performance Characteristics:
 * - Vec3/Vec4: Full hardware SIMD acceleration with FMA support
 * - Vec3: Uses Vec4 operations internally with W=0 for optimal performance
 * - Vec2: Scalar operations optimized for compiler auto-vectorization
 * - Normalization: Uses optimized SIMD rsqrt for Vec3/Vec4
 * - Memory: 16-byte aligned for Vec3/Vec4, optimal cache line usage
 * - Branching: Minimal branching in hot paths
 *
 * API Design Patterns:
 * 1. Constructor Functions: vec3_new(), vec3_zero(), vec3_one()
 * 2. Basic Operations: vec3_add(), vec3_sub(), vec3_mul(), vec3_scale()
 * 3. Geometric Operations: vec3_dot(), vec3_cross(), vec3_length(), vec3_normalize()
 * 4. Advanced Operations: vec3_lerp(), vec3_reflect(), vec3_distance()
 * 5. Type Conversions: vec3_to_vec4(), vec4_to_vec3(), vec2_to_vec3()
 * 6. Mutable Operations: vec4_add_mut() for performance-critical code
 * 7. FMA Operations: vec4_muladd(), vec4_scaleadd() for precision
 *
 * Usage Examples:
 *
 * Basic Vector Operations:
 * ```c
 * // Create vectors using different semantic meanings
 * Vec3 position = vec3_new(10.0f, 5.0f, 0.0f);
 * Vec3 velocity = vec3_new(1.0f, 0.0f, 0.0f);
 * Vec3 color = {.r = 1.0f, .g = 0.5f, .b = 0.2f};
 * 
 * // Basic arithmetic
 * Vec3 new_position = vec3_add(position, velocity);
 * Vec3 scaled_velocity = vec3_scale(velocity, 2.0f);
 * 
 * // Geometric operations
 * float distance = vec3_length(velocity);
 * Vec3 direction = vec3_normalize(velocity);
 * float projection = vec3_dot(position, direction);
 * ```
 *
 * Rendering Example:
 * ```c
 * // Lighting calculation
 * Vec3 surface_normal = vec3_normalize(surface_pos);
 * Vec3 light_dir = vec3_normalize(vec3_sub(light_pos, surface_pos));
 * Vec3 view_dir = vec3_normalize(vec3_sub(camera_pos, surface_pos));
 * 
 * // Diffuse lighting
 * float diffuse = max_f32(0.0f, vec3_dot(surface_normal, light_dir));
 * 
 * // Specular reflection
 * Vec3 reflect_dir = vec3_reflect(light_dir, surface_normal);
 * float specular = pow_f32(max_f32(0.0f, vec3_dot(view_dir, reflect_dir)), shininess);
 * 
 * // Color computation using color semantic aliases
 * Vec3 final_color = vec3_add(
 *     vec3_scale(material_color, diffuse),
 *     vec3_scale(specular_color, specular)
 * );
 * ```
 *
 * SIMD Vec4 Example:
 * ```c
 * // Hardware-accelerated 4D operations
 * Vec4 vertices[4] = {
 *     vec4_new(0.0f, 0.0f, 0.0f, 1.0f),
 *     vec4_new(1.0f, 0.0f, 0.0f, 1.0f),
 *     vec4_new(1.0f, 1.0f, 0.0f, 1.0f),
 *     vec4_new(0.0f, 1.0f, 0.0f, 1.0f)
 * };
 * 
 * // Transform vertices with matrix (conceptual)
 * for (int i = 0; i < 4; i++) {
 *     // FMA-optimized transformation
 *     vertices[i] = vec4_muladd(row0, vec4_new(vertices[i].x, vertices[i].x, vertices[i].x, vertices[i].x),
 *                               vec4_muladd(row1, vec4_new(vertices[i].y, vertices[i].y, vertices[i].y, vertices[i].y),
 *                                          vec4_muladd(row2, vec4_new(vertices[i].z, vertices[i].z, vertices[i].z, vertices[i].z),
 *                                                     row3)));
 * }
 * ```
 *
 * Type Conversion and Interoperability:
 * ```c
 * // Seamless conversion between vector types
 * Vec2 texture_coord = vec2_new(0.5f, 0.25f);
 * Vec3 position_3d = vec2_to_vec3(texture_coord, 0.0f);  // W=0 automatically
 * Vec4 homogeneous = vec3_to_vec4(position_3d, 1.0f);    // Sets W=1
 * 
 * // Extract components
 * Vec3 rgb = vec4_to_vec3(homogeneous);  // Sets W=0
 * Vec2 uv = vec3_to_vec2(position_3d);   // Drops Z and W
 * ```
 *
 * Performance-Critical Code:
 * ```c
 * // Use mutable operations to avoid temporary allocations
 * Vec4 accumulator = vec4_zero();
 * for (int i = 0; i < vertex_count; i++) {
 *     vec4_add_mut(&accumulator, accumulator, vertices[i]);
 * }
 * vec4_scale_mut(&accumulator, accumulator, 1.0f / vertex_count);
 * ```
 *
 * Thread Safety:
 * All vector operations are thread-safe as they operate on local data and
 * do not modify global state. Vectors can be safely used in multi-threaded
 * environments without synchronization.
 *
 * Compiler Optimizations:
 * - All functions are marked static inline for zero-cost abstractions
 * - Structured to encourage compiler auto-vectorization where SIMD unavailable
 * - FMA operations use hardware acceleration when available
 * - Memory access patterns optimized for cache efficiency
 */

// clang-format on
#pragma once

#include "../simd/simd.h"
#include "math.h"

// =============================================================================
// Vector Type Definitions
// =============================================================================

/**
 * @brief 64-bit vector of two 32-bit floating-point values.
 * Optimized for 2D operations like texture coordinates or complex numbers.
 */
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

/**
 * @brief 3D vector type using 128-bit SIMD representation internally.
 * Uses Vec4 (SIMD) operations for hardware acceleration with W=0 padding.
 * This follows industry standard practice for optimal performance.
 * @note The W component is always 0 for Vec3 operations and should be ignored.
 */
typedef SIMD_F32X4 Vec3;

/**
 * @brief 128-bit vector of four 32-bit floating-point values.
 * Optimized for 4D operations like homogeneous coordinates or colors.
 */
typedef SIMD_F32X4 Vec4;

/**
 * @brief 64-bit vector of two 32-bit signed integers.
 * Used for integer vector operations, masks, and bit manipulation.
 */
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

/**
 * @brief 96-bit vector of three 32-bit signed integers.
 * Used for integer vector operations, masks, and bit manipulation.
 */
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

/**
 * @brief 128-bit vector of four 32-bit signed integers.
 * Used for integer vector operations, masks, and bit manipulation.
 */
typedef SIMD_I32X4 IVec4;

// =============================================================================
// Vector Constructor Functions
// =============================================================================

static INLINE Vec2 vec2_new(float x, float y) { return (Vec2){x, y}; }

static INLINE Vec2 vec2_zero(void) { return (Vec2){0.0f, 0.0f}; }

static INLINE Vec2 vec2_one(void) { return (Vec2){1.0f, 1.0f}; }

static INLINE Vec3 vec3_new(float x, float y, float z) {
  return simd_set_f32x4(x, y, z, 0.0f); // Pad with 0 for W
}

static INLINE Vec3 vec3_zero(void) { return simd_set1_f32x4(0.0f); }

static INLINE Vec3 vec3_one(void) {
  return simd_set_f32x4(1.0f, 1.0f, 1.0f, 0.0f);
}

static INLINE Vec4 vec4_new(float x, float y, float z, float w) {
  return simd_set_f32x4(x, y, z, w);
}

static INLINE Vec4 vec4_zero(void) { return simd_set1_f32x4(0.0f); }

static INLINE Vec4 vec4_one(void) { return simd_set1_f32x4(1.0f); }

static INLINE IVec2 ivec2_new(int32_t x, int32_t y) { return (IVec2){x, y}; }

static INLINE IVec2 ivec2_zero(void) { return (IVec2){0, 0}; }

static INLINE IVec3 ivec3_new(int32_t x, int32_t y, int32_t z) {
  return (IVec3){x, y, z};
}

static INLINE IVec3 ivec3_zero(void) { return (IVec3){0, 0, 0}; }

static INLINE IVec4 ivec4_new(int32_t x, int32_t y, int32_t z, int32_t w) {
  return simd_set_i32x4(x, y, z, w);
}

static INLINE IVec4 ivec4_zero(void) { return simd_set1_i32x4(0); }

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

static INLINE Vec2 vec2_scale(Vec2 v, float s) {
  return (Vec2){v.x * s, v.y * s};
}

static INLINE float vec2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

static INLINE float vec2_length_squared(Vec2 v) { return vec2_dot(v, v); }

static INLINE float vec2_length(Vec2 v) {
  return sqrt_f32(vec2_length_squared(v));
}

static INLINE Vec2 vec2_normalize(Vec2 v) {
  float len_sq = vec2_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    float inv_len = 1.0f / sqrt_f32(len_sq);
    return vec2_scale(v, inv_len);
  }
  return vec2_zero();
}

static INLINE Vec2 vec2_div(Vec2 a, Vec2 b) {
  return (Vec2){a.x / b.x, a.y / b.y};
}

static INLINE Vec2 vec2_negate(Vec2 v) { return (Vec2){-v.x, -v.y}; }

// =============================================================================
// Vec3 Operations (SIMD-accelerated using Vec4 with W=0)
// =============================================================================

static INLINE Vec3 vec3_add(Vec3 a, Vec3 b) { return simd_add_f32x4(a, b); }

static INLINE Vec3 vec3_sub(Vec3 a, Vec3 b) { return simd_sub_f32x4(a, b); }

static INLINE Vec3 vec3_mul(Vec3 a, Vec3 b) { return simd_mul_f32x4(a, b); }

static INLINE Vec3 vec3_div(Vec3 a, Vec3 b) {
  // Set W to 1 to avoid division by 0 (result W is ignored)
  Vec3 b_safe = simd_set_f32x4(b.x, b.y, b.z, 1.0f);
  return simd_div_f32x4(a, b_safe);
}

static INLINE Vec3 vec3_scale(Vec3 v, float s) {
  return simd_mul_f32x4(v, simd_set1_f32x4(s));
}

static INLINE float vec3_dot(Vec3 a, Vec3 b) { return simd_dot3_f32x4(a, b); }

static INLINE Vec3 vec3_cross(Vec3 a, Vec3 b) {
  Vec3 a_yzx = simd_shuffle_f32x4(a, 1, 2, 0, 3); // (y, z, x, w)
  Vec3 b_yzx = simd_shuffle_f32x4(b, 1, 2, 0, 3); // (y, z, x, w)
  Vec3 a_zxy = simd_shuffle_f32x4(a, 2, 0, 1, 3); // (z, x, y, w)
  Vec3 b_zxy = simd_shuffle_f32x4(b, 2, 0, 1, 3); // (z, x, y, w)

  Vec3 result = simd_sub_f32x4(simd_mul_f32x4(a_yzx, b_zxy),
                               simd_mul_f32x4(a_zxy, b_yzx));

  result.w = 0.0f;
  return result;
}

static INLINE float vec3_length_squared(Vec3 v) {
  return simd_dot3_f32x4(v, v);
}

static INLINE float vec3_length(Vec3 v) {
  return sqrt_f32(vec3_length_squared(v));
}

static INLINE Vec3 vec3_normalize(Vec3 v) {
  float len_sq = vec3_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    Vec3 result = simd_mul_f32x4(v, simd_rsqrt_f32x4(simd_set1_f32x4(len_sq)));
    result.w = 0.0f;
    return result;
  }
  return vec3_zero();
}

static INLINE Vec3 vec3_negate(Vec3 v) {
  return simd_sub_f32x4(vec3_zero(), v); // W stays 0
}

// =============================================================================
// Vec4 Operations (SIMD-optimized)
// =============================================================================

static INLINE Vec4 vec4_add(Vec4 a, Vec4 b) { return simd_add_f32x4(a, b); }

static INLINE Vec4 vec4_sub(Vec4 a, Vec4 b) { return simd_sub_f32x4(a, b); }

static INLINE Vec4 vec4_mul(Vec4 a, Vec4 b) { return simd_mul_f32x4(a, b); }

static INLINE Vec4 vec4_scale(Vec4 v, float s) {
  return simd_mul_f32x4(v, simd_set1_f32x4(s));
}

static INLINE float vec4_dot(Vec4 a, Vec4 b) { return simd_dot4_f32x4(a, b); }

static INLINE float vec4_length_squared(Vec4 v) { return vec4_dot(v, v); }

static INLINE float vec4_length(Vec4 v) {
  return sqrt_f32(vec4_length_squared(v));
}

static INLINE Vec4 vec4_normalize(Vec4 v) {
  float len_sq = vec4_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    return simd_mul_f32x4(v, simd_rsqrt_f32x4(simd_set1_f32x4(len_sq)));
  }
  return vec4_zero();
}

static INLINE Vec4 vec4_div(Vec4 a, Vec4 b) { return simd_div_f32x4(a, b); }

static INLINE Vec4 vec4_negate(Vec4 v) {
  return simd_sub_f32x4(vec4_zero(), v);
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

static INLINE IVec4 ivec4_add(IVec4 a, IVec4 b) { return simd_add_i32x4(a, b); }

static INLINE IVec4 ivec4_sub(IVec4 a, IVec4 b) { return simd_sub_i32x4(a, b); }

static INLINE IVec4 ivec4_mul(IVec4 a, IVec4 b) { return simd_mul_i32x4(a, b); }

static INLINE IVec4 ivec4_scale(IVec4 v, int32_t s) {
  return simd_mul_i32x4(v, simd_set1_i32x4(s));
}

// =============================================================================
// Advanced Vector Operations
// =============================================================================

// Linear interpolation
static INLINE Vec2 vec2_lerp(Vec2 a, Vec2 b, float t) {
  return vec2_add(a, vec2_scale(vec2_sub(b, a), t));
}

static INLINE Vec3 vec3_lerp(Vec3 a, Vec3 b, float t) {
  Vec4 t_vec = vec4_new(t, t, t, 0.0f); // Keep W at 0
  return simd_fma_f32x4(a, simd_sub_f32x4(b, a), t_vec);
}

// FMA-optimized Vec4 lerp: a + t * (b - a)
static INLINE Vec4 vec4_lerp(Vec4 a, Vec4 b, float t) {
  Vec4 t_vec = vec4_new(t, t, t, t);
  return simd_fma_f32x4(a, simd_sub_f32x4(b, a), t_vec);
}

static INLINE Vec3 vec3_reflect(Vec3 v, Vec3 n) {
  float dot2 = 2.0f * simd_dot3_f32x4(v, n);
  return simd_sub_f32x4(v, simd_mul_f32x4(n, simd_set1_f32x4(dot2)));
}

static INLINE float vec2_distance(Vec2 a, Vec2 b) {
  return vec2_length(vec2_sub(a, b));
}

static INLINE float vec3_distance(Vec3 a, Vec3 b) {
  return vec3_length(vec3_sub(a, b));
}

static INLINE float vec4_distance(Vec4 a, Vec4 b) {
  return vec4_length(vec4_sub(a, b));
}

// =============================================================================
// Type Conversions
// =============================================================================

static INLINE Vec3 vec4_to_vec3(Vec4 v) {
  v.w = 0.0f;
  return v;
}

static INLINE Vec4 vec3_to_vec4(Vec3 v, float w) {
  Vec4 result = v;
  result.w = w;
  return result;
}

static INLINE Vec2 vec3_to_vec2(Vec3 v) { return (Vec2){v.x, v.y}; }

static INLINE Vec3 vec2_to_vec3(Vec2 v, float z) {
  return vec3_new(v.x, v.y, z);
}

// =============================================================================
// Mutable Operations (for performance-critical code)
// =============================================================================

static INLINE void vec4_add_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = simd_add_f32x4(a, b);
}

static INLINE void vec4_sub_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = simd_sub_f32x4(a, b);
}

static INLINE void vec4_mul_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = simd_mul_f32x4(a, b);
}

static INLINE void vec4_scale_mut(Vec4 *dest, Vec4 v, float s) {
  *dest = simd_mul_f32x4(v, simd_set1_f32x4(s));
}

// =============================================================================
// FMA-Optimized Operations
// =============================================================================

static INLINE Vec4 vec4_muladd(Vec4 a, Vec4 b, Vec4 c) {
  return simd_fma_f32x4(c, a, b);
}

static INLINE Vec4 vec4_mulsub(Vec4 a, Vec4 b, Vec4 c) {
  return simd_sub_f32x4(simd_mul_f32x4(a, b), c);
}

static INLINE Vec4 vec4_scaleadd(Vec4 a, Vec4 v, float scale) {
  return simd_fma_f32x4(a, v, simd_set1_f32x4(scale));
}

static INLINE float vec4_dot3(Vec4 a, Vec4 b) { return simd_dot3_f32x4(a, b); }

static INLINE float vec4_length_squared_fast(Vec4 v) {
  return simd_dot4_f32x4(v, v);
}

static INLINE float vec4_length3_squared_fast(Vec4 v) {
  return simd_dot3_f32x4(v, v);
}
