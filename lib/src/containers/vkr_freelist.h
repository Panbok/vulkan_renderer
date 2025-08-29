#pragma once

#include "core/logger.h"
#include "memory/arena.h"

typedef struct VkrFreeListNode {
  uint32_t size;
  uint32_t offset;

  struct VkrFreeListNode *next;
} VkrFreeListNode;

typedef struct VkrFreeList {
  Arena *arena;
  void *memory;

  uint32_t total_size;
  uint32_t max_count;
  // Number of bytes allocated for the internal nodes buffer pointed to by
  // `memory`. Used for proper cleanup/reset.
  uint64_t nodes_allocated_size;

  VkrFreeListNode *head;
  VkrFreeListNode *nodes;
} VkrFreeList;

bool8_t vkr_freelist_create(Arena *arena, uint32_t total_size,
                            VkrFreeList *out_freelist);

void vkr_freelist_destroy(VkrFreeList *freelist);

bool8_t vkr_freelist_allocate(VkrFreeList *freelist, uint32_t size,
                              uint32_t *out_offset);

bool8_t vkr_freelist_free(VkrFreeList *freelist, uint32_t size,
                          uint32_t offset);

void vkr_freelist_clear(VkrFreeList *freelist);

uint64_t vkr_freelist_free_space(VkrFreeList *freelist);
