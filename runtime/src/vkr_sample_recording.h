#pragma once

#include "core/vkr_codec.h"
#include "math/vec.h"
#include "recording/vkr_recording.h"
#include "recording/vkr_recording_playback.h"

/* vkr_standard_scene_runtime.h includes vkr_sample_runtime.h, which includes
   this header, so the runtime is only named here. */
struct VkrStandardSceneRuntime;

/*
 * The client's control of recordings (docs/proposals/recordings.md): one
 * typed request per frame, applied after the UI build, and the status the
 * next frame lends back. Playback is a transport state of its own: the
 * simulation stays stopped, scene edits are refused, and closing it restores
 * the editor camera and the level's hour.
 */

typedef enum VkrSampleRecordingCommand {
  VKR_SAMPLE_RECORDING_NONE = 0,
  /* Starts recording into `path`, a new bundle directory. */
  VKR_SAMPLE_RECORDING_START,
  VKR_SAMPLE_RECORDING_STOP,
  /* Adds a marker of `marker_kind` with `label`. */
  VKR_SAMPLE_RECORDING_MARK,
  /* Opens `path` and enters Playback, paused at frame 0. */
  VKR_SAMPLE_RECORDING_OPEN,
  /* Leaves Playback. */
  VKR_SAMPLE_RECORDING_CLOSE,
  VKR_SAMPLE_RECORDING_PLAY,
  VKR_SAMPLE_RECORDING_PAUSE,
  /* Seeks to `time` seconds. */
  VKR_SAMPLE_RECORDING_SEEK_TIME,
  /* Seeks exactly onto `frame`. */
  VKR_SAMPLE_RECORDING_SEEK_FRAME,
  /* Sets `speed` and `loop`. */
  VKR_SAMPLE_RECORDING_SETTINGS,
} VkrSampleRecordingCommand;

typedef struct VkrSampleRecordingRequest {
  VkrSampleRecordingCommand command;
  /* Echoed in the status with the outcome. */
  uint64_t token;
  char path[VKR_RECORDING_PATH_MAX];
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  char author[32];
  uint32_t marker_kind;
  char label[VKR_RECORDING_LABEL_MAX + 1u];
  float64_t time;
  uint64_t frame;
  float32_t speed;
  bool8_t loop;
  /* START: the compressor the client links; NULL stores chunks raw. */
  const VkrCodec *codec;
} VkrSampleRecordingRequest;

typedef struct VkrSampleRecordingStatus {
  /* The outcome of the latest request. */
  uint64_t token;
  bool8_t ok;
  char message[192];
  /* The recording in progress. */
  bool8_t recording;
  char recording_path[VKR_RECORDING_PATH_MAX];
  uint64_t frames;
  float64_t seconds;
  uint32_t markers;
  VkrRecorderStats stats;
  /* The open playback, borrowed for the build; NULL outside Playback. */
  const VkrRecordingPlayback *playback;
  /* The camera the last frame rendered with, in world space as recordings
     store it: what a recording wrote for its last frame, and what playback
     placed. */
  bool8_t camera_valid;
  float64_t camera_position[3];
  float32_t camera_yaw;
  float32_t camera_pitch;
} VkrSampleRecordingStatus;

typedef struct VkrSampleRecordingState {
  VkrSampleRecordingStatus status;
  /* The editor camera and the level's hour from before Playback. */
  bool8_t saved;
  Vec3 position;
  float32_t yaw;
  float32_t pitch;
  float32_t zoom;
  float32_t near_clip;
  float32_t far_clip;
  float64_t hour;
  /* The scene journals' summed revision the recording last saw. */
  uint64_t edit_revision;
} VkrSampleRecordingState;

/** Applies one request; `scene_path` names the level for the summary. */
void vkr_sample_recording_apply(struct VkrStandardSceneRuntime *runtime,
                                VkrSampleRecordingState *state,
                                const VkrSampleRecordingRequest *request,
                                String8 scene_path);

/** Refreshes the live part of the status before the next build.
 * `edit_revision` sums the loaded scenes' journal revisions: a change while
 * recording adds an edit marker. */
void vkr_sample_recording_publish(struct VkrStandardSceneRuntime *runtime,
                                  VkrSampleRecordingState *state,
                                  uint64_t edit_revision);
