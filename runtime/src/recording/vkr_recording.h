#pragma once

#include "core/vkr_codec.h"
#include "core/vkr_entity_ref.h"
#include "core/vkr_metrics.h"
#include "defines.h"
#include "memory/vkr_dmemory.h"
#include "recording/vkr_recorder.h"
#include "vkr_renderer_metrics.h"

/*
 * A recording in progress (docs/proposals/recordings.md, phase R1): the
 * bundle directory, its recorder and the records the runtime writes each
 * frame. The frame-loop thread starts, writes, marks and stops it.
 *
 *   <name>.vkrrec/stream.vkrs   records (vkr_recording_format.h)
 *   <name>.vkrrec/summary.json  what an agent reads first
 */

#define VKR_RECORDING_BUNDLE_EXTENSION ".vkrrec"
#define VKR_RECORDING_STREAM_FILE "stream.vkrs"
#define VKR_RECORDING_SUMMARY_FILE "summary.json"
#define VKR_RECORDING_PATH_MAX 1024u
#define VKR_RECORDING_LABEL_MAX 120u
#define VKR_RECORDING_MARKER_MAX 256u

/* Stream ids of the runtime's records. */
#define VKR_RECORDING_STREAM_FRAME 1u
#define VKR_RECORDING_STREAM_CAMERA 2u
#define VKR_RECORDING_STREAM_MARKER 3u
/* Tier 0 frame metrics: a u64 `valid` bitmask, then one f64 per metric named
   after it, durations in milliseconds. Droppable. */
#define VKR_RECORDING_STREAM_METRICS 4u
/* Metrics a recording can carry; the bitmask has one bit each. */
#define VKR_RECORDING_METRIC_MAX 48u
/* Process memory joins the metrics as this field, sampled every 100 ms. */
#define VKR_RECORDING_RESIDENT_FIELD "process.resident_bytes"
/* Tier 1 render pass rows (vkr_recording_format.h): the name of each pass
   id, then the rows of each rendered frame. Rows are droppable. */
#define VKR_RECORDING_STREAM_PASS_NAME 5u
#define VKR_RECORDING_STREAM_PASSES 6u
/* World poses of entities whose transform changed (VkrRecordingPose), in
   records of up to VKR_RECORDING_POSE_BATCH entries; the first frames list
   every entity once. Not droppable: a lost pose would show a stale one. */
#define VKR_RECORDING_STREAM_POSE 7u
#define VKR_RECORDING_POSE_BATCH 256u
/* Poses a frame writes at most; a larger change list, such as the first
   one of a big level, continues on the next frames. */
#define VKR_RECORDING_POSE_FRAME_MAX 2048u
/* VkrRecordingPose.node of an entity that is not a model node, and
   VkrRecordingPose.document of one named by a ref. */
#define VKR_RECORDING_POSE_NODE_NONE UINT32_MAX
#define VKR_RECORDING_POSE_DOCUMENT_NONE UINT32_MAX
/* VkrRecordingPose.kind: named by ref or document index, or by spawn id,
   which `document` then holds. */
#define VKR_RECORDING_POSE_KIND_DOCUMENT 0u
#define VKR_RECORDING_POSE_KIND_SPAWN 1u
/* VkrRecordingPose.container */
#define VKR_RECORDING_CONTAINER_ACTIVE 0u
#define VKR_RECORDING_CONTAINER_WORLD 1u
#define VKR_RECORDING_CONTAINER_ADDITIVE 2u
/* Gameplay records from script modules (vkr_rec_player, vkr_rec_tag,
   vkr_rec_event in sdk/sdk.h), under the frame whose update ran them. A
   player record is a VkrRecordingPlayer per fixed tick; a tag is a u16 key
   length, the key, then the value; an event is a u16 name length, the
   name, then its payload. */
#define VKR_RECORDING_STREAM_PLAYER 8u
#define VKR_RECORDING_STREAM_TAG 9u
#define VKR_RECORDING_STREAM_EVENT 10u
/* A module's admitted input actions (vkr_rec_action), a
   VkrRecordingAction each: what Drive feeds back. */
#define VKR_RECORDING_STREAM_ACTION 11u
/* Where the scene edit journals stood (a u64: the sequence of the newest
   edit an undo would revert, zero for none), at the start and at each frame
   whose edits changed it. Watch steps the journals back to it. */
#define VKR_RECORDING_STREAM_EDIT 12u
/* Entities scripts spawn, which have no document name: a spawn record is a
   u32 spawn id, a u8 container, then a u16 length and the mesh path and a
   u16 length and the animation bank path of its model (both empty for
   none), then optionally a VkrRecordingSpawnShape; a later record of the id
   gives the look it got: a model, or a shape. A despawn record is the u32
   id of a spawn that went away. */
#define VKR_RECORDING_STREAM_SPAWN 13u
#define VKR_RECORDING_STREAM_DESPAWN 14u
/* Spawns a recording names; later ones are left out. */
#define VKR_RECORDING_SPAWN_MAX 4096u
/* Longest tag key or value and event name; longest event payload. */
#define VKR_RECORDING_TEXT_MAX 63u
#define VKR_RECORDING_EVENT_PAYLOAD_MAX 256u
/* Recent recorded frames whose renderer frame index pass rows can name;
   rows arrive a few frames after their frame. */
#define VKR_RECORDING_FRAME_INDEX_HISTORY 32u

/* VkrRecordingFrame.flags */
#define VKR_RECORDING_FRAME_SIMULATING 1u
#define VKR_RECORDING_FRAME_SCRIPT_CAMERA 2u
#define VKR_RECORDING_FRAME_RENDERING_STOPPED 4u
/* The frame took per-pass GPU timestamps, which cost GPU time on the tiled
   pipeline. */
#define VKR_RECORDING_FRAME_PASS_TIMING 8u

/* What a recording measures (docs/proposals/recordings.md, "Metrics and
   capture tiers"). */
typedef enum VkrRecordingTier {
  VKR_RECORDING_TIER_NONE = 0,
  /* Frame metrics. */
  VKR_RECORDING_TIER_0,
  /* Frame metrics and render pass rows. */
  VKR_RECORDING_TIER_1,
} VkrRecordingTier;

/* One record per frame: the frame's timing and simulation state. */
typedef struct VkrRecordingFrame {
  /* Seconds since the previous frame, as the host measured it. */
  float64_t display_dt;
  float64_t simulation_time;
  /* The level's hour, [0, 24). */
  float32_t time_of_day;
  uint32_t flags;
} VkrRecordingFrame;

/* One record per frame: the camera the frame rendered with. */
typedef struct VkrRecordingCamera {
  /* World position: the camera position plus the scene's origin offset. */
  float64_t position[3];
  /* Degrees, as VkrCamera stores them. */
  float32_t yaw;
  float32_t pitch;
  float32_t fov;
  float32_t near_clip;
  float32_t far_clip;
  /* CameraType */
  uint32_t projection;
} VkrRecordingCamera;

/* One entity's world pose. An entity is named by the container that holds
   it and its document-stable ref, or, for a node of an imported model, by
   the ref of the nearest ancestor that has one and the glTF node index. A
   document without refs names its entities by document index instead, with
   a zero ref: stable while the document is unchanged. A script spawn, or a
   node of a model spawned under one, is named by spawn id (kind SPAWN). */
typedef struct VkrRecordingPose {
  VkrEntityRef ref;
  uint32_t node;
  uint8_t container;
  uint8_t kind;
  uint8_t reserved[2];
  /* World translation plus the scene's origin offset. */
  float64_t translation[3];
  /* The world matrix's upper 3x3, column-major. */
  float32_t linear[9];
  uint32_t document;
} VkrRecordingPose;

/* A module's player at one fixed tick: feet position in world space with
   the scene's origin offset, velocity, view and module-defined state. */
typedef struct VkrRecordingPlayer {
  uint64_t tick;
  float64_t position[3];
  float32_t velocity[3];
  float32_t yaw;
  float32_t pitch;
  /* VKR_REC_PLAYER_* (sdk/sdk.h) */
  uint32_t flags;
  uint32_t state;
  uint32_t reserved;
} VkrRecordingPlayer;

/* One input action a module admitted at a fixed tick. `action` and the
   x, y values are the module's own; `sequence` orders actions of one tick.
 */
typedef struct VkrRecordingAction {
  uint64_t tick;
  uint64_t sequence;
  uint32_t action;
  uint32_t pressed;
  float32_t x;
  float32_t y;
} VkrRecordingAction;

/* Marker records are a u32 kind followed by a UTF-8 label. */
typedef struct VkrRecordingMarker {
  uint64_t frame;
  float64_t time;
  uint32_t kind;
  char label[VKR_RECORDING_LABEL_MAX + 1u];
} VkrRecordingMarker;

/* The streams every runtime recording declares. */
const VkrRecordingStream *vkr_recording_streams(uint32_t *out_count);

typedef struct VkrRecordingConfig {
  /* The bundle directory to create, ending in VKR_RECORDING_BUNDLE_EXTENSION.
   */
  const char *bundle_path;
  /* NULL stores chunks raw. */
  const VkrCodec *codec;
  /* Summary facts; may be empty. */
  const char *name;
  const char *scene;
  const char *author;
  /* UTF-8 JSON for the IDENTITY chunk; NULL writes "{}". */
  const char *identity_json;
  /* The registry whose Tier 0 metrics the recording carries each frame;
     NULL records no metrics. Borrowed while the recording runs. */
  const VkrMetrics *metrics;
  /* Tier 1: also record each rendered frame's pass rows. Needs `metrics`. */
  bool8_t passes;
  /* Record the world poses of entities that move (VKR_RECORDING_STREAM_POSE).
   */
  bool8_t poses;
  /* Tier 1: take per-pass GPU timestamps in one frame of every
     `pass_gpu_period`; zero leaves them as the metrics config has them.
     vkr_recording_pass_gpu_frame tells the runtime which frames. */
  uint32_t pass_gpu_period;
} VkrRecordingConfig;

typedef struct VkrRecordingFrameIndex {
  uint64_t frame_index;
  uint64_t frame;
} VkrRecordingFrameIndex;

typedef struct VkrRecording {
  bool8_t active;
  VkrDMemory memory;
  VkrAllocator allocator;
  VkrRecorder *recorder;
  char bundle_path[VKR_RECORDING_PATH_MAX];
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  char scene[VKR_RECORDING_PATH_MAX];
  char author[32];
  int64_t started_unix;
  /* Frames written and the recorded seconds they span. */
  uint64_t frame;
  float64_t time;
  VkrRecordingMarker markers[VKR_RECORDING_MARKER_MAX];
  uint32_t marker_count;
  /* Tier 0 metrics found in the registry at start, in field order: their
     ids, and 1e-6 for nanosecond durations, else 1. The resident memory
     field follows them. */
  bool8_t metrics_enabled;
  uint32_t metric_count;
  VkrMetricId metric_ids[VKR_RECORDING_METRIC_MAX];
  float64_t metric_scale[VKR_RECORDING_METRIC_MAX];
  VkrMetricKind metric_kind[VKR_RECORDING_METRIC_MAX];
  float64_t resident_sampled_at;
  uint64_t resident_bytes;
  bool8_t resident_valid;
  /* Tier 1, and the renderer frame index of recent recorded frames. */
  bool8_t passes_enabled;
  uint32_t pass_gpu_period;
  VkrRecordingFrameIndex frame_history[VKR_RECORDING_FRAME_INDEX_HISTORY];
  uint32_t frame_history_next;
  /* The newest renderer frame whose rows were written, plus one. */
  uint64_t pass_frame_written;
  /* Pass names by id, and an open-addressed table of id + 1 by name hash;
     both from the recording's memory. */
  uint32_t pass_count;
  char (*pass_names)[VKR_RECORDING_PASS_NAME_MAX + 1u];
  uint16_t *pass_slots;
  uint64_t pass_rows_written;
  /* Poses waiting for vkr_recording_poses_flush, from the recording's
     memory. */
  bool8_t poses_enabled;
  VkrRecordingPose *pose_batch;
  uint32_t pose_batch_count;
  uint64_t poses_written;
  /* Spawns the recording named: their entity ids and containers by spawn
     id, an open-addressed table of id + 1 by entity, and which spawns are
     still alive and have their model written. From the recording's memory.
   */
  uint64_t *spawn_entities;
  uint8_t *spawn_containers;
  uint8_t *spawn_flags;
  uint32_t *spawn_slots;
  uint32_t spawn_count;
  /* Gameplay records written, for the summary. */
  uint64_t player_records;
  uint64_t action_records;
} VkrRecording;

/**
 * @brief Creates the bundle directory and its stream, and starts recording.
 * @return false_v when the bundle exists or cannot be created; the recording
 * stays inactive.
 */
bool8_t vkr_recording_start(VkrRecording *recording,
                            const VkrRecordingConfig *config);

/** Writes one frame's records; the frame number advances. */
void vkr_recording_frame(VkrRecording *recording,
                         const VkrRecordingFrame *frame,
                         const VkrRecordingCamera *camera);

/**
 * @brief Writes the Tier 0 metrics of recorded frame `frame` from a published
 * metrics frame; no-op for a recording without metrics. Frame-loop thread.
 */
void vkr_recording_metrics(VkrRecording *recording, uint64_t frame,
                           const VkrMetricsFrame *published);

/**
 * @brief Writes the pass rows of `passes`, the table collected with renderer
 * frame `frame_index`, which recorded frame `frame` rendered with. The rows
 * describe an earlier frame, named by their GPU source frame; rows of a frame
 * already written or older than the history are skipped. No-op below Tier 1.
 */
void vkr_recording_passes(VkrRecording *recording, uint64_t frame,
                          uint64_t frame_index,
                          const VkrRendererMetricsPassTable *passes);

/**
 * @brief Whether the next recorded frame takes per-pass GPU timestamps.
 * @return false_v when the recording leaves them to the metrics config;
 * otherwise true_v with the answer in `out_timed`.
 */
bool8_t vkr_recording_pass_gpu_frame(const VkrRecording *recording,
                                     bool8_t *out_timed);

/** Adds a pose of the last written frame; full batches go to the recorder.
    No-op for a recording without poses. */
void vkr_recording_pose(VkrRecording *recording, const VkrRecordingPose *pose);

/** Writes the poses still batched. */
void vkr_recording_poses_flush(VkrRecording *recording);

/** Writes a player record under the frame being updated. */
void vkr_recording_player(VkrRecording *recording,
                          const VkrRecordingPlayer *player);

/* A spawn's primitive shape (SceneShape). */
typedef struct VkrRecordingSpawnShape {
  uint32_t type;
  float32_t size[3];
  float32_t color[4];
} VkrRecordingSpawnShape;

/* VkrRecording.spawn_flags */
#define VKR_RECORDING_SPAWN_ALIVE 1u
/* Its look, a model or a shape, is written. */
#define VKR_RECORDING_SPAWN_LOOK 2u

/** The spawn id of `entity` in `container`, naming it on first sight and
    writing its spawn record; UINT32_MAX when the table is full. */
uint32_t vkr_recording_spawn_id(VkrRecording *recording, uint64_t entity,
                                uint8_t container);

/** Writes the look a spawn got, once: its model's paths, or with empty
    paths its shape. */
void vkr_recording_spawn_look(VkrRecording *recording, uint32_t id,
                              String8 mesh, String8 animation,
                              const VkrRecordingSpawnShape *shape);

/** Writes that a spawn went away; its id is not reused. */
void vkr_recording_despawn(VkrRecording *recording, uint32_t id);

/** Writes where the edit journals stand, under the frame on screen. */
void vkr_recording_edit(VkrRecording *recording, uint64_t sequence);

/** Writes an action record under the frame being updated. */
void vkr_recording_action(VkrRecording *recording,
                          const VkrRecordingAction *action);

/** Writes a tag: `key` became `value`, each cut to VKR_RECORDING_TEXT_MAX
    bytes. */
void vkr_recording_tag(VkrRecording *recording, String8 key, String8 value);

/** Writes an event `name` with up to VKR_RECORDING_EVENT_PAYLOAD_MAX bytes
    of payload. */
void vkr_recording_event(VkrRecording *recording, String8 name,
                         const void *payload, uint32_t size);

/** Adds a marker at the current frame. Returns false_v when it was lost. */
bool8_t vkr_recording_mark(VkrRecording *recording, VkrRecordingMarkerKind kind,
                           const char *label);

/**
 * @brief Stops the recorder, writes summary.json and releases the memory.
 * @return false_v when the stream or the summary could not be written.
 */
bool8_t vkr_recording_stop(VkrRecording *recording,
                           VkrRecorderStats *out_stats);

/** Live counters while active. */
void vkr_recording_stats(const VkrRecording *recording,
                         VkrRecorderStats *out_stats);
