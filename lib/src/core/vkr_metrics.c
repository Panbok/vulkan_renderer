#include "core/vkr_metrics.h"

#include "core/logger.h"

#define VKR_METRICS_SNAPSHOT_WRITER_OWNED UINT32_MAX

static VkrAtomicUint32 s_vkr_metrics_next_generation = 1u;

vkr_internal bool8_t vkr_metrics_name_is_valid(const String8 *name) {
  if (!name || !name->str || name->length == 0 ||
      name->length > VKR_METRIC_NAME_MAX || name->str[0] == '.' ||
      name->str[name->length - 1u] == '.') {
    return false_v;
  }

  bool8_t previous_dot = false_v;
  for (uint64_t i = 0; i < name->length; ++i) {
    const uint8_t c = name->str[i];
    const bool8_t valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '.';
    if (!valid || (c == '.' && previous_dot)) {
      return false_v;
    }
    previous_dot = c == '.';
  }
  return true_v;
}

vkr_internal bool8_t
vkr_metrics_description_is_valid(const VkrMetricDescription *description) {
  if (!description || !vkr_metrics_name_is_valid(&description->name) ||
      description->domain >= VKR_METRIC_DOMAIN_COUNT ||
      description->kind > VKR_METRIC_KIND_DURATION ||
      description->unit >= VKR_METRIC_UNIT_COUNT_MAX ||
      description->scalar > VKR_METRIC_SCALAR_F64 ||
      description->writer > VKR_METRIC_WRITER_CONCURRENT) {
    return false_v;
  }
  if ((description->kind == VKR_METRIC_KIND_COUNTER ||
       description->kind == VKR_METRIC_KIND_DURATION) &&
      description->scalar != VKR_METRIC_SCALAR_U64) {
    return false_v;
  }
  if (description->writer == VKR_METRIC_WRITER_CONCURRENT &&
      description->scalar != VKR_METRIC_SCALAR_U64) {
    return false_v;
  }
  return true_v;
}

void vkr_metrics_init(VkrMetrics *metrics) {
  assert(metrics != NULL);
  MemZero(metrics, sizeof(*metrics));
  metrics->registry_generation = (uint16_t)vkr_atomic_uint32_fetch_add(
      &s_vkr_metrics_next_generation, 1u, VKR_MEMORY_ORDER_RELAXED);
  metrics->render_thread_id = vkr_thread_current_id();
  for (uint64_t i = 0; i < VKR_METRIC_EVENT_CAPACITY; ++i) {
    vkr_atomic_uint64_store(&metrics->events[i].sequence, i,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

bool8_t vkr_metrics_register(VkrMetrics *metrics,
                             const VkrMetricDescription *description,
                             VkrMetricId *out_id) {
  if (out_id) {
    *out_id = VKR_METRIC_ID_INVALID;
  }
  if (!metrics || !out_id || metrics->sealed ||
      metrics->slot_count >= VKR_METRICS_MAX_SLOTS ||
      !vkr_metrics_description_is_valid(description)) {
    return false_v;
  }

  for (uint32_t i = 0; i < metrics->slot_count; ++i) {
    const VkrMetricCatalogEntry *entry = &metrics->catalog[i];
    if (entry->name_length == description->name.length &&
        MemCompare(entry->name, description->name.str,
                   description->name.length) == 0) {
      return false_v;
    }
  }

  const uint32_t index = metrics->slot_count++;
  const VkrMetricId id =
      ((VkrMetricId)metrics->registry_generation << 16u) | index;
  VkrMetricCatalogEntry *entry = &metrics->catalog[index];
  MemCopy(entry->name, description->name.str, description->name.length);
  entry->name[description->name.length] = '\0';
  entry->name_length = (uint8_t)description->name.length;
  entry->domain = description->domain;
  entry->kind = description->kind;
  entry->unit = description->unit;
  entry->scalar = description->scalar;
  entry->writer = description->writer;
  entry->required_when_enabled = description->required_when_enabled;
  *out_id = id;
  return true_v;
}

bool8_t vkr_metrics_seal(VkrMetrics *metrics) {
  if (!metrics || metrics->sealed || metrics->slot_count == 0) {
    return false_v;
  }
  metrics->sealed = true_v;
  log_info("Metrics catalog sealed: %u/%u slots used", metrics->slot_count,
           VKR_METRICS_MAX_SLOTS);
  return true_v;
}

#if VKR_METRICS_ENABLED
void vkr_metrics_begin_frame(VkrMetrics *metrics, uint64_t cpu_frame_index,
                             uint64_t submit_serial) {
  assert(metrics != NULL && metrics->sealed);
  assert(metrics->render_thread_id == vkr_thread_current_id());
  assert(!metrics->frame_active);
  MemZero(metrics->active, sizeof(VkrMetricSample) * metrics->slot_count);
  for (uint32_t i = 0; i < metrics->slot_count; ++i) {
    // Kind/scalar are mirrored from the catalog every frame so a consumer that
    // holds only a published frame can prove which union member is live.
    metrics->active[i].kind = metrics->catalog[i].kind;
    metrics->active[i].scalar = metrics->catalog[i].scalar;
    // A counter that is simply not incremented this frame is a valid zero, not
    // an absent sample; anything else conflates "no draws" with "not measured".
    if (metrics->catalog[i].kind == VKR_METRIC_KIND_COUNTER) {
      metrics->active[i].availability = VKR_METRIC_AVAILABILITY_VALID;
      metrics->active[i].reason = VKR_METRIC_REASON_NONE;
      continue;
    }
    metrics->active[i].availability = VKR_METRIC_AVAILABILITY_UNAVAILABLE;
    metrics->active[i].reason = VKR_METRIC_REASON_NOT_SAMPLED;
  }
  metrics->active_cpu_frame_index = cpu_frame_index;
  metrics->active_submit_serial = submit_serial;
  metrics->frame_active = true_v;
}

vkr_internal void vkr_metrics_collect_concurrent(VkrMetrics *metrics,
                                                 VkrMetricsFrame *frame) {
  for (uint32_t i = 0; i < metrics->slot_count; ++i) {
    const VkrMetricCatalogEntry *entry = &metrics->catalog[i];
    if (entry->writer != VKR_METRIC_WRITER_CONCURRENT) {
      continue;
    }

    VkrMetricConcurrentSlot *slot = &metrics->concurrent[i];
    const uint64_t current =
        vkr_atomic_uint64_load(&slot->value, VKR_MEMORY_ORDER_ACQUIRE);
    VkrMetricSample *sample = &frame->samples[i];
    sample->availability = VKR_METRIC_AVAILABILITY_VALID;
    sample->reason = VKR_METRIC_REASON_NONE;
    if (entry->kind == VKR_METRIC_KIND_GAUGE) {
      sample->value.u64 = current;
      slot->previous_value = current;
      continue;
    }

    const uint64_t delta = current - slot->previous_value;
    slot->previous_value = current;
    if (entry->kind == VKR_METRIC_KIND_COUNTER) {
      sample->value.u64 = delta;
      continue;
    }

    const uint64_t current_count =
        vkr_atomic_uint64_load(&slot->aux, VKR_MEMORY_ORDER_ACQUIRE);
    sample->value.duration.sum_ns = delta;
    sample->value.duration.count = current_count - slot->previous_aux;
    sample->value.duration.min_ns = 0;
    sample->value.duration.max_ns = 0;
    slot->previous_aux = current_count;
  }
}

// A dropped publication is still a completed interval. Advance cumulative
// baselines so the next published frame cannot silently absorb work from the
// dropped frame and attribute it to the wrong CPU frame/submit serial.
vkr_internal void vkr_metrics_discard_concurrent_interval(VkrMetrics *metrics) {
  for (uint32_t i = 0; i < metrics->slot_count; ++i) {
    const VkrMetricCatalogEntry *entry = &metrics->catalog[i];
    if (entry->writer != VKR_METRIC_WRITER_CONCURRENT) {
      continue;
    }

    VkrMetricConcurrentSlot *slot = &metrics->concurrent[i];
    slot->previous_value =
        vkr_atomic_uint64_load(&slot->value, VKR_MEMORY_ORDER_ACQUIRE);
    if (entry->kind == VKR_METRIC_KIND_DURATION) {
      slot->previous_aux =
          vkr_atomic_uint64_load(&slot->aux, VKR_MEMORY_ORDER_ACQUIRE);
    }
  }
}

vkr_internal bool8_t vkr_metrics_claim_publish_buffer(VkrMetrics *metrics,
                                                      uint32_t *out_index) {
  const uint32_t current = vkr_atomic_uint32_load(&metrics->published_index,
                                                  VKR_MEMORY_ORDER_ACQUIRE);
  for (uint32_t offset = 1; offset < VKR_METRICS_SNAPSHOT_BUFFER_COUNT;
       ++offset) {
    const uint32_t index =
        (current + offset) % VKR_METRICS_SNAPSHOT_BUFFER_COUNT;
    uint32_t expected = 0;
    if (vkr_atomic_uint32_compare_exchange(
            &metrics->snapshot_owners[index], &expected,
            VKR_METRICS_SNAPSHOT_WRITER_OWNED, VKR_MEMORY_ORDER_ACQ_REL,
            VKR_MEMORY_ORDER_ACQUIRE)) {
      *out_index = index;
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_metrics_end_frame(VkrMetrics *metrics) {
  assert(metrics != NULL && metrics->sealed && metrics->frame_active);
  assert(metrics->render_thread_id == vkr_thread_current_id());

  uint32_t target_index = 0;
  if (!vkr_metrics_claim_publish_buffer(metrics, &target_index)) {
    vkr_metrics_discard_concurrent_interval(metrics);
    metrics->snapshot_publications_dropped++;
    metrics->frame_active = false_v;
    return false_v;
  }

  VkrMetricsFrame *frame = &metrics->published[target_index];
  MemZero(frame, offsetof(VkrMetricsFrame, samples));
  MemZero(frame->samples, sizeof(VkrMetricSample) * metrics->slot_count);
  frame->registry_generation = metrics->registry_generation;
  frame->cpu_frame_index = metrics->active_cpu_frame_index;
  frame->submit_serial = metrics->active_submit_serial;
  frame->publication_serial = ++metrics->publication_serial;
  frame->snapshot_publications_dropped = metrics->snapshot_publications_dropped;
  frame->events_dropped = vkr_atomic_uint64_load(&metrics->events_dropped,
                                                 VKR_MEMORY_ORDER_ACQUIRE);
  frame->event_subjects_truncated = vkr_atomic_uint64_load(
      &metrics->event_subjects_truncated, VKR_MEMORY_ORDER_ACQUIRE);
  frame->slot_count = metrics->slot_count;
  MemCopy(frame->samples, metrics->active,
          sizeof(VkrMetricSample) * metrics->slot_count);
  vkr_metrics_collect_concurrent(metrics, frame);

  vkr_atomic_uint32_store(&metrics->published_index, target_index,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_atomic_bool_store(&metrics->snapshot_available, true_v,
                        VKR_MEMORY_ORDER_RELEASE);
  vkr_atomic_uint32_store(&metrics->snapshot_owners[target_index], 0,
                          VKR_MEMORY_ORDER_RELEASE);
  metrics->frame_active = false_v;
  return true_v;
}
#endif

bool8_t vkr_metrics_snapshot_acquire(VkrMetrics *metrics,
                                     VkrMetricsSnapshotView *out_view) {
  if (!metrics || !out_view ||
      !vkr_atomic_bool_load(&metrics->snapshot_available,
                            VKR_MEMORY_ORDER_ACQUIRE)) {
    return false_v;
  }

  for (;;) {
    const uint32_t index = vkr_atomic_uint32_load(&metrics->published_index,
                                                  VKR_MEMORY_ORDER_ACQUIRE);
    uint32_t owners = vkr_atomic_uint32_load(&metrics->snapshot_owners[index],
                                             VKR_MEMORY_ORDER_ACQUIRE);
    // The writer publishes its index before clearing its own ownership, so a
    // reader can briefly observe the current buffer still writer-owned. That
    // window is three atomic stores wide; spinning through it never blocks on
    // the writer's payload copy.
    if (owners == VKR_METRICS_SNAPSHOT_WRITER_OWNED ||
        owners == VKR_METRICS_SNAPSHOT_WRITER_OWNED - 1u) {
      continue;
    }
    if (!vkr_atomic_uint32_compare_exchange(&metrics->snapshot_owners[index],
                                            &owners, owners + 1u,
                                            VKR_MEMORY_ORDER_ACQ_REL,
                                            VKR_MEMORY_ORDER_ACQUIRE)) {
      continue;
    }
    if (index != vkr_atomic_uint32_load(&metrics->published_index,
                                        VKR_MEMORY_ORDER_ACQUIRE)) {
      vkr_atomic_uint32_fetch_sub(&metrics->snapshot_owners[index], 1u,
                                  VKR_MEMORY_ORDER_RELEASE);
      continue;
    }

    out_view->frame = &metrics->published[index];
    out_view->buffer_index = index;
    out_view->publication_serial = out_view->frame->publication_serial;
    return true_v;
  }
}

void vkr_metrics_snapshot_release(VkrMetrics *metrics,
                                  VkrMetricsSnapshotView *view) {
  if (!metrics || !view || !view->frame ||
      view->buffer_index >= VKR_METRICS_SNAPSHOT_BUFFER_COUNT) {
    return;
  }
  const uint32_t previous = vkr_atomic_uint32_fetch_sub(
      &metrics->snapshot_owners[view->buffer_index], 1u,
      VKR_MEMORY_ORDER_RELEASE);
  assert(previous > 0 && previous != VKR_METRICS_SNAPSHOT_WRITER_OWNED);
  (void)previous;
  *view = (VkrMetricsSnapshotView){0};
}

bool8_t vkr_metrics_event_push(VkrMetrics *metrics, const VkrMetricEvent *event,
                               String8 subject) {
#if !VKR_METRICS_ENABLED
  (void)metrics;
  (void)event;
  (void)subject;
  return true_v;
#else
  if (!metrics || !event ||
      vkr_metric_id_generation(event->source) != metrics->registry_generation ||
      vkr_metric_id_index(event->source) >= metrics->slot_count ||
      (!subject.str && subject.length > 0)) {
    return false_v;
  }

  uint64_t pos = vkr_atomic_uint64_load(&metrics->event_enqueue_pos,
                                        VKR_MEMORY_ORDER_RELAXED);
  VkrMetricEventSlot *slot = NULL;
  for (;;) {
    slot = &metrics->events[pos % VKR_METRIC_EVENT_CAPACITY];
    const uint64_t sequence =
        vkr_atomic_uint64_load(&slot->sequence, VKR_MEMORY_ORDER_ACQUIRE);
    const int64_t difference = (int64_t)sequence - (int64_t)pos;
    if (difference == 0) {
      uint64_t expected = pos;
      if (vkr_atomic_uint64_compare_exchange(
              &metrics->event_enqueue_pos, &expected, pos + 1u,
              VKR_MEMORY_ORDER_RELAXED, VKR_MEMORY_ORDER_RELAXED)) {
        break;
      }
      pos = expected;
    } else if (difference < 0) {
      vkr_atomic_uint64_fetch_add(&metrics->events_dropped, 1u,
                                  VKR_MEMORY_ORDER_RELAXED);
      return false_v;
    } else {
      pos = vkr_atomic_uint64_load(&metrics->event_enqueue_pos,
                                   VKR_MEMORY_ORDER_RELAXED);
    }
  }

  slot->event = *event;
  uint64_t copy_length =
      metrics->config.event_subjects
          ? Min(subject.length, (uint64_t)VKR_METRIC_EVENT_SUBJECT_MAX)
          : 0u;
  // Back the cut off any UTF-8 continuation byte. Subjects are asset paths;
  // a sequence severed mid-codepoint would make the emitted report invalid
  // UTF-8 and every strict JSON reader would reject the whole document.
  while (copy_length > 0 && copy_length < subject.length &&
         (subject.str[copy_length] & 0xC0u) == 0x80u) {
    copy_length--;
  }
  if (subject.str && copy_length > 0) {
    MemCopy(slot->event.subject, subject.str, copy_length);
  }
  slot->event.subject[copy_length] = '\0';
  slot->event.subject_length = (uint8_t)copy_length;
  slot->event.subject_truncated =
      metrics->config.event_subjects && subject.length > copy_length;
  if (slot->event.subject_truncated) {
    // Cumulative, like events_dropped: the report must state how many subjects
    // were cut over the whole run, not just over the events still buffered.
    vkr_atomic_uint64_fetch_add(&metrics->event_subjects_truncated, 1u,
                                VKR_MEMORY_ORDER_RELAXED);
  }
  slot->event.thread_id = (uint32_t)vkr_thread_current_id();
  vkr_atomic_uint64_store(&slot->sequence, pos + 1u, VKR_MEMORY_ORDER_RELEASE);
  return true_v;
#endif
}

#if VKR_METRICS_ENABLED
bool8_t vkr_metrics_event_record(VkrMetricEventProducer producer,
                                 String8 subject, uint64_t start_ns,
                                 uint64_t duration_ns, uint64_t bytes,
                                 VkrMetricEventStatus status) {
  if (!producer.metrics || producer.source == VKR_METRIC_ID_INVALID) {
    return false_v;
  }
  // A failed operation still publishes its event, but its time never enters
  // the aggregate: `pipeline.create` must describe the cost of creating a
  // pipeline, not the cost of failing to.
  if (status == VKR_METRIC_EVENT_STATUS_SUCCESS) {
    vkr_metrics_duration_add_ns(producer.metrics, producer.source, duration_ns);
  }
  const VkrMetricEvent event = {
      .source = producer.source,
      .start_ns = start_ns,
      .duration_ns = duration_ns,
      .bytes = bytes,
      .status = status,
  };
  return vkr_metrics_event_push(producer.metrics, &event, subject);
}
#endif

bool8_t vkr_metrics_event_pop(VkrMetrics *metrics, VkrMetricEvent *out_event) {
  if (!vkr_metrics_event_peek(metrics, 0, out_event)) {
    return false_v;
  }
  return vkr_metrics_event_consume(metrics, 1u);
}

bool8_t vkr_metrics_event_peek(const VkrMetrics *metrics, uint32_t offset,
                               VkrMetricEvent *out_event) {
  if (!metrics || !out_event || offset >= VKR_METRIC_EVENT_CAPACITY) {
    return false_v;
  }

  const uint64_t pos = vkr_atomic_uint64_load(&metrics->event_dequeue_pos,
                                              VKR_MEMORY_ORDER_RELAXED) +
                       offset;
  const VkrMetricEventSlot *slot =
      &metrics->events[pos % VKR_METRIC_EVENT_CAPACITY];
  const uint64_t sequence =
      vkr_atomic_uint64_load(&slot->sequence, VKR_MEMORY_ORDER_ACQUIRE);
  if ((int64_t)sequence - (int64_t)(pos + 1u) != 0) {
    return false_v;
  }

  *out_event = slot->event;
  return true_v;
}

bool8_t vkr_metrics_event_consume(VkrMetrics *metrics, uint32_t count) {
  if (!metrics || count > VKR_METRIC_EVENT_CAPACITY) {
    return false_v;
  }
  uint64_t pos = vkr_atomic_uint64_load(&metrics->event_dequeue_pos,
                                        VKR_MEMORY_ORDER_RELAXED);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMetricEventSlot *slot =
        &metrics->events[(pos + i) % VKR_METRIC_EVENT_CAPACITY];
    const uint64_t sequence =
        vkr_atomic_uint64_load(&slot->sequence, VKR_MEMORY_ORDER_ACQUIRE);
    if ((int64_t)sequence - (int64_t)(pos + i + 1u) != 0) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    VkrMetricEventSlot *slot =
        &metrics->events[(pos + i) % VKR_METRIC_EVENT_CAPACITY];
    vkr_atomic_uint64_store(&slot->sequence,
                            pos + i + VKR_METRIC_EVENT_CAPACITY,
                            VKR_MEMORY_ORDER_RELEASE);
  }
  vkr_atomic_uint64_store(&metrics->event_dequeue_pos, pos + count,
                          VKR_MEMORY_ORDER_RELAXED);
  return true_v;
}

const VkrMetricCatalogEntry *vkr_metrics_get_catalog(const VkrMetrics *metrics,
                                                     uint32_t *out_count) {
  if (out_count) {
    *out_count = metrics ? metrics->slot_count : 0;
  }
  return metrics ? metrics->catalog : NULL;
}

const VkrMetricSample *vkr_metrics_frame_get(const VkrMetricsFrame *frame,
                                             VkrMetricId id) {
  const uint32_t index = vkr_metric_id_index(id);
  if (!frame || vkr_metric_id_generation(id) != frame->registry_generation ||
      index >= frame->slot_count) {
    return NULL;
  }
  return &frame->samples[index];
}

/** Shared gate: a readable sample of a known kind, or NULL. */
vkr_internal const VkrMetricSample *
vkr_metrics_frame_readable(const VkrMetricsFrame *frame, VkrMetricId id,
                           VkrMetricKind kind) {
  const VkrMetricSample *sample = vkr_metrics_frame_get(frame, id);
  if (!sample || sample->kind != kind ||
      sample->availability == VKR_METRIC_AVAILABILITY_UNAVAILABLE) {
    return NULL;
  }
  return sample;
}

bool8_t vkr_metrics_frame_read_u64(const VkrMetricsFrame *frame, VkrMetricId id,
                                   uint64_t *out_value) {
  const VkrMetricSample *sample = vkr_metrics_frame_get(frame, id);
  if (!out_value || !sample ||
      sample->availability == VKR_METRIC_AVAILABILITY_UNAVAILABLE ||
      sample->scalar != VKR_METRIC_SCALAR_U64 ||
      sample->kind == VKR_METRIC_KIND_DURATION) {
    return false_v;
  }
  *out_value = sample->value.u64;
  return true_v;
}

bool8_t vkr_metrics_frame_read_f64(const VkrMetricsFrame *frame, VkrMetricId id,
                                   float64_t *out_value) {
  const VkrMetricSample *sample =
      vkr_metrics_frame_readable(frame, id, VKR_METRIC_KIND_GAUGE);
  if (!out_value || !sample || sample->scalar != VKR_METRIC_SCALAR_F64) {
    return false_v;
  }
  *out_value = sample->value.f64;
  return true_v;
}

bool8_t vkr_metrics_frame_read_duration(const VkrMetricsFrame *frame,
                                        VkrMetricId id,
                                        VkrMetricDurationSample *out_sample) {
  const VkrMetricSample *sample =
      vkr_metrics_frame_readable(frame, id, VKR_METRIC_KIND_DURATION);
  if (!out_sample || !sample) {
    return false_v;
  }
  *out_sample = sample->value.duration;
  return true_v;
}

bool8_t vkr_metrics_frame_read_duration_mean_ns(const VkrMetricsFrame *frame,
                                                VkrMetricId id,
                                                uint64_t *out_mean_ns) {
  VkrMetricDurationSample duration = {0};
  if (!out_mean_ns || !vkr_metrics_frame_read_duration(frame, id, &duration) ||
      duration.count == 0) {
    return false_v;
  }
  *out_mean_ns = duration.sum_ns / duration.count;
  return true_v;
}

uint32_t vkr_metrics_frame_missing_required(const VkrMetrics *metrics,
                                            const VkrMetricsFrame *frame) {
  if (!metrics || !frame ||
      frame->registry_generation != metrics->registry_generation) {
    return 0;
  }
  const uint32_t count = Min(frame->slot_count, metrics->slot_count);
  uint32_t missing = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (metrics->catalog[i].required_when_enabled &&
        frame->samples[i].availability ==
            VKR_METRIC_AVAILABILITY_UNAVAILABLE) {
      missing++;
    }
  }
  return missing;
}
