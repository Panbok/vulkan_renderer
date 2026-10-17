#include "recording/vkr_recording.h"

#include "containers/str.h"
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
};

_Static_assert(sizeof(VkrRecordingFrame) == 24u, "frame record layout");
_Static_assert(sizeof(VkrRecordingCamera) == 48u, "camera record layout");

const VkrRecordingStream *vkr_recording_streams(uint32_t *out_count) {
  *out_count = ArrayCount(VKR_RECORDING_STREAMS);
  return VKR_RECORDING_STREAMS;
}

const char *vkr_recording_marker_kind_name(uint32_t kind) {
  switch (kind) {
  case VKR_RECORDING_MARKER_NOTE:
    return "note";
  case VKR_RECORDING_MARKER_BUG:
    return "bug";
  case VKR_RECORDING_MARKER_STEP:
    return "step";
  case VKR_RECORDING_MARKER_EDIT:
    return "edit";
  }
  return "unknown";
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
  uint32_t stream_count = 0u;
  const VkrRecordingStream *streams = vkr_recording_streams(&stream_count);
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

/* summary.json: the small file an agent or the Library reads first. Later
   phases add statistics and spikes; readers ignore names they do not know. */
vkr_internal bool8_t vkr_recording_write_summary(const VkrRecording *recording,
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
  vkr_recording_json_text(json, "tier", "none");
  vkr_recording_json_u64(json, "frames", recording->frame);
  vkr_recording_json_f64(json, "duration_seconds", recording->time);
  vkr_recording_json_u64(json, "dropped_records", stats->dropped_records);
  vkr_recording_json_u64(json, "lost_records", stats->lost_records);
  vkr_recording_json_u64(json, "stream_bytes", stats->stored_bytes);
  vkr_recording_json_f64(json, "writer_seconds", stats->writer_seconds);
  vkr_recording_write_markers(json, recording);
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
