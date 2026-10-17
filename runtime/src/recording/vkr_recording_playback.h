#pragma once

#include "core/vkr_codec.h"
#include "defines.h"
#include "memory/vkr_dmemory.h"
#include "recording/vkr_recording.h"

/*
 * Watch playback (docs/proposals/recordings.md, "Watch owns the world"): a
 * recording's frames and cameras loaded into memory, and a playhead on the
 * recorded clock. Frame 0 is at time zero and each later frame follows its
 * display_dt. Between two frames the camera is interpolated, so playback at
 * another frame rate or speed stays smooth; at a frame's time it is exactly
 * the recorded camera.
 */

/* One recorded pose of one entity. */
typedef struct VkrRecordingPoseSample {
  uint64_t frame;
  float64_t translation[3];
  float32_t linear[9];
  uint32_t key;
} VkrRecordingPoseSample;

/* An entity the recording posed, named as in VkrRecordingPose; its samples
   are pose_samples[first, first + count), by frame. */
typedef struct VkrRecordingPoseKey {
  VkrEntityRef ref;
  uint32_t node;
  uint32_t document;
  uint8_t container;
  /* VKR_RECORDING_POSE_KIND_* */
  uint8_t kind;
  uint32_t first;
  uint32_t count;
} VkrRecordingPoseKey;

/* A script spawn the recording named: the frames it lived in and the model
   it got, its paths in VkrRecordingPlayback.spawn_text. */
typedef struct VkrRecordingSpawn {
  uint64_t first_frame;
  /* The frame it went away in, or UINT64_MAX when it lived to the end. */
  uint64_t last_frame;
  uint8_t container;
  bool8_t seen;
  uint32_t mesh_offset;
  uint32_t mesh_length;
  uint32_t animation_offset;
  uint32_t animation_length;
  /* A spawn without a model may have a shape. */
  bool8_t has_shape;
  VkrRecordingSpawnShape shape;
} VkrRecordingSpawn;

/* VkrRecordingPlayback.pose_applied: no sample applied, as before the
   entity's first recorded pose (vkr_recording_playback_pose_at), or a key
   whose only sample matched the scene when it was bound, which needs none. */
#define VKR_RECORDING_POSE_APPLIED_NONE UINT32_MAX
#define VKR_RECORDING_POSE_APPLIED_STATIC (UINT32_MAX - 1u)

/* Event names a playback keeps; events of later names are dropped. */
#define VKR_RECORDING_PLAYBACK_EVENT_NAMES 32u

/* A record of stream "event" (vkr_recording_format.h): an engine event or a
   script's, by frame, its name's index in event_names. */
typedef struct VkrRecordingPlaybackEvent {
  uint64_t frame;
  uint32_t name;
  uint32_t count;
  /* Milliseconds the events took, zero when not measured. */
  float32_t ms;
  bool8_t engine;
} VkrRecordingPlaybackEvent;

typedef struct VkrRecordingPlayback {
  bool8_t open;
  VkrDMemory memory;
  VkrAllocator allocator;
  char bundle_path[VKR_RECORDING_PATH_MAX];
  /* Frames 0 to frame_count - 1. A frame whose records were lost repeats the
     frame before it. */
  uint64_t frame_count;
  VkrRecordingFrame *frames;
  VkrRecordingCamera *cameras;
  /* Recorded time of each frame, in seconds from frame 0. */
  float64_t *times;
  VkrRecordingMarker *markers;
  uint32_t marker_count;
  /* Events by frame, and their names in first-seen order. */
  VkrRecordingPlaybackEvent *events;
  uint32_t event_count;
  char event_names[VKR_RECORDING_PLAYBACK_EVENT_NAMES]
                  [VKR_RECORDING_TEXT_MAX + 1u];
  uint32_t event_name_count;
  /* Frames that had no records and repeat the one before. */
  uint64_t missing_frames;
  /* Memory of each frame from its metrics (Tier 0): the process's resident
     bytes and the GPU driver's allocated bytes, two per frame, where bit 0
     and bit 1 of memory_valid say each was sampled. has_memory says the
     recording declares either. */
  bool8_t has_memory;
  float64_t *memory_bytes;
  uint8_t *memory_valid;
  /* The player of each frame's last tick (vkr_rec_player): three per frame,
     where player_valid is 1; has_player says any frame has one. */
  bool8_t has_player;
  float64_t *player_positions;
  uint8_t *player_valid;
  /* The stream ends without a clean stop. */
  bool8_t truncated;

  /* Where the edit journals stood (VKR_RECORDING_STREAM_EDIT), by frame;
     the first is the recording's start. */
  uint64_t *edit_frames;
  uint64_t *edit_sequences;
  uint32_t edit_count;

  /* Script spawns by spawn id, and the text their paths point into. */
  VkrRecordingSpawn *spawns;
  uint32_t spawn_count;
  char *spawn_text;
  uint64_t spawn_text_bytes;
  uint64_t spawn_text_capacity;
  /* The runtime's stand-ins for spawns with a model, VkrEntityId values by
     spawn id, and whether each shows. */
  uint64_t *spawn_proxies;
  uint8_t *spawn_shown;
  /* The node keys of each spawn: spawn_node_keys[spawn_node_first[id] ..
     spawn_node_first[id + 1]), and the frame whose node pose the animated
     stand-ins last took, UINT64_MAX for none. */
  uint32_t *spawn_node_first;
  uint32_t *spawn_node_keys;
  /* Each spawn's own pose key, UINT32_MAX for none. */
  uint32_t *spawn_self_key;
  uint64_t spawn_pose_frame;

  /* Recorded poses, grouped by key and ordered by frame. */
  VkrRecordingPoseSample *pose_samples;
  uint64_t pose_sample_count;
  VkrRecordingPoseKey *pose_keys;
  uint32_t pose_key_count;
  /* The runtime's binding of the keys to live entities
     (vkr_standard_scene_recording.c): VkrEntityId values, invalid for a key
     the scene does not hold, and the sample each key last applied. */
  bool8_t poses_bound;
  /* Some spawn keys wait for their stand-in's model to load. */
  bool8_t poses_pending;
  uint64_t *pose_entities;
  uint32_t *pose_applied;

  float64_t duration;
  /* The playhead, in [0, duration]. */
  float64_t time;
  float32_t speed;
  bool8_t playing;
  bool8_t loop;
  /* With `loop`, the times it loops between; loop_end at or below
     loop_start loops the whole recording. */
  float64_t loop_start;
  float64_t loop_end;
} VkrRecordingPlayback;

typedef struct VkrRecordingPlaybackPose {
  VkrRecordingCamera camera;
  float32_t time_of_day;
  /* The recorded frame at or before the playhead. */
  uint64_t frame;
} VkrRecordingPlaybackPose;

/**
 * @brief Loads a bundle directory or a stream file.
 * @param codec Decodes compressed chunks; the runtime's zstd decoder.
 * @param error Receives a one-line reason on failure.
 */
bool8_t vkr_recording_playback_open(VkrRecordingPlayback *playback,
                                    const char *path, const VkrCodec *codec,
                                    char *error, uint64_t error_capacity);

void vkr_recording_playback_close(VkrRecordingPlayback *playback);

/** Loops between recorded times `start` and `end`, clamped to the
    recording; an empty span loops the whole recording. */
void vkr_recording_playback_set_loop_range(VkrRecordingPlayback *playback,
                                           float64_t start, float64_t end);

/** Moves a playing playhead by `seconds` of display time times the speed. */
void vkr_recording_playback_advance(VkrRecordingPlayback *playback,
                                    float64_t seconds);

/** Places the playhead at a recorded time, clamped to the recording. */
void vkr_recording_playback_seek_time(VkrRecordingPlayback *playback,
                                      float64_t time);

/** Places the playhead exactly on a frame, clamped to the recording. */
void vkr_recording_playback_seek_frame(VkrRecordingPlayback *playback,
                                       uint64_t frame);

/** The frame at or before a recorded time. */
uint64_t vkr_recording_playback_frame_at(const VkrRecordingPlayback *playback,
                                         float64_t time);

/** The last sample of `key` at or before `frame`, or UINT32_MAX when the
    entity had not moved yet. */
uint32_t vkr_recording_playback_pose_at(const VkrRecordingPlayback *playback,
                                        uint32_t key, uint64_t frame);

/** The mesh and animation paths of spawn `id`'s model, NUL-terminated;
    false_v for a spawn without one. */
bool8_t vkr_recording_playback_spawn_model(const VkrRecordingPlayback *playback,
                                           uint32_t id, String8 *out_mesh,
                                           String8 *out_animation);

/** Where the edit journals stood at `frame`: the last edit record at or
    before it, else the start's. False for a recording without edit records.
 */
bool8_t vkr_recording_playback_edit_at(const VkrRecordingPlayback *playback,
                                       uint64_t frame, uint64_t *out_sequence);

/** The camera and time of day at the playhead. */
VkrRecordingPlaybackPose
vkr_recording_playback_pose(const VkrRecordingPlayback *playback);
