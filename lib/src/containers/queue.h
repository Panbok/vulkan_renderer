/**
 * @file queue.h
 * @brief Queue implementation based on a circular buffer
 *
 * This file provides a generic, type-safe queue implementation using the C
 * preprocessor. The queue is implemented as a circular buffer, which provides
 * efficient O(1) enqueue and dequeue operations without requiring memory
 * allocations for each operation.
 *
 * Memory Layout:
 * A queue consists of a metadata structure and a contiguous array of elements:
 *
 * +---------------------+ <-- Queue_TYPE structure
 * | Arena *arena        |     (Pointer to the arena allocator used for memory)
 * | uint64_t capacity     |     (Maximum number of elements the queue can hold)
 * | uint64_t size         |     (Current number of elements in the queue)
 * | uint64_t tail         |     (Index where next element will be inserted)
 * | uint64_t head         |     (Index where next element will be removed from)
 * | TYPE *data          | --> Points to contiguous memory block of capacity *
 * | sizeof(TYPE)        |     (Size of each element in the queue)
 * +---------------------+
 *
 * Circular Buffer Design:
 * The circular buffer uses head and tail indices to track where elements should
 * be dequeued from and enqueued to, respectively. When these indices reach the
 * end of the allocated memory, they wrap around to the beginning, creating a
 * circular pattern.
 *
 * This implementation is primarily used for event processing and other
 * scenarios where FIFO (First-In-First-Out) data access patterns are required.
 *
 * Usage Pattern:
 * 1. Create a queue using queue_create_TYPE()
 * 2. Enqueue elements with queue_enqueue_TYPE()
 * 3. Dequeue elements with queue_dequeue_TYPE()
 * 4. Check status with queue_is_empty_TYPE() and queue_is_full_TYPE()
 * 5. When done, optionally call queue_destroy_TYPE() to clean up metadata
 */

#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "memory/vkr_allocator.h"

#define QueueConstruct(type, name)                                             \
  /**                                                                          \
   * @brief Queue structure for storing elements of type 'type'                \
   * A queue of type 'name' implemented as a circular buffer                   \
   */                                                                          \
  struct Queue_##name;                                                         \
  typedef struct Queue_##name {                                                \
    VkrAllocator *allocator; /**< Allocator used for memory allocation */      \
    uint64_t capacity; /**< Maximum number of elements the queue can hold */   \
    uint64_t size;     /**< Current number of elements in the queue */         \
    uint64_t tail;     /**< Index where the next element will be inserted */   \
    uint64_t head; /**< Index where the next element will be removed from */   \
    type *data;    /**< Pointer to the circular buffer storage */              \
  } Queue_##name;                                                              \
  /**                                                                          \
   * @brief Creates a new queue with the specified capacity                    \
   * @param arena Arena allocator to use for memory allocation                 \
   * @param capacity Maximum number of elements the queue can hold             \
   * @return Initialized Queue_##name structure                                \
   */                                                                          \
  static inline Queue_##name queue_create_##name(VkrAllocator *allocator,      \
                                                 uint64_t capacity) {          \
    assert_log(allocator != NULL, "Allocator is NULL");                        \
    assert_log(capacity > 0, "Capacity is 0");                                 \
    type *buf = vkr_allocator_alloc(allocator, capacity * sizeof(type),        \
                                    VKR_ALLOCATOR_MEMORY_TAG_QUEUE);           \
    assert_log(buf != NULL, "alloc failed in queue_create");                   \
    Queue_##name queue = {allocator, capacity, 0, 0, 0, buf};                  \
    return queue;                                                              \
  }                                                                            \
  /**                                                                          \
   * @brief Checks if the queue is empty                                       \
   * @param queue Pointer to the queue to check                                \
   * @return true if the queue is empty, false otherwise                       \
   */                                                                          \
  static inline bool32_t queue_is_empty_##name(const Queue_##name *queue) {    \
    assert_log(queue != NULL, "Queue pointer cannot be NULL");                 \
    return queue->size == 0;                                                   \
  }                                                                            \
  /**                                                                          \
   * @brief Checks if the queue is full                                        \
   * @param queue Pointer to the queue to check                                \
   * @return true if the queue is full, false otherwise                        \
   */                                                                          \
  static inline bool32_t queue_is_full_##name(const Queue_##name *queue) {     \
    assert_log(queue != NULL, "Queue pointer cannot be NULL");                 \
    return queue->size == queue->capacity;                                     \
  }                                                                            \
  /**                                                                          \
   * @brief Adds an element to the end of the queue                            \
   * @param q Pointer to the queue                                             \
   * @param data Element to add to the queue                                   \
   * @return true if the element was added, false if the queue is full         \
   */                                                                          \
  static inline bool32_t queue_enqueue_##name(Queue_##name *q, type data) {    \
    assert_log(q != NULL, "Queue is NULL");                                    \
    if (queue_is_full_##name(q)) {                                             \
      return false;                                                            \
    }                                                                          \
    q->data[q->tail] = data;                                                   \
    q->tail = (q->tail + 1) % q->capacity; /* wrap around if needed */         \
    q->size++;                                                                 \
    return true;                                                               \
  }                                                                            \
  /**                                                                          \
   * @brief Removes an element from the front of the queue                     \
   * @param q Pointer to the queue                                             \
   * @param value_ptr Optional pointer to store the removed element, can be    \
   * NULL                                                                      \
   * @return true if an element was removed, false if the queue is empty       \
   */                                                                          \
  static inline bool32_t queue_dequeue_##name(                                 \
      Queue_##name *q, type *value_ptr /* optional parameter */) {             \
    assert_log(q != NULL, "Queue is NULL");                                    \
    if (queue_is_empty_##name(q)) {                                            \
      return false;                                                            \
    }                                                                          \
    if (value_ptr != NULL) {                                                   \
      *value_ptr = q->data[q->head];                                           \
    }                                                                          \
    q->head = (q->head + 1) % q->capacity; /* wrap around if needed */         \
    q->size--;                                                                 \
    return true;                                                               \
  }                                                                            \
  /**                                                                          \
   * @brief Returns the element at the front of the queue without removing it  \
   * @param q Pointer to the queue                                             \
   * @return The element at the front of the queue                             \
   * @note Asserts if the queue is empty                                       \
   */                                                                          \
  static inline type queue_peek_##name(Queue_##name *q) {                      \
    assert_log(q != NULL, "Queue is NULL");                                    \
    assert_log(!queue_is_empty_##name(q), "Queue is empty");                   \
    return q->data[q->head];                                                   \
  }                                                                            \
  /**                                                                          \
   * @brief Removes all elements from the queue                                \
   * @param q Pointer to the queue                                             \
   * @note This does not deallocate memory, only resets indices                \
   */                                                                          \
  static inline void queue_clear_##name(Queue_##name *q) {                     \
    assert_log(q != NULL, "Queue is NULL");                                    \
    q->size = 0;                                                               \
    q->head = 0;                                                               \
    q->tail = 0;                                                               \
  }                                                                            \
  /**                                                                          \
   * @brief Marks the queue as destroyed, sets all members to NULL/0           \
   * @param q Pointer to the queue                                             \
   * @note This does not deallocate memory, as that's managed by the arena     \
   */                                                                          \
  static inline void queue_destroy_##name(Queue_##name *q) {                   \
    assert_log(q != NULL, "Queue is NULL");                                    \
    if (q->data) {                                                             \
      vkr_allocator_free(q->allocator, q->data,                                \
                         q->capacity * sizeof(type),                          \
                         VKR_ALLOCATOR_MEMORY_TAG_QUEUE);                      \
    }                                                                          \
    q->data = NULL;                                                            \
    q->allocator = NULL;                                                       \
    q->capacity = 0;                                                           \
    q->size = 0;                                                               \
    q->head = 0;                                                               \
    q->tail = 0;                                                               \
  }

#define Queue(type) QueueConstruct(type, type)

Queue(uint8_t);
Queue(uint32_t);
Queue(uint64_t);
Queue(bool8_t);
Queue(String8);
