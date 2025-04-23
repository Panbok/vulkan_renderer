#pragma once

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_LEVEL 5

#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define debug_break() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define debug_break() __debugbreak()
#endif
#endif

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

void *mem_reserve(size_t size);
bool mem_commit(void *ptr, size_t size);
void mem_decommit(void *ptr, size_t size);
void mem_release(void *ptr, size_t size);

size_t get_page_size();
