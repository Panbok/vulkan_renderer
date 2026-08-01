#include "vkr_harness_runtime.h"

typedef struct VkrHarnessSampleLayout {
  uint64_t value_count;
  uint64_t pass_value_count;
  uint64_t metrics;
  uint64_t values;
  uint64_t availability;
  uint64_t passes;
  uint64_t pass_cpu_ms;
  uint64_t pass_gpu_ms;
  uint64_t pass_flags;
  uint64_t events;
  uint64_t total;
} VkrHarnessSampleLayout;

/** Byte-array sections have no alignment of their own; the sections that
    follow them are read back as float64/struct arrays and must stay aligned. */
static uint64_t vkr_harness_align8(uint64_t cursor) {
  return (cursor + 7u) & ~(uint64_t)7u;
}

/** Single description of the on-disk section order used by write and read. */
static VkrHarnessSampleLayout
vkr_harness_sample_layout(const VkrHarnessSampleFileHeader *header) {
  const uint64_t frames =
      (uint64_t)header->warmup_frames + header->measure_frames;
  VkrHarnessSampleLayout layout = {
      .value_count = frames * header->metric_count,
      .pass_value_count = frames * header->pass_count,
  };
  uint64_t cursor = vkr_harness_align8(sizeof(VkrHarnessSampleFileHeader));
  layout.metrics = cursor;
  cursor = vkr_harness_align8(cursor + sizeof(VkrHarnessSampleMetric) *
                                           header->metric_count);
  layout.values = cursor;
  cursor += sizeof(float64_t) * layout.value_count;
  layout.availability = cursor;
  cursor = vkr_harness_align8(cursor + layout.value_count);
  layout.passes = cursor;
  cursor = vkr_harness_align8(cursor + sizeof(VkrHarnessSamplePass) *
                                           header->pass_count);
  layout.pass_cpu_ms = cursor;
  cursor += sizeof(float64_t) * layout.pass_value_count;
  layout.pass_gpu_ms = cursor;
  cursor += sizeof(float64_t) * layout.pass_value_count;
  layout.pass_flags = cursor;
  cursor = vkr_harness_align8(cursor + layout.pass_value_count);
  layout.events = cursor;
  cursor += sizeof(VkrHarnessSampleEvent) * header->event_count;
  layout.total = cursor;
  return layout;
}

bool8_t vkr_harness_samples_write(const char *path,
                                  const VkrHarnessSampleFileHeader *header,
                                  const VkrHarnessSampleSet *samples,
                                  Arena *transient,
                                  VkrHarnessError *out_error) {
  const VkrHarnessSampleLayout layout = vkr_harness_sample_layout(header);
  Scratch scratch = scratch_create(transient);
  uint8_t *data = arena_alloc(transient, layout.total, ARENA_MEMORY_TAG_BUFFER);
  if (!data) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_BUFFER);
    vkr_harness_error_set(out_error, "samples.allocate", "$",
                          "Unable to allocate the raw sample buffer");
    return false_v;
  }
  /* Zeroed so inter-section alignment padding is defined rather than whatever
     the arena block last held. */
  MemZero(data, layout.total);
  MemCopy(data, header, sizeof(*header));
  MemCopy(data + layout.metrics, samples->metrics,
          sizeof(*samples->metrics) * header->metric_count);
  MemCopy(data + layout.values, samples->values,
          sizeof(*samples->values) * layout.value_count);
  MemCopy(data + layout.availability, samples->availability,
          layout.value_count);
  if (header->pass_count > 0u) {
    MemCopy(data + layout.passes, samples->passes,
            sizeof(*samples->passes) * header->pass_count);
    MemCopy(data + layout.pass_cpu_ms, samples->pass_cpu_ms,
            sizeof(*samples->pass_cpu_ms) * layout.pass_value_count);
    MemCopy(data + layout.pass_gpu_ms, samples->pass_gpu_ms,
            sizeof(*samples->pass_gpu_ms) * layout.pass_value_count);
    MemCopy(data + layout.pass_flags, samples->pass_flags,
            layout.pass_value_count);
  }
  if (header->event_count > 0u) {
    MemCopy(data + layout.events, samples->events,
            sizeof(*samples->events) * header->event_count);
  }
  const bool8_t written =
      vkr_harness_atomic_write(path, data, layout.total, out_error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_BUFFER);
  return written;
}

bool8_t vkr_harness_samples_read(const char *path,
                                 const VkrHarnessCase *case_manifest,
                                 Arena *persistent,
                                 VkrHarnessSampleSet *out_samples) {
  uint64_t length = 0;
  uint8_t *data = NULL;
  if (!vkr_harness_read_file(path, persistent, &data, &length) ||
      length < sizeof(VkrHarnessSampleFileHeader)) {
    return false_v;
  }
  VkrHarnessSampleFileHeader header;
  MemCopy(&header, data, sizeof(header));
  if (MemCompare(header.magic, VKR_HARNESS_SAMPLE_MAGIC,
                 sizeof(header.magic)) != 0 ||
      header.schema_version != VKR_HARNESS_SCHEMA_VERSION ||
      header.metric_count == 0u ||
      header.metric_count > VKR_METRICS_MAX_SLOTS ||
      header.pass_count > VKR_METRICS_MAX_SLOTS ||
      header.event_count > VKR_HARNESS_MAX_EVENTS ||
      header.warmup_frames != case_manifest->warmup_frames ||
      header.measure_frames != case_manifest->measure_frames) {
    return false_v;
  }
  const VkrHarnessSampleLayout layout = vkr_harness_sample_layout(&header);
  if (layout.total != length) {
    return false_v;
  }
  *out_samples = (VkrHarnessSampleSet){
      .header = header,
      .metrics = (const VkrHarnessSampleMetric *)(data + layout.metrics),
      .values = (const float64_t *)(data + layout.values),
      .availability = data + layout.availability,
      .passes = (const VkrHarnessSamplePass *)(data + layout.passes),
      .pass_cpu_ms = (const float64_t *)(data + layout.pass_cpu_ms),
      .pass_gpu_ms = (const float64_t *)(data + layout.pass_gpu_ms),
      .pass_flags = data + layout.pass_flags,
      .events = (const VkrHarnessSampleEvent *)(data + layout.events),
  };
  return true_v;
}

bool8_t vkr_harness_compute_metric_results(
    const VkrHarnessArenas *arenas, uint32_t warmup_frames,
    uint32_t measure_frames, uint32_t metric_count,
    const VkrHarnessSampleMetric *catalog, const float64_t *values,
    const uint8_t *availability, VkrHarnessMetricResult **out_metrics,
    VkrHarnessError *out_error) {
  VkrHarnessMetricResult *results =
      arena_alloc(arenas->persistent, (uint64_t)metric_count * sizeof(*results),
                  ARENA_MEMORY_TAG_STRUCT);
  Scratch scratch = scratch_create(arenas->transient);
  const uint64_t scratch_bytes = (uint64_t)measure_frames * sizeof(float64_t);
  float64_t *valid =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  float64_t *sorted =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  if (!results || !valid || !sorted) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    vkr_harness_error_set(out_error, "statistics.allocate", "$",
                          "Unable to allocate metric statistics");
    return false_v;
  }
  MemZero(results, (uint64_t)metric_count * sizeof(*results));
  for (uint32_t metric = 0; metric < metric_count; ++metric) {
    string_format(results[metric].name, sizeof(results[metric].name), "%s",
                  catalog[metric].name);
    string_format(results[metric].unit, sizeof(results[metric].unit), "%s",
                  catalog[metric].unit);
    uint64_t valid_count = 0;
    uint64_t invalid_count = 0;
    for (uint32_t frame = 0; frame < measure_frames; ++frame) {
      const uint64_t offset =
          (uint64_t)(warmup_frames + frame) * metric_count + metric;
      if (availability[offset] == VKR_METRIC_AVAILABILITY_VALID) {
        valid[valid_count++] = values[offset];
      } else {
        invalid_count++;
      }
    }
    if (!vkr_harness_statistics_compute(valid, valid_count, invalid_count,
                                        sorted, &results[metric].statistics)) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      vkr_harness_error_set(out_error, "statistics.compute", "$",
                            "Unable to compute metric '%s'",
                            catalog[metric].name);
      return false_v;
    }
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  *out_metrics = results;
  return true_v;
}

bool8_t vkr_harness_compute_pass_results(
    const VkrHarnessArenas *arenas, uint32_t warmup_frames,
    uint32_t measure_frames, uint32_t pass_count,
    const VkrHarnessSamplePass *catalog, const float64_t *cpu_ms,
    const float64_t *gpu_ms, const uint8_t *flags,
    VkrHarnessPassResult **out_passes, VkrHarnessError *out_error) {
  if (pass_count == 0u) {
    *out_passes = NULL;
    return true_v;
  }
  VkrHarnessPassResult *results =
      arena_alloc(arenas->persistent, (uint64_t)pass_count * sizeof(*results),
                  ARENA_MEMORY_TAG_STRUCT);
  Scratch scratch = scratch_create(arenas->transient);
  const uint64_t scratch_bytes = (uint64_t)measure_frames * sizeof(float64_t);
  float64_t *valid =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  float64_t *sorted =
      arena_alloc(arenas->transient, scratch_bytes, ARENA_MEMORY_TAG_ARRAY);
  if (!results || !valid || !sorted) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    vkr_harness_error_set(out_error, "statistics.allocate", "$",
                          "Unable to allocate pass statistics");
    return false_v;
  }
  MemZero(results, (uint64_t)pass_count * sizeof(*results));
  for (uint32_t pass = 0; pass < pass_count; ++pass) {
    string_format(results[pass].name, sizeof(results[pass].name), "%s",
                  catalog[pass].name);
    const float64_t *const series[2] = {cpu_ms, gpu_ms};
    const uint8_t series_flag[2] = {VKR_HARNESS_PASS_FLAG_CPU_VALID,
                                    VKR_HARNESS_PASS_FLAG_GPU_VALID};
    VkrHarnessStatistics *const target[2] = {&results[pass].cpu_ms,
                                             &results[pass].gpu_ms};
    for (uint32_t which = 0; which < 2u; ++which) {
      uint64_t valid_count = 0u;
      uint64_t invalid_count = 0u;
      for (uint32_t frame = 0; frame < measure_frames; ++frame) {
        const uint64_t offset =
            (uint64_t)(warmup_frames + frame) * pass_count + pass;
        if ((flags[offset] & series_flag[which]) != 0u) {
          valid[valid_count++] = series[which][offset];
        } else {
          invalid_count++;
        }
        if (which == 0u) {
          results[pass].culled_count +=
              (flags[offset] & VKR_HARNESS_PASS_FLAG_CULLED) != 0u;
          results[pass].disabled_count +=
              (flags[offset] & VKR_HARNESS_PASS_FLAG_DISABLED) != 0u;
        }
      }
      if (!vkr_harness_statistics_compute(valid, valid_count, invalid_count,
                                          sorted, target[which])) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        vkr_harness_error_set(out_error, "statistics.compute", "$",
                              "Unable to compute pass '%s'",
                              catalog[pass].name);
        return false_v;
      }
    }
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  *out_passes = results;
  return true_v;
}
