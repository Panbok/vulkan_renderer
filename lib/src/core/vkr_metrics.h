/**
 * @file vkr_metrics.h
 * @brief Bounded typed metrics storage with non-blocking frame publication.
 */
#pragma once

#include "containers/str.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "platform/vkr_platform.h"

#ifndef VKR_METRICS_ENABLED
#define VKR_METRICS_ENABLED 1
#endif

/**
 * Catalog capacity. The renderer adapter's fixed set plus its worst-case
 * device-dependent rows (2 per memory type, 3 per heap, 7 per shadow cascade)
 * must fit with headroom; exhaustion degrades instrumentation and is never
 * allowed to fail startup. See vkr_renderer_metrics_register_device_memory().
 */
#define VKR_METRICS_MAX_SLOTS 384u
#define VKR_METRIC_NAME_MAX 63u
#define VKR_METRICS_SNAPSHOT_BUFFER_COUNT 3u
#define VKR_METRIC_EVENT_CAPACITY 4096u
#define VKR_METRIC_EVENT_SUBJECT_MAX 95u
#define VKR_METRIC_ID_INVALID UINT32_MAX

/**
 * Stable metric handle: low 16 bits are the slot, high 16 bits are the
 * registry generation. The generation catches handles retained across a
 * registry teardown/reinitialization in debug builds.
 */
typedef uint32_t VkrMetricId;

typedef enum VkrMetricKind {
  VKR_METRIC_KIND_COUNTER,  /**< Interval u64; cumulative sources are
                               differenced. */
  VKR_METRIC_KIND_GAUGE,    /**< Instantaneous u64 or f64. */
  VKR_METRIC_KIND_DURATION, /**< Integer nanoseconds; sum/count/min/max. */
} VkrMetricKind;

typedef enum VkrMetricDomain {
  VKR_METRIC_DOMAIN_FRAME,
  VKR_METRIC_DOMAIN_RENDERGRAPH,
  VKR_METRIC_DOMAIN_DRAW,
  VKR_METRIC_DOMAIN_MEMORY_CPU,
  VKR_METRIC_DOMAIN_MEMORY_GPU,
  VKR_METRIC_DOMAIN_ASSET,
  VKR_METRIC_DOMAIN_PIPELINE,
  VKR_METRIC_DOMAIN_JOB,
  VKR_METRIC_DOMAIN_UPLOAD,
  VKR_METRIC_DOMAIN_BOOT,
  VKR_METRIC_DOMAIN_COUNT,
} VkrMetricDomain;

typedef enum VkrMetricWriter {
  VKR_METRIC_WRITER_RENDER_THREAD,
  VKR_METRIC_WRITER_CONCURRENT,
} VkrMetricWriter;

/**
 * Storage units. Durations are always stored and published in nanoseconds;
 * a metric name must never imply a unit the slot does not carry, so there is
 * deliberately no millisecond storage unit.
 */
typedef enum VkrMetricUnit {
  VKR_METRIC_UNIT_COUNT,
  VKR_METRIC_UNIT_BYTES,
  VKR_METRIC_UNIT_NANOSECONDS,
  VKR_METRIC_UNIT_RATIO,
  VKR_METRIC_UNIT_PERCENT,
  VKR_METRIC_UNIT_COUNT_PER_SECOND,
  VKR_METRIC_UNIT_COUNT_MAX,
} VkrMetricUnit;

typedef enum VkrMetricScalar {
  VKR_METRIC_SCALAR_U64,
  VKR_METRIC_SCALAR_F64,
} VkrMetricScalar;

typedef enum VkrMetricAvailability {
  VKR_METRIC_AVAILABILITY_UNAVAILABLE,
  VKR_METRIC_AVAILABILITY_VALID,
  VKR_METRIC_AVAILABILITY_INEXACT,
} VkrMetricAvailability;

typedef enum VkrMetricReason {
  VKR_METRIC_REASON_NONE,
  VKR_METRIC_REASON_NOT_SAMPLED,
  VKR_METRIC_REASON_DISABLED,
  VKR_METRIC_REASON_UNSUPPORTED,
  VKR_METRIC_REASON_NOT_READY,
  VKR_METRIC_REASON_SOURCE_INEXACT,
  VKR_METRIC_REASON_PUBLICATION_DROPPED,
} VkrMetricReason;

/** Whether the operation an event describes succeeded. */
typedef enum VkrMetricEventStatus {
  VKR_METRIC_EVENT_STATUS_SUCCESS,
  VKR_METRIC_EVENT_STATUS_FAILED,
} VkrMetricEventStatus;

typedef struct VkrMetricDescription {
  String8 name;
  VkrMetricDomain domain;
  VkrMetricKind kind;
  VkrMetricUnit unit;
  VkrMetricScalar scalar;
  VkrMetricWriter writer;
  /**
   * This slot must carry a valid sample for a report to be complete. Absence
   * is a reportable defect, not a value to default; see
   * vkr_metrics_frame_missing_required().
   */
  bool8_t required_when_enabled;
} VkrMetricDescription;

typedef struct VkrMetricCatalogEntry {
  char name[VKR_METRIC_NAME_MAX + 1u];
  uint8_t name_length;
  VkrMetricDomain domain;
  VkrMetricKind kind;
  VkrMetricUnit unit;
  VkrMetricScalar scalar;
  VkrMetricWriter writer;
  bool8_t required_when_enabled;
} VkrMetricCatalogEntry;

typedef struct VkrMetricDurationSample {
  uint64_t sum_ns;
  uint64_t count;
  uint64_t min_ns;
  uint64_t max_ns;
} VkrMetricDurationSample;

/**
 * @brief One slot's published value for one frame.
 *
 * `kind` and `scalar` are mirrored from the catalog so that a consumer holding
 * only a published frame can prove which union member is live before reading
 * it. Read through vkr_metrics_frame_read_*(), never by touching `value`.
 */
typedef struct VkrMetricSample {
  union {
    uint64_t u64;
    float64_t f64;
    VkrMetricDurationSample duration;
  } value;
  VkrMetricAvailability availability;
  VkrMetricReason reason;
  VkrMetricKind kind;
  VkrMetricScalar scalar;
} VkrMetricSample;

typedef struct VkrMetricsFrame {
  uint16_t registry_generation;
  uint64_t cpu_frame_index;
  uint64_t submit_serial;
  uint64_t publication_serial;
  uint64_t snapshot_publications_dropped;
  uint64_t events_dropped;
  uint64_t event_subjects_truncated;
  uint32_t slot_count;
  VkrMetricSample samples[VKR_METRICS_MAX_SLOTS];
} VkrMetricsFrame;

typedef struct VkrMetricsSnapshotView {
  const VkrMetricsFrame *frame;
  uint32_t buffer_index;
  uint64_t publication_serial;
} VkrMetricsSnapshotView;

typedef struct VkrMetricEvent {
  VkrMetricId source;
  uint8_t subject_length;
  char subject[VKR_METRIC_EVENT_SUBJECT_MAX + 1u];
  uint64_t start_ns;
  uint64_t duration_ns;
  uint64_t bytes;
  uint32_t thread_id;
  VkrMetricEventStatus status;
  bool8_t subject_truncated;
} VkrMetricEvent;

/**
 * @brief Copyable handle used by low-frequency event producers.
 *
 * The registry owns all storage. Producers retain neither metric names nor
 * event subjects; event publication copies the subject into the bounded ring.
 */
typedef struct VkrMetricEventProducer {
  struct VkrMetrics *metrics;
  VkrMetricId source;
} VkrMetricEventProducer;

typedef struct VkrMetricConcurrentSlot {
  VkrAtomicUint64 value;
  VkrAtomicUint64 aux;
  uint64_t previous_value;
  uint64_t previous_aux;
} VkrMetricConcurrentSlot;

typedef struct VkrMetricEventSlot {
  VkrAtomicUint64 sequence;
  VkrMetricEvent event;
} VkrMetricEventSlot;

/**
 * @brief Runtime instrumentation policy.
 *
 * These flags change what a run measures, so each is part of a run's
 * comparison fingerprint and report. The registry is authoritative: packet
 * construction asks this config instead of keeping a second policy copy.
 */
typedef struct VkrMetricsConfig {
  bool8_t pass_gpu_timings;
  bool8_t submission_gpu_timings;
  bool8_t event_subjects;
} VkrMetricsConfig;

typedef struct VkrMetrics {
  VkrMetricCatalogEntry catalog[VKR_METRICS_MAX_SLOTS];
  VkrMetricSample active[VKR_METRICS_MAX_SLOTS];
  VkrMetricConcurrentSlot concurrent[VKR_METRICS_MAX_SLOTS];
  VkrMetricsFrame published[VKR_METRICS_SNAPSHOT_BUFFER_COUNT];
  VkrAtomicUint32 snapshot_owners[VKR_METRICS_SNAPSHOT_BUFFER_COUNT];
  VkrAtomicUint32 published_index;
  VkrAtomicBool snapshot_available;

  VkrMetricEventSlot events[VKR_METRIC_EVENT_CAPACITY];
  VkrAtomicUint64 event_enqueue_pos;
  VkrAtomicUint64 event_dequeue_pos;
  VkrAtomicUint64 events_dropped;
  VkrAtomicUint64 event_subjects_truncated;

  uint64_t publication_serial;
  uint64_t snapshot_publications_dropped;
  uint64_t active_cpu_frame_index;
  uint64_t active_submit_serial;
  uint32_t slot_count;
  uint16_t registry_generation;
  VkrThreadId render_thread_id;
  VkrMetricsConfig config;
  bool8_t sealed;
  bool8_t frame_active;
} VkrMetrics;

static INLINE uint32_t vkr_metric_id_index(VkrMetricId id) {
  return id & UINT16_MAX;
}

static INLINE uint16_t vkr_metric_id_generation(VkrMetricId id) {
  return (uint16_t)(id >> 16u);
}

void vkr_metrics_init(VkrMetrics *metrics);
bool8_t vkr_metrics_register(VkrMetrics *metrics,
                             const VkrMetricDescription *description,
                             VkrMetricId *out_id);
bool8_t vkr_metrics_seal(VkrMetrics *metrics);

/** Remaining catalog capacity; producers use this to degrade before failing. */
static INLINE uint32_t vkr_metrics_slots_available(const VkrMetrics *metrics) {
  return metrics ? VKR_METRICS_MAX_SLOTS - metrics->slot_count : 0u;
}

#if VKR_METRICS_ENABLED
void vkr_metrics_begin_frame(VkrMetrics *metrics, uint64_t cpu_frame_index,
                             uint64_t submit_serial);
bool8_t vkr_metrics_end_frame(VkrMetrics *metrics);

static INLINE uint32_t vkr_metrics_writer_slot(VkrMetrics *metrics,
                                               VkrMetricId id,
                                               VkrMetricKind kind,
                                               VkrMetricScalar scalar) {
  const uint32_t index = vkr_metric_id_index(id);
#ifndef NDEBUG
  assert(metrics != NULL && metrics->sealed && index < metrics->slot_count);
  assert(vkr_metric_id_generation(id) == metrics->registry_generation);
  const VkrMetricCatalogEntry *entry = &metrics->catalog[index];
  assert(entry->kind == kind && entry->scalar == scalar);
  if (entry->writer == VKR_METRIC_WRITER_RENDER_THREAD) {
    assert(metrics->render_thread_id == vkr_thread_current_id());
    assert(metrics->frame_active);
  }
#else
  (void)kind;
  (void)scalar;
#endif
  return index;
}

static INLINE void vkr_metrics_counter_add(VkrMetrics *metrics, VkrMetricId id,
                                           uint64_t value) {
  const uint32_t index = vkr_metrics_writer_slot(
      metrics, id, VKR_METRIC_KIND_COUNTER, VKR_METRIC_SCALAR_U64);
  if (metrics->catalog[index].writer == VKR_METRIC_WRITER_CONCURRENT) {
    vkr_atomic_uint64_fetch_add(&metrics->concurrent[index].value, value,
                                VKR_MEMORY_ORDER_RELAXED);
    return;
  }
  metrics->active[index].value.u64 += value;
  metrics->active[index].availability = VKR_METRIC_AVAILABILITY_VALID;
  metrics->active[index].reason = VKR_METRIC_REASON_NONE;
}

static INLINE void vkr_metrics_gauge_set_u64(VkrMetrics *metrics,
                                             VkrMetricId id, uint64_t value) {
  const uint32_t index = vkr_metrics_writer_slot(
      metrics, id, VKR_METRIC_KIND_GAUGE, VKR_METRIC_SCALAR_U64);
  if (metrics->catalog[index].writer == VKR_METRIC_WRITER_CONCURRENT) {
    vkr_atomic_uint64_store(&metrics->concurrent[index].value, value,
                            VKR_MEMORY_ORDER_RELAXED);
    return;
  }
  metrics->active[index].value.u64 = value;
  metrics->active[index].availability = VKR_METRIC_AVAILABILITY_VALID;
  metrics->active[index].reason = VKR_METRIC_REASON_NONE;
}

static INLINE void vkr_metrics_gauge_set_f64(VkrMetrics *metrics,
                                             VkrMetricId id, float64_t value) {
  const uint32_t index = vkr_metrics_writer_slot(
      metrics, id, VKR_METRIC_KIND_GAUGE, VKR_METRIC_SCALAR_F64);
  assert(metrics->catalog[index].writer == VKR_METRIC_WRITER_RENDER_THREAD);
  metrics->active[index].value.f64 = value;
  metrics->active[index].availability = VKR_METRIC_AVAILABILITY_VALID;
  metrics->active[index].reason = VKR_METRIC_REASON_NONE;
}

static INLINE void vkr_metrics_duration_add_ns(VkrMetrics *metrics,
                                               VkrMetricId id,
                                               uint64_t duration_ns) {
  const uint32_t index = vkr_metrics_writer_slot(
      metrics, id, VKR_METRIC_KIND_DURATION, VKR_METRIC_SCALAR_U64);
  if (metrics->catalog[index].writer == VKR_METRIC_WRITER_CONCURRENT) {
    vkr_atomic_uint64_fetch_add(&metrics->concurrent[index].value, duration_ns,
                                VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_fetch_add(&metrics->concurrent[index].aux, 1u,
                                VKR_MEMORY_ORDER_RELAXED);
    return;
  }

  VkrMetricDurationSample *sample = &metrics->active[index].value.duration;
  sample->sum_ns += duration_ns;
  sample->count++;
  if (sample->count == 1u || duration_ns < sample->min_ns) {
    sample->min_ns = duration_ns;
  }
  if (duration_ns > sample->max_ns) {
    sample->max_ns = duration_ns;
  }
  metrics->active[index].availability = VKR_METRIC_AVAILABILITY_VALID;
  metrics->active[index].reason = VKR_METRIC_REASON_NONE;
}

static INLINE void vkr_metrics_mark(VkrMetrics *metrics, VkrMetricId id,
                                    VkrMetricAvailability availability,
                                    VkrMetricReason reason) {
  const uint32_t index = vkr_metric_id_index(id);
#ifndef NDEBUG
  assert(metrics != NULL && metrics->sealed && metrics->frame_active);
  assert(metrics->render_thread_id == vkr_thread_current_id());
  assert(index < metrics->slot_count);
  assert(vkr_metric_id_generation(id) == metrics->registry_generation);
  assert(metrics->catalog[index].writer == VKR_METRIC_WRITER_RENDER_THREAD);
#endif
  metrics->active[index].availability = availability;
  metrics->active[index].reason = reason;
}

/**
 * @brief Publishes one bounded event and folds its duration into `source`.
 *
 * Only successful operations contribute to the duration aggregate; a failed
 * operation still publishes its event so the failure is visible, but its time
 * never widens a percentile that is supposed to describe successful work.
 */
bool8_t vkr_metrics_event_record(VkrMetricEventProducer producer,
                                 String8 subject, uint64_t start_ns,
                                 uint64_t duration_ns, uint64_t bytes,
                                 VkrMetricEventStatus status);
#else
#define vkr_metrics_begin_frame(metrics, cpu_frame_index, submit_serial)       \
  ((void)0)
#define vkr_metrics_end_frame(metrics) (true_v)
#define vkr_metrics_counter_add(metrics, id, value) ((void)0)
#define vkr_metrics_gauge_set_u64(metrics, id, value) ((void)0)
#define vkr_metrics_gauge_set_f64(metrics, id, value) ((void)0)
#define vkr_metrics_duration_add_ns(metrics, id, duration_ns) ((void)0)
#define vkr_metrics_mark(metrics, id, availability, reason) ((void)0)
#define vkr_metrics_event_record(producer, subject, start_ns, duration_ns,     \
                                 bytes, status)                                \
  (true_v)
#endif

/**
 * @brief Wall-clock scope timer for CPU interval metrics.
 *
 * Use the macros below rather than hand-rolling `#if VKR_METRICS_ENABLED`
 * blocks around a start timestamp: when instrumentation is compiled out the
 * timestamp declaration disappears with its use, so no variable is left
 * referenced outside the guard that declared it.
 *
 *   VKR_METRICS_SCOPE_NS(metrics, id) {
 *     err = vkr_renderer_prepare_frame(renderer, &setup);
 *   }
 */
typedef struct VkrMetricsScopeTimer {
  float64_t start_seconds;
  bool8_t active;
} VkrMetricsScopeTimer;

static INLINE uint64_t vkr_metrics_elapsed_ns(float64_t start_seconds) {
  const float64_t elapsed = vkr_platform_get_absolute_time() - start_seconds;
  return elapsed > 0.0 ? (uint64_t)(elapsed * 1000000000.0) : 0u;
}

#if VKR_METRICS_ENABLED
/* The timer lives in the `for` scope, so nesting shadows rather than collides.
 */
#define VKR_METRICS_SCOPE_NS(metrics, id)                                      \
  for (VkrMetricsScopeTimer                                                    \
           vkr_metrics_scope_ = {vkr_platform_get_absolute_time(), true_v};    \
       vkr_metrics_scope_.active; vkr_metrics_scope_.active = false_v,         \
           vkr_metrics_duration_add_ns((metrics), (id),                        \
                                       vkr_metrics_elapsed_ns(                 \
                                           vkr_metrics_scope_.start_seconds)))

#define VKR_METRICS_ADD_ELAPSED_NS(metrics, id, start_seconds)                 \
  vkr_metrics_duration_add_ns((metrics), (id),                                 \
                              vkr_metrics_elapsed_ns((start_seconds)))
#else
#define VKR_METRICS_SCOPE_NS(metrics, id)
#define VKR_METRICS_ADD_ELAPSED_NS(metrics, id, start_seconds) ((void)0)
#endif

bool8_t vkr_metrics_snapshot_acquire(VkrMetrics *metrics,
                                     VkrMetricsSnapshotView *out_view);
void vkr_metrics_snapshot_release(VkrMetrics *metrics,
                                  VkrMetricsSnapshotView *view);

bool8_t vkr_metrics_event_push(VkrMetrics *metrics, const VkrMetricEvent *event,
                               String8 subject);
bool8_t vkr_metrics_event_pop(VkrMetrics *metrics, VkrMetricEvent *out_event);
bool8_t vkr_metrics_event_peek(const VkrMetrics *metrics, uint32_t offset,
                               VkrMetricEvent *out_event);
bool8_t vkr_metrics_event_consume(VkrMetrics *metrics, uint32_t count);

const VkrMetricCatalogEntry *vkr_metrics_get_catalog(const VkrMetrics *metrics,
                                                     uint32_t *out_count);
const VkrMetricSample *vkr_metrics_frame_get(const VkrMetricsFrame *frame,
                                             VkrMetricId id);

/**
 * @brief Typed sample readers.
 *
 * Each verifies registry generation, slot range, availability, and that the
 * requested union member is the one the producer wrote. A false return means
 * "no sample", never "zero"; callers must not substitute a default.
 */
bool8_t vkr_metrics_frame_read_u64(const VkrMetricsFrame *frame, VkrMetricId id,
                                   uint64_t *out_value);
bool8_t vkr_metrics_frame_read_f64(const VkrMetricsFrame *frame, VkrMetricId id,
                                   float64_t *out_value);
bool8_t vkr_metrics_frame_read_duration(const VkrMetricsFrame *frame,
                                        VkrMetricId id,
                                        VkrMetricDurationSample *out_sample);
/** Mean nanoseconds over the frame's samples; false when count is zero. */
bool8_t vkr_metrics_frame_read_duration_mean_ns(const VkrMetricsFrame *frame,
                                                VkrMetricId id,
                                                uint64_t *out_mean_ns);

/**
 * @brief Counts required slots the frame did not carry.
 *
 * This is what makes a report `incomplete` rather than quietly short a few
 * numbers: a nonzero result means evidence a consumer was promised is absent.
 */
uint32_t vkr_metrics_frame_missing_required(const VkrMetrics *metrics,
                                            const VkrMetricsFrame *frame);
