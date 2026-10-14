#pragma once

#include "defines.h"

/**
 * The process heap (the C library's malloc), for blocks whose owner changes
 * across modules and threads.
 *
 * Use it only where a block crosses an ownership boundary that a VkrAllocator
 * cannot follow: decode workers that allocate in parallel without a shared
 * lock, payloads of hundreds of MB whose pages must return on release, or
 * blocks that vendor decoders allocate with malloc. Any thread may free a
 * block, and the free needs no size. Allocator tag accounting does not see
 * these bytes. Everything else uses a VkrAllocator backend (vkr-memory).
 */

/** Returns `size` bytes of uninitialized storage, or NULL. */
void *vkr_heap_alloc(uint64_t size);

/** Releases a block from vkr_heap_alloc or from vendor code that uses malloc.
 * A NULL block is ignored. */
void vkr_heap_free(void *block);
