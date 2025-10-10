/**
 * @file vkr_dmemory_allocator.h
 * @brief Defines the dynamic memory allocator using platform memory and
 * freelist tracking
 */

#pragma once

#include "vkr_allocator.h"

/**
 * @brief Creates a new dmemory allocator.
 * @param out_allocator Pointer to the allocator to create.
 */
void vkr_dmemory_allocator_create(VkrAllocator *out_allocator);

/**
 * @brief Destroys a dmemory allocator.
 * @param allocator Pointer to the allocator to destroy.
 */
void vkr_dmemory_allocator_destroy(VkrAllocator *allocator);