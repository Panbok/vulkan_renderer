#include "recording/vkr_recording_playback.h"

#include "core/vkr_zone.h"

#include "assets/vkr_recording_format.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "containers/vkr_sort.h"
#include "memory/vkr_dmemory_allocator.h"

/* Address space only; pages commit as the recording's data needs them. A
   long recording of a busy level holds tens of millions of poses. */
#define VKR_RECORDING_PLAYBACK_MEMORY_COMMIT (1ull * 1024ull * 1024ull)
#define VKR_RECORDING_PLAYBACK_MEMORY_RESERVE                                  \
  (8ull * 1024ull * 1024ull * 1024ull)
/* Key slots the first pass starts with; it doubles them at half load. */
#define VKR_RECORDING_POSE_SLOTS_INITIAL 4096u

/* A pose key as the first pass found it, with its index in that pass's
   order, for sorting. */
typedef struct VkrRecordingPoseKeyLoad {
  VkrRecordingPoseKey key;
  uint32_t index;
} VkrRecordingPoseKeyLoad;

/* One frame's pass rows or GPU starts as read, before the join. */
typedef struct VkrRecordingTimelinePasses {
  uint64_t frame;
  uint32_t first;
  uint32_t rows;
} VkrRecordingTimelinePasses;

typedef struct VkrRecordingTimelineStarts {
  uint64_t frame;
  uint64_t gpu_start_ns;
  uint64_t gpu_end_ns;
  uint32_t first;
  uint32_t rows;
} VkrRecordingTimelineStarts;

typedef struct VkrRecordingTimelineRow {
  uint16_t pass;
  uint16_t flags;
  float32_t gpu_ms;
} VkrRecordingTimelineRow;

typedef struct VkrRecordingTimelineLoad {
  uint32_t zones;
  uint32_t passes_records;
  uint32_t passes_rows;
  uint32_t starts_records;
  uint32_t starts_rows;
  VkrRecordingTimelinePasses *passes;
  VkrRecordingTimelineRow *rows;
  VkrRecordingTimelineStarts *starts;
  float32_t *offsets;
} VkrRecordingTimelineLoad;

typedef struct VkrRecordingLoad {
  VkrRecordingPlayback *playback;
  const VkrRecordingReader *reader;
  const VkrCodec *codec;
  /* Optional; and which of the two reads of the stream runs. */
  VkrRecordingPlaybackProgress *progress;
  uint32_t read_index;
  /* Which frames had a frame and a camera record. */
  uint8_t *present;
  /* The pose keys the first pass found, in the order it found them, with
     their pose counts; open-addressed slots hold index + 1, zero empty. The
     second pass writes each pose at its key's place in pose_samples, through
     key_order (first-pass index to sorted index). */
  VkrRecordingPoseKey *keys;
  uint32_t key_count;
  uint32_t key_capacity;
  uint32_t *key_slots;
  uint32_t key_slot_count;
  uint32_t *key_order;
  /* The metrics record's size, its valid mask's offset, and per memory
     field its offset and valid bit, or UINT32_MAX without it. */
  uint32_t metrics_size;
  uint32_t metrics_valid_offset;
  uint32_t memory_offset[2];
  uint32_t memory_bit[2];
  /* The same for each VkrRecordingSystemField. */
  uint32_t system_offset[VKR_RECORDING_SYSTEM_FIELDS];
  uint32_t system_bit[VKR_RECORDING_SYSTEM_FIELDS];
  /* Timeline sources, counted on the first pass: each frame's pass rows
     (from "passes") and their GPU starts (from "pass_starts"), joined into
     GPU spans once every record is read; the record capacities the second
     pass fills up to. */
  VkrRecordingTimelineLoad timeline;
  uint32_t timeline_passes_capacity;
  uint32_t timeline_starts_capacity;
  char *error;
  uint64_t error_capacity;
} VkrRecordingLoad;

vkr_internal bool8_t vkr_recording_load_fail(VkrRecordingLoad *load,
                                             const char *reason) {
  string_format(load->error, load->error_capacity, "%s", reason);
  return false_v;
}

_Static_assert(VKR_RECORDING_SYSTEM_FIELDS <= 8,
               "system_valid holds one bit per field");

/* The memory fields of the metrics stream: float64 values after a u64
   valid mask whose bit i covers the i-th field after it. */
vkr_internal void
vkr_recording_load_memory_fields(VkrRecordingLoad *load,
                                 const VkrRecordingStream *metrics) {
  static const char *const names[2] = {
      VKR_RECORDING_RESIDENT_FIELD,
      "memory.gpu.driver.current_allocated_bytes"};
  static const char *const system_names[VKR_RECORDING_SYSTEM_FIELDS] = {
      [VKR_RECORDING_SYSTEM_THERMAL] = "os.thermal.level",
      [VKR_RECORDING_SYSTEM_MEMORY_PRESSURE] = "os.memory.pressure",
      [VKR_RECORDING_SYSTEM_LOW_POWER] = "os.power.low_power",
      [VKR_RECORDING_SYSTEM_FOOTPRINT] = "os.memory.footprint_bytes",
      [VKR_RECORDING_SYSTEM_GPU_ERRORS] = "gpu.errors",
      [VKR_RECORDING_SYSTEM_LOG_ERRORS] = "log.errors",
  };
  load->memory_offset[0] = load->memory_offset[1] = UINT32_MAX;
  MemSet(load->system_offset, 0xff, sizeof(load->system_offset));
  if (!metrics || !string_equals(metrics->name, "metrics") ||
      metrics->field_count < 2u ||
      !string_equals(metrics->fields[0].name, "valid") ||
      metrics->fields[0].type != VKR_RECORDING_FIELD_U64) {
    return;
  }
  load->metrics_size = metrics->record_size;
  load->metrics_valid_offset = metrics->fields[0].offset;
  for (uint32_t i = 1u; i < metrics->field_count && i <= 64u; ++i) {
    const VkrRecordingField *field = &metrics->fields[i];
    for (uint32_t m = 0u; m < 2u; ++m) {
      if (field->type == VKR_RECORDING_FIELD_F64 &&
          string_equals(field->name, names[m]) &&
          field->offset + sizeof(float64_t) <= metrics->record_size) {
        load->memory_offset[m] = field->offset;
        load->memory_bit[m] = i - 1u;
        load->playback->has_memory = true_v;
      }
    }
    for (uint32_t k = 0u; k < VKR_RECORDING_SYSTEM_FIELDS; ++k) {
      if (field->type == VKR_RECORDING_FIELD_F64 &&
          string_equals(field->name, system_names[k]) &&
          field->offset + sizeof(float64_t) <= metrics->record_size) {
        load->system_offset[k] = field->offset;
        load->system_bit[k] = i - 1u;
        load->playback->has_system = true_v;
      }
    }
  }
}

/* Checks that the dictionary declares this runtime's frame and camera
   layouts, so the records can be read as the structs. */
vkr_internal bool8_t vkr_recording_load_dictionary(VkrRecordingLoad *load) {
  const uint32_t index =
      vkr_recording_reader_find(load->reader, VKR_RECORDING_CHUNK_DICTIONARY);
  uint8_t *payload = NULL;
  uint64_t size = 0u;
  if (index == UINT32_MAX || !vkr_recording_reader_chunk(
                                 load->reader, index, load->codec,
                                 &load->playback->allocator, &payload, &size)) {
    return vkr_recording_load_fail(load, "the recording has no dictionary");
  }
  VkrRecordingDictionary dictionary;
  const bool8_t decoded = vkr_recording_dictionary_decode(
      payload, size, &load->playback->allocator, &dictionary);
  vkr_recording_reader_chunk_free(&load->playback->allocator, payload, size);
  if (!decoded) {
    return vkr_recording_load_fail(load, "the dictionary is damaged");
  }
  const VkrRecordingStream *frame =
      vkr_recording_dictionary_find(&dictionary, VKR_RECORDING_STREAM_FRAME);
  const VkrRecordingStream *camera =
      vkr_recording_dictionary_find(&dictionary, VKR_RECORDING_STREAM_CAMERA);
  const bool8_t matches = frame && camera &&
                          frame->record_size == sizeof(VkrRecordingFrame) &&
                          camera->record_size == sizeof(VkrRecordingCamera) &&
                          string_equals(frame->name, "frame") &&
                          string_equals(camera->name, "camera");
  vkr_recording_load_memory_fields(
      load,
      vkr_recording_dictionary_find(&dictionary, VKR_RECORDING_STREAM_METRICS));
  vkr_recording_dictionary_free(&load->playback->allocator, &dictionary);
  return matches ? true_v
                 : vkr_recording_load_fail(
                       load, "the frame or camera layout is not this build's");
}

/* Calls `visit` for every record of every RECORDS chunk. */
typedef bool8_t (*VkrRecordingVisit)(VkrRecordingLoad *load,
                                     const VkrRecordingRecord *record);

vkr_internal bool8_t vkr_recording_load_records(VkrRecordingLoad *load,
                                                VkrRecordingVisit visit) {
  const uint32_t chunks = load->reader->chunk_count;
  for (uint32_t i = 0u; i < chunks; ++i) {
    if (load->progress) {
      if (vkr_atomic_uint32_load(&load->progress->cancel,
                                 VKR_MEMORY_ORDER_RELAXED)) {
        return vkr_recording_load_fail(load, "cancelled");
      }
      const uint64_t read = (uint64_t)load->read_index * chunks + i;
      vkr_atomic_uint32_store(
          &load->progress->permille,
          (uint32_t)(read * 1000u / (2u * (uint64_t)chunks)),
          VKR_MEMORY_ORDER_RELAXED);
    }
    if (load->reader->chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(load->reader, i, load->codec,
                                    &load->playback->allocator, &payload,
                                    &size)) {
      return vkr_recording_load_fail(load, "a records chunk is damaged");
    }
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    bool8_t ok = true_v;
    while (ok && vkr_recording_records_next(payload, size, &cursor, &record)) {
      ok = visit(load, &record);
    }
    vkr_recording_reader_chunk_free(&load->playback->allocator, payload, size);
    if (!ok) {
      return false_v;
    }
  }
  return true_v;
}

/* The id a name record names, and whether the record is one; its text
   follows the id. */
vkr_internal bool8_t vkr_recording_timeline_name_id(
    const VkrRecordingRecord *record, uint32_t *out_id) {
  if ((record->stream != VKR_RECORDING_STREAM_ZONE_NAME &&
       record->stream != VKR_RECORDING_STREAM_PASS_NAME) ||
      record->size < 4u) {
    return false_v;
  }
  MemCopy(out_id, record->data, sizeof(*out_id));
  return true_v;
}

/* The row count of a passes or pass_starts record whose size matches it,
   else zero. */
vkr_internal uint32_t
vkr_recording_timeline_rows(const VkrRecordingRecord *record) {
  uint32_t rows = 0u;
  if (record->size < 4u) {
    return 0u;
  }
  MemCopy(&rows, record->data, sizeof(rows));
  if (record->stream == VKR_RECORDING_STREAM_PASSES &&
      rows <= VKR_RECORDING_PASS_MAX &&
      record->size == VKR_RECORDING_PASSES_HEADER_SIZE +
                          (uint64_t)rows * VKR_RECORDING_PASS_ROW_SIZE) {
    return rows;
  }
  if (record->stream == VKR_RECORDING_STREAM_PASS_STARTS &&
      rows <= VKR_RECORDING_PASS_MAX &&
      record->size ==
          VKR_RECORDING_PASS_STARTS_HEADER_SIZE + (uint64_t)rows * 4u) {
    return rows;
  }
  return 0u;
}

/* First pass: counts the timeline's records and the names it needs. */
vkr_internal void
vkr_recording_timeline_count(VkrRecordingLoad *load,
                             const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
  VkrRecordingTimelineLoad *timeline = &load->timeline;
  uint32_t id = 0u;
  if (record->stream == VKR_RECORDING_STREAM_ZONE &&
      record->size == sizeof(VkrZoneRecord)) {
    ++timeline->zones;
  } else if (vkr_recording_timeline_name_id(record, &id)) {
    if (record->stream == VKR_RECORDING_STREAM_ZONE_NAME &&
        id < VKR_ZONE_SITE_MAX) {
      playback->zone_name_count = Max(playback->zone_name_count, id + 1u);
    } else if (record->stream == VKR_RECORDING_STREAM_PASS_NAME &&
               id < VKR_RECORDING_PASS_MAX) {
      playback->pass_name_count = Max(playback->pass_name_count, id + 1u);
    }
  } else if (record->stream == VKR_RECORDING_STREAM_PASSES) {
    const uint32_t rows = vkr_recording_timeline_rows(record);
    timeline->passes_records += rows > 0u ? 1u : 0u;
    timeline->passes_rows += rows;
  } else if (record->stream == VKR_RECORDING_STREAM_PASS_STARTS) {
    const uint32_t rows = vkr_recording_timeline_rows(record);
    timeline->starts_records += rows > 0u ? 1u : 0u;
    timeline->starts_rows += rows;
  }
}

/* Container, then ref, then node, then kind, then document. */
vkr_internal int32_t vkr_recording_compare_keys(const void *lhs,
                                                const void *rhs) {
  const VkrRecordingPoseKey *a = &((const VkrRecordingPoseKeyLoad *)lhs)->key;
  const VkrRecordingPoseKey *b = &((const VkrRecordingPoseKeyLoad *)rhs)->key;
  if (a->container != b->container) {
    return a->container < b->container ? -1 : 1;
  }
  const int32_t ref = MemCompare(&a->ref, &b->ref, sizeof(a->ref));
  if (ref != 0) {
    return ref;
  }
  if (a->node != b->node) {
    return a->node < b->node ? -1 : 1;
  }
  if (a->kind != b->kind) {
    return a->kind < b->kind ? -1 : 1;
  }
  if (a->document != b->document) {
    return a->document < b->document ? -1 : 1;
  }
  return 0;
}

vkr_internal bool8_t vkr_recording_pose_same_key(const VkrRecordingPoseKey *a,
                                                 const VkrRecordingPose *b) {
  return a->container == b->container && a->node == b->node &&
         a->kind == b->kind && a->document == b->document &&
         MemCompare(&a->ref, &b->ref, sizeof(a->ref)) == 0;
}

vkr_internal uint32_t
vkr_recording_pose_key_hash(const VkrRecordingPose *pose) {
  uint8_t bytes[sizeof(VkrEntityRef) + 10u];
  bytes[0] = pose->container;
  bytes[1] = pose->kind;
  MemCopy(bytes + 2u, &pose->ref, sizeof(pose->ref));
  MemCopy(bytes + 2u + sizeof(pose->ref), &pose->node, sizeof(pose->node));
  MemCopy(bytes + 6u + sizeof(pose->ref), &pose->document,
          sizeof(pose->document));
  return (uint32_t)vkr_hash_table_key_string8(
             string8_create_from_cstr(bytes, sizeof(bytes)))
      .hash;
}

/* The first-pass index of `pose`'s key, or UINT32_MAX when the first pass
   did not find it. */
vkr_internal uint32_t vkr_recording_pose_key_find(
    const VkrRecordingLoad *load, const VkrRecordingPose *pose) {
  if (load->key_slot_count == 0u) {
    return UINT32_MAX;
  }
  const uint32_t mask = load->key_slot_count - 1u;
  uint32_t probe = vkr_recording_pose_key_hash(pose) & mask;
  while (load->key_slots[probe] != 0u) {
    const uint32_t index = load->key_slots[probe] - 1u;
    if (vkr_recording_pose_same_key(&load->keys[index], pose)) {
      return index;
    }
    probe = (probe + 1u) & mask;
  }
  return UINT32_MAX;
}

/* Doubles the key slots and rehashes the keys into them. */
vkr_internal bool8_t vkr_recording_pose_slots_grow(VkrRecordingLoad *load) {
  VkrAllocator *allocator = &load->playback->allocator;
  const uint32_t slot_count = load->key_slot_count == 0u
                                  ? VKR_RECORDING_POSE_SLOTS_INITIAL
                                  : load->key_slot_count * 2u;
  uint32_t *slots =
      vkr_allocator_alloc(allocator, sizeof(uint32_t) * slot_count,
                          VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!slots) {
    return false_v;
  }
  MemZero(slots, sizeof(uint32_t) * slot_count);
  const uint32_t mask = slot_count - 1u;
  for (uint32_t i = 0u; i < load->key_count; ++i) {
    const VkrRecordingPoseKey *key = &load->keys[i];
    const VkrRecordingPose named = {.ref = key->ref,
                                    .node = key->node,
                                    .container = key->container,
                                    .kind = key->kind,
                                    .document = key->document};
    uint32_t probe = vkr_recording_pose_key_hash(&named) & mask;
    while (slots[probe] != 0u) {
      probe = (probe + 1u) & mask;
    }
    slots[probe] = i + 1u;
  }
  if (load->key_slots) {
    vkr_allocator_free(allocator, load->key_slots,
                       sizeof(uint32_t) * load->key_slot_count,
                       VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  }
  load->key_slots = slots;
  load->key_slot_count = slot_count;
  return true_v;
}

/* First pass: counts a pose under its key, adding the key when new. */
vkr_internal bool8_t vkr_recording_pose_count(VkrRecordingLoad *load,
                                              const VkrRecordingPose *pose) {
  uint32_t index = vkr_recording_pose_key_find(load, pose);
  if (index == UINT32_MAX) {
    if ((load->key_count + 1u) * 2u > load->key_slot_count &&
        !vkr_recording_pose_slots_grow(load)) {
      return false_v;
    }
    if (load->key_count == load->key_capacity) {
      const uint32_t capacity = Max(load->key_capacity * 2u, 1024u);
      VkrRecordingPoseKey *keys = vkr_allocator_realloc(
          &load->playback->allocator, load->keys,
          sizeof(VkrRecordingPoseKey) * load->key_capacity,
          sizeof(VkrRecordingPoseKey) * capacity,
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!keys) {
        return false_v;
      }
      load->keys = keys;
      load->key_capacity = capacity;
    }
    index = load->key_count++;
    load->keys[index] = (VkrRecordingPoseKey){
        .ref = pose->ref,
        .node = pose->node,
        .document = pose->document,
        .container = pose->container,
        .kind = pose->kind,
    };
    const uint32_t mask = load->key_slot_count - 1u;
    uint32_t probe = vkr_recording_pose_key_hash(pose) & mask;
    while (load->key_slots[probe] != 0u) {
      probe = (probe + 1u) & mask;
    }
    load->key_slots[probe] = index + 1u;
  }
  ++load->keys[index].count;
  return true_v;
}

vkr_internal bool8_t vkr_recording_count(VkrRecordingLoad *load,
                                         const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
  vkr_recording_timeline_count(load, record);
  if (record->stream == VKR_RECORDING_STREAM_FRAME) {
    playback->frame_count = Max(playback->frame_count, record->frame + 1u);
  } else if (record->stream == VKR_RECORDING_STREAM_MARKER &&
             record->size >= 4u) {
    ++playback->marker_count;
  } else if (record->stream == VKR_RECORDING_STREAM_EVENT &&
             record->size >= 2u) {
    ++playback->event_count;
  } else if (record->stream == VKR_RECORDING_STREAM_POSE &&
             record->size % sizeof(VkrRecordingPose) == 0u) {
    const uint32_t count = record->size / (uint32_t)sizeof(VkrRecordingPose);
    for (uint32_t i = 0u; i < count; ++i) {
      VkrRecordingPose pose;
      MemCopy(&pose, record->data + i * sizeof(VkrRecordingPose), sizeof(pose));
      if (!vkr_recording_pose_count(load, &pose)) {
        return vkr_recording_load_fail(load, "out of memory for pose keys");
      }
    }
    playback->pose_sample_count += count;
  } else if (record->stream == VKR_RECORDING_STREAM_EDIT &&
             record->size == sizeof(uint64_t)) {
    ++playback->edit_count;
  } else if (record->stream == VKR_RECORDING_STREAM_SPAWN &&
             record->size >= 9u) {
    uint32_t id = 0u;
    MemCopy(&id, record->data, sizeof(id));
    if (id < VKR_RECORDING_SPAWN_MAX) {
      playback->spawn_count = Max(playback->spawn_count, id + 1u);
      playback->spawn_text_bytes += record->size;
    }
  }
  return true_v;
}

/* Fills a spawn from a spawn record: its first frame and, when the record
   carries one, its model's paths. */
vkr_internal void vkr_recording_fill_spawn(VkrRecordingPlayback *playback,
                                           const VkrRecordingRecord *record) {
  uint32_t id = 0u;
  MemCopy(&id, record->data, sizeof(id));
  if (record->size < 9u || id >= playback->spawn_count) {
    return;
  }
  uint16_t mesh_length = 0u;
  uint16_t animation_length = 0u;
  MemCopy(&mesh_length, record->data + 5u, sizeof(mesh_length));
  if (7u + (uint32_t)mesh_length + 2u > record->size) {
    return;
  }
  MemCopy(&animation_length, record->data + 7u + mesh_length,
          sizeof(animation_length));
  const uint32_t paths = 9u + (uint32_t)mesh_length + animation_length;
  const bool8_t shaped = paths + sizeof(VkrRecordingSpawnShape) == record->size;
  if (paths != record->size && !shaped) {
    return;
  }
  VkrRecordingSpawn *spawn = &playback->spawns[id];
  if (!spawn->seen) {
    spawn->seen = true_v;
    spawn->first_frame = record->frame;
    spawn->container = record->data[4];
  }
  if (shaped) {
    MemCopy(&spawn->shape, record->data + paths, sizeof(spawn->shape));
    spawn->has_shape = true_v;
  }
  if (mesh_length == 0u ||
      playback->spawn_text_bytes + mesh_length + animation_length + 2u >
          playback->spawn_text_capacity) {
    return;
  }
  char *text = playback->spawn_text + playback->spawn_text_bytes;
  MemCopy(text, record->data + 7u, mesh_length);
  text[mesh_length] = '\0';
  MemCopy(text + mesh_length + 1u, record->data + 9u + mesh_length,
          animation_length);
  text[mesh_length + 1u + animation_length] = '\0';
  spawn->mesh_offset = (uint32_t)playback->spawn_text_bytes;
  spawn->mesh_length = mesh_length;
  spawn->animation_offset = spawn->mesh_offset + mesh_length + 1u;
  spawn->animation_length = animation_length;
  playback->spawn_text_bytes += mesh_length + animation_length + 2u;
}

/* An event record, its name added to the names when new. */
vkr_internal void vkr_recording_fill_event(VkrRecordingPlayback *playback,
                                           const VkrRecordingRecord *record) {
  VkrRecordingEventView view;
  if (!vkr_recording_event_parse(record->data, record->size, &view)) {
    return;
  }
  const uint32_t length = Min(view.name_length, VKR_RECORDING_TEXT_MAX);
  uint32_t name = 0u;
  while (name < playback->event_name_count &&
         !(string_length(playback->event_names[name]) == length &&
           MemCompare(playback->event_names[name], view.name, length) == 0)) {
    ++name;
  }
  if (name == playback->event_name_count) {
    if (name == VKR_RECORDING_PLAYBACK_EVENT_NAMES) {
      return;
    }
    MemCopy(playback->event_names[name], view.name, length);
    playback->event_names[name][length] = '\0';
    ++playback->event_name_count;
  }
  playback->events[playback->event_count++] = (VkrRecordingPlaybackEvent){
      .frame = record->frame,
      .name = name,
      .count = view.count,
      .ms = view.ms,
      .engine = view.engine,
  };
}

/* Second pass: copies the timeline's records. Names come first, since a
   stopping recording writes its last names after its last frame. */
vkr_internal void
vkr_recording_timeline_fill(VkrRecordingLoad *load,
                            const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
  VkrRecordingTimelineLoad *timeline = &load->timeline;
  uint32_t id = 0u;
  if (vkr_recording_timeline_name_id(record, &id)) {
    const bool8_t zone = record->stream == VKR_RECORDING_STREAM_ZONE_NAME;
    const uint32_t count =
        zone ? playback->zone_name_count : playback->pass_name_count;
    char *name = id < count ? (zone ? playback->zone_names[id]
                                    : playback->pass_names[id])
                            : NULL;
    /* The first name record of an id wins. */
    if (name && name[0] == '\0') {
      const uint32_t length =
          Min(record->size - 4u, (uint32_t)VKR_RECORDING_PLAYBACK_NAME_MAX);
      MemCopy(name, record->data + 4u, length);
      name[length] = '\0';
    }
    return;
  }
  if (record->frame >= playback->frame_count) {
    return;
  }
  if (record->stream == VKR_RECORDING_STREAM_ZONE &&
      record->size == sizeof(VkrZoneRecord) &&
      playback->zone_count < timeline->zones) {
    VkrZoneRecord zone;
    MemCopy(&zone, record->data, sizeof(zone));
    if (zone.end_ns < zone.begin_ns) {
      return;
    }
    playback->zones[playback->zone_count++] = (VkrRecordingPlaybackZone){
        .begin_ns = zone.begin_ns,
        .end_ns = zone.end_ns,
        .frame = record->frame,
        .zone = zone.zone,
        .thread = zone.thread,
        .depth = zone.depth,
    };
    if (zone.thread < 32u) {
      playback->zone_threads |= 1u << zone.thread;
    }
    return;
  }
  const uint32_t rows = vkr_recording_timeline_rows(record);
  if (rows == 0u) {
    return;
  }
  if (record->stream == VKR_RECORDING_STREAM_PASSES &&
      timeline->passes_records < load->timeline_passes_capacity) {
    VkrRecordingTimelinePasses *entry =
        &timeline->passes[timeline->passes_records++];
    *entry = (VkrRecordingTimelinePasses){
        .frame = record->frame, .first = timeline->passes_rows, .rows = rows};
    const uint8_t *row = record->data + VKR_RECORDING_PASSES_HEADER_SIZE;
    for (uint32_t i = 0u; i < rows; ++i) {
      VkrRecordingTimelineRow *out = &timeline->rows[timeline->passes_rows++];
      MemCopy(&out->pass, row, sizeof(out->pass));
      MemCopy(&out->flags, row + 2u, sizeof(out->flags));
      MemCopy(&out->gpu_ms, row + 8u, sizeof(out->gpu_ms));
      row += VKR_RECORDING_PASS_ROW_SIZE;
    }
  } else if (record->stream == VKR_RECORDING_STREAM_PASS_STARTS &&
             timeline->starts_records < load->timeline_starts_capacity) {
    uint32_t flags = 0u;
    MemCopy(&flags, record->data + 4u, sizeof(flags));
    if (!(flags & VKR_RECORDING_PASS_STARTS_HOST_CLOCK)) {
      return;
    }
    VkrRecordingTimelineStarts *entry =
        &timeline->starts[timeline->starts_records++];
    *entry = (VkrRecordingTimelineStarts){
        .frame = record->frame, .first = timeline->starts_rows, .rows = rows};
    MemCopy(&entry->gpu_start_ns, record->data + 8u, sizeof(uint64_t));
    MemCopy(&entry->gpu_end_ns, record->data + 16u, sizeof(uint64_t));
    MemCopy(timeline->offsets + timeline->starts_rows,
            record->data + VKR_RECORDING_PASS_STARTS_HEADER_SIZE,
            sizeof(float32_t) * rows);
    timeline->starts_rows += rows;
  }
}

vkr_internal bool8_t vkr_recording_fill(VkrRecordingLoad *load,
                                        const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
  vkr_recording_timeline_fill(load, record);
  if (record->frame >= playback->frame_count) {
    return true_v;
  }
  if (record->stream == VKR_RECORDING_STREAM_FRAME &&
      record->size == sizeof(VkrRecordingFrame)) {
    MemCopy(&playback->frames[record->frame], record->data,
            sizeof(VkrRecordingFrame));
    load->present[record->frame] |= 1u;
  } else if (record->stream == VKR_RECORDING_STREAM_CAMERA &&
             record->size == sizeof(VkrRecordingCamera)) {
    MemCopy(&playback->cameras[record->frame], record->data,
            sizeof(VkrRecordingCamera));
    load->present[record->frame] |= 2u;
  } else if (record->stream == VKR_RECORDING_STREAM_MARKER &&
             record->size >= 4u) {
    VkrRecordingMarker *marker = &playback->markers[playback->marker_count++];
    MemZero(marker, sizeof(*marker));
    marker->frame = record->frame;
    MemCopy(&marker->kind, record->data, sizeof(marker->kind));
    const uint32_t length =
        Min(record->size - 4u, (uint32_t)VKR_RECORDING_LABEL_MAX);
    MemCopy(marker->label, record->data + 4u, length);
  } else if (record->stream == VKR_RECORDING_STREAM_EVENT) {
    vkr_recording_fill_event(playback, record);
  } else if (record->stream == VKR_RECORDING_STREAM_METRICS &&
             (playback->has_memory || playback->has_system) &&
             record->size == load->metrics_size) {
    /* Partial records merge: each carries only its valid fields. */
    uint64_t valid = 0u;
    MemCopy(&valid, record->data + load->metrics_valid_offset, sizeof(valid));
    for (uint32_t m = 0u; m < 2u; ++m) {
      if (load->memory_offset[m] == UINT32_MAX ||
          !(valid & (1ull << load->memory_bit[m]))) {
        continue;
      }
      MemCopy(&playback->memory_bytes[record->frame * 2u + m],
              record->data + load->memory_offset[m], sizeof(float64_t));
      playback->memory_valid[record->frame] |= (uint8_t)(1u << m);
    }
    for (uint32_t k = 0u;
         playback->has_system && k < VKR_RECORDING_SYSTEM_FIELDS; ++k) {
      if (load->system_offset[k] == UINT32_MAX ||
          !(valid & (1ull << load->system_bit[k]))) {
        continue;
      }
      MemCopy(
          &playback
               ->system_values[record->frame * VKR_RECORDING_SYSTEM_FIELDS + k],
          record->data + load->system_offset[k], sizeof(float64_t));
      playback->system_valid[record->frame] |= (uint8_t)(1u << k);
    }
  } else if (record->stream == VKR_RECORDING_STREAM_PLAYER &&
             record->size == sizeof(VkrRecordingPlayer)) {
    /* Ticks arrive in order, so the frame keeps its last one. */
    VkrRecordingPlayer player;
    MemCopy(&player, record->data, sizeof(player));
    MemCopy(&playback->player_positions[record->frame * 3u], player.position,
            sizeof(player.position));
    playback->player_valid[record->frame] = 1u;
    playback->has_player = true_v;
  } else if (record->stream == VKR_RECORDING_STREAM_CLOUDS &&
             record->size == sizeof(VkrRecordingClouds)) {
    MemCopy(&playback->cloud_offsets[record->frame * 2u], record->data,
            sizeof(VkrRecordingClouds));
    playback->cloud_valid[record->frame] = 1u;
    playback->has_clouds = true_v;
  } else if (record->stream == VKR_RECORDING_STREAM_SPAWN) {
    vkr_recording_fill_spawn(playback, record);
  } else if (record->stream == VKR_RECORDING_STREAM_DESPAWN &&
             record->size == sizeof(uint32_t)) {
    uint32_t id = 0u;
    MemCopy(&id, record->data, sizeof(id));
    if (id < playback->spawn_count) {
      playback->spawns[id].last_frame = record->frame;
    }
  } else if (record->stream == VKR_RECORDING_STREAM_EDIT &&
             record->size == sizeof(uint64_t)) {
    /* Records of one producer arrive in frame order. */
    const uint32_t at = playback->edit_count++;
    playback->edit_frames[at] = record->frame;
    MemCopy(&playback->edit_sequences[at], record->data, sizeof(uint64_t));
  } else if (record->stream == VKR_RECORDING_STREAM_POSE &&
             record->size % sizeof(VkrRecordingPose) == 0u) {
    const uint32_t count = record->size / (uint32_t)sizeof(VkrRecordingPose);
    for (uint32_t i = 0u; i < count; ++i) {
      VkrRecordingPose pose;
      MemCopy(&pose, record->data + i * sizeof(VkrRecordingPose), sizeof(pose));
      const uint32_t found = vkr_recording_pose_key_find(load, &pose);
      if (found == UINT32_MAX) {
        continue;
      }
      VkrRecordingPoseKey *key = &playback->pose_keys[load->key_order[found]];
      VkrRecordingPoseSample *sample =
          &playback->pose_samples[key->first + key->count++];
      sample->frame = record->frame;
      MemCopy(sample->translation, pose.translation,
              sizeof(sample->translation));
      MemCopy(sample->linear, pose.linear, sizeof(sample->linear));
      sample->flags = pose.flags;
      ++playback->pose_sample_count;
    }
  }
  return true_v;
}

/* Between the passes: the keys sorted, each given its place in
   pose_samples, and the first pass's order mapped to the sorted one. The
   second pass counts each key's samples again as it writes them. */
vkr_internal bool8_t vkr_recording_playback_place_keys(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  VkrAllocator *allocator = &playback->allocator;
  const uint32_t keys = load->key_count;
  if (keys == 0u) {
    return true_v;
  }
  VkrRecordingPoseKeyLoad *sorted =
      vkr_allocator_alloc(allocator, sizeof(VkrRecordingPoseKeyLoad) * keys,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  load->key_order = vkr_allocator_alloc(allocator, sizeof(uint32_t) * keys,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->pose_keys =
      vkr_allocator_alloc(allocator, sizeof(VkrRecordingPoseKey) * keys,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->pose_samples = vkr_allocator_alloc(
      allocator, sizeof(VkrRecordingPoseSample) * playback->pose_sample_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!sorted || !load->key_order || !playback->pose_keys ||
      !playback->pose_samples) {
    if (sorted) {
      vkr_allocator_free(allocator, sorted,
                         sizeof(VkrRecordingPoseKeyLoad) * keys,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return false_v;
  }
  for (uint32_t i = 0u; i < keys; ++i) {
    sorted[i] = (VkrRecordingPoseKeyLoad){.key = load->keys[i], .index = i};
  }
  vkr_sort(sorted, keys, sizeof(*sorted), vkr_recording_compare_keys);
  uint32_t first = 0u;
  for (uint32_t k = 0u; k < keys; ++k) {
    playback->pose_keys[k] = sorted[k].key;
    playback->pose_keys[k].first = first;
    playback->pose_keys[k].count = 0u;
    first += sorted[k].key.count;
    load->key_order[sorted[k].index] = k;
  }
  playback->pose_key_count = keys;
  vkr_allocator_free(allocator, sorted, sizeof(VkrRecordingPoseKeyLoad) * keys,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

vkr_internal int32_t vkr_recording_compare_samples(const void *lhs,
                                                   const void *rhs) {
  const VkrRecordingPoseSample *a = lhs;
  const VkrRecordingPoseSample *b = rhs;
  return a->frame < b->frame ? -1 : (a->frame > b->frame ? 1 : 0);
}

/* After the second pass: orders each key's samples by frame, and closes the
   gaps poses past the last frame left, dropping keys they emptied. */
vkr_internal void vkr_recording_playback_group_poses(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  uint32_t keys = 0u;
  uint32_t samples = 0u;
  for (uint32_t k = 0u; k < playback->pose_key_count; ++k) {
    VkrRecordingPoseKey key = playback->pose_keys[k];
    if (key.count == 0u) {
      continue;
    }
    VkrRecordingPoseSample *range = &playback->pose_samples[key.first];
    for (uint32_t i = 1u; i < key.count; ++i) {
      if (range[i].frame < range[i - 1u].frame) {
        vkr_sort(range, key.count, sizeof(*range),
                 vkr_recording_compare_samples);
        break;
      }
    }
    if (key.first != samples) {
      MemCopy(&playback->pose_samples[samples], range,
              sizeof(*range) * key.count);
    }
    key.first = samples;
    samples += key.count;
    playback->pose_keys[keys++] = key;
  }
  playback->pose_key_count = keys;
}

/* Releases the first pass's key table. */
vkr_internal void vkr_recording_load_keys_free(VkrRecordingLoad *load) {
  VkrAllocator *allocator = &load->playback->allocator;
  if (load->key_slots) {
    vkr_allocator_free(allocator, load->key_slots,
                       sizeof(uint32_t) * load->key_slot_count,
                       VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  }
  if (load->keys) {
    vkr_allocator_free(allocator, load->keys,
                       sizeof(VkrRecordingPoseKey) * load->key_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (load->key_order) {
    vkr_allocator_free(allocator, load->key_order,
                       sizeof(uint32_t) * load->key_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  load->key_slots = NULL;
  load->keys = NULL;
  load->key_order = NULL;
}

/* By frame, then name, then count: a total order. */
vkr_internal int32_t vkr_recording_compare_events(const void *lhs,
                                                  const void *rhs) {
  const VkrRecordingPlaybackEvent *a = lhs;
  const VkrRecordingPlaybackEvent *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  if (a->name != b->name) {
    return a->name < b->name ? -1 : 1;
  }
  return a->count < b->count ? -1 : (a->count > b->count ? 1 : 0);
}

/* Repeats the last whole frame over lost ones and builds the clock. */
vkr_internal void vkr_recording_playback_finish(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  for (uint64_t i = 0u; i < playback->frame_count; ++i) {
    if (load->present[i] != 3u) {
      ++playback->missing_frames;
      if (i > 0u) {
        playback->frames[i] = playback->frames[i - 1u];
        playback->cameras[i] = playback->cameras[i - 1u];
      }
    }
    /* Clouds write only while the scene has them; a frame without a
       record keeps the offset before it. */
    if (!playback->cloud_valid[i] && i > 0u && playback->cloud_valid[i - 1u]) {
      playback->cloud_offsets[i * 2u] = playback->cloud_offsets[i * 2u - 2u];
      playback->cloud_offsets[i * 2u + 1u] =
          playback->cloud_offsets[i * 2u - 1u];
      playback->cloud_valid[i] = 1u;
    }
    playback->times[i] =
        i == 0u ? 0.0
                : playback->times[i - 1u] + playback->frames[i].display_dt;
  }
  playback->duration = playback->times[playback->frame_count - 1u];
  for (uint32_t i = 0u; i < playback->marker_count; ++i) {
    VkrRecordingMarker *marker = &playback->markers[i];
    marker->time =
        playback->times[Min(marker->frame, playback->frame_count - 1u)];
  }
  /* The renderer's events arrive with their publication, after events of
     later frames. */
  vkr_sort(playback->events, playback->event_count, sizeof(*playback->events),
           vkr_recording_compare_events);
}

vkr_internal bool8_t
vkr_recording_playback_allocate(VkrRecordingPlayback *playback) {
  const uint64_t frames = playback->frame_count;
  playback->frames = vkr_allocator_alloc(&playback->allocator,
                                         sizeof(VkrRecordingFrame) * frames,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->cameras = vkr_allocator_alloc(&playback->allocator,
                                          sizeof(VkrRecordingCamera) * frames,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->times =
      vkr_allocator_alloc(&playback->allocator, sizeof(float64_t) * frames,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->markers = vkr_allocator_alloc(
      &playback->allocator,
      sizeof(VkrRecordingMarker) * ((uint64_t)playback->marker_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->events =
      vkr_allocator_alloc(&playback->allocator,
                          sizeof(VkrRecordingPlaybackEvent) *
                              ((uint64_t)playback->event_count + 1u),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->player_positions =
      vkr_allocator_alloc(&playback->allocator, sizeof(float64_t) * frames * 3u,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->player_valid = vkr_allocator_alloc(&playback->allocator, frames,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->cloud_offsets =
      vkr_allocator_alloc(&playback->allocator, sizeof(float64_t) * frames * 2u,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->cloud_valid = vkr_allocator_alloc(&playback->allocator, frames,
                                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->memory_bytes =
      vkr_allocator_alloc(&playback->allocator, sizeof(float64_t) * frames * 2u,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->memory_valid = vkr_allocator_alloc(&playback->allocator, frames,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (playback->has_system) {
    playback->system_values = vkr_allocator_alloc(
        &playback->allocator,
        sizeof(float64_t) * frames * VKR_RECORDING_SYSTEM_FIELDS,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    playback->system_valid = vkr_allocator_alloc(
        &playback->allocator, frames, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!playback->frames || !playback->cameras || !playback->times ||
      !playback->markers || !playback->events || !playback->player_positions ||
      !playback->player_valid || !playback->cloud_offsets ||
      !playback->cloud_valid || !playback->memory_bytes ||
      !playback->memory_valid ||
      (playback->has_system &&
       (!playback->system_values || !playback->system_valid))) {
    return false_v;
  }
  MemZero(playback->player_valid, frames);
  MemZero(playback->cloud_valid, frames);
  MemZero(playback->memory_valid, frames);
  if (playback->has_system) {
    MemZero(playback->system_valid, frames);
  }
  MemZero(playback->frames, sizeof(VkrRecordingFrame) * frames);
  MemZero(playback->cameras, sizeof(VkrRecordingCamera) * frames);
  return true_v;
}

/* Between the passes: storage for the timeline the first pass counted, and
   counts reset for the second pass to fill. Recordings without zones or
   GPU starts get none. */
vkr_internal bool8_t vkr_recording_timeline_allocate(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  VkrRecordingTimelineLoad *timeline = &load->timeline;
  playback->has_timeline =
      timeline->zones > 0u || timeline->starts_records > 0u;
  if (!playback->has_timeline) {
    return true_v;
  }
  VkrAllocator *allocator = &playback->allocator;
  const uint64_t name_bytes = VKR_RECORDING_PLAYBACK_NAME_MAX + 1u;
  playback->zones = vkr_allocator_alloc(
      allocator, sizeof(*playback->zones) * ((uint64_t)timeline->zones + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->zone_names = vkr_allocator_alloc(
      allocator, name_bytes * ((uint64_t)playback->zone_name_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  playback->pass_names = vkr_allocator_alloc(
      allocator, name_bytes * ((uint64_t)playback->pass_name_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  playback->frame_begin_ns =
      vkr_allocator_alloc(allocator, sizeof(uint64_t) * playback->frame_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->gpu_spans = vkr_allocator_alloc(
      allocator,
      sizeof(*playback->gpu_spans) *
          ((uint64_t)timeline->starts_rows + timeline->starts_records + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  timeline->passes = vkr_allocator_alloc(
      allocator,
      sizeof(*timeline->passes) * ((uint64_t)timeline->passes_records + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  timeline->rows = vkr_allocator_alloc(
      allocator,
      sizeof(*timeline->rows) * ((uint64_t)timeline->passes_rows + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  timeline->starts = vkr_allocator_alloc(
      allocator,
      sizeof(*timeline->starts) * ((uint64_t)timeline->starts_records + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  timeline->offsets = vkr_allocator_alloc(
      allocator,
      sizeof(*timeline->offsets) * ((uint64_t)timeline->starts_rows + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!playback->zones || !playback->zone_names || !playback->pass_names ||
      !playback->frame_begin_ns || !playback->gpu_spans || !timeline->passes ||
      !timeline->rows || !timeline->starts || !timeline->offsets) {
    return false_v;
  }
  MemZero(playback->zone_names,
          name_bytes * ((uint64_t)playback->zone_name_count + 1u));
  MemZero(playback->pass_names,
          name_bytes * ((uint64_t)playback->pass_name_count + 1u));
  MemZero(playback->frame_begin_ns, sizeof(uint64_t) * playback->frame_count);
  load->timeline_passes_capacity = timeline->passes_records;
  load->timeline_starts_capacity = timeline->starts_records;
  timeline->passes_records = 0u;
  timeline->passes_rows = 0u;
  timeline->starts_records = 0u;
  timeline->starts_rows = 0u;
  return true_v;
}

vkr_internal int32_t vkr_recording_compare_zones(const void *lhs,
                                                 const void *rhs) {
  const VkrRecordingPlaybackZone *a = lhs;
  const VkrRecordingPlaybackZone *b = rhs;
  if (a->begin_ns != b->begin_ns) {
    return a->begin_ns < b->begin_ns ? -1 : 1;
  }
  if (a->thread != b->thread) {
    return a->thread < b->thread ? -1 : 1;
  }
  return a->depth < b->depth ? -1 : (a->depth > b->depth ? 1 : 0);
}

vkr_internal int32_t vkr_recording_compare_gpu_spans(const void *lhs,
                                                     const void *rhs) {
  const VkrRecordingPlaybackGpuSpan *a = lhs;
  const VkrRecordingPlaybackGpuSpan *b = rhs;
  if (a->begin_ns != b->begin_ns) {
    return a->begin_ns < b->begin_ns ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

/* After the second pass: joins each frame's GPU starts with its pass rows,
   in the order both carry, sorts zones and spans by begin, and finds each
   frame's "Frame" zone. Frames whose rows and starts disagree in count keep
   only their GPU frame span. */
vkr_internal bool8_t vkr_recording_timeline_finish(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  VkrRecordingTimelineLoad *timeline = &load->timeline;
  if (!playback->has_timeline) {
    return true_v;
  }
  VkrAllocator *allocator = &playback->allocator;
  uint32_t *passes_of_frame =
      vkr_allocator_alloc(allocator, sizeof(uint32_t) * playback->frame_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!passes_of_frame) {
    return false_v;
  }
  MemZero(passes_of_frame, sizeof(uint32_t) * playback->frame_count);
  for (uint32_t i = 0u; i < timeline->passes_records; ++i) {
    const uint64_t frame = timeline->passes[i].frame;
    if (frame < playback->frame_count && passes_of_frame[frame] == 0u) {
      passes_of_frame[frame] = i + 1u;
    }
  }
  for (uint32_t s = 0u; s < timeline->starts_records; ++s) {
    const VkrRecordingTimelineStarts *starts = &timeline->starts[s];
    if (starts->frame >= playback->frame_count ||
        starts->gpu_end_ns < starts->gpu_start_ns) {
      continue;
    }
    playback->gpu_spans[playback->gpu_span_count++] =
        (VkrRecordingPlaybackGpuSpan){
            .begin_ns = starts->gpu_start_ns,
            .end_ns = starts->gpu_end_ns,
            .frame = starts->frame,
            .pass = VKR_RECORDING_PLAYBACK_GPU_FRAME,
        };
    const uint32_t found = passes_of_frame[starts->frame];
    if (found == 0u || timeline->passes[found - 1u].rows != starts->rows) {
      continue;
    }
    const VkrRecordingTimelinePasses *passes = &timeline->passes[found - 1u];
    for (uint32_t r = 0u; r < starts->rows; ++r) {
      const VkrRecordingTimelineRow *row = &timeline->rows[passes->first + r];
      const float32_t offset_ms = timeline->offsets[starts->first + r];
      if (!(row->flags & VKR_RECORDING_PASS_GPU_VALID) ||
          !vkr_is_finite_f32(offset_ms) || offset_ms < 0.0f ||
          !vkr_is_finite_f32(row->gpu_ms) || row->gpu_ms < 0.0f) {
        continue;
      }
      const uint64_t begin =
          starts->gpu_start_ns + (uint64_t)((float64_t)offset_ms * 1.0e6);
      playback->gpu_spans[playback->gpu_span_count++] =
          (VkrRecordingPlaybackGpuSpan){
              .begin_ns = begin,
              .end_ns = begin + (uint64_t)((float64_t)row->gpu_ms * 1.0e6),
              .frame = starts->frame,
              .pass = row->pass,
          };
    }
  }
  vkr_allocator_free(allocator, passes_of_frame,
                     sizeof(uint32_t) * playback->frame_count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  vkr_sort(playback->zones, playback->zone_count, sizeof(*playback->zones),
           vkr_recording_compare_zones);
  vkr_sort(playback->gpu_spans, playback->gpu_span_count,
           sizeof(*playback->gpu_spans), vkr_recording_compare_gpu_spans);
  for (uint32_t i = 0u; i < playback->zone_count; ++i) {
    const VkrRecordingPlaybackZone *zone = &playback->zones[i];
    playback->zone_longest_ns =
        Max(playback->zone_longest_ns, zone->end_ns - zone->begin_ns);
    if (zone->thread == VKR_ZONE_THREAD_MAIN &&
        zone->zone < playback->zone_name_count &&
        string_equals(playback->zone_names[zone->zone], "Frame")) {
      playback->frame_begin_ns[zone->frame] = zone->begin_ns;
    }
  }
  for (uint32_t i = 0u; i < playback->gpu_span_count; ++i) {
    const VkrRecordingPlaybackGpuSpan *span = &playback->gpu_spans[i];
    playback->gpu_longest_ns =
        Max(playback->gpu_longest_ns, span->end_ns - span->begin_ns);
  }
  return true_v;
}

vkr_internal bool8_t vkr_recording_playback_load(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  if (!vkr_recording_load_dictionary(load) ||
      !vkr_recording_load_records(load, vkr_recording_count)) {
    return false_v;
  }
  if (playback->frame_count == 0u) {
    return vkr_recording_load_fail(load, "the recording has no frames");
  }
  if (!vkr_recording_playback_allocate(playback) ||
      !vkr_recording_timeline_allocate(load)) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  load->present =
      vkr_allocator_alloc(&playback->allocator, playback->frame_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!load->present) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  MemZero(load->present, playback->frame_count);
  /* Sample indices are u32, below the VKR_RECORDING_POSE_APPLIED_* values. */
  if (playback->pose_sample_count >= VKR_RECORDING_POSE_APPLIED_BLENDED) {
    return vkr_recording_load_fail(load, "the recording holds too many poses");
  }
  if (!vkr_recording_playback_place_keys(load)) {
    string_format(load->error, load->error_capacity,
                  "out of memory for %llu poses (%.1f GB)",
                  (unsigned long long)playback->pose_sample_count,
                  (float64_t)(playback->pose_sample_count *
                              sizeof(VkrRecordingPoseSample)) /
                      (1024.0 * 1024.0 * 1024.0));
    return false_v;
  }
  playback->edit_frames = vkr_allocator_alloc(
      &playback->allocator, sizeof(uint64_t) * (playback->edit_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->edit_sequences = vkr_allocator_alloc(
      &playback->allocator, sizeof(uint64_t) * (playback->edit_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!playback->edit_frames || !playback->edit_sequences) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  playback->spawns = vkr_allocator_alloc(
      &playback->allocator,
      sizeof(VkrRecordingSpawn) * ((uint64_t)playback->spawn_count + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->spawn_text_capacity = playback->spawn_text_bytes + 1u;
  playback->spawn_text =
      vkr_allocator_alloc(&playback->allocator, playback->spawn_text_capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!playback->spawns || !playback->spawn_text) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  MemZero(playback->spawns,
          sizeof(VkrRecordingSpawn) * ((uint64_t)playback->spawn_count + 1u));
  for (uint32_t i = 0u; i < playback->spawn_count; ++i) {
    playback->spawns[i].last_frame = UINT64_MAX;
  }
  playback->spawn_text_bytes = 0u;
  playback->marker_count = 0u;
  playback->event_count = 0u;
  playback->event_name_count = 0u;
  playback->pose_sample_count = 0u;
  playback->edit_count = 0u;
  load->read_index = 1u;
  if (!vkr_recording_load_records(load, vkr_recording_fill)) {
    return false_v;
  }
  vkr_recording_playback_group_poses(load);
  vkr_recording_load_keys_free(load);
  vkr_recording_playback_finish(load);
  if (!vkr_recording_timeline_finish(load)) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  return true_v;
}

/* A bundle directory holds the stream file; a file path is used as given. */
vkr_internal VkrRecordingReadError
vkr_recording_playback_reader(VkrRecordingPlayback *playback,
                              VkrRecordingReader *reader, const char *path) {
  char stream[VKR_RECORDING_PATH_MAX];
  string_format(stream, sizeof(stream), "%s/%s", path,
                VKR_RECORDING_STREAM_FILE);
  VkrRecordingReadError error =
      vkr_recording_reader_open(reader, &playback->allocator, stream);
  if (error == VKR_RECORDING_READ_OPEN) {
    error = vkr_recording_reader_open(reader, &playback->allocator, path);
  }
  return error;
}

bool8_t vkr_recording_playback_open(VkrRecordingPlayback *playback,
                                    const char *path, const VkrCodec *codec,
                                    VkrRecordingPlaybackProgress *progress,
                                    char *error, uint64_t error_capacity) {
  vkr_assert(playback != NULL && path != NULL && error != NULL);
  vkr_assert(!playback->open && "close the open playback first");
  MemZero(playback, sizeof(*playback));
  if (!vkr_dmemory_create(VKR_RECORDING_PLAYBACK_MEMORY_COMMIT,
                          VKR_RECORDING_PLAYBACK_MEMORY_RESERVE,
                          &playback->memory)) {
    string_format(error, error_capacity, "out of memory");
    return false_v;
  }
  playback->allocator = (VkrAllocator){.ctx = &playback->memory};
  vkr_dmemory_allocator_create(&playback->allocator);
  string_format(playback->bundle_path, sizeof(playback->bundle_path), "%s",
                path);

  VkrRecordingReader reader;
  const VkrRecordingReadError read_error =
      vkr_recording_playback_reader(playback, &reader, path);
  if (read_error != VKR_RECORDING_READ_OK) {
    string_format(error, error_capacity, "'%s' is not a readable recording",
                  path);
    vkr_dmemory_allocator_destroy(&playback->allocator);
    MemZero(playback, sizeof(*playback));
    return false_v;
  }
  VkrRecordingLoad load = {.playback = playback,
                           .reader = &reader,
                           .codec = codec,
                           .progress = progress,
                           .error = error,
                           .error_capacity = error_capacity};
  const bool8_t loaded = vkr_recording_playback_load(&load);
  playback->truncated = reader.truncated;
  vkr_recording_reader_close(&reader);
  if (!loaded) {
    vkr_dmemory_allocator_destroy(&playback->allocator);
    MemZero(playback, sizeof(*playback));
    return false_v;
  }
  playback->speed = 1.0f;
  playback->open = true_v;
  return true_v;
}

/* The first of `count` spans of `stride` bytes, begin_ns first, whose begin
   is at or after `at`. */
vkr_internal uint32_t vkr_recording_lower_bound(const void *spans,
                                                uint32_t count, uint64_t stride,
                                                uint64_t at) {
  uint32_t low = 0u;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    uint64_t begin = 0u;
    MemCopy(&begin, (const uint8_t *)spans + stride * middle, sizeof(begin));
    if (begin < at) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

uint32_t vkr_recording_playback_zones_in(const VkrRecordingPlayback *playback,
                                         uint64_t begin_ns, uint64_t end_ns,
                                         uint32_t *out_first) {
  *out_first = 0u;
  if (!playback->zones || playback->zone_count == 0u || end_ns <= begin_ns) {
    return 0u;
  }
  /* A zone that began up to the longest zone's length earlier may still
     cover begin_ns. */
  const uint64_t from = begin_ns > playback->zone_longest_ns
                            ? begin_ns - playback->zone_longest_ns
                            : 0u;
  const uint32_t first = vkr_recording_lower_bound(
      playback->zones, playback->zone_count, sizeof(*playback->zones), from);
  const uint32_t last = vkr_recording_lower_bound(
      playback->zones, playback->zone_count, sizeof(*playback->zones), end_ns);
  *out_first = first;
  return last - first;
}

uint32_t
vkr_recording_playback_gpu_spans_in(const VkrRecordingPlayback *playback,
                                    uint64_t begin_ns, uint64_t end_ns,
                                    uint32_t *out_first) {
  *out_first = 0u;
  if (!playback->gpu_spans || playback->gpu_span_count == 0u ||
      end_ns <= begin_ns) {
    return 0u;
  }
  const uint64_t from = begin_ns > playback->gpu_longest_ns
                            ? begin_ns - playback->gpu_longest_ns
                            : 0u;
  const uint32_t first =
      vkr_recording_lower_bound(playback->gpu_spans, playback->gpu_span_count,
                                sizeof(*playback->gpu_spans), from);
  const uint32_t last =
      vkr_recording_lower_bound(playback->gpu_spans, playback->gpu_span_count,
                                sizeof(*playback->gpu_spans), end_ns);
  *out_first = first;
  return last - first;
}

void vkr_recording_playback_move(VkrRecordingPlayback *to,
                                 VkrRecordingPlayback *from) {
  vkr_assert(to != NULL && from != NULL && !to->open);
  *to = *from;
  /* The allocator names the memory beside it; the memory's own pointers
     reach only the blocks it reserved. */
  to->allocator.ctx = &to->memory;
  MemZero(from, sizeof(*from));
}

void vkr_recording_playback_close(VkrRecordingPlayback *playback) {
  vkr_assert(playback != NULL);
  if (playback->open) {
    vkr_dmemory_allocator_destroy(&playback->allocator);
  }
  MemZero(playback, sizeof(*playback));
}

void vkr_recording_playback_advance(VkrRecordingPlayback *playback,
                                    float64_t seconds) {
  vkr_assert(playback != NULL && playback->open);
  if (!playback->playing) {
    return;
  }
  float64_t time = playback->time + seconds * (float64_t)playback->speed;
  const bool8_t span = playback->loop_end > playback->loop_start;
  const float64_t start = span ? playback->loop_start : 0.0;
  const float64_t end = span ? playback->loop_end : playback->duration;
  if (playback->loop && end > start && (time >= end || time < start)) {
    /* A playhead outside the span enters it at its start. */
    const float64_t length = end - start;
    time = time < start
               ? start
               : start + (time - end) -
                     length * (float64_t)(uint64_t)((time - end) / length);
  } else if (time >= playback->duration) {
    time = playback->duration;
    playback->playing = false_v;
  }
  playback->time = Max(time, 0.0);
}

void vkr_recording_playback_set_loop_range(VkrRecordingPlayback *playback,
                                           float64_t start, float64_t end) {
  vkr_assert(playback != NULL && playback->open);
  playback->loop_start = Min(Max(start, 0.0), playback->duration);
  playback->loop_end = Min(Max(end, 0.0), playback->duration);
}

void vkr_recording_playback_seek_time(VkrRecordingPlayback *playback,
                                      float64_t time) {
  vkr_assert(playback != NULL && playback->open);
  playback->time = Min(Max(time, 0.0), playback->duration);
}

void vkr_recording_playback_seek_frame(VkrRecordingPlayback *playback,
                                       uint64_t frame) {
  vkr_assert(playback != NULL && playback->open);
  playback->time = playback->times[Min(frame, playback->frame_count - 1u)];
}

bool8_t vkr_recording_playback_spawn_model(const VkrRecordingPlayback *playback,
                                           uint32_t id, String8 *out_mesh,
                                           String8 *out_animation) {
  vkr_assert(playback != NULL && out_mesh != NULL && out_animation != NULL);
  if (id >= playback->spawn_count || !playback->spawns[id].mesh_length) {
    return false_v;
  }
  const VkrRecordingSpawn *spawn = &playback->spawns[id];
  *out_mesh =
      (String8){.str = (uint8_t *)playback->spawn_text + spawn->mesh_offset,
                .length = spawn->mesh_length};
  *out_animation = (String8){.str = (uint8_t *)playback->spawn_text +
                                    spawn->animation_offset,
                             .length = spawn->animation_length};
  return true_v;
}

bool8_t vkr_recording_playback_edit_at(const VkrRecordingPlayback *playback,
                                       uint64_t frame, uint64_t *out_sequence) {
  vkr_assert(playback != NULL && out_sequence != NULL);
  if (playback->edit_count == 0u) {
    return false_v;
  }
  uint32_t at = 0u;
  while (at + 1u < playback->edit_count &&
         playback->edit_frames[at + 1u] <= frame) {
    ++at;
  }
  *out_sequence = playback->edit_sequences[at];
  return true_v;
}

uint32_t vkr_recording_playback_pose_at(const VkrRecordingPlayback *playback,
                                        uint32_t key, uint64_t frame) {
  vkr_assert(playback != NULL && key < playback->pose_key_count);
  const VkrRecordingPoseKey *pose_key = &playback->pose_keys[key];
  /* The first sample past `frame`, then the one before it. */
  uint32_t low = pose_key->first;
  uint32_t high = pose_key->first + pose_key->count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (playback->pose_samples[middle].frame <= frame) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low > pose_key->first ? low - 1u : UINT32_MAX;
}

uint64_t vkr_recording_playback_frame_at(const VkrRecordingPlayback *playback,
                                         float64_t time) {
  vkr_assert(playback != NULL && playback->open);
  /* The last frame whose time is at or before `time`. */
  uint64_t low = 0u;
  uint64_t high = playback->frame_count;
  while (high - low > 1u) {
    const uint64_t middle = low + (high - low) / 2u;
    if (playback->times[middle] <= time) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

/* `to` minus `from` taken the short way round a circle of `period`. */
vkr_internal float32_t vkr_recording_wrap_delta(float32_t from, float32_t to,
                                                float32_t period) {
  float32_t delta = to - from;
  while (delta > period * 0.5f) {
    delta -= period;
  }
  while (delta < -period * 0.5f) {
    delta += period;
  }
  return delta;
}

VkrRecordingPlaybackPose
vkr_recording_playback_pose(const VkrRecordingPlayback *playback) {
  vkr_assert(playback != NULL && playback->open);
  const uint64_t frame =
      vkr_recording_playback_frame_at(playback, playback->time);
  const uint64_t next = Min(frame + 1u, playback->frame_count - 1u);
  const float64_t span = playback->times[next] - playback->times[frame];
  const float64_t t =
      span > 0.0 ? Min((playback->time - playback->times[frame]) / span, 1.0)
                 : 0.0;
  const float32_t tf = (float32_t)t;
  const VkrRecordingCamera *a = &playback->cameras[frame];
  const VkrRecordingCamera *b = &playback->cameras[next];

  VkrRecordingPlaybackPose pose = {.camera = *a, .blend = tf, .frame = frame};
  for (uint32_t i = 0u; i < 3u; ++i) {
    pose.camera.position[i] =
        a->position[i] + (b->position[i] - a->position[i]) * t;
  }
  pose.camera.yaw =
      a->yaw + vkr_recording_wrap_delta(a->yaw, b->yaw, 360.0f) * tf;
  pose.camera.pitch = a->pitch + (b->pitch - a->pitch) * tf;
  pose.camera.fov = a->fov + (b->fov - a->fov) * tf;
  const float32_t hour_a = playback->frames[frame].time_of_day;
  const float32_t hour_b = playback->frames[next].time_of_day;
  float32_t hour =
      hour_a + vkr_recording_wrap_delta(hour_a, hour_b, 24.0f) * tf;
  if (hour < 0.0f) {
    hour += 24.0f;
  } else if (hour >= 24.0f) {
    hour -= 24.0f;
  }
  pose.time_of_day = hour;
  /* Clouds drift centimetres a frame, so the frame's own offset serves;
     interpolating would also have to wrap at the weather period. */
  if (playback->cloud_valid[frame]) {
    pose.has_clouds = true_v;
    pose.cloud_wind_offset_m[0] = playback->cloud_offsets[frame * 2u];
    pose.cloud_wind_offset_m[1] = playback->cloud_offsets[frame * 2u + 1u];
  }
  return pose;
}
