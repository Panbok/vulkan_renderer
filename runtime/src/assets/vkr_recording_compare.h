#pragma once

#include "assets/vkr_recording_analysis.h"
#include "containers/str.h"
#include "core/vkr_json_writer.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/*
 * Compare (docs/proposals/recordings.md, "Performance view"): recording B
 * against recording A. Statistics differ without any alignment; the
 * frame-by-frame difference needs one, which maps every frame of A to the
 * frame of B at the same key:
 *
 *   time      seconds from the first frame
 *   marker    seconds from a marker label both carry
 *   distance  meters along the path, the player's when both recorded one,
 *             else the camera's; two runs at different speeds line up
 *   frame     the frame number, for re-runs of one recording only
 */

/* Pass differences kept, largest first. */
#define VKR_RECORDING_COMPARE_PASS_MAX 64u
/* Aligned frames of A the slowest-window search averages over. */
#define VKR_RECORDING_COMPARE_WINDOW 30u

typedef enum VkrRecordingAlign {
  VKR_RECORDING_ALIGN_TIME = 0,
  VKR_RECORDING_ALIGN_MARKER,
  VKR_RECORDING_ALIGN_DISTANCE,
  VKR_RECORDING_ALIGN_FRAME,
  VKR_RECORDING_ALIGN_COUNT,
} VkrRecordingAlign;

/* Statistics both recordings have, lower is better for each. */
typedef enum VkrRecordingCompareStatId {
  VKR_RECORDING_COMPARE_P50 = 0,
  VKR_RECORDING_COMPARE_P95,
  VKR_RECORDING_COMPARE_P99,
  VKR_RECORDING_COMPARE_MAX,
  VKR_RECORDING_COMPARE_LOW_1PCT,
  VKR_RECORDING_COMPARE_MEAN,
  VKR_RECORDING_COMPARE_OVER_BUDGET,
  VKR_RECORDING_COMPARE_RAM_PEAK,
  VKR_RECORDING_COMPARE_VRAM_PEAK,
  VKR_RECORDING_COMPARE_STAT_COUNT,
} VkrRecordingCompareStatId;

typedef struct VkrRecordingCompareStat {
  float64_t a;
  float64_t b;
  /* Both recordings measured it. */
  bool8_t valid;
} VkrRecordingCompareStat;

/* One pass by name: its mean time in A and B, GPU when both recordings
   carry GPU pass times, else CPU encode. A pass one of them lacks has
   zero there and `in_a` or `in_b` false. */
typedef struct VkrRecordingPassDelta {
  char name[VKR_RECORDING_PASS_NAME_MAX + 1u];
  float64_t a_ms;
  float64_t b_ms;
  bool8_t in_a;
  bool8_t in_b;
} VkrRecordingPassDelta;

typedef struct VkrRecordingCompareOptions {
  VkrRecordingAlign align;
  /* MARKER: the label to align on; NULL or empty takes the first marker of
     A whose label B also carries. */
  const char *marker;
} VkrRecordingCompareOptions;

typedef struct VkrRecordingCompare {
  VkrAllocator *allocator;
  void *block;
  uint64_t block_bytes;

  VkrRecordingAlign align;
  /* MARKER: the label and its time in each recording. */
  char marker[VKR_RECORDING_DATA_LABEL_MAX + 1u];
  float64_t marker_a_time;
  float64_t marker_b_time;
  /* DISTANCE: whether the players' paths were used. */
  bool8_t distance_player;

  VkrRecordingCompareStat stats[VKR_RECORDING_COMPARE_STAT_COUNT];
  /* Largest absolute difference first. */
  VkrRecordingPassDelta passes[VKR_RECORDING_COMPARE_PASS_MAX];
  uint32_t pass_count;
  bool8_t pass_gpu;

  /* One per frame of A: the frame of B at the same key, UINT64_MAX where B
     has none or either frame has no frame time. */
  uint64_t *b_frame;
  uint64_t aligned_frames;
  /* Over the aligned frames: the mean of B's frame time minus A's, and the
     VKR_RECORDING_COMPARE_WINDOW aligned frames of A where it is largest,
     UINT64_MAX without enough aligned frames. */
  float64_t delta_mean_ms;
  uint64_t worst_first;
  uint64_t worst_last;
  float64_t worst_delta_ms;
} VkrRecordingCompare;

const char *vkr_recording_align_name(VkrRecordingAlign align);

/** Reads "time", "marker", "distance" or "frame". */
bool8_t vkr_recording_align_parse(String8 name, VkrRecordingAlign *out);

/** "p50_ms", "p95_ms", ..., "ram_peak_bytes", "vram_peak_bytes". */
const char *vkr_recording_compare_stat_name(VkrRecordingCompareStatId id);

/**
 * @brief Compares B with A, both loaded and analyzed.
 * @param options NULL aligns by time.
 * @param error Receives a one-line reason on failure, such as a marker
 * label neither recording shares.
 */
bool8_t vkr_recording_compare(const VkrRecordingData *a,
                              const VkrRecordingAnalysis *a_analysis,
                              const VkrRecordingData *b,
                              const VkrRecordingAnalysis *b_analysis,
                              const VkrRecordingCompareOptions *options,
                              VkrAllocator *allocator, VkrRecordingCompare *out,
                              char *error, uint64_t error_capacity);

void vkr_recording_compare_free(VkrRecordingCompare *compare);

/** Writes the comparison as one JSON object: the alignment, the statistics
    with their differences, the passes and the slowest window. */
void vkr_recording_compare_write_json(VkrJsonWriter *json,
                                      const VkrRecordingData *a,
                                      const VkrRecordingCompare *compare);
