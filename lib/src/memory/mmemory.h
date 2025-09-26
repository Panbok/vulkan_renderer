// clang-format off

/**
 * @file mmemory.h
 * @brief Defines a virtual memory management system for tracking allocated memory blocks.
 *
 * The MMemory allocator provides a layer of abstraction over the platform's virtual memory
 * system, allowing efficient management of memory blocks while tracking their usage.
 * It handles reserving, committing, and releasing memory using the platform's virtual memory APIs.
 *
 * Memory Block Management:
 * Each allocated block is recorded in an MBlock structure, which tracks:
 * - The pointer to the allocated memory
 * - The size requested by the user
 * - The actual reserved size (aligned to page boundaries)
 * - Whether the block is currently in use
 *
 * Memory Layout:
 * +------------------+
 * | MMemory          |   The allocator structure containing metadata
 * | - blocks array   | --+
 * | - count/capacity |   |
 * | - page_size      |   |
 * +------------------+   |
 *                        |
 *                        v
 * +------------------+ <-- Blocks array (dynamically sized)
 * | MBlock[0]        |     Active and inactive block tracking
 * +------------------+
 * | MBlock[1]        |
 * +------------------+
 * |       ...        |
 * +------------------+
 * | MBlock[capacity-1]
 * +------------------+
 *
 * For each allocation:
 * +------------------+ <-- Block returned by mem_reserve/mem_commit
 * |                  |     
 * | User data        |     The memory available for the user to use
 * | (usr_size bytes) |     
 * |                  |     
 * +------------------+ <-- Potentially extra reserved space for page alignment
 * | (padding)        |     Not used by the user, but reserved for alignment
 * +------------------+
 *
 * Key Operations:
 * - Creation/Destruction: Initialize and clean up the allocator
 * - Allocation: Reserve and commit memory blocks
 * - Reallocation: Resize existing allocations
 * - Deallocation: Track freed blocks
 * - Querying: Get information about allocated blocks
 */

// clang-format on
#pragma once

#include "containers/bitset.h"
#include "core/logger.h"
#include "defines.h"
#include "platform/vkr_platform.h"
#include "vkr_pch.h"

/**
 * @brief Represents a single memory block managed by the MMemory allocator.
 */
typedef struct MBlock {
  void *ptr;         /**< Pointer returned by mem_reserve/mem_commit */
  uint64_t usr_size; /**< Size requested by the user */
  uint64_t rsv_size; /**< Actual size reserved (multiple of page size) */
  bool32_t is_used;  /**< Is this slot currently tracking an active block? */
} MBlock;

/**
 * @brief Memory allocator that manages and tracks memory blocks.
 */
typedef struct MMemory {
  MBlock *blocks;     /**< Array of blocks */
  uint64_t count;     /**< Number of blocks currently in use */
  uint64_t capacity;  /**< Capacity of the blocks array */
  uint64_t page_size; /**< System page size for alignment */
} MMemory;

/**
 * @brief Creates a new memory allocator.
 *
 * Initializes the memory allocator with the specified capacity for tracking
 * memory blocks. The capacity refers to how many blocks the allocator can
 * track, not the total memory size that can be allocated.
 *
 * @param capacity Initial capacity for number of blocks the allocator can
 * track.
 * @param out_allocator Pointer to store the created allocator.
 * @return true if creation was successful, false otherwise.
 */
bool32_t mmemory_create(uint64_t capacity, MMemory *out_allocator);

/**
 * @brief Destroys a memory allocator.
 *
 * Cleans up all resources used by the allocator, including freeing
 * the blocks array. Does not release memory allocated through the allocator,
 * which should be done before calling this function.
 *
 * @param allocator The allocator to destroy.
 */
void mmemory_destroy(MMemory *allocator);

/**
 * @brief Allocates memory from the allocator.
 *
 * Reserves and commits virtual memory for the requested size, tracks it
 * in the allocator's blocks array, and returns a pointer to the allocated
 * memory. Memory is reserved in page-size aligned chunks.
 *
 * @param allocator The allocator to use.
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
void *mmemory_alloc(MMemory *allocator, uint64_t size);

/**
 * @brief Reallocates memory previously allocated with mmemory_alloc.
 *
 * If the new size fits within the existing reserved size, the original pointer
 * is returned. Otherwise, a new larger block is allocated, data is copied,
 * and the old block is freed.
 *
 * @param allocator The allocator to use.
 * @param ptr Pointer to memory previously allocated with mmemory_alloc.
 * @param size New size in bytes.
 * @return Pointer to the resized memory block, or NULL on failure.
 */
void *mmemory_realloc(MMemory *allocator, void *ptr, uint64_t size);

/**
 * @brief Frees memory previously allocated with mmemory_alloc.
 *
 * Marks the block as unused in the allocator's tracking system.
 * The actual memory is released by the platform.
 *
 * @param allocator The allocator to use.
 * @param ptr Pointer to memory previously allocated with mmemory_alloc.
 * @return true if the memory was successfully freed, false otherwise (e.g., if
 * ptr was not found).
 */
bool32_t mmemory_free(MMemory *allocator, void *ptr);

/**
 * @brief Gets the reserved size of a memory block.
 *
 * Returns the actual size (in bytes) of the memory block reserved by the
 * allocator. This may be larger than the size requested by the user due to page
 * size alignment.
 *
 * @param allocator The allocator to query.
 * @param ptr Pointer to memory previously allocated with mmemory_alloc.
 * @return The size of the block in bytes, or 0 if the pointer was not found.
 */
uint64_t mmemory_get_block_size(MMemory *allocator, void *ptr);
