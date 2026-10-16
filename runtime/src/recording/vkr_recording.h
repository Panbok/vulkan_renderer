#pragma once

#include "core/vkr_codec.h"
#include "defines.h"
#include "memory/vkr_dmemory.h"
#include "recording/vkr_recorder.h"

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

/* VkrRecordingFrame.flags */
#define VKR_RECORDING_FRAME_SIMULATING 1u
#define VKR_RECORDING_FRAME_SCRIPT_CAMERA 2u
#define VKR_RECORDING_FRAME_RENDERING_STOPPED 4u

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

typedef enum VkrRecordingMarkerKind {
  VKR_RECORDING_MARKER_NOTE = 1u,
  VKR_RECORDING_MARKER_BUG = 2u,
  VKR_RECORDING_MARKER_STEP = 3u,
} VkrRecordingMarkerKind;

/* Marker records are a u32 kind followed by a UTF-8 label. */
typedef struct VkrRecordingMarker {
  uint64_t frame;
  float64_t time;
  uint32_t kind;
  char label[VKR_RECORDING_LABEL_MAX + 1u];
} VkrRecordingMarker;

/* The streams every runtime recording declares. */
const VkrRecordingStream *vkr_recording_streams(uint32_t *out_count);

const char *vkr_recording_marker_kind_name(uint32_t kind);

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
} VkrRecordingConfig;

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
