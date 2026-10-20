#pragma once

#include "assets/vkr_recording_format.h"
#include "containers/str.h"
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

/* Distinct event names and events a load keeps; later ones are dropped and
   counted in events_dropped. */
#define VKR_RECORDING_DATA_EVENT_NAME_MAX 64u
#define VKR_RECORDING_DATA_EVENT_MAX (1u << 16u)
/* Events a spike lists from the frames around it, and a frame range. */
#define VKR_RECORDING_ANALYSIS_SPIKE_EVENT_MAX 4u
#define VKR_RECORDING_FRAMES_EVENT_MAX 200u

/* A record of stream "event": an engine event (VKR_RECORDING_EVENT_*) with
   how many it counts and the milliseconds they took, or a script's event,
   which counts one and carries no time. */
typedef struct VkrRecordingDataEvent {
  uint64_t frame;
  /* Index into VkrRecordingData.event_names. */
  uint32_t name;
  uint32_t count;
  float32_t ms;
  bool8_t engine;
} VkrRecordingDataEvent;

typedef struct VkrRecordingDataEventName {
  char name[VKR_RECORDING_NAME_MAX + 1u];
} VkrRecordingDataEventName;

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

/* A named allocator of a Tier 1 recording, by the id its rows carry. */
typedef struct VkrRecordingAllocator {
  char name[VKR_RECORDING_ALLOCATOR_NAME_MAX + 1u];
  /* VkrAllocatorType, and 1 for a raw arena. */
  uint8_t type;
  uint8_t arena;
} VkrRecordingAllocator;

/* One allocator's sample in one frame (vkr_recording_format.h). Counts are
   since its previous sample; arena bytes are zero without an arena. */
typedef struct VkrRecordingAllocatorRow {
  uint32_t allocator;
  uint32_t scope_depth_high;
  uint32_t allocs;
  uint32_t frees;
  uint32_t reallocs;
  uint32_t scopes;
  uint64_t live_bytes;
  uint64_t used_bytes;
  uint64_t committed_bytes;
  uint64_t reserved_bytes;
  uint64_t peak_used_bytes;
} VkrRecordingAllocatorRow;

/* An object of the draw census and its world box. */
typedef struct VkrRecordingDrawObject {
  uint32_t object_id;
  float32_t min[3];
  float32_t max[3];
} VkrRecordingDrawObject;

/* The objects' boxes as recorded at `frame`, sorted by object id, in effect
   until the next set. */
typedef struct VkrRecordingDrawBounds {
  uint64_t frame;
  /* World partition cell size in metres; zero without a partition. */
  float32_t cell_size;
  VkrRecordingDrawObject *objects;
  uint32_t object_count;
} VkrRecordingDrawBounds;

/* One candidate order of the census: each candidate's object id. */
typedef struct VkrRecordingDrawIds {
  uint64_t frame;
  uint64_t generation;
  uint32_t *ids;
  uint32_t count;
} VkrRecordingDrawIds;

/* One frame's census: its candidates' 4-bit VKR_RECORDING_DRAW_STATE_*
   values, eight per word, the ids they follow and its flags. */
typedef struct VkrRecordingDrawFrame {
  const uint32_t *words;
  uint32_t count;
  const VkrRecordingDrawIds *ids;
  /* VKR_RECORDING_DRAW_STATES_* */
  uint32_t flags;
  uint32_t overflow;
} VkrRecordingDrawFrame;

/* Zone ids a recording names at most, as the loader keeps them. */
#define VKR_RECORDING_DATA_ZONE_MAX 4096u

/* The thread class of a zone: the frame loop, the render thread, or a job
   worker. */
#define VKR_RECORDING_ZONE_THREAD_MAIN 0u
#define VKR_RECORDING_ZONE_THREAD_RENDER 1u
#define VKR_RECORDING_ZONE_THREAD_WORKER 2u

typedef struct VkrRecordingZoneName {
  char name[VKR_RECORDING_NAME_MAX + 1u];
} VkrRecordingZoneName;

/* One zone's calls on one thread class in one frame: how many ended there,
   their summed and longest time. */
typedef struct VkrRecordingZoneRow {
  uint16_t zone;
  uint8_t thread;
  uint8_t depth;
  uint32_t calls;
  float32_t total_ms;
  float32_t max_ms;
} VkrRecordingZoneRow;

/* Text in a recorded graph's names, by offset and length. */
typedef struct VkrRecordingGraphText {
  uint32_t offset;
  uint32_t length;
} VkrRecordingGraphText;

/* A pass of a recorded graph shape (vkr_recording_format.h, "graph"). */
typedef struct VkrRecordingGraphPass {
  VkrRecordingGraphText name;
  /* VKR_RECORDING_GRAPH_KIND_* and VKR_RECORDING_GRAPH_PASS_* */
  uint8_t kind;
  uint8_t flags;
  /* Execution position; UINT32_MAX when not scheduled. */
  uint32_t order;
  /* uses[first_use, first_use + use_count) of the graph. */
  uint32_t first_use;
  uint32_t use_count;
  /* The recording's pass of the same name, whose rows time it, or
     UINT32_MAX. */
  uint32_t pass;
} VkrRecordingGraphPass;

typedef struct VkrRecordingGraphUse {
  /* The graph's resource of that name, or UINT32_MAX. */
  uint32_t resource;
  /* VKR_RECORDING_GRAPH_ROLE_* and VKR_RECORDING_GRAPH_USE_* */
  uint8_t role;
  uint8_t flags;
} VkrRecordingGraphUse;

typedef struct VkrRecordingGraphResource {
  VkrRecordingGraphText name;
  VkrRecordingGraphText format;
  VkrRecordingGraphText lifetime;
  /* VKR_RECORDING_GRAPH_RESOURCE_* */
  uint32_t flags;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t layers;
  uint32_t mip_levels;
  uint32_t samples;
  uint64_t bytes;
  uint32_t first_order;
  uint32_t last_order;
} VkrRecordingGraphResource;

/* One shape of the render graph, in effect from `frame` until the next
   graph's frame. */
typedef struct VkrRecordingGraph {
  uint64_t frame;
  uint32_t viewport_width;
  uint32_t viewport_height;
  float32_t render_scale;
  bool8_t truncated;
  /* Scheduled passes in execution order, then the unscheduled ones. */
  VkrRecordingGraphPass *passes;
  uint32_t pass_count;
  uint32_t scheduled_count;
  VkrRecordingGraphUse *uses;
  uint32_t use_count;
  VkrRecordingGraphResource *resources;
  uint32_t resource_count;
  const char *names;
  uint32_t name_bytes;
} VkrRecordingGraph;

/* A pipeline the renderer created (Tier 1, ADR-130): the fields of its
   pipeline record, the first frame it was recorded under and its
   statistics, pipeline_stat_count from pipeline_stats[stat_first]. */
typedef struct VkrRecordingPipeline {
  char name[VKR_RECORDING_PIPELINE_NAME_MAX + 1u];
  uint64_t frame;
  uint32_t kind;
  float32_t compile_ms;
  uint32_t max_threads;
  uint32_t execution_width;
  uint32_t threadgroup_memory;
  uint32_t registers;
  uint32_t spill_bytes;
  uint32_t instructions;
  uint32_t stat_first;
  uint32_t stat_count;
} VkrRecordingPipeline;

typedef struct VkrRecordingPipelineStat {
  char name[VKR_RECORDING_PIPELINE_STAT_NAME_MAX + 1u];
  float64_t value;
} VkrRecordingPipelineStat;

/* A GPU counter imported from an Instruments trace (ADR-130), with its
   average, 95th percentile and largest frame mean over the frames the trace
   covered; NaN without any. */
typedef struct VkrRecordingGpuCounter {
  char name[VKR_RECORDING_GPU_COUNTER_NAME_MAX + 1u];
  /* VkrRecordingGpuCounterUnit */
  uint32_t unit;
  float32_t mean;
  float32_t p95;
  float32_t max;
} VkrRecordingGpuCounter;

/* A loaded recording. Every array lives in one allocation, the pass rows in
   a second and the graphs in a third, that vkr_recording_data_free
   releases; pointers stay valid until then. */
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
  /* Degrees and metres, as VkrRecordingCamera; zero in recordings whose
     camera records lack them. */
  float32_t *pitches;
  float32_t *fovs;
  float32_t *far_clips;
  uint8_t *camera_valid;
  /* The player of the frame's last tick (vkr_rec_player): three positions
     and a speed per frame where player_valid is 1. has_player says any
     frame has one: editor recordings declare the player stream even when
     no Play session wrote to it. */
  bool8_t has_player;
  float64_t *player_positions;
  float32_t *player_speeds;
  uint8_t *player_valid;
  /* Frames without a frame record. */
  uint64_t missing_frames;

  VkrRecordingDataMarker *markers;
  uint32_t marker_count;
  /* By frame, then as recorded; names in first-seen order. */
  VkrRecordingDataEvent *events;
  uint32_t event_count;
  VkrRecordingDataEventName *event_names;
  uint32_t event_name_count;
  uint64_t events_dropped;
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

  /* Tier 1 named allocators: frame f's samples are allocator_row_counts[f]
     rows from allocator_rows[allocator_first[f]]. NULL without any. */
  void *allocator_block;
  uint64_t allocator_block_bytes;
  VkrRecordingAllocator *allocators;
  uint32_t allocator_count;
  VkrRecordingAllocatorRow *allocator_rows;
  uint64_t allocator_row_count;
  uint64_t *allocator_first;
  uint16_t *allocator_row_counts;

  /* CPU zones (rec.start zones), reduced per frame: frame f's rows are
     zone_row_counts[f] rows from zone_rows[zone_first[f]], by zone then
     thread. NULL without zones. */
  void *zone_block;
  uint64_t zone_block_bytes;
  VkrRecordingZoneName *zone_names;
  uint32_t zone_name_count;
  VkrRecordingZoneRow *zone_rows;
  uint64_t zone_row_count;
  uint64_t *zone_first;
  uint32_t *zone_row_counts;
  uint64_t zones_cut;

  /* Tier 1 draw census: box sets and candidate orders by frame, oldest
     first, and per frame the state words from draw_words[draw_first[f]]
     for draw_candidates[f] candidates following draw_id_table[f]
     (UINT32_MAX without a census). NULL without a census. */
  void *draw_block;
  uint64_t draw_block_bytes;
  VkrRecordingDrawBounds *draw_bounds;
  uint32_t draw_bounds_count;
  VkrRecordingDrawIds *draw_ids;
  uint32_t draw_ids_count;
  uint32_t *draw_words;
  uint64_t draw_word_count;
  uint64_t *draw_first;
  uint32_t *draw_candidates;
  uint32_t *draw_id_table;
  uint32_t *draw_flags;
  uint32_t *draw_overflow;
  uint64_t draw_frames;
  uint64_t draw_words_cut;

  /* Tier 1 render graph shapes by frame, oldest first; NULL without any. */
  void *graph_block;
  uint64_t graph_block_bytes;
  VkrRecordingGraph *graphs;
  uint32_t graph_count;

  /* Tier 1 hardware (ADR-130): the device's facts, the first of each key,
     and the renderer's pipelines in creation order with their statistics.
     NULL without any. */
  void *hardware_block;
  uint64_t hardware_block_bytes;
  VkrRecordingHardwareFact *hardware;
  uint32_t hardware_count;
  VkrRecordingPipeline *pipelines;
  uint32_t pipeline_count;
  VkrRecordingPipelineStat *pipeline_stats;
  uint32_t pipeline_stat_count;

  /* GPU counters imported from an Instruments trace
     (VKR_RECORDING_GPU_COUNTERS_FILE in the bundle, ADR-130); NULL without
     the file. Read them with vkr_recording_data_gpu_counter and
     vkr_recording_data_gpu_counter_pass. A frame's value is the counter's
     mean over its GPU work, from the start of the frame's first command
     buffer to the end of its last; a pass's, the mean over its GPU work in
     the GPU-timed frames the trace covered, gpu_counter_pass_frames[p] of
     them. gpu_counters_bad says the bundle's file did not parse. */
  void *gpu_counter_block;
  uint64_t gpu_counter_block_bytes;
  VkrRecordingGpuCounter *gpu_counters;
  uint32_t gpu_counter_count;
  float32_t *gpu_counter_frames;
  float32_t *gpu_counter_passes;
  uint32_t *gpu_counter_pass_frames;
  /* Frames the trace covered, the first and the last of them. */
  uint64_t gpu_counter_frame_count;
  uint64_t gpu_counter_first_frame;
  uint64_t gpu_counter_last_frame;
  /* The ns the file's samples were averaged over; the trace's clock
     signposts, and how far their offsets to the CPU clock spread, in ns. */
  uint32_t gpu_counter_bucket_ns;
  uint32_t gpu_counter_anchors;
  uint32_t gpu_counter_spread_ns;
  bool8_t gpu_counters_bad;

  /* From the IDENTITY chunk: the graphics pipeline class, "tiled" (Metal) or
     "desktop" (Vulkan), and the OS; empty when it names none. */
  char pipeline[16];
  char os[16];
  /* The Scene's output size when recording started; zero when unnamed. */
  uint32_t output_width;
  uint32_t output_height;

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

/** The census of `frame`; false without one. */
bool8_t vkr_recording_data_draw_frame(const VkrRecordingData *data,
                                      uint64_t frame,
                                      VkrRecordingDrawFrame *out);

/** The box set in effect at `frame`: the last at or before it, else the
    first; UINT32_MAX without any. */
uint32_t vkr_recording_data_draw_bounds_at(const VkrRecordingData *data,
                                           uint64_t frame);

/** Candidate `index`'s VKR_RECORDING_DRAW_STATE_* value. */
static inline uint32_t vkr_recording_draw_state(const uint32_t *words,
                                                uint32_t index) {
  return (words[index / VKR_RECORDING_DRAW_STATES_PER_WORD] >>
          (4u * (index % VKR_RECORDING_DRAW_STATES_PER_WORD))) &
         0xFu;
}

/** Counter `counter`'s mean over `frame`'s GPU work; false where the
    trace did not cover it. */
bool8_t vkr_recording_data_gpu_counter(const VkrRecordingData *data,
                                       uint32_t counter, uint64_t frame,
                                       float32_t *out_value);

/** Counter `counter`'s mean over pass `pass`'s GPU work; false where no
    GPU-timed frame of the trace ran it. */
bool8_t vkr_recording_data_gpu_counter_pass(const VkrRecordingData *data,
                                            uint32_t pass, uint32_t counter,
                                            float32_t *out_value);

/** The zone rows of `frame`; NULL with a zero count without any. */
const VkrRecordingZoneRow *
vkr_recording_data_zone_rows(const VkrRecordingData *data, uint64_t frame,
                             uint32_t *out_count);

/** The allocator samples of `frame`; NULL with a zero count without any. */
const VkrRecordingAllocatorRow *
vkr_recording_data_allocator_rows(const VkrRecordingData *data, uint64_t frame,
                                  uint32_t *out_count);

/** The graph shape in effect at `frame`: the last one recorded at or before
    it, else the first; UINT32_MAX without graphs. */
uint32_t vkr_recording_data_graph_at(const VkrRecordingData *data,
                                     uint64_t frame);

/** Text of a graph's names, not null-terminated. */
String8 vkr_recording_graph_text(const VkrRecordingGraph *graph,
                                 VkrRecordingGraphText text);

/* ------------------------------------------------------------------------ */
/* Analysis                                                                  */

typedef struct VkrRecordingAnalysisOptions {
  /* Zero selects the median of the recording's frame.present_interval, or
     VKR_RECORDING_ANALYSIS_BUDGET_MS without one. */
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
/* Frames before a spike searched for the GPU work it waited on: the most
   frames either backend keeps in flight, plus one. */
#define VKR_RECORDING_ANALYSIS_GPU_LOOKBACK 4u

/* Bytes of a cause's evidence text, with its terminator. */
#define VKR_RECORDING_CAUSE_EVIDENCE_MAX 160u

/* What a cause of a slow frame is (vkr_recording_explain): time a CPU
   stage, a pass, the GPU frame, presentation, another "ms" series or an
   engine event added to the frame, or a condition that slows frames
   without a time of its own (thermal and below). */
typedef enum VkrRecordingCauseKind {
  VKR_RECORDING_CAUSE_UNKNOWN = 0,
  VKR_RECORDING_CAUSE_CPU_STAGE,
  VKR_RECORDING_CAUSE_GPU_PASS,
  VKR_RECORDING_CAUSE_CPU_PASS,
  VKR_RECORDING_CAUSE_GPU_FRAME,
  VKR_RECORDING_CAUSE_PRESENT,
  VKR_RECORDING_CAUSE_METRIC,
  VKR_RECORDING_CAUSE_PIPELINE_CREATION,
  VKR_RECORDING_CAUSE_SHADER_LOAD,
  VKR_RECORDING_CAUSE_ASSET_LOAD,
  VKR_RECORDING_CAUSE_SCRIPT_RELOAD,
  VKR_RECORDING_CAUSE_THERMAL,
  VKR_RECORDING_CAUSE_MEMORY_PRESSURE,
  VKR_RECORDING_CAUSE_PAGE_FAULTS,
  VKR_RECORDING_CAUSE_ERRORS,
  VKR_RECORDING_CAUSE_KIND_COUNT,
} VkrRecordingCauseKind;

typedef struct VkrRecordingCause {
  VkrRecordingCauseKind kind;
  /* The series, pass or event; empty for VKR_RECORDING_CAUSE_UNKNOWN. */
  char name[VKR_RECORDING_NAME_MAX + 1u];
  /* Where the measurement comes from: the explained frame, the GPU frame
     its wait stalled on, or the frame whose pass rows stand in for it. */
  uint64_t frame;
  /* true_v: excess_ms is time over the typical frame. false_v: a
     condition, and value is its sample. */
  bool8_t measured;
  float64_t excess_ms;
  float64_t value;
  char evidence[VKR_RECORDING_CAUSE_EVIDENCE_MAX];
} VkrRecordingCause;

/** "cpu_stage", "gpu_pass", ..., "unknown": the JSON name of `kind`. */
const char *vkr_recording_cause_kind_name(VkrRecordingCauseKind kind);

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
  /* When the frame loop's wait for the renderer leads the contributors: the
     frame among the VKR_RECORDING_ANALYSIS_GPU_LOOKBACK before `frame`, or
     `frame` itself, whose gpu.submission grew most above its median, and
     by how much. A GPU spike in frame N stalls the frame loop only once
     the frames in flight run out, a few frames later. UINT64_MAX when no
     such frame grew by a quarter of the wait's excess. */
  uint64_t gpu_frame;
  float64_t gpu_ms;
  float64_t gpu_excess;
  /* Tier 1: the passes that grew most in `pass_frame`, positive excesses
     only, largest first. pass_frame is `frame` when its rows carry GPU
     times, else the run's slowest frame that has them, else `frame` with
     CPU times; UINT64_MAX without rows. */
  uint64_t pass_frame;
  bool8_t pass_gpu;
  VkrRecordingPassContributor passes[VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX];
  uint32_t pass_contributor_count;
  /* The first cause vkr_recording_explain ranks for `frame`. */
  VkrRecordingCause cause;
} VkrRecordingSpike;

/* Slow frames: valid frames over VKR_RECORDING_ANALYSIS_SLOW_FACTOR times
   the budget, so a vsync-paced frame that ran a little long does not count
   while one that missed its interval does. The analysis compares them
   with the frames within it and keeps, per kind, this many largest
   differences, and counts runs of adjacent slow frames by length, the last
   bin holding longer runs. */
#define VKR_RECORDING_ANALYSIS_SLOW_FACTOR 1.2
#define VKR_RECORDING_ANALYSIS_SLOW_MAX 5u
#define VKR_RECORDING_ANALYSIS_RUN_BINS 8u

/* An "ms" series or a pass whose mean over the slow frames exceeds its mean
   over the other frames. `lag` 1 compares the row before each frame: a
   frame's render-thread work and GPU time land in the row before the frame
   loop iteration that waits for them. A pass absent from a frame counts as
   zero, so a pass that runs only in slow frames shows. */
typedef struct VkrRecordingSlowDelta {
  uint32_t index;
  uint32_t lag;
  float64_t normal_ms;
  float64_t slow_ms;
} VkrRecordingSlowDelta;

/* A bit of a flag series (packet.static_repack_reasons,
   packet.publication_changes, packet.publication_pending) set in a larger
   share of the slow frames than of the others, at the same lag rule. */
typedef struct VkrRecordingSlowFlag {
  uint32_t series;
  /* Index into the analysis's flag name table: the name of the bit. */
  uint32_t flag;
  uint32_t lag;
  float64_t normal_share;
  float64_t slow_share;
} VkrRecordingSlowFlag;

typedef struct VkrRecordingSlowFrames {
  float64_t threshold_ms;
  uint64_t frames;
  uint64_t normal_frames;
  uint64_t runs;
  uint64_t run_bins[VKR_RECORDING_ANALYSIS_RUN_BINS];
  uint64_t longest_run;
  uint64_t longest_run_first;
  /* Largest difference first. */
  VkrRecordingSlowDelta series[VKR_RECORDING_ANALYSIS_SLOW_MAX];
  uint32_t series_count;
  /* GPU times of frames whose rows carry them. */
  VkrRecordingSlowDelta passes[VKR_RECORDING_ANALYSIS_SLOW_MAX];
  uint32_t pass_count;
  VkrRecordingSlowFlag flags[VKR_RECORDING_ANALYSIS_SLOW_MAX];
  uint32_t flag_count;
} VkrRecordingSlowFrames;

/** The name of flag `flag` of a VkrRecordingSlowFlag. */
const char *vkr_recording_analysis_flag_name(uint32_t flag);

typedef struct VkrRecordingEventTotal {
  uint64_t records;
  uint64_t count;
  float64_t ms;
} VkrRecordingEventTotal;

typedef struct VkrRecordingAnalysis {
  VkrAllocator *allocator;
  void *block;
  uint64_t block_bytes;

  float64_t budget_ms;
  /* Where budget_ms came from: "option", "frame.present_interval" (the
     median interval presentation paced the frames to: the display's
     refresh with vertical sync, or the frame limit) or "default"
     (VKR_RECORDING_ANALYSIS_BUDGET_MS). */
  const char *budget_source;
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
  /* Per event name, in VkrRecordingData.event_names order: records, the
     events they count and their milliseconds. */
  VkrRecordingEventTotal event_totals[VKR_RECORDING_DATA_EVENT_NAME_MAX];
  VkrRecordingSlowFrames slow;
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

/* One segment of a recording (ADR-122): its frames, cut to the recording,
   their times, frame-time statistics over its valid frames, the frames over
   the analysis's budget and twice it, the analysis's spikes whose worst
   frame lies inside, and its slowest frame. */
typedef struct VkrRecordingSegmentStats {
  uint64_t first;
  uint64_t last;
  float64_t start_seconds;
  float64_t end_seconds;
  VkrRecordingStats frame_time;
  uint64_t over_budget;
  uint64_t over_double_budget;
  uint32_t spikes;
  uint64_t worst_frame;
  float64_t worst_ms;
} VkrRecordingSegmentStats;

/**
 * @brief Describes frames `first` to `last` of `data`, analyzed as
 * `analysis`; `scratch` lends the frame-time samples until it returns.
 * @return false when no frame of the span is in the recording or the
 * samples cannot be allocated.
 */
bool8_t vkr_recording_segment_stats(const VkrRecordingData *data,
                                    const VkrRecordingAnalysis *analysis,
                                    uint64_t first, uint64_t last,
                                    VkrAllocator *scratch,
                                    VkrRecordingSegmentStats *out);

/* ------------------------------------------------------------------------ */
/* Explanation                                                               */

/*
 * Why one frame was slow, against the typical frame: its frame time against
 * the median of the VKR_RECORDING_ANALYSIS_WINDOW frames around it (a
 * spike's median), and every series and pass against its median over the
 * whole recording (the analysis statistics).
 *
 * Causes rank in two groups. Measured causes come first, largest excess
 * first, when the frame took longer than its typical frame (the lower of
 * its window median and the recording's median frame time): the "ms"
 * series a spike's contributors take, with a frame-loop
 * wait for the renderer named as the GPU frame it stalled on (as a spike's
 * gpu_wait), the passes that grew in the pass frame when it is the frame
 * itself or in a spike's run, and the engine events of the frame by their
 * time. Each needs at least
 * VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS. Conditions follow, in this order:
 * thermal level serious or critical, critical memory pressure, major page
 * faults, GPU errors, log errors and missed presents. With neither, the
 * one cause is VKR_RECORDING_CAUSE_UNKNOWN. A kept spike's `cause` is the
 * first cause of its frame.
 */
#define VKR_RECORDING_EXPLAIN_STAGE_MAX 16u
#define VKR_RECORDING_EXPLAIN_PASS_MAX 5u
#define VKR_RECORDING_EXPLAIN_EVENT_MAX 8u
#define VKR_RECORDING_EXPLAIN_SYSTEM_MAX 10u
#define VKR_RECORDING_EXPLAIN_FLAG_MAX 24u
#define VKR_RECORDING_EXPLAIN_CAUSE_MAX 12u
/* Frames searched either side for pass rows with GPU times: one tiled
   sampling period. */
#define VKR_RECORDING_EXPLAIN_PASS_REACH 8u
#define VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS 0.25

/* A series (index into VkrRecordingData.series) or a pass (pass id) in the
   frame: its value, its median over the recording and the difference. */
typedef struct VkrRecordingExplainValue {
  uint32_t index;
  float64_t value;
  float64_t median;
  float64_t excess;
} VkrRecordingExplainValue;

/* The engine events of one name in the frame. */
typedef struct VkrRecordingExplainEvent {
  /* Index into VkrRecordingData.event_names. */
  uint32_t name;
  uint32_t count;
  float64_t ms;
} VkrRecordingExplainEvent;

/* A platform or health series in the frame (ADR-125). */
typedef struct VkrRecordingExplainSystem {
  uint32_t series;
  bool8_t valid;
  float64_t value;
  /* os.memory.footprint_bytes only: value minus the previous frame's. */
  bool8_t has_delta;
  float64_t delta;
} VkrRecordingExplainSystem;

/* A flag bit (vkr_recording_analysis_flag_name) set in the frame (lag 0)
   or in the row before it (lag 1), where render-thread work lands. */
typedef struct VkrRecordingExplainFlag {
  uint32_t flag;
  uint32_t lag;
} VkrRecordingExplainFlag;

typedef struct VkrRecordingExplanation {
  uint64_t frame;
  float64_t time;
  /* 1-based rank of the kept spike whose worst frame this is; 0 for none. */
  uint32_t spike_rank;
  bool8_t frame_valid;
  float64_t frame_ms;
  /* Window median; median_valid is false_v when the window has no frame. */
  bool8_t median_valid;
  float64_t median_ms;
  float64_t budget_ms;
  /* The cpu.* stages valid in the frame, largest excess first. */
  VkrRecordingExplainValue stages[VKR_RECORDING_EXPLAIN_STAGE_MAX];
  uint32_t stage_count;
  /* gpu.submission of the frame, when valid. */
  bool8_t gpu_valid;
  float64_t gpu_ms;
  float64_t gpu_median;
  /* As VkrRecordingSpike.gpu_frame; UINT64_MAX for none. */
  uint64_t gpu_wait_frame;
  float64_t gpu_wait_ms;
  float64_t gpu_wait_excess;
  /* The frame whose rows explain this one: a spike's pass_frame, else the
     frame itself or the nearest frame with GPU times within
     VKR_RECORDING_EXPLAIN_PASS_REACH, else the frame's CPU times;
     UINT64_MAX without rows. Passes are those that grew, largest excess
     first. */
  uint64_t pass_frame;
  bool8_t pass_gpu;
  VkrRecordingExplainValue passes[VKR_RECORDING_EXPLAIN_PASS_MAX];
  uint32_t pass_count;
  /* Engine events of the frame, by name in first-seen order. */
  VkrRecordingExplainEvent events[VKR_RECORDING_EXPLAIN_EVENT_MAX];
  uint32_t event_count;
  /* The series of the system table the recording has. */
  VkrRecordingExplainSystem system[VKR_RECORDING_EXPLAIN_SYSTEM_MAX];
  uint32_t system_count;
  VkrRecordingExplainFlag flags[VKR_RECORDING_EXPLAIN_FLAG_MAX];
  uint32_t flag_count;
  /* Ranked; at least one. */
  VkrRecordingCause causes[VKR_RECORDING_EXPLAIN_CAUSE_MAX];
  uint32_t cause_count;
} VkrRecordingExplanation;

/**
 * @brief Explains `frame` of an analyzed recording. Allocates nothing.
 * @return false_v when `frame` is not below data->frame_count.
 */
bool8_t vkr_recording_explain(const VkrRecordingData *data,
                              const VkrRecordingAnalysis *analysis,
                              uint64_t frame, VkrRecordingExplanation *out);

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

/**
 * @brief Writes an explanation as one JSON object: frame, time, spike,
 * frame_ms, median_ms, median_frames, budget_ms, budget_source, ratio,
 * excess_ms, columns, cpu, gpu, events, flags, system and causes.
 * @return false_v when the writer failed.
 */
bool8_t
vkr_recording_explain_write_json(VkrJsonWriter *json,
                                 const VkrRecordingData *data,
                                 const VkrRecordingAnalysis *analysis,
                                 const VkrRecordingExplanation *explanation);

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
