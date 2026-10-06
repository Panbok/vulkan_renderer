#include "containers/vkr_freelist.h"
#include "core/logger.h"
#include "defines.h"

vkr_internal VkrFreeListNode *vkr_freelist_get_node(VkrFreeList *freelist) {
  VkrFreeListNode *node = freelist->spare;
  if (node) {
    freelist->spare = node->next;
    node->next = NULL;
  }
  return node;
}

vkr_internal void vkr_return_node(VkrFreeList *freelist,
                                  VkrFreeListNode *node) {
  node->size = VKR_INVALID_ID;
  node->offset = VKR_INVALID_ID;
  node->next = freelist->spare;
  freelist->spare = node;
}

/* Nodes from `first` to the end of storage become the spare stack. */
vkr_internal void vkr_freelist_link_spare(VkrFreeList *freelist,
                                          uint32_t first) {
  freelist->spare = NULL;
  for (uint32_t i = freelist->max_count; i > first; --i) {
    VkrFreeListNode *node = &freelist->nodes[i - 1u];
    node->size = VKR_INVALID_ID;
    node->offset = VKR_INVALID_ID;
    node->next = freelist->spare;
    freelist->spare = node;
  }
}

uint64_t vkr_freelist_calculate_memory_requirement(uint64_t total_size) {
  /* A starting estimate of one free block per 4 KiB; owners grow the node
     storage when fragmentation needs more (vkr_freelist_grow_nodes). */
  uint64_t max_count = (total_size / 4096) + 16;
  if (max_count > UINT32_MAX) {
    max_count = UINT32_MAX;
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
  if (max_count > UINT32_MAX) {
    max_count = UINT32_MAX;
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
  vkr_freelist_link_spare(out_freelist, 1u);

  return true_v;
}

bool8_t vkr_freelist_out_of_nodes(const VkrFreeList *freelist) {
  return freelist && !freelist->spare;
}

/* Copy the free blocks into `nodes` in list order; false when they do not
   fit. */
vkr_internal bool8_t vkr_freelist_copy_nodes(const VkrFreeList *freelist,
                                             VkrFreeListNode *nodes,
                                             uint32_t count, uint32_t *used,
                                             VkrFreeListNode **head) {
  uint32_t index = 0u;
  VkrFreeListNode *previous = NULL;
  *head = NULL;
  for (const VkrFreeListNode *node = freelist->head; node; node = node->next) {
    if (index == count) {
      return false_v;
    }
    nodes[index] = (VkrFreeListNode){
        .size = node->size, .offset = node->offset, .next = NULL};
    if (previous) {
      previous->next = &nodes[index];
    } else {
      *head = &nodes[index];
    }
    previous = &nodes[index++];
  }
  *used = index;
  return true_v;
}

bool8_t vkr_freelist_grow_nodes(VkrFreeList *freelist, void *new_memory,
                                uint64_t new_memory_size,
                                void **out_old_memory) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(new_memory != NULL, "New node memory must not be NULL");
  assert_log(out_old_memory != NULL, "Output old memory must not be NULL");
  uint64_t new_count = new_memory_size / sizeof(VkrFreeListNode);
  if (new_count > UINT32_MAX) {
    new_count = UINT32_MAX;
  }
  if (new_count <= freelist->max_count) {
    return false_v;
  }
  VkrFreeListNode *nodes = (VkrFreeListNode *)new_memory;
  uint32_t used = 0u;
  VkrFreeListNode *head = NULL;
  if (!vkr_freelist_copy_nodes(freelist, nodes, (uint32_t)new_count, &used,
                               &head)) {
    return false_v;
  }
  *out_old_memory = freelist->memory;
  freelist->memory = new_memory;
  freelist->nodes = nodes;
  freelist->nodes_allocated_size = new_memory_size;
  freelist->max_count = (uint32_t)new_count;
  freelist->head = head;
  vkr_freelist_link_spare(freelist, used);
  return true_v;
}

void vkr_freelist_destroy(VkrFreeList *freelist) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");

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
  if (offset > freelist->total_size || size > freelist->total_size - offset) {
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

  freelist->head = &freelist->nodes[0];
  freelist->head->size = freelist->total_size;
  freelist->head->offset = 0;
  freelist->head->next = NULL;
  vkr_freelist_link_spare(freelist, 1u);
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

bool8_t vkr_freelist_resize(VkrFreeList *freelist, uint64_t new_total_size,
                            void *new_memory, uint64_t new_memory_size,
                            void **out_old_memory) {
  assert_log(freelist != NULL, "Freelist must not be NULL");
  assert_log(freelist->memory != NULL, "Freelist memory must not be NULL");
  assert_log(new_memory != NULL, "New node memory must not be NULL");
  assert_log(out_old_memory != NULL, "Output old memory must not be NULL");
  assert_log(new_total_size > freelist->total_size,
             "New total size must be greater than current size");

  uint64_t new_max_count = new_memory_size / sizeof(VkrFreeListNode);
  if (new_max_count > UINT32_MAX) {
    new_max_count = UINT32_MAX;
  }

  VkrFreeListNode *new_nodes = (VkrFreeListNode *)new_memory;
  uint32_t used = 0u;
  VkrFreeListNode *new_head = NULL;
  if (!vkr_freelist_copy_nodes(freelist, new_nodes, (uint32_t)new_max_count,
                               &used, &new_head)) {
    log_error("Ran out of nodes while copying freelist (need more memory)");
    return false_v;
  }

  const VkrFreeList previous = *freelist;
  freelist->memory = new_memory;
  freelist->nodes = new_nodes;
  freelist->nodes_allocated_size = new_memory_size;
  freelist->max_count = (uint32_t)new_max_count;
  freelist->head = new_head;
  freelist->total_size = new_total_size;
  vkr_freelist_link_spare(freelist, used);

  uint64_t growth_size = new_total_size - previous.total_size;
  if (!vkr_freelist_free(freelist, growth_size, previous.total_size)) {
    log_error("Failed to add new space to freelist after resize");
    *freelist = previous;
    return false_v;
  }

  *out_old_memory = previous.memory;

  return true_v;
}
