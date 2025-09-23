#pragma once

#include "arena.h"
#include "vkr_allocator.h"

/**
 * @brief Initializes an arena allocator.
 * @param out_allocator Pointer to the allocator to initialize.
 */
void vkr_allocator_arena(VkrAllocator *out_allocator);