/**
 * @file vkr_pool_allocator.h
 * @brief Adapter that wires VkrPool into the generic VkrAllocator interface.
 */

#pragma once

#include "vkr_allocator.h"

/**
 * @brief Initializes a VkrAllocator to use a VkrPool context.
 * @param out_allocator Allocator to initialize. Its ctx must point to a valid
 * VkrPool instance.
 */
void vkr_pool_allocator_create(VkrAllocator *out_allocator);

/**
 * @brief Destroys a pool allocator (also destroys the underlying VkrPool).
 * @param allocator Allocator to destroy.
 */
void vkr_pool_allocator_destroy(VkrAllocator *allocator);
