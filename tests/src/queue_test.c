#include "queue_test.h"
#include "container_test_allocator.h"
#include "memory/vkr_arena_allocator.h"

static void test_queue_create_failure(void) {
  ContainerTestAllocator state = {.fail = true};
  VkrAllocator allocator = container_test_allocator(&state);
  Queue_uint32_t queue = queue_create_uint32_t(&allocator, 8);
  assert(!queue.data && !queue.allocator && !queue.capacity && !queue.size);
  assert(!queue.head && !queue.tail && state.calls == 1);
  queue_destroy_uint32_t(&queue);
  queue = queue_create_uint32_t(&allocator, SIZE_MAX / sizeof(uint32_t) + 1);
  assert(!queue.data && !queue.allocator && state.calls == 1);
  assert(!state.live_bytes);
}

bool32_t run_queue_tests(void) {
  printf("--- Starting Queue Tests ---\n");
  test_queue_create_failure();
  Arena *arena = arena_create(MB(1), MB(1));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  Queue_uint32_t queue = queue_create_uint32_t(&allocator, 3u);
  uint32_t value = 0u;
  assert(queue_is_empty_uint32_t(&queue));
  assert(!queue_dequeue_uint32_t(&queue, &value));
  assert(queue_enqueue_uint32_t(&queue, 11u));
  assert(queue_enqueue_uint32_t(&queue, 22u));
  assert(queue_enqueue_uint32_t(&queue, 33u));
  assert(!queue_enqueue_uint32_t(&queue, 44u));
  assert(queue_peek_uint32_t(&queue) == 11u);
  assert(queue_dequeue_uint32_t(&queue, &value) && value == 11u);

  /* Reuse the first slot while two older values remain queued. */
  assert(queue_enqueue_uint32_t(&queue, 44u));
  assert(queue_dequeue_uint32_t(&queue, &value) && value == 22u);
  assert(queue_dequeue_uint32_t(&queue, &value) && value == 33u);
  assert(queue_dequeue_uint32_t(&queue, &value) && value == 44u);
  assert(queue_is_empty_uint32_t(&queue));

  assert(queue_enqueue_uint32_t(&queue, 55u));
  queue_clear_uint32_t(&queue);
  assert(queue_is_empty_uint32_t(&queue));
  assert(queue_enqueue_uint32_t(&queue, 66u));
  assert(queue_dequeue_uint32_t(&queue, &value) && value == 66u);
  queue_destroy_uint32_t(&queue);
  arena_destroy(arena);
  printf("--- Queue Tests Completed ---\n");
  return true_v;
}
