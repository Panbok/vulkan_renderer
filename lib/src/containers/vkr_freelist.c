#include "containers/vkr_freelist.h"
#include "core/logger.h"
#include "defines.h"

vkr_internal VkrFreeListNode *vkr_freelist_get_node(VkrFreeList *freelist) {
  for (uint32_t node_index = 0; node_index < freelist->max_count;
       node_index++) {
    if (freelist->nodes[node_index].offset == VKR_INVALID_ID) {
      return &freelist->nodes[node_index];
    }
  }

  return NULL;
}

vkr_internal void vkr_return_node(VkrFreeList *freelist,
                                  VkrFreeListNode *node) {
  node->size = VKR_INVALID_ID;
  node->offset = VKR_INVALID_ID;
  node->next = NULL;
}

uint64_t vkr_freelist_calculate_memory_requirement(uint64_t total_size) {
  // Use a reasonable default: assume average block size of 4KB
  uint64_t max_count = (uint64_t)(total_size / 4096) + 16;
  if (max_count < 2) {
    max_count = 2; // ensure at least head + one additional node is available
  }

  if (max_count > 1024) {
    max_count = 1024;
  }

  uint64_t mem_size = (uint64_t)max_count * sizeof(VkrFreeListNode);
  return mem_size;
}

bool8_t vkr_freelist_create(void *memory, uint64_t memory_size,
                            uint64_t total_size, VkrFreeList *out_freelist) {
  assert_log(memory != NULL, "Memory must not be NULL");
  assert_log(memory_size > 0, "Memory size must be greater than 0");
  assert_log(total_size > 0, "Total size must be greater than 0");
  assert_log(out_freelist != NULL, "Output freelist must not be NULL");

  uint64_t max_count = memory_size / sizeof(VkrFreeListNode);
  if (max_count < 2) {
    log_error("Memory block too small for freelist (need at least 2 nodes)");
    return false_v;
  }

  if (max_count > 1024) {
    max_count = 1024;
  }

  out_freelist->memory = memory;
  out_freelist->total_size = total_size;
  out_freelist->max_count = (uint32_t)max_count;
  out_freelist->nodes_allocated_size = memory_size;
  out_freelist->nodes = (VkrFreeListNode *)memory;

  out_freelist->head = &out_freelist->nodes[0];
  out_freelist->head->size = total_size;
  out_freelist->head->offset = 0;
  out_freelist->head->next = NULL;

  for (uint32_t i = 1; i < max_count; i++) {
    out_freelist->nodes[i].size = VKR_INVALID_ID;
    out_freelist->nodes[i].offset = VKR_INVALID_ID;
    out_freelist->nodes[i].next = NULL;
  }

  return true_v;
}

void vkr_freelist_destroy(VkrFreeList *freelist) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");

  freelist->memory = NULL;
  freelist->total_size = 0;
  freelist->max_count = 0;
  freelist->nodes_allocated_size = 0;
  freelist->head = NULL;
  freelist->nodes = NULL;
  MemZero(freelist, sizeof(VkrFreeList));
}

bool8_t vkr_freelist_allocate(VkrFreeList *freelist, uint64_t size,
                              uint64_t *out_offset) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(out_offset != NULL, "Output offset must not be NULL");

  VkrFreeListNode *node = freelist->head;
  VkrFreeListNode *previous = NULL;
  while (node != NULL) {
    if (node->size == size) {
      *out_offset = node->offset;
      VkrFreeListNode *node_to_return = NULL;
      if (previous) {
        previous->next = node->next;
        node_to_return = node;
      } else {
        node_to_return = freelist->head;
        freelist->head = node->next;
      }
      vkr_return_node(freelist, node_to_return);
      return true_v;
    } else if (node->size > size) {
      *out_offset = node->offset;
      node->size -= size;
      node->offset += size;
      return true_v;
    }
    previous = node;
    node = node->next;
  }

  return false_v;
}

bool8_t vkr_freelist_free(VkrFreeList *freelist, uint64_t size,
                          uint64_t offset) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");
  if (offset == VKR_INVALID_ID) {
    log_error("Invalid offset passed to vkr_freelist_free");
    return false_v;
  }
  if (size == 0) {
    log_error("Zero size passed to vkr_freelist_free");
    return false_v;
  }
  if (offset + size > freelist->total_size) {
    log_error("Free block exceeds freelist range");
    return false_v;
  }

  // If the list is empty, insert as the head
  if (freelist->head == NULL) {
    VkrFreeListNode *new_node = vkr_freelist_get_node(freelist);
    if (new_node == NULL) {
      log_error("Freelist out of nodes while freeing into empty list");
      return false_v;
    }
    new_node->size = size;
    new_node->offset = offset;
    new_node->next = NULL;
    freelist->head = new_node;
    return true_v;
  }

  uint64_t block_start = offset;
  uint64_t block_end = offset + size;

  VkrFreeListNode *node = freelist->head;
  VkrFreeListNode *previous = NULL;

  // Find insertion point: first node with offset >= block_start
  while (node != NULL && node->offset < block_start) {
    previous = node;
    node = node->next;
  }

  // Detect overlap with previous
  if (previous) {
    uint64_t previous_end = previous->offset + previous->size;
    if (block_start < previous_end) {
      log_error("Free range overlaps with existing free block (prev)");
      return false_v;
    }
  }

  // Detect overlap with next
  if (node) {
    if (block_end > node->offset) {
      if (block_start == node->offset) {
        log_error("Double free detected at same offset");
      } else {
        log_error("Free range overlaps with existing free block");
      }
      return false_v;
    }
  }

  // Merge with previous if adjacent
  if (previous && (previous->offset + previous->size == block_start)) {
    previous->size += size;
    // Also merge with next if now adjacent
    if (node && (previous->offset + previous->size == node->offset)) {
      previous->size += node->size;
      previous->next = node->next;
      vkr_return_node(freelist, node);
    }
    return true_v;
  }

  // Merge with next if adjacent (block before node)
  if (node && (block_end == node->offset)) {
    node->offset = block_start;
    node->size += size;
    // If also adjacent to previous (should not happen due to earlier branch),
    // coalesce fully
    if (previous && (previous->offset + previous->size == node->offset)) {
      previous->size += node->size;
      previous->next = node->next;
      vkr_return_node(freelist, node);
    }
    return true_v;
  }

  // Otherwise insert a new node between previous and node
  VkrFreeListNode *new_node = vkr_freelist_get_node(freelist);
  if (new_node == NULL) {
    log_error("Freelist out of nodes while inserting new free block");
    return false_v;
  }
  new_node->size = size;
  new_node->offset = offset;
  new_node->next = node;
  if (previous) {
    previous->next = new_node;
  } else {
    freelist->head = new_node;
  }

  return true_v;
}

void vkr_freelist_clear(VkrFreeList *freelist) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");

  for (uint32_t i = 1; i < freelist->max_count; i++) {
    freelist->nodes[i].size = VKR_INVALID_ID;
    freelist->nodes[i].offset = VKR_INVALID_ID;
    freelist->nodes[i].next = NULL;
  }

  freelist->head = &freelist->nodes[0];
  freelist->head->size = freelist->total_size;
  freelist->head->offset = 0;
  freelist->head->next = NULL;
}

uint64_t vkr_freelist_free_space(VkrFreeList *freelist) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");

  uint64_t free_space = 0;
  VkrFreeListNode *node = freelist->head;
  while (node != NULL) {
    free_space += node->size;
    node = node->next;
  }

  return free_space;
}