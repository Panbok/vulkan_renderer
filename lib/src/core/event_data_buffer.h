#pragma once

#include "defines.h"
#include "logger.h"
#include "memory/arena.h"
#include "pch.h"
#include "platform/platform.h"

/**
 * @brief Structure for managing a ring buffer for variable-sized event data.
 *
 * The buffer stores data blocks, where each block consists of:
 * 1. A uint64_t header storing the size of the payload.
 * 2. The actual payload data.
 *
 * This structure is not inherently thread-safe; synchronization must be
 * handled externally (e.g., by EventManager's mutex).
 */
typedef struct EventDataBuffer {
  Arena *arena;      /**< Arena from which this buffer's memory is allocated
                        (optional, for context). */
  uint8_t *buffer;   /**< The contiguous memory block for the ring buffer. */
  uint64_t capacity; /**< Total capacity of the buffer in bytes. */
  uint64_t head;     /**< Read position (start of the oldest data block). */
  uint64_t tail;     /**< Write position (where new data block will start). */
  uint64_t fill;     /**< Current number of bytes used in the buffer (headers +
                        payloads). */
  uint64_t last_alloc_block_size; /**< Size of the last successfully allocated
                                     block (header + payload), for rollback. */
} EventDataBuffer;

/**
 * @brief Creates and initializes an EventDataBuffer.
 *
 * Allocates memory for the buffer itself from the provided arena.
 *
 * @param owner_arena The arena to allocate the EventDataBuffer's internal
 * buffer from.
 * @param capacity The desired capacity of the ring buffer in bytes.
 * @param out_edb Pointer to the EventDataBuffer structure to initialize.
 * @return true if creation was successful, false otherwise (e.g., allocation
 * failure).
 */
bool8_t event_data_buffer_create(Arena *owner_arena, uint64_t capacity,
                                 EventDataBuffer *out_edb);

/**
 * @brief Destroys an EventDataBuffer.
 *
 * This function primarily resets the EventDataBuffer's fields. The actual
 * memory for `buffer` is managed by the `owner_arena` used during creation
 * and will be freed when that arena is destroyed.
 *
 * @param edb Pointer to the EventDataBuffer to destroy.
 */
void event_data_buffer_destroy(EventDataBuffer *edb);

/**
 * @brief Attempts to allocate a block of memory in the EventDataBuffer for
 * event payload.
 *
 * The allocation includes space for a header (payload size) and the payload
 * itself. If successful, `out_payload_ptr` will point to the beginning of the
 * payload section within the allocated block.
 *
 * @param edb Pointer to the EventDataBuffer.
 * @param payload_size The size of the payload data to allocate.
 * @param out_payload_ptr Pointer to a void* that will be set to the location of
 * the payload data within the buffer if allocation is successful.
 * @return true if allocation was successful, false if there's not enough
 * contiguous space or buffer is too full.
 */
bool8_t event_data_buffer_alloc(EventDataBuffer *edb, uint64_t payload_size,
                                void **out_payload_ptr);

/**
 * @brief Frees the oldest data block from the EventDataBuffer.
 *
 * This advances the head of the buffer. It's assumed that the `payload_size`
 * matches the payload size of the block currently at the head.
 * This function should be called after the data associated with the oldest
 * event has been processed.
 *
 * @param edb Pointer to the EventDataBuffer.
 * @param payload_size The size of the payload that was at the head of the
 * buffer. This is used for sanity checks and to correctly update `fill`.
 * @return true if a block was successfully marked as free, false on error
 * (e.g., empty buffer, size mismatch).
 */
bool8_t event_data_buffer_free(EventDataBuffer *edb, uint64_t payload_size);

/**
 * @brief Rolls back the last successful allocation made by
 * event_data_buffer_alloc.
 *
 * This is used if an event could not be enqueued after its data was allocated
 * in the buffer, to prevent orphaning the space.
 *
 * @param edb Pointer to the EventDataBuffer.
 */
void event_data_buffer_rollback_last_alloc(EventDataBuffer *edb);

/**
 * @brief Checks if the EventDataBuffer can accommodate a new allocation of a
 * given payload size.
 *
 * This does not perform the allocation, only checks if it's possible.
 * @param edb Pointer to the EventDataBuffer.
 * @param payload_size The size of the payload data to check for.
 * @return true if an allocation of this size is possible, false otherwise.
 */
bool8_t event_data_buffer_can_alloc(const EventDataBuffer *edb,
                                    uint64_t payload_size);