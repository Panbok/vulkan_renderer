#include "vkr_event_data_buffer.h"
#include "core/logger.h"

bool8_t vkr_event_data_buffer_create(VkrAllocator *allocator, uint64_t capacity,
                                     VkrEventDataBuffer *out_edb) {
  assert_log(allocator != NULL, "Allocator cannot be NULL.");
  assert_log(out_edb != NULL, "Output VkrEventDataBuffer cannot be NULL.");
  assert_log(capacity > 0, "Capacity must be greater than 0.");

  out_edb->allocator = allocator;
  out_edb->capacity = capacity;
  out_edb->buffer =
      vkr_allocator_alloc(allocator, capacity, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  if (out_edb->buffer == NULL) {
    log_error(
        "Failed to allocate memory for VkrEventDataBuffer internal buffer.");
    return false;
  }

  out_edb->head = 0;
  out_edb->tail = 0;
  out_edb->fill = 0;
  out_edb->wrap_boundary = capacity;
  out_edb->last_alloc_block_size = 0;
  out_edb->last_alloc_tail = 0;

  return true;
}

void vkr_event_data_buffer_destroy(VkrEventDataBuffer *edb) {
  assert_log(edb != NULL, "EventDataBuffer cannot be NULL.");
  if (edb->buffer && edb->allocator && edb->allocator->free) {
    vkr_allocator_free(edb->allocator, edb->buffer, edb->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  MemZero(edb, sizeof(*edb));
}

bool8_t vkr_event_data_buffer_alloc(VkrEventDataBuffer *edb,
                                    uint64_t payload_size,
                                    void **out_payload_ptr) {
  assert_log(edb != NULL, "EventDataBuffer cannot be NULL.");
  assert_log(out_payload_ptr != NULL, "Output payload pointer cannot be NULL.");

  if (payload_size == 0) {
    *out_payload_ptr = NULL;
    edb->last_alloc_block_size = 0;
    return true;
  }

  if (payload_size > edb->capacity ||
      edb->capacity - payload_size < sizeof(uint64_t)) {
    return false;
  }
  uint64_t block_size_needed = sizeof(uint64_t) + payload_size;
  if (block_size_needed > edb->capacity - edb->fill ||
      (edb->fill != 0 && edb->tail == edb->head)) {
    return false;
  }

  uint64_t write_offset = edb->tail;
  if (edb->tail < edb->head) {
    if (block_size_needed > edb->head - edb->tail) {
      return false;
    }
  } else if (block_size_needed > edb->capacity - edb->tail) {
    if (block_size_needed > edb->head) {
      return false;
    }
    edb->wrap_boundary = edb->tail;
    write_offset = 0;
  }

  uint8_t *actual_write_location = edb->buffer + write_offset;
  edb->last_alloc_tail = edb->tail;
  MemCopy(actual_write_location, &payload_size, sizeof(uint64_t));
  *out_payload_ptr = actual_write_location + sizeof(uint64_t);

  edb->tail = (write_offset + block_size_needed) % edb->capacity;
  edb->fill += block_size_needed;
  edb->last_alloc_block_size = block_size_needed;

  return true;
}

bool8_t vkr_event_data_buffer_free(VkrEventDataBuffer *edb,
                                   uint64_t payload_size_from_event) {
  assert_log(edb != NULL, "VkrEventDataBuffer cannot be NULL.");

  if (payload_size_from_event == 0) {
    return true;
  }

  if (edb->fill == 0) {
    return true;
  }

  assert_log(edb->head < edb->capacity, "Buffer head out of bounds.");

  uint64_t actual_payload_size_in_header;
  MemCopy(&actual_payload_size_in_header, edb->buffer + edb->head,
          sizeof(uint64_t));

  if (actual_payload_size_in_header != payload_size_from_event) {
    log_fatal(
        "VkrEventDataBuffer consistency error during free! Expected payload "
        "size %llu from event, "
        "but header at buffer head contains %llu. Head: %llu, Fill: "
        "%llu, Capacity: %llu",
        payload_size_from_event, actual_payload_size_in_header, edb->head,
        edb->fill, edb->capacity);
    return false;
  }

  uint64_t actual_block_size_to_free =
      sizeof(uint64_t) + actual_payload_size_in_header;

  if (edb->fill < actual_block_size_to_free) {
    log_fatal(
        "VkrEventDataBuffer consistency error during free! Fill count %llu "
        "is less than "
        "block size to free %llu (payload %llu). Head: %llu, Capacity: %llu",
        edb->fill, actual_block_size_to_free, actual_payload_size_in_header,
        edb->head, edb->capacity);
    return false;
  }

  edb->head += actual_block_size_to_free;
  if (edb->head == edb->wrap_boundary) {
    edb->head = 0;
    edb->wrap_boundary = edb->capacity;
  }
  edb->fill -= actual_block_size_to_free;

  if (edb->fill == 0) {
    edb->head = 0;
    edb->tail = 0;
    edb->wrap_boundary = edb->capacity;
    edb->last_alloc_block_size = 0;
  }

  return true;
}

void vkr_event_data_buffer_rollback_last_alloc(VkrEventDataBuffer *edb) {
  assert_log(edb != NULL, "VkrEventDataBuffer cannot be NULL.");

  if (edb->last_alloc_block_size == 0) {
    return;
  }

  assert_log(edb->fill >= edb->last_alloc_block_size,
             "Rollback error: fill < last_alloc_block_size");

  if (edb->last_alloc_tail == edb->wrap_boundary) {
    edb->wrap_boundary = edb->capacity;
  }
  edb->tail = edb->last_alloc_tail;

  edb->fill -= edb->last_alloc_block_size;
  edb->last_alloc_block_size = 0;

  if (edb->fill == 0) {
    edb->head = 0;
    edb->tail = 0;
    edb->wrap_boundary = edb->capacity;
    edb->last_alloc_block_size = 0;
  }
}
