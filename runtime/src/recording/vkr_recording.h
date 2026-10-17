#pragma once

#include "core/vkr_codec.h"
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
