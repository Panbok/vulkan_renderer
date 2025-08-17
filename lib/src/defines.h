#pragma once

#include "pch.h"

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
#define MaxAlign() __BIGGEST_ALIGNMENT__
#elif defined(_MSC_VER)
#define MaxAlign() 16 // MSVC typically uses 16-byte max alignment
#else
#define MaxAlign() 16 // Most modern platforms use 16-byte max alignment
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

#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define debug_break() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define debug_break() __debugbreak()
#endif
#endif

#define true_v (uint8_t)1
#define false_v (uint8_t)0

#define vkr_global static
#define vkr_local_persist static
#define vkr_internal static

#define VKR_INVALID_OBJECT_ID 4294967295U

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
#if defined(SIMD_ARM_NEON) || defined(SIMD_X86_AVX)
#define SIMD_AVAILABLE 1
#else
#define SIMD_AVAILABLE 0
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