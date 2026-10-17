#pragma once

#include "core/vkr_codec.h"
#include "defines.h"
#include "memory/vkr_dmemory.h"
#include "recording/vkr_recording.h"

/*
 * Drive (docs/proposals/recordings.md, "Drive and re-simulation"): a
 * recording's admitted actions fed back to the current build from the
 * scene's start, and its player track as the reference. Ticks count from
 * the first tick each run drives: driven tick `base + n` replays recorded
 * tick `first + n`. Drive does not stop when the paths part; it measures
 * how far they are apart.
 */

/* Path distance, in meters, past which Drive reports a divergence. */
#define VKR_RECORDING_DRIVE_TOLERANCE 0.25

typedef struct VkrRecordingDrive {
  bool8_t open;
  VkrDMemory memory;
  VkrAllocator allocator;
  char bundle_path[VKR_RECORDING_PATH_MAX];
  /* By tick, then sequence. */
  VkrRecordingAction *actions;
  uint64_t action_count;
  /* By tick, with the recorded frame of each. */
  VkrRecordingPlayer *players;
  uint64_t *player_frames;
  uint64_t player_count;
  /* The recorded ticks Drive replays, inclusive. */
  uint64_t first_tick;
  uint64_t last_tick;

  /* The driven tick that replays `first_tick`; UINT64_MAX before the first
     driven tick. */
  uint64_t base_tick;
  uint64_t action_cursor;
  /* Driven ticks so far. */
  uint64_t ticks_driven;
  /* The last driven tick replays `last_tick` or later. */
  bool8_t finished;
  /* Modules steer the driven player back toward the recorded one
     (vkr_drive_follow). */
  bool8_t follow;

  /* Distance between the driven and the recorded player at the latest
     compared tick, its maximum, the driven path's length and the first
     recorded tick and frame past VKR_RECORDING_DRIVE_TOLERANCE, UINT64_MAX
     while none is. */
  uint64_t ticks_compared;
  float64_t distance;
  float64_t max_distance;
  float64_t path_length;
  bool8_t has_position;
  float64_t last_position[3];
  uint64_t divergent_tick;
  uint64_t divergent_frame;

  /* Re-simulation: the recording's first Play session, if it started while
     recording: its seed, the frame clock of each of its frames and the
     hashes of each of its ticks, by tick, with the frame that ran each and
     the module names hashes index. `resim` drives with them: the session
     takes the seed and the clock, and every tick's hashes are compared. */
  bool8_t has_session;
  bool8_t resim;
  uint64_t seed;
  VkrRecordingSimFrame *sim_frames;
  uint64_t sim_frame_count;
  uint64_t sim_cursor;
  VkrRecordingTickHash *hashes;
  uint64_t *hash_frames;
  uint64_t hash_count;
  uint64_t hash_cursor;
  char module_names[VKR_RECORDING_HASH_MODULE_MAX]
                   [VKR_RECORDING_HASH_MODULE_NAME_MAX + 1u];
  uint32_t module_name_count;
  /* Ticks whose hashes matched, and the first that did not: its recorded
     tick and frame, the subsystems that differ, and the transform buckets
     among them. */
  uint64_t ticks_hashed;
  bool8_t hash_diverged;
  uint64_t hash_divergent_tick;
  uint64_t hash_divergent_frame;
  char hash_subsystems[160];
  uint32_t hash_buckets;
} VkrRecordingDrive;

/**
 * @brief Loads the actions and player track of a bundle or stream file.
 * @param error Receives a one-line reason on failure, such as a recording
 * without actions.
 */
bool8_t vkr_recording_drive_open(VkrRecordingDrive *drive, const char *path,
                                 const VkrCodec *codec, char *error,
                                 uint64_t error_capacity);

void vkr_recording_drive_close(VkrRecordingDrive *drive);

/**
 * @brief The recorded actions of driven tick `tick`, in order; the first
 * call sets the base tick. Returns how many were written, at most
 * `capacity`; ticks must not go backward.
 */
uint32_t vkr_recording_drive_actions(VkrRecordingDrive *drive, uint64_t tick,
                                     VkrRecordingAction *out,
                                     uint32_t capacity);

/** The recorded player Drive starts from: the recording's first. */
const VkrRecordingPlayer *
vkr_recording_drive_start_player(const VkrRecordingDrive *drive);

/** The recorded player that driven tick `tick` replays, or NULL before the
    base tick or for a tick without one. */
const VkrRecordingPlayer *
vkr_recording_drive_reference(const VkrRecordingDrive *drive, uint64_t tick);

/** The frame clock of the next re-simulated session frame; false past the
    recording's last, which finishes the drive. */
bool8_t vkr_recording_drive_sim_frame(VkrRecordingDrive *drive,
                                      VkrRecordingSimFrame *out);

/** Compares a re-simulated tick's hashes with the recording's tick of the
    same number. False at the first tick that differs, which it describes in
    the hash_* fields; true for a match or a tick the recording has none
    for. */
bool8_t vkr_recording_drive_check_hash(VkrRecordingDrive *drive,
                                       const VkrRecordingTickHash *live,
                                       const char *const *module_names);

/** Compares the driven player at tick `tick` with the recorded one. */
void vkr_recording_drive_player(VkrRecordingDrive *drive, uint64_t tick,
                                const float64_t position[3]);
