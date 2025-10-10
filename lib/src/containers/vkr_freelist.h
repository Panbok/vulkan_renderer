#pragma once

#include "defines.h"

typedef struct VkrFreeListNode {
  uint64_t size;
  uint64_t offset;
  struct VkrFreeListNode *next;
} VkrFreeListNode;

typedef struct VkrFreeList {
  void *memory; // Raw memory block for storing nodes

  uint64_t total_size; // Total size of the address space being tracked
  uint32_t max_count;  // Maximum number of nodes available
  uint64_t nodes_allocated_size; // Size of the memory block for nodes

  VkrFreeListNode *head;
  VkrFreeListNode *nodes;
} VkrFreeList;

/**
 * @brief Creates a freelist using a provided raw memory block for node storage
 * @param memory Raw memory block to store freelist nodes (must be large enough)
 * @param memory_size Size of the provided memory block in bytes
 * @param total_size Total size of the address space to track
 * @param out_freelist Output freelist structure
 * @return true if successful, false otherwise
 */
bool8_t vkr_freelist_create(void *memory, uint64_t memory_size,
                            uint64_t total_size, VkrFreeList *out_freelist);

/**
 * @brief Destroys a freelist (clears state, does not free memory)
 * @param freelist The freelist to destroy
 */
void vkr_freelist_destroy(VkrFreeList *freelist);

/**
 * @brief Allocates a block from the freelist
 * @param freelist The freelist to allocate from
 * @param size Size of the block to allocate
 * @param out_offset Output offset of the allocated block
 * @return true if allocation succeeded, false otherwise
 */
bool8_t vkr_freelist_allocate(VkrFreeList *freelist, uint64_t size,
                              uint64_t *out_offset);

/**
 * @brief Frees a block back to the freelist
 * @param freelist The freelist to free to
 * @param size Size of the block to free
 * @param offset Offset of the block to free
 * @return true if free succeeded, false otherwise
 */
bool8_t vkr_freelist_free(VkrFreeList *freelist, uint64_t size,
                          uint64_t offset);

/**
 * @brief Clears the freelist, marking all space as free
 * @param freelist The freelist to clear
 */
void vkr_freelist_clear(VkrFreeList *freelist);

/**
 * @brief Gets the total free space in the freelist
 * @param freelist The freelist to query
 * @return Total free space in bytes
 */
uint64_t vkr_freelist_free_space(VkrFreeList *freelist);

/**
 * @brief Calculates the required memory size for freelist node storage
 * @param total_size Total size of address space to track
 * @return Required memory size in bytes for node storage
 */
uint64_t vkr_freelist_calculate_memory_requirement(uint64_t total_size);
