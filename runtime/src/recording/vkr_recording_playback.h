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
  uint32_t first;
  uint32_t count;
} VkrRecordingPoseKey;

/* VkrRecordingPlayback.pose_applied: no sample applied, as before the
   entity's first recorded pose (vkr_recording_playback_pose_at), or a key
   whose only sample matched the scene when it was bound, which needs none. */
#define VKR_RECORDING_POSE_APPLIED_NONE UINT32_MAX
#define VKR_RECORDING_POSE_APPLIED_STATIC (UINT32_MAX - 1u)

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
  /* Frames that had no records and repeat the one before. */
  uint64_t missing_frames;
  /* The stream ends without a clean stop. */
  bool8_t truncated;

  /* Recorded poses, grouped by key and ordered by frame. */
  VkrRecordingPoseSample *pose_samples;
  uint64_t pose_sample_count;
  VkrRecordingPoseKey *pose_keys;
  uint32_t pose_key_count;
  /* The runtime's binding of the keys to live entities
     (vkr_standard_scene_recording.c): VkrEntityId values, invalid for a key
     the scene does not hold, and the sample each key last applied. */
  bool8_t poses_bound;
  uint64_t *pose_entities;
  uint32_t *pose_applied;

  float64_t duration;
  /* The playhead, in [0, duration]. */
  float64_t time;
  float32_t speed;
  bool8_t playing;
  bool8_t loop;
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

/** The camera and time of day at the playhead. */
VkrRecordingPlaybackPose
vkr_recording_playback_pose(const VkrRecordingPlayback *playback);
