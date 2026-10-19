#include "zone_test.h"

#include "containers/str.h"
#include "core/vkr_threads.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

#define ZONE_TEST_THREADS 4u
#define ZONE_TEST_ROUNDS 64u
#define ZONE_TEST_CAPACITY (ZONE_TEST_ROUNDS * 2u)

/* What the sink received, by thread slot: each slot is one thread, so its
   row has a single writer. */
typedef struct ZoneTestSink {
  VkrZoneRecord records[VKR_ZONE_THREAD_MAX][ZONE_TEST_CAPACITY];
  VkrAtomicUint32 counts[VKR_ZONE_THREAD_MAX];
  /* A write that waits until the test lets it finish. */
  VkrAtomicBool block_writes;
  VkrAtomicBool write_started;
  VkrAtomicBool write_finished;
} ZoneTestSink;

static ZoneTestSink zone_test_sink;

static void zone_test_write(void *context, const VkrZoneRecord *record) {
  ZoneTestSink *sink = (ZoneTestSink *)context;
  if (vkr_atomic_bool_load(&sink->block_writes, VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_atomic_bool_store(&sink->write_started, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
    vkr_thread_sleep(50u);
    vkr_atomic_bool_store(&sink->write_finished, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
    return;
  }
  assert(record->thread < VKR_ZONE_THREAD_MAX);
  const uint32_t index = vkr_atomic_uint32_fetch_add(
      &sink->counts[record->thread], 1u, VKR_MEMORY_ORDER_RELAXED);
  assert(index < ZONE_TEST_CAPACITY);
  sink->records[record->thread][index] = *record;
}

typedef struct ZoneTestThread {
  uint16_t slot;
} ZoneTestThread;

/* Nested zones on a worker slot: an outer zone around an inner one. */
static void *zone_test_worker(void *argument) {
  const ZoneTestThread *thread = (const ZoneTestThread *)argument;
  vkr_zone_thread_set(thread->slot);
  for (uint32_t round = 0u; round < ZONE_TEST_ROUNDS; ++round) {
    VKR_ZONE_BEGIN(outer, "Test.Outer");
    VKR_ZONE_BEGIN(inner, "Test.Inner");
    VKR_ZONE_END(inner);
    VKR_ZONE_END(outer);
  }
  return NULL;
}

/* A worker whose one zone the sink holds inside its write. */
static void *zone_test_blocked_worker(void *argument) {
  (void)argument;
  vkr_zone_thread_set(VKR_ZONE_THREAD_WORKER_FIRST);
  VKR_ZONE_BEGIN(held, "Test.Held");
  VKR_ZONE_END(held);
  return NULL;
}

static void zone_test_reset(void) {
  MemZero(&zone_test_sink, sizeof(zone_test_sink));
}

static void test_zone_without_sink_records_nothing(void) {
  printf("  Running test_zone_without_sink_records_nothing...\n");
  zone_test_reset();
  vkr_zone_thread_set(VKR_ZONE_THREAD_MAIN);
  VKR_ZONE_BEGIN(idle, "Test.Idle");
  assert(!idle.open);
  VKR_ZONE_END(idle);
  vkr_zone_thread_set(VKR_ZONE_THREAD_NONE);
  printf("  test_zone_without_sink_records_nothing PASSED\n");
}

/* Signposts open zones without a sink: nested intervals get distinct ids,
   nothing reaches a sink, and the depth they took is back at zero when a
   recording starts. */
static void test_zone_signposts_without_sink(void) {
  printf("  Running test_zone_signposts_without_sink...\n");
  zone_test_reset();
  vkr_zone_thread_set(VKR_ZONE_THREAD_MAIN);
  vkr_zone_signposts_set(true_v);
  assert(vkr_zone_signposts());
  VKR_ZONE_BEGIN(outer, "Test.SignpostOuter");
  VKR_ZONE_BEGIN(inner, "Test.SignpostInner");
  assert(outer.open && inner.open);
  assert(outer.signpost != 0u && inner.signpost != 0u);
  assert(outer.signpost != inner.signpost);
  assert(outer.depth == 0u && inner.depth == 1u);
  VKR_ZONE_END(inner);
  VKR_ZONE_END(outer);
  vkr_zone_signposts_set(false_v);
  const VkrZoneSink sink = {.write = zone_test_write,
                            .context = &zone_test_sink};
  vkr_zone_sink_set(&sink);
  VKR_ZONE_BEGIN(recorded, "Test.AfterSignposts");
  assert(recorded.signpost == 0u);
  VKR_ZONE_END(recorded);
  vkr_zone_sink_set(NULL);
  assert(vkr_atomic_uint32_load(&zone_test_sink.counts[VKR_ZONE_THREAD_MAIN],
                                VKR_MEMORY_ORDER_RELAXED) == 1u);
  assert(zone_test_sink.records[VKR_ZONE_THREAD_MAIN][0].depth == 0u);
  vkr_zone_thread_set(VKR_ZONE_THREAD_NONE);
  printf("  test_zone_signposts_without_sink PASSED\n");
}

static void test_zone_threads_record_nested_zones(VkrAllocator *allocator) {
  printf("  Running test_zone_threads_record_nested_zones...\n");
  zone_test_reset();
  const VkrZoneSink sink = {.write = zone_test_write,
                            .context = &zone_test_sink};
  vkr_zone_sink_set(&sink);

  /* A thread without a slot records nothing. */
  VKR_ZONE_BEGIN(unnamed, "Test.Unnamed");
  assert(!unnamed.open);
  VKR_ZONE_END(unnamed);

  ZoneTestThread threads[ZONE_TEST_THREADS];
  VkrThread handles[ZONE_TEST_THREADS] = {0};
  for (uint32_t i = 0u; i < ZONE_TEST_THREADS; ++i) {
    threads[i].slot = (uint16_t)(VKR_ZONE_THREAD_WORKER_FIRST + i);
    assert(vkr_thread_create(allocator, &handles[i], zone_test_worker,
                             &threads[i]));
  }
  for (uint32_t i = 0u; i < ZONE_TEST_THREADS; ++i) {
    assert(vkr_thread_join(handles[i]));
    assert(vkr_thread_destroy(allocator, &handles[i]));
  }
  vkr_zone_sink_set(NULL);

  for (uint32_t i = 0u; i < ZONE_TEST_THREADS; ++i) {
    const uint16_t slot = threads[i].slot;
    const uint32_t count = vkr_atomic_uint32_load(&zone_test_sink.counts[slot],
                                                  VKR_MEMORY_ORDER_ACQUIRE);
    assert(count == ZONE_TEST_ROUNDS * 2u);
    /* Inner zones end first: records alternate inner (depth 1) and outer
       (depth 0), and each inner lies within the outer after it. */
    for (uint32_t r = 0u; r < count; r += 2u) {
      const VkrZoneRecord *inner = &zone_test_sink.records[slot][r];
      const VkrZoneRecord *outer = &zone_test_sink.records[slot][r + 1u];
      assert(inner->thread == slot && outer->thread == slot);
      assert(inner->depth == 1u && outer->depth == 0u);
      assert(inner->begin_ns <= inner->end_ns);
      assert(outer->begin_ns <= inner->begin_ns &&
             inner->end_ns <= outer->end_ns);
      assert(string_equals(vkr_zone_site_name(inner->zone), "Test.Inner"));
      assert(string_equals(vkr_zone_site_name(outer->zone), "Test.Outer"));
    }
  }
  printf("  test_zone_threads_record_nested_zones PASSED\n");
}

static void test_zone_stop_waits_for_writes(VkrAllocator *allocator) {
  printf("  Running test_zone_stop_waits_for_writes...\n");
  zone_test_reset();
  vkr_atomic_bool_store(&zone_test_sink.block_writes, true_v,
                        VKR_MEMORY_ORDER_RELEASE);
  const VkrZoneSink sink = {.write = zone_test_write,
                            .context = &zone_test_sink};
  vkr_zone_sink_set(&sink);
  VkrThread handle = NULL;
  assert(vkr_thread_create(allocator, &handle, zone_test_blocked_worker, NULL));
  while (!vkr_atomic_bool_load(&zone_test_sink.write_started,
                               VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_thread_yield();
  }
  /* The write sleeps; the stop must not return before it finishes, or the
     sink's context could be released under it. */
  vkr_zone_sink_set(NULL);
  assert(vkr_atomic_bool_load(&zone_test_sink.write_finished,
                              VKR_MEMORY_ORDER_ACQUIRE));
  assert(vkr_thread_join(handle));
  assert(vkr_thread_destroy(allocator, &handle));
  printf("  test_zone_stop_waits_for_writes PASSED\n");
}

bool32_t run_zone_tests(void) {
  printf("--- Running Zone tests... ---\n");
  Arena *arena = arena_create(MB(1));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  test_zone_without_sink_records_nothing();
  test_zone_signposts_without_sink();
  test_zone_threads_record_nested_zones(&allocator);
  test_zone_stop_waits_for_writes(&allocator);
  arena_destroy(arena);
  printf("--- Zone tests completed. ---\n");
  return true_v;
}
