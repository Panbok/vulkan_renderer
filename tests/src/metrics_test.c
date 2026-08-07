#include "metrics_test.h"

#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vulkan/vulkan_backend.h"

#include "core/vkr_metrics.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

typedef struct MetricsThreadContext {
  VkrMetrics *metrics;
  VkrMetricId counter;
  VkrMetricId gauge;
  VkrMetricId duration;
  uint32_t iterations;
  uint32_t thread_index;
} MetricsThreadContext;

typedef struct MetricsEventThreadContext {
  VkrMetrics *metrics;
  VkrMetricId source;
  uint32_t base;
  uint32_t count;
} MetricsEventThreadContext;

/**
 * @brief Arena-backed fixture.
 *
 * `VkrMetrics` embeds the 4096-entry event ring, so an instance is several
 * hundred kilobytes. Tests allocate it rather than declaring it locally: a
 * stack instance survives the main thread by luck and would overflow a worker.
 */
typedef struct MetricsFixture {
  Arena *arena;
  VkrAllocator allocator;
  VkrMetrics *metrics;
} MetricsFixture;

static MetricsFixture metrics_fixture_create(void) {
  MetricsFixture fixture = {0};
  fixture.arena = arena_create(MB(4), MB(1));
  assert(fixture.arena);
  fixture.allocator = (VkrAllocator){.ctx = fixture.arena};
  assert(vkr_allocator_arena(&fixture.allocator));
  fixture.metrics =
      arena_alloc(fixture.arena, sizeof(VkrMetrics), ARENA_MEMORY_TAG_STRUCT);
  assert(fixture.metrics);
  vkr_metrics_init(fixture.metrics);
  return fixture;
}

static void metrics_fixture_destroy(MetricsFixture *fixture) {
  arena_destroy(fixture->arena);
  *fixture = (MetricsFixture){0};
}

static VkrMetricDescription metrics_desc(const char *name, VkrMetricKind kind,
                                         VkrMetricScalar scalar,
                                         VkrMetricWriter writer) {
  return (VkrMetricDescription){
      .name =
          string8_create_from_cstr((const uint8_t *)name, string_length(name)),
      .domain = VKR_METRIC_DOMAIN_FRAME,
      .kind = kind,
      .unit = kind == VKR_METRIC_KIND_DURATION ? VKR_METRIC_UNIT_NANOSECONDS
                                               : VKR_METRIC_UNIT_COUNT,
      .scalar = scalar,
      .writer = writer,
      .required_when_enabled = true_v,
  };
}

static void *metrics_counter_thread(void *data) {
  MetricsThreadContext *ctx = data;
  for (uint32_t i = 0; i < ctx->iterations; ++i) {
    vkr_metrics_counter_add(ctx->metrics, ctx->counter, 1u);
    vkr_metrics_gauge_set_u64(ctx->metrics, ctx->gauge,
                              ctx->thread_index * ctx->iterations + i);
    vkr_metrics_duration_add_ns(ctx->metrics, ctx->duration, 1u);
  }
  return NULL;
}

static void *metrics_event_thread(void *data) {
  MetricsEventThreadContext *ctx = data;
  for (uint32_t i = 0; i < ctx->count; ++i) {
    const VkrMetricEvent event = {
        .source = ctx->source,
        .duration_ns = 1u,
        .bytes = ctx->base + i,
    };
    const bool8_t pushed =
        vkr_metrics_event_push(ctx->metrics, &event, string8_lit("event"));
    assert(pushed);
    (void)pushed;
  }
  return NULL;
}

static void test_metrics_registration_and_samples(void) {
  printf("  Running test_metrics_registration_and_samples...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId counter = VKR_METRIC_ID_INVALID;
  VkrMetricId gauge = VKR_METRIC_ID_INVALID;
  VkrMetricId duration = VKR_METRIC_ID_INVALID;
  VkrMetricId concurrent = VKR_METRIC_ID_INVALID;

  VkrMetricDescription counter_desc =
      metrics_desc("draw.calls", VKR_METRIC_KIND_COUNTER, VKR_METRIC_SCALAR_U64,
                   VKR_METRIC_WRITER_RENDER_THREAD);
  VkrMetricDescription gauge_desc =
      metrics_desc("frame.ratio", VKR_METRIC_KIND_GAUGE, VKR_METRIC_SCALAR_F64,
                   VKR_METRIC_WRITER_RENDER_THREAD);
  VkrMetricDescription duration_desc =
      metrics_desc("cpu.submit", VKR_METRIC_KIND_DURATION,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD);
  VkrMetricDescription concurrent_desc =
      metrics_desc("jobs.completed", VKR_METRIC_KIND_COUNTER,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);

  assert(vkr_metrics_register(metrics, &counter_desc, &counter));
  assert(vkr_metrics_register(metrics, &gauge_desc, &gauge));
  assert(vkr_metrics_register(metrics, &duration_desc, &duration));
  assert(vkr_metrics_register(metrics, &concurrent_desc, &concurrent));
  VkrMetricId duplicate = VKR_METRIC_ID_INVALID;
  assert(!vkr_metrics_register(metrics, &counter_desc, &duplicate));
  VkrMetricDescription invalid =
      metrics_desc("Bad Name", VKR_METRIC_KIND_COUNTER, VKR_METRIC_SCALAR_U64,
                   VKR_METRIC_WRITER_RENDER_THREAD);
  assert(!vkr_metrics_register(metrics, &invalid, &duplicate));
  assert(vkr_metrics_seal(metrics));
  assert(!vkr_metrics_register(metrics, &invalid, &duplicate));

  vkr_metrics_counter_add(metrics, concurrent, 7u);
  vkr_metrics_begin_frame(metrics, 12u, 4u);
  vkr_metrics_counter_add(metrics, counter, 3u);
  vkr_metrics_counter_add(metrics, counter, 2u);
  vkr_metrics_gauge_set_f64(metrics, gauge, 0.25);
  vkr_metrics_duration_add_ns(metrics, duration, 30u);
  vkr_metrics_duration_add_ns(metrics, duration, 10u);
  assert(vkr_metrics_end_frame(metrics));

  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  assert(view.frame->cpu_frame_index == 12u);
  assert(view.frame->submit_serial == 4u);

  uint64_t u64_value = 0;
  float64_t f64_value = 0.0;
  VkrMetricDurationSample duration_sample = {0};
  assert(vkr_metrics_frame_read_u64(view.frame, counter, &u64_value) &&
         u64_value == 5u);
  assert(vkr_metrics_frame_read_f64(view.frame, gauge, &f64_value) &&
         f64_value == 0.25);
  assert(
      vkr_metrics_frame_read_duration(view.frame, duration, &duration_sample));
  assert(duration_sample.sum_ns == 40u);
  assert(duration_sample.count == 2u);
  assert(duration_sample.min_ns == 10u);
  assert(duration_sample.max_ns == 30u);
  assert(vkr_metrics_frame_read_u64(view.frame, concurrent, &u64_value) &&
         u64_value == 7u);

  // Typed readers must refuse a union member the producer did not write.
  assert(!vkr_metrics_frame_read_f64(view.frame, counter, &f64_value));
  assert(!vkr_metrics_frame_read_u64(view.frame, duration, &u64_value));
  assert(!vkr_metrics_frame_read_u64(view.frame, gauge, &u64_value));
  assert(!vkr_metrics_frame_read_duration(view.frame, gauge, &duration_sample));
  vkr_metrics_snapshot_release(metrics, &view);

  vkr_metrics_counter_add(metrics, concurrent, 2u);
  vkr_metrics_begin_frame(metrics, 13u, 5u);
  assert(vkr_metrics_end_frame(metrics));
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  // A counter nobody incremented is a valid zero, not an absent sample: "no
  // draws this frame" and "draws were not measured" are different claims.
  assert(vkr_metrics_frame_get(view.frame, counter)->availability ==
         VKR_METRIC_AVAILABILITY_VALID);
  assert(vkr_metrics_frame_read_u64(view.frame, counter, &u64_value) &&
         u64_value == 0u);
  // A gauge nobody set stays absent; there is no meaningful default for it.
  assert(vkr_metrics_frame_get(view.frame, gauge)->availability ==
         VKR_METRIC_AVAILABILITY_UNAVAILABLE);
  assert(vkr_metrics_frame_get(view.frame, gauge)->reason ==
         VKR_METRIC_REASON_NOT_SAMPLED);
  assert(!vkr_metrics_frame_read_f64(view.frame, gauge, &f64_value));
  assert(vkr_metrics_frame_read_u64(view.frame, concurrent, &u64_value) &&
         u64_value == 2u);
  // gauge and duration were declared required and went unsampled.
  assert(vkr_metrics_frame_missing_required(metrics, view.frame) == 2u);
  vkr_metrics_snapshot_release(metrics, &view);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_registration_and_samples PASSED\n");
}

static void test_metrics_availability_marking(void) {
  printf("  Running test_metrics_availability_marking...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId exact = VKR_METRIC_ID_INVALID;
  VkrMetricId unsupported = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("memory.gpu.bytes", VKR_METRIC_KIND_GAUGE,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD);
  assert(vkr_metrics_register(metrics, &description, &exact));
  description =
      metrics_desc("memory.gpu.budget", VKR_METRIC_KIND_GAUGE,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD);
  assert(vkr_metrics_register(metrics, &description, &unsupported));
  assert(vkr_metrics_seal(metrics));

  vkr_metrics_begin_frame(metrics, 1u, 1u);
  vkr_metrics_gauge_set_u64(metrics, exact, 4096u);
  // An inexact source keeps its value but is flagged, so a consumer can report
  // the number without presenting it as exact evidence.
  vkr_metrics_mark(metrics, exact, VKR_METRIC_AVAILABILITY_INEXACT,
                   VKR_METRIC_REASON_SOURCE_INEXACT);
  vkr_metrics_mark(metrics, unsupported, VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);
  assert(vkr_metrics_end_frame(metrics));

  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  const VkrMetricSample *sample = vkr_metrics_frame_get(view.frame, exact);
  assert(sample->availability == VKR_METRIC_AVAILABILITY_INEXACT);
  assert(sample->reason == VKR_METRIC_REASON_SOURCE_INEXACT);
  uint64_t value = 0;
  assert(vkr_metrics_frame_read_u64(view.frame, exact, &value) &&
         value == 4096u);

  sample = vkr_metrics_frame_get(view.frame, unsupported);
  assert(sample->availability == VKR_METRIC_AVAILABILITY_UNAVAILABLE);
  assert(sample->reason == VKR_METRIC_REASON_UNSUPPORTED);
  assert(!vkr_metrics_frame_read_u64(view.frame, unsupported, &value));
  assert(vkr_metrics_frame_missing_required(metrics, view.frame) == 1u);
  vkr_metrics_snapshot_release(metrics, &view);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_availability_marking PASSED\n");
}

static void test_metrics_concurrent_counter(void) {
  printf("  Running test_metrics_concurrent_counter...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId counter = VKR_METRIC_ID_INVALID;
  VkrMetricId gauge = VKR_METRIC_ID_INVALID;
  VkrMetricId duration = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("jobs.completed", VKR_METRIC_KIND_COUNTER,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &counter));
  description =
      metrics_desc("jobs.queue_depth", VKR_METRIC_KIND_GAUGE,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &gauge));
  description =
      metrics_desc("jobs.busy", VKR_METRIC_KIND_DURATION, VKR_METRIC_SCALAR_U64,
                   VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &duration));
  assert(vkr_metrics_seal(metrics));

  enum { THREAD_COUNT = 4, ITERATIONS = 25000 };
  VkrThread threads[THREAD_COUNT] = {0};
  MetricsThreadContext contexts[THREAD_COUNT] = {0};
  for (uint32_t i = 0; i < THREAD_COUNT; ++i) {
    contexts[i] = (MetricsThreadContext){
        .metrics = metrics,
        .counter = counter,
        .gauge = gauge,
        .duration = duration,
        .iterations = ITERATIONS,
        .thread_index = i,
    };
    assert(vkr_thread_create(&fixture.allocator, &threads[i],
                             metrics_counter_thread, &contexts[i]));
  }
  for (uint32_t i = 0; i < THREAD_COUNT; ++i) {
    assert(vkr_thread_join(threads[i]));
    assert(vkr_thread_destroy(&fixture.allocator, &threads[i]));
  }

  vkr_metrics_begin_frame(metrics, 1u, 1u);
  assert(vkr_metrics_end_frame(metrics));
  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  uint64_t value = 0;
  assert(vkr_metrics_frame_read_u64(view.frame, counter, &value) &&
         value == THREAD_COUNT * ITERATIONS);
  assert(vkr_metrics_frame_read_u64(view.frame, gauge, &value) &&
         value < THREAD_COUNT * ITERATIONS);
  VkrMetricDurationSample duration_sample = {0};
  assert(
      vkr_metrics_frame_read_duration(view.frame, duration, &duration_sample));
  assert(duration_sample.sum_ns == THREAD_COUNT * ITERATIONS);
  assert(duration_sample.count == THREAD_COUNT * ITERATIONS);
  // Cumulative atomics cannot be differenced into extrema, so concurrent
  // duration slots publish sum/count only.
  assert(duration_sample.min_ns == 0u);
  assert(duration_sample.max_ns == 0u);
  vkr_metrics_snapshot_release(metrics, &view);

  vkr_metrics_gauge_set_u64(metrics, gauge, 73u);
  vkr_metrics_duration_add_ns(metrics, duration, 9u);
  vkr_metrics_duration_add_ns(metrics, duration, 9u);
  vkr_metrics_begin_frame(metrics, 2u, 2u);
  assert(vkr_metrics_end_frame(metrics));
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  assert(vkr_metrics_frame_read_u64(view.frame, gauge, &value) && value == 73u);
  assert(
      vkr_metrics_frame_read_duration(view.frame, duration, &duration_sample));
  assert(duration_sample.sum_ns == 18u);
  assert(duration_sample.count == 2u);
  vkr_metrics_snapshot_release(metrics, &view);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_concurrent_counter PASSED\n");
}

static void test_metrics_snapshot_pinning_and_drop(void) {
  printf("  Running test_metrics_snapshot_pinning_and_drop...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId counter = VKR_METRIC_ID_INVALID;
  VkrMetricId concurrent = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("frame.count", VKR_METRIC_KIND_COUNTER,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD);
  assert(vkr_metrics_register(metrics, &description, &counter));
  description =
      metrics_desc("job.completed", VKR_METRIC_KIND_COUNTER,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &concurrent));
  assert(vkr_metrics_seal(metrics));

  VkrMetricsSnapshotView views[VKR_METRICS_SNAPSHOT_BUFFER_COUNT] = {0};
  for (uint32_t i = 0; i < VKR_METRICS_SNAPSHOT_BUFFER_COUNT; ++i) {
    vkr_metrics_begin_frame(metrics, i, i);
    vkr_metrics_counter_add(metrics, counter, i + 1u);
    assert(vkr_metrics_end_frame(metrics));
    assert(vkr_metrics_snapshot_acquire(metrics, &views[i]));
  }

  // Every buffer is pinned: the writer drops rather than waiting or writing
  // into a buffer a reader still holds.
  vkr_metrics_begin_frame(metrics, 99u, 99u);
  vkr_metrics_counter_add(metrics, counter, 99u);
  vkr_metrics_counter_add(metrics, concurrent, 11u);
  assert(!vkr_metrics_end_frame(metrics));
  assert(metrics->snapshot_publications_dropped == 1u);

  for (uint32_t i = 0; i < VKR_METRICS_SNAPSHOT_BUFFER_COUNT; ++i) {
    vkr_metrics_snapshot_release(metrics, &views[i]);
  }
  vkr_metrics_begin_frame(metrics, 100u, 100u);
  vkr_metrics_counter_add(metrics, concurrent, 7u);
  assert(vkr_metrics_end_frame(metrics));
  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  assert(view.frame->snapshot_publications_dropped == 1u);
  // The dropped frame's 11 must not reappear here: a dropped publication is
  // still a completed interval, so its baseline was advanced.
  uint64_t value = 0;
  assert(vkr_metrics_frame_read_u64(view.frame, concurrent, &value) &&
         value == 7u);
  vkr_metrics_snapshot_release(metrics, &view);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_snapshot_pinning_and_drop PASSED\n");
}

static void test_metrics_event_ring(void) {
  printf("  Running test_metrics_event_ring...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  metrics->config.event_subjects = true_v;
  VkrMetricId source = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("asset.load", VKR_METRIC_KIND_DURATION,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &source));
  assert(vkr_metrics_seal(metrics));

  char long_subject[VKR_METRIC_EVENT_SUBJECT_MAX + 8u];
  MemSet(long_subject, 'x', sizeof(long_subject));
  String8 subject =
      string8_create((uint8_t *)long_subject, sizeof(long_subject));
  VkrMetricEvent event = {.source = source, .duration_ns = 10u};
  for (uint32_t i = 0; i < VKR_METRIC_EVENT_CAPACITY; ++i) {
    event.bytes = i;
    assert(vkr_metrics_event_push(metrics, &event, subject));
  }
  assert(!vkr_metrics_event_push(metrics, &event, subject));
  assert(vkr_atomic_uint64_load(&metrics->events_dropped,
                                VKR_MEMORY_ORDER_RELAXED) == 1u);
  assert(vkr_atomic_uint64_load(&metrics->event_subjects_truncated,
                                VKR_MEMORY_ORDER_RELAXED) ==
         VKR_METRIC_EVENT_CAPACITY);

  VkrMetricEvent peeked = {0};
  assert(vkr_metrics_event_peek(metrics, 0u, &peeked));
  assert(peeked.bytes == 0u);
  assert(!vkr_metrics_event_consume(metrics, VKR_METRIC_EVENT_CAPACITY + 1u));
  assert(vkr_metrics_event_peek(metrics, 0u, &peeked));
  assert(peeked.bytes == 0u);

  VkrMetricEvent popped = {0};
  for (uint32_t i = 0; i < VKR_METRIC_EVENT_CAPACITY; ++i) {
    assert(vkr_metrics_event_pop(metrics, &popped));
    assert(popped.bytes == i);
    assert(popped.subject_length == VKR_METRIC_EVENT_SUBJECT_MAX);
    assert(popped.subject_truncated);
  }
  assert(!vkr_metrics_event_pop(metrics, &popped));
  assert(vkr_metrics_event_push(metrics, &event, string8_lit("mesh")));
  assert(vkr_metrics_event_pop(metrics, &popped));
  assert(popped.subject_length == 4u);
  assert(!popped.subject_truncated);

  event.bytes = 41u;
  assert(vkr_metrics_event_push(metrics, &event, string8_lit("one")));
  event.bytes = 42u;
  assert(vkr_metrics_event_push(metrics, &event, string8_lit("two")));
  assert(!vkr_metrics_event_consume(metrics, 3u));
  assert(vkr_metrics_event_peek(metrics, 0u, &peeked));
  assert(peeked.bytes == 41u);
  assert(vkr_metrics_event_consume(metrics, 2u));
  assert(!vkr_metrics_event_peek(metrics, 0u, &peeked));

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_event_ring PASSED\n");
}

static void test_metrics_event_subject_policy(void) {
  printf("  Running test_metrics_event_subject_policy...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId source = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("asset.texture_load", VKR_METRIC_KIND_DURATION,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &source));
  assert(vkr_metrics_seal(metrics));

  // Subjects off (the default): the event still publishes, without the string.
  const VkrMetricEvent event = {.source = source, .duration_ns = 5u};
  assert(vkr_metrics_event_push(metrics, &event, string8_lit("a/b/c.png")));
  VkrMetricEvent popped = {0};
  assert(vkr_metrics_event_pop(metrics, &popped));
  assert(popped.subject_length == 0u);
  assert(!popped.subject_truncated);
  assert(vkr_atomic_uint64_load(&metrics->event_subjects_truncated,
                                VKR_MEMORY_ORDER_RELAXED) == 0u);

  // Truncation must land on a UTF-8 boundary, never mid-codepoint, or the
  // emitted report would not be valid UTF-8.
  metrics->config.event_subjects = true_v;
  uint8_t wide[VKR_METRIC_EVENT_SUBJECT_MAX + 4u];
  MemSet(wide, 'a', sizeof(wide));
  // Place a 3-byte sequence (U+20AC) straddling the truncation boundary.
  wide[VKR_METRIC_EVENT_SUBJECT_MAX - 1u] = 0xE2u;
  wide[VKR_METRIC_EVENT_SUBJECT_MAX] = 0x82u;
  wide[VKR_METRIC_EVENT_SUBJECT_MAX + 1u] = 0xACu;
  assert(vkr_metrics_event_push(metrics, &event,
                                string8_create(wide, sizeof(wide))));
  assert(vkr_metrics_event_pop(metrics, &popped));
  assert(popped.subject_truncated);
  assert(popped.subject_length == VKR_METRIC_EVENT_SUBJECT_MAX - 1u);
  assert((uint8_t)popped.subject[popped.subject_length - 1u] == 'a');

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_event_subject_policy PASSED\n");
}

static void test_metrics_event_record_status(void) {
  printf("  Running test_metrics_event_record_status...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId source = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("pipeline.create", VKR_METRIC_KIND_DURATION,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &source));
  assert(vkr_metrics_seal(metrics));

  const VkrMetricEventProducer producer = {metrics, source};
  assert(vkr_metrics_event_record(producer, string8_lit("ok.slang"), 100u, 40u,
                                  8u, VKR_METRIC_EVENT_STATUS_SUCCESS));
  assert(vkr_metrics_event_record(producer, string8_lit("bad.slang"), 200u,
                                  9000u, 0u, VKR_METRIC_EVENT_STATUS_FAILED));

  vkr_metrics_begin_frame(metrics, 1u, 1u);
  assert(vkr_metrics_end_frame(metrics));
  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(metrics, &view));
  VkrMetricDurationSample duration = {0};
  assert(vkr_metrics_frame_read_duration(view.frame, source, &duration));
  // The failure's 9000ns must not widen the aggregate: `pipeline.create`
  // describes the cost of creating a pipeline, not of failing to.
  assert(duration.sum_ns == 40u);
  assert(duration.count == 1u);
  vkr_metrics_snapshot_release(metrics, &view);

  VkrMetricEvent popped = {0};
  assert(vkr_metrics_event_pop(metrics, &popped));
  assert(popped.status == VKR_METRIC_EVENT_STATUS_SUCCESS);
  assert(popped.duration_ns == 40u && popped.bytes == 8u);
  assert(vkr_metrics_event_pop(metrics, &popped));
  // The failure is still visible as an event, just not in the aggregate.
  assert(popped.status == VKR_METRIC_EVENT_STATUS_FAILED);
  assert(popped.duration_ns == 9000u);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_event_record_status PASSED\n");
}

static void test_metrics_event_ring_mpsc(void) {
  printf("  Running test_metrics_event_ring_mpsc...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrMetrics *metrics = fixture.metrics;
  VkrMetricId source = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("asset.load", VKR_METRIC_KIND_DURATION,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_CONCURRENT);
  assert(vkr_metrics_register(metrics, &description, &source));
  assert(vkr_metrics_seal(metrics));

  enum { THREAD_COUNT = 4, EVENTS_PER_THREAD = 512, EVENT_COUNT = 2048 };
  VkrThread threads[THREAD_COUNT] = {0};
  MetricsEventThreadContext contexts[THREAD_COUNT] = {0};
  for (uint32_t i = 0; i < THREAD_COUNT; ++i) {
    contexts[i] = (MetricsEventThreadContext){
        .metrics = metrics,
        .source = source,
        .base = i * EVENTS_PER_THREAD,
        .count = EVENTS_PER_THREAD,
    };
    assert(vkr_thread_create(&fixture.allocator, &threads[i],
                             metrics_event_thread, &contexts[i]));
  }
  for (uint32_t i = 0; i < THREAD_COUNT; ++i) {
    assert(vkr_thread_join(threads[i]));
    assert(vkr_thread_destroy(&fixture.allocator, &threads[i]));
  }

  bool8_t seen[EVENT_COUNT] = {0};
  VkrMetricEvent event = {0};
  for (uint32_t i = 0; i < EVENT_COUNT; ++i) {
    assert(vkr_metrics_event_pop(metrics, &event));
    assert(event.bytes < EVENT_COUNT);
    assert(!seen[event.bytes]);
    seen[event.bytes] = true_v;
  }
  assert(!vkr_metrics_event_pop(metrics, &event));
  assert(vkr_atomic_uint64_load(&metrics->events_dropped,
                                VKR_MEMORY_ORDER_RELAXED) == 0u);

  metrics_fixture_destroy(&fixture);
  printf("  test_metrics_event_ring_mpsc PASSED\n");
}

static void test_metrics_registry_generation(void) {
  printf("  Running test_metrics_registry_generation...\n");

  MetricsFixture first_fixture = metrics_fixture_create();
  MetricsFixture second_fixture = metrics_fixture_create();
  VkrMetrics *first = first_fixture.metrics;
  VkrMetrics *second = second_fixture.metrics;
  VkrMetricId first_id = VKR_METRIC_ID_INVALID;
  VkrMetricId second_id = VKR_METRIC_ID_INVALID;
  VkrMetricDescription description =
      metrics_desc("frame.count", VKR_METRIC_KIND_COUNTER,
                   VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD);
  assert(vkr_metrics_register(first, &description, &first_id));
  assert(vkr_metrics_register(second, &description, &second_id));
  assert(vkr_metric_id_index(first_id) == vkr_metric_id_index(second_id));
  assert(vkr_metric_id_generation(first_id) !=
         vkr_metric_id_generation(second_id));
  assert(vkr_metrics_seal(first));
  assert(vkr_metrics_seal(second));

  vkr_metrics_begin_frame(second, 1u, 1u);
  vkr_metrics_counter_add(second, second_id, 1u);
  assert(vkr_metrics_end_frame(second));
  VkrMetricsSnapshotView view = {0};
  assert(vkr_metrics_snapshot_acquire(second, &view));
  assert(vkr_metrics_frame_get(view.frame, second_id));
  // A handle from another registry must not resolve, even though its slot
  // index is identical.
  assert(!vkr_metrics_frame_get(view.frame, first_id));
  vkr_metrics_snapshot_release(second, &view);

  const VkrMetricEvent stale_event = {.source = first_id};
  assert(!vkr_metrics_event_push(second, &stale_event, string8_lit("stale")));

  metrics_fixture_destroy(&first_fixture);
  metrics_fixture_destroy(&second_fixture);
  printf("  test_metrics_registry_generation PASSED\n");
}

static VkDeviceMemory test_device_memory_handle(uintptr_t value) {
  return (VkDeviceMemory)value;
}

static void test_device_memory_owner_tracking(void) {
  printf("  Running test_device_memory_owner_tracking...\n");

  VulkanDeviceMemoryEntry entries[8] = {0};
  VulkanDeviceMemoryStats stats = {
      .entries = entries,
      .entry_capacity = 8,
      .tracking_exact = true_v,
  };
  // These two handles collide in the eight-slot table. Freeing the first must
  // reinsert the second so owner attribution remains reachable.
  const VkDeviceMemory mesh_a = test_device_memory_handle(0x10u);
  const VkDeviceMemory font = test_device_memory_handle(0x90u);
  const VkDeviceMemory mesh_b = test_device_memory_handle(0x20u);
  vulkan_device_memory_stats_record_alloc(&stats, mesh_a, 100u, 2u,
                                          VKR_GPU_ALLOCATION_OWNER_MESH);
  vulkan_device_memory_stats_record_alloc(&stats, font, 50u, 2u,
                                          VKR_GPU_ALLOCATION_OWNER_FONT);
  vulkan_device_memory_stats_record_alloc(&stats, mesh_b, 25u, 3u,
                                          VKR_GPU_ALLOCATION_OWNER_MESH);

  const VkrGpuAllocationOwnerTotals *mesh =
      &stats.owners[VKR_GPU_ALLOCATION_OWNER_MESH];
  const VkrGpuAllocationOwnerTotals *font_totals =
      &stats.owners[VKR_GPU_ALLOCATION_OWNER_FONT];
  assert(stats.live_allocation_count == 3u);
  assert(stats.live_bytes == 175u);
  assert(mesh->live_allocation_count == 2u);
  assert(mesh->live_bytes == 125u);
  assert(mesh->peak_bytes == 125u);
  assert(font_totals->total_bytes == 50u);
  assert(stats.live_count_by_type[2] == 2u);

  vulkan_device_memory_stats_record_free(&stats, mesh_a);
  vulkan_device_memory_stats_record_free(&stats, font);
  assert(stats.live_allocation_count == 1u);
  assert(stats.live_bytes == 25u);
  assert(font_totals->live_allocation_count == 0u);
  // Peak and cumulative totals survive the frees that live counts do not.
  assert(mesh->peak_allocation_count == 2u);
  assert(mesh->total_allocation_count == 2u);

  const VkDeviceMemory invalid_owner = test_device_memory_handle(0x30u);
  vulkan_device_memory_stats_record_alloc(
      &stats, invalid_owner, 7u, 4u,
      (VkrGpuAllocationOwner)VKR_GPU_ALLOCATION_OWNER_COUNT);
  assert(stats.owners[VKR_GPU_ALLOCATION_OWNER_UNKNOWN].live_bytes == 7u);
  vulkan_device_memory_stats_record_free(&stats, invalid_owner);
  vulkan_device_memory_stats_record_free(&stats, mesh_b);
  assert(stats.live_allocation_count == 0u);
  assert(stats.live_bytes == 0u);

  VulkanDeviceMemoryEntry short_entries[2] = {0};
  VulkanDeviceMemoryStats saturated = {
      .entries = short_entries,
      .entry_capacity = 2,
      .tracking_exact = true_v,
  };
  const VkDeviceMemory first = test_device_memory_handle(0x10u);
  const VkDeviceMemory second = test_device_memory_handle(0x20u);
  const VkDeviceMemory overflow = test_device_memory_handle(0x30u);
  vulkan_device_memory_stats_record_alloc(&saturated, first, 10u, 0u,
                                          VKR_GPU_ALLOCATION_OWNER_STAGING);
  vulkan_device_memory_stats_record_alloc(&saturated, second, 20u, 0u,
                                          VKR_GPU_ALLOCATION_OWNER_STAGING);
  vulkan_device_memory_stats_record_alloc(&saturated, overflow, 30u, 0u,
                                          VKR_GPU_ALLOCATION_OWNER_READBACK);
  assert(!saturated.tracking_exact);
  assert(saturated.total_allocation_count == 3u);
  // The overflowing allocation never entered the table, so its cumulative row
  // is still exact while the live figures it feeds are not.
  assert(saturated.owners[VKR_GPU_ALLOCATION_OWNER_READBACK].total_bytes ==
         30u);
  vulkan_device_memory_stats_record_free(&saturated, first);
  vulkan_device_memory_stats_record_free(&saturated, second);
  assert(saturated.live_allocation_count == 1u);
  assert(saturated.live_bytes == 30u);
  assert(saturated.owners[VKR_GPU_ALLOCATION_OWNER_READBACK].live_bytes == 30u);

  printf("  test_device_memory_owner_tracking PASSED\n");
}

static void test_renderer_owner_metric_catalog(void) {
  printf("  Running test_renderer_owner_metric_catalog...\n");

  MetricsFixture fixture = metrics_fixture_create();
  VkrRendererMetrics renderer_metrics = {0};
  assert(vkr_renderer_metrics_register(&renderer_metrics, fixture.metrics));
  assert(renderer_metrics.previous.gpu_memory_interval_contiguous);

  uint32_t catalog_count = 0;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(fixture.metrics, &catalog_count);

  // Every owner publishes every row, under a name the report contract fixes.
  // Spot-checking two rows would not catch a bucket whose name went missing.
  static const char *const owner_names[VKR_GPU_ALLOCATION_OWNER_COUNT] = {
      "unknown",  "mesh",     "texture", "font",     "render_graph", "shader",
      "instance", "indirect", "staging", "readback", "swapchain",
  };
  static const struct {
    const char *suffix;
    VkrMetricKind kind;
    VkrMetricUnit unit;
  } rows[VKR_GPU_OWNER_METRIC_ROW_COUNT] = {
      [VKR_GPU_OWNER_METRIC_ROW_LIVE_BYTES] = {"bytes.live",
                                               VKR_METRIC_KIND_GAUGE,
                                               VKR_METRIC_UNIT_BYTES},
      [VKR_GPU_OWNER_METRIC_ROW_PEAK_BYTES] = {"bytes.peak",
                                               VKR_METRIC_KIND_GAUGE,
                                               VKR_METRIC_UNIT_BYTES},
      [VKR_GPU_OWNER_METRIC_ROW_ALLOCATED_BYTES] = {"bytes.allocated",
                                                    VKR_METRIC_KIND_COUNTER,
                                                    VKR_METRIC_UNIT_BYTES},
      [VKR_GPU_OWNER_METRIC_ROW_LIVE_ALLOCATIONS] = {"allocations.live",
                                                     VKR_METRIC_KIND_GAUGE,
                                                     VKR_METRIC_UNIT_COUNT},
      [VKR_GPU_OWNER_METRIC_ROW_PEAK_ALLOCATIONS] = {"allocations.peak",
                                                     VKR_METRIC_KIND_GAUGE,
                                                     VKR_METRIC_UNIT_COUNT},
      [VKR_GPU_OWNER_METRIC_ROW_CREATED_ALLOCATIONS] = {"allocations.created",
                                                        VKR_METRIC_KIND_COUNTER,
                                                        VKR_METRIC_UNIT_COUNT},
  };

  const uint32_t aggregate_counter_index =
      vkr_metric_id_index(renderer_metrics.ids.gpu_allocations_created);
  assert(aggregate_counter_index < catalog_count);
  assert(strcmp(catalog[aggregate_counter_index].name,
                "memory.gpu.allocations.created") == 0);
  assert(catalog[aggregate_counter_index].kind == VKR_METRIC_KIND_COUNTER);
  assert(catalog[aggregate_counter_index].unit == VKR_METRIC_UNIT_COUNT);

  const uint32_t command_wait_index =
      vkr_metric_id_index(renderer_metrics.ids.frame_command_slot_waits);
  assert(command_wait_index < catalog_count);
  assert(strcmp(catalog[command_wait_index].name, "frame.command_slot_waits") ==
         0);
  assert(catalog[command_wait_index].domain == VKR_METRIC_DOMAIN_FRAME);
  assert(catalog[command_wait_index].kind == VKR_METRIC_KIND_GAUGE);
  assert(catalog[command_wait_index].unit == VKR_METRIC_UNIT_COUNT);

  for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
    for (uint32_t row = 0; row < VKR_GPU_OWNER_METRIC_ROW_COUNT; ++row) {
      const VkrMetricId id = renderer_metrics.ids.gpu_owner[owner][row];
      assert(id != VKR_METRIC_ID_INVALID);
      const uint32_t index = vkr_metric_id_index(id);
      assert(index < catalog_count);

      char expected[64];
      snprintf(expected, sizeof(expected), "memory.gpu.owner.%s.%s",
               owner_names[owner], rows[row].suffix);
      assert(strcmp(catalog[index].name, expected) == 0);
      assert(catalog[index].unit == rows[row].unit);
      assert(catalog[index].kind == rows[row].kind);
      assert(catalog[index].scalar == VKR_METRIC_SCALAR_U64);
      assert(catalog[index].domain == VKR_METRIC_DOMAIN_MEMORY_GPU);
    }
  }

  metrics_fixture_destroy(&fixture);
  printf("  test_renderer_owner_metric_catalog PASSED\n");
}

static void test_renderer_cumulative_delta(void) {
  printf("  Running test_renderer_cumulative_delta...\n");

  uint64_t previous = 0;
  assert(vkr_renderer_metrics_cumulative_delta(17u, &previous) == 17u);
  assert(previous == 17u);
  assert(vkr_renderer_metrics_cumulative_delta(23u, &previous) == 6u);
  assert(previous == 23u);
  assert(vkr_renderer_metrics_cumulative_delta(23u, &previous) == 0u);
  // A reset cannot be attributed to one frame. Match the other process-lifetime
  // pull sources: publish zero for the reset interval and resume from the new
  // baseline on the following frame.
  assert(vkr_renderer_metrics_cumulative_delta(4u, &previous) == 0u);
  assert(previous == 4u);
  assert(vkr_renderer_metrics_cumulative_delta(9u, &previous) == 5u);

  printf("  test_renderer_cumulative_delta PASSED\n");
}

static void test_renderer_pass_sample_publication(void) {
  printf("  Running test_renderer_pass_sample_publication...\n");
  VkrRendererMetricsPassSample storage[2] = {0};
  VkrRendererMetricsPassSample source[3] = {
      {.name = "shadow", .name_length = 6, .gpu_ms = 0.25, .gpu_valid = true_v},
      {.name = "opaque", .name_length = 6, .gpu_ms = 0.50, .gpu_valid = true_v},
      {.name = "tonemap",
       .name_length = 7,
       .gpu_ms = 0.10,
       .gpu_valid = true_v},
  };
  VkrRendererMetrics metrics = {
      .passes = {.samples = storage, .capacity = ArrayCount(storage)},
  };
  assert(vkr_renderer_metrics_publish_pass_samples(&metrics, source,
                                                   ArrayCount(source), 42u));
  assert(metrics.passes.count == ArrayCount(storage));
  assert(metrics.passes.truncated);
  assert(metrics.passes.cpu_frame_index == 42u);
  assert(storage[0].cpu_frame_index == 42u);
  assert(storage[1].cpu_frame_index == 42u);
  assert(strcmp(storage[0].name, "shadow") == 0);
  assert(storage[1].gpu_ms == 0.50);
  assert(!vkr_renderer_metrics_publish_pass_samples(NULL, source, 1u, 0u));
  printf("  test_renderer_pass_sample_publication PASSED\n");
}

bool32_t run_metrics_tests(void) {
  printf("--- Running Metrics tests... ---\n");
  test_metrics_registration_and_samples();
  test_metrics_availability_marking();
  test_metrics_concurrent_counter();
  test_metrics_snapshot_pinning_and_drop();
  test_metrics_event_ring();
  test_metrics_event_subject_policy();
  test_metrics_event_record_status();
  test_metrics_event_ring_mpsc();
  test_metrics_registry_generation();
  test_device_memory_owner_tracking();
  test_renderer_owner_metric_catalog();
  test_renderer_cumulative_delta();
  test_renderer_pass_sample_publication();
  printf("--- Metrics tests completed. ---\n");
  return true_v;
}
