#pragma once

#if defined(__APPLE__)
// Apple platforms
#define PLATFORM_APPLE 1
#include <TargetConditionals.h>
#endif

#define LOG_LEVEL 5
#define ASSERT_LOG 1

#define AlginPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))
#define AlignOf(T) __alignof(T)

#define Min(A, B) (((A) < (B)) ? (A) : (B))
#define Max(A, B) (((A) > (B)) ? (A) : (B))
#define ClampTop(A, X) Min(A, X)
#define ClampBot(X, B) Max(X, B)

#define KB(x) ((x) * 1024)
#define MB(x) ((x) * 1024 * 1024)

#define SingleListAppend(f, n, next) ((n)->next = (f), (f) = (n))
#define SingleListPop(f, n) ((f) = (n)->next)

#define MemCopy(dst, src, size) memmove((dst), (src), (size))
#define MemZero(dst, size) memset((dst), 0, (size))

#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define debug_break() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define debug_break() __debugbreak()
#endif
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