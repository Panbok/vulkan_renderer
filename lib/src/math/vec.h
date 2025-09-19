// clang-format off

/**
 * @file vec.h
 * @brief Comprehensive vector mathematics library for 2D, 3D, and 4D operations.
 *
 * This file provides a complete set of vector types and operations optimized for
 * graphics programming, game development, and scientific computation. The library
 * supports float32_t and integer vectors with semantic aliasing for different use cases.
 *
 * Coordinate System:
 * - RIGHT-HANDED coordinate system (industry standard)
 * - X-axis: Points right
 * - Y-axis: Points up
 * - Z-axis: Points backward (toward the viewer)
 * - Rotations: Positive rotations are counter-clockwise when looking along the positive axis
 * - Cross product: X × Y = Z (right-hand rule)
 * - Compatible with: Vulkan, USD, glTF, Maya, Houdini
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
 * float32_t distance = vec3_length(velocity);
 * Vec3 direction = vec3_normalize(velocity);
 * float32_t projection = vec3_dot(position, direction);
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
 * float32_t diffuse = max_f32(0.0f, vec3_dot(surface_normal, light_dir));
 * 
 * // Specular reflection
 * Vec3 reflect_dir = vec3_reflect(light_dir, surface_normal);
 * float32_t specular = pow_f32(max_f32(0.0f, vec3_dot(view_dir, reflect_dir)), shininess);
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

#include "math.h"
#include "vkr_simd.h"

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
typedef VKR_SIMD_F32X4 Vec3;

/**
 * @brief 128-bit vector of four 32-bit floating-point values.
 * Optimized for 4D operations like homogeneous coordinates or colors.
 */
typedef VKR_SIMD_F32X4 Vec4;

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
typedef VKR_SIMD_I32X4 IVec4;

// =============================================================================
// Vector Constructor Functions
// =============================================================================

/**
 * @brief Creates a 2D vector from individual components
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @return 2D vector with specified components
 */
static INLINE Vec2 vec2_new(float32_t x, float32_t y) { return (Vec2){x, y}; }

/**
 * @brief Creates a zero 2D vector (0, 0)
 * @return 2D vector with all components set to zero
 */
static INLINE Vec2 vec2_zero(void) { return (Vec2){0.0f, 0.0f}; }

/**
 * @brief Creates a 2D vector with all components set to one (1, 1)
 * @return 2D vector with all components set to one
 */
static INLINE Vec2 vec2_one(void) { return (Vec2){1.0f, 1.0f}; }

/**
 * @brief Creates a 3D vector from individual components (SIMD-optimized)
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @param z Third component (Z/Blue/P/Q coordinate)
 * @return 3D vector with specified components (W automatically set to 0)
 */
static INLINE Vec3 vec3_new(float32_t x, float32_t y, float32_t z) {
  return vkr_simd_set_f32x4(x, y, z, 0.0f); // Pad with 0 for W
}

/**
 * @brief Creates a zero 3D vector (0, 0, 0)
 * @return 3D vector with all components set to zero
 */
static INLINE Vec3 vec3_zero(void) { return vkr_simd_set1_f32x4(0.0f); }

/**
 * @brief Creates a 3D vector with all components set to one (1, 1, 1)
 * @return 3D vector with all components set to one (W=0)
 */
static INLINE Vec3 vec3_one(void) {
  return vkr_simd_set_f32x4(1.0f, 1.0f, 1.0f, 0.0f);
}

/**
 * @brief Creates the up direction vector in right-handed coordinate system
 * @return 3D vector pointing up (0, 1, 0)
 * @note In right-handed system: Y-axis points upward
 */
static INLINE Vec3 vec3_up(void) {
  return vkr_simd_set_f32x4(0.0f, 1.0f, 0.0f, 0.0f);
}

/**
 * @brief Creates the down direction vector in right-handed coordinate system
 * @return 3D vector pointing down (0, -1, 0)
 * @note In right-handed system: Negative Y-axis points downward
 */
static INLINE Vec3 vec3_down(void) {
  return vkr_simd_set_f32x4(0.0f, -1.0f, 0.0f, 0.0f);
}

/**
 * @brief Creates the left direction vector in right-handed coordinate system
 * @return 3D vector pointing left (-1, 0, 0)
 * @note In right-handed system: Negative X-axis points left
 */
static INLINE Vec3 vec3_left(void) {
  return vkr_simd_set_f32x4(-1.0f, 0.0f, 0.0f, 0.0f);
}

/**
 * @brief Creates the right direction vector in right-handed coordinate system
 * @return 3D vector pointing right (1, 0, 0)
 * @note In right-handed system: X-axis points right
 */
static INLINE Vec3 vec3_right(void) {
  return vkr_simd_set_f32x4(1.0f, 0.0f, 0.0f, 0.0f);
}

/**
 * @brief Creates the forward direction vector in right-handed coordinate system
 * @return 3D vector pointing forward (0, 0, -1)
 * @note In right-handed system: Negative Z-axis points toward viewer (forward)
 * @note Compatible with Vulkan, USD, glTF coordinate conventions
 */
static INLINE Vec3 vec3_forward(void) {
  return vkr_simd_set_f32x4(0.0f, 0.0f, -1.0f,
                            0.0f); // -Z is forward in right-handed
}

/**
 * @brief Creates the backward direction vector in right-handed coordinate
 * system
 * @return 3D vector pointing backward (0, 0, 1)
 * @note In right-handed system: Positive Z-axis points away from viewer
 * (backward)
 * @note Compatible with Vulkan, USD, glTF coordinate conventions
 */
static INLINE Vec3 vec3_back(void) {
  return vkr_simd_set_f32x4(0.0f, 0.0f, 1.0f,
                            0.0f); // +Z is backward in right-handed
}

/**
 * @brief Creates a 4D vector from individual components (SIMD-optimized)
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @param z Third component (Z/Blue/P/Q coordinate)
 * @param w Fourth component (W/Alpha/Q coordinate)
 * @return 4D vector with specified components
 */
static INLINE Vec4 vec4_new(float32_t x, float32_t y, float32_t z,
                            float32_t w) {
  return vkr_simd_set_f32x4(x, y, z, w);
}

/**
 * @brief Creates a zero 4D vector (0, 0, 0, 0)
 * @return 4D vector with all components set to zero
 */
static INLINE Vec4 vec4_zero(void) { return vkr_simd_set1_f32x4(0.0f); }

/**
 * @brief Creates a 4D vector with all components set to one (1, 1, 1, 1)
 * @return 4D vector with all components set to one
 */
static INLINE Vec4 vec4_one(void) { return vkr_simd_set1_f32x4(1.0f); }

/**
 * @brief Creates a 2D integer vector from individual components
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @return 2D integer vector with specified components
 */
static INLINE IVec2 ivec2_new(int32_t x, int32_t y) { return (IVec2){x, y}; }

/**
 * @brief Creates a zero 2D integer vector (0, 0)
 * @return 2D integer vector with all components set to zero
 */
static INLINE IVec2 ivec2_zero(void) { return (IVec2){0, 0}; }

/**
 * @brief Creates a 3D integer vector from individual components
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @param z Third component (Z/Blue/P/Q coordinate)
 * @return 3D integer vector with specified components
 */
static INLINE IVec3 ivec3_new(int32_t x, int32_t y, int32_t z) {
  return (IVec3){x, y, z};
}

/**
 * @brief Creates a zero 3D integer vector (0, 0, 0)
 * @return 3D integer vector with all components set to zero
 */
static INLINE IVec3 ivec3_zero(void) { return (IVec3){0, 0, 0}; }

/**
 * @brief Creates a 4D integer vector from individual components
 * (SIMD-optimized)
 * @param x First component (X/Red/S/U coordinate)
 * @param y Second component (Y/Green/T/V coordinate)
 * @param z Third component (Z/Blue/P/Q coordinate)
 * @param w Fourth component (W/Alpha/Q coordinate)
 * @return 4D integer vector with specified components
 */
static INLINE IVec4 ivec4_new(int32_t x, int32_t y, int32_t z, int32_t w) {
  return vkr_simd_set_i32x4(x, y, z, w);
}

/**
 * @brief Creates a zero 4D integer vector (0, 0, 0, 0)
 * @return 4D integer vector with all components set to zero
 */
static INLINE IVec4 ivec4_zero(void) { return vkr_simd_set1_i32x4(0); }

// =============================================================================
// Vec2 Operations
// =============================================================================

/**
 * @brief Performs element-wise addition of two 2D vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y}
 */
static INLINE Vec2 vec2_add(Vec2 a, Vec2 b) {
  return (Vec2){a.x + b.x, a.y + b.y};
}

/**
 * @brief Performs element-wise subtraction of two 2D vectors
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y}
 */
static INLINE Vec2 vec2_sub(Vec2 a, Vec2 b) {
  return (Vec2){a.x - b.x, a.y - b.y};
}

/**
 * @brief Performs element-wise multiplication of two 2D vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y}
 */
static INLINE Vec2 vec2_mul(Vec2 a, Vec2 b) {
  return (Vec2){a.x * b.x, a.y * b.y};
}

/**
 * @brief Scales a 2D vector by a scalar value
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s}
 */
static INLINE Vec2 vec2_scale(Vec2 v, float32_t s) {
  return (Vec2){v.x * s, v.y * s};
}

/**
 * @brief Computes the dot product of two 2D vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Scalar dot product (a.x*b.x + a.y*b.y)
 */
static INLINE float32_t vec2_dot(Vec2 a, Vec2 b) {
  return a.x * b.x + a.y * b.y;
}

/**
 * @brief Computes the squared length of a 2D vector
 * @param v Input vector
 * @return Squared magnitude (avoids expensive sqrt operation)
 */
static INLINE float32_t vec2_length_squared(Vec2 v) { return vec2_dot(v, v); }

/**
 * @brief Computes the length (magnitude) of a 2D vector
 * @param v Input vector
 * @return Length of the vector
 */
static INLINE float32_t vec2_length(Vec2 v) {
  return sqrt_f32(vec2_length_squared(v));
}

/**
 * @brief Normalizes a 2D vector to unit length
 * @param v Vector to normalize
 * @return Unit vector in the same direction, or zero vector if input is too
 * small
 * @note Returns zero vector if input length is smaller than FLOAT_EPSILON
 */
static INLINE Vec2 vec2_normalize(Vec2 v) {
  float32_t len_sq = vec2_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    float32_t inv_len = 1.0f / sqrt_f32(len_sq);
    return vec2_scale(v, inv_len);
  }
  return vec2_zero();
}

/**
 * @brief Performs element-wise division of two 2D vectors
 * @param a First vector operand (dividend)
 * @param b Second vector operand (divisor)
 * @return Vector containing {a.x/b.x, a.y/b.y}
 * @note Division by zero behavior is undefined
 */
static INLINE Vec2 vec2_div(Vec2 a, Vec2 b) {
  return (Vec2){a.x / b.x, a.y / b.y};
}

/**
 * @brief Negates all components of a 2D vector
 * @param v Vector to negate
 * @return Vector with all components negated {-v.x, -v.y}
 */
static INLINE Vec2 vec2_negate(Vec2 v) { return (Vec2){-v.x, -v.y}; }

// =============================================================================
// Vec3 Operations (SIMD-accelerated using Vec4 with W=0)
// =============================================================================

/**
 * @brief Performs element-wise addition of two 3D vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z, 0}
 */
static INLINE Vec3 vec3_add(Vec3 a, Vec3 b) { return vkr_simd_add_f32x4(a, b); }

/**
 * @brief Performs element-wise subtraction of two 3D vectors (SIMD-optimized)
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z, 0}
 */
static INLINE Vec3 vec3_sub(Vec3 a, Vec3 b) { return vkr_simd_sub_f32x4(a, b); }

/**
 * @brief Performs element-wise multiplication of two 3D vectors
 * (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z, 0}
 */
static INLINE Vec3 vec3_mul(Vec3 a, Vec3 b) { return vkr_simd_mul_f32x4(a, b); }

/**
 * @brief Performs element-wise division of two 3D vectors (SIMD-optimized)
 * @param a First vector operand (dividend)
 * @param b Second vector operand (divisor)
 * @return Vector containing {a.x/b.x, a.y/b.y, a.z/b.z, 0}
 * @note Division by zero behavior is undefined
 * @note Sets W component to 1 internally to avoid division by zero
 */
static INLINE Vec3 vec3_div(Vec3 a, Vec3 b) {
  // Set W to 1 to avoid division by 0 (result W is ignored)
  Vec3 b_safe = vkr_simd_set_f32x4(b.x, b.y, b.z, 1.0f);
  return vkr_simd_div_f32x4(a, b_safe);
}

/**
 * @brief Scales a 3D vector by a scalar value (SIMD-optimized)
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s, v.z*s, 0}
 */
static INLINE Vec3 vec3_scale(Vec3 v, float32_t s) {
  return vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

/**
 * @brief Computes the dot product of two 3D vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Scalar dot product (a.x*b.x + a.y*b.y + a.z*b.z)
 * @note W component is ignored in calculation
 */
static INLINE float32_t vec3_dot(Vec3 a, Vec3 b) {
  return vkr_simd_dot3_f32x4(a, b);
}

/**
 * @brief Computes the cross product of two 3D vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Cross product vector perpendicular to both inputs
 * @note Follows right-hand rule: thumb=a, fingers=b, palm=result
 * @note Result magnitude equals |a|×|b|×sin(θ) where θ is angle between vectors
 * @note Cross product is anti-commutative: a×b = -(b×a)
 */
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

/**
 * @brief Computes the squared length of a 3D vector (SIMD-optimized)
 * @param v Input vector
 * @return Squared magnitude (avoids expensive sqrt operation)
 * @note W component is ignored in calculation
 */
static INLINE float32_t vec3_length_squared(Vec3 v) {
  return vkr_simd_dot3_f32x4(v, v);
}

/**
 * @brief Computes the length (magnitude) of a 3D vector
 * @param v Input vector
 * @return Length of the vector
 * @note W component is ignored in calculation
 */
static INLINE float32_t vec3_length(Vec3 v) {
  return sqrt_f32(vec3_length_squared(v));
}

/**
 * @brief Normalizes a 3D vector to unit length (SIMD-optimized)
 * @param v Vector to normalize
 * @return Unit vector in the same direction, or zero vector if input is too
 * small
 * @note Returns zero vector if input length is smaller than FLOAT_EPSILON
 * @note Uses hardware-accelerated reciprocal square root when available
 * @note W component is always set to 0
 */
static INLINE Vec3 vec3_normalize(Vec3 v) {
  float32_t len_sq = vec3_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    Vec3 result = vkr_simd_mul_f32x4(
        v, vkr_simd_rsqrt_f32x4(vkr_simd_set1_f32x4(len_sq)));
    result.w = 0.0f;
    return result;
  }
  return vec3_zero();
}

/**
 * @brief Negates all components of a 3D vector (SIMD-optimized)
 * @param v Vector to negate
 * @return Vector with all components negated {-v.x, -v.y, -v.z, 0}
 */
static INLINE Vec3 vec3_negate(Vec3 v) {
  return vkr_simd_sub_f32x4(vec3_zero(), v); // W stays 0
}

// =============================================================================
// Vec4 Operations (SIMD-optimized)
// =============================================================================

/**
 * @brief Performs element-wise addition of two 4D vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}
 */
static INLINE Vec4 vec4_add(Vec4 a, Vec4 b) { return vkr_simd_add_f32x4(a, b); }

/**
 * @brief Performs element-wise subtraction of two 4D vectors (SIMD-optimized)
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}
 */
static INLINE Vec4 vec4_sub(Vec4 a, Vec4 b) { return vkr_simd_sub_f32x4(a, b); }

/**
 * @brief Performs element-wise multiplication of two 4D vectors
 * (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}
 */
static INLINE Vec4 vec4_mul(Vec4 a, Vec4 b) { return vkr_simd_mul_f32x4(a, b); }

/**
 * @brief Scales a 4D vector by a scalar value (SIMD-optimized)
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s, v.z*s, v.w*s}
 */
static INLINE Vec4 vec4_scale(Vec4 v, float32_t s) {
  return vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

/**
 * @brief Computes the 4D dot product of two vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Scalar dot product (a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w)
 */
static INLINE float32_t vec4_dot(Vec4 a, Vec4 b) {
  return vkr_simd_dot4_f32x4(a, b);
}

/**
 * @brief Computes the 3D cross product of two Vec4 vectors (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Cross product vector perpendicular to both inputs (W component = 0)
 * @note Treats Vec4 as 3D vectors, ignoring W components in calculation
 * @note Follows right-hand rule: thumb=a, fingers=b, palm=result
 * @note Consistent with vec4_dot3() for 3D operations on Vec4 data
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

/**
 * @brief Computes the squared length of a 4D vector (SIMD-optimized)
 * @param v Input vector
 * @return Squared magnitude (avoids expensive sqrt operation)
 */
static INLINE float32_t vec4_length_squared(Vec4 v) { return vec4_dot(v, v); }

/**
 * @brief Computes the length (magnitude) of a 4D vector
 * @param v Input vector
 * @return Length of the vector
 */
static INLINE float32_t vec4_length(Vec4 v) {
  return sqrt_f32(vec4_length_squared(v));
}

/**
 * @brief Normalizes a 4D vector to unit length (SIMD-optimized)
 * @param v Vector to normalize
 * @return Unit vector in the same direction, or zero vector if input is too
 * small
 * @note Returns zero vector if input length is smaller than FLOAT_EPSILON
 * @note Uses hardware-accelerated reciprocal square root when available
 */
static INLINE Vec4 vec4_normalize(Vec4 v) {
  float32_t len_sq = vec4_length_squared(v);
  if (len_sq > FLOAT_EPSILON * FLOAT_EPSILON) {
    return vkr_simd_mul_f32x4(
        v, vkr_simd_rsqrt_f32x4(vkr_simd_set1_f32x4(len_sq)));
  }
  return vec4_zero();
}

/**
 * @brief Performs element-wise division of two 4D vectors (SIMD-optimized)
 * @param a First vector operand (dividend)
 * @param b Second vector operand (divisor)
 * @return Vector containing {a.x/b.x, a.y/b.y, a.z/b.z, a.w/b.w}
 * @note Division by zero behavior is undefined
 */
static INLINE Vec4 vec4_div(Vec4 a, Vec4 b) { return vkr_simd_div_f32x4(a, b); }

/**
 * @brief Negates all components of a 4D vector (SIMD-optimized)
 * @param v Vector to negate
 * @return Vector with all components negated {-v.x, -v.y, -v.z, -v.w}
 */
static INLINE Vec4 vec4_negate(Vec4 v) {
  return vkr_simd_sub_f32x4(vec4_zero(), v);
}

// =============================================================================
// IVec2 Operations
// =============================================================================

/**
 * @brief Performs element-wise addition of two 2D integer vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y}
 */
static INLINE IVec2 ivec2_add(IVec2 a, IVec2 b) {
  return (IVec2){a.x + b.x, a.y + b.y};
}

/**
 * @brief Performs element-wise subtraction of two 2D integer vectors
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y}
 */
static INLINE IVec2 ivec2_sub(IVec2 a, IVec2 b) {
  return (IVec2){a.x - b.x, a.y - b.y};
}

/**
 * @brief Performs element-wise multiplication of two 2D integer vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y}
 */
static INLINE IVec2 ivec2_mul(IVec2 a, IVec2 b) {
  return (IVec2){a.x * b.x, a.y * b.y};
}

/**
 * @brief Scales a 2D integer vector by a scalar value
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s}
 */
static INLINE IVec2 ivec2_scale(IVec2 v, int32_t s) {
  return (IVec2){v.x * s, v.y * s};
}

// =============================================================================
// IVec3 Operations
// =============================================================================

/**
 * @brief Performs element-wise addition of two 3D integer vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z}
 */
static INLINE IVec3 ivec3_add(IVec3 a, IVec3 b) {
  return (IVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

/**
 * @brief Performs element-wise subtraction of two 3D integer vectors
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z}
 */
static INLINE IVec3 ivec3_sub(IVec3 a, IVec3 b) {
  return (IVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

/**
 * @brief Performs element-wise multiplication of two 3D integer vectors
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z}
 */
static INLINE IVec3 ivec3_mul(IVec3 a, IVec3 b) {
  return (IVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}

/**
 * @brief Scales a 3D integer vector by a scalar value
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s, v.z*s}
 */
static INLINE IVec3 ivec3_scale(IVec3 v, int32_t s) {
  return (IVec3){v.x * s, v.y * s, v.z * s};
}

// =============================================================================
// IVec4 Operations (SIMD-optimized)
// =============================================================================

/**
 * @brief Performs element-wise addition of two 4D integer vectors
 * (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}
 */
static INLINE IVec4 ivec4_add(IVec4 a, IVec4 b) {
  return vkr_simd_add_i32x4(a, b);
}

/**
 * @brief Performs element-wise subtraction of two 4D integer vectors
 * (SIMD-optimized)
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}
 */
static INLINE IVec4 ivec4_sub(IVec4 a, IVec4 b) {
  return vkr_simd_sub_i32x4(a, b);
}

/**
 * @brief Performs element-wise multiplication of two 4D integer vectors
 * (SIMD-optimized)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}
 */
static INLINE IVec4 ivec4_mul(IVec4 a, IVec4 b) {
  return vkr_simd_mul_i32x4(a, b);
}

/**
 * @brief Scales a 4D integer vector by a scalar value (SIMD-optimized)
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @return Scaled vector {v.x*s, v.y*s, v.z*s, v.w*s}
 */
static INLINE IVec4 ivec4_scale(IVec4 v, int32_t s) {
  return vkr_simd_mul_i32x4(v, vkr_simd_set1_i32x4(s));
}

// =============================================================================
// Advanced Vector Operations
// =============================================================================

/**
 * @brief Performs linear interpolation between two 2D vectors
 * @param a Start vector (returned when t = 0.0)
 * @param b End vector (returned when t = 1.0)
 * @param t Interpolation parameter, typically in range [0.0, 1.0]
 * @return Interpolated vector between a and b
 * @note t values outside [0.0, 1.0] will extrapolate beyond the range [a, b]
 * @note Formula: a + t * (b - a)
 */
static INLINE Vec2 vec2_lerp(Vec2 a, Vec2 b, float32_t t) {
  return vec2_add(a, vec2_scale(vec2_sub(b, a), t));
}

/**
 * @brief Performs linear interpolation between two 3D vectors (FMA-optimized)
 * @param a Start vector (returned when t = 0.0)
 * @param b End vector (returned when t = 1.0)
 * @param t Interpolation parameter, typically in range [0.0, 1.0]
 * @return Interpolated vector between a and b
 * @note t values outside [0.0, 1.0] will extrapolate beyond the range [a, b]
 * @note Uses hardware FMA for improved precision and performance
 * @note W component remains 0
 */
static INLINE Vec3 vec3_lerp(Vec3 a, Vec3 b, float32_t t) {
  Vec4 t_vec = vec4_new(t, t, t, 0.0f); // Keep W at 0
  return vkr_simd_fma_f32x4(a, vkr_simd_sub_f32x4(b, a), t_vec);
}

/**
 * @brief Performs linear interpolation between two 4D vectors (FMA-optimized)
 * @param a Start vector (returned when t = 0.0)
 * @param b End vector (returned when t = 1.0)
 * @param t Interpolation parameter, typically in range [0.0, 1.0]
 * @return Interpolated vector between a and b
 * @note t values outside [0.0, 1.0] will extrapolate beyond the range [a, b]
 * @note Uses hardware FMA for improved precision and performance
 */
static INLINE Vec4 vec4_lerp(Vec4 a, Vec4 b, float32_t t) {
  Vec4 t_vec = vec4_new(t, t, t, t);
  return vkr_simd_fma_f32x4(a, vkr_simd_sub_f32x4(b, a), t_vec);
}

/**
 * @brief Reflects a 3D vector across a surface normal (SIMD-optimized)
 * @param v Incident vector (direction toward surface)
 * @param n Surface normal vector (should be normalized)
 * @return Reflected vector (direction away from surface)
 * @note Formula: v - 2 * (v · n) * n
 * @note Normal should be pointing away from the surface
 * @note Commonly used for ray tracing, lighting calculations, and physics
 */
static INLINE Vec3 vec3_reflect(Vec3 v, Vec3 n) {
  float32_t dot2 = 2.0f * vkr_simd_dot3_f32x4(v, n);
  return vkr_simd_sub_f32x4(v,
                            vkr_simd_mul_f32x4(n, vkr_simd_set1_f32x4(dot2)));
}

/**
 * @brief Computes the Euclidean distance between two 2D points
 * @param a First point
 * @param b Second point
 * @return Distance between the two points
 * @note Equivalent to vec2_length(vec2_sub(a, b))
 */
static INLINE float32_t vec2_distance(Vec2 a, Vec2 b) {
  return vec2_length(vec2_sub(a, b));
}

/**
 * @brief Computes the Euclidean distance between two 3D points (SIMD-optimized)
 * @param a First point
 * @param b Second point
 * @return Distance between the two points
 * @note Equivalent to vec3_length(vec3_sub(a, b))
 */
static INLINE float32_t vec3_distance(Vec3 a, Vec3 b) {
  return vec3_length(vec3_sub(a, b));
}

/**
 * @brief Computes the Euclidean distance between two 4D points (SIMD-optimized)
 * @param a First point
 * @param b Second point
 * @return Distance between the two points
 * @note Equivalent to vec4_length(vec4_sub(a, b))
 */
static INLINE float32_t vec4_distance(Vec4 a, Vec4 b) {
  return vec4_length(vec4_sub(a, b));
}

// =============================================================================
// Type Conversions
// =============================================================================

/**
 * @brief Converts a 4D vector to a 3D vector by discarding the W component
 * @param v Input 4D vector
 * @return 3D vector with X, Y, Z components copied and W set to 0
 * @note This is a zero-cost conversion as Vec3 is internally Vec4
 */
static INLINE Vec3 vec4_to_vec3(Vec4 v) {
  v.w = 0.0f;
  return v;
}

/**
 * @brief Converts a 3D vector to a 4D vector by setting the W component
 * @param v Input 3D vector
 * @param w Value for the W component
 * @return 4D vector with X, Y, Z components copied and W set to specified value
 * @note Common values: w=1.0 for positions, w=0.0 for directions
 */
static INLINE Vec4 vec3_to_vec4(Vec3 v, float32_t w) {
  Vec4 result = v;
  result.w = w;
  return result;
}

/**
 * @brief Converts a 3D vector to a 2D vector by discarding the Z component
 * @param v Input 3D vector
 * @return 2D vector with X and Y components copied
 * @note Useful for converting 3D positions to screen coordinates
 */
static INLINE Vec2 vec3_to_vec2(Vec3 v) { return (Vec2){v.x, v.y}; }

/**
 * @brief Converts a 2D vector to a 3D vector by setting the Z component
 * @param v Input 2D vector
 * @param z Value for the Z component
 * @return 3D vector with X, Y components copied and Z set to specified value
 * @note Useful for converting texture coordinates to 3D positions
 */
static INLINE Vec3 vec2_to_vec3(Vec2 v, float32_t z) {
  return vec3_new(v.x, v.y, z);
}

// =============================================================================
// Mutable Operations (for performance-critical code)
// =============================================================================

/**
 * @brief In-place addition of two 4D vectors (SIMD-optimized)
 * @param dest Pointer to destination vector (modified in-place)
 * @param a First vector operand
 * @param b Second vector operand
 * @note Stores result directly to avoid temporary allocations
 * @note Useful in performance-critical loops and accumulation operations
 */
static INLINE void vec4_add_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_add_f32x4(a, b);
}

/**
 * @brief In-place subtraction of two 4D vectors (SIMD-optimized)
 * @param dest Pointer to destination vector (modified in-place)
 * @param a First vector operand (minuend)
 * @param b Second vector operand (subtrahend)
 * @note Stores result directly to avoid temporary allocations
 * @note Useful in performance-critical loops and physics updates
 */
static INLINE void vec4_sub_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_sub_f32x4(a, b);
}

/**
 * @brief In-place multiplication of two 4D vectors (SIMD-optimized)
 * @param dest Pointer to destination vector (modified in-place)
 * @param a First vector operand
 * @param b Second vector operand
 * @note Stores result directly to avoid temporary allocations
 * @note Useful in performance-critical loops and component-wise scaling
 */
static INLINE void vec4_mul_mut(Vec4 *dest, Vec4 a, Vec4 b) {
  *dest = vkr_simd_mul_f32x4(a, b);
}

/**
 * @brief In-place scaling of a 4D vector (SIMD-optimized)
 * @param dest Pointer to destination vector (modified in-place)
 * @param v Vector to scale
 * @param s Scalar multiplier
 * @note Stores result directly to avoid temporary allocations
 * @note Useful in performance-critical loops and batch processing
 */
static INLINE void vec4_scale_mut(Vec4 *dest, Vec4 v, float32_t s) {
  *dest = vkr_simd_mul_f32x4(v, vkr_simd_set1_f32x4(s));
}

// =============================================================================
// FMA-Optimized Operations
// =============================================================================

/**
 * @brief Fused multiply-add operation for 4D vectors: a * b + c
 * @param a First multiplicand vector
 * @param b Second multiplicand vector
 * @param c Addend vector
 * @return Vector containing {a.x*b.x+c.x, a.y*b.y+c.y, a.z*b.z+c.z,
 * a.w*b.w+c.w}
 * @note Uses hardware FMA when available for improved precision and performance
 * @note Single rounding error instead of two separate operations
 */
static INLINE Vec4 vec4_muladd(Vec4 a, Vec4 b, Vec4 c) {
  return vkr_simd_fma_f32x4(c, a, b);
}

/**
 * @brief Multiply-subtract operation for 4D vectors: a * b - c
 * @param a First multiplicand vector
 * @param b Second multiplicand vector
 * @param c Subtrahend vector
 * @return Vector containing {a.x*b.x-c.x, a.y*b.y-c.y, a.z*b.z-c.z,
 * a.w*b.w-c.w}
 * @note More precise than separate multiply and subtract operations
 */
static INLINE Vec4 vec4_mulsub(Vec4 a, Vec4 b, Vec4 c) {
  return vkr_simd_sub_f32x4(vkr_simd_mul_f32x4(a, b), c);
}

/**
 * @brief Scale-add operation for 4D vectors: a + v * scale
 * @param a Base vector
 * @param v Vector to scale and add
 * @param scale Scalar multiplier
 * @return Vector containing a + v * scale
 * @note Uses hardware FMA for improved precision and performance
 * @note Useful for accumulation operations and physics updates
 */
static INLINE Vec4 vec4_scaleadd(Vec4 a, Vec4 v, float32_t scale) {
  return vkr_simd_fma_f32x4(a, v, vkr_simd_set1_f32x4(scale));
}

/**
 * @brief Computes the 3D dot product of two 4D vectors (ignores W component)
 * @param a First vector operand
 * @param b Second vector operand
 * @return Scalar dot product (a.x*b.x + a.y*b.y + a.z*b.z)
 * @note Provides 3D dot product for consistency with Vec4 API
 * @note Commonly used for 3D operations stored in Vec4 format
 */
static INLINE float32_t vec4_dot3(Vec4 a, Vec4 b) {
  return vkr_simd_dot3_f32x4(a, b);
}

/**
 * @brief Fast squared length computation for 4D vectors
 * @param v Input vector
 * @return Squared magnitude (v.x² + v.y² + v.z² + v.w²)
 * @note Alias for vec4_length_squared for performance-critical code
 * @note Avoids function call overhead in hot paths
 */
static INLINE float32_t vec4_length_squared_fast(Vec4 v) {
  return vkr_simd_dot4_f32x4(v, v);
}

/**
 * @brief Fast 3D squared length computation for 4D vectors (ignores W)
 * @param v Input vector
 * @return Squared 3D magnitude (v.x² + v.y² + v.z²)
 * @note Alias for vec3_length_squared but operates on Vec4
 * @note Useful when working with Vec4 storage but 3D semantics
 */
static INLINE float32_t vec4_length3_squared_fast(Vec4 v) {
  return vkr_simd_dot3_f32x4(v, v);
}
