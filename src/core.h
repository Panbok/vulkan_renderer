#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGN_UP(x, b) (((x) + (b) - 1) & (~((b) - 1)))
#define ClampTop(x, y) ((x) < (y) ? (y) : (x))
#define ClampBot(x, y) ((x) > (y) ? (y) : (x))

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
