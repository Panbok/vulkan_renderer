#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"

// FIFO payload storage. Each block has a uint64_t payload-size header.
// The caller serializes allocation, free, and rollback; EventManager uses its
// mutex. Payload pointers remain valid until their block is freed or rolled
// back.
typedef struct VkrEventDataBuffer {
  VkrAllocator *allocator;
  uint8_t *buffer;
  uint64_t capacity;
  uint64_t head;
  uint64_t tail;
  uint64_t fill; // Live header + payload bytes; excludes unused end padding.
  uint64_t wrap_boundary; // End of the older contiguous run, or capacity.
  uint64_t last_alloc_block_size;
  uint64_t last_alloc_tail; // Tail before allocation, including a skipped gap.
} VkrEventDataBuffer;

bool8_t vkr_event_data_buffer_create(VkrAllocator *allocator, uint64_t capacity,
                                     VkrEventDataBuffer *out_edb);
void vkr_event_data_buffer_destroy(VkrEventDataBuffer *edb);

// Zero-size allocation succeeds with a NULL payload. Failure preserves the
// buffer and output pointer. Payloads must fit contiguously with their header.
bool8_t vkr_event_data_buffer_alloc(VkrEventDataBuffer *edb,
                                    uint64_t payload_size,
                                    void **out_payload_ptr);

// Free in FIFO order with the original payload size. Zero size and an empty
// buffer are no-ops; a mismatched size reports a consistency error.
bool8_t vkr_event_data_buffer_free(VkrEventDataBuffer *edb,
                                   uint64_t payload_size);

// Undo the latest successful allocation before any other buffer mutation.
// Failed allocations preserve the rollback record; rollback consumes it.
void vkr_event_data_buffer_rollback_last_alloc(VkrEventDataBuffer *edb);
