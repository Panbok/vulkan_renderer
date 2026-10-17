#include "recording/vkr_recording_playback.h"

#include "assets/vkr_recording_format.h"
#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "memory/vkr_dmemory_allocator.h"

#define VKR_RECORDING_PLAYBACK_MEMORY_COMMIT (1ull * 1024ull * 1024ull)
#define VKR_RECORDING_PLAYBACK_MEMORY_RESERVE                                  \
  (2ull * 1024ull * 1024ull * 1024ull)

typedef struct VkrRecordingPoseLoad {
  VkrRecordingPose pose;
  uint64_t frame;
} VkrRecordingPoseLoad;

typedef struct VkrRecordingLoad {
  VkrRecordingPlayback *playback;
  const VkrRecordingReader *reader;
  const VkrCodec *codec;
  /* Which frames had a frame and a camera record. */
  uint8_t *present;
  /* Poses as recorded, with their frame, before they are grouped. */
  VkrRecordingPoseLoad *poses;
  char *error;
  uint64_t error_capacity;
} VkrRecordingLoad;

vkr_internal bool8_t vkr_recording_load_fail(VkrRecordingLoad *load,
                                             const char *reason) {
  string_format(load->error, load->error_capacity, "%s", reason);
  return false_v;
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
  for (uint32_t i = 0u; i < load->reader->chunk_count; ++i) {
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

vkr_internal bool8_t vkr_recording_count(VkrRecordingLoad *load,
                                         const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
  if (record->stream == VKR_RECORDING_STREAM_FRAME) {
    playback->frame_count = Max(playback->frame_count, record->frame + 1u);
  } else if (record->stream == VKR_RECORDING_STREAM_MARKER &&
             record->size >= 4u) {
    ++playback->marker_count;
  } else if (record->stream == VKR_RECORDING_STREAM_POSE &&
             record->size % sizeof(VkrRecordingPose) == 0u) {
    playback->pose_sample_count += record->size / sizeof(VkrRecordingPose);
  }
  return true_v;
}

vkr_internal bool8_t vkr_recording_fill(VkrRecordingLoad *load,
                                        const VkrRecordingRecord *record) {
  VkrRecordingPlayback *playback = load->playback;
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
  } else if (record->stream == VKR_RECORDING_STREAM_POSE &&
             record->size % sizeof(VkrRecordingPose) == 0u) {
    const uint32_t count = record->size / (uint32_t)sizeof(VkrRecordingPose);
    for (uint32_t i = 0u; i < count; ++i) {
      VkrRecordingPoseLoad *pose = &load->poses[playback->pose_sample_count++];
      MemCopy(&pose->pose, record->data + i * sizeof(VkrRecordingPose),
              sizeof(VkrRecordingPose));
      pose->frame = record->frame;
    }
  }
  return true_v;
}

/* Container, then ref, then node, then document, then frame. */
vkr_internal int32_t vkr_recording_compare_poses(const void *lhs,
                                                 const void *rhs) {
  const VkrRecordingPoseLoad *a = lhs;
  const VkrRecordingPoseLoad *b = rhs;
  if (a->pose.container != b->pose.container) {
    return a->pose.container < b->pose.container ? -1 : 1;
  }
  const int32_t ref =
      MemCompare(&a->pose.ref, &b->pose.ref, sizeof(a->pose.ref));
  if (ref != 0) {
    return ref;
  }
  if (a->pose.node != b->pose.node) {
    return a->pose.node < b->pose.node ? -1 : 1;
  }
  if (a->pose.document != b->pose.document) {
    return a->pose.document < b->pose.document ? -1 : 1;
  }
  return a->frame < b->frame ? -1 : (a->frame > b->frame ? 1 : 0);
}

vkr_internal bool8_t vkr_recording_pose_same_key(const VkrRecordingPose *a,
                                                 const VkrRecordingPose *b) {
  return a->container == b->container && a->node == b->node &&
         a->document == b->document &&
         MemCompare(&a->ref, &b->ref, sizeof(a->ref)) == 0;
}

/* Groups the loaded poses into keys with their samples by frame. */
vkr_internal bool8_t
vkr_recording_playback_group_poses(VkrRecordingLoad *load) {
  VkrRecordingPlayback *playback = load->playback;
  const uint64_t count = playback->pose_sample_count;
  if (count == 0u) {
    return true_v;
  }
  vkr_sort(load->poses, count, sizeof(VkrRecordingPoseLoad),
           vkr_recording_compare_poses);
  uint64_t keys = 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    keys += i == 0u || !vkr_recording_pose_same_key(&load->poses[i].pose,
                                                    &load->poses[i - 1u].pose)
                ? 1u
                : 0u;
  }
  playback->pose_samples = vkr_allocator_alloc(
      &playback->allocator, sizeof(VkrRecordingPoseSample) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  playback->pose_keys = vkr_allocator_alloc(&playback->allocator,
                                            sizeof(VkrRecordingPoseKey) * keys,
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!playback->pose_samples || !playback->pose_keys) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  uint32_t key = UINT32_MAX;
  for (uint64_t i = 0u; i < count; ++i) {
    const VkrRecordingPoseLoad *loaded = &load->poses[i];
    if (i == 0u || !vkr_recording_pose_same_key(&loaded->pose,
                                                &load->poses[i - 1u].pose)) {
      key = key == UINT32_MAX ? 0u : key + 1u;
      playback->pose_keys[key] = (VkrRecordingPoseKey){
          .ref = loaded->pose.ref,
          .node = loaded->pose.node,
          .document = loaded->pose.document,
          .container = loaded->pose.container,
          .first = (uint32_t)i,
      };
    }
    ++playback->pose_keys[key].count;
    VkrRecordingPoseSample *sample = &playback->pose_samples[i];
    sample->frame = loaded->frame;
    sample->key = key;
    MemCopy(sample->translation, loaded->pose.translation,
            sizeof(sample->translation));
    MemCopy(sample->linear, loaded->pose.linear, sizeof(sample->linear));
  }
  playback->pose_key_count = (uint32_t)keys;
  return true_v;
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
  if (!playback->frames || !playback->cameras || !playback->times ||
      !playback->markers) {
    return false_v;
  }
  MemZero(playback->frames, sizeof(VkrRecordingFrame) * frames);
  MemZero(playback->cameras, sizeof(VkrRecordingCamera) * frames);
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
  if (!vkr_recording_playback_allocate(playback)) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  load->present =
      vkr_allocator_alloc(&playback->allocator, playback->frame_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!load->present) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  MemZero(load->present, playback->frame_count);
  /* Sample indices are u32, two values short of their maximum. */
  if (playback->pose_sample_count >= UINT32_MAX - 2u) {
    return vkr_recording_load_fail(load, "the recording holds too many poses");
  }
  load->poses = vkr_allocator_alloc(&playback->allocator,
                                    sizeof(VkrRecordingPoseLoad) *
                                        (playback->pose_sample_count + 1u),
                                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!load->poses) {
    return vkr_recording_load_fail(load, "out of memory");
  }
  playback->marker_count = 0u;
  playback->pose_sample_count = 0u;
  if (!vkr_recording_load_records(load, vkr_recording_fill) ||
      !vkr_recording_playback_group_poses(load)) {
    return false_v;
  }
  vkr_recording_playback_finish(load);
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
  if (time >= playback->duration) {
    if (playback->loop && playback->duration > 0.0) {
      time -=
          playback->duration * (float64_t)(uint64_t)(time / playback->duration);
    } else {
      time = playback->duration;
      playback->playing = false_v;
    }
  }
  playback->time = Max(time, 0.0);
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

  VkrRecordingPlaybackPose pose = {.camera = *a, .frame = frame};
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
  return pose;
}
