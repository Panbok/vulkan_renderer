// clang-format off

/**
 * @file simd.h
 * @brief Cross-platform SIMD (Single Instruction, Multiple Data) operations abstraction layer.
 *
 * This file provides a unified interface for vector operations across different
 * CPU architectures, specifically ARM NEON and x86 SSE. The implementation uses
 * compile-time detection to select the appropriate instruction set, with scalar
 * fallbacks for unsupported platforms.
 *
 * SIMD Architecture Support:
 * - ARM NEON (ARMv7, ARM64): Full implementation with native intrinsics
 * - x86 SSE/SSE2 (Intel/AMD): Type definitions ready, implementation pending
 * - Scalar Fallback: Pure C implementation for unsupported architectures
 *
 * Memory Layout and Alignment:
 * All SIMD types are 16-byte aligned for optimal performance. The union-based
 * approach allows seamless access to vector data in multiple ways:
 *
 * SIMD_F32X4 Memory Layout:
 * +------------------+ <-- 16-byte aligned address
 * | float32_t x/r/s  |  [0] First component (X/Red/S texture coordinate)
 * +------------------+
 * | float32_t y/g/t  |  [4] Second component (Y/Green/T texture coordinate)
 * +------------------+
 * | float32_t z/b/p  |  [8] Third component (Z/Blue/P texture coordinate)
 * +------------------+
 * | float32_t w/a/q  | [12] Fourth component (W/Alpha/Q texture coordinate)
 * +------------------+
 *
 * Element Access Patterns:
 * - Mathematical: x, y, z, w (position, direction vectors)
 * - Color: r, g, b, a (red, green, blue, alpha channels)
 * - Texture: s, t, p, q (texture coordinates)
 * - Array: elements[0-3] (direct array access)
 * - Native: .neon or .sse (platform-specific vector register)
 *
 * Performance Characteristics:
 * - ARM NEON: Optimized with hardware FMA, efficient horizontal operations
 * - Scalar Fallback: Structured to encourage compiler auto-vectorization
 * - 16-byte alignment ensures cache-friendly access patterns
 * - Minimal branching in hot paths for predictable performance
 *
 * Usage Patterns:
 * 1. Load data from memory: simd_load_f32x4()
 * 2. Perform vector operations: simd_add_f32x4(), simd_mul_f32x4(), etc.
 * 3. Use specialized operations: simd_dot_f32x4(), simd_fma_f32x4()
 * 4. Store results back to memory: simd_store_f32x4()
 *
 * Example Usage:
 * ```c
 * // Vector addition example
 * float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
 * float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
 * float result[4];
 * 
 * SIMD_F32X4 va = simd_load_f32x4(a);
 * SIMD_F32X4 vb = simd_load_f32x4(b);
 * SIMD_F32X4 vr = simd_add_f32x4(va, vb);
 * simd_store_f32x4(result, vr);
 * // result = {6.0f, 8.0f, 10.0f, 12.0f}
 * 
 * // Dot product example
 * float dot = simd_dot_f32x4(va, vb);
 * // dot = (1*5 + 2*6 + 3*7 + 4*8) = 70.0f
 * ```
 *
 * Thread Safety:
 * All SIMD operations are thread-safe as they operate on local data and
 * registers. No global state is modified during vector operations.
 */

// clang-format on
#pragma once

#include "../math/math.h"
#include "defines.h"
#include "pch.h"

// =============================================================================
// SIMD Alignment and Attributes
// =============================================================================

/**
 * @brief Alignment attribute for optimal SIMD performance.
 * Ensures 16-byte alignment required by most SIMD instruction sets.
 */
#if defined(_MSC_VER)
#define SIMD_ALIGN __declspec(align(16))
#else
#define SIMD_ALIGN __attribute__((aligned(16)))
#endif

// =============================================================================
// SIMD Type Definitions
// =============================================================================

/**
 * @brief 128-bit vector of four 32-bit floating-point values (ARM NEON, x86
 * SSE).
 * Provides multiple access patterns for different use cases:
 * - Mathematical: x, y, z, w components
 * - Color: r, g, b, a channels
 * - Texture: s, t, p, q coordinates
 * - Array: elements[0-3] for indexed access
 * - Native: .neon for direct ARM NEON intrinsic access
 * - Native: .sse for direct x86 SSE intrinsic access
 */
typedef SIMD_ALIGN union {
#if SIMD_ARM_NEON
  float32x4_t neon; /**< Native ARM NEON vector register */
#elif SIMD_X86_AVX
  __m128 sse; /**< Native x86 SSE vector register */
#else
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
} SIMD_F32X4;

/**
 * @brief 128-bit vector of four 32-bit signed integers (ARM NEON).
 * Used for integer vector operations, masks, and bit manipulation.
 */
typedef SIMD_ALIGN union {
#if SIMD_ARM_NEON
  int32x4_t neon; /**< Native ARM NEON integer vector register */
#elif SIMD_X86_AVX
  __m128i sse; /**< Native x86 SSE integer vector register */
#else
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
} SIMD_I32X4;

// =============================================================================
// General-Purpose SIMD Operations
// =============================================================================

// =============================================================================
// SIMD Operations for float32_t vectors
// =============================================================================

/**
 * @brief Loads four 32-bit floats from aligned memory into a SIMD vector.
 * @param ptr Pointer to memory containing four consecutive floats.
 *            Should be 16-byte aligned for optimal performance.
 * @return SIMD vector containing the loaded values.
 * @note Undefined behavior if ptr is null or points to invalid memory.
 */
static INLINE SIMD_F32X4 simd_load_f32x4(const float32_t *ptr);

/**
 * @brief Stores a SIMD vector to aligned memory as four 32-bit floats.
 * @param ptr Pointer to memory where four consecutive floats will be stored.
 *            Should be 16-byte aligned for optimal performance.
 * @param v SIMD vector to store.
 * @note Undefined behavior if ptr is null or points to invalid memory.
 */
static INLINE void simd_store_f32x4(float32_t *ptr, SIMD_F32X4 v);

/**
 * @brief Creates a SIMD vector from four individual float values.
 * @param x First component (X/Red/S coordinate).
 * @param y Second component (Y/Green/T coordinate).
 * @param z Third component (Z/Blue/P coordinate).
 * @param w Fourth component (W/Alpha/Q coordinate).
 * @return SIMD vector with the specified component values.
 */
static INLINE SIMD_F32X4 simd_set_f32x4(float32_t x, float32_t y, float32_t z,
                                        float32_t w);

/**
 * @brief Creates a SIMD vector with all four components set to the same value.
 * @param value The value to broadcast to all four components.
 * @return SIMD vector with all components equal to value.
 */
static INLINE SIMD_F32X4 simd_set1_f32x4(float32_t value);

/**
 * @brief Performs element-wise addition of two SIMD vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}.
 */
static INLINE SIMD_F32X4 simd_add_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Performs element-wise subtraction of two SIMD vectors.
 * @param a First vector operand (minuend).
 * @param b Second vector operand (subtrahend).
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}.
 */
static INLINE SIMD_F32X4 simd_sub_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Performs element-wise multiplication of two SIMD vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}.
 */
static INLINE SIMD_F32X4 simd_mul_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Performs element-wise division of two SIMD vectors.
 * @param a First vector operand (dividend).
 * @param b Second vector operand (divisor).
 * @return Vector containing {a.x/b.x, a.y/b.y, a.z/b.z, a.w/b.w}.
 * @note Division by zero behavior is platform-dependent.
 */
static INLINE SIMD_F32X4 simd_div_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Computes the square root of each element in the vector.
 * @param v Input vector.
 * @return Vector containing {sqrt(v.x), sqrt(v.y), sqrt(v.z), sqrt(v.w)}.
 * @note Square root of negative values is platform-dependent.
 */
static INLINE SIMD_F32X4 simd_sqrt_f32x4(SIMD_F32X4 v);

/**
 * @brief Computes the reciprocal square root (1/sqrt) of each element.
 * @param v Input vector.
 * @return Vector containing {1/sqrt(v.x), 1/sqrt(v.y), 1/sqrt(v.z),
 * 1/sqrt(v.w)}.
 * @note On ARM NEON, uses Newton-Raphson iteration for improved precision.
 * @note Reciprocal square root of zero or negative values is
 * platform-dependent.
 */
static INLINE SIMD_F32X4 simd_rsqrt_f32x4(SIMD_F32X4 v);

/**
 * @brief Computes the element-wise minimum of two vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {min(a.x,b.x), min(a.y,b.y), min(a.z,b.z),
 * min(a.w,b.w)}.
 */
static INLINE SIMD_F32X4 simd_min_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Computes the element-wise maximum of two vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {max(a.x,b.x), max(a.y,b.y), max(a.z,b.z),
 * max(a.w,b.w)}.
 */
static INLINE SIMD_F32X4 simd_max_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Performs fused multiply-add operation: a + (b * c).
 * @param a Addend vector.
 * @param b First multiplicand vector.
 * @param c Second multiplicand vector.
 * @return Vector containing {a.x+(b.x*c.x), a.y+(b.y*c.y), a.z+(b.z*c.z),
 * a.w+(b.w*c.w)}.
 * @note Uses hardware FMA on ARM NEON for improved precision and performance.
 */
static INLINE SIMD_F32X4 simd_fma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c);

/**
 * @brief Performs fused multiply-subtract operation: a - (b * c).
 * @param a Minuend vector.
 * @param b First multiplicand vector.
 * @param c Second multiplicand vector.
 * @return Vector containing {a.x-(b.x*c.x), a.y-(b.y*c.y), a.z-(b.z*c.z),
 * a.w-(b.w*c.w)}.
 * @note Uses hardware FMA on ARM NEON for improved precision and performance.
 */
static INLINE SIMD_F32X4 simd_fms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c);

/**
 * @brief Performs negated fused multiply-add operation: -(a + b * c).
 * @param a Addend vector.
 * @param b First multiplicand vector.
 * @param c Second multiplicand vector.
 * @return Vector containing {-(a.x+b.x*c.x), -(a.y+b.y*c.y), -(a.z+b.z*c.z),
 * -(a.w+b.w*c.w)}.
 * @note Uses hardware FMA on ARM NEON for improved precision and performance.
 */
static INLINE SIMD_F32X4 simd_fnma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c);

/**
 * @brief Performs negated fused multiply-subtract operation: -(a - b * c).
 * @param a Minuend vector.
 * @param b First multiplicand vector.
 * @param c Second multiplicand vector.
 * @return Vector containing {-(a.x-b.x*c.x), -(a.y-b.y*c.y), -(a.z-b.z*c.z),
 * -(a.w-b.w*c.w)}.
 * @note Uses hardware FMA on ARM NEON for improved precision and performance.
 */
static INLINE SIMD_F32X4 simd_fnms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c);

/**
 * @brief Computes the horizontal sum of all elements in the vector.
 * @param v Input vector.
 * @return Single float containing v.x + v.y + v.z + v.w.
 * @note Useful for reduction operations and computing vector magnitudes.
 */
static INLINE float32_t simd_hadd_f32x4(SIMD_F32X4 v);

/**
 * @brief Computes the 4D dot product of two vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Single float containing a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w.
 * @note Optimized with hardware acceleration on supported platforms.
 */
static INLINE float32_t simd_dot_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Computes the 3D dot product of two vectors (ignores W component).
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Single float containing a.x*b.x + a.y*b.y + a.z*b.z.
 * @note Optimized for 3D vector operations, commonly used in graphics.
 */
static INLINE float32_t simd_dot3_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Computes the 4D dot product of two vectors (alias for clarity).
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Single float containing a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w.
 * @note Identical to simd_dot_f32x4(), provided for API consistency.
 */
static INLINE float32_t simd_dot4_f32x4(SIMD_F32X4 a, SIMD_F32X4 b);

/**
 * @brief Shuffles vector elements according to the specified indices.
 * @param v Input vector to shuffle.
 * @param x Index (0-3) for the first output element.
 * @param y Index (0-3) for the second output element.
 * @param z Index (0-3) for the third output element.
 * @param w Index (0-3) for the fourth output element.
 * @return Vector with elements rearranged: {v[x], v[y], v[z], v[w]}.
 * @note In debug builds, asserts that all indices are in range [0,3].
 * @note ARM NEON implementation uses element access due to limited shuffle
 * support.
 */
static INLINE SIMD_F32X4 simd_shuffle_f32x4(SIMD_F32X4 v, int32_t x, int32_t y,
                                            int32_t z, int32_t w);

// =============================================================================
// SIMD Operations for int32_t vectors
// =============================================================================

/**
 * @brief Creates a SIMD integer vector from four individual int32_t values.
 * @param x First component (X/Red/S coordinate).
 * @param y Second component (Y/Green/T coordinate).
 * @param z Third component (Z/Blue/P coordinate).
 * @param w Fourth component (W/Alpha/Q coordinate).
 * @return SIMD integer vector with the specified component values.
 */
static INLINE SIMD_I32X4 simd_set_i32x4(int32_t x, int32_t y, int32_t z,
                                        int32_t w);

/**
 * @brief Creates a SIMD integer vector with all four components set to the
 * same value.
 * @param value The value to broadcast to all four components.
 * @return SIMD integer vector with all components equal to value.
 */
static INLINE SIMD_I32X4 simd_set1_i32x4(int32_t value);

/**
 * @brief Performs element-wise addition of two SIMD integer vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}.
 */
static INLINE SIMD_I32X4 simd_add_i32x4(SIMD_I32X4 a, SIMD_I32X4 b);

/**
 * @brief Performs element-wise subtraction of two SIMD integer vectors.
 * @param a First vector operand (minuend).
 * @param b Second vector operand (subtrahend).
 * @return Vector containing {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}.
 */
static INLINE SIMD_I32X4 simd_sub_i32x4(SIMD_I32X4 a, SIMD_I32X4 b);

/**
 * @brief Performs element-wise multiplication of two SIMD integer vectors.
 * @param a First vector operand.
 * @param b Second vector operand.
 * @return Vector containing {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}.
 */
static INLINE SIMD_I32X4 simd_mul_i32x4(SIMD_I32X4 a, SIMD_I32X4 b);

// =============================================================================
// SIMD Operations Scatter-
// =============================================================================

/**
 * @brief Scatters the elements of a SIMD vector into specific positions based
 * on indices.
 * @param v Input vector to scatter.
 * @param indices Indices to scatter the elements into.
 * @return Vector with elements from v placed at positions specified by indices,
 * with out-of-bounds indices ignored and unwritten positions set to zero.
 */
static INLINE SIMD_F32X4 simd_scatter_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices);

/**
 * @brief Gathers elements from a SIMD vector at positions specified by indices.
 * @param v Input vector to gather from.
 * @param indices Indices to gather the elements from.
 * @return Vector with elements gathered from v at positions specified by
 * indices, with out-of-bounds indices producing zero elements.
 */
static INLINE SIMD_F32X4 simd_gather_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices);

// =============================================================================
// Platform-specific implementations
// =============================================================================

// Include platform-specific static INLINE implementations
#if SIMD_ARM_NEON

// ARM NEON static INLINE implementations
static INLINE SIMD_F32X4 simd_load_f32x4(const float32_t *ptr) {
  SIMD_F32X4 result;
  result.neon = vld1q_f32(ptr);
  return result;
}

static INLINE void simd_store_f32x4(float32_t *ptr, SIMD_F32X4 v) {
  vst1q_f32(ptr, v.neon);
}

static INLINE SIMD_F32X4 simd_set_f32x4(float32_t x, float32_t y, float32_t z,
                                        float32_t w) {
  SIMD_F32X4 result;
  result.neon = (float32x4_t){x, y, z, w};
  return result;
}

static INLINE SIMD_F32X4 simd_set1_f32x4(float32_t value) {
  SIMD_F32X4 result;
  result.neon = vdupq_n_f32(value);
  return result;
}

static INLINE SIMD_F32X4 simd_add_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vaddq_f32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_sub_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vsubq_f32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_mul_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vmulq_f32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_div_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vdivq_f32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_sqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result;
  result.neon = vsqrtq_f32(v.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_rsqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result;
  result.neon = vrsqrteq_f32(v.neon);
  // One Newton-Raphson iteration for better precision
  result.neon = vmulq_f32(
      result.neon, vrsqrtsq_f32(vmulq_f32(v.neon, result.neon), result.neon));
  return result;
}

static INLINE SIMD_F32X4 simd_min_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vminq_f32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_max_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.neon = vmaxq_f32(a.neon, b.neon);
  return result;
}

// ARM NEON FMA implementations
static INLINE SIMD_F32X4 simd_fma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.neon = vfmaq_f32(a.neon, b.neon, c.neon); // a + (b * c)
  return result;
}

static INLINE SIMD_F32X4 simd_fms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.neon = vfmsq_f32(a.neon, b.neon, c.neon); // a - (b * c)
  return result;
}

static INLINE SIMD_F32X4 simd_fnma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result;
  // -(a + b * c) = -a - (b * c)
  result.neon = vfmsq_f32(vnegq_f32(a.neon), b.neon, c.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_fnms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result;
  // -(a - b * c) = -a + (b * c)
  result.neon = vfmaq_f32(vnegq_f32(a.neon), b.neon, c.neon);
  return result;
}

static INLINE float32_t simd_hadd_f32x4(SIMD_F32X4 v) {
  return vaddvq_f32(v.neon);
}

// Optimized dot product using FMA and pairwise operations
static INLINE float32_t simd_dot_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  // Multiply the vectors element-wise
  float32x4_t prod = vmulq_f32(a.neon, b.neon);
  // Sum all elements of the result vector
  return vaddvq_f32(prod);
}

// 3D dot product (optimized for vec3 stored in vec4)
static INLINE float32_t simd_dot3_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  float32x4_t prod = vmulq_f32(a.neon, b.neon);
  float32x2_t sum = vpadd_f32(vget_low_f32(prod), vget_low_f32(prod));
  return vget_lane_f32(sum, 0) + vgetq_lane_f32(prod, 2);
}

// 4D dot product (alias for clarity)
static INLINE float32_t simd_dot4_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  return simd_dot_f32x4(a, b);
}

static INLINE SIMD_F32X4 simd_shuffle_f32x4(SIMD_F32X4 v, int32_t x, int32_t y,
                                            int32_t z, int32_t w) {
  assert(x >= 0 && x < 4);
  assert(y >= 0 && y < 4);
  assert(z >= 0 && z < 4);
  assert(w >= 0 && w < 4);
  // ARM NEON doesn't have arbitrary shuffle, so we use element access
  SIMD_F32X4 result;
  result.elements[0] = v.elements[x];
  result.elements[1] = v.elements[y];
  result.elements[2] = v.elements[z];
  result.elements[3] = v.elements[w];
  return result;
}

static INLINE SIMD_I32X4 simd_set_i32x4(int32_t x, int32_t y, int32_t z,
                                        int32_t w) {
  SIMD_I32X4 result;
  result.neon = (int32x4_t){x, y, z, w};
  return result;
}

static INLINE SIMD_I32X4 simd_set1_i32x4(int32_t value) {
  SIMD_I32X4 result;
  result.neon = vdupq_n_s32(value);
  return result;
}

static INLINE SIMD_I32X4 simd_add_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.neon = vaddq_s32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_I32X4 simd_sub_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.neon = vsubq_s32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_I32X4 simd_mul_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.neon = vmulq_s32(a.neon, b.neon);
  return result;
}

static INLINE SIMD_F32X4 simd_scatter_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  // ARM NEON doesn't have direct scatter, so we use element access
  // This creates a result vector where each element from v is placed at the
  // position specified by the corresponding index (with bounds checking)
  SIMD_F32X4 result = simd_set1_f32x4(0.0f); // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

static INLINE SIMD_F32X4 simd_gather_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  // ARM NEON doesn't have direct gather, so we use element access
  // This gathers elements from v using indices to specify which elements to
  // pick
  SIMD_F32X4 result;

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

#elif defined(SIMD_X86_AVX)

static INLINE SIMD_F32X4 simd_load_f32x4(const float32_t *ptr) {
  SIMD_F32X4 result;
  result.sse = _mm_loadu_ps(ptr);
  return result;
}

static INLINE void simd_store_f32x4(float32_t *ptr, SIMD_F32X4 v) {
  _mm_storeu_ps(ptr, v.sse);
}

static INLINE SIMD_F32X4 simd_set_f32x4(float32_t x, float32_t y, float32_t z,
                                        float32_t w) {
  SIMD_F32X4 result;
  result.sse = _mm_set_ps(w, z, y, x);
  return result;
}

static INLINE SIMD_F32X4 simd_set1_f32x4(float32_t value) {
  SIMD_F32X4 result;
  result.sse = _mm_set1_ps(value);
  return result;
}

static INLINE SIMD_F32X4 simd_add_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_add_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_sub_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_sub_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_mul_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_mul_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_div_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_div_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_sqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result;
  result.sse = _mm_sqrt_ps(v.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_rsqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result;
  result.sse = _mm_rsqrt_ps(v.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_min_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_min_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_max_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result;
  result.sse = _mm_max_ps(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_fma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.sse = _mm_fmadd_ps(a.sse, b.sse, c.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_fms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.sse = _mm_fmsub_ps(a.sse, b.sse, c.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_fnma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.sse = _mm_fnmadd_ps(a.sse, b.sse, c.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_fnms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result;
  result.sse = _mm_fnmsub_ps(a.sse, b.sse, c.sse);
  return result;
}

static INLINE float32_t simd_hadd_f32x4(SIMD_F32X4 v) {
  return _mm_cvtss_f32(_mm_hadd_ps(v.sse, v.sse));
}

static INLINE float32_t simd_dot_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0xFF));
}

static INLINE float32_t simd_dot3_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0xF1));
}

static INLINE float32_t simd_dot4_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  return _mm_cvtss_f32(_mm_dp_ps(a.sse, b.sse, 0xFF));
}

static INLINE SIMD_F32X4 simd_shuffle_f32x4(SIMD_F32X4 v, int32_t x, int32_t y,
                                            int32_t z, int32_t w) {
  // _mm_shuffle_ps requires a compile-time constant for the control mask,
  // so we must do the shuffle manually for variable indices.
  SIMD_F32X4 result;
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

static INLINE SIMD_I32X4 simd_set_i32x4(int32_t x, int32_t y, int32_t z,
                                        int32_t w) {
  SIMD_I32X4 result;
  result.sse = _mm_set_epi32(w, z, y, x);
  return result;
}

static INLINE SIMD_I32X4 simd_set1_i32x4(int32_t value) {
  SIMD_I32X4 result;
  result.sse = _mm_set1_epi32(value);
  return result;
}

static INLINE SIMD_I32X4 simd_add_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.sse = _mm_add_epi32(a.sse, b.sse);
  return result;
}

static INLINE SIMD_I32X4 simd_sub_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.sse = _mm_sub_epi32(a.sse, b.sse);
  return result;
}

static INLINE SIMD_I32X4 simd_mul_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result;
  result.sse = _mm_mullo_epi32(a.sse, b.sse);
  return result;
}

static INLINE SIMD_F32X4 simd_scatter_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  SIMD_F32X4 result = simd_set1_f32x4(0.0f); // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

static INLINE SIMD_F32X4 simd_gather_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  SIMD_F32X4 result;

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
#else
// Fallback scalar implementations
static INLINE SIMD_F32X4 simd_load_f32x4(const float32_t *ptr) {
  SIMD_F32X4 result = {{ptr[0], ptr[1], ptr[2], ptr[3]}};
  return result;
}

static INLINE void simd_store_f32x4(float32_t *ptr, SIMD_F32X4 v) {
  ptr[0] = v.x;
  ptr[1] = v.y;
  ptr[2] = v.z;
  ptr[3] = v.w;
}

static INLINE SIMD_F32X4 simd_set_f32x4(float32_t x, float32_t y, float32_t z,
                                        float32_t w) {
  SIMD_F32X4 result = {{x, y, z, w}};
  return result;
}

static INLINE SIMD_F32X4 simd_set1_f32x4(float32_t value) {
  SIMD_F32X4 result = {{value, value, value, value}};
  return result;
}

static INLINE SIMD_F32X4 simd_add_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}};
  return result;
}

static INLINE SIMD_F32X4 simd_sub_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}};
  return result;
}

static INLINE SIMD_F32X4 simd_mul_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}};
  return result;
}

static INLINE SIMD_F32X4 simd_div_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}};
  return result;
}

static INLINE SIMD_F32X4 simd_sqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result = {
      {sqrt_f32(v.x), sqrt_f32(v.y), sqrt_f32(v.z), sqrt_f32(v.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_rsqrt_f32x4(SIMD_F32X4 v) {
  SIMD_F32X4 result = {{1.0f / sqrt_f32(v.x), 1.0f / sqrt_f32(v.y),
                        1.0f / sqrt_f32(v.z), 1.0f / sqrt_f32(v.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_min_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {
      {Min(a.x, b.x), Min(a.y, b.y), Min(a.z, b.z), Min(a.w, b.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_max_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  SIMD_F32X4 result = {
      {Max(a.x, b.x), Max(a.y, b.y), Max(a.z, b.z), Max(a.w, b.w)}};
  return result;
}

// Fallback FMA implementations
static INLINE SIMD_F32X4 simd_fma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result = {{a.x + (b.x * c.x), a.y + (b.y * c.y), a.z + (b.z * c.z),
                        a.w + (b.w * c.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_fms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                        SIMD_F32X4 c) {
  SIMD_F32X4 result = {{a.x - (b.x * c.x), a.y - (b.y * c.y), a.z - (b.z * c.z),
                        a.w - (b.w * c.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_fnma_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result = {{-(a.x + b.x * c.x), -(a.y + b.y * c.y),
                        -(a.z + b.z * c.z), -(a.w + b.w * c.w)}};
  return result;
}

static INLINE SIMD_F32X4 simd_fnms_f32x4(SIMD_F32X4 a, SIMD_F32X4 b,
                                         SIMD_F32X4 c) {
  SIMD_F32X4 result = {{-(a.x - b.x * c.x), -(a.y - b.y * c.y),
                        -(a.z - b.z * c.z), -(a.w - b.w * c.w)}};
  return result;
}

static INLINE float32_t simd_hadd_f32x4(SIMD_F32X4 v) {
  return v.x + v.y + v.z + v.w;
}

// Optimized fallback dot product - hint to compiler for FMA
static INLINE float32_t simd_dot_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  // Structure to encourage compiler FMA generation
  float result = a.x * b.x;
  result += a.y * b.y; // Compiler may generate FMA here
  result += a.z * b.z; // And here
  result += a.w * b.w; // And here
  return result;
}

// Fallback 3D dot product
static INLINE float32_t simd_dot3_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  float result = a.x * b.x;
  result += a.y * b.y;
  result += a.z * b.z;
  // Ignore w component
  return result;
}

// Fallback 4D dot product (alias)
static INLINE float32_t simd_dot4_f32x4(SIMD_F32X4 a, SIMD_F32X4 b) {
  return simd_dot_f32x4(a, b);
}

static INLINE SIMD_F32X4 simd_shuffle_f32x4(SIMD_F32X4 v, int32_t x, int32_t y,
                                            int32_t z, int32_t w) {
  assert(x >= 0 && x < 4);
  assert(y >= 0 && y < 4);
  assert(z >= 0 && z < 4);
  assert(w >= 0 && w < 4);
  SIMD_F32X4 result = {
      {v.elements[x], v.elements[y], v.elements[z], v.elements[w]}};
  return result;
}

static INLINE SIMD_I32X4 simd_set_i32x4(int32_t x, int32_t y, int32_t z,
                                        int32_t w) {
  SIMD_I32X4 result = {{x, y, z, w}};
  return result;
}

static INLINE SIMD_I32X4 simd_set1_i32x4(int32_t value) {
  SIMD_I32X4 result = {{value, value, value, value}};
  return result;
}

static INLINE SIMD_I32X4 simd_add_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result = {{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}};
  return result;
}

static INLINE SIMD_I32X4 simd_sub_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result = {{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}};
  return result;
}

static INLINE SIMD_I32X4 simd_mul_i32x4(SIMD_I32X4 a, SIMD_I32X4 b) {
  SIMD_I32X4 result = {{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}};
  return result;
}

static INLINE SIMD_F32X4 simd_scatter_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  // Scalar fallback for scatter operation
  SIMD_F32X4 result = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Initialize to zero

  for (int i = 0; i < 4; i++) {
    int32_t idx = indices.elements[i];
    if (idx >= 0 && idx < 4) {
      result.elements[idx] = v.elements[i];
    }
  }
  return result;
}

static INLINE SIMD_F32X4 simd_gather_f32x4(SIMD_F32X4 v, SIMD_I32X4 indices) {
  // Scalar fallback for gather operation
  SIMD_F32X4 result;

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
#endif