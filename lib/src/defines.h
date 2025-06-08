#pragma once

#define LOG_LEVEL 5
#define ASSERT_LOG 1

#define AlignPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))
#define AlignPow2Down(x, b)                                                    \
  ((x) & (~((b) - 1))) // Align x down to multiple of b (b must be power of 2)
#define AlignOf(T) __alignof(T)

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

#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define debug_break() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define debug_break() __debugbreak()
#endif
#endif

#define true_v (uint8_t)1
#define false_v (uint8_t)0

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