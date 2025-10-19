#pragma once

#include "arena.h"
#include "vkr_allocator.h"

/**
 * @brief Initializes an arena allocator.
 * @param out_allocator Pointer to the allocator to initialize.
 * @note The arena context must be set in out_allocator->ctx before calling this
 * function. The caller retains ownership of the arena and must ensure it
 * outlives the allocator.
 */
bool8_t vkr_allocator_arena(VkrAllocator *out_allocator);