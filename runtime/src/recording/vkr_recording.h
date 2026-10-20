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
/* Renderer event metrics a recording turns into engine events. */
#define VKR_RECORDING_EVENT_METRIC_MAX 8u
/* Process memory joins the metrics as this field, sampled every 100 ms. */
#define VKR_RECORDING_RESIDENT_FIELD "process.resident_bytes"
/* Tier 1 render pass rows (vkr_recording_format.h): the name of each pass
   id, then the rows of each rendered frame. Rows are droppable. */
#define VKR_RECORDING_STREAM_PASS_NAME 5u
#define VKR_RECORDING_STREAM_PASSES 6u
/* Poses of entities whose transform changed (VkrRecordingPose), in records
   of up to VKR_RECORDING_POSE_BATCH entries; the first frames list every
   entity once. An entity with a parent is posed relative to it
   (VKR_RECORDING_POSE_FLAG_PARENTED) and gets no pose when only an ancestor
   moved. Not droppable: a lost pose would show a stale one. */
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
/* VkrRecordingPose.flags: the entity was hidden (its own visibility off),
   such as a first-person player's body; a change of visibility writes a
   pose. */
#define VKR_RECORDING_POSE_FLAG_HIDDEN 1u
/* VkrRecordingPose.flags: the translation and 3x3 are the entity's pose in
   its parent's space, without the origin offset; its world pose is its
   parent's times it. Recordings before the flag pose every entity in world
   space. */
#define VKR_RECORDING_POSE_FLAG_PARENTED 2u
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
/* Re-simulation (docs/proposals/recordings.md, "Drive and re-simulation"):
   a session record (VkrRecordingSession) when a Play session starts while
   recording; a sim_frame record (VkrRecordingSimFrame) for every frame the
   session then runs, the clock its modules read; and a tick_hash record
   (VkrRecordingTickHash) for every fixed tick. A hash_module record, a u32
   index then the UTF-8 module name, names the index tick hashes use. */
#define VKR_RECORDING_STREAM_SESSION 15u
#define VKR_RECORDING_STREAM_SIM_FRAME 16u
#define VKR_RECORDING_STREAM_TICK_HASH 17u
#define VKR_RECORDING_STREAM_HASH_MODULE 18u
/* An asynchronous result the session applied (VkrRecordingJoin): a
   re-simulation applies it after as many fixed ticks, whenever its load
   finishes. */
#define VKR_RECORDING_STREAM_JOIN 19u
/* The world partition cells loaded when the session started: a u32 count,
   then that many {i32 x, i32 z}. A re-simulation loads and unloads cells
   to match before it starts. */
#define VKR_RECORDING_STREAM_CELLS 20u
/* Cells a cells record holds at most. */
#define VKR_RECORDING_CELLS_MAX 256u
/* All measurements (VkrRecordingConfig.all_measurements): every metric of
   the registry beyond the metrics stream's, each named once by a
   measurement_name record (a u32 index, a u16 name length, a u16 unit
   length, the name, the unit) and recorded each frame in measurements
   records of up to 64: a u32 first index, a u32 count, a u64 valid mask,
   then `count` float64 values in the unit named. Tier 1 recordings carry
   the metrics named by VKR_RECORDING_TIER1_MEASUREMENTS the same way: what
   the Performance window's Memory, Backend and Draws tabs read. */
#define VKR_RECORDING_STREAM_MEASUREMENT_NAME 21u
#define VKR_RECORDING_STREAM_MEASUREMENTS 22u
/* CPU zones (VkrRecordingConfig.zones, lib/src/core/vkr_zone.h): a
   zone_name record is a u32 zone id then the UTF-8 name; a zone record is
   one VkrZoneRecord (u16 zone, u16 thread slot, u32 depth, u64 begin_ns, u64
   end_ns on the CPU's monotonic clock) under the recorded frame current
   when it ended. Slot 0 is the frame loop, 1 the render thread, 2 + k job
   worker k; a "Frame" zone on slot 0 spans one frame-loop iteration. */
#define VKR_RECORDING_STREAM_ZONE_NAME 23u
#define VKR_RECORDING_STREAM_ZONE 24u
/* GPU pass starts (Tier 1, vkr_recording_format.h). */
#define VKR_RECORDING_STREAM_PASS_STARTS 25u
/* The render graph's shape (Tier 1, vkr_recording_format.h): when the
   recording starts and whenever the frame's passes change. Not droppable: a
   lost one would show another graph's shape. */
#define VKR_RECORDING_STREAM_GRAPH 26u
/* Named allocators (Tier 1, vkr_recording_format.h): an allocator_name
   record per registry entry, then each frame's samples. Samples are
   droppable. */
#define VKR_RECORDING_STREAM_ALLOCATOR_NAME 27u
#define VKR_RECORDING_STREAM_ALLOCATORS 28u
/* The draw census (Tier 1, vkr_recording_format.h): object bounds and
   candidate ids when they change, not droppable; each rendered frame's
   culling states, droppable. */
#define VKR_RECORDING_STREAM_DRAW_BOUNDS 29u
#define VKR_RECORDING_STREAM_DRAW_IDS 30u
#define VKR_RECORDING_STREAM_DRAW_STATES 31u
/* Hardware facts and the renderer's pipelines (Tier 1,
   vkr_recording_format.h): not droppable, so a lost record is written again
   on a later frame. */
#define VKR_RECORDING_STREAM_HARDWARE 32u
#define VKR_RECORDING_STREAM_PIPELINE 33u
/* Ring bytes of each job worker's producer when zones are recorded. */
#define VKR_RECORDING_WORKER_RING_BYTES (1024u * 1024u)
/* Transform hashes are split by stable entity key
   (vkr_scene_entity_stable_key), so a mismatch narrows to the entities of
   the buckets that differ. */
#define VKR_RECORDING_HASH_BUCKETS 16u
/* Script modules a tick hash covers; later ones are left out. */
#define VKR_RECORDING_HASH_MODULE_MAX 16u
#define VKR_RECORDING_HASH_MODULE_NAME_MAX 63u
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

/* The Play session that started while recording: the seed of its modules'
   generators. Its ticks count from one. */
typedef struct VkrRecordingSession {
  uint64_t seed;
} VkrRecordingSession;

/* The clock a session frame's modules read (vkr_time), the scene delta
   the frame began with, and the streaming source that frame's world
   partition and terrain streaming read (the camera position; `has_source`
   zero without one). */
typedef struct VkrRecordingSimFrame {
  float64_t now;
  float64_t delta;
  float64_t source[3];
  uint32_t has_source;
  uint32_t reserved;
} VkrRecordingSimFrame;

/* The scene's join kinds (VkrSceneJoinKind), by the same values. */
typedef enum VkrRecordingJoinKind {
  /** A requested model joined its wrapper, or failed. */
  VKR_RECORDING_JOIN_MODEL = 1,
  /** A streamed terrain's worker-built collision body replaced its body. */
  VKR_RECORDING_JOIN_TERRAIN_BODY = 2,
} VkrRecordingJoinKind;

/* An asynchronous result a session applied: its kind, the stable key of
   the entity it joined (vkr_scene_entity_stable_key, as tick hashes name
   entities) and the fixed ticks completed before it. */
typedef struct VkrRecordingJoin {
  uint64_t ticks;
  uint64_t entity;
  uint32_t kind;
  uint32_t failed;
} VkrRecordingJoin;

/* The simulation after one fixed tick, hashed by subsystem: the scene's
   transforms in VKR_RECORDING_HASH_BUCKETS buckets by stable entity key,
   its physics bodies, and each script module's state (generators,
   vkr_sim_hash input, components and per-entity state), in hash_module
   order. Per-entity hashes are summed, so slot and chunk order do not
   count. */
typedef struct VkrRecordingTickHash {
  uint64_t tick;
  uint32_t transforms[VKR_RECORDING_HASH_BUCKETS];
  uint32_t bodies;
  uint32_t module_count;
  uint32_t modules[VKR_RECORDING_HASH_MODULE_MAX];
} VkrRecordingTickHash;

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

/* One entity's pose, in world space or, with
   VKR_RECORDING_POSE_FLAG_PARENTED, in its parent's. An entity is named by the
   container that holds it and its document-stable ref, or, for a node of an
   imported model, by the ref of the nearest ancestor that has one and the glTF
   node index. A document without refs names its entities by document index
   instead, with a zero ref: stable while the document is unchanged. A script
   spawn, or a node of a model spawned under one, is named by spawn id (kind
   SPAWN). */
typedef struct VkrRecordingPose {
  VkrEntityRef ref;
  uint32_t node;
  uint8_t container;
  uint8_t kind;
  /* VKR_RECORDING_POSE_FLAG_*; zero in recordings before them. */
  uint8_t flags;
  uint8_t reserved;
  /* World translation plus the scene's origin offset, or the translation
     in the parent's space. */
  float64_t translation[3];
  /* The world or parent-space matrix's upper 3x3, column-major. */
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
  /* Every metric of the registry, not only Tier 0's, in the measurements
     streams. Needs `metrics`. */
  bool8_t all_measurements;
  /* Record the world poses of entities that move (VKR_RECORDING_STREAM_POSE).
   */
  bool8_t poses;
  /* Tier 1: take per-pass GPU timestamps in one frame of every
     `pass_gpu_period`; zero leaves them as the metrics config has them.
     vkr_recording_pass_gpu_frame tells the runtime which frames. */
  uint32_t pass_gpu_period;
  /* Record CPU zones from every thread that named its zone slot. The
     recording installs the process's zone sink while it runs, so at most
     one recording records zones. */
  bool8_t zones;
  /* Rolling: keep only about this many recent seconds in memory, create no
     bundle, and write one only through vkr_recording_save_window; stopping
     discards the window. Zero records a bundle as usual. A rolling
     recording takes no poses and no all-measurements streams. */
  float64_t rolling_seconds;
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
  /* A Play session started while recording: its sim frames and tick hashes
     are written, and the module names written so far. */
  bool8_t session_written;
  uint32_t hash_modules_named;
  uint64_t tick_hashes_written;
  /* Tier 0 metrics found in the registry at start, in field order: their
     ids, and 1e-6 for nanosecond durations, else 1. The resident memory
     field follows them. */
  bool8_t metrics_enabled;
  /* The measurements are every catalog metric, not Tier 1's set. */
  bool8_t all_measurements;
  uint32_t metric_count;
  VkrMetricId metric_ids[VKR_RECORDING_METRIC_MAX];
  float64_t metric_scale[VKR_RECORDING_METRIC_MAX];
  VkrMetricKind metric_kind[VKR_RECORDING_METRIC_MAX];
  /* Renderer event metrics found at start, written as engine events on
     the frames whose publication counted them: their ids and the index of
     their event name. */
  uint32_t event_metric_count;
  VkrMetricId event_metric_ids[VKR_RECORDING_EVENT_METRIC_MAX];
  uint8_t event_metric_names[VKR_RECORDING_EVENT_METRIC_MAX];
  float64_t resident_sampled_at;
  uint64_t resident_bytes;
  bool8_t resident_valid;
  /* gpu.submission's field index, UINT32_MAX without it, and the newest
     renderer frame whose GPU time was written, plus one. Its value lags the
     published frame, so it is written on the frame that rendered it. */
  uint32_t gpu_submission_metric;
  uint64_t gpu_frame_written;
  /* All measurements: the registry metrics beyond metric_ids. */
  uint32_t measurement_count;
  VkrMetricId measurement_ids[VKR_RECORDING_MEASUREMENT_MAX];
  float64_t measurement_scale[VKR_RECORDING_MEASUREMENT_MAX];
  VkrMetricKind measurement_kind[VKR_RECORDING_MEASUREMENT_MAX];
  /* Field masks of VKR_RECORDING_LOOP_METRICS and
     VKR_RECORDING_LOOP_BUILD_METRICS. */
  uint64_t loop_metrics;
  uint64_t loop_build_metrics;
  /* The renderer frame index of recent recorded frames. */
  VkrRecordingFrameIndex frame_history[VKR_RECORDING_FRAME_INDEX_HISTORY];
  uint32_t frame_history_next;
  /* Seconds a rolling recording keeps; zero for a bundle recording. */
  float64_t rolling_seconds;
  /* CPU zones: the sink is installed, the zone names written so far, the
     recorded frame of the frame-loop iteration that runs, which the frame
     loop's and the job workers' zones file under, and the recorded frame
     the render thread renders, which its zones file under. */
  bool8_t zones_enabled;
  uint32_t zone_names_written;
  VkrAtomicUint64 zone_frame;
  VkrAtomicUint64 zone_render_frame;
  /* Tier 1. */
  bool8_t passes_enabled;
  uint32_t pass_gpu_period;
  /* The newest renderer frame whose rows were written, plus one. */
  uint64_t pass_frame_written;
  /* Pass names by id, and an open-addressed table of id + 1 by name hash;
     both from the recording's memory. */
  uint32_t pass_count;
  char (*pass_names)[VKR_RECORDING_PASS_NAME_MAX + 1u];
  uint16_t *pass_slots;
  uint64_t pass_rows_written;
  /* The allocator registry's entries by recorded id: each id's slot and
     generation, and its cumulative counts at the last sample. */
  uint32_t allocator_count;
  uint32_t allocator_slot[VKR_RECORDING_ALLOCATOR_MAX];
  uint32_t allocator_generation[VKR_RECORDING_ALLOCATOR_MAX];
  uint64_t allocator_counts[VKR_RECORDING_ALLOCATOR_MAX][4];
  /* The draw census: the bounds generation and ids generation written, and
     the newest renderer frame whose states were written, plus one. */
  uint64_t draw_bounds_generation;
  bool8_t draw_bounds_written;
  uint64_t draw_ids_generation;
  bool8_t draw_ids_written;
  uint64_t draw_frame_written;
  /* Hardware facts are written, and the renderer pipelines written so
     far (ADR-130). */
  bool8_t hardware_written;
  uint32_t pipelines_written;
  /* Pass ids a recorded graph shape holds; a frame that runs a pass none
     holds asks the caller for a snapshot (vkr_recording_graph_wanted). The
     passes a frame culls change often and its rows show them, so they make
     no new shape. */
  uint8_t *graph_covered;
  /* Uncovered passes that asked for the snapshot not yet written; it covers
     them too, so a pass that runs only now and then asks once. */
  uint8_t *graph_pending;
  bool8_t graph_wanted;
  uint32_t graphs_written;
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

/** Starts a frame-loop iteration: its zones, those before its frame record
 * too, file under the frame it records next (the frame of its frame.wall
 * and of its loop metrics, ADR-082). Frame-loop thread. */
void vkr_recording_loop_begin(VkrRecording *recording);

/** The recorded frame the render thread renders next, which its zones file
 * under; call before handing it that frame. Frame-loop thread. */
void vkr_recording_render_frame(VkrRecording *recording, uint64_t frame);

/** Writes one frame's records; the frame number advances. */
void vkr_recording_frame(VkrRecording *recording,
                         const VkrRecordingFrame *frame,
                         const VkrRecordingCamera *camera);

/* Where a publication's frame-loop work belongs when a render thread
   completed its frame during a later frame-loop iteration (ADR-082): that
   iteration measured its update and its wait for the frame before closing
   the metrics frame, so they lengthen its own frame.wall, not the published
   frame's. `frame` is that iteration's recorded frame (UINT64_MAX before the
   first) and `built` says it built its frame before the completion. */
typedef struct VkrRecordingMetricsLoop {
  uint64_t frame;
  bool8_t built;
} VkrRecordingMetricsLoop;

/**
 * @brief Writes the Tier 0 metrics of recorded frame `frame` from a published
 * metrics frame, except gpu.submission, which vkr_recording_gpu_submission
 * places, and, with `loop`, the frame-loop work it names, which goes on
 * `loop->frame`; `frame` UINT64_MAX (a frame from before the recording)
 * writes only that. No-op for a recording without metrics. Frame-loop
 * thread.
 */
void vkr_recording_metrics(VkrRecording *recording, uint64_t frame,
                           const VkrMetricsFrame *published,
                           const VkrRecordingMetricsLoop *loop);

/**
 * @brief Notes that recorded frame `frame` rendered renderer frame
 * `frame_index`, then writes the pass rows of `passes`, the table collected
 * with it. The rows describe an earlier frame, named by their GPU source
 * frame; rows of a frame already written or older than the history are
 * skipped. Rows need Tier 1; the note is taken at every tier.
 */
void vkr_recording_passes(VkrRecording *recording, uint64_t frame,
                          uint64_t frame_index,
                          const VkrRendererMetricsPassTable *passes);

/**
 * @brief Whether the passes changed since the graph was last recorded, so
 * the caller should ask the renderer for a graph snapshot; true once per
 * change. Tier 1 only.
 */
bool8_t vkr_recording_graph_wanted(VkrRecording *recording);

/**
 * @brief Writes the render graph shape `snapshot` describes under the
 * recorded frame that rendered its frame, or `frame` when that left the
 * history. Tier 1 only.
 */
void vkr_recording_graph(VkrRecording *recording, uint64_t frame,
                         const struct VkrRgSnapshot *snapshot);

/**
 * @brief Whether the draw census wants object bounds of generation
 * `generation`: Tier 1 and none written yet for it.
 */
bool8_t vkr_recording_draw_bounds_wanted(const VkrRecording *recording,
                                         uint64_t generation);

/**
 * @brief Writes the world boxes of `count` objects (VkrWorldObjectBounds rows)
 * of bounds generation `generation` under recorded frame `frame`, with the
 * world partition's cell size (zero without one).
 */
void vkr_recording_draw_bounds(VkrRecording *recording, uint64_t frame,
                               uint64_t generation,
                               const struct VkrWorldObjectBounds *rows,
                               uint32_t count, float32_t cell_size);

/**
 * @brief Writes the culling states of `census` under the recorded frame that
 * rendered its source frame, after its candidate ids when they changed; a
 * census already written or older than the history is skipped. Tier 1 only.
 */
void vkr_recording_draws(VkrRecording *recording,
                         const VkrRendererMetricsDrawCensus *census);

/**
 * @brief Samples the lib's allocator registry and writes recorded frame
 * `frame`'s allocator rows, naming new entries first. Tier 1 only.
 */
void vkr_recording_allocators(VkrRecording *recording, uint64_t frame);

/**
 * @brief Writes the device's hardware facts once, under recorded frame
 * `frame`; later calls write nothing. Tier 1 only.
 */
void vkr_recording_hardware(VkrRecording *recording, uint64_t frame,
                            const VkrRecordingHardwareFact *facts,
                            uint32_t count);

struct VkrPipelineRegistry;

/**
 * @brief Writes the pipelines `registry` gained since the last call under
 * recorded frame `frame`, the earlier ones at the first call. Tier 1 only.
 */
void vkr_recording_pipelines(VkrRecording *recording, uint64_t frame,
                             struct VkrPipelineRegistry *registry);

/**
 * @brief Writes gpu.submission, `gpu_ms` of renderer frame
 * `source_frame_index`, on the recorded frame that rendered it, as a metrics
 * record holding that field alone. Call after vkr_recording_passes; a frame
 * already written or older than the history is skipped.
 */
void vkr_recording_gpu_submission(VkrRecording *recording,
                                  uint64_t source_frame_index,
                                  float64_t gpu_ms);

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

/** Writes engine event `name` (VKR_RECORDING_EVENT_*): `count` of them in
    the frame being updated, taking `ms` milliseconds, zero when not
    measured. */
void vkr_recording_engine_event(VkrRecording *recording, const char *name,
                                uint32_t count, float32_t ms);

/** Writes that a Play session started with generator seed `seed`; its sim
    frames and tick hashes follow. */
void vkr_recording_session(VkrRecording *recording, uint64_t seed);

/** Writes the clock of the session frame being updated; nothing before a
    session started while recording. */
void vkr_recording_sim_frame(VkrRecording *recording,
                             const VkrRecordingSimFrame *frame);

/** Writes the world partition cells loaded as the session starts,
    `count` {x, z} pairs, at most VKR_RECORDING_CELLS_MAX; nothing before a
    session started while recording. */
void vkr_recording_cells(VkrRecording *recording, const int32_t *cells,
                         uint32_t count);

/** Writes an asynchronous result the session applied; nothing before a
    session started while recording. */
void vkr_recording_join(VkrRecording *recording, const VkrRecordingJoin *join);

/** Writes a tick's hashes, and first the names of modules not yet named;
    nothing before a session started while recording. */
void vkr_recording_tick_hash(VkrRecording *recording,
                             const VkrRecordingTickHash *hash,
                             const char *const *module_names);

/** Adds a marker at the current frame. Returns false_v when it was lost. */
bool8_t vkr_recording_mark(VkrRecording *recording, VkrRecordingMarkerKind kind,
                           const char *label);

/**
 * @brief Stops the recorder, writes summary.json and releases the memory.
 * @return false_v when the stream or the summary could not be written.
 */
/* What vkr_recording_save_window wrote. */
typedef struct VkrRecordingWindow {
  uint64_t frames;
  float64_t seconds;
} VkrRecordingWindow;

/**
 * @brief Rolling recordings: writes the frames still held as a bundle at
 * `bundle_path` (created; must not exist), renumbered from frame zero, with
 * its stream and summary. The recording keeps running. Frame-loop thread.
 * @return false_v when the recording is not rolling, holds nothing yet, or
 * the bundle could not be written.
 */
bool8_t vkr_recording_save_window(VkrRecording *recording,
                                  const char *bundle_path,
                                  VkrRecordingWindow *out_window);

bool8_t vkr_recording_stop(VkrRecording *recording,
                           VkrRecorderStats *out_stats);

/** Live counters while active. */
void vkr_recording_stats(const VkrRecording *recording,
                         VkrRecorderStats *out_stats);
