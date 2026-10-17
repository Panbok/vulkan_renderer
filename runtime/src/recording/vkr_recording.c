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

static const VkrRecordingField VKR_RECORDING_SESSION_FIELDS[] = {
    {.name = "seed", .type = VKR_RECORDING_FIELD_U64, .count = 1u},
};

static const VkrRecordingField VKR_RECORDING_SIM_FRAME_FIELDS[] = {
    {.name = "now", .unit = "s", .type = VKR_RECORDING_FIELD_F64, .count = 1u},
    {.name = "delta",
     .unit = "s",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 8u},
};

static const VkrRecordingField VKR_RECORDING_JOIN_FIELDS[] = {
    {.name = "ticks", .type = VKR_RECORDING_FIELD_U64, .count = 1u},
    {.name = "entity",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 8u},
    {.name = "kind",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 16u},
    {.name = "failed",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 20u},
};

static const VkrRecordingField VKR_RECORDING_TICK_HASH_FIELDS[] = {
    {.name = "tick", .type = VKR_RECORDING_FIELD_U64, .count = 1u},
    {.name = "transforms",
     .type = VKR_RECORDING_FIELD_U32,
     .count = VKR_RECORDING_HASH_BUCKETS,
     .offset = 8u},
    {.name = "bodies",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 8u + 4u * VKR_RECORDING_HASH_BUCKETS},
    {.name = "module_count",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 12u + 4u * VKR_RECORDING_HASH_BUCKETS},
    {.name = "modules",
     .type = VKR_RECORDING_FIELD_U32,
     .count = VKR_RECORDING_HASH_MODULE_MAX,
     .offset = 16u + 4u * VKR_RECORDING_HASH_BUCKETS},
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
    {.id = VKR_RECORDING_STREAM_SESSION,
     .name = "session",
     .record_size = sizeof(VkrRecordingSession),
     .field_count = ArrayCount(VKR_RECORDING_SESSION_FIELDS),
     .fields = VKR_RECORDING_SESSION_FIELDS},
    {.id = VKR_RECORDING_STREAM_SIM_FRAME,
     .name = "sim_frame",
     .record_size = sizeof(VkrRecordingSimFrame),
     .field_count = ArrayCount(VKR_RECORDING_SIM_FRAME_FIELDS),
     .fields = VKR_RECORDING_SIM_FRAME_FIELDS},
    {.id = VKR_RECORDING_STREAM_TICK_HASH,
     .name = "tick_hash",
     .record_size = sizeof(VkrRecordingTickHash),
     .field_count = ArrayCount(VKR_RECORDING_TICK_HASH_FIELDS),
     .fields = VKR_RECORDING_TICK_HASH_FIELDS},
    /* A u32 index, then the UTF-8 module name. */
    {.id = VKR_RECORDING_STREAM_HASH_MODULE, .name = "hash_module"},
    {.id = VKR_RECORDING_STREAM_JOIN,
     .name = "join",
     .record_size = sizeof(VkrRecordingJoin),
     .field_count = ArrayCount(VKR_RECORDING_JOIN_FIELDS),
     .fields = VKR_RECORDING_JOIN_FIELDS},
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

/* Frame-loop work that a render thread's completion iteration (ADR-082)
   measures before it completes the frame it publishes: it belongs to that
   iteration's recorded frame, whose frame.wall it lengthens. */
static const char *const VKR_RECORDING_LOOP_METRICS[] = {
    "cpu.update",
    "cpu.simulation",
    "cpu.physics",
    "cpu.render_wait",
};

/* Frame-loop work measured before the completion only when the iteration
   built its frame first (a decoupled frame). */
static const char *const VKR_RECORDING_LOOP_BUILD_METRICS[] = {
    "cpu.world_payload_build",
};

/* Renderer metrics and the engine events they become: a duration's
   samples in the frame, with their time, or a gauge's nonzero value. */
typedef struct VkrRecordingEventMetric {
  const char *metric;
  const char *event;
  VkrMetricKind kind;
} VkrRecordingEventMetric;

static const VkrRecordingEventMetric VKR_RECORDING_EVENT_METRICS[] = {
    {"pipeline.create", VKR_RECORDING_EVENT_PIPELINE, VKR_METRIC_KIND_DURATION},
    {"shader.load", VKR_RECORDING_EVENT_SHADER, VKR_METRIC_KIND_DURATION},
    {"pipelines.late", VKR_RECORDING_EVENT_PIPELINE_LATE,
     VKR_METRIC_KIND_GAUGE},
    {"asset.texture_load", VKR_RECORDING_EVENT_TEXTURE,
     VKR_METRIC_KIND_DURATION},
    {"asset.mesh_load", VKR_RECORDING_EVENT_MESH, VKR_METRIC_KIND_DURATION},
    {"asset.material_load", VKR_RECORDING_EVENT_MATERIAL,
     VKR_METRIC_KIND_DURATION},
    {"asset.scene_load", VKR_RECORDING_EVENT_SCENE, VKR_METRIC_KIND_DURATION},
};

_Static_assert(ArrayCount(VKR_RECORDING_EVENT_METRICS) <=
                   VKR_RECORDING_EVENT_METRIC_MAX,
               "room for every event metric");

/* Sampling period of the process's resident memory, in seconds. */
#define VKR_RECORDING_RESIDENT_PERIOD 0.1
/* Open-addressed spawn slots: twice the spawns, a power of two. */
#define VKR_RECORDING_SPAWN_SLOTS (2u * VKR_RECORDING_SPAWN_MAX)
/* Open-addressed pass name slots: twice the names, a power of two. */
#define VKR_RECORDING_PASS_SLOTS (2u * VKR_RECORDING_PASS_MAX)

_Static_assert(sizeof(VkrRecordingFrame) == 24u, "frame record layout");
_Static_assert(sizeof(VkrRecordingPose) == 88u, "pose record layout");
_Static_assert(sizeof(VkrRecordingPlayer) == 64u, "player record layout");
_Static_assert(sizeof(VkrRecordingAction) == 32u, "action record layout");
_Static_assert(sizeof(VkrRecordingCamera) == 48u, "camera record layout");
_Static_assert(sizeof(VkrRecordingJoin) == 24u, "join record layout");
_Static_assert(sizeof(VkrRecordingTickHash) ==
                   16u + 4u * (VKR_RECORDING_HASH_BUCKETS +
                               VKR_RECORDING_HASH_MODULE_MAX),
               "tick hash record layout");

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
      if (string_equals(catalog[i].name, "gpu.submission")) {
        recording->gpu_submission_metric = index;
      }
      for (uint32_t l = 0u; l < ArrayCount(VKR_RECORDING_LOOP_METRICS); ++l) {
        if (string_equals(catalog[i].name, VKR_RECORDING_LOOP_METRICS[l])) {
          recording->loop_metrics |= 1ull << index;
        }
      }
      for (uint32_t l = 0u; l < ArrayCount(VKR_RECORDING_LOOP_BUILD_METRICS);
           ++l) {
        if (string_equals(catalog[i].name,
                          VKR_RECORDING_LOOP_BUILD_METRICS[l])) {
          recording->loop_build_metrics |= 1ull << index;
        }
      }
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

/* Finds the renderer event metrics in the registry. */
vkr_internal void vkr_recording_event_metrics(VkrRecording *recording,
                                              const VkrMetrics *metrics) {
  uint32_t catalog_count = 0u;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(metrics, &catalog_count);
  recording->event_metric_count = 0u;
  for (uint32_t e = 0u; e < ArrayCount(VKR_RECORDING_EVENT_METRICS); ++e) {
    for (uint32_t i = 0u; i < catalog_count; ++i) {
      if (catalog[i].kind != VKR_RECORDING_EVENT_METRICS[e].kind ||
          !string_equals(catalog[i].name,
                         VKR_RECORDING_EVENT_METRICS[e].metric)) {
        continue;
      }
      const uint32_t index = recording->event_metric_count++;
      recording->event_metric_ids[index] =
          ((VkrMetricId)metrics->registry_generation << 16u) | i;
      recording->event_metric_names[index] = (uint8_t)e;
      break;
    }
  }
}

/* The pass name table, from the recording's memory. */
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
  recording->passes_enabled = true_v;
  return true_v;
}

/* The spawn table, from the recording's memory. */
vkr_internal bool8_t vkr_recording_spawns_create(VkrRecording *recording) {
  const uint64_t spawns = VKR_RECORDING_SPAWN_MAX;
  recording->spawn_entities =
      vkr_allocator_alloc(&recording->allocator, sizeof(uint64_t) * spawns,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  recording->spawn_containers = vkr_allocator_alloc(
      &recording->allocator, spawns, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  recording->spawn_flags = vkr_allocator_alloc(&recording->allocator, spawns,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  recording->spawn_slots = vkr_allocator_alloc(
      &recording->allocator,
      sizeof(uint32_t) * (uint64_t)VKR_RECORDING_SPAWN_SLOTS,
      VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!recording->spawn_entities || !recording->spawn_containers ||
      !recording->spawn_flags || !recording->spawn_slots) {
    return false_v;
  }
  MemZero(recording->spawn_slots,
          sizeof(uint32_t) * (uint64_t)VKR_RECORDING_SPAWN_SLOTS);
  return true_v;
}

bool8_t vkr_recording_start(VkrRecording *recording,
                            const VkrRecordingConfig *config) {
  vkr_assert(recording != NULL && config != NULL);
  vkr_assert(!recording->active && "stop the active recording first");
  MemZero(recording, sizeof(*recording));
  recording->gpu_submission_metric = UINT32_MAX;
  for (uint32_t i = 0u; i < VKR_RECORDING_FRAME_INDEX_HISTORY; ++i) {
    recording->frame_history[i] = (VkrRecordingFrameIndex){
        .frame_index = UINT64_MAX, .frame = UINT64_MAX};
  }
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
  VkrRecordingStream streams[28];
  _Static_assert(ArrayCount(VKR_RECORDING_STREAMS) + 6u <= 28u,
                 "room for the metrics, pose and pass streams");
  VkrRecordingField metric_fields[VKR_RECORDING_METRIC_MAX + 1u];
  MemCopy(streams, base, sizeof(*base) * base_count);
  uint32_t stream_count = base_count;
  if (config->metrics) {
    recording->metrics_enabled = true_v;
    vkr_recording_event_metrics(recording, config->metrics);
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
    if (!recording->pose_batch || !vkr_recording_spawns_create(recording)) {
      goto cleanup;
    }
    recording->poses_enabled = true_v;
    /* Variable records of VkrRecordingPose entries. */
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_POSE, .name = "pose"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_SPAWN, .name = "spawn"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_DESPAWN, .name = "despawn"};
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

/* A u16 length and `head`, then `tail`: the layout of tags and events. */
vkr_internal void vkr_recording_text_record(VkrRecording *recording,
                                            uint16_t stream, uint64_t frame,
                                            String8 head, const void *tail,
                                            uint32_t tail_size) {
  const uint16_t length =
      (uint16_t)Min(head.length, (uint64_t)VKR_RECORDING_TEXT_MAX);
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           stream, frame, 2u + (uint32_t)length + tail_size);
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

vkr_internal void vkr_recording_engine_event_at(VkrRecording *recording,
                                                uint64_t frame,
                                                const char *name,
                                                uint32_t count, float32_t ms) {
  const VkrRecordingEngineEvent event = {.count = count, .ms = ms};
  vkr_recording_text_record(
      recording, VKR_RECORDING_STREAM_EVENT, frame,
      string8_create_from_cstr((const uint8_t *)name, string_length(name)),
      &event, sizeof(event));
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

/* A metrics record on `frame` with the published fields in `fields`, and
   resident memory when `resident` is set. */
vkr_internal void vkr_recording_metrics_record(VkrRecording *recording,
                                               uint64_t frame,
                                               const VkrMetricsFrame *published,
                                               uint64_t fields,
                                               bool8_t resident) {
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
        i < count
            ? published && ((fields >> i) & 1u) != 0u &&
                  vkr_recording_metric_value(recording, i, published, &value)
            : resident && recording->resident_valid;
    if (i == count && present) {
      value = (float64_t)recording->resident_bytes;
    }
    valid |= present ? 1ull << i : 0ull;
    MemCopy(record + 8u + 8u * i, &value, sizeof(value));
  }
  MemCopy(record, &valid, sizeof(valid));
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
}

void vkr_recording_metrics(VkrRecording *recording, uint64_t frame,
                           const VkrMetricsFrame *published,
                           const VkrRecordingMetricsLoop *loop) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->metrics_enabled) {
    return;
  }
  /* The renderer's events of this publication land on its frame. */
  for (uint32_t i = 0u; published && i < recording->event_metric_count; ++i) {
    const VkrRecordingEventMetric *metric =
        &VKR_RECORDING_EVENT_METRICS[recording->event_metric_names[i]];
    const VkrMetricId id = recording->event_metric_ids[i];
    VkrMetricDurationSample sample;
    uint64_t value = 0u;
    if (metric->kind == VKR_METRIC_KIND_DURATION &&
        vkr_metrics_frame_read_duration(published, id, &sample) &&
        sample.count > 0u) {
      vkr_recording_engine_event_at(
          recording, frame, metric->event,
          (uint32_t)Min(sample.count, UINT32_MAX),
          (float32_t)((float64_t)sample.sum_ns * 1.0e-6));
    } else if (metric->kind == VKR_METRIC_KIND_GAUGE &&
               vkr_metrics_frame_read_u64(published, id, &value) &&
               value > 0u) {
      vkr_recording_engine_event_at(recording, frame, metric->event,
                                    (uint32_t)Min(value, UINT32_MAX), 0.0f);
    }
  }
  /* Resident memory is a system call, so it is sampled, then carried. */
  const float64_t now = vkr_platform_get_absolute_time();
  if (now - recording->resident_sampled_at >= VKR_RECORDING_RESIDENT_PERIOD) {
    recording->resident_sampled_at = now;
    recording->resident_valid =
        vkr_platform_get_process_resident_memory(&recording->resident_bytes);
  }
  /* gpu.submission measures an earlier frame; it is written there. */
  uint64_t fields = UINT64_MAX;
  if (recording->gpu_submission_metric != UINT32_MAX) {
    fields &= ~(1ull << recording->gpu_submission_metric);
  }
  uint64_t loop_fields = 0u;
  if (loop && loop->frame != frame) {
    loop_fields = (recording->loop_metrics |
                   (loop->built ? recording->loop_build_metrics : 0u)) &
                  fields;
    fields &= ~loop_fields;
  }
  vkr_recording_metrics_record(recording, frame, published, fields, true_v);
  /* A record whose mask holds the loop fields alone; readers merge it into
     the frame's record by the mask. Before the first recorded frame they
     belong to no frame. */
  if (loop_fields != 0u && published && loop->frame != UINT64_MAX) {
    vkr_recording_metrics_record(recording, loop->frame, published, loop_fields,
                                 false_v);
  }
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
  recording->frame_history[recording->frame_history_next] =
      (VkrRecordingFrameIndex){.frame_index = frame_index, .frame = frame};
  recording->frame_history_next =
      (recording->frame_history_next + 1u) % VKR_RECORDING_FRAME_INDEX_HISTORY;
  if (!recording->passes_enabled || !passes || passes->count == 0u) {
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

void vkr_recording_gpu_submission(VkrRecording *recording,
                                  uint64_t source_frame_index,
                                  float64_t gpu_ms) {
  vkr_assert(recording != NULL && recording->active);
  const uint32_t field = recording->gpu_submission_metric;
  if (!recording->metrics_enabled || field == UINT32_MAX ||
      source_frame_index == UINT64_MAX ||
      source_frame_index < recording->gpu_frame_written ||
      !vkr_is_finite_f64(gpu_ms)) {
    return;
  }
  const uint64_t target = vkr_recording_frame_of(recording, source_frame_index);
  if (target == UINT64_MAX) {
    return;
  }
  recording->gpu_frame_written = source_frame_index + 1u;
  /* A metrics record whose mask holds this field alone; readers merge it
     into the frame's record by the mask. */
  const uint32_t size = 8u * (recording->metric_count + 2u);
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_METRICS, target, size);
  if (!record) {
    return;
  }
  MemZero(record, size);
  const uint64_t valid = 1ull << field;
  MemCopy(record, &valid, sizeof(valid));
  MemCopy(record + 8u + 8u * field, &gpu_ms, sizeof(gpu_ms));
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
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

/* A spawn record: the id, the container, then the model's paths. */
vkr_internal void
vkr_recording_spawn_record(VkrRecording *recording, uint32_t id, String8 mesh,
                           String8 animation,
                           const VkrRecordingSpawnShape *shape) {
  const uint16_t mesh_length =
      (uint16_t)Min(mesh.length, (uint64_t)VKR_RECORDING_PATH_MAX - 1u);
  const uint16_t animation_length =
      (uint16_t)Min(animation.length, (uint64_t)VKR_RECORDING_PATH_MAX - 1u);
  const uint64_t frame = recording->frame > 0u ? recording->frame - 1u : 0u;
  uint8_t *record = vkr_recorder_reserve(
      recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
      VKR_RECORDING_STREAM_SPAWN, frame,
      9u + (uint32_t)mesh_length + (uint32_t)animation_length +
          (shape ? (uint32_t)sizeof(*shape) : 0u));
  if (!record) {
    return;
  }
  MemCopy(record, &id, sizeof(id));
  record[4] = recording->spawn_containers[id];
  MemCopy(record + 5u, &mesh_length, sizeof(mesh_length));
  if (mesh_length > 0u) {
    MemCopy(record + 7u, mesh.str, mesh_length);
  }
  MemCopy(record + 7u + mesh_length, &animation_length,
          sizeof(animation_length));
  if (animation_length > 0u) {
    MemCopy(record + 9u + mesh_length, animation.str, animation_length);
  }
  if (shape) {
    MemCopy(record + 9u + mesh_length + animation_length, shape,
            sizeof(*shape));
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
}

uint32_t vkr_recording_spawn_id(VkrRecording *recording, uint64_t entity,
                                uint8_t container) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->spawn_slots) {
    return UINT32_MAX;
  }
  const uint32_t mask = VKR_RECORDING_SPAWN_SLOTS - 1u;
  /* The entity id holds its container's world and a generation, so no two
     spawns of one session share it. */
  uint32_t probe = (uint32_t)vkr_hash_table_finalize(entity) & mask;
  while (recording->spawn_slots[probe] != 0u) {
    const uint32_t id = recording->spawn_slots[probe] - 1u;
    if (recording->spawn_entities[id] == entity) {
      return id;
    }
    probe = (probe + 1u) & mask;
  }
  if (recording->spawn_count >= VKR_RECORDING_SPAWN_MAX) {
    return UINT32_MAX;
  }
  const uint32_t id = recording->spawn_count++;
  recording->spawn_entities[id] = entity;
  recording->spawn_containers[id] = container;
  recording->spawn_flags[id] = VKR_RECORDING_SPAWN_ALIVE;
  recording->spawn_slots[probe] = id + 1u;
  vkr_recording_spawn_record(recording, id, (String8){0}, (String8){0}, NULL);
  return id;
}

void vkr_recording_spawn_look(VkrRecording *recording, uint32_t id,
                              String8 mesh, String8 animation,
                              const VkrRecordingSpawnShape *shape) {
  vkr_assert(recording != NULL && recording->active &&
             id < recording->spawn_count);
  if (recording->spawn_flags[id] & VKR_RECORDING_SPAWN_LOOK) {
    return;
  }
  recording->spawn_flags[id] |= VKR_RECORDING_SPAWN_LOOK;
  vkr_recording_spawn_record(recording, id, mesh, animation, shape);
}

void vkr_recording_despawn(VkrRecording *recording, uint32_t id) {
  vkr_assert(recording != NULL && recording->active &&
             id < recording->spawn_count);
  recording->spawn_flags[id] &= (uint8_t)~VKR_RECORDING_SPAWN_ALIVE;
  const uint64_t frame = recording->frame > 0u ? recording->frame - 1u : 0u;
  (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_DESPAWN, frame, &id,
                           sizeof(id));
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

void vkr_recording_tag(VkrRecording *recording, String8 key, String8 value) {
  vkr_assert(recording != NULL && recording->active);
  vkr_recording_text_record(
      recording, VKR_RECORDING_STREAM_TAG, recording->frame, key, value.str,
      (uint32_t)Min(value.length, (uint64_t)VKR_RECORDING_TEXT_MAX));
}

void vkr_recording_event(VkrRecording *recording, String8 name,
                         const void *payload, uint32_t size) {
  vkr_assert(recording != NULL && recording->active);
  vkr_recording_text_record(
      recording, VKR_RECORDING_STREAM_EVENT, recording->frame, name, payload,
      payload ? Min(size, VKR_RECORDING_EVENT_PAYLOAD_MAX) : 0u);
}

void vkr_recording_engine_event(VkrRecording *recording, const char *name,
                                uint32_t count, float32_t ms) {
  vkr_assert(recording != NULL && recording->active);
  vkr_recording_engine_event_at(recording, recording->frame, name, count, ms);
}

void vkr_recording_session(VkrRecording *recording, uint64_t seed) {
  vkr_assert(recording != NULL && recording->active);
  const VkrRecordingSession session = {.seed = seed};
  if (vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                         VKR_RECORDING_STREAM_SESSION, recording->frame,
                         &session, sizeof(session))) {
    recording->session_written = true_v;
    recording->hash_modules_named = 0u;
  }
}

void vkr_recording_sim_frame(VkrRecording *recording,
                             const VkrRecordingSimFrame *frame) {
  vkr_assert(recording != NULL && recording->active);
  if (recording->session_written) {
    (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                             VKR_RECORDING_STREAM_SIM_FRAME, recording->frame,
                             frame, sizeof(*frame));
  }
}

void vkr_recording_join(VkrRecording *recording, const VkrRecordingJoin *join) {
  vkr_assert(recording != NULL && recording->active);
  if (recording->session_written) {
    (void)vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                             VKR_RECORDING_STREAM_JOIN, recording->frame, join,
                             sizeof(*join));
  }
}

void vkr_recording_tick_hash(VkrRecording *recording,
                             const VkrRecordingTickHash *hash,
                             const char *const *module_names) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->session_written) {
    return;
  }
  for (uint32_t i = recording->hash_modules_named; i < hash->module_count;
       ++i) {
    const uint32_t length = (uint32_t)string_length_bounded(
        module_names[i], VKR_RECORDING_HASH_MODULE_NAME_MAX);
    uint8_t *record = vkr_recorder_reserve(
        recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
        VKR_RECORDING_STREAM_HASH_MODULE, recording->frame, 4u + length);
    if (!record) {
      return;
    }
    MemCopy(record, &i, sizeof(i));
    MemCopy(record + 4u, module_names[i], length);
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
    recording->hash_modules_named = i + 1u;
  }
  if (vkr_recorder_write(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                         VKR_RECORDING_STREAM_TICK_HASH, recording->frame, hash,
                         sizeof(*hash))) {
    ++recording->tick_hashes_written;
  }
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
    vkr_recording_json_u64(json, "spawns", recording->spawn_count);
  }
  /* Drive needs both. */
  vkr_recording_json_u64(json, "player_ticks", recording->player_records);
  vkr_recording_json_u64(json, "actions", recording->action_records);
  /* Re-simulate needs them too. */
  vkr_recording_json_u64(json, "tick_hashes", recording->tick_hashes_written);
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
