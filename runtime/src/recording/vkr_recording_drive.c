#include "recording/vkr_recording_drive.h"

#include "assets/vkr_recording_format.h"
#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "math/vkr_math.h"
#include "memory/vkr_dmemory_allocator.h"

#define VKR_RECORDING_DRIVE_MEMORY_COMMIT (1ull * 1024ull * 1024ull)
#define VKR_RECORDING_DRIVE_MEMORY_RESERVE (1024ull * 1024ull * 1024ull)

typedef struct VkrRecordingDriveLoad {
  VkrRecordingDrive *drive;
  const VkrRecordingReader *reader;
  const VkrCodec *codec;
  bool8_t counting;
  /* Session records seen in this pass. */
  uint32_t sessions;
} VkrRecordingDriveLoad;

/* A hash_module record: the name of module index `index`. */
vkr_internal void
vkr_recording_drive_name_module(VkrRecordingDrive *drive,
                                const VkrRecordingRecord *record) {
  uint32_t index = 0u;
  MemCopy(&index, record->data, sizeof(index));
  if (index >= VKR_RECORDING_HASH_MODULE_MAX) {
    return;
  }
  const uint32_t length =
      Min(record->size - 4u, (uint32_t)VKR_RECORDING_HASH_MODULE_NAME_MAX);
  MemCopy(drive->module_names[index], record->data + 4u, length);
  drive->module_names[index][length] = '\0';
  drive->module_name_count = Max(drive->module_name_count, index + 1u);
}

/* Counts, then copies, the action and player records of every readable
   records chunk. */
vkr_internal bool8_t vkr_recording_drive_scan(VkrRecordingDriveLoad *load) {
  VkrRecordingDrive *drive = load->drive;
  for (uint32_t i = 0u; i < load->reader->chunk_count; ++i) {
    if (load->reader->chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(load->reader, i, load->codec,
                                    &drive->allocator, &payload, &size)) {
      return false_v;
    }
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      if (record.stream == VKR_RECORDING_STREAM_ACTION &&
          record.size == sizeof(VkrRecordingAction)) {
        if (!load->counting) {
          MemCopy(&drive->actions[drive->action_count], record.data,
                  sizeof(VkrRecordingAction));
        }
        ++drive->action_count;
      } else if (record.stream == VKR_RECORDING_STREAM_SESSION &&
                 record.size == sizeof(VkrRecordingSession)) {
        /* Only the first session re-simulates; a restart ends it. */
        if (++load->sessions == 1u && !load->counting) {
          VkrRecordingSession session;
          MemCopy(&session, record.data, sizeof(session));
          drive->seed = session.seed;
        }
        drive->has_session = true_v;
      } else if (load->sessions == 1u &&
                 record.stream == VKR_RECORDING_STREAM_SIM_FRAME &&
                 record.size == sizeof(VkrRecordingSimFrame)) {
        if (!load->counting) {
          MemCopy(&drive->sim_frames[drive->sim_frame_count], record.data,
                  sizeof(VkrRecordingSimFrame));
        }
        ++drive->sim_frame_count;
      } else if (load->sessions == 1u &&
                 record.stream == VKR_RECORDING_STREAM_TICK_HASH &&
                 record.size == sizeof(VkrRecordingTickHash)) {
        if (!load->counting) {
          MemCopy(&drive->hashes[drive->hash_count], record.data,
                  sizeof(VkrRecordingTickHash));
          drive->hash_frames[drive->hash_count] = record.frame;
        }
        ++drive->hash_count;
      } else if (load->sessions == 1u && !load->counting &&
                 record.stream == VKR_RECORDING_STREAM_HASH_MODULE &&
                 record.size > 4u) {
        vkr_recording_drive_name_module(drive, &record);
      } else if (load->sessions == 1u && !load->counting &&
                 record.stream == VKR_RECORDING_STREAM_CELLS &&
                 record.size >= 4u && !drive->has_start_cells) {
        uint32_t count = 0u;
        MemCopy(&count, record.data, sizeof(count));
        count = Min(count, (record.size - 4u) / (2u * sizeof(int32_t)));
        count = Min(count, VKR_RECORDING_CELLS_MAX);
        MemCopy(drive->start_cells, record.data + 4u,
                sizeof(int32_t) * 2u * count);
        drive->start_cell_count = count;
        drive->has_start_cells = true_v;
      } else if (load->sessions == 1u && !load->counting &&
                 record.stream == VKR_RECORDING_STREAM_JOIN &&
                 record.size == sizeof(VkrRecordingJoin)) {
        if (drive->join_count < VKR_RECORDING_DRIVE_JOIN_MAX) {
          MemCopy(&drive->joins[drive->join_count++], record.data,
                  sizeof(VkrRecordingJoin));
        } else {
          drive->joins_cut = true_v;
        }
      } else if (record.stream == VKR_RECORDING_STREAM_PLAYER &&
                 record.size == sizeof(VkrRecordingPlayer)) {
        if (!load->counting) {
          MemCopy(&drive->players[drive->player_count], record.data,
                  sizeof(VkrRecordingPlayer));
          drive->player_frames[drive->player_count] = record.frame;
        }
        ++drive->player_count;
      }
    }
    vkr_recording_reader_chunk_free(&drive->allocator, payload, size);
  }
  return true_v;
}

vkr_internal int32_t vkr_recording_compare_actions(const void *lhs,
                                                   const void *rhs) {
  const VkrRecordingAction *a = lhs;
  const VkrRecordingAction *b = rhs;
  if (a->tick != b->tick) {
    return a->tick < b->tick ? -1 : 1;
  }
  return a->sequence < b->sequence ? -1 : (a->sequence > b->sequence ? 1 : 0);
}

vkr_internal bool8_t vkr_recording_drive_load(VkrRecordingDrive *drive,
                                              const VkrRecordingReader *reader,
                                              const VkrCodec *codec,
                                              char *error,
                                              uint64_t error_capacity) {
  VkrRecordingDriveLoad load = {
      .drive = drive, .reader = reader, .codec = codec, .counting = true_v};
  if (!vkr_recording_drive_scan(&load)) {
    string_format(error, error_capacity, "a records chunk is damaged");
    return false_v;
  }
  /* A session with tick hashes re-simulates without input, such as an
     idle one; Drive itself needs actions and a player track, which the
     caller checks. */
  if ((drive->action_count == 0u || drive->player_count == 0u) &&
      (!drive->has_session || drive->hash_count == 0u)) {
    string_format(error, error_capacity,
                  "the recording has no %s to drive with",
                  drive->action_count == 0u ? "actions" : "player track");
    return false_v;
  }
  const uint64_t actions = drive->action_count;
  const uint64_t players = drive->player_count;
  drive->actions = vkr_allocator_alloc(
      &drive->allocator, sizeof(VkrRecordingAction) * (actions + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  drive->players = vkr_allocator_alloc(
      &drive->allocator, sizeof(VkrRecordingPlayer) * (players + 1u),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  drive->player_frames =
      vkr_allocator_alloc(&drive->allocator, sizeof(uint64_t) * (players + 1u),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!drive->actions || !drive->players || !drive->player_frames) {
    string_format(error, error_capacity, "out of memory");
    return false_v;
  }
  if (drive->sim_frame_count > 0u || drive->hash_count > 0u) {
    drive->sim_frames = vkr_allocator_alloc(&drive->allocator,
                                            sizeof(VkrRecordingSimFrame) *
                                                (drive->sim_frame_count + 1u),
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    drive->hashes = vkr_allocator_alloc(&drive->allocator,
                                        sizeof(VkrRecordingTickHash) *
                                            (drive->hash_count + 1u),
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    drive->hash_frames = vkr_allocator_alloc(
        &drive->allocator, sizeof(uint64_t) * (drive->hash_count + 1u),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!drive->sim_frames || !drive->hashes || !drive->hash_frames) {
      string_format(error, error_capacity, "out of memory");
      return false_v;
    }
  }
  drive->action_count = 0u;
  drive->player_count = 0u;
  drive->sim_frame_count = 0u;
  drive->hash_count = 0u;
  load.sessions = 0u;
  load.counting = false_v;
  if (!vkr_recording_drive_scan(&load)) {
    string_format(error, error_capacity, "a records chunk is damaged");
    return false_v;
  }
  vkr_sort(drive->actions, drive->action_count, sizeof(VkrRecordingAction),
           vkr_recording_compare_actions);
  /* Player records arrive in tick order within a run; a session without
     either spans its hashed ticks. */
  drive->first_tick = UINT64_MAX;
  drive->last_tick = 0u;
  if (drive->action_count > 0u) {
    drive->first_tick = drive->actions[0].tick;
    drive->last_tick = drive->actions[drive->action_count - 1u].tick;
  }
  if (drive->player_count > 0u) {
    drive->first_tick = Min(drive->first_tick, drive->players[0].tick);
    drive->last_tick =
        Max(drive->last_tick, drive->players[drive->player_count - 1u].tick);
  }
  if (drive->first_tick == UINT64_MAX) {
    drive->first_tick = drive->hashes[0].tick;
    drive->last_tick = drive->hashes[drive->hash_count - 1u].tick;
  }
  return true_v;
}

bool8_t vkr_recording_drive_open(VkrRecordingDrive *drive, const char *path,
                                 const VkrCodec *codec, char *error,
                                 uint64_t error_capacity) {
  vkr_assert(drive != NULL && path != NULL && error != NULL);
  vkr_assert(!drive->open && "close the open drive first");
  MemZero(drive, sizeof(*drive));
  if (!vkr_dmemory_create(VKR_RECORDING_DRIVE_MEMORY_COMMIT,
                          VKR_RECORDING_DRIVE_MEMORY_RESERVE, &drive->memory)) {
    string_format(error, error_capacity, "out of memory");
    return false_v;
  }
  drive->allocator = (VkrAllocator){.ctx = &drive->memory};
  vkr_dmemory_allocator_create(&drive->allocator);
  string_format(drive->bundle_path, sizeof(drive->bundle_path), "%s", path);

  /* A bundle directory holds the stream file; a file path is used as is. */
  char stream[VKR_RECORDING_PATH_MAX];
  string_format(stream, sizeof(stream), "%s/%s", path,
                VKR_RECORDING_STREAM_FILE);
  VkrRecordingReader reader;
  VkrRecordingReadError read_error =
      vkr_recording_reader_open(&reader, &drive->allocator, stream);
  if (read_error == VKR_RECORDING_READ_OPEN) {
    read_error = vkr_recording_reader_open(&reader, &drive->allocator, path);
  }
  if (read_error != VKR_RECORDING_READ_OK) {
    string_format(error, error_capacity, "'%s' is not a readable recording",
                  path);
    vkr_dmemory_allocator_destroy(&drive->allocator);
    MemZero(drive, sizeof(*drive));
    return false_v;
  }
  const bool8_t loaded =
      vkr_recording_drive_load(drive, &reader, codec, error, error_capacity);
  vkr_recording_reader_close(&reader);
  if (!loaded) {
    vkr_dmemory_allocator_destroy(&drive->allocator);
    MemZero(drive, sizeof(*drive));
    return false_v;
  }
  drive->base_tick = UINT64_MAX;
  drive->divergent_tick = UINT64_MAX;
  drive->divergent_frame = UINT64_MAX;
  drive->hash_divergent_tick = UINT64_MAX;
  drive->hash_divergent_frame = UINT64_MAX;
  drive->open = true_v;
  return true_v;
}

void vkr_recording_drive_close(VkrRecordingDrive *drive) {
  vkr_assert(drive != NULL);
  if (drive->open) {
    vkr_dmemory_allocator_destroy(&drive->allocator);
  }
  MemZero(drive, sizeof(*drive));
}

/* The recorded tick driven tick `tick` replays; UINT64_MAX before the
   base. */
vkr_internal uint64_t
vkr_recording_drive_recorded(const VkrRecordingDrive *drive, uint64_t tick) {
  if (drive->base_tick == UINT64_MAX || tick < drive->base_tick) {
    return UINT64_MAX;
  }
  return drive->first_tick + (tick - drive->base_tick);
}

uint32_t vkr_recording_drive_actions(VkrRecordingDrive *drive, uint64_t tick,
                                     VkrRecordingAction *out,
                                     uint32_t capacity) {
  vkr_assert(drive != NULL && drive->open && (out != NULL || capacity == 0u));
  if (drive->base_tick == UINT64_MAX) {
    drive->base_tick = tick;
  }
  const uint64_t recorded = vkr_recording_drive_recorded(drive, tick);
  if (recorded == UINT64_MAX) {
    return 0u;
  }
  drive->finished = recorded >= drive->last_tick;
  drive->ticks_driven = tick - drive->base_tick + 1u;
  /* Actions of ticks the run skipped are not replayed late. */
  while (drive->action_cursor < drive->action_count &&
         drive->actions[drive->action_cursor].tick < recorded) {
    ++drive->action_cursor;
  }
  uint32_t written = 0u;
  while (written < capacity && drive->action_cursor < drive->action_count &&
         drive->actions[drive->action_cursor].tick == recorded) {
    out[written++] = drive->actions[drive->action_cursor++];
  }
  return written;
}

/* The index of the recorded player at tick `recorded`, or UINT64_MAX. */
vkr_internal uint64_t vkr_recording_drive_find_player(
    const VkrRecordingDrive *drive, uint64_t recorded) {
  uint64_t low = 0u;
  uint64_t high = drive->player_count;
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2u;
    if (drive->players[middle].tick < recorded) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low < drive->player_count && drive->players[low].tick == recorded
             ? low
             : UINT64_MAX;
}

const VkrRecordingPlayer *
vkr_recording_drive_start_player(const VkrRecordingDrive *drive) {
  vkr_assert(drive != NULL && drive->open);
  return &drive->players[0];
}

const VkrRecordingPlayer *
vkr_recording_drive_reference(const VkrRecordingDrive *drive, uint64_t tick) {
  vkr_assert(drive != NULL && drive->open);
  const uint64_t recorded = vkr_recording_drive_recorded(drive, tick);
  const uint64_t index = recorded == UINT64_MAX
                             ? UINT64_MAX
                             : vkr_recording_drive_find_player(drive, recorded);
  return index == UINT64_MAX ? NULL : &drive->players[index];
}

void vkr_recording_drive_player(VkrRecordingDrive *drive, uint64_t tick,
                                const float64_t position[3]) {
  vkr_assert(drive != NULL && drive->open && position != NULL);
  const uint64_t recorded = vkr_recording_drive_recorded(drive, tick);
  const uint64_t index = recorded == UINT64_MAX
                             ? UINT64_MAX
                             : vkr_recording_drive_find_player(drive, recorded);
  if (drive->has_position) {
    float64_t step = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      const float64_t delta = position[axis] - drive->last_position[axis];
      step += delta * delta;
    }
    drive->path_length += vkr_sqrt_f64(step);
  }
  MemCopy(drive->last_position, position, sizeof(drive->last_position));
  drive->has_position = true_v;
  if (index == UINT64_MAX) {
    return;
  }
  const VkrRecordingPlayer *player = &drive->players[index];
  float64_t squared = 0.0;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float64_t delta = position[axis] - player->position[axis];
    squared += delta * delta;
  }
  drive->distance = vkr_sqrt_f64(squared);
  drive->max_distance = Max(drive->max_distance, drive->distance);
  ++drive->ticks_compared;
  if (drive->divergent_tick == UINT64_MAX &&
      drive->distance > VKR_RECORDING_DRIVE_TOLERANCE) {
    drive->divergent_tick = recorded;
    drive->divergent_frame = drive->player_frames[index];
  }
}

bool8_t vkr_recording_drive_peek_sim_frame(const VkrRecordingDrive *drive,
                                           VkrRecordingSimFrame *out) {
  vkr_assert(drive != NULL && out != NULL);
  if (drive->sim_cursor >= drive->sim_frame_count) {
    return false_v;
  }
  *out = drive->sim_frames[drive->sim_cursor];
  return true_v;
}

bool8_t vkr_recording_drive_sim_frame(VkrRecordingDrive *drive,
                                      VkrRecordingSimFrame *out) {
  vkr_assert(drive != NULL && drive->open && out != NULL);
  if (drive->sim_cursor >= drive->sim_frame_count) {
    drive->finished = true_v;
    return false_v;
  }
  *out = drive->sim_frames[drive->sim_cursor++];
  return true_v;
}

/* Appends `text` to the divergence's subsystem list. */
vkr_internal void vkr_recording_drive_differs(VkrRecordingDrive *drive,
                                              const char *text) {
  const uint64_t length = string_length(drive->hash_subsystems);
  string_format(drive->hash_subsystems + length,
                sizeof(drive->hash_subsystems) - length, "%s%s",
                length ? ", " : "", text);
}

/* The recorded hash of module `name` in `recorded`, or false when the
   recording has no such module. */
vkr_internal bool8_t vkr_recording_drive_module_hash(
    const VkrRecordingDrive *drive, const VkrRecordingTickHash *recorded,
    const char *name, uint32_t *out) {
  for (uint32_t i = 0u;
       i < recorded->module_count && i < drive->module_name_count; ++i) {
    if (string_equals(drive->module_names[i], name)) {
      *out = recorded->modules[i];
      return true_v;
    }
  }
  return false_v;
}

/* The next recorded result `kind` of `entity` not yet applied, or
   UINT32_MAX: a wrapper given a second model matches its second join. */
vkr_internal uint32_t vkr_recording_drive_find_join(
    const VkrRecordingDrive *drive, uint32_t kind, uint64_t entity) {
  for (uint32_t i = 0u; i < drive->join_count; ++i) {
    if (!drive->join_applied[i] && drive->joins[i].kind == kind &&
        drive->joins[i].entity == entity) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool8_t vkr_recording_drive_may_join(const VkrRecordingDrive *drive,
                                     uint32_t kind, uint64_t entity,
                                     uint64_t ticks) {
  vkr_assert(drive != NULL);
  const uint32_t index = vkr_recording_drive_find_join(drive, kind, entity);
  if (index == UINT32_MAX) {
    return drive->joins_cut;
  }
  return drive->joins[index].ticks <= ticks;
}

void vkr_recording_drive_joined(VkrRecordingDrive *drive, uint32_t kind,
                                uint64_t entity) {
  vkr_assert(drive != NULL);
  const uint32_t index = vkr_recording_drive_find_join(drive, kind, entity);
  if (index != UINT32_MAX) {
    drive->join_applied[index] = true_v;
  }
}

bool8_t vkr_recording_drive_check_hash(VkrRecordingDrive *drive,
                                       const VkrRecordingTickHash *live,
                                       const char *const *module_names) {
  vkr_assert(drive != NULL && drive->open && live != NULL);
  if (drive->hash_diverged) {
    return false_v;
  }
  while (drive->hash_cursor < drive->hash_count &&
         drive->hashes[drive->hash_cursor].tick < live->tick) {
    ++drive->hash_cursor;
  }
  if (drive->hash_cursor >= drive->hash_count ||
      drive->hashes[drive->hash_cursor].tick != live->tick) {
    return true_v;
  }
  const VkrRecordingTickHash *recorded = &drive->hashes[drive->hash_cursor];
  /* Script state first: a module that drew a different number or kept
     different data is the likely cause of what moved after it. */
  for (uint32_t i = 0u; i < live->module_count; ++i) {
    uint32_t hash = 0u;
    char text[96];
    if (!vkr_recording_drive_module_hash(drive, recorded, module_names[i],
                                         &hash)) {
      string_format(text, sizeof(text), "module %s (not in the recording)",
                    module_names[i]);
      vkr_recording_drive_differs(drive, text);
    } else if (hash != live->modules[i]) {
      string_format(text, sizeof(text), "module %s", module_names[i]);
      vkr_recording_drive_differs(drive, text);
    }
  }
  if (recorded->bodies != live->bodies) {
    vkr_recording_drive_differs(drive, "physics bodies");
  }
  for (uint32_t b = 0u; b < VKR_RECORDING_HASH_BUCKETS; ++b) {
    if (recorded->transforms[b] != live->transforms[b]) {
      drive->hash_buckets |= 1u << b;
    }
  }
  if (drive->hash_buckets) {
    vkr_recording_drive_differs(drive, "transforms");
  }
  if (drive->hash_subsystems[0] == '\0') {
    ++drive->ticks_hashed;
    return true_v;
  }
  drive->hash_diverged = true_v;
  drive->hash_divergent_tick = live->tick;
  drive->hash_divergent_frame = drive->hash_frames[drive->hash_cursor];
  return false_v;
}
