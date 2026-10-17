#pragma once

#include "assets/vkr_recording_format.h"
#include "core/vkr_codec.h"
#include "core/vkr_json_writer.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/*
 * Recording analysis (docs/proposals/recordings.md, "Performance view" and
 * "Agent workflow"): loads a recording into per-frame arrays and computes
 * frame-time statistics and spikes for summary.json, vkr_rec and the editor.
 *
 * Streams and fields are found by name in the DICTIONARY chunk:
 *
 *   frame    display_dt (s)
 *   camera   position (m, 3 elements), yaw (deg)
 *   marker     variable records: u32 kind, then the UTF-8 label
 *   metrics    "valid" (u64 bit mask), then one value per metric
 *   player     position (m, 3 elements), velocity (m/s, 3 elements)
 *   pass_name  variable records: u32 pass id, then the UTF-8 name
 *   passes     variable records of pass rows (vkr_recording_format.h)
 *
 * Bit i of a metrics record's "valid" field covers the i-th metrics field
 * after "valid", in dictionary order. A stream without a "valid" field marks
 * every value valid. Records of every stream carry the recorder frame number,
 * which indexes every array here; a lost frame keeps its index and reads as
 * an invalid sample.
 */

/* Frame-time column name for vkr_recording_frames_write_json: the series
   that vkr_recording_data_frame_ms reads. */
#define VKR_RECORDING_FRAME_MS_FIELD "frame_ms"
/* Labels longer than this are cut at load. */
#define VKR_RECORDING_DATA_LABEL_MAX 120u
/* Largest frame count a load accepts: 77 hours at 60 frames per second. */
#define VKR_RECORDING_DATA_FRAME_MAX (1ull << 24u)
/* Pass rows a load keeps, 48 MiB: about 8 minutes of 150 passes at 60
   frames per second. Later frames load without rows. */
#define VKR_RECORDING_DATA_PASS_ROW_MAX (1ull << 22u)
/* vkr_recording_frames_write_json columns of the player's last tick in a
   frame: its position and its speed in m/s. */
#define VKR_RECORDING_PLAYER_X_FIELD "player.x"
#define VKR_RECORDING_PLAYER_Y_FIELD "player.y"
#define VKR_RECORDING_PLAYER_Z_FIELD "player.z"
#define VKR_RECORDING_PLAYER_SPEED_FIELD "player.speed"
/* Prefixes of vkr_recording_frames_write_json columns that read one pass's
   GPU or CPU time, followed by the pass name. */
#define VKR_RECORDING_PASS_GPU_FIELD "pass.gpu:"
#define VKR_RECORDING_PASS_CPU_FIELD "pass.cpu:"

#define VKR_RECORDING_ANALYSIS_BUDGET_MS 16.667
#define VKR_RECORDING_ANALYSIS_SPIKE_MAX 20u
/* Frames in the neighbour window of the spike rule. */
#define VKR_RECORDING_ANALYSIS_WINDOW 19u
#define VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX 3u
/* Markers listed with a spike, and their distance from it in frames. */
#define VKR_RECORDING_ANALYSIS_SPIKE_MARKER_MAX 4u
#define VKR_RECORDING_ANALYSIS_MARKER_FRAMES 10u
/* Default bound of the analysis object, in bytes. */
#define VKR_RECORDING_ANALYSIS_JSON_BUDGET 8192u
/* Passes the bound keeps ahead of the spikes. */
#define VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST 10u
#define VKR_RECORDING_FRAMES_ROW_MAX 500u

typedef enum VkrRecordingDataError {
  VKR_RECORDING_DATA_OK = 0,
  /* No stream file at the path or inside the bundle. */
  VKR_RECORDING_DATA_OPEN,
  /* Not a recording stream, or another format version. */
  VKR_RECORDING_DATA_HEADER,
  /* The DICTIONARY chunk is missing or damaged. */
  VKR_RECORDING_DATA_DICTIONARY,
  VKR_RECORDING_DATA_EMPTY,
  /* More than VKR_RECORDING_DATA_FRAME_MAX frames. */
  VKR_RECORDING_DATA_TOO_LARGE,
  VKR_RECORDING_DATA_MEMORY,
} VkrRecordingDataError;

/* One metrics field over the whole recording. */
typedef struct VkrRecordingSeries {
  char name[VKR_RECORDING_NAME_MAX + 1u];
  /* "ms", "bytes", "count", "ratio", "percent" or empty. */
  char unit[VKR_RECORDING_NAME_MAX + 1u];
  /* frame_count values; values[i] holds a sample only when valid[i] is 1. */
  float64_t *values;
  uint8_t *valid;
} VkrRecordingSeries;

typedef struct VkrRecordingDataMarker {
  uint64_t frame;
  float64_t time;
  uint32_t kind;
  char label[VKR_RECORDING_DATA_LABEL_MAX + 1u];
} VkrRecordingDataMarker;

/* A render pass of a Tier 1 recording, by the id its rows carry. */
typedef struct VkrRecordingDataPass {
  char name[VKR_RECORDING_PASS_NAME_MAX + 1u];
} VkrRecordingDataPass;

/* One executed pass of one frame. */
typedef struct VkrRecordingPassRow {
  uint16_t pass;
  /* VKR_RECORDING_PASS_* */
  uint16_t flags;
  float32_t cpu_ms;
  /* Zero without VKR_RECORDING_PASS_GPU_VALID. */
  float32_t gpu_ms;
} VkrRecordingPassRow;

/* A loaded recording. Every array lives in one allocation, the pass rows in
   a second, that vkr_recording_data_free releases; pointers stay valid until
   then. */
typedef struct VkrRecordingData {
  VkrAllocator *allocator;
  void *block;
  uint64_t block_bytes;

  /* Highest recorder frame number of a frame, camera or metrics record,
     plus one. */
  uint64_t frame_count;
  /* Seconds from frame 0. A lost frame repeats the previous display_dt, as
     VkrRecordingPlayback does. */
  float64_t *times;
  float64_t *display_dt;
  uint8_t *frame_valid;
  /* Three per frame; a lost camera record repeats the previous pose. */
  float64_t *positions;
  float32_t *yaws;
  uint8_t *camera_valid;
  /* The player of the frame's last tick (vkr_rec_player): three positions
     and a speed per frame where player_valid is 1. has_player says the
     recording declares a player stream. */
  bool8_t has_player;
  float64_t *player_positions;
  float32_t *player_speeds;
  uint8_t *player_valid;
  /* Frames without a frame record. */
  uint64_t missing_frames;

  VkrRecordingDataMarker *markers;
  uint32_t marker_count;
  VkrRecordingSeries *series;
  uint32_t series_count;
  /* The series vkr_recording_data_frame_ms reads: "frame.wall" when it has a
     valid sample, else UINT32_MAX for display_dt in milliseconds. */
  uint32_t frame_time_series;

  /* Tier 1, all NULL or zero without pass rows. Frame f's rows are
     pass_row_counts[f] rows from pass_rows[pass_first[f]];
     pass_frame_flags[f] holds the frame's VKR_RECORDING_PASSES_* flags. A
     second record for a frame is ignored; rows of a pass id without a name
     record are dropped. */
  void *pass_block;
  uint64_t pass_block_bytes;
  VkrRecordingDataPass *passes;
  uint32_t pass_count;
  VkrRecordingPassRow *pass_rows;
  uint64_t pass_row_count;
  uint64_t *pass_first;
  uint16_t *pass_row_counts;
  uint8_t *pass_frame_flags;
  /* Frames with rows, those whose rows carry GPU times, and rows past
     VKR_RECORDING_DATA_PASS_ROW_MAX that were not loaded. */
  uint64_t pass_frames;
  uint64_t pass_gpu_frames;
  uint64_t pass_rows_cut;

  /* Reader state: whole chunks after a crash, and records chunks that failed
     their CRC or decode and were skipped. */
  bool8_t truncated;
  uint32_t bad_chunks;
} VkrRecordingData;

/**
 * @brief Loads a recording.
 * @param path A bundle directory holding stream.vkrs, or a stream file.
 * @param codec Decodes compressed chunks; NULL reads raw chunks only.
 */
VkrRecordingDataError vkr_recording_data_load(VkrRecordingData *out,
                                              VkrAllocator *allocator,
                                              const char *path,
                                              const VkrCodec *codec);

void vkr_recording_data_free(VkrRecordingData *data);

/** The series named `name`, or UINT32_MAX. */
uint32_t vkr_recording_data_find_series(const VkrRecordingData *data,
                                        const char *name);

/** Frame time in milliseconds; false_v for a frame without a sample. */
bool8_t vkr_recording_data_frame_ms(const VkrRecordingData *data,
                                    uint64_t frame, float64_t *out_ms);

/** "frame.wall" or "display_dt": where frame times come from. */
const char *vkr_recording_data_frame_source(const VkrRecordingData *data);

/** The rows of `frame`; NULL with a zero count for a frame without rows. */
const VkrRecordingPassRow *
vkr_recording_data_pass_rows(const VkrRecordingData *data, uint64_t frame,
                             uint32_t *out_count);

/** The pass named `name`, or UINT32_MAX. */
uint32_t vkr_recording_data_find_pass(const VkrRecordingData *data,
                                      const char *name);

/** The frame nearest `frame` whose rows carry GPU times, the earlier one on
    a tie, within `reach` frames; UINT64_MAX when there is none. */
uint64_t vkr_recording_data_gpu_frame_near(const VkrRecordingData *data,
                                           uint64_t frame, uint64_t reach);

/* ------------------------------------------------------------------------ */
/* Analysis                                                                  */

typedef struct VkrRecordingAnalysisOptions {
  /* Zero selects VKR_RECORDING_ANALYSIS_BUDGET_MS. */
  float64_t budget_ms;
  /* Spikes kept; zero selects VKR_RECORDING_ANALYSIS_SPIKE_MAX. */
  uint32_t spike_max;
} VkrRecordingAnalysisOptions;

/* Statistics over the valid samples of a series. Percentiles use the
   nearest rank: the ceil(p * n)-th smallest sample. */
typedef struct VkrRecordingStats {
  uint64_t valid;
  float64_t min;
  float64_t max;
  float64_t mean;
  float64_t p50;
  float64_t p95;
  float64_t p99;
} VkrRecordingStats;

/* An "ms" series that grew at a spike: value minus the series median. */
typedef struct VkrRecordingContributor {
  uint32_t series;
  float64_t value;
  float64_t excess;
} VkrRecordingContributor;

/* A pass that grew at a spike: its time minus its median, both GPU or both
   CPU as the spike's pass_gpu says. */
typedef struct VkrRecordingPassContributor {
  uint32_t pass;
  float64_t value;
  float64_t excess;
} VkrRecordingPassContributor;

/* One pass over the recording. */
typedef struct VkrRecordingPassStats {
  uint32_t pass;
  /* Over the frames with a row, and the rows with a GPU time. */
  VkrRecordingStats cpu;
  VkrRecordingStats gpu;
} VkrRecordingPassStats;

/*
 * A run of adjacent spike frames. A frame is a candidate when its frame time
 * exceeds 1.2 times the median of the 19-frame window centred on it (shifted
 * inward at the ends, invalid samples left out) and is at least 4 ms above
 * that median, or when it exceeds twice the budget.
 */
typedef struct VkrRecordingSpike {
  /* The worst frame of the run. */
  uint64_t frame;
  uint64_t first_frame;
  uint64_t last_frame;
  float64_t time;
  float64_t frame_ms;
  /* Median of the window around `frame`. */
  float64_t median_ms;
  float64_t ratio;
  /* frame_ms - median_ms: the rank order. */
  float64_t excess_ms;
  /* Positive excesses only, largest first. */
  VkrRecordingContributor contributors[VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX];
  uint32_t contributor_count;
  /* Tier 1: the passes that grew most in `pass_frame`, positive excesses
     only, largest first. pass_frame is `frame` when its rows carry GPU
     times, else the run's slowest frame that has them, else `frame` with
     CPU times; UINT64_MAX without rows. */
  uint64_t pass_frame;
  bool8_t pass_gpu;
  VkrRecordingPassContributor passes[VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX];
  uint32_t pass_contributor_count;
} VkrRecordingSpike;

typedef struct VkrRecordingAnalysis {
  VkrAllocator *allocator;
  void *block;
  uint64_t block_bytes;

  float64_t budget_ms;
  VkrRecordingStats frame_time;
  /*
   * Time-based 1% low: frame times sorted from slowest, summed until the sum
   * reaches 1% of the total frame time; this is the frame time that reaches
   * it. Slow frames weigh by the time they cover, not by their count.
   */
  float64_t low_1pct_ms;
  uint64_t over_budget;
  uint64_t over_double_budget;
  /* One per data series, in dictionary order. */
  VkrRecordingStats *series;
  /* Largest excess first, then lowest frame. */
  VkrRecordingSpike *spikes;
  uint32_t spike_count;
  /* Spikes before the options' spike_max cut. */
  uint64_t spikes_found;
  /* Tier 1: one per pass with a row, by GPU total (mean times GPU
     samples), then CPU total, then id, largest first. */
  VkrRecordingPassStats *passes;
  uint32_t pass_count;
} VkrRecordingAnalysis;

/**
 * @brief Computes statistics and spikes. Deterministic for equal inputs.
 * @param options NULL selects the defaults.
 */
bool8_t vkr_recording_analyze(const VkrRecordingData *data,
                              const VkrRecordingAnalysisOptions *options,
                              VkrAllocator *allocator,
                              VkrRecordingAnalysis *out);

void vkr_recording_analysis_free(VkrRecordingAnalysis *analysis);

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

typedef struct VkrRecordingAnalysisJsonOptions {
  /* Upper bound of the written object in bytes; zero for none. Series, in
     dictionary order, then the first VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST
     passes, then spikes, in rank order, then the other passes are cut to
     fit, and each cut is stated. */
  uint64_t budget_bytes;
  bool8_t include_series;
} VkrRecordingAnalysisJsonOptions;

/**
 * @brief Writes the analysis as one JSON object value: frame_time, series
 * and passes (as column arrays), bytes_series names and spikes. The caller
 * names it, for example "analysis" in summary.json. Fractional numbers keep
 * six significant digits.
 * @param options NULL selects VKR_RECORDING_ANALYSIS_JSON_BUDGET with series.
 * @return false_v when the writer failed.
 */
bool8_t vkr_recording_analysis_write_json(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis,
    const VkrRecordingAnalysisJsonOptions *options);

/**
 * @brief Writes frames `first` to `last`, inclusive, as one JSON object of
 * columns and rows: frame, time, camera x, y, z, then `fields`. A field is a
 * series name, VKR_RECORDING_FRAME_MS_FIELD, or VKR_RECORDING_PASS_GPU_FIELD
 * or VKR_RECORDING_PASS_CPU_FIELD followed by a pass name; an invalid
 * sample, a frame without the pass or an unknown field writes null. More than
 * `row_max` rows (zero selects VKR_RECORDING_FRAMES_ROW_MAX) are cut and
 * "truncated" says so.
 */
/** True when `name` is a column vkr_recording_frames_write_json can read. */
bool8_t vkr_recording_data_has_field(const VkrRecordingData *data,
                                     const char *name);

/**
 * @brief Writes the pass statistics as one JSON object: pass_frames,
 * pass_gpu_frames, pass_rows_cut, pass_columns and the first `top` passes
 * in analysis order (zero for all). With `frame` other than UINT64_MAX it
 * adds "frame": that frame's rows as [name, cpu_ms, gpu_ms or null], slowest
 * GPU first, then slowest CPU, after moving to the nearest frame with GPU
 * times within `reach` frames when `frame` has none.
 */
bool8_t vkr_recording_passes_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        const VkrRecordingAnalysis *analysis,
                                        uint32_t top, uint64_t frame,
                                        uint64_t reach);

bool8_t vkr_recording_frames_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        uint64_t first, uint64_t last,
                                        const char *const *fields,
                                        uint32_t field_count, uint32_t row_max);
