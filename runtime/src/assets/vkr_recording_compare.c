#include "assets/vkr_recording_compare.h"

#include "containers/vkr_sort.h"
#include "math/vkr_math.h"

static const char *const VKR_RECORDING_ALIGN_NAMES[] = {
    "time",
    "marker",
    "distance",
    "frame",
};

_Static_assert(ArrayCount(VKR_RECORDING_ALIGN_NAMES) ==
                   VKR_RECORDING_ALIGN_COUNT,
               "a name for every alignment");

static const char *const VKR_RECORDING_COMPARE_STAT_NAMES[] = {
    "p50_ms",      "p95_ms",         "p99_ms",
    "max_ms",      "low_1pct_ms",    "mean_ms",
    "over_budget", "ram_peak_bytes", "vram_peak_bytes",
};

_Static_assert(ArrayCount(VKR_RECORDING_COMPARE_STAT_NAMES) ==
                   VKR_RECORDING_COMPARE_STAT_COUNT,
               "a name for every statistic");

const char *vkr_recording_align_name(VkrRecordingAlign align) {
  return (uint32_t)align < VKR_RECORDING_ALIGN_COUNT
             ? VKR_RECORDING_ALIGN_NAMES[align]
             : "time";
}

bool8_t vkr_recording_align_parse(String8 name, VkrRecordingAlign *out) {
  for (uint32_t i = 0u; i < VKR_RECORDING_ALIGN_COUNT; ++i) {
    if (vkr_string8_equals_cstr(&name, VKR_RECORDING_ALIGN_NAMES[i])) {
      *out = (VkrRecordingAlign)i;
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_recording_compare_stat_name(VkrRecordingCompareStatId id) {
  return (uint32_t)id < VKR_RECORDING_COMPARE_STAT_COUNT
             ? VKR_RECORDING_COMPARE_STAT_NAMES[id]
             : "";
}

/* ------------------------------------------------------------------------ */
/* Statistics                                                                */

/* The peak of series `name`, when the recording measured it. */
vkr_internal bool8_t vkr_recording_compare_peak(
    const VkrRecordingData *data, const VkrRecordingAnalysis *analysis,
    const char *name, float64_t *out) {
  const uint32_t series = vkr_recording_data_find_series(data, name);
  if (series == UINT32_MAX || analysis->series[series].valid == 0u) {
    return false_v;
  }
  *out = analysis->series[series].max;
  return true_v;
}

vkr_internal void vkr_recording_compare_stats(const VkrRecordingData *a,
                                              const VkrRecordingAnalysis *aa,
                                              const VkrRecordingData *b,
                                              const VkrRecordingAnalysis *ba,
                                              VkrRecordingCompare *out) {
  const bool8_t frames = aa->frame_time.valid > 0u && ba->frame_time.valid > 0u;
  const float64_t pairs[][2] = {
      [VKR_RECORDING_COMPARE_P50] = {aa->frame_time.p50, ba->frame_time.p50},
      [VKR_RECORDING_COMPARE_P95] = {aa->frame_time.p95, ba->frame_time.p95},
      [VKR_RECORDING_COMPARE_P99] = {aa->frame_time.p99, ba->frame_time.p99},
      [VKR_RECORDING_COMPARE_MAX] = {aa->frame_time.max, ba->frame_time.max},
      [VKR_RECORDING_COMPARE_LOW_1PCT] = {aa->low_1pct_ms, ba->low_1pct_ms},
      [VKR_RECORDING_COMPARE_MEAN] = {aa->frame_time.mean, ba->frame_time.mean},
      [VKR_RECORDING_COMPARE_OVER_BUDGET] = {(float64_t)aa->over_budget,
                                             (float64_t)ba->over_budget},
  };
  for (uint32_t i = 0u; i < ArrayCount(pairs); ++i) {
    out->stats[i] = (VkrRecordingCompareStat){
        .a = pairs[i][0], .b = pairs[i][1], .valid = frames};
  }
  const char *const memory[] = {
      "process.resident_bytes",
      "memory.gpu.driver.current_allocated_bytes",
  };
  const VkrRecordingCompareStatId ids[] = {VKR_RECORDING_COMPARE_RAM_PEAK,
                                           VKR_RECORDING_COMPARE_VRAM_PEAK};
  for (uint32_t i = 0u; i < ArrayCount(ids); ++i) {
    VkrRecordingCompareStat *stat = &out->stats[ids[i]];
    stat->valid = vkr_recording_compare_peak(a, aa, memory[i], &stat->a) &&
                  vkr_recording_compare_peak(b, ba, memory[i], &stat->b);
  }
}

/* ------------------------------------------------------------------------ */
/* Passes                                                                    */

vkr_internal int32_t vkr_recording_compare_pass_order(const void *lhs,
                                                      const void *rhs) {
  const VkrRecordingPassDelta *a = lhs;
  const VkrRecordingPassDelta *b = rhs;
  const float64_t da = vkr_abs_f64(a->b_ms - a->a_ms);
  const float64_t db = vkr_abs_f64(b->b_ms - b->a_ms);
  if (da != db) {
    return da > db ? -1 : 1;
  }
  return string_compare(a->name, b->name);
}

vkr_internal float64_t vkr_recording_compare_pass_mean(
    const VkrRecordingPassStats *stats, bool8_t gpu) {
  const VkrRecordingStats *shown = gpu ? &stats->gpu : &stats->cpu;
  return shown->valid > 0u ? shown->mean : 0.0;
}

/* The analysis's stats of the pass named `name`, or NULL. */
vkr_internal const VkrRecordingPassStats *
vkr_recording_compare_find_pass(const VkrRecordingData *data,
                                const VkrRecordingAnalysis *analysis,
                                const char *name) {
  for (uint32_t i = 0u; i < analysis->pass_count; ++i) {
    if (string_equals(data->passes[analysis->passes[i].pass].name, name)) {
      return &analysis->passes[i];
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_recording_compare_passes(
    const VkrRecordingData *a, const VkrRecordingAnalysis *aa,
    const VkrRecordingData *b, const VkrRecordingAnalysis *ba,
    VkrAllocator *allocator, VkrRecordingCompare *out) {
  const uint64_t capacity = (uint64_t)aa->pass_count + ba->pass_count;
  if (capacity == 0u) {
    return true_v;
  }
  out->pass_gpu = a->pass_gpu_frames > 0u && b->pass_gpu_frames > 0u;
  const uint64_t bytes = sizeof(VkrRecordingPassDelta) * capacity;
  VkrRecordingPassDelta *all =
      vkr_allocator_alloc(allocator, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!all) {
    return false_v;
  }
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < aa->pass_count; ++i) {
    const char *name = a->passes[aa->passes[i].pass].name;
    const VkrRecordingPassStats *other =
        vkr_recording_compare_find_pass(b, ba, name);
    VkrRecordingPassDelta *delta = &all[count++];
    *delta = (VkrRecordingPassDelta){
        .a_ms = vkr_recording_compare_pass_mean(&aa->passes[i], out->pass_gpu),
        .b_ms =
            other ? vkr_recording_compare_pass_mean(other, out->pass_gpu) : 0.0,
        .in_a = true_v,
        .in_b = other != NULL,
    };
    string_format(delta->name, sizeof(delta->name), "%s", name);
  }
  for (uint32_t i = 0u; i < ba->pass_count; ++i) {
    const char *name = b->passes[ba->passes[i].pass].name;
    if (vkr_recording_compare_find_pass(a, aa, name)) {
      continue;
    }
    VkrRecordingPassDelta *delta = &all[count++];
    *delta = (VkrRecordingPassDelta){
        .b_ms = vkr_recording_compare_pass_mean(&ba->passes[i], out->pass_gpu),
        .in_b = true_v,
    };
    string_format(delta->name, sizeof(delta->name), "%s", name);
  }
  vkr_sort(all, count, sizeof(*all), vkr_recording_compare_pass_order);
  out->pass_count = Min(count, (uint32_t)VKR_RECORDING_COMPARE_PASS_MAX);
  MemCopy(out->passes, all, sizeof(*all) * out->pass_count);
  vkr_allocator_free(allocator, all, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Alignment                                                                 */

/* The first marker labelled `label` in `data`, or NULL. */
vkr_internal const VkrRecordingDataMarker *
vkr_recording_compare_marker(const VkrRecordingData *data, const char *label) {
  for (uint32_t i = 0u; i < data->marker_count; ++i) {
    if (string_equals(data->markers[i].label, label)) {
      return &data->markers[i];
    }
  }
  return NULL;
}

/* Meters along the path at each frame: the player's, holding its last
   position between ticks, or the camera's. */
vkr_internal void vkr_recording_compare_distance(const VkrRecordingData *data,
                                                 bool8_t player,
                                                 float64_t *out) {
  const float64_t *last = NULL;
  float64_t travelled = 0.0;
  for (uint64_t f = 0u; f < data->frame_count; ++f) {
    const bool8_t valid =
        player ? data->player_valid[f] != 0u : data->camera_valid[f] != 0u;
    const float64_t *at =
        player ? &data->player_positions[f * 3u] : &data->positions[f * 3u];
    if (valid) {
      if (last) {
        const float64_t dx = at[0] - last[0];
        const float64_t dy = at[1] - last[1];
        const float64_t dz = at[2] - last[2];
        travelled += vkr_sqrt_f64(dx * dx + dy * dy + dz * dz);
      }
      last = at;
    }
    out[f] = travelled;
  }
}

/* Each frame's key for the comparison's alignment. */
vkr_internal void vkr_recording_compare_keys(const VkrRecordingData *data,
                                             const VkrRecordingCompare *compare,
                                             float64_t marker_time,
                                             float64_t *out) {
  if (compare->align == VKR_RECORDING_ALIGN_DISTANCE) {
    vkr_recording_compare_distance(data, compare->distance_player, out);
    return;
  }
  for (uint64_t f = 0u; f < data->frame_count; ++f) {
    out[f] = compare->align == VKR_RECORDING_ALIGN_FRAME
                 ? (float64_t)f
                 : data->times[f] - marker_time;
  }
}

/* The frame of B whose key is nearest `key`, or UINT64_MAX outside B's
   keys. Keys never decrease. */
vkr_internal uint64_t vkr_recording_compare_nearest(const float64_t *keys,
                                                    uint64_t count,
                                                    float64_t key) {
  if (count == 0u || key < keys[0] || key > keys[count - 1u]) {
    return UINT64_MAX;
  }
  uint64_t low = 0u;
  uint64_t high = count;
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2u;
    if (keys[middle] < key) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  if (low > 0u && (low == count || key - keys[low - 1u] < keys[low] - key)) {
    --low;
  }
  return low;
}

/* Maps A's frames to B's and measures the difference over the mapped ones. */
vkr_internal void vkr_recording_compare_align(const VkrRecordingData *a,
                                              const VkrRecordingData *b,
                                              const float64_t *a_keys,
                                              const float64_t *b_keys,
                                              VkrRecordingCompare *out) {
  float64_t window[VKR_RECORDING_COMPARE_WINDOW];
  uint64_t window_frames[VKR_RECORDING_COMPARE_WINDOW];
  float64_t window_sum = 0.0;
  float64_t sum = 0.0;
  out->worst_first = out->worst_last = UINT64_MAX;
  for (uint64_t f = 0u; f < a->frame_count; ++f) {
    uint64_t match =
        vkr_recording_compare_nearest(b_keys, b->frame_count, a_keys[f]);
    float64_t a_ms = 0.0;
    float64_t b_ms = 0.0;
    if (match == UINT64_MAX || !vkr_recording_data_frame_ms(a, f, &a_ms) ||
        !vkr_recording_data_frame_ms(b, match, &b_ms)) {
      out->b_frame[f] = UINT64_MAX;
      continue;
    }
    out->b_frame[f] = match;
    const float64_t delta = b_ms - a_ms;
    const uint64_t slot = out->aligned_frames % VKR_RECORDING_COMPARE_WINDOW;
    if (out->aligned_frames >= VKR_RECORDING_COMPARE_WINDOW) {
      window_sum -= window[slot];
    }
    window[slot] = delta;
    window_frames[slot] = f;
    window_sum += delta;
    sum += delta;
    ++out->aligned_frames;
    if (out->aligned_frames >= VKR_RECORDING_COMPARE_WINDOW) {
      const float64_t mean = window_sum / VKR_RECORDING_COMPARE_WINDOW;
      if (out->worst_first == UINT64_MAX || mean > out->worst_delta_ms) {
        out->worst_delta_ms = mean;
        out->worst_first =
            window_frames[(slot + 1u) % VKR_RECORDING_COMPARE_WINDOW];
        out->worst_last = f;
      }
    }
  }
  out->delta_mean_ms =
      out->aligned_frames > 0u ? sum / (float64_t)out->aligned_frames : 0.0;
}

/* Settles the alignment's inputs: the marker both carry, or the path. */
vkr_internal bool8_t vkr_recording_compare_prepare(
    const VkrRecordingData *a, const VkrRecordingData *b,
    const VkrRecordingCompareOptions *options, VkrRecordingCompare *out,
    char *error, uint64_t error_capacity) {
  if (out->align == VKR_RECORDING_ALIGN_MARKER) {
    const VkrRecordingDataMarker *in_a = NULL;
    const VkrRecordingDataMarker *in_b = NULL;
    if (options->marker && options->marker[0]) {
      in_a = vkr_recording_compare_marker(a, options->marker);
      in_b = vkr_recording_compare_marker(b, options->marker);
    } else {
      for (uint32_t i = 0u; i < a->marker_count && !in_b; ++i) {
        in_a = &a->markers[i];
        in_b = vkr_recording_compare_marker(b, in_a->label);
      }
    }
    if (!in_a || !in_b) {
      string_format(error, error_capacity,
                    options->marker && options->marker[0]
                        ? "both recordings need a marker labelled '%s'"
                        : "the recordings share no marker label%s",
                    options->marker ? options->marker : "");
      return false_v;
    }
    string_format(out->marker, sizeof(out->marker), "%s", in_a->label);
    out->marker_a_time = in_a->time;
    out->marker_b_time = in_b->time;
  }
  if (out->align == VKR_RECORDING_ALIGN_DISTANCE) {
    out->distance_player = a->has_player && b->has_player;
  }
  return true_v;
}

bool8_t vkr_recording_compare(const VkrRecordingData *a,
                              const VkrRecordingAnalysis *a_analysis,
                              const VkrRecordingData *b,
                              const VkrRecordingAnalysis *b_analysis,
                              const VkrRecordingCompareOptions *options,
                              VkrAllocator *allocator, VkrRecordingCompare *out,
                              char *error, uint64_t error_capacity) {
  vkr_assert(a && a_analysis && b && b_analysis && allocator && out && error);
  const VkrRecordingCompareOptions defaults = {0};
  options = options ? options : &defaults;
  MemZero(out, sizeof(*out));
  out->allocator = allocator;
  out->align = (uint32_t)options->align < VKR_RECORDING_ALIGN_COUNT
                   ? options->align
                   : VKR_RECORDING_ALIGN_TIME;
  if (!vkr_recording_compare_prepare(a, b, options, out, error,
                                     error_capacity)) {
    return false_v;
  }
  vkr_recording_compare_stats(a, a_analysis, b, b_analysis, out);
  if (!vkr_recording_compare_passes(a, a_analysis, b, b_analysis, allocator,
                                    out)) {
    string_format(error, error_capacity, "out of memory");
    return false_v;
  }
  /* The mapping lives in the block; the keys only while it is built. */
  out->block_bytes = sizeof(uint64_t) * a->frame_count;
  out->block = vkr_allocator_alloc(allocator, out->block_bytes,
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  const uint64_t key_bytes =
      sizeof(float64_t) * (a->frame_count + b->frame_count);
  float64_t *keys =
      vkr_allocator_alloc(allocator, key_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!out->block || !keys) {
    if (keys) {
      vkr_allocator_free(allocator, keys, key_bytes,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    vkr_recording_compare_free(out);
    string_format(error, error_capacity, "out of memory");
    return false_v;
  }
  out->b_frame = out->block;
  vkr_recording_compare_keys(a, out, out->marker_a_time, keys);
  vkr_recording_compare_keys(b, out, out->marker_b_time, keys + a->frame_count);
  vkr_recording_compare_align(a, b, keys, keys + a->frame_count, out);
  vkr_allocator_free(allocator, keys, key_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

void vkr_recording_compare_free(VkrRecordingCompare *compare) {
  vkr_assert(compare != NULL);
  if (compare->block) {
    vkr_allocator_free(compare->allocator, compare->block, compare->block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  MemZero(compare, sizeof(*compare));
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

vkr_internal void vkr_recording_compare_name(VkrJsonWriter *json,
                                             const char *name) {
  vkr_json_writer_name(json, string8_create_from_cstr((const uint8_t *)name,
                                                      string_length(name)));
}

vkr_internal void vkr_recording_compare_number(VkrJsonWriter *json,
                                               const char *name,
                                               float64_t value) {
  vkr_recording_compare_name(json, name);
  vkr_json_writer_f64_digits(json, value, 6u);
}

void vkr_recording_compare_write_json(VkrJsonWriter *json,
                                      const VkrRecordingData *a,
                                      const VkrRecordingCompare *compare) {
  vkr_json_writer_begin_object(json);
  vkr_recording_compare_name(json, "align");
  vkr_json_writer_string(
      json, string8_create_from_cstr(
                (const uint8_t *)vkr_recording_align_name(compare->align),
                string_length(vkr_recording_align_name(compare->align))));
  if (compare->align == VKR_RECORDING_ALIGN_MARKER) {
    vkr_recording_compare_name(json, "marker");
    vkr_json_writer_string(
        json, string8_create_from_cstr((const uint8_t *)compare->marker,
                                       string_length(compare->marker)));
    vkr_recording_compare_number(json, "marker_a_time", compare->marker_a_time);
    vkr_recording_compare_number(json, "marker_b_time", compare->marker_b_time);
  }
  if (compare->align == VKR_RECORDING_ALIGN_DISTANCE) {
    vkr_recording_compare_name(json, "distance_path");
    vkr_json_writer_string(json, compare->distance_player
                                     ? string8_lit("player")
                                     : string8_lit("camera"));
  }

  /* Each statistic as [a, b, b - a]; lower is better for all of them. */
  vkr_recording_compare_name(json, "stats");
  vkr_json_writer_begin_object(json);
  for (uint32_t i = 0u; i < VKR_RECORDING_COMPARE_STAT_COUNT; ++i) {
    const VkrRecordingCompareStat *stat = &compare->stats[i];
    vkr_recording_compare_name(
        json, vkr_recording_compare_stat_name((VkrRecordingCompareStatId)i));
    if (!stat->valid) {
      vkr_json_writer_null(json);
      continue;
    }
    vkr_json_writer_begin_array(json);
    vkr_json_writer_f64_digits(json, stat->a, 6u);
    vkr_json_writer_f64_digits(json, stat->b, 6u);
    vkr_json_writer_f64_digits(json, stat->b - stat->a, 6u);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_object(json);

  vkr_recording_compare_name(json, "aligned_frames");
  vkr_json_writer_u64(json, compare->aligned_frames);
  vkr_recording_compare_name(json, "frames_a");
  vkr_json_writer_u64(json, a->frame_count);
  vkr_recording_compare_number(json, "delta_mean_ms", compare->delta_mean_ms);
  vkr_recording_compare_name(json, "slowest_window");
  if (compare->worst_first == UINT64_MAX) {
    vkr_json_writer_null(json);
  } else {
    vkr_json_writer_begin_object(json);
    vkr_recording_compare_name(json, "first_frame_a");
    vkr_json_writer_u64(json, compare->worst_first);
    vkr_recording_compare_name(json, "last_frame_a");
    vkr_json_writer_u64(json, compare->worst_last);
    vkr_recording_compare_number(json, "time_a",
                                 a->times[compare->worst_first]);
    vkr_recording_compare_number(json, "delta_mean_ms",
                                 compare->worst_delta_ms);
    vkr_json_writer_end_object(json);
  }

  vkr_recording_compare_name(json, "pass_times");
  vkr_json_writer_string(json, compare->pass_gpu ? string8_lit("gpu")
                                                 : string8_lit("cpu"));
  vkr_recording_compare_name(json, "pass_columns");
  vkr_json_writer_begin_array(json);
  const char *const columns[] = {"name", "a_ms", "b_ms", "delta_ms"};
  for (uint32_t i = 0u; i < ArrayCount(columns); ++i) {
    vkr_json_writer_string(json,
                           string8_create_from_cstr((const uint8_t *)columns[i],
                                                    string_length(columns[i])));
  }
  vkr_json_writer_end_array(json);
  vkr_recording_compare_name(json, "passes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < compare->pass_count; ++i) {
    const VkrRecordingPassDelta *delta = &compare->passes[i];
    vkr_json_writer_begin_array(json);
    vkr_json_writer_string(
        json, string8_create_from_cstr((const uint8_t *)delta->name,
                                       string_length(delta->name)));
    if (delta->in_a) {
      vkr_json_writer_f64_digits(json, delta->a_ms, 6u);
    } else {
      vkr_json_writer_null(json);
    }
    if (delta->in_b) {
      vkr_json_writer_f64_digits(json, delta->b_ms, 6u);
    } else {
      vkr_json_writer_null(json);
    }
    vkr_json_writer_f64_digits(json, delta->b_ms - delta->a_ms, 6u);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
}
