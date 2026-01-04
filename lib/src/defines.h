#pragma once

#include "vkr_pch.h"

// Only define these if not already defined by CMake
#ifndef LOG_LEVEL
#define LOG_LEVEL 5
#endif

#ifndef ASSERT_LOG
#define ASSERT_LOG 1
#endif

#define AlignPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))
#define AlignPow2Down(x, b)                                                    \
  ((x) & (~((b) - 1))) // Align x down to multiple of b (b must be power of 2)
#define AlignOf(T) __alignof(T)
// Maximum fundamental alignment for the platform
// Use the compiler's __BIGGEST_ALIGNMENT__ if available, otherwise fall back to
// common values
#if defined(__BIGGEST_ALIGNMENT__)
#define MaxAlign()                                                             \
  (((__BIGGEST_ALIGNMENT__) < 16) ? 16 : (__BIGGEST_ALIGNMENT__))
#elif defined(_MSC_VER)
#define MaxAlign() 16 // MSVC typically uses 16-byte max alignment
#else
#define MaxAlign() 16 // Most modern platforms use 16-byte max alignment
#endif

// =============================================================================
// SIMD Alignment and Attributes
// =============================================================================

/**
 * @brief Alignment attribute for optimal SIMD performance.
 * Ensures 16-byte alignment required by most SIMD instruction sets.
 */
#if defined(_MSC_VER)
#define VKR_SIMD_ALIGN __declspec(align(16))
#else
#define VKR_SIMD_ALIGN __attribute__((aligned(16)))
#endif

#define Min(A, B) (((A) < (B)) ? (A) : (B))
#define Max(A, B) (((A) > (B)) ? (A) : (B))
#define ClampTop(A, X) Min(A, X)
#define ClampBot(X, B) Max(X, B)
#define Clamp(X, A, B) ClampTop(ClampBot(X, A), B)
#define ArrayCount(array) (sizeof(array) / sizeof(array[0]))

#define KB(x) ((x) * 1024ULL)
#define MB(x) ((x) * 1024ULL * 1024ULL)
#define GB(x) ((x) * 1024ULL * 1024ULL * 1024ULL)

#define SingleListAppend(f, n, next) ((n)->next = (f), (f) = (n))
#define SingleListPop(f, n) ((f) = (n)->next)

#define MemCopy(dst, src, size) memmove((dst), (src), (size))
#define MemZero(dst, size) memset((dst), 0, (size))
#define MemSet(dst, value, size) memset((dst), (value), (size))
#define MemCompare(a, b, size) memcmp((a), (b), (size))

#if !defined(NDEBUG) // Often debug_break is only active in debug builds
#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define debug_break() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define debug_break() __debugbreak()
#else
// Fallback for compilers without specific intrinsics (e.g., assembly for
// x86/x64)
#if defined(_MSC_VER)
#include <intrin.h> // For __debugbreak
#define debug_break() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define debug_break() __asm__("int $3") // For GCC/Clang on x86/x64
#else
#define debug_break() /* no-op or custom crash for other platforms */
#endif
#endif
#else
// Compilers without __has_builtin
#if defined(_MSC_VER)
#include <intrin.h> // For __debugbreak
#define debug_break() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define debug_break() __asm__("int $3")
#else
#define debug_break() /* no-op or custom crash */
#endif
#endif
#else                           // NDEBUG is defined (release build)
#define debug_break() ((void)0) // No-op in release builds
#endif

#define true_v (uint8_t)1
#define false_v (uint8_t)0

#define vkr_global static
#define vkr_local_persist static
#define vkr_internal static

#define VKR_INVALID_ID 4294967295U

// Count leading zeros for 32-bit integers - macro version
#if defined(__GNUC__) || defined(__clang__)
#define VkrCountLeadingZeros32(x) ((x) == 0 ? 32 : __builtin_clz(x))
#elif defined(_MSC_VER)
#define VkrCountLeadingZeros32(x)                                              \
  ((x) == 0 ? 32 : ({                                                          \
    unsigned long _clz_index;                                                  \
    _BitScanReverse(&_clz_index, (x));                                         \
    31 - (int)_clz_index;                                                      \
  }))
#else
// Fallback macro implementation
#define VkrCountLeadingZeros32(x)                                              \
  ((x) == 0 ? 32 : ({                                                          \
    uint32_t _clz_val = (x);                                                   \
    int _clz_count = 0;                                                        \
    if (_clz_val <= 0x0000FFFF) {                                              \
      _clz_count += 16;                                                        \
      _clz_val <<= 16;                                                         \
    }                                                                          \
    if (_clz_val <= 0x00FFFFFF) {                                              \
      _clz_count += 8;                                                         \
      _clz_val <<= 8;                                                          \
    }                                                                          \
    if (_clz_val <= 0x0FFFFFFF) {                                              \
      _clz_count += 4;                                                         \
      _clz_val <<= 4;                                                          \
    }                                                                          \
    if (_clz_val <= 0x3FFFFFFF) {                                              \
      _clz_count += 2;                                                         \
      _clz_val <<= 2;                                                          \
    }                                                                          \
    if (_clz_val <= 0x7FFFFFFF) {                                              \
      _clz_count += 1;                                                         \
    }                                                                          \
    _clz_count;                                                                \
  }))
#endif

// Count leading zeros for 64-bit integers - macro version
#if defined(__GNUC__) || defined(__clang__)
#define VkrCountLeadingZeros64(x) ((x) == 0 ? 64 : __builtin_clzll(x))
#elif defined(_MSC_VER)
#ifdef _WIN64
#define VkrCountLeadingZeros64(x)                                              \
  ((x) == 0 ? 64 : ({                                                          \
    unsigned long _clz_index;                                                  \
    _BitScanReverse64(&_clz_index, (x));                                       \
    63 - (int)_clz_index;                                                      \
  }))
#else
#define VkrCountLeadingZeros64(x)                                              \
  ((x) == 0 ? 64 : ({                                                          \
    uint64_t _clz_val = (x);                                                   \
    uint32_t _clz_high = (uint32_t)(_clz_val >> 32);                           \
    (_clz_high != 0) ? CountLeadingZeros32(_clz_high)                          \
                     : 32 + CountLeadingZeros32((uint32_t)_clz_val);           \
  }))
#endif
#else
// Fallback macro implementation for 64-bit
#define VkrCountLeadingZeros64(x)                                              \
  ((x) == 0 ? 64 : ({                                                          \
    uint64_t _clz_val = (x);                                                   \
    int _clz_count = 0;                                                        \
    if (_clz_val <= 0x00000000FFFFFFFFULL) {                                   \
      _clz_count += 32;                                                        \
      _clz_val <<= 32;                                                         \
    }                                                                          \
    if (_clz_val <= 0x0000FFFFFFFFFFFFULL) {                                   \
      _clz_count += 16;                                                        \
      _clz_val <<= 16;                                                         \
    }                                                                          \
    if (_clz_val <= 0x00FFFFFFFFFFFFFFULL) {                                   \
      _clz_count += 8;                                                         \
      _clz_val <<= 8;                                                          \
    }                                                                          \
    if (_clz_val <= 0x0FFFFFFFFFFFFFFFULL) {                                   \
      _clz_count += 4;                                                         \
      _clz_val <<= 4;                                                          \
    }                                                                          \
    if (_clz_val <= 0x3FFFFFFFFFFFFFFFULL) {                                   \
      _clz_count += 2;                                                         \
      _clz_val <<= 2;                                                          \
    }                                                                          \
    if (_clz_val <= 0x7FFFFFFFFFFFFFFFULL) {                                   \
      _clz_count += 1;                                                         \
    }                                                                          \
    _clz_count;                                                                \
  }))
#endif

// Generic macro that chooses the appropriate macro based on type size
#define VkrCountLeadingZeros(x)                                                \
  _Generic((x),                                                                \
      uint32_t: VkrCountLeadingZeros32(x),                                     \
      uint64_t: VkrCountLeadingZeros64(x),                                     \
      default: VkrCountLeadingZeros32(x))

// Inlining
#if defined(__clang__) || defined(__GNUC__)
// If NDEBUG is NOT defined (i.e., debug build), use plain 'inline'
// Otherwise (NDEBUG is defined, release/optimized build), use aggressive
// 'always_inline'
#ifndef NDEBUG
#define INLINE inline
#define NOINLINE __attribute__((noinline))
#else
#define INLINE __attribute__((always_inline)) inline
#define NOINLINE __attribute__((noinline))
#endif
#elif defined(_MSC_VER)
// If NDEBUG is NOT defined (i.e., debug build), use plain 'inline'
// Otherwise (NDEBUG is defined, release/optimized build), use aggressive
// '__forceinline'
#ifndef NDEBUG
#define INLINE inline
#define NOINLINE __declspec(noinline)
#else
#define INLINE __forceinline
#define NOINLINE __declspec(noinline)
#endif
#else
// Fallback for other compilers
#ifndef NDEBUG
#define INLINE inline
#define NOINLINE
#else
#define INLINE inline
#define NOINLINE
#endif
#endif

// Check if any SIMD is available
#if defined(VKR_SIMD_ARM_NEON) || defined(VKR_SIMD_X86_AVX)
#define VKR_SIMD_AVAILABLE 1
#else
#define VKR_SIMD_AVAILABLE 0
#endif

// Floating point types
/** @brief 32-bit floating point number */
typedef float float32_t;
/** @brief 64-bit floating point number */
typedef double float64_t;

// Boolean types
/** @brief 32-bit boolean type */
typedef int bool32_t;
/** @brief 8-bit boolean type */
typedef char bool8_t;