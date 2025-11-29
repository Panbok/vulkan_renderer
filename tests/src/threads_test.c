#include "threads_test.h"

#include <stdatomic.h>

static Arena *arena = NULL;
static VkrAllocator allocator = {0};
static const uint64_t ARENA_SIZE = MB(1);

static void setup_suite(void) {
  arena = arena_create(ARENA_SIZE);
  allocator = (VkrAllocator){.ctx = arena};
  assert(vkr_allocator_arena(&allocator) && "allocator init failed");
}

static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
  allocator = (VkrAllocator){0};
}

typedef struct ThreadCounterArgs {
  atomic_int *counter;
} ThreadCounterArgs;

static void *thread_counter_fn(void *arg) {
  ThreadCounterArgs *data = (ThreadCounterArgs *)arg;
  atomic_fetch_add_explicit(data->counter, 1, memory_order_relaxed);
  return NULL;
}

static void test_thread_create_join(void) {
  printf("  Running test_thread_create_join...\n");
  setup_suite();

  atomic_int counter = 0;
  ThreadCounterArgs args = {.counter = &counter};
  VkrThread thread = NULL;

  assert(vkr_thread_create(&allocator, &thread, thread_counter_fn, &args) &&
         "thread creation failed");
  assert(vkr_thread_get_id(thread) != 0 && "thread id should be non-zero");

  assert(vkr_thread_join(thread) && "thread join failed");
  assert(!vkr_thread_is_active(thread) && "thread should be inactive after join");

  assert(vkr_thread_destroy(&allocator, &thread) &&
         "thread destroy failed after join");
  assert(thread == NULL && "thread handle should be NULL after destroy");
  assert(atomic_load_explicit(&counter, memory_order_relaxed) == 1 &&
         "counter increment mismatch");

  teardown_suite();
  printf("  test_thread_create_join PASSED\n");
}

typedef struct MutexCounterArgs {
  VkrMutex mutex;
  int *accumulator;
  int iterations;
} MutexCounterArgs;

static void *mutex_counter_fn(void *arg) {
  MutexCounterArgs *data = (MutexCounterArgs *)arg;
  for (int i = 0; i < data->iterations; i++) {
    vkr_mutex_lock(data->mutex);
    (*data->accumulator)++;
    vkr_mutex_unlock(data->mutex);
  }
  return NULL;
}

static void test_mutex_contention(void) {
  printf("  Running test_mutex_contention...\n");
  setup_suite();

  VkrMutex mutex = NULL;
  assert(vkr_mutex_create(&allocator, &mutex) && "mutex create failed");

  int accumulator = 0;
  const int iterations = 500;
  MutexCounterArgs args = {.mutex = mutex,
                           .accumulator = &accumulator,
                           .iterations = iterations};

  VkrThread t1 = NULL;
  VkrThread t2 = NULL;
  assert(vkr_thread_create(&allocator, &t1, mutex_counter_fn, &args) &&
         "thread 1 create failed");
  assert(vkr_thread_create(&allocator, &t2, mutex_counter_fn, &args) &&
         "thread 2 create failed");

  vkr_thread_join(t1);
  vkr_thread_join(t2);
  vkr_thread_destroy(&allocator, &t1);
  vkr_thread_destroy(&allocator, &t2);

  assert(accumulator == iterations * 2 && "mutex-protected increments mismatch");

  assert(vkr_mutex_destroy(&allocator, &mutex) && "mutex destroy failed");
  teardown_suite();
  printf("  test_mutex_contention PASSED\n");
}

typedef struct CondWaitData {
  VkrMutex mutex;
  VkrCondVar cond;
  bool32_t ready;
  bool32_t woke;
  atomic_bool waiting;
} CondWaitData;

static void *cond_waiter_fn(void *arg) {
  CondWaitData *data = (CondWaitData *)arg;

  vkr_mutex_lock(data->mutex);
  atomic_store_explicit(&data->waiting, true, memory_order_release);
  while (!data->ready) {
    vkr_cond_wait(data->cond, data->mutex);
  }
  data->woke = true_v;
  vkr_mutex_unlock(data->mutex);
  return NULL;
}

static void test_cond_wait_signal(void) {
  printf("  Running test_cond_wait_signal...\n");
  setup_suite();

  CondWaitData data = {.mutex = NULL,
                       .cond = NULL,
                       .ready = false_v,
                       .woke = false_v,
                       .waiting = ATOMIC_VAR_INIT(false)};

  assert(vkr_mutex_create(&allocator, &data.mutex) && "mutex create failed");
  assert(vkr_cond_create(&allocator, &data.cond) && "cond create failed");

  VkrThread waiter = NULL;
  assert(vkr_thread_create(&allocator, &waiter, cond_waiter_fn, &data) &&
         "waiter thread create failed");

  // Wait for the thread to start waiting.
  while (!atomic_load_explicit(&data.waiting, memory_order_acquire)) {
    vkr_thread_sleep(1);
  }

  vkr_mutex_lock(data.mutex);
  data.ready = true_v;
  vkr_cond_signal(data.cond);
  vkr_mutex_unlock(data.mutex);

  vkr_thread_join(waiter);
  vkr_thread_destroy(&allocator, &waiter);

  assert(data.woke == true_v && "waiter thread did not resume after signal");

  assert(vkr_cond_destroy(&allocator, &data.cond) && "cond destroy failed");
  assert(vkr_mutex_destroy(&allocator, &data.mutex) && "mutex destroy failed");
  teardown_suite();
  printf("  test_cond_wait_signal PASSED\n");
}

bool32_t run_threads_tests(void) {
  printf("--- Running Threads tests... ---\n");
  test_thread_create_join();
  test_mutex_contention();
  test_cond_wait_signal();
  printf("--- Threads tests completed. ---\n");
  return true;
}
