#include "recording/vkr_recording.h"

#include "core/vkr_zone.h"

#include "assets/vkr_recording_analysis.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "vkr_frame_input.h"
#include "vkr_gpu_abi.h"
#include "vkr_pipeline_registry.h"
#include "vkr_rg_debug.h"

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
    {.name = "source",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 3u,
     .offset = 16u},
    {.name = "has_source",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 40u},
    {.name = "reserved",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 44u},
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

static const VkrRecordingField VKR_RECORDING_ZONE_FIELDS[] = {
    {.name = "zone", .type = VKR_RECORDING_FIELD_U16, .count = 1u},
    {.name = "thread",
     .type = VKR_RECORDING_FIELD_U16,
     .count = 1u,
     .offset = 2u},
    {.name = "depth",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 4u},
    {.name = "begin_ns",
     .unit = "ns",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 8u},
    {.name = "end_ns",
     .unit = "ns",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 16u},
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
    /* A u32 count, then {i32 x, i32 z} per cell. */
    {.id = VKR_RECORDING_STREAM_CELLS, .name = "cells"},
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
    "cpu.packet_candidate_pack",
    "cpu.backend_frame_wait",
    "packet.static_repack_reasons",
    "packet.publication_changes",
    "packet.publication_pending",
    "gpu.submission",
    "frame.render_scale",
    "frame.present_interval",
    "draw.world.calls_issued",
    "draw.world.commands_issued",
    "visibility.objects_tested",
    "upload.fence_waits",
    "memory.gpu.driver.current_allocated_bytes",
    "memory.gpu.driver.recommended_budget_bytes",
    /* Present cost, faults and the platform signals that explain a frame
       (ADR-124, docs/proposals/profiling-and-diagnostics.md). */
    "cpu.backend_present",
    "cpu.drawable_acquire",
    "gpu.present.latency",
    "gpu.present.missed",
    "cpu.process_time",
    "gpu.errors",
    "log.errors",
    "os.memory.footprint_bytes",
    "os.memory.pressure",
    "os.thermal.level",
    "os.power.low_power",
    "os.page_faults.major",
    "memory.gpu.os.budget_bytes",
    "memory.gpu.os.usage_bytes",
    "os.gpu.other_process_util",
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
_Static_assert(sizeof(VkrRecordingSimFrame) == 48u, "sim frame record layout");
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
/* Creates `bundle_path` and its missing parents; the bundle itself must not
   exist yet. */
vkr_internal bool8_t vkr_recording_create_directory(const char *bundle_path) {
  char prefix[VKR_RECORDING_PATH_MAX];
  const uint64_t length = string_length(bundle_path);
  if (length >= sizeof(prefix)) {
    return false_v;
  }
  MemCopy(prefix, bundle_path, length + 1u);
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
  const FilePath path = file_path_view(bundle_path);
  return file_create_directory_exclusive(&path) == FILE_ERROR_NONE;
}

vkr_internal bool8_t vkr_recording_create_bundle(VkrRecording *recording) {
  return vkr_recording_create_directory(recording->bundle_path);
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
  /* Covered ids, then pending ids. */
  recording->graph_covered =
      vkr_allocator_alloc(&recording->allocator, 2u * VKR_RECORDING_PASS_MAX,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!recording->pass_names || !recording->pass_slots ||
      !recording->graph_covered) {
    return false_v;
  }
  recording->graph_pending = recording->graph_covered + VKR_RECORDING_PASS_MAX;
  MemZero(recording->pass_names, name_bytes);
  MemZero(recording->pass_slots, slot_bytes);
  MemZero(recording->graph_covered, 2u * VKR_RECORDING_PASS_MAX);
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

vkr_internal void vkr_recording_measurements_start(VkrRecording *recording,
                                                   const VkrMetrics *metrics,
                                                   bool8_t tier1);
vkr_internal uint32_t
vkr_recording_allocator_id(VkrRecording *recording, uint64_t frame,
                           const VkrAllocatorRegistrySample *sample);

/* The zone sink: files a finished zone in the ring of the thread that ran
   it, under the recorded frame current now. Each slot is one thread, so
   each ring keeps its single producer. */
vkr_internal void vkr_recording_zone_write(void *context,
                                           const VkrZoneRecord *record) {
  VkrRecording *recording = (VkrRecording *)context;
  VkrRecorderProducer producer = VKR_RECORDER_PRODUCER_COUNT;
  if (record->thread == VKR_ZONE_THREAD_MAIN) {
    producer = VKR_RECORDER_PRODUCER_MAIN;
  } else if (record->thread == VKR_ZONE_THREAD_RENDER) {
    producer = VKR_RECORDER_PRODUCER_RENDER;
  } else if (record->thread >= VKR_ZONE_THREAD_WORKER_FIRST &&
             record->thread - VKR_ZONE_THREAD_WORKER_FIRST <
                 VKR_RECORDER_WORKER_PRODUCERS) {
    producer =
        (VkrRecorderProducer)(VKR_RECORDER_PRODUCER_WORKER_FIRST +
                              record->thread - VKR_ZONE_THREAD_WORKER_FIRST);
  }
  if (producer == VKR_RECORDER_PRODUCER_COUNT) {
    return;
  }
  const uint64_t frame = vkr_atomic_uint64_load(
      producer == VKR_RECORDER_PRODUCER_RENDER ? &recording->zone_render_frame
                                               : &recording->zone_frame,
      VKR_MEMORY_ORDER_RELAXED);
  (void)vkr_recorder_write(recording->recorder, producer,
                           VKR_RECORDING_STREAM_ZONE, frame, record,
                           sizeof(*record));
}

/* Names the zones registered since the last call (frame-loop thread). */
vkr_internal void vkr_recording_zone_names(VkrRecording *recording,
                                           uint64_t frame) {
  const uint32_t count = vkr_zone_site_count();
  while (recording->zone_names_written < count) {
    const uint32_t zone = recording->zone_names_written;
    const char *name = vkr_zone_site_name(zone);
    if (!name) {
      /* Registered but not published yet; the next frame names it. */
      break;
    }
    const uint32_t length =
        (uint32_t)Min(string_length(name), (uint64_t)VKR_RECORDING_TEXT_MAX);
    uint8_t *record = vkr_recorder_reserve(
        recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
        VKR_RECORDING_STREAM_ZONE_NAME, frame, 4u + length);
    if (!record) {
      break;
    }
    MemCopy(record, &zone, sizeof(zone));
    MemCopy(record + 4u, name, length);
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
    ++recording->zone_names_written;
  }
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
  VkrRecordingStream streams[32];
  _Static_assert(ArrayCount(VKR_RECORDING_STREAMS) + 17u <= 32u,
                 "room for the metrics, pose, pass, graph, allocator, draw "
                 "and zone streams");
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
  /* A rolling window keeps no start-time records, so it names no
     measurements. */
  const bool8_t measured =
      config->metrics && (config->all_measurements ||
                          (config->passes && config->rolling_seconds <= 0.0));
  if (measured) {
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_MEASUREMENT_NAME,
                             .name = "measurement_name"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_MEASUREMENTS,
                             .flags = VKR_RECORDING_STREAM_DROPPABLE,
                             .name = "measurements"};
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
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASS_STARTS,
                             .flags = VKR_RECORDING_STREAM_DROPPABLE,
                             .name = "pass_starts"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_GRAPH, .name = "graph"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_ALLOCATOR_NAME, .name = "allocator_name"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_ALLOCATORS,
                             .flags = VKR_RECORDING_STREAM_DROPPABLE,
                             .name = "allocators"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_DRAW_BOUNDS, .name = "draw_bounds"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_DRAW_IDS, .name = "draw_ids"};
    streams[stream_count++] =
        (VkrRecordingStream){.id = VKR_RECORDING_STREAM_DRAW_STATES,
                             .flags = VKR_RECORDING_STREAM_DROPPABLE,
                             .name = "draw_states"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_HARDWARE, .name = "hardware"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_PIPELINE, .name = "pipeline"};
  }
  if (config->zones) {
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_ZONE_NAME, .name = "zone_name"};
    streams[stream_count++] = (VkrRecordingStream){
        .id = VKR_RECORDING_STREAM_ZONE,
        .flags = VKR_RECORDING_STREAM_DROPPABLE,
        .name = "zone",
        .record_size = sizeof(VkrZoneRecord),
        .field_count = ArrayCount(VKR_RECORDING_ZONE_FIELDS),
        .fields = VKR_RECORDING_ZONE_FIELDS,
    };
  }
  const VkrRecorderConfig recorder_config = {
      .stream_path = stream_path,
      .codec = config->codec,
      .identity_json = config->identity_json,
      .streams = streams,
      .stream_count = stream_count,
      .ring_bytes = VKR_RECORDING_RING_BYTES,
      .worker_ring_bytes = config->zones ? VKR_RECORDING_WORKER_RING_BYTES : 0u,
      .rolling_seconds = config->rolling_seconds,
  };
  recording->rolling_seconds = Max(config->rolling_seconds, 0.0);
  /* A rolling recording creates its bundles at each save. */
  if (recording->rolling_seconds <= 0.0 &&
      !vkr_recording_create_bundle(recording)) {
    goto cleanup;
  }
  recording->recorder =
      vkr_recorder_start(&recording->allocator, &recorder_config);
  if (!recording->recorder) {
    goto cleanup;
  }
  recording->active = true_v;
  /* The allocators registered now count from here, so the first frame's
     rows hold only that frame's work. */
  if (recording->passes_enabled) {
    VkrAllocatorRegistrySample samples[VKR_ALLOCATOR_REGISTRY_CAPACITY];
    const uint32_t found =
        vkr_allocator_registry_sample(samples, ArrayCount(samples));
    for (uint32_t i = 0u; i < Min(found, (uint32_t)ArrayCount(samples)); ++i) {
      (void)vkr_recording_allocator_id(recording, 0u, &samples[i]);
    }
  }
  if (measured) {
    recording->all_measurements = config->all_measurements;
    vkr_recording_measurements_start(recording, config->metrics,
                                     !config->all_measurements);
  }
  if (config->zones) {
    recording->zones_enabled = true_v;
    const VkrZoneSink sink = {.write = vkr_recording_zone_write,
                              .context = recording};
    vkr_zone_sink_set(&sink);
  }
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
  if (recording->zones_enabled) {
    vkr_recording_zone_names(recording, recording->frame);
  }
  ++recording->frame;
}

void vkr_recording_loop_begin(VkrRecording *recording) {
  if (recording->active && recording->zones_enabled) {
    vkr_atomic_uint64_store(&recording->zone_frame, recording->frame,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

void vkr_recording_render_frame(VkrRecording *recording, uint64_t frame) {
  if (recording->active && recording->zones_enabled && frame != UINT64_MAX) {
    vkr_atomic_uint64_store(&recording->zone_render_frame, frame,
                            VKR_MEMORY_ORDER_RELAXED);
  }
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
vkr_internal bool8_t vkr_recording_sample_value(
    VkrMetricId id, VkrMetricKind kind, float64_t scale,
    const VkrMetricsFrame *published, float64_t *out_value) {
  if (kind == VKR_METRIC_KIND_DURATION) {
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

vkr_internal bool8_t vkr_recording_metric_value(
    const VkrRecording *recording, uint32_t index,
    const VkrMetricsFrame *published, float64_t *out_value) {
  return vkr_recording_sample_value(
      recording->metric_ids[index], recording->metric_kind[index],
      recording->metric_scale[index], published, out_value);
}

/* All measurements: every catalog metric the metrics stream does not
   carry, named once at the start. */
/* Name prefixes of the metrics a Tier 1 recording carries beyond Tier 0's:
   GPU memory by owner and the driver's view, draws and culling, pipeline
   and backend counters, render graph memory, uploads and streaming, and
   the GPU driver's own statistics (ADR-130). */
static const char *const VKR_RECORDING_TIER1_MEASUREMENTS[] = {
    "memory.gpu.",
    "os.gpu.",
    "draw.",
    "visibility.",
    "pipeline.",
    "backend.",
    "rg.",
    "upload.",
    "material.texture_stream.",
    "geometry.megabuffer.",
};

vkr_internal bool8_t vkr_recording_tier1_measurement(const char *name) {
  for (uint32_t i = 0u; i < ArrayCount(VKR_RECORDING_TIER1_MEASUREMENTS); ++i) {
    const char *prefix = VKR_RECORDING_TIER1_MEASUREMENTS[i];
    const uint64_t length = string_length(prefix);
    if (string_length_bounded(name, length) == length &&
        MemCompare(name, prefix, length) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* Names the measurements and keeps their ids: every catalog metric, or
   with `tier1` those of VKR_RECORDING_TIER1_MEASUREMENTS. */
vkr_internal void vkr_recording_measurements_start(VkrRecording *recording,
                                                   const VkrMetrics *metrics,
                                                   bool8_t tier1) {
  uint32_t catalog_count = 0u;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(metrics, &catalog_count);
  for (uint32_t i = 0u; i < catalog_count && recording->measurement_count <
                                                 VKR_RECORDING_MEASUREMENT_MAX;
       ++i) {
    if (tier1 && !vkr_recording_tier1_measurement(catalog[i].name)) {
      continue;
    }
    const VkrMetricId id =
        ((VkrMetricId)metrics->registry_generation << 16u) | i;
    bool8_t carried = false_v;
    for (uint32_t m = 0u; m < recording->metric_count && !carried; ++m) {
      carried = recording->metric_ids[m] == id;
    }
    if (carried) {
      continue;
    }
    const uint32_t index = recording->measurement_count++;
    recording->measurement_ids[index] = id;
    recording->measurement_kind[index] = catalog[i].kind;
    recording->measurement_scale[index] =
        catalog[i].unit == VKR_METRIC_UNIT_NANOSECONDS ? 1.0e-6 : 1.0;
    const char *unit = vkr_recording_unit(catalog[i].unit);
    const uint16_t name_length = (uint16_t)string_length_bounded(
        catalog[i].name, VKR_RECORDING_NAME_MAX);
    const uint16_t unit_length = (uint16_t)string_length(unit);
    uint8_t *record =
        vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                             VKR_RECORDING_STREAM_MEASUREMENT_NAME, 0u,
                             8u + (uint32_t)name_length + unit_length);
    if (!record) {
      continue;
    }
    MemCopy(record, &index, sizeof(index));
    MemCopy(record + 4u, &name_length, sizeof(name_length));
    MemCopy(record + 6u, &unit_length, sizeof(unit_length));
    MemCopy(record + 8u, catalog[i].name, name_length);
    MemCopy(record + 8u + name_length, unit, unit_length);
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  }
}

/* One frame's measurements, a record per page of
   VKR_RECORDING_MEASUREMENT_PAGE. */
vkr_internal void vkr_recording_measurements(VkrRecording *recording,
                                             uint64_t frame,
                                             const VkrMetricsFrame *published) {
  for (uint32_t first = 0u; first < recording->measurement_count;
       first += VKR_RECORDING_MEASUREMENT_PAGE) {
    const uint32_t count = Min(recording->measurement_count - first,
                               VKR_RECORDING_MEASUREMENT_PAGE);
    uint8_t *record = vkr_recorder_reserve(
        recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
        VKR_RECORDING_STREAM_MEASUREMENTS, frame, 16u + 8u * count);
    if (!record) {
      return;
    }
    uint64_t valid = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
      const uint32_t index = first + i;
      float64_t value = 0.0;
      if (vkr_recording_sample_value(recording->measurement_ids[index],
                                     recording->measurement_kind[index],
                                     recording->measurement_scale[index],
                                     published, &value)) {
        valid |= 1ull << i;
      }
      MemCopy(record + 16u + 8u * i, &value, sizeof(value));
    }
    MemCopy(record, &first, sizeof(first));
    MemCopy(record + 4u, &count, sizeof(count));
    MemCopy(record + 8u, &valid, sizeof(valid));
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  }
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
  /* The renderer's events of this publication land on its frame; a
     publication of a frame from before the recording keeps only the loop
     work, which belongs to a recorded frame. */
  const bool8_t recorded = frame != UINT64_MAX;
  for (uint32_t i = 0u;
       recorded && published && i < recording->event_metric_count; ++i) {
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
  if (recorded) {
    vkr_recording_metrics_record(recording, frame, published, fields, true_v);
    if (published && recording->measurement_count) {
      vkr_recording_measurements(recording, frame, published);
    }
  }
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

/* The GPU starts of the rows just written, in their order, placed on the
   CPU clock by the submission's host start (VKR_RECORDING_STREAM_PASS_STARTS).
 */
vkr_internal void
vkr_recording_pass_starts(VkrRecording *recording, uint64_t target,
                          const VkrRendererMetricsPassTable *passes,
                          const uint16_t *ids, uint32_t count, uint32_t rows) {
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_PASS_STARTS, target,
                           VKR_RECORDING_PASS_STARTS_HEADER_SIZE + 4u * rows);
  if (!record) {
    return;
  }
  const uint32_t flags = VKR_RECORDING_PASS_STARTS_HOST_CLOCK;
  MemCopy(record, &rows, sizeof(rows));
  MemCopy(record + 4u, &flags, sizeof(flags));
  MemCopy(record + 8u, &passes->gpu_start_host_ns, sizeof(uint64_t));
  MemCopy(record + 16u, &passes->gpu_end_host_ns, sizeof(uint64_t));
  uint8_t *row = record + VKR_RECORDING_PASS_STARTS_HEADER_SIZE;
  for (uint32_t i = 0u; i < count; ++i) {
    if (ids[i] == UINT16_MAX) {
      continue;
    }
    const VkrRendererMetricsPassSample *sample = &passes->samples[i];
    const float32_t start_ms = sample->gpu_valid && sample->gpu_start_valid
                                   ? (float32_t)sample->gpu_start_ms
                                   : (float32_t)VKR_FLOAT_NAN;
    MemCopy(row, &start_ms, sizeof(start_ms));
    row += 4u;
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
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
    if (!recording->graph_covered[ids[i]]) {
      recording->graph_pending[ids[i]] = 1u;
      recording->graph_wanted = true_v;
    }
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
  if (passes->gpu_host_valid) {
    vkr_recording_pass_starts(recording, target, passes, ids, count, rows);
  }
}

bool8_t vkr_recording_graph_wanted(VkrRecording *recording) {
  vkr_assert(recording != NULL && recording->active);
  const bool8_t wanted = recording->passes_enabled && recording->graph_wanted;
  recording->graph_wanted = false_v;
  return wanted;
}

_Static_assert(VKR_RG_PASS_TYPE_GRAPHICS == VKR_RECORDING_GRAPH_KIND_GRAPHICS &&
                   VKR_RG_PASS_TYPE_COMPUTE ==
                       VKR_RECORDING_GRAPH_KIND_COMPUTE &&
                   VKR_RG_PASS_TYPE_TRANSFER ==
                       VKR_RECORDING_GRAPH_KIND_TRANSFER,
               "recorded pass kinds are the graph's");
_Static_assert(VKR_RG_SNAPSHOT_PASS_CULLED == VKR_RECORDING_GRAPH_PASS_CULLED &&
                   VKR_RG_SNAPSHOT_PASS_DISABLED ==
                       VKR_RECORDING_GRAPH_PASS_DISABLED &&
                   VKR_RG_SNAPSHOT_PASS_USES_CUT ==
                       VKR_RECORDING_GRAPH_PASS_USES_CUT,
               "recorded pass flags are the snapshot's");
_Static_assert(
    VKR_RG_SNAPSHOT_ROLE_READ == VKR_RECORDING_GRAPH_ROLE_READ &&
        VKR_RG_SNAPSHOT_ROLE_WRITE == VKR_RECORDING_GRAPH_ROLE_WRITE &&
        VKR_RG_SNAPSHOT_ROLE_COLOR == VKR_RECORDING_GRAPH_ROLE_COLOR &&
        VKR_RG_SNAPSHOT_ROLE_DEPTH == VKR_RECORDING_GRAPH_ROLE_DEPTH &&
        VKR_RG_SNAPSHOT_ROLE_DEPTH_READ == VKR_RECORDING_GRAPH_ROLE_DEPTH_READ,
    "recorded use roles are the snapshot's");
_Static_assert(VKR_RG_SNAPSHOT_RESOURCE_BUFFER ==
                       VKR_RECORDING_GRAPH_RESOURCE_BUFFER &&
                   VKR_RG_SNAPSHOT_RESOURCE_IMPORTED ==
                       VKR_RECORDING_GRAPH_RESOURCE_IMPORTED &&
                   VKR_RG_SNAPSHOT_RESOURCE_EXPORTED ==
                       VKR_RECORDING_GRAPH_RESOURCE_EXPORTED &&
                   VKR_RG_SNAPSHOT_RESOURCE_PRESENT ==
                       VKR_RECORDING_GRAPH_RESOURCE_PRESENT,
               "recorded resource flags are the snapshot's");

vkr_internal uint8_t *vkr_recording_put_u16(uint8_t *at, uint32_t value) {
  const uint16_t v = (uint16_t)Min(value, (uint32_t)UINT16_MAX);
  MemCopy(at, &v, sizeof(v));
  return at + sizeof(v);
}

vkr_internal uint8_t *vkr_recording_put_u32(uint8_t *at, uint32_t value) {
  MemCopy(at, &value, sizeof(value));
  return at + sizeof(value);
}

vkr_internal uint8_t *vkr_recording_put_name(uint8_t *at,
                                             VkrRgSnapshotName name) {
  at = vkr_recording_put_u32(at, name.offset);
  return vkr_recording_put_u16(at, name.length);
}

void vkr_recording_graph(VkrRecording *recording, uint64_t frame,
                         const VkrRgSnapshot *snapshot) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled || !snapshot) {
    return;
  }
  const uint64_t rendered =
      vkr_recording_frame_of(recording, snapshot->frame_index);
  const uint64_t target = rendered != UINT64_MAX ? rendered : frame;
  const uint64_t size =
      VKR_RECORDING_GRAPH_HEADER_SIZE +
      (uint64_t)VKR_RECORDING_GRAPH_PASS_SIZE * snapshot->pass_count +
      (uint64_t)VKR_RECORDING_GRAPH_USE_SIZE * snapshot->use_count +
      (uint64_t)VKR_RECORDING_GRAPH_RESOURCE_SIZE * snapshot->resource_count +
      snapshot->name_bytes;
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_GRAPH, target, (uint32_t)size);
  /* A lost record leaves its passes uncovered, so the next frame that runs
     them asks again. */
  if (!record) {
    return;
  }
  uint8_t *at = record;
  at = vkr_recording_put_u32(at, VKR_RECORDING_GRAPH_VERSION);
  at = vkr_recording_put_u32(at, snapshot->pass_count);
  at = vkr_recording_put_u32(at, snapshot->use_count);
  at = vkr_recording_put_u32(at, snapshot->resource_count);
  at = vkr_recording_put_u32(at, snapshot->name_bytes);
  at = vkr_recording_put_u32(
      at, snapshot->truncated ? VKR_RECORDING_GRAPH_TRUNCATED : 0u);
  at = vkr_recording_put_u32(at, snapshot->viewport_width);
  at = vkr_recording_put_u32(at, snapshot->viewport_height);
  MemCopy(at, &snapshot->render_scale, sizeof(float32_t));
  at += sizeof(float32_t);

  for (uint32_t i = 0u; i < snapshot->pass_count; ++i) {
    const VkrRgSnapshotPass *pass = &snapshot->passes[i];
    at = vkr_recording_put_name(at, pass->name);
    *at++ = (uint8_t)pass->type;
    *at++ = (uint8_t)pass->flags;
    at = vkr_recording_put_u32(at, pass->order);
    at = vkr_recording_put_u32(at, pass->first_use);
    at = vkr_recording_put_u32(at, pass->use_count);
  }
  for (uint32_t i = 0u; i < snapshot->use_count; ++i) {
    const VkrRgSnapshotUse *use = &snapshot->uses[i];
    at = vkr_recording_put_name(at, use->resource);
    *at++ = (uint8_t)use->role;
    *at++ = (uint8_t)((use->buffer ? VKR_RECORDING_GRAPH_USE_BUFFER : 0u) |
                      (use->write ? VKR_RECORDING_GRAPH_USE_WRITE : 0u));
  }
  for (uint32_t i = 0u; i < snapshot->resource_count; ++i) {
    const VkrRgSnapshotResource *resource = &snapshot->resources[i];
    at = vkr_recording_put_name(at, resource->name);
    at = vkr_recording_put_u16(at, resource->flags);
    at = vkr_recording_put_u32(at, resource->format.offset);
    at = vkr_recording_put_u16(at, resource->format.length);
    at = vkr_recording_put_u16(at, resource->lifetime.length);
    at = vkr_recording_put_u32(at, resource->lifetime.offset);
    at = vkr_recording_put_u32(at, resource->width);
    at = vkr_recording_put_u32(at, resource->height);
    at = vkr_recording_put_u32(at, resource->depth);
    at = vkr_recording_put_u16(at, resource->layers);
    at = vkr_recording_put_u16(at, resource->mip_levels);
    at = vkr_recording_put_u16(at, resource->samples);
    at = vkr_recording_put_u16(at, 0u);
    MemCopy(at, &resource->bytes, sizeof(uint64_t));
    at += sizeof(uint64_t);
    at = vkr_recording_put_u32(at, resource->first_order);
    at = vkr_recording_put_u32(at, resource->last_order);
  }
  MemCopy(at, snapshot->names, snapshot->name_bytes);
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  ++recording->graphs_written;
  /* Its scheduled passes are covered, and so are the ones that asked for
     it; naming a new one writes its name record, so only after the graph's
     is committed. */
  for (uint32_t id = 0u; id < VKR_RECORDING_PASS_MAX; ++id) {
    recording->graph_covered[id] |= recording->graph_pending[id];
    recording->graph_pending[id] = 0u;
  }
  for (uint32_t i = 0u; i < snapshot->pass_count; ++i) {
    const VkrRgSnapshotPass *pass = &snapshot->passes[i];
    if (pass->order == UINT32_MAX || pass->name.length == 0u ||
        pass->name.length > VKR_RECORDING_PASS_NAME_MAX) {
      continue;
    }
    const uint16_t id = vkr_recording_pass_id(
        recording, target, snapshot->names + pass->name.offset,
        pass->name.length);
    if (id != UINT16_MAX) {
      recording->graph_covered[id] = 1u;
    }
  }
}

/* The recorded id of registry entry `sample`, naming a new one first;
   UINT32_MAX when the ids ran out or the name record was lost. */
vkr_internal uint32_t
vkr_recording_allocator_id(VkrRecording *recording, uint64_t frame,
                           const VkrAllocatorRegistrySample *sample) {
  for (uint32_t i = 0u; i < recording->allocator_count; ++i) {
    if (recording->allocator_slot[i] == sample->slot &&
        recording->allocator_generation[i] == sample->generation) {
      return i;
    }
  }
  if (recording->allocator_count == VKR_RECORDING_ALLOCATOR_MAX) {
    return UINT32_MAX;
  }
  const uint32_t length = (uint32_t)string_length_bounded(
      sample->name, VKR_RECORDING_ALLOCATOR_NAME_MAX);
  uint8_t *record = vkr_recorder_reserve(
      recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
      VKR_RECORDING_STREAM_ALLOCATOR_NAME, frame, 8u + length);
  if (!record) {
    return UINT32_MAX;
  }
  const uint32_t id = recording->allocator_count++;
  const uint16_t name_length = (uint16_t)length;
  MemCopy(record, &id, sizeof(id));
  record[4] = (uint8_t)sample->type;
  record[5] = sample->owner == VKR_ALLOCATOR_REGISTRY_OWNER_ARENA ? 1u : 0u;
  MemCopy(record + 6u, &name_length, sizeof(name_length));
  MemCopy(record + 8u, sample->name, length);
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  recording->allocator_slot[id] = sample->slot;
  recording->allocator_generation[id] = sample->generation;
  /* Counting starts here: an allocator older than the recording does not
     report its whole history on its first row. One registered while the
     recording runs loses the work before its first sample. */
  recording->allocator_counts[id][0] = sample->alloc_count;
  recording->allocator_counts[id][1] = sample->free_count;
  recording->allocator_counts[id][2] = sample->realloc_count;
  recording->allocator_counts[id][3] = sample->scope_count;
  return id;
}

void vkr_recording_hardware(VkrRecording *recording, uint64_t frame,
                            const VkrRecordingHardwareFact *facts,
                            uint32_t count) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled || recording->hardware_written) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const uint16_t key_length = (uint16_t)string_length_bounded(
        facts[i].key, VKR_RECORDING_HARDWARE_KEY_MAX);
    const uint16_t value_length = (uint16_t)string_length_bounded(
        facts[i].value, VKR_RECORDING_HARDWARE_VALUE_MAX);
    uint8_t *record =
        vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                             VKR_RECORDING_STREAM_HARDWARE, frame,
                             4u + (uint32_t)key_length + value_length);
    /* A full ring writes every fact again on a later frame; the reader
       keeps the first of each key. */
    if (!record) {
      return;
    }
    MemCopy(record, &key_length, sizeof(key_length));
    MemCopy(record + 2u, &value_length, sizeof(value_length));
    MemCopy(record + 4u, facts[i].key, key_length);
    MemCopy(record + 4u + key_length, facts[i].value, value_length);
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  }
  recording->hardware_written = true_v;
}

typedef struct VkrRecordingPipelineWrite {
  VkrRecording *recording;
  uint64_t frame;
  /* A record did not fit the ring: it and later ones wait for the next
     call. */
  bool8_t stopped;
} VkrRecordingPipelineWrite;

vkr_internal void vkr_recording_pipeline_visit(void *context, uint32_t index,
                                               const VkrPipelineInfo *info,
                                               const VkrPipelineStat *stats,
                                               uint32_t stat_count) {
  VkrRecordingPipelineWrite *write = context;
  VkrRecording *recording = write->recording;
  if (write->stopped || index != recording->pipelines_written ||
      index >= VKR_RECORDING_PIPELINE_MAX) {
    return;
  }
  _Static_assert(VKR_PIPELINE_STATS_PER_ENTRY <=
                     VKR_RECORDING_PIPELINE_STATS_PER_RECORD,
                 "a record holds every statistic of an entry");
  const uint16_t name_length = (uint16_t)string_length_bounded(
      info->name, VKR_RECORDING_PIPELINE_NAME_MAX);
  uint32_t size = VKR_RECORDING_PIPELINE_HEADER_SIZE + name_length;
  for (uint32_t i = 0u; i < stat_count; ++i) {
    size += 2u + 8u +
            (uint32_t)string_length_bounded(
                stats[i].name, VKR_RECORDING_PIPELINE_STAT_NAME_MAX);
  }
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_PIPELINE, write->frame, size);
  if (!record) {
    write->stopped = true_v;
    return;
  }
  uint8_t *at = vkr_recording_put_u32(record, index);
  at[0] = (uint8_t)info->kind;
  at[1] = 0u;
  MemCopy(at + 2u, &name_length, sizeof(name_length));
  at += 4u;
  MemCopy(at, &info->compile_ms, sizeof(info->compile_ms));
  at += sizeof(info->compile_ms);
  at = vkr_recording_put_u32(at, info->max_threads);
  at = vkr_recording_put_u32(at, info->execution_width);
  at = vkr_recording_put_u32(at, info->threadgroup_memory);
  at = vkr_recording_put_u32(at, info->registers);
  at = vkr_recording_put_u32(at, info->spill_bytes);
  at = vkr_recording_put_u32(at, info->instructions);
  at = vkr_recording_put_u32(at, stat_count);
  MemCopy(at, info->name, name_length);
  at += name_length;
  for (uint32_t i = 0u; i < stat_count; ++i) {
    const uint16_t length = (uint16_t)string_length_bounded(
        stats[i].name, VKR_RECORDING_PIPELINE_STAT_NAME_MAX);
    MemCopy(at, &length, sizeof(length));
    at += sizeof(length);
    MemCopy(at, stats[i].name, length);
    at += length;
    MemCopy(at, &stats[i].value, sizeof(stats[i].value));
    at += sizeof(stats[i].value);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  ++recording->pipelines_written;
}

void vkr_recording_pipelines(VkrRecording *recording, uint64_t frame,
                             VkrPipelineRegistry *registry) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled || !registry) {
    return;
  }
  VkrRecordingPipelineWrite write = {.recording = recording, .frame = frame};
  (void)vkr_pipeline_registry_visit(registry, recording->pipelines_written,
                                    vkr_recording_pipeline_visit, &write);
}

void vkr_recording_allocators(VkrRecording *recording, uint64_t frame) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled) {
    return;
  }
  VkrAllocatorRegistrySample samples[VKR_ALLOCATOR_REGISTRY_CAPACITY];
  const uint32_t found =
      vkr_allocator_registry_sample(samples, ArrayCount(samples));
  const uint32_t count = Min(found, (uint32_t)ArrayCount(samples));
  uint32_t ids[VKR_ALLOCATOR_REGISTRY_CAPACITY];
  uint32_t rows = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    ids[i] = vkr_recording_allocator_id(recording, frame, &samples[i]);
    rows += ids[i] != UINT32_MAX ? 1u : 0u;
  }
  if (rows == 0u) {
    return;
  }
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_ALLOCATORS, frame,
                           VKR_RECORDING_ALLOCATORS_HEADER_SIZE +
                               VKR_RECORDING_ALLOCATOR_ROW_SIZE * rows);
  /* A dropped record leaves the counts, so the next row covers both
     frames. */
  if (!record) {
    return;
  }
  uint8_t *at = vkr_recording_put_u32(record, rows);
  at = vkr_recording_put_u32(at, 0u);
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t id = ids[i];
    if (id == UINT32_MAX) {
      continue;
    }
    const VkrAllocatorRegistrySample *sample = &samples[i];
    const uint64_t counts[4] = {sample->alloc_count, sample->free_count,
                                sample->realloc_count, sample->scope_count};
    at = vkr_recording_put_u32(at, id);
    at = vkr_recording_put_u32(at, sample->scope_depth_high);
    for (uint32_t c = 0u; c < 4u; ++c) {
      const uint64_t since = counts[c] - recording->allocator_counts[id][c];
      at =
          vkr_recording_put_u32(at, (uint32_t)Min(since, (uint64_t)UINT32_MAX));
      recording->allocator_counts[id][c] = counts[c];
    }
    const uint64_t bytes[5] = {sample->live_bytes, sample->arena_used_bytes,
                               sample->arena_committed_bytes,
                               sample->arena_reserved_bytes,
                               sample->arena_interval_peak_used_bytes};
    MemCopy(at, bytes, sizeof(bytes));
    at += sizeof(bytes);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
}

_Static_assert(
    VKR_GPU_DRAW_CENSUS_NOT_CAMERA == VKR_RECORDING_DRAW_STATE_NOT_CAMERA &&
        VKR_GPU_DRAW_CENSUS_DRAWN == VKR_RECORDING_DRAW_STATE_DRAWN &&
        VKR_GPU_DRAW_CENSUS_DRAWN_LATE == VKR_RECORDING_DRAW_STATE_DRAWN_LATE &&
        VKR_GPU_DRAW_CENSUS_OCCLUDED == VKR_RECORDING_DRAW_STATE_OCCLUDED &&
        VKR_GPU_DRAW_CENSUS_FRUSTUM == VKR_RECORDING_DRAW_STATE_FRUSTUM &&
        VKR_GPU_DRAW_CENSUS_DISTANCE == VKR_RECORDING_DRAW_STATE_DISTANCE,
    "recorded draw states are the census's");
_Static_assert(VKR_GPU_DRAW_CENSUS_STATES_PER_WORD ==
                   VKR_RECORDING_DRAW_STATES_PER_WORD,
               "recorded state words pack like the census");

bool8_t vkr_recording_draw_bounds_wanted(const VkrRecording *recording,
                                         uint64_t generation) {
  return recording->active && recording->passes_enabled &&
         (!recording->draw_bounds_written ||
          recording->draw_bounds_generation != generation);
}

void vkr_recording_draw_bounds(VkrRecording *recording, uint64_t frame,
                               uint64_t generation,
                               const VkrWorldObjectBounds *rows, uint32_t count,
                               float32_t cell_size) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled || !rows ||
      count > VKR_RECORDING_DRAW_CANDIDATE_MAX) {
    return;
  }
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_DRAW_BOUNDS, frame,
                           VKR_RECORDING_DRAW_BOUNDS_HEADER_SIZE +
                               VKR_RECORDING_DRAW_BOUNDS_ROW_SIZE * count);
  /* A full ring leaves the bounds wanted for the next frame. */
  if (!record) {
    return;
  }
  uint8_t *at = vkr_recording_put_u32(record, count);
  at = vkr_recording_put_u32(at, 0u);
  MemCopy(at, &cell_size, sizeof(cell_size));
  at = vkr_recording_put_u32(at + sizeof(cell_size), 0u);
  for (uint32_t i = 0u; i < count; ++i) {
    at = vkr_recording_put_u32(at, rows[i].object_id);
    MemCopy(at, rows[i].min, sizeof(rows[i].min));
    at += sizeof(rows[i].min);
    MemCopy(at, rows[i].max, sizeof(rows[i].max));
    at += sizeof(rows[i].max);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
  recording->draw_bounds_generation = generation;
  recording->draw_bounds_written = true_v;
}

void vkr_recording_draws(VkrRecording *recording,
                         const VkrRendererMetricsDrawCensus *census) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->passes_enabled || !census || !census->valid ||
      !census->states || !census->object_ids ||
      census->candidate_count > VKR_RECORDING_DRAW_CANDIDATE_MAX ||
      census->source_frame_index < recording->draw_frame_written) {
    return;
  }
  const uint64_t target =
      vkr_recording_frame_of(recording, census->source_frame_index);
  if (target == UINT64_MAX) {
    return;
  }
  recording->draw_frame_written = census->source_frame_index + 1u;
  const uint32_t count = census->candidate_count;
  if (!recording->draw_ids_written ||
      recording->draw_ids_generation != census->ids_generation) {
    uint8_t *ids =
        vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                             VKR_RECORDING_STREAM_DRAW_IDS, target,
                             VKR_RECORDING_DRAW_IDS_HEADER_SIZE + 4u * count);
    /* States without their ids would name the wrong objects. */
    if (!ids) {
      return;
    }
    uint8_t *at = vkr_recording_put_u32(ids, count);
    at = vkr_recording_put_u32(at, 0u);
    MemCopy(at, &census->ids_generation, sizeof(uint64_t));
    MemCopy(at + sizeof(uint64_t), census->object_ids, 4u * (uint64_t)count);
    vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
    recording->draw_ids_generation = census->ids_generation;
    recording->draw_ids_written = true_v;
  }
  const uint32_t words = (count + VKR_RECORDING_DRAW_STATES_PER_WORD - 1u) /
                         VKR_RECORDING_DRAW_STATES_PER_WORD;
  uint8_t *record =
      vkr_recorder_reserve(recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
                           VKR_RECORDING_STREAM_DRAW_STATES, target,
                           VKR_RECORDING_DRAW_STATES_HEADER_SIZE + 4u * words);
  if (!record) {
    return;
  }
  uint32_t flags = 0u;
  flags |= (census->flags & VKR_DRAW_CENSUS_TWO_PHASE)
               ? VKR_RECORDING_DRAW_STATES_TWO_PHASE
               : 0u;
  flags |= (census->flags & VKR_DRAW_CENSUS_HZB_HISTORY_VALID)
               ? VKR_RECORDING_DRAW_STATES_HZB_VALID
               : 0u;
  flags |= (census->flags & VKR_DRAW_CENSUS_FROZEN_CULL)
               ? VKR_RECORDING_DRAW_STATES_FROZEN
               : 0u;
  uint8_t *at = vkr_recording_put_u32(record, count);
  at = vkr_recording_put_u32(at, flags);
  at = vkr_recording_put_u32(at, census->overflow_count);
  at = vkr_recording_put_u32(at, 0u);
  MemCopy(at, &census->ids_generation, sizeof(uint64_t));
  MemCopy(at + sizeof(uint64_t), census->states, 4u * (uint64_t)words);
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
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

void vkr_recording_cells(VkrRecording *recording, const int32_t *cells,
                         uint32_t count) {
  vkr_assert(recording != NULL && recording->active);
  if (!recording->session_written) {
    return;
  }
  count = Min(count, VKR_RECORDING_CELLS_MAX);
  const uint32_t pairs = sizeof(int32_t) * 2u * count;
  uint8_t *record = vkr_recorder_reserve(
      recording->recorder, VKR_RECORDER_PRODUCER_MAIN,
      VKR_RECORDING_STREAM_CELLS, recording->frame, 4u + pairs);
  if (!record) {
    return;
  }
  MemCopy(record, &count, sizeof(count));
  if (pairs) {
    MemCopy(record + 4u, cells, pairs);
  }
  vkr_recorder_commit(recording->recorder, VKR_RECORDER_PRODUCER_MAIN);
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
vkr_internal void vkr_recording_write_analysis(const char *bundle_path,
                                               VkrAllocator *allocator,
                                               VkrJsonWriter *json) {
  char stream[VKR_RECORDING_PATH_MAX];
  string_format(stream, sizeof(stream), "%s/%s", bundle_path,
                VKR_RECORDING_STREAM_FILE);
  VkrRecordingData data;
  if (vkr_recording_data_load(&data, allocator, stream,
                              vkr_content_codec_zstd_decoder()) !=
      VKR_RECORDING_DATA_OK) {
    return;
  }
  VkrRecordingAnalysis analysis;
  const VkrRecordingAnalysisOptions options = {0};
  if (vkr_recording_analyze(&data, &options, allocator, &analysis)) {
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
/* The facts of one written bundle: where it is, the frames and seconds it
   holds, whether it carries the recording's markers, and the allocator its
   analysis may use. */
typedef struct VkrRecordingSummaryTarget {
  const char *bundle_path;
  uint64_t frames;
  float64_t seconds;
  bool8_t markers;
  VkrAllocator *allocator;
} VkrRecordingSummaryTarget;

vkr_internal bool8_t vkr_recording_write_summary_at(
    VkrRecording *recording, const VkrRecordingSummaryTarget *target,
    const VkrRecorderStats *stats, bool8_t stream_ok) {
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s", target->bundle_path,
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
  vkr_recording_json_u64(json, "frames", target->frames);
  if (recording->poses_enabled) {
    vkr_recording_json_u64(json, "poses", recording->poses_written);
    vkr_recording_json_u64(json, "spawns", recording->spawn_count);
  }
  /* Drive needs both. */
  vkr_recording_json_u64(json, "player_ticks", recording->player_records);
  vkr_recording_json_u64(json, "actions", recording->action_records);
  /* Re-simulate needs them too. */
  vkr_recording_json_u64(json, "tick_hashes", recording->tick_hashes_written);
  vkr_recording_json_u64(json, "measurements", recording->measurement_count);
  vkr_recording_json_u64(json, "all_measurements",
                         recording->all_measurements ? 1u : 0u);
  vkr_recording_json_f64(json, "duration_seconds", target->seconds);
  vkr_recording_json_u64(json, "dropped_records", stats->dropped_records);
  vkr_recording_json_u64(json, "lost_records", stats->lost_records);
  vkr_recording_json_u64(json, "stream_bytes", stats->stored_bytes);
  vkr_recording_json_f64(json, "writer_seconds", stats->writer_seconds);
  if (target->markers) {
    vkr_recording_write_markers(json, recording);
  }
  if (recording->metrics_enabled && stream_ok) {
    vkr_recording_write_analysis(target->bundle_path, target->allocator, json);
  }
  vkr_json_writer_end_object(json);
  return vkr_json_file_writer_commit(&file);
}

vkr_internal bool8_t vkr_recording_write_summary(VkrRecording *recording,
                                                 const VkrRecorderStats *stats,
                                                 bool8_t stream_ok) {
  const VkrRecordingSummaryTarget target = {
      .bundle_path = recording->bundle_path,
      .frames = recording->frame,
      .seconds = recording->time,
      .markers = true_v,
      .allocator = &recording->allocator,
  };
  return vkr_recording_write_summary_at(recording, &target, stats, stream_ok);
}

/* The names the window's records refer to: every pass and zone name, as
   records in the chunk layout, so a saved window resolves them although
   their first records left the window. Heap storage; the caller frees it. */
vkr_internal uint8_t *vkr_recording_window_preamble(VkrRecording *recording,
                                                    uint64_t *out_size) {
  const uint32_t passes =
      recording->passes_enabled ? recording->pass_count : 0u;
  const uint32_t zones = recording->zones_enabled ? vkr_zone_site_count() : 0u;
  const uint64_t capacity =
      (uint64_t)passes *
          vkr_recording_record_bytes(4u + VKR_RECORDING_PASS_NAME_MAX) +
      (uint64_t)zones * vkr_recording_record_bytes(4u + VKR_RECORDING_TEXT_MAX);
  *out_size = 0u;
  if (capacity == 0u) {
    return NULL;
  }
  uint8_t *preamble = vkr_heap_alloc_zeroed(1u, capacity);
  if (!preamble) {
    return NULL;
  }
  uint64_t size = 0u;
  for (uint32_t id = 0u; id < passes + zones; ++id) {
    const bool8_t pass = id < passes;
    const uint32_t key = pass ? id : id - passes;
    const char *name =
        pass ? recording->pass_names[key] : vkr_zone_site_name(key);
    if (!name) {
      continue;
    }
    const uint32_t length = (uint32_t)Min(
        string_length(name), (uint64_t)(pass ? VKR_RECORDING_PASS_NAME_MAX
                                             : VKR_RECORDING_TEXT_MAX));
    uint8_t *record = preamble + size;
    vkr_recording_record_header_store(record,
                                      pass ? VKR_RECORDING_STREAM_PASS_NAME
                                           : VKR_RECORDING_STREAM_ZONE_NAME,
                                      0u, 4u + length, 0u);
    MemCopy(record + VKR_RECORDING_RECORD_HEADER_SIZE, &key, sizeof(key));
    MemCopy(record + VKR_RECORDING_RECORD_HEADER_SIZE + 4u, name, length);
    size += vkr_recording_record_bytes(4u + length);
  }
  *out_size = size;
  return preamble;
}

bool8_t vkr_recording_save_window(VkrRecording *recording,
                                  const char *bundle_path,
                                  VkrRecordingWindow *out_window) {
  vkr_assert(recording != NULL && bundle_path != NULL);
  if (!recording->active || recording->rolling_seconds <= 0.0 ||
      string_length(bundle_path) + 16u >= VKR_RECORDING_PATH_MAX ||
      !vkr_recording_create_directory(bundle_path)) {
    return false_v;
  }
  char stream_path[VKR_RECORDING_PATH_MAX];
  string_format(stream_path, sizeof(stream_path), "%s/%s", bundle_path,
                VKR_RECORDING_STREAM_FILE);
  uint64_t preamble_size = 0u;
  uint8_t *preamble = vkr_recording_window_preamble(recording, &preamble_size);
  VkrRecorderSaveResult result = {0};
  const bool8_t stream_ok = vkr_recorder_save(recording->recorder, stream_path,
                                              preamble, preamble_size, &result);
  if (preamble) {
    vkr_heap_free(preamble);
  }
  if (!stream_ok) {
    return false_v;
  }
  /* The writer thread keeps allocating from the recording's memory while it
     runs, so the summary's analysis takes memory of its own. */
  VkrDMemory memory = {0};
  if (!vkr_dmemory_create(MB(4), GB(1), &memory)) {
    return false_v;
  }
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrRecorderStats stats = {0};
  vkr_recorder_stats(recording->recorder, &stats);
  const VkrRecordingSummaryTarget target = {
      .bundle_path = bundle_path,
      .frames = result.frame_count,
      .seconds = result.seconds,
      .markers = false_v,
      .allocator = &allocator,
  };
  const bool8_t summary_ok =
      vkr_recording_write_summary_at(recording, &target, &stats, true_v);
  vkr_dmemory_allocator_destroy(&allocator);
  if (out_window) {
    *out_window = (VkrRecordingWindow){.frames = result.frame_count,
                                       .seconds = result.seconds};
  }
  return summary_ok;
}

bool8_t vkr_recording_stop(VkrRecording *recording,
                           VkrRecorderStats *out_stats) {
  vkr_assert(recording != NULL);
  if (!recording->active) {
    return false_v;
  }
  if (recording->zones_enabled) {
    /* Waits until no thread is inside a zone write, so the recorder can
       stop; then names zones that only workers ran since the last frame. */
    vkr_zone_sink_set(NULL);
    vkr_recording_zone_names(recording, recording->frame);
    recording->zones_enabled = false_v;
  }
  VkrRecorderStats stats;
  const bool8_t stream_ok = vkr_recorder_stop(recording->recorder, &stats);
  /* A rolling recording wrote its saves; it has no bundle of its own. */
  const bool8_t summary_ok =
      recording->rolling_seconds > 0.0 ||
      vkr_recording_write_summary(recording, &stats, stream_ok);
  if (out_stats) {
    *out_stats = stats;
  }
  vkr_dmemory_allocator_destroy(&recording->allocator);
  recording->recorder = NULL;
  recording->active = false_v;
  return stream_ok && summary_ok;
}
