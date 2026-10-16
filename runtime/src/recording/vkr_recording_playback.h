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

/** The camera and time of day at the playhead. */
VkrRecordingPlaybackPose
vkr_recording_playback_pose(const VkrRecordingPlayback *playback);
