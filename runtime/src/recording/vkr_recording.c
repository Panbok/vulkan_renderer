#include "recording/vkr_recording.h"

#include "assets/vkr_recording_analysis.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"

/* The recorder's rings, chunk buffer and writer scratch live here; the
   reservation only commits pages the recording touches. */
#define VKR_RECORDING_MEMORY_RESERVE (256ull * 1024ull * 1024ull)
#define VKR_RECORDING_MEMORY_COMMIT (4ull * 1024ull * 1024ull)
/* Two 4 MiB rings: a frame and camera record pair is under 100 bytes, so a
   ring holds tens of thousands of frames before the writer must drain it. */
#define VKR_RECORDING_RING_BYTES (4ull * 1024ull * 1024ull)

static const VkrRecordingField VKR_RECORDING_FRAME_FIELDS[] = {
    {.name = "display_dt",
     .unit = "s",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 0u},
    {.name = "simulation_time",
     .unit = "s",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 8u},
    {.name = "time_of_day",
     .unit = "h",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 16u},
    {.name = "flags",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 20u},
};

static const VkrRecordingField VKR_RECORDING_PLAYER_FIELDS[] = {
    {.name = "tick",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 0u},
    {.name = "position",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 3u,
     .offset = 8u},
    {.name = "velocity",
     .unit = "m/s",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 3u,
     .offset = 32u},
    {.name = "yaw",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 44u},
    {.name = "pitch",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 48u},
    {.name = "flags",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 52u},
    {.name = "state",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 56u},
};

static const VkrRecordingField VKR_RECORDING_EDIT_FIELDS[] = {
    {.name = "sequence",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 0u},
};

static const VkrRecordingField VKR_RECORDING_ACTION_FIELDS[] = {
    {.name = "tick",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 0u},
    {.name = "sequence",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 8u},
    {.name = "action",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 16u},
    {.name = "pressed",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 20u},
    {.name = "x", .type = VKR_RECORDING_FIELD_F32, .count = 1u, .offset = 24u},
    {.name = "y", .type = VKR_RECORDING_FIELD_F32, .count = 1u, .offset = 28u},
};

static const VkrRecordingField VKR_RECORDING_CAMERA_FIELDS[] = {
    {.name = "position",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 3u,
     .offset = 0u},
    {.name = "yaw",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 24u},
    {.name = "pitch",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 28u},
    {.name = "fov",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 32u},
    {.name = "near_clip",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 36u},
    {.name = "far_clip",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 40u},
    {.name = "projection",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 44u},
};

static const VkrRecordingStream VKR_RECORDING_STREAMS[] = {
    {.id = VKR_RECORDING_STREAM_FRAME,
     .name = "frame",
     .record_size = sizeof(VkrRecordingFrame),
     .field_count = ArrayCount(VKR_RECORDING_FRAME_FIELDS),
     .fields = VKR_RECORDING_FRAME_FIELDS},
    {.id = VKR_RECORDING_STREAM_CAMERA,
     .name = "camera",
     .record_size = sizeof(VkrRecordingCamera),
     .field_count = ArrayCount(VKR_RECORDING_CAMERA_FIELDS),
     .fields = VKR_RECORDING_CAMERA_FIELDS},
    /* A u32 kind, then the UTF-8 label. */
    {.id = VKR_RECORDING_STREAM_MARKER, .name = "marker"},
    {.id = VKR_RECORDING_STREAM_PLAYER,
     .name = "player",
     .record_size = sizeof(VkrRecordingPlayer),
     .field_count = ArrayCount(VKR_RECORDING_PLAYER_FIELDS),
     .fields = VKR_RECORDING_PLAYER_FIELDS},
    /* A u16 key length, the key, then the value. */
    {.id = VKR_RECORDING_STREAM_TAG, .name = "tag"},
    /* A u16 name length, the name, then the payload. */
    {.id = VKR_RECORDING_STREAM_EVENT, .name = "event"},
    {.id = VKR_RECORDING_STREAM_EDIT,
     .name = "edit",
     .record_size = sizeof(uint64_t),
     .field_count = 1u,
     .fields = VKR_RECORDING_EDIT_FIELDS},
    {.id = VKR_RECORDING_STREAM_ACTION,
     .name = "action",
     .record_size = sizeof(VkrRecordingAction),
     .field_count = ArrayCount(VKR_RECORDING_ACTION_FIELDS),
     .fields = VKR_RECORDING_ACTION_FIELDS},
};

/* Tier 0 (docs/proposals/recordings.md, "Metrics and capture tiers"):
   whatever of these the registry holds, in this order. */
static const char *const VKR_RECORDING_TIER0_METRICS[] = {
    "frame.wall",
    "cpu.frame_work",
    "cpu.update",
    "cpu.simulation",
    "cpu.physics",
    "cpu.render_prepare",
    "cpu.render_submit",
    "cpu.render_wait",
    "cpu.shadow_update",
    "cpu.world_payload_build",
    "cpu.graph_build",
    "cpu.graph_compile",
    "gpu.submission",
    "frame.render_scale",
    "draw.world.calls_issued",
    "draw.world.commands_issued",
    "visibility.objects_tested",
    "upload.fence_waits",
    "memory.gpu.driver.current_allocated_bytes",
    "memory.gpu.driver.recommended_budget_bytes",
};

/* Sampling period of the process's resident memory, in seconds. */
#define VKR_RECORDING_RESIDENT_PERIOD 0.1
/* Open-addressed pass name slots: twice the names, a power of two. */
#define VKR_RECORDING_PASS_SLOTS (2u * VKR_RECORDING_PASS_MAX)

_Static_assert(sizeof(VkrRecordingFrame) == 24u, "frame record layout");
_Static_assert(sizeof(VkrRecordingPose) == 88u, "pose record layout");
_Static_assert(sizeof(VkrRecordingPlayer) == 64u, "player record layout");
_Static_assert(sizeof(VkrRecordingAction) == 32u, "action record layout");
_Static_assert(sizeof(VkrRecordingCamera) == 48u, "camera record layout");

const VkrRecordingStream *vkr_recording_streams(uint32_t *out_count) {
  *out_count = ArrayCount(VKR_RECORDING_STREAMS);
  return VKR_RECORDING_STREAMS;
}

vkr_internal void vkr_recording_copy_text(char *destination, uint64_t capacity,
                                          const char *source) {
  const uint64_t length =
      source ? string_length_bounded(source, capacity - 1u) : 0u;
  if (length > 0u) {
    MemCopy(destination, source, length);
  }
  destination[length] = '\0';
}

/* Creates each missing directory above the bundle, then the bundle itself,
   which must not exist yet. */
vkr_internal bool8_t vkr_recording_create_bundle(VkrRecording *recording) {
  char prefix[VKR_RECORDING_PATH_MAX];
  const uint64_t length = string_length(recording->bundle_path);
  MemCopy(prefix, recording->bundle_path, length + 1u);
  for (uint64_t i = 1u; i < length; ++i) {
    if (prefix[i] != '/' && prefix[i] != '\\') {
      continue;
    }
    const char separator = prefix[i];
    prefix[i] = '\0';
    const FilePath parent = file_path_view(prefix);
    /* A drive root such as "C:" cannot be created and need not be. */
    const bool8_t drive = i == 2u && prefix[1] == ':';
    if (!drive && !file_create_directory(&parent)) {
      return false_v;
    }
    prefix[i] = separator;
  }
  const FilePath path = file_path_view(recording->bundle_path);
  return file_create_directory_exclusive(&path) == FILE_ERROR_NONE;
}

vkr_internal const char *vkr_recording_unit(VkrMetricUnit unit) {
  switch (unit) {
  case VKR_METRIC_UNIT_NANOSECONDS:
    return "ms";
  case VKR_METRIC_UNIT_BYTES:
    return "bytes";
  case VKR_METRIC_UNIT_RATIO:
    return "ratio";
  case VKR_METRIC_UNIT_PERCENT:
    return "percent";
  case VKR_METRIC_UNIT_COUNT_PER_SECOND:
    return "count/s";
  default:
    return "count";
  }
}

/* Finds the Tier 0 metrics in the registry and describes them, after the
   `valid` mask, in `fields`; returns the field count. */
vkr_internal uint32_t vkr_recording_metric_fields(VkrRecording *recording,
                                                  const VkrMetrics *metrics,
                                                  VkrRecordingField *fields) {
  fields[0] = (VkrRecordingField){
      .name = "valid", .type = VKR_RECORDING_FIELD_U64, .count = 1u};
  uint32_t catalog_count = 0u;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(metrics, &catalog_count);
  for (uint32_t m = 0u; m < ArrayCount(VKR_RECORDING_TIER0_METRICS) &&
                        recording->metric_count + 1u < VKR_RECORDING_METRIC_MAX;
       ++m) {
    for (uint32_t i = 0u; i < catalog_count; ++i) {
      if (!string_equals(catalog[i].name, VKR_RECORDING_TIER0_METRICS[m])) {
        continue;
      }
      const uint32_t index = recording->metric_count++;
      recording->metric_ids[index] =
          ((VkrMetricId)metrics->registry_generation << 16u) | i;
      recording->metric_kind[index] = catalog[i].kind;
      recording->metric_scale[index] =
          catalog[i].unit == VKR_METRIC_UNIT_NANOSECONDS ? 1.0e-6 : 1.0;
      fields[1u + index] = (VkrRecordingField){
          .name = catalog[i].name,
          .unit = vkr_recording_unit(catalog[i].unit),
          .type = VKR_RECORDING_FIELD_F64,
          .count = 1u,
          .offset = 8u + 8u * index,
      };
      break;
    }
  }
  const uint32_t resident = recording->metric_count;
  fields[1u + resident] = (VkrRecordingField){
      .name = VKR_RECORDING_RESIDENT_FIELD,
      .unit = "bytes",
      .type = VKR_RECORDING_FIELD_F64,
      .count = 1u,
      .offset = 8u + 8u * resident,
  };
  return resident + 2u;
}

/* The pass name table, from the recording's memory, and an empty frame
   history. */
vkr_internal bool8_t vkr_recording_passes_create(VkrRecording *recording) {
  const uint64_t name_bytes =
      sizeof(*recording->pass_names) * (uint64_t)VKR_RECORDING_PASS_MAX;
  const uint64_t slot_bytes =
      sizeof(*recording->pass_slots) * (uint64_t)VKR_RECORDING_PASS_SLOTS;
  recording->pass_names = vkr_allocator_alloc(&recording->allocator, name_bytes,
                                              VKR_ALLOCATOR_MEMORY_TAG_STRING);
  recording->pass_slots = vkr_allocator_alloc(
      &recording->allocator, slot_bytes, VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!recording->pass_names || !recording->pass_slots) {
    return false_v;
  }
  MemZero(recording->pass_names, name_bytes);
  MemZero(recording->pass_slots, slot_bytes);
  for (uint32_t i = 0u; i < VKR_RECORDING_FRAME_INDEX_HISTORY; ++i) {
    recording->frame_history[i] = (VkrRecordingFrameIndex){
        .frame_index = UINT64_MAX, .frame = UINT64_MAX};
  }
  recording->passes_enabled = true_v;
  return true_v;
}

bool8_t vkr_recording_start(VkrRecording *recording,
                            const VkrRecordingConfig *config) {
  vkr_assert(recording != NULL && config != NULL);
  vkr_assert(!recording->active && "stop the active recording first");
  MemZero(recording, sizeof(*recording));
  if (!config->bundle_path ||
      string_length(config->bundle_path) + 16u >= VKR_RECORDING_PATH_MAX) {
    return false_v;
  }
  if (!vkr_dmemory_create(VKR_RECORDING_MEMORY_COMMIT,
                          VKR_RECORDING_MEMORY_RESERVE, &recording->memory)) {
    return false_v;
  }
  recording->allocator = (VkrAllocator){.ctx = &recording->memory};
  vkr_dmemory_allocator_create(&recording->allocator);

  vkr_recording_copy_text(recording->bundle_path,
                          sizeof(recording->bundle_path), config->bundle_path);
  vkr_recording_copy_text(recording->name, sizeof(recording->name),
                          config->name);
  vkr_recording_copy_text(recording->scene, sizeof(recording->scene),
                          config->scene);
  vkr_recording_copy_text(recording->author, sizeof(recording->author),
                          config->author ? config->author : "user");
  recording->started_unix = vkr_platform_get_unix_time();

  char stream_path[VKR_RECORDING_PATH_MAX];
  string_format(stream_path, sizeof(stream_path), "%s/%s",
                recording->bundle_path, VKR_RECORDING_STREAM_FILE);
  uint32_t base_count = 0u;
  const VkrRecordingStream *base = vkr_recording_streams(&base_count);
  VkrRecordingStream streams[16];
  _Static_assert(ArrayCount(VKR_RECORDING_STREAMS) + 4u <= 16u,
                 "room for the metrics, pose and pass streams");
  VkrRecordingField metric_fields[VKR_RECORDING_METRIC_MAX + 1u];
  MemCopy(streams, base, sizeof(*base) * base_count);
  uint32_t stream_count = base_count;
  if (config->metrics) {
    recording->metrics_enabled = true_v;
    const uint32_t field_count =
        vkr_recording_metric_fields(recording, config->metrics, metric_fields);
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_METRICS,
        .flags = VKR_RECORDING_STREAM_DROPPABLE,
        .name = "metrics",
        .record_size = 8u * field_count,
        .field_count = field_count,
        .fields = metric_fields,
    };
  }
  if (config->poses) {
    recording->pose_batch = vkr_allocator_alloc(
        &recording->allocator,
        sizeof(VkrRecordingPose) * (uint64_t)VKR_RECORDING_POSE_BATCH,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!recording->pose_batch) {
      goto cleanup;
    }
    recording->poses_enabled = true_v;
    /* Variable records of VkrRecordingPose entries. */
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_POSE, .name = "pose"};
  }
  if (config->metrics && config->passes) {
    if (!vkr_recording_passes_create(recording)) {
      goto cleanup;
    }
    recording->pass_gpu_period = config->pass_gpu_period;
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_PASS_NAME, .name = "pass_name"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASSES,
                             .flags = VKR_RECORDING_STREAM_DROPPABLE,
                             .name = "passes"};
  }
  const VkrRecorderConfig recorder_config = {
      .stream_path = stream_path,
      .codec = config->codec,
      .identity_json = config->identity_json,
      .streams = streams,
      .stream_count = stream_count,
      .ring_bytes = VKR_RECORDING_RING_BYTES,
  };
  if (!vkr_recording_create_bundle(recording)) {
    goto cleanup;
  }
  recording->recorder =
      vkr_recorder_start(&recording->allocator, &recorder_config);
  if (!recording->recorder) {
    goto cleanup;
  }
  recording->active = true_v;
  return true_v;

cleanup:
  vkr_dmemory_allocator_destroy(&recording->allocator);
  MemZero(recording, sizeof(*recording));
  return false_v;
}

void vkr_recording_frame(VkrRecording *recording,
                         const VkrRecordingFrame *frame,
                         const VkrRecordingCamera *camera) {
  vkr_assert(recording != NULL && recording->active);
  /* Frame 0 is at time zero; each later frame is its display_dt after the
     one before (VkrRecordingPlayback uses the same clock). */
  if (recording->frame > 0u) {
    recording->time += frame->display_dt;
  }
  (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_FRAME, recording->frame, frame,
                           sizeof(*frame));
  (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_CAMERA, recording->frame,
                           camera, sizeof(*camera));
  ++recording->frame;
}

/* The value a recording keeps of one published sample: a duration's sum, a
   counter's or gauge's value, in the field's unit. */
vkr_internal bool8_t vkr_recording_metric_value(
    const VkrRecording *recording, uint32_t index,
    const VkrMetricsFrame *published, float64_t *out_value) {
  const VkrMetricId id = recording->metric_ids[index];
  const float64_t scale = recording->metric_scale[index];
  if (recording->metric_kind[index] == VKR_METRIC_KIND_DURATION) {
    VkrMetricDurationSample sample;
    if (!vkr_metrics_frame_read_duration(published, id, &sample) ||
        sample.count == 0u) {
      return false_v;
    }
    *out_value = (float64_t)sample.sum_ns * scale;
    return true_v;
  }
  uint64_t whole = 0u;
  if (vkr_metrics_frame_read_u64(published, id, &whole)) {
    *out_value = (float64_t)whole * scale;
    return true_v;
  }
  float64_t real = 0.0;
  if (vkr_metrics_frame_read_f64(published, id, &real)) {
    *out_value = real * scale;
    return true_v;
  }
  return false_v;
}

void vkr_recording_metrics(VkrRecording *recording, uint64_t frame,
                           const VkrMetricsFrame *published) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->metrics_enabled) {
    return;
  }
  /* Resident memory is a system call, so it is sampled, then carried. */
  const float64_t now = vkr_platform_get_absolute_time();
  if (now - recording->resident_sampled_at >= VKR_RECORDING_RESIDENT_PERIOD) {
    recording->resident_sampled_at = now;
    recording->resident_valid =
        vkr_platform_get_process_resident_memory(&recording->resident_bytes);
  }
  const uint32_t count = recording->metric_count;
  const uint32_t size = 8u * (count + 2u);
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_METRICS, frame, size);
  if (!record) {
    return;
  }
  uint64_t valid = 0u;
  for (uint32_t i = 0u; i <= count; ++i) {
    float64_t value = 0.0;
    const bool8_t present =
        i < count ? published && vkr_recording_metric_value(recording, i,
                                                            published, &value)
                  : recording->resident_valid;
    if (i == count && present) {
      value = (float64_t)recording->resident_bytes;
    }
    valid |= present ? 1ull << i : 0ull;
    MemCopy(record + 8u + 8u * i, &value, sizeof(value));
  }
  MemCopy(record, &valid, sizeof(valid));
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
}

/* The id of the pass named `name`; a new name gets the next id and a name
   record first. UINT16_MAX when the table is full or the record was lost,
   so a later frame tries again. */
vkr_internal uint16_t vkr_recording_pass_id(VkrRecording *recording,
                                            uint64_t frame, const char *name,
                                            uint32_t length) {
  const VkrHashTableKey key = vkr_hash_table_key_string8(
      string8_create_from_cstr((const uint8_t *)name, length));
  const uint32_t mask = VKR_RECORDING_PASS_SLOTS - 1u;
  uint32_t probe = (uint32_t)key.hash & mask;
  while (recording->pass_slots[probe] != 0u) {
    const uint16_t id = (uint16_t)(recording->pass_slots[probe] - 1u);
    const char *known = recording->pass_names[id];
    if (string_length(known) == length &&
        MemCompare(known, name, length) == 0) {
      return id;
    }
    probe = (probe + 1u) & mask;
  }
  if (recording->pass_count >= VKR_RECORDING_PASS_MAX) {
    return UINT16_MAX;
  }
  const uint16_t id = (uint16_t)recording->pass_count;
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_PASS_NAME, frame, 4u + length);
  if (!record) {
    return UINT16_MAX;
  }
  const uint32_t id_value = id;
  MemCopy(record, &id_value, sizeof(id_value));
  MemCopy(record + 4u, name, length);
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  MemCopy(recording->pass_names[id], name, length);
  recording->pass_names[id][length] = '\0';
  recording->pass_slots[probe] = (uint16_t)(id + 1u);
  ++recording->pass_count;
  return id;
}

/* The recorded frame that rendered renderer frame `frame_index`, or
   UINT64_MAX when it left the history or was never recorded. */
vkr_internal uint64_t vkr_recording_frame_of(const VkrRecording *recording,
                                             uint64_t frame_index) {
  for (uint32_t i = 0u; i < VKR_RECORDING_FRAME_INDEX_HISTORY; ++i) {
    if (recording->frame_history[i].frame_index == frame_index) {
      return recording->frame_history[i].frame;
    }
  }
  return UINT64_MAX;
}

void vkr_recording_passes(VkrRecording *recording, uint64_t frame,
                          uint64_t frame_index,
                          const VkrRendererMetricsPassTable *passes) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled) {
    return;
  }
  recording->frame_history[recording->frame_history_next] =
      (VkrRecordingFrameIndex){.frame_index = frame_index, .frame = frame};
  recording->frame_history_next =
      (recording->frame_history_next + 1u) % VKR_RECORDING_FRAME_INDEX_HISTORY;
  if (!passes || passes->count == 0u) {
    return;
  }
  /* Every row of a table names the same source frame. The table repeats
     the last completed frame until another completes. */
  const uint64_t source = passes->samples[0].gpu_source_frame_index;
  if (source < recording->pass_frame_written) {
    return;
  }
  const uint64_t target = vkr_recording_frame_of(recording, source);
  if (target == UINT64_MAX) {
    return;
  }
  recording->pass_frame_written = source + 1u;

  const uint32_t count = Min(passes->count, (uint32_t)VKR_RECORDING_PASS_MAX);
  uint16_t ids[VKR_RECORDING_PASS_MAX];
  uint32_t rows = 0u;
  uint32_t flags = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRendererMetricsPassSample *sample = &passes->samples[i];
    ids[i] = UINT16_MAX;
    if (sample->culled || sample->disabled) {
      continue;
    }
    ids[i] = vkr_recording_pass_id(recording, frame, sample->name,
                                   sample->name_length);
    if (ids[i] == UINT16_MAX) {
      continue;
    }
    ++rows;
    flags |= sample->gpu_valid ? VKR_RECORDING_PASSES_GPU_TIMED : 0u;
  }
  uint8_t *record = vkr_recorder_reserve(
      recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
      VKR_RECORDING_STREAM_PASSES, target,
      VKR_RECORDING_PASSES_HEADER_SIZE + VKR_RECORDING_PASS_ROW_SIZE * rows);
  if (!record) {
    return;
  }
  MemCopy(record, &rows, sizeof(rows));
  MemCopy(record + 4u, &flags, sizeof(flags));
  uint8_t *row = record + VKR_RECORDING_PASSES_HEADER_SIZE;
  for (uint32_t i = 0u; i < count; ++i) {
    if (ids[i] == UINT16_MAX) {
      continue;
    }
    const VkrRendererMetricsPassSample *sample = &passes->samples[i];
    const uint16_t row_flags =
        sample->gpu_valid ? VKR_RECORDING_PASS_GPU_VALID : 0u;
    const float32_t cpu_ms = (float32_t)sample->cpu_ms;
    const float32_t gpu_ms =
        sample->gpu_valid ? (float32_t)sample->gpu_ms : 0.0f;
    MemCopy(row, &ids[i], sizeof(ids[i]));
    MemCopy(row + 2u, &row_flags, sizeof(row_flags));
    MemCopy(row + 4u, &cpu_ms, sizeof(cpu_ms));
    MemCopy(row + 8u, &gpu_ms, sizeof(gpu_ms));
    row += VKR_RECORDING_PASS_ROW_SIZE;
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  recording->pass_rows_written += rows;
}

bool8_t vkr_recording_pass_gpu_frame(const VkrRecording *recording,
                                     bool8_t *out_timed) {
  vkr_assert(recording != NULL && out_timed != NULL);
  if (!recording->active || !recording->passes_enabled ||
      recording->pass_gpu_period == 0u) {
    return false_v;
  }
  *out_timed = recording->frame % recording->pass_gpu_period == 0u;
  return true_v;
}

void vkr_recording_poses_flush(VkrRecording *recording) {
  vkr_assert(recording != NULL && recording->active);
  const uint32_t count = recording->pose_batch_count;
  if (count == 0u) {
    return;
  }
  recording->pose_batch_count = 0u;
  /* The poses belong to the frame on screen: the last one written. */
  const uint64_t frame = recording->frame > 0u ? recording->frame - 1u : 0u;
  if (vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                         VKR_RECORDING_STREAM_POSE, frame,
                         recording->pose_batch,
                         (uint32_t)sizeof(VkrRecordingPose) * count)) {
    recording->poses_written += count;
  }
}

void vkr_recording_pose(VkrRecording *recording, const VkrRecordingPose *pose) {
  vkr_assert(recording != NULL && recording->active && pose != NULL);
  if (!recording->poses_enabled) {
    return;
  }
  recording->pose_batch[recording->pose_batch_count++] = *pose;
  if (recording->pose_batch_count == VKR_RECORDING_POSE_BATCH) {
    vkr_recording_poses_flush(recording);
  }
}

void vkr_recording_player(VkrRecording *recording,
                          const VkrRecordingPlayer *player) {
  vkr_assert(recording != NULL && recording->active && player != NULL);
  if (vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                         VKR_RECORDING_STREAM_PLAYER, recording->frame, player,
                         sizeof(*player))) {
    ++recording->player_records;
  }
}

void vkr_recording_edit(VkrRecording *recording, uint64_t sequence) {
  vkr_assert(recording != NULL && recording->active);
  const uint64_t frame = recording->frame > 0u ? recording->frame - 1u : 0u;
  (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_EDIT, frame, &sequence,
                           sizeof(sequence));
}

void vkr_recording_action(VkrRecording *recording,
                          const VkrRecordingAction *action) {
  vkr_assert(recording != NULL && recording->active && action != NULL);
  if (vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                         VKR_RECORDING_STREAM_ACTION, recording->frame, action,
                         sizeof(*action))) {
    ++recording->action_records;
  }
}

/* A u16 length and `head`, then `tail`: the layout of tags and events. */
vkr_internal void vkr_recording_text_record(VkrRecording *recording,
                                            uint16_t stream, String8 head,
                                            const void *tail,
                                            uint32_t tail_size) {
  const uint16_t length =
      (uint16_t)Min(head.length, (uint64_t)VKR_RECORDING_TEXT_MAX);
  uint8_t *record = vkr_recorder_reserve(
      recording->recorder, VKR_RECORDER_PRODUCER_MAIN, stream, recording->frame,
      2u + (uint32_t)length + tail_size);
  if (!record) {
    return;
  }
  MemCopy(record, &length, sizeof(length));
  if (length > 0u) {
    MemCopy(record + 2u, head.str, length);
  }
  if (tail_size > 0u) {
    MemCopy(record + 2u + length, tail, tail_size);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
}

void vkr_recording_tag(VkrRecording *recording, String8 key, String8 value) {
  vkr_assert(recording != NULL && recording->active);
  vkr_recording_text_record(
      recording, VKR_RECORDING_STREAM_TAG, key, value.str,
      (uint32_t)Min(value.length, (uint64_t)VKR_RECORDING_TEXT_MAX));
}

void vkr_recording_event(VkrRecording *recording, String8 name,
                         const void *payload, uint32_t size) {
  vkr_assert(recording != NULL && recording->active);
  vkr_recording_text_record(
      recording, VKR_RECORDING_STREAM_EVENT, name, payload,
      payload ? Min(size, VKR_RECORDING_EVENT_PAYLOAD_MAX) : 0u);
}

bool8_t vkr_recording_mark(VkrRecording *recording, VkrRecordingMarkerKind kind,
                           const char *label) {
  vkr_assert(recording != NULL && recording->active);
  const char *text = label ? label : "";
  const uint32_t length =
      (uint32_t)string_length_bounded(text, VKR_RECORDING_LABEL_MAX);
  /* A marker belongs to the frame on screen: the last one written. */
  const uint64_t frame = recording->frame > 0u ? recording->frame - 1u : 0u;
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_MARKER, frame, 4u + length);
  if (!record) {
    return false_v;
  }
  const uint32_t kind_value = (uint32_t)kind;
  MemCopy(record, &kind_value, sizeof(kind_value));
  if (length > 0u) {
    MemCopy(record + 4u, text, length);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);

  if (recording->marker_count < VKR_RECORDING_MARKER_MAX) {
    VkrRecordingMarker *marker = &recording->markers[recording->marker_count++];
    marker->frame = frame;
    marker->time = recording->time;
    marker->kind = (uint32_t)kind;
    vkr_recording_copy_text(marker->label, sizeof(marker->label), text);
  }
  return true_v;
}

void vkr_recording_stats(const VkrRecording *recording,
                         VkrRecorderStats *out_stats) {
  vkr_assert(recording != NULL && out_stats != NULL);
  if (!recording->active) {
    MemZero(out_stats, sizeof(*out_stats));
    return;
  }
  vkr_recorder_stats(recording->recorder, out_stats);
}

vkr_internal void vkr_recording_json_text(VkrJsonWriter *json, const char *name,
                                          const char *value) {
  vkr_json_writer_name(json, string8_create_from_cstr((const uint8_t *)name,
                                                      string_length(name)));
  vkr_json_writer_string(json, string8_create_from_cstr((const uint8_t *)value,
                                                        string_length(value)));
}

vkr_internal void vkr_recording_json_u64(VkrJsonWriter *json, const char *name,
                                         uint64_t value) {
  vkr_json_writer_name(json, string8_create_from_cstr((const uint8_t *)name,
                                                      string_length(name)));
  vkr_json_writer_u64(json, value);
}

vkr_internal void vkr_recording_json_f64(VkrJsonWriter *json, const char *name,
                                         float64_t value) {
  vkr_json_writer_name(json, string8_create_from_cstr((const uint8_t *)name,
                                                      string_length(name)));
  vkr_json_writer_f64(json, value);
}

vkr_internal void vkr_recording_write_markers(VkrJsonWriter *json,
                                              const VkrRecording *recording) {
  vkr_json_writer_name(json, string8_lit("markers"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < recording->marker_count; ++i) {
    const VkrRecordingMarker *marker = &recording->markers[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_json_u64(json, "frame", marker->frame);
    vkr_recording_json_f64(json, "time", marker->time);
    vkr_recording_json_text(json, "kind",
                            vkr_recording_marker_kind_name(marker->kind));
    vkr_recording_json_text(json, "label", marker->label);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
}

/* Reads the stream back and writes its statistics and spikes as the
   summary's "analysis": once, at stop, off the frame path. */
vkr_internal void vkr_recording_write_analysis(VkrRecording *recording,
                                               VkrJsonWriter *json) {
  char stream[VKR_RECORDING_PATH_MAX];
  string_format(stream, sizeof(stream), "%s/%s", recording->bundle_path,
                VKR_RECORDING_STREAM_FILE);
  VkrRecordingData data;
  if (vkr_recording_data_load(&data, &recording->allocator, stream,
                              vkr_content_codec_zstd_decoder()) !=
      VKR_RECORDING_DATA_OK) {
    return;
  }
  VkrRecordingAnalysis analysis;
  const VkrRecordingAnalysisOptions options = {0};
  if (vkr_recording_analyze(&data, &options, &recording->allocator,
                            &analysis)) {
    const VkrRecordingAnalysisJsonOptions json_options = {
        .budget_bytes = 8192u,
        .include_series = true_v,
    };
    vkr_json_writer_name(json, string8_lit("analysis"));
    (void)vkr_recording_analysis_write_json(json, &data, &analysis,
                                            &json_options);
    vkr_recording_analysis_free(&analysis);
  }
  vkr_recording_data_free(&data);
}

/* summary.json: the small file an agent or the Library reads first; readers
   ignore names they do not know. */
vkr_internal bool8_t vkr_recording_write_summary(VkrRecording *recording,
                                                 const VkrRecorderStats *stats,
                                                 bool8_t stream_ok) {
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s", recording->bundle_path,
                VKR_RECORDING_SUMMARY_FILE);
  VkrJsonFileWriter file;
  if (!vkr_json_file_writer_begin(
          &file, string8_create_from_cstr((const uint8_t *)path,
                                          string_length(path)))) {
    return false_v;
  }
  VkrJsonWriter *json = &file.writer;
  vkr_json_writer_begin_object(json);
  vkr_recording_json_text(json, "kind", "vkr.recording.summary");
  vkr_recording_json_u64(json, "version", 1u);
  vkr_recording_json_text(json, "name", recording->name);
  vkr_recording_json_text(json, "scene", recording->scene);
  vkr_recording_json_text(json, "author", recording->author);
  vkr_recording_json_u64(json, "started_unix",
                         (uint64_t)recording->started_unix);
  vkr_recording_json_text(json, "status",
                          stream_ok ? "complete" : "incomplete");
  vkr_recording_json_text(json, "tier",
                          recording->passes_enabled    ? "1"
                          : recording->metrics_enabled ? "0"
                                                       : "none");
  if (recording->passes_enabled) {
    vkr_recording_json_u64(json, "pass_gpu_period", recording->pass_gpu_period);
    vkr_recording_json_u64(json, "pass_rows", recording->pass_rows_written);
  }
  vkr_recording_json_u64(json, "frames", recording->frame);
  if (recording->poses_enabled) {
    vkr_recording_json_u64(json, "poses", recording->poses_written);
  }
  /* Drive needs both. */
  vkr_recording_json_u64(json, "player_ticks", recording->player_records);
  vkr_recording_json_u64(json, "actions", recording->action_records);
  vkr_recording_json_f64(json, "duration_seconds", recording->time);
  vkr_recording_json_u64(json, "dropped_records", stats->dropped_records);
  vkr_recording_json_u64(json, "lost_records", stats->lost_records);
  vkr_recording_json_u64(json, "stream_bytes", stats->stored_bytes);
  vkr_recording_json_f64(json, "writer_seconds", stats->writer_seconds);
  vkr_recording_write_markers(json, recording);
  if (recording->metrics_enabled && stream_ok) {
    vkr_recording_write_analysis(recording, json);
  }
  vkr_json_writer_end_object(json);
  return vkr_json_file_writer_commit(&file);
}

bool8_t vkr_recording_stop(VkrRecording *recording,
                           VkrRecorderStats *out_stats) {
  vkr_assert(recording != NULL);
  if (!recording->active) {
    return false_v;
  }
  VkrRecorderStats stats;
  const bool8_t stream_ok = vkr_recorder_stop(recording->recorder, &stats);
  const bool8_t summary_ok =
      vkr_recording_write_summary(recording, &stats, stream_ok);
  if (out_stats) {
    *out_stats = stats;
  }
  vkr_dmemory_allocator_destroy(&recording->allocator);
  recording->recorder = NULL;
  recording->active = false_v;
  return stream_ok && summary_ok;
}
