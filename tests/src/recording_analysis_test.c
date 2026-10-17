#include "recording_analysis_test.h"

#include "assets/vkr_recording_analysis.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_json.h"
#include "vkr_depot_zstd.h"

/* Recording analysis (docs/proposals/recordings.md, "Performance view"):
 * statistics, spikes and the JSON an agent reads. Each test writes a
 * synthetic recording with the runtime's stream names under tests/tmp,
 * removes it, and names the failure it detects. */

#define ANALYSIS_TEST_DT (1.0 / 60.0)
#define ANALYSIS_TEST_CHUNK_FRAMES 50u
#define ANALYSIS_TEST_NO_FRAME UINT64_MAX
#define ANALYSIS_TEST_LABEL_BYTES 120u

typedef enum AnalysisTestMetric {
  ANALYSIS_TEST_WALL = 0,
  ANALYSIS_TEST_UPDATE,
  ANALYSIS_TEST_RENDER_WAIT,
  ANALYSIS_TEST_GPU,
  ANALYSIS_TEST_BYTES_LIVE,
  ANALYSIS_TEST_DRAW_CALLS,
  ANALYSIS_TEST_METRIC_COUNT,
} AnalysisTestMetric;

typedef struct AnalysisTestFrame {
  float64_t display_dt;
  float64_t simulation_time;
  float32_t time_of_day;
  uint32_t flags;
} AnalysisTestFrame;

typedef struct AnalysisTestCamera {
  float64_t position[3];
  float32_t yaw;
  float32_t pitch;
  float32_t fov;
  float32_t near_clip;
  float32_t far_clip;
  uint32_t projection;
} AnalysisTestCamera;

typedef struct AnalysisTestMetrics {
  uint64_t valid;
  float64_t values[ANALYSIS_TEST_METRIC_COUNT];
} AnalysisTestMetrics;

static const VkrRecordingField ANALYSIS_TEST_FRAME_FIELDS[] = {
    {.name = "display_dt",
     .unit = "s",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 0u},
    {.name = "simulation_time",
     .unit = "s",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 8u},
    {.name = "time_of_day",
     .unit = "h",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 16u},
    {.name = "flags",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 20u},
};

static const VkrRecordingField ANALYSIS_TEST_CAMERA_FIELDS[] = {
    {.name = "position",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 3u,
     .offset = 0u},
    {.name = "yaw",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 24u},
    {.name = "pitch",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 28u},
    {.name = "fov",
     .unit = "deg",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 32u},
    {.name = "near_clip",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 36u},
    {.name = "far_clip",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 40u},
    {.name = "projection",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 44u},
};

static const VkrRecordingField ANALYSIS_TEST_METRIC_FIELDS[] = {
    {.name = "valid",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 0u},
    {.name = "frame.wall",
     .unit = "ms",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 8u},
    {.name = "cpu.update",
     .unit = "ms",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 16u},
    {.name = "cpu.render_wait",
     .unit = "ms",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 24u},
    {.name = "gpu.submission",
     .unit = "ms",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 32u},
    {.name = "memory.cpu.bytes.live",
     .unit = "bytes",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 40u},
    {.name = "draw.world.calls_issued",
     .unit = "count",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 1u,
     .offset = 48u},
};

/* The runtime's stream ids and names (recording/vkr_recording.h). */
static const VkrRecordingStream ANALYSIS_TEST_STREAMS[] = {
    {.id = 1u,
     .name = "frame",
     .record_size = sizeof(AnalysisTestFrame),
     .field_count = ArrayCount(ANALYSIS_TEST_FRAME_FIELDS),
     .fields = ANALYSIS_TEST_FRAME_FIELDS},
    {.id = 2u,
     .name = "camera",
     .record_size = sizeof(AnalysisTestCamera),
     .field_count = ArrayCount(ANALYSIS_TEST_CAMERA_FIELDS),
     .fields = ANALYSIS_TEST_CAMERA_FIELDS},
    {.id = 3u, .name = "marker"},
    {.id = 4u,
     .flags = VKR_RECORDING_STREAM_DROPPABLE,
     .name = "metrics",
     .record_size = sizeof(AnalysisTestMetrics),
     .field_count = ArrayCount(ANALYSIS_TEST_METRIC_FIELDS),
     .fields = ANALYSIS_TEST_METRIC_FIELDS},
};

_Static_assert(sizeof(AnalysisTestFrame) == 24u, "frame record layout");
_Static_assert(sizeof(AnalysisTestCamera) == 48u, "camera record layout");
_Static_assert(sizeof(AnalysisTestMetrics) == 56u, "metrics record layout");

typedef struct AnalysisTestScenario {
  uint64_t frames;
  /* A frame whose frame, camera and metrics records are all lost. */
  uint64_t missing_frame;
  void (*metrics)(uint64_t frame, AnalysisTestMetrics *out);
  /* Up to two markers at `frame`; returns how many. */
  uint32_t (*markers)(uint64_t frame, uint32_t *out_kinds,
                      const char **out_labels);
} AnalysisTestScenario;

typedef struct AnalysisTestMemory {
  VkrDMemory memory;
  VkrAllocator allocator;
} AnalysisTestMemory;

static void analysis_memory_create(AnalysisTestMemory *memory) {
  assert(vkr_dmemory_create(MB(4), GB(1), &memory->memory));
  memory->allocator = (VkrAllocator){.ctx = &memory->memory};
  vkr_dmemory_allocator_create(&memory->allocator);
}

static void analysis_memory_destroy(AnalysisTestMemory *memory) {
  vkr_dmemory_allocator_destroy(&memory->allocator);
}

static uint32_t analysis_test_serial = 0u;

static void analysis_test_path(const char *label, char *out,
                               uint32_t capacity) {
  snprintf(out, capacity, "%stests/tmp/recording_analysis_%u_%u_%s.vkrs",
           PROJECT_SOURCE_DIR, vkr_platform_get_process_id(),
           ++analysis_test_serial, label);
}

static void analysis_test_remove(const char *path) {
  const FilePath file = file_path_view(path);
  (void)file_remove(&file);
}

typedef struct AnalysisTestChunk {
  uint8_t data[32u * 1024u];
  uint64_t size;
} AnalysisTestChunk;

static void analysis_test_put(AnalysisTestChunk *chunk, uint16_t stream,
                              uint64_t frame, const void *payload,
                              uint32_t size) {
  const uint64_t bytes = vkr_recording_record_bytes(size);
  assert(chunk->size + bytes <= sizeof(chunk->data));
  uint8_t *at = chunk->data + chunk->size;
  MemZero(at, bytes);
  vkr_recording_record_header_store(at, stream, 0u, size, frame);
  MemCopy(at + VKR_RECORDING_RECORD_HEADER_SIZE, payload, size);
  chunk->size += bytes;
}

static void analysis_test_put_frame(AnalysisTestChunk *chunk,
                                    const AnalysisTestScenario *scenario,
                                    uint64_t frame) {
  const AnalysisTestFrame record = {
      .display_dt = ANALYSIS_TEST_DT,
      .simulation_time = (float64_t)frame * ANALYSIS_TEST_DT,
      .time_of_day = 12.0f,
  };
  const AnalysisTestCamera camera = {
      .position = {(float64_t)frame * 0.5, 1.5, -(float64_t)frame},
      .yaw = (float32_t)frame,
      .fov = 60.0f,
      .near_clip = 0.1f,
      .far_clip = 1000.0f,
  };
  AnalysisTestMetrics metrics;
  scenario->metrics(frame, &metrics);
  analysis_test_put(chunk, 1u, frame, &record, sizeof(record));
  analysis_test_put(chunk, 2u, frame, &camera, sizeof(camera));
  analysis_test_put(chunk, 4u, frame, &metrics, sizeof(metrics));
}

static void analysis_test_put_markers(AnalysisTestChunk *chunk,
                                      const AnalysisTestScenario *scenario,
                                      uint64_t frame) {
  if (!scenario->markers) {
    return;
  }
  uint32_t kinds[2];
  const char *labels[2];
  const uint32_t count = scenario->markers(frame, kinds, labels);
  for (uint32_t i = 0u; i < count; ++i) {
    uint8_t record[4u + ANALYSIS_TEST_LABEL_BYTES];
    const uint32_t length = (uint32_t)strlen(labels[i]);
    assert(length <= ANALYSIS_TEST_LABEL_BYTES);
    MemCopy(record, &kinds[i], sizeof(kinds[i]));
    MemCopy(record + 4u, labels[i], length);
    analysis_test_put(chunk, 3u, frame, record, 4u + length);
  }
}

/* Writes identity, dictionary, one records chunk per 50 frames and END. */
static void analysis_test_write(const char *path, VkrAllocator *allocator,
                                const AnalysisTestScenario *scenario) {
  VkrRecordingWriter writer;
  assert(vkr_recording_writer_open(&writer, allocator, path,
                                   vkr_depot_zstd_codec()));
  const char *identity = "{\"scene\":\"synthetic\"}";
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_IDENTITY, 0u,
                                    0u, identity, strlen(identity)));
  uint8_t *dictionary = NULL;
  uint64_t dictionary_size = 0u;
  assert(vkr_recording_dictionary_encode(
      ANALYSIS_TEST_STREAMS, ArrayCount(ANALYSIS_TEST_STREAMS), allocator,
      &dictionary, &dictionary_size));
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_DICTIONARY, 0u,
                                    0u, dictionary, dictionary_size));
  vkr_allocator_free(allocator, dictionary, dictionary_size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  static AnalysisTestChunk chunk;
  chunk.size = 0u;
  for (uint64_t frame = 0u; frame < scenario->frames; ++frame) {
    if (frame != scenario->missing_frame) {
      analysis_test_put_frame(&chunk, scenario, frame);
    }
    analysis_test_put_markers(&chunk, scenario, frame);
    const bool8_t last = frame + 1u == scenario->frames;
    if ((frame + 1u) % ANALYSIS_TEST_CHUNK_FRAMES == 0u || last) {
      const uint64_t first = frame - frame % ANALYSIS_TEST_CHUNK_FRAMES;
      assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_RECORDS,
                                        first, frame, chunk.data, chunk.size));
      chunk.size = 0u;
    }
  }
  const VkrRecordingEnd end = {
      .first_frame = 0u,
      .last_frame = scenario->frames - 1u,
      .seconds = (float64_t)scenario->frames * ANALYSIS_TEST_DT,
  };
  uint8_t encoded[VKR_RECORDING_END_SIZE];
  vkr_recording_end_encode(&end, encoded);
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_END, 0u,
                                    scenario->frames - 1u, encoded,
                                    sizeof(encoded)));
  assert(vkr_recording_writer_close(&writer));
}

/* ------------------------------------------------------------------------ */
/* Main scenario: 200 frames at 16 ms with a 24 ms plateau over frames 40 to  */
/* 79, an isolated 30 ms spike at 100, a 28 then 31 ms spike at 160 and 161,  */
/* frame 150 lost and a bug marker at 95.                                     */

static float64_t analysis_test_wall(uint64_t frame) {
  if (frame >= 40u && frame < 80u) {
    return 24.0;
  }
  switch (frame) {
  case 100u:
    return 30.0;
  case 160u:
    return 28.0;
  case 161u:
    return 31.0;
  }
  return 16.0;
}

static void analysis_test_main_metrics(uint64_t frame,
                                       AnalysisTestMetrics *out) {
  MemZero(out, sizeof(*out));
  out->valid = (1ull << ANALYSIS_TEST_METRIC_COUNT) - 1u;
  /* Draw calls are measured on even frames only; odd frames carry a value
     whose valid bit is clear. */
  if (frame % 2u == 1u) {
    out->valid &= ~(1ull << ANALYSIS_TEST_DRAW_CALLS);
  }
  out->values[ANALYSIS_TEST_WALL] = analysis_test_wall(frame);
  out->values[ANALYSIS_TEST_UPDATE] = frame == 100u ? 19.0 : 5.0;
  out->values[ANALYSIS_TEST_RENDER_WAIT] = frame == 100u ? 4.0 : 2.0;
  out->values[ANALYSIS_TEST_GPU] = frame == 161u ? 22.0 : 10.0;
  out->values[ANALYSIS_TEST_BYTES_LIVE] = 1.0e9 + (float64_t)frame * 4096.0;
  out->values[ANALYSIS_TEST_DRAW_CALLS] = 1000.0 + (float64_t)frame;
}

static uint32_t analysis_test_main_markers(uint64_t frame, uint32_t *out_kinds,
                                           const char **out_labels) {
  if (frame != 95u) {
    return 0u;
  }
  out_kinds[0] = 2u;
  out_labels[0] = "stutter here";
  return 1u;
}

static const AnalysisTestScenario ANALYSIS_TEST_MAIN = {
    .frames = 200u,
    .missing_frame = 150u,
    .metrics = analysis_test_main_metrics,
    .markers = analysis_test_main_markers,
};

static bool8_t analysis_test_near(float64_t value, float64_t expected) {
  const float64_t difference = value - expected;
  return difference < 1.0e-9 && difference > -1.0e-9;
}

static void analysis_test_load(const char *label,
                               const AnalysisTestScenario *scenario,
                               AnalysisTestMemory *memory, char *path,
                               uint32_t path_capacity,
                               VkrRecordingData *out_data) {
  analysis_test_path(label, path, path_capacity);
  analysis_test_write(path, &memory->allocator, scenario);
  assert(vkr_recording_data_load(out_data, &memory->allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
}

/* Detects: a lost frame that shifts later indices or counts as a sample, a
   clock that skips a lost frame, a metrics valid bit that is ignored, the
   frame time taken from display_dt while frame.wall exists, and wrong
   nearest-rank percentiles, mean, time-based 1% low or budget counts. */
static void test_recording_analysis_statistics(void) {
  printf("  Running test_recording_analysis_statistics...\n");
  AnalysisTestMemory memory;
  analysis_memory_create(&memory);
  char path[1024];
  VkrRecordingData data;
  analysis_test_load("statistics", &ANALYSIS_TEST_MAIN, &memory, path,
                     sizeof(path), &data);

  assert(data.frame_count == 200u && data.missing_frames == 1u);
  assert(!data.truncated && data.bad_chunks == 0u);
  assert(data.series_count == ANALYSIS_TEST_METRIC_COUNT);
  assert(data.frame_time_series == ANALYSIS_TEST_WALL);
  assert(strcmp(vkr_recording_data_frame_source(&data), "frame.wall") == 0);
  const uint32_t draw =
      vkr_recording_data_find_series(&data, "draw.world.calls_issued");
  assert(draw == ANALYSIS_TEST_DRAW_CALLS);
  assert(strcmp(data.series[draw].unit, "count") == 0);
  assert(!data.frame_valid[150] && !data.camera_valid[150] &&
         !data.series[ANALYSIS_TEST_WALL].valid[150]);
  assert(data.frame_valid[151] &&
         data.series[ANALYSIS_TEST_WALL].values[161] == 31.0);
  assert(data.positions[161u * 3u] == 80.5 && data.yaws[161] == 161.0f);
  assert(analysis_test_near(data.times[150], 150.0 * ANALYSIS_TEST_DT));
  assert(analysis_test_near(data.times[161], 161.0 * ANALYSIS_TEST_DT));
  assert(data.marker_count == 1u && data.markers[0].frame == 95u &&
         data.markers[0].kind == 2u &&
         strcmp(data.markers[0].label, "stutter here") == 0 &&
         analysis_test_near(data.markers[0].time, 95.0 * ANALYSIS_TEST_DT));

  VkrRecordingAnalysis analysis;
  assert(vkr_recording_analyze(&data, NULL, &memory.allocator, &analysis));
  /* 199 samples: 156 at 16, 40 at 24, then 28, 30 and 31. Nearest ranks
     are 100 (p50), 190 (p95) and 198 (p99). */
  const VkrRecordingStats *frame_time = &analysis.frame_time;
  assert(frame_time->valid == 199u);
  assert(frame_time->min == 16.0 && frame_time->max == 31.0);
  assert(analysis_test_near(frame_time->mean, 3545.0 / 199.0));
  assert(frame_time->p50 == 16.0 && frame_time->p95 == 24.0 &&
         frame_time->p99 == 30.0);
  /* 1% of 3545 ms is 35.45 ms: 31 alone stays under it, 31 + 30 reaches it. */
  assert(analysis.low_1pct_ms == 30.0);
  assert(analysis.budget_ms == VKR_RECORDING_ANALYSIS_BUDGET_MS);
  assert(analysis.over_budget == 43u && analysis.over_double_budget == 0u);
  /* Even frames 0 to 198 without the lost frame 150. */
  assert(analysis.series[draw].valid == 99u);
  assert(analysis.series[ANALYSIS_TEST_UPDATE].p50 == 5.0 &&
         analysis.series[ANALYSIS_TEST_GPU].p50 == 10.0 &&
         analysis.series[ANALYSIS_TEST_GPU].max == 22.0);

  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  analysis_test_remove(path);
  analysis_memory_destroy(&memory);
  printf("  test_recording_analysis_statistics PASSED\n");
}

/* Detects: a plateau reported as spikes, a two-frame spike reported twice or
   without its worst frame, a missed isolated spike, wrong rank order or
   neighbour median, and contributors that include the frame time itself,
   miss the inflated series or list series that did not grow. */
static void test_recording_analysis_spikes(void) {
  printf("  Running test_recording_analysis_spikes...\n");
  AnalysisTestMemory memory;
  analysis_memory_create(&memory);
  char path[1024];
  VkrRecordingData data;
  analysis_test_load("spikes", &ANALYSIS_TEST_MAIN, &memory, path, sizeof(path),
                     &data);
  VkrRecordingAnalysis analysis;
  assert(vkr_recording_analyze(&data, NULL, &memory.allocator, &analysis));

  assert(analysis.spikes_found == 2u && analysis.spike_count == 2u);
  const VkrRecordingSpike *merged = &analysis.spikes[0];
  assert(merged->frame == 161u && merged->first_frame == 160u &&
         merged->last_frame == 161u);
  assert(merged->frame_ms == 31.0 && merged->median_ms == 16.0 &&
         merged->excess_ms == 15.0 && merged->ratio == 31.0 / 16.0);
  assert(analysis_test_near(merged->time, 161.0 * ANALYSIS_TEST_DT));
  assert(merged->contributor_count == 1u &&
         merged->contributors[0].series == ANALYSIS_TEST_GPU &&
         merged->contributors[0].value == 22.0 &&
         merged->contributors[0].excess == 12.0);

  const VkrRecordingSpike *isolated = &analysis.spikes[1];
  assert(isolated->frame == 100u && isolated->first_frame == 100u &&
         isolated->last_frame == 100u);
  assert(isolated->frame_ms == 30.0 && isolated->median_ms == 16.0 &&
         isolated->excess_ms == 14.0);
  /* frame.wall grew by 14 ms too; it is the frame time, not a contributor. */
  assert(isolated->contributor_count == 2u &&
         isolated->contributors[0].series == ANALYSIS_TEST_UPDATE &&
         isolated->contributors[0].excess == 14.0 &&
         isolated->contributors[1].series == ANALYSIS_TEST_RENDER_WAIT &&
         isolated->contributors[1].excess == 2.0);

  /* A lower spike_max keeps the worst spike only. */
  vkr_recording_analysis_free(&analysis);
  const VkrRecordingAnalysisOptions options = {.spike_max = 1u};
  assert(vkr_recording_analyze(&data, &options, &memory.allocator, &analysis));
  assert(analysis.spikes_found == 2u && analysis.spike_count == 1u &&
         analysis.spikes[0].frame == 161u);

  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  analysis_test_remove(path);
  analysis_memory_destroy(&memory);
  printf("  test_recording_analysis_spikes PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

typedef struct AnalysisTestJson {
  uint8_t data[512u * 1024u];
  uint64_t length;
} AnalysisTestJson;

static AnalysisTestJson analysis_test_json;

static bool8_t analysis_test_sink(void *context, const uint8_t *data,
                                  uint64_t length) {
  AnalysisTestJson *buffer = context;
  if (length > sizeof(buffer->data) - buffer->length) {
    return false_v;
  }
  MemCopy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  return true_v;
}

static VkrJsonWriter *analysis_test_json_begin(VkrJsonWriter *writer) {
  analysis_test_json.length = 0u;
  vkr_json_writer_init(writer, analysis_test_sink, &analysis_test_json);
  return writer;
}

static VkrBakeryJson *analysis_test_parse(Arena *arena) {
  VkrBakeryJsonError error;
  VkrBakeryJson *root = vkr_bakery_json_parse(
      arena, analysis_test_json.data, analysis_test_json.length, 16u, &error);
  assert(root != NULL && root->type == VKR_BAKERY_JSON_OBJECT);
  return root;
}

static VkrBakeryJson *analysis_test_member(const VkrBakeryJson *object,
                                           const char *key) {
  VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  assert(value != NULL);
  return value;
}

static float64_t analysis_test_number(const VkrBakeryJson *object,
                                      const char *key) {
  float64_t value = 0.0;
  assert(vkr_bakery_json_get_number(object, key, &value));
  return value;
}

static bool8_t analysis_test_flag(const VkrBakeryJson *object,
                                  const char *key) {
  bool8_t value = false_v;
  assert(vkr_bakery_json_get_bool(object, key, &value));
  return value;
}

/* Detects: analysis JSON that does not parse, exceeds the summary.json
   bound, drops a statistic, a spike's camera, marker, contributor or bytes
   value; and frame rows that shift, print a lost frame as a value, or hide
   a row cut. */
static void test_recording_analysis_json(void) {
  printf("  Running test_recording_analysis_json...\n");
  AnalysisTestMemory memory;
  analysis_memory_create(&memory);
  Arena *arena = arena_create(MB(16), MB(1));
  assert(arena != NULL);
  char path[1024];
  VkrRecordingData data;
  analysis_test_load("json", &ANALYSIS_TEST_MAIN, &memory, path, sizeof(path),
                     &data);
  VkrRecordingAnalysis analysis;
  assert(vkr_recording_analyze(&data, NULL, &memory.allocator, &analysis));

  VkrJsonWriter writer;
  assert(vkr_recording_analysis_write_json(analysis_test_json_begin(&writer),
                                           &data, &analysis, NULL));
  assert(vkr_json_writer_complete(&writer));
  assert(analysis_test_json.length <= VKR_RECORDING_ANALYSIS_JSON_BUDGET);
  const VkrBakeryJson *root = analysis_test_parse(arena);
  assert(analysis_test_number(root, "frames") == 200.0 &&
         analysis_test_number(root, "missing_frames") == 1.0);
  const VkrBakeryJson *frame_time = analysis_test_member(root, "frame_time");
  assert(vkr_bakery_json_is_string(analysis_test_member(frame_time, "source"),
                                   "frame.wall"));
  assert(analysis_test_number(frame_time, "p99") == 30.0 &&
         analysis_test_number(frame_time, "low_1pct") == 30.0 &&
         analysis_test_number(frame_time, "over_budget") == 43.0);
  const VkrBakeryJson *series = analysis_test_member(root, "series");
  assert(series->type == VKR_BAKERY_JSON_OBJECT &&
         series->count == ANALYSIS_TEST_METRIC_COUNT &&
         !analysis_test_flag(root, "series_truncated"));
  /* [unit, valid, min, max, mean, p50, p95, p99] */
  const VkrBakeryJson *gpu = analysis_test_member(series, "gpu.submission");
  assert(gpu->count == 8u &&
         vkr_bakery_json_is_string(vkr_bakery_json_at(gpu, 0u), "ms") &&
         vkr_bakery_json_at(gpu, 3u)->number == 22.0);
  const VkrBakeryJson *bytes_series =
      analysis_test_member(root, "bytes_series");
  assert(bytes_series->count == 1u &&
         vkr_bakery_json_is_string(vkr_bakery_json_at(bytes_series, 0u),
                                   "memory.cpu.bytes.live"));

  const VkrBakeryJson *spikes = analysis_test_member(root, "spikes");
  assert(spikes->count == 2u && !analysis_test_flag(root, "spikes_truncated"));
  const VkrBakeryJson *first = vkr_bakery_json_at(spikes, 0u);
  assert(analysis_test_number(first, "frame") == 161.0 &&
         analysis_test_number(first, "frames") == 2.0 &&
         analysis_test_number(first, "ms") == 31.0 &&
         analysis_test_number(first, "median") == 16.0 &&
         analysis_test_number(first, "ratio") == 1.9375 &&
         analysis_test_number(first, "yaw") == 161.0);
  const VkrBakeryJson *position = analysis_test_member(first, "position");
  assert(position->count == 3u &&
         vkr_bakery_json_at(position, 0u)->number == 80.5 &&
         vkr_bakery_json_at(position, 2u)->number == -161.0);
  const VkrBakeryJson *contributors =
      analysis_test_member(first, "contributors");
  assert(contributors->count == 1u &&
         vkr_bakery_json_is_string(
             analysis_test_member(vkr_bakery_json_at(contributors, 0u), "name"),
             "gpu.submission"));
  const VkrBakeryJson *bytes = analysis_test_member(first, "bytes");
  assert(bytes->count == 1u &&
         vkr_bakery_json_at(bytes, 0u)->number == 1.0e9 + 161.0 * 4096.0);
  assert(analysis_test_member(first, "markers")->count == 0u);
  const VkrBakeryJson *markers =
      analysis_test_member(vkr_bakery_json_at(spikes, 1u), "markers");
  assert(markers->count == 1u);
  const VkrBakeryJson *marker = vkr_bakery_json_at(markers, 0u);
  assert(
      analysis_test_number(marker, "frame") == 95.0 &&
      vkr_bakery_json_is_string(analysis_test_member(marker, "kind"), "bug") &&
      vkr_bakery_json_is_string(analysis_test_member(marker, "label"),
                                "stutter here"));

  /* Rows 148 to 152; the lost frame 150 prints null samples. */
  const char *fields[] = {VKR_RECORDING_FRAME_MS_FIELD, "cpu.update", "nope"};
  assert(vkr_recording_frames_write_json(analysis_test_json_begin(&writer),
                                         &data, 148u, 152u, fields,
                                         ArrayCount(fields), 0u));
  assert(vkr_json_writer_complete(&writer));
  root = analysis_test_parse(arena);
  assert(analysis_test_member(root, "columns")->count == 8u);
  const VkrBakeryJson *rows = analysis_test_member(root, "rows");
  assert(rows->count == 5u && analysis_test_number(root, "rows_total") == 5.0 &&
         !analysis_test_flag(root, "truncated"));
  const VkrBakeryJson *row = vkr_bakery_json_at(rows, 0u);
  assert(row->count == 8u && vkr_bakery_json_at(row, 0u)->number == 148.0 &&
         vkr_bakery_json_at(row, 2u)->number == 74.0 &&
         vkr_bakery_json_at(row, 5u)->number == 16.0 &&
         vkr_bakery_json_at(row, 6u)->number == 5.0 &&
         vkr_bakery_json_at(row, 7u)->type == VKR_BAKERY_JSON_NULL);
  const VkrBakeryJson *lost = vkr_bakery_json_at(rows, 2u);
  assert(vkr_bakery_json_at(lost, 0u)->number == 150.0 &&
         vkr_bakery_json_at(lost, 2u)->type == VKR_BAKERY_JSON_NULL &&
         vkr_bakery_json_at(lost, 5u)->type == VKR_BAKERY_JSON_NULL &&
         vkr_bakery_json_at(lost, 6u)->type == VKR_BAKERY_JSON_NULL);
  assert(vkr_bakery_json_at(vkr_bakery_json_at(rows, 3u), 0u)->number == 151.0);

  /* A row cap below the range states the cut. */
  assert(vkr_recording_frames_write_json(analysis_test_json_begin(&writer),
                                         &data, 0u, 100000u, fields, 1u, 50u));
  assert(vkr_json_writer_complete(&writer));
  root = analysis_test_parse(arena);
  assert(analysis_test_member(root, "rows")->count == 50u &&
         analysis_test_number(root, "rows_total") == 200.0 &&
         analysis_test_flag(root, "truncated"));

  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  analysis_test_remove(path);
  arena_destroy(arena);
  analysis_memory_destroy(&memory);
  printf("  test_recording_analysis_json PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* Bound scenario: 2000 frames with a spike every 40 frames, two long        */
/* markers at each, so 20 spikes do not fit the summary bound.               */

static float64_t analysis_test_bound_wall(uint64_t frame) {
  return frame % 40u == 20u ? 30.0 + (float64_t)((frame / 40u) % 7u) : 16.0;
}

static void analysis_test_bound_metrics(uint64_t frame,
                                        AnalysisTestMetrics *out) {
  analysis_test_main_metrics(frame, out);
  out->values[ANALYSIS_TEST_WALL] = analysis_test_bound_wall(frame);
  out->values[ANALYSIS_TEST_UPDATE] = analysis_test_bound_wall(frame) - 11.0;
  out->values[ANALYSIS_TEST_RENDER_WAIT] = 2.0;
  out->values[ANALYSIS_TEST_GPU] = 10.0;
}

static char analysis_test_long_label[ANALYSIS_TEST_LABEL_BYTES + 1u];

static uint32_t analysis_test_bound_markers(uint64_t frame, uint32_t *out_kinds,
                                            const char **out_labels) {
  if (frame % 40u != 20u) {
    return 0u;
  }
  for (uint32_t i = 0u; i < 2u; ++i) {
    out_kinds[i] = 1u;
    out_labels[i] = analysis_test_long_label;
  }
  return 2u;
}

/* Detects: a summary analysis that grows past its byte bound when spikes
   carry long markers, a cut that is not stated or that drops the worst
   spikes first, an unstable rank order among equal spikes, and a frames
   query that returns more than 500 rows. */
static void test_recording_analysis_json_bound(void) {
  printf("  Running test_recording_analysis_json_bound...\n");
  MemSet(analysis_test_long_label, 'm', ANALYSIS_TEST_LABEL_BYTES);
  analysis_test_long_label[ANALYSIS_TEST_LABEL_BYTES] = '\0';
  const AnalysisTestScenario scenario = {
      .frames = 2000u,
      .missing_frame = ANALYSIS_TEST_NO_FRAME,
      .metrics = analysis_test_bound_metrics,
      .markers = analysis_test_bound_markers,
  };
  AnalysisTestMemory memory;
  analysis_memory_create(&memory);
  Arena *arena = arena_create(MB(32), MB(1));
  assert(arena != NULL);
  char path[1024];
  VkrRecordingData data;
  analysis_test_load("bound", &scenario, &memory, path, sizeof(path), &data);
  assert(data.marker_count == 100u && data.missing_frames == 0u);
  VkrRecordingAnalysis analysis;
  assert(vkr_recording_analyze(&data, NULL, &memory.allocator, &analysis));
  assert(analysis.spikes_found == 50u &&
         analysis.spike_count == VKR_RECORDING_ANALYSIS_SPIKE_MAX);
  /* Seven spikes reach 36 ms; equal excesses rank by frame. */
  assert(analysis.spikes[0].frame == 260u &&
         analysis.spikes[6].frame == 1940u && analysis.spikes[7].frame == 220u);

  VkrJsonWriter writer;
  const VkrRecordingAnalysisJsonOptions unbounded = {.include_series = true_v};
  assert(vkr_recording_analysis_write_json(analysis_test_json_begin(&writer),
                                           &data, &analysis, &unbounded));
  assert(vkr_json_writer_complete(&writer));
  assert(analysis_test_json.length > VKR_RECORDING_ANALYSIS_JSON_BUDGET);
  const VkrBakeryJson *root = analysis_test_parse(arena);
  assert(analysis_test_member(root, "spikes")->count ==
         VKR_RECORDING_ANALYSIS_SPIKE_MAX);

  assert(vkr_recording_analysis_write_json(analysis_test_json_begin(&writer),
                                           &data, &analysis, NULL));
  assert(vkr_json_writer_complete(&writer));
  printf("    bounded analysis %llu bytes\n",
         (unsigned long long)analysis_test_json.length);
  assert(analysis_test_json.length <= VKR_RECORDING_ANALYSIS_JSON_BUDGET);
  root = analysis_test_parse(arena);
  const VkrBakeryJson *spikes = analysis_test_member(root, "spikes");
  assert(spikes->count > 0u &&
         spikes->count < VKR_RECORDING_ANALYSIS_SPIKE_MAX);
  assert(analysis_test_flag(root, "spikes_truncated") &&
         analysis_test_number(root, "spikes_found") == 50.0);
  assert(analysis_test_number(vkr_bakery_json_at(spikes, 0u), "frame") ==
         260.0);
  const VkrBakeryJson *markers =
      analysis_test_member(vkr_bakery_json_at(spikes, 0u), "markers");
  assert(markers->count == 2u);

  const char *fields[] = {VKR_RECORDING_FRAME_MS_FIELD};
  assert(vkr_recording_frames_write_json(analysis_test_json_begin(&writer),
                                         &data, 0u, 1999u, fields,
                                         ArrayCount(fields), 0u));
  assert(vkr_json_writer_complete(&writer));
  root = analysis_test_parse(arena);
  assert(analysis_test_member(root, "rows")->count ==
             VKR_RECORDING_FRAMES_ROW_MAX &&
         analysis_test_number(root, "rows_total") == 2000.0 &&
         analysis_test_flag(root, "truncated"));

  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  analysis_test_remove(path);
  arena_destroy(arena);
  analysis_memory_destroy(&memory);
  printf("  test_recording_analysis_json_bound PASSED\n");
}

bool32_t run_recording_analysis_tests(void) {
  printf("--- Starting Recording Analysis Tests ---\n");
  test_recording_analysis_statistics();
  test_recording_analysis_spikes();
  test_recording_analysis_json();
  test_recording_analysis_json_bound();
  printf("--- Recording Analysis Tests Completed ---\n");
  return true_v;
}
