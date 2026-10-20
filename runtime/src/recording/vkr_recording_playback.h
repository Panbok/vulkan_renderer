#pragma once

#include "core/vkr_atomic.h"
#include "core/vkr_codec.h"
#include "defines.h"
#include "math/mat.h"
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

/* One recorded pose of one entity: its world pose, or with
   VKR_RECORDING_POSE_FLAG_PARENTED its pose relative to its parent. A long
   recording holds tens of millions, so it stays 72 bytes. */
typedef struct VkrRecordingPoseSample {
  uint64_t frame;
  float64_t translation[3];
  float32_t linear[9];
  /* VKR_RECORDING_POSE_FLAG_* */
  uint32_t flags;
} VkrRecordingPoseSample;

_Static_assert(sizeof(VkrRecordingPoseSample) == 72u,
               "pose samples dominate a playback's memory");

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
   entity's first recorded pose (vkr_recording_playback_pose_at), a key
   whose only sample matched the scene when it was bound, which needs none,
   one whose pose relative to its parent is its own local pose, which its
   parent carries without an override, or a pose blended between two
   samples, which the next frame sets again. */
#define VKR_RECORDING_POSE_APPLIED_NONE UINT32_MAX
#define VKR_RECORDING_POSE_APPLIED_STATIC (UINT32_MAX - 1u)
#define VKR_RECORDING_POSE_APPLIED_FOLLOWS (UINT32_MAX - 2u)
#define VKR_RECORDING_POSE_APPLIED_BLENDED (UINT32_MAX - 3u)

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

/** The per-frame platform fields a playback loads, in storage order. */
typedef enum VkrRecordingSystemField {
  VKR_RECORDING_SYSTEM_THERMAL = 0,
  VKR_RECORDING_SYSTEM_MEMORY_PRESSURE,
  VKR_RECORDING_SYSTEM_LOW_POWER,
  VKR_RECORDING_SYSTEM_FOOTPRINT,
  VKR_RECORDING_SYSTEM_GPU_ERRORS,
  VKR_RECORDING_SYSTEM_LOG_ERRORS,
  VKR_RECORDING_SYSTEM_FIELDS,
} VkrRecordingSystemField;

/* Names a playback's timeline keeps per zone or pass. */
#define VKR_RECORDING_PLAYBACK_NAME_MAX 63u
/* A GPU span that is a whole GPU frame rather than one pass. */
#define VKR_RECORDING_PLAYBACK_GPU_FRAME UINT16_MAX

/* One CPU zone (ADR-126) on the recording machine's monotonic clock: its
   name's index in zone_names, its thread slot (0 frame loop, 1 render
   thread, 2 + k job worker k) and nesting depth. */
typedef struct VkrRecordingPlaybackZone {
  uint64_t begin_ns;
  uint64_t end_ns;
  uint64_t frame;
  uint16_t zone;
  uint16_t thread;
  uint32_t depth;
} VkrRecordingPlaybackZone;

/* One timed GPU pass, its name's index in pass_names, or a whole GPU frame
   (pass VKR_RECORDING_PLAYBACK_GPU_FRAME), on the same clock as the zones. */
typedef struct VkrRecordingPlaybackGpuSpan {
  uint64_t begin_ns;
  uint64_t end_ns;
  uint64_t frame;
  uint16_t pass;
} VkrRecordingPlaybackGpuSpan;

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
  /* Platform signals and fault counts of each frame from its metrics (Tier
     0, ADR-125): VKR_RECORDING_SYSTEM_FIELDS per frame in the order of
     VkrRecordingSystemField, where bit k of system_valid says field k was
     sampled. has_system says the recording declares any; without it both
     arrays are NULL. */
  bool8_t has_system;
  float64_t *system_values;
  uint8_t *system_valid;
  /* The timeline (ADR-126), for recordings with zones or GPU pass starts:
     zones and GPU spans sorted by begin, with the longest of each for range
     queries; zone and pass names by id; each frame's "Frame" zone begin,
     zero without one; and one bit per thread slot that recorded zones.
     Without a timeline the arrays are NULL. */
  bool8_t has_timeline;
  VkrRecordingPlaybackZone *zones;
  uint32_t zone_count;
  uint64_t zone_longest_ns;
  VkrRecordingPlaybackGpuSpan *gpu_spans;
  uint32_t gpu_span_count;
  uint64_t gpu_longest_ns;
  char (*zone_names)[VKR_RECORDING_PLAYBACK_NAME_MAX + 1u];
  uint32_t zone_name_count;
  char (*pass_names)[VKR_RECORDING_PLAYBACK_NAME_MAX + 1u];
  uint32_t pass_name_count;
  uint64_t *frame_begin_ns;
  uint32_t zone_threads;
  /* The player of each frame's last tick (vkr_rec_player): three per frame,
     where player_valid is 1; has_player says any frame has one. */
  bool8_t has_player;
  float64_t *player_positions;
  uint8_t *player_valid;
  /* The cloud wind offset each frame rendered with
     (VKR_RECORDING_STREAM_CLOUDS): two per frame where cloud_valid is 1, a
     frame without a record keeping the one before; has_clouds says the
     recording carries any. */
  bool8_t has_clouds;
  float64_t *cloud_offsets;
  uint8_t *cloud_valid;
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
  /* Each key's entity's parent's key, UINT32_MAX for none, and the world
     pose each key's entity has at pose_world_frames[k], composed from its
     ancestors' for a parented one (UINT64_MAX for none yet). */
  uint32_t *pose_parents;
  Mat4 *pose_worlds;
  uint64_t *pose_world_frames;
  /* Keys whose entity the playback hid because the pose it shows was
     recorded hidden; closing shows them again. */
  uint8_t *pose_hidden;

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
  /* How far the playhead is from `frame` toward the next one, [0, 1]; the
     camera is interpolated by it. */
  float32_t blend;
  /* The recorded frame at or before the playhead. */
  uint64_t frame;
  /* The cloud wind offset `frame` rendered with, when the recording
     carries one. */
  bool8_t has_clouds;
  float64_t cloud_wind_offset_m[2];
} VkrRecordingPlaybackPose;

/* Where an open on another thread stands: thousandths of the stream read
   twice (once to count, once to fill), and a request to stop early, which
   fails the open with "cancelled". */
typedef struct VkrRecordingPlaybackProgress {
  VkrAtomicUint32 permille;
  VkrAtomicUint32 cancel;
} VkrRecordingPlaybackProgress;

/**
 * @brief Loads a bundle directory or a stream file. It touches only
 * `playback`, the file and the codec, so it may run on any thread.
 * @param codec Decodes compressed chunks; the runtime's zstd decoder.
 * @param progress Optional: written as the load reads, read for a cancel.
 * @param error Receives a one-line reason on failure.
 */
bool8_t vkr_recording_playback_open(VkrRecordingPlayback *playback,
                                    const char *path, const VkrCodec *codec,
                                    VkrRecordingPlaybackProgress *progress,
                                    char *error, uint64_t error_capacity);

/** Moves an open playback from `from` into the closed `to`, leaving `from`
 * closed: its memory follows it. */
void vkr_recording_playback_move(VkrRecordingPlayback *to,
                                 VkrRecordingPlayback *from);

void vkr_recording_playback_close(VkrRecordingPlayback *playback);

/** The zones that may overlap [begin_ns, end_ns): zones[*out_first] on, as
 * many as returned. Some may end before begin_ns; callers test each. */
uint32_t vkr_recording_playback_zones_in(const VkrRecordingPlayback *playback,
                                         uint64_t begin_ns, uint64_t end_ns,
                                         uint32_t *out_first);

/** The GPU spans that may overlap [begin_ns, end_ns), as for zones. */
uint32_t
vkr_recording_playback_gpu_spans_in(const VkrRecordingPlayback *playback,
                                    uint64_t begin_ns, uint64_t end_ns,
                                    uint32_t *out_first);

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

/** The camera, time of day and cloud wind offset at the playhead. */
VkrRecordingPlaybackPose
vkr_recording_playback_pose(const VkrRecordingPlayback *playback);
