#pragma once

#include "core/vkr_codec.h"
#include "math/vec.h"
#include "recording/vkr_recording.h"
#include "recording/vkr_recording_drive.h"
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
  /* Resets the session and plays it with the actions of `path`
     (docs/proposals/recordings.md, "Drive and re-simulation"). */
  VKR_SAMPLE_RECORDING_DRIVE,
  /* Ends the drive and pauses the session. */
  VKR_SAMPLE_RECORDING_DRIVE_STOP,
  /* Writes a rolling recording's held seconds into `path`, a new bundle
     directory; the rolling recording keeps running. */
  VKR_SAMPLE_RECORDING_SAVE_WINDOW,
} VkrSampleRecordingCommand;

/* Transport the client applies for a recording request: Drive resets the
   session and plays it, and pauses it when the actions run out. */
typedef enum VkrSampleRecordingTransport {
  VKR_SAMPLE_RECORDING_TRANSPORT_NONE = 0,
  VKR_SAMPLE_RECORDING_TRANSPORT_RESET,
  VKR_SAMPLE_RECORDING_TRANSPORT_PLAY,
  VKR_SAMPLE_RECORDING_TRANSPORT_PAUSE,
} VkrSampleRecordingTransport;

/* Whether Watch shows the recording's scene edits
   (docs/proposals/recordings.md, "Recorded state"). */
typedef enum VkrSampleRecordingEdits {
  /* The recording made no edits. */
  VKR_SAMPLE_RECORDING_EDITS_NONE = 0,
  /* The journals step back and forth to each frame's edits. */
  VKR_SAMPLE_RECORDING_EDITS_REPLAYED,
  /* The scene changed after the recording, or its edits left the undo
     history: Watch shows the scene as it is now. */
  VKR_SAMPLE_RECORDING_EDITS_UNAVAILABLE,
} VkrSampleRecordingEdits;

/* What the latest drive measured (VkrRecordingDrive). */
typedef struct VkrSampleRecordingDriveResult {
  bool8_t valid;
  char path[VKR_RECORDING_PATH_MAX];
  /* Recorded ticks replayed out of the recording's, and those compared. */
  uint64_t ticks;
  uint64_t ticks_total;
  uint64_t ticks_compared;
  float64_t distance;
  float64_t max_distance;
  float64_t path_length;
  /* The first recorded tick and frame past VKR_RECORDING_DRIVE_TOLERANCE,
     UINT64_MAX for none. */
  uint64_t divergent_tick;
  uint64_t divergent_frame;
  /* Re-simulation: ticks whose hashes matched; at the first that did not,
     its recorded tick and frame, the subsystems that differ and the
     simulated entities in the transform buckets that differ. */
  bool8_t resim;
  bool8_t hash_diverged;
  uint64_t ticks_hashed;
  uint64_t hash_tick;
  uint64_t hash_frame;
  char subsystems[160];
  char entities[192];
} VkrSampleRecordingDriveResult;

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
  /* SETTINGS: the recorded times `loop` loops between; an empty span loops
     the whole recording. */
  float64_t loop_start;
  float64_t loop_end;
  /* START: the compressor the client links; NULL stores chunks raw. */
  const VkrCodec *codec;
  /* START: what the recording measures. */
  VkrRecordingTier tier;
  /* START: every measurement the engine has: all registry metrics, physics
     counts, per-pass GPU times every frame and event subjects (implies
     Tier 1). */
  bool8_t all_measurements;
  /* START: every thread's CPU zones and the GPU pass starts for a timeline
     (VkrRecordingConfig.zones). */
  bool8_t zones;
  /* START: keep only this many recent seconds in memory and write a bundle
     only on SAVE_WINDOW (VkrRecordingConfig.rolling_seconds); zero records
     into `path`. */
  float64_t rolling_seconds;
  /* DRIVE: modules steer the driven player back toward the recorded one. */
  bool8_t follow;
  /* DRIVE: re-simulate: replay the session's seed and clock and stop at the
     first tick whose hashes differ from the recording's. */
  bool8_t resim;
} VkrSampleRecordingRequest;

typedef struct VkrSampleRecordingStatus {
  /* The outcome of the latest request. */
  uint64_t token;
  bool8_t ok;
  char message[192];
  /* The recording in progress. */
  bool8_t recording;
  /* The recording in progress takes every measurement
     (VkrRecordingConfig.all_measurements). */
  bool8_t recording_all_measurements;
  /* The recording in progress keeps only its last this many seconds; zero
     for a recording into a bundle. */
  float64_t recording_rolling_seconds;
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
  /* How the open playback shows the recording's edits. */
  VkrSampleRecordingEdits playback_edits;
  /* A drive in progress, and what the drive in progress or the last one
     measured. */
  bool8_t driving;
  VkrSampleRecordingDriveResult drive;
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
  /* The scene journals' summed revision the recording last saw, and whether
     the recording wrote where the journals stood at its start. */
  uint64_t edit_revision;
  bool8_t edit_start_written;
  /* Whole-frame and per-pass GPU timing and event subjects as they were
     before a recording with metrics took them over; restored at stop. */
  bool8_t restore_gpu_timing;
  bool8_t submission_timing;
  bool8_t pass_timing;
  bool8_t event_subjects;
  /* Transport for the client to apply, in order
     (vkr_sample_recording_take_transport). */
  VkrSampleRecordingTransport transport[2];
  uint32_t transport_count;
  /* The playback whose edits the journals replay (its open serial, zero
     for none) and the journals' position when it opened, where closing
     returns them. */
  uint64_t playback_edits_serial;
  uint64_t playback_edit_end;
  /* The drive saw the session simulate; a stop after that ends it. */
  bool8_t drive_started;
  uint64_t drive_token;
} VkrSampleRecordingState;

/** Applies one request; `scene_path` names the level for the summary. */
void vkr_sample_recording_apply(struct VkrStandardSceneRuntime *runtime,
                                VkrSampleRecordingState *state,
                                const VkrSampleRecordingRequest *request,
                                String8 scene_path);

/** The next transport a request or a drive asks for, in order; false_v when
    none is waiting. */
bool8_t vkr_sample_recording_take_transport(VkrSampleRecordingState *state,
                                            VkrSampleRecordingTransport *out);

/** Refreshes the live part of the status before the next build.
 * `edit_revision` sums the loaded scenes' journal revisions: a change while
 * recording adds an edit marker and records `edit_sequence`, the newest edit
 * an undo would revert across the journals. */
void vkr_sample_recording_publish(struct VkrStandardSceneRuntime *runtime,
                                  VkrSampleRecordingState *state,
                                  uint64_t edit_revision,
                                  uint64_t edit_sequence);
