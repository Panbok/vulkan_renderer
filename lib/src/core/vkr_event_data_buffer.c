#include "vkr_event_data_buffer.h"

bool8_t vkr_event_data_buffer_create(Arena *owner_arena, uint64_t capacity,
                                     VkrEventDataBuffer *out_edb) {
  assert_log(owner_arena != NULL, "Owner arena cannot be NULL.");
  assert_log(out_edb != NULL, "Output VkrEventDataBuffer cannot be NULL.");
  assert_log(capacity > 0, "Capacity must be greater than 0.");

  out_edb->arena = owner_arena;
  out_edb->capacity = capacity;
  out_edb->buffer = arena_alloc(owner_arena, capacity, ARENA_MEMORY_TAG_BUFFER);

  if (out_edb->buffer == NULL) {
    log_error(
        "Failed to allocate memory for VkrEventDataBuffer internal buffer.");
    return false;
  }

  out_edb->head = 0;
  out_edb->tail = 0;
  out_edb->fill = 0;
  out_edb->last_alloc_block_size = 0;

  return true;
}

void vkr_event_data_buffer_destroy(VkrEventDataBuffer *edb) {
  assert_log(edb != NULL, "EventDataBuffer cannot be NULL.");
  edb->buffer = NULL;
  edb->arena = NULL;
  edb->capacity = 0;
  edb->head = 0;
  edb->tail = 0;
  edb->fill = 0;
  edb->last_alloc_block_size = 0;
}

bool8_t vkr_event_data_buffer_can_alloc(const VkrEventDataBuffer *edb,
                                        uint64_t payload_size) {
  assert_log(edb != NULL, "VkrEventDataBuffer cannot be NULL.");
  if (payload_size == 0) {
    return true;
  }
  uint64_t block_size_needed = sizeof(uint64_t) + payload_size;
  return edb->fill + block_size_needed <= edb->capacity;
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

  uint64_t block_size_needed = sizeof(uint64_t) + payload_size;

  if (edb->fill + block_size_needed > edb->capacity) {
    log_warn("EventDataBuffer full. Cannot allocate %llu bytes (payload %llu). "
             "Fill: %llu, Capacity: %llu",
             block_size_needed, payload_size, edb->fill, edb->capacity);
    return false;
  }

  uint8_t *write_ptr_base = edb->buffer;
  uint64_t new_tail_candidate = edb->tail;
  uint8_t *actual_write_location = NULL;

  if (edb->tail >= edb->head) {
    if (edb->tail + block_size_needed <= edb->capacity) {
      actual_write_location = write_ptr_base + edb->tail;
      new_tail_candidate = edb->tail + block_size_needed;
    } else {
      if (block_size_needed <= edb->head) {
        actual_write_location = write_ptr_base + 0; // Write at the start
        new_tail_candidate = block_size_needed;
      } else {
        log_warn("EventDataBuffer fragmented. Cannot allocate %llu bytes. "
                 "Tail: %llu, Head: %llu, Capacity: %llu",
                 block_size_needed, edb->tail, edb->head, edb->capacity);
        return false;
      }
    }
  } else {
    if (edb->tail + block_size_needed <= edb->head) {
      actual_write_location = write_ptr_base + edb->tail;
      new_tail_candidate = edb->tail + block_size_needed;
    } else {
      log_warn("EventDataBuffer fragmented (wrapped). Cannot allocate %llu "
               "bytes. Tail: %llu, Head: %llu",
               block_size_needed, edb->tail, edb->head);
      return false;
    }
  }

  assert_log(actual_write_location != NULL,
             "Write location should have been determined.");

  MemCopy(actual_write_location, &payload_size, sizeof(uint64_t));
  *out_payload_ptr = actual_write_location + sizeof(uint64_t);

  edb->tail = new_tail_candidate % edb->capacity;
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

  // If the buffer is already empty, the data this event pointed to
  // has already been implicitly "freed" by the head pointer advancing past it
  // due to previous free operations. This is not an error from the buffer's
  // perspective; its state (empty) is consistent.
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

  edb->head = (edb->head + actual_block_size_to_free) % edb->capacity;
  edb->fill -= actual_block_size_to_free;

  if (edb->fill == 0) {
    edb->head = 0;
    edb->tail = 0;
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

  // To correctly rollback tail, we need to know if it wrapped.
  // The alloc logic ensures tail points to the *end* of the allocated block.
  // So, we subtract last_alloc_block_size from tail, handling underflow by
  // wrapping around capacity.
  if (edb->tail < edb->last_alloc_block_size) {
    edb->tail = (edb->tail + edb->capacity - edb->last_alloc_block_size) %
                edb->capacity;

  } else {
    edb->tail -= edb->last_alloc_block_size;
  }

  edb->fill -= edb->last_alloc_block_size;
  edb->last_alloc_block_size = 0;

  if (edb->fill == 0) {
    edb->head = 0;
    edb->tail = 0;
  }
}