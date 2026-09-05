#include "event_data_buffer_test.h"
#include "memory/vkr_arena_allocator.h"

static void test_event_data_buffer_fifo(VkrAllocator *allocator) {
  VkrEventDataBuffer buffer = {0};
  assert(vkr_event_data_buffer_create(allocator, 100u, &buffer));
  void *first = NULL;
  void *second = NULL;
  assert(vkr_event_data_buffer_alloc(&buffer, 10u, &first));
  assert(vkr_event_data_buffer_alloc(&buffer, 20u, &second));
  MemSet(first, 0x31, 10u);
  MemSet(second, 0x72, 20u);
  assert(vkr_event_data_buffer_free(&buffer, 10u));
  for (uint32_t i = 0u; i < 20u; ++i) {
    assert(((uint8_t *)second)[i] == 0x72);
  }
  assert(vkr_event_data_buffer_free(&buffer, 20u));
  assert(buffer.fill == 0u && buffer.head == 0u && buffer.tail == 0u);
  vkr_event_data_buffer_destroy(&buffer);
  assert(!buffer.buffer && !buffer.allocator && buffer.capacity == 0u);
}

static void test_event_data_buffer_admission(VkrAllocator *allocator) {
  VkrEventDataBuffer buffer = {0};
  assert(vkr_event_data_buffer_create(allocator, 60u, &buffer));
  void *payload = NULL;
  assert(vkr_event_data_buffer_alloc(&buffer, 0u, &payload));
  assert(!payload && buffer.fill == 0u);
  assert(!vkr_event_data_buffer_alloc(&buffer, UINT64_MAX, &payload));
  assert(!payload && buffer.fill == 0u);
  assert(vkr_event_data_buffer_alloc(&buffer, 10u, &payload));
  assert(vkr_event_data_buffer_alloc(&buffer, 15u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 10u));

  /* 37 bytes are free, split into ranges of 18 and 19 bytes. */
  void *rejected = NULL;
  assert(!vkr_event_data_buffer_alloc(&buffer, 20u, &rejected));
  assert(!rejected && buffer.fill == 23u);
  assert(vkr_event_data_buffer_alloc(&buffer, 5u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 15u));
  assert(vkr_event_data_buffer_free(&buffer, 5u));
  assert(buffer.fill == 0u);
  assert(vkr_event_data_buffer_alloc(&buffer, 52u, &payload));
  assert(!vkr_event_data_buffer_alloc(&buffer, 1u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 52u));
  vkr_event_data_buffer_destroy(&buffer);
}

static void test_event_data_buffer_wrap_and_drain(VkrAllocator *allocator) {
  VkrEventDataBuffer buffer = {0};
  assert(vkr_event_data_buffer_create(allocator, 100u, &buffer));
  void *payload = NULL;
  void *wrapped = NULL;
  assert(vkr_event_data_buffer_alloc(&buffer, 42u, &payload));
  assert(vkr_event_data_buffer_alloc(&buffer, 22u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 42u));

  /* The 45-byte block wraps over 20 bytes of unused tail padding. */
  assert(vkr_event_data_buffer_alloc(&buffer, 37u, &wrapped));
  MemSet(wrapped, 0xA5, 37u);
  assert(vkr_event_data_buffer_free(&buffer, 22u));
  assert(buffer.head == 0u);
  for (uint32_t i = 0u; i < 37u; ++i) {
    assert(((uint8_t *)wrapped)[i] == 0xA5);
  }
  assert(vkr_event_data_buffer_free(&buffer, 37u));
  assert(buffer.fill == 0u && buffer.head == 0u && buffer.tail == 0u);
  assert(vkr_event_data_buffer_alloc(&buffer, 92u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 92u));
  vkr_event_data_buffer_destroy(&buffer);
}

static void test_event_data_buffer_wrapped_full(VkrAllocator *allocator) {
  VkrEventDataBuffer buffer = {0};
  assert(vkr_event_data_buffer_create(allocator, 100u, &buffer));
  void *payload = NULL;
  assert(vkr_event_data_buffer_alloc(&buffer, 42u, &payload));
  assert(vkr_event_data_buffer_alloc(&buffer, 22u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 42u));
  assert(vkr_event_data_buffer_alloc(&buffer, 42u, &payload));
  MemSet(payload, 0x5A, 42u);

  /* head == tail with live blocks; the remaining 20 bytes are padding. */
  assert(buffer.head == buffer.tail && buffer.fill == 80u);
  void *rejected = NULL;
  assert(!vkr_event_data_buffer_alloc(&buffer, 1u, &rejected));
  assert(!rejected && ((uint8_t *)payload)[0] == 0x5A);
  assert(vkr_event_data_buffer_free(&buffer, 22u));
  assert(vkr_event_data_buffer_free(&buffer, 42u));
  assert(buffer.fill == 0u);
  vkr_event_data_buffer_destroy(&buffer);
}

static void test_event_data_buffer_rollback(VkrAllocator *allocator) {
  VkrEventDataBuffer buffer = {0};
  assert(vkr_event_data_buffer_create(allocator, 100u, &buffer));
  void *payload = NULL;
  assert(vkr_event_data_buffer_alloc(&buffer, 42u, &payload));
  assert(vkr_event_data_buffer_alloc(&buffer, 22u, &payload));
  assert(vkr_event_data_buffer_free(&buffer, 42u));
  assert(vkr_event_data_buffer_alloc(&buffer, 37u, &payload));
  vkr_event_data_buffer_rollback_last_alloc(&buffer);
  assert(buffer.tail == 80u && buffer.fill == 30u);

  /* Rolling back the wrapped block makes the original tail usable again. */
  assert(vkr_event_data_buffer_alloc(&buffer, 12u, &payload));
  assert(payload == buffer.buffer + 80u + sizeof(uint64_t));
  vkr_event_data_buffer_rollback_last_alloc(&buffer);
  assert(buffer.tail == 80u && buffer.fill == 30u);
  vkr_event_data_buffer_rollback_last_alloc(&buffer);
  assert(buffer.tail == 80u && buffer.fill == 30u);
  assert(vkr_event_data_buffer_free(&buffer, 22u));
  assert(buffer.fill == 0u);

  assert(vkr_event_data_buffer_alloc(&buffer, 10u, &payload));
  vkr_event_data_buffer_rollback_last_alloc(&buffer);
  assert(buffer.fill == 0u && buffer.head == 0u && buffer.tail == 0u);
  vkr_event_data_buffer_destroy(&buffer);
}

bool32_t run_event_data_buffer_tests(void) {
  printf("--- Running Event Data Buffer tests... ---\n");
  Arena *arena = arena_create(MB(1), MB(1));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  test_event_data_buffer_fifo(&allocator);
  test_event_data_buffer_admission(&allocator);
  test_event_data_buffer_wrap_and_drain(&allocator);
  test_event_data_buffer_wrapped_full(&allocator);
  test_event_data_buffer_rollback(&allocator);
  arena_destroy(arena);
  printf("--- Event Data Buffer tests completed. ---\n");
  return true_v;
}
