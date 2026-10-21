#include "assets/vkr_recording_analysis.h"

#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_json.h"
#include "core/vkr_zone.h"
#include "math/vkr_math.h"
#include "vkr_candidate_residency.h"

#define VKR_RECORDING_DATA_PATH_MAX 4096u
/* A spike candidate exceeds its window median by this factor and by at
   least this many milliseconds. */
#define VKR_RECORDING_SPIKE_RATIO 1.2
#define VKR_RECORDING_SPIKE_MIN_EXCESS_MS 4.0
/* JSON numbers keep this many significant digits: a microsecond for
   frame times, a millimetre for positions within 100 m. */
#define VKR_RECORDING_JSON_DIGITS 6u
/* "false" is one byte longer than "true"; a measured header may hold three
   truncation flags that turn false. */
#define VKR_RECORDING_JSON_FLAG_SLACK 3u
/* The series whose median paces the frames, the budget without an option. */
#define VKR_RECORDING_PRESENT_INTERVAL_SERIES "frame.present_interval"
/* Column of vkr_recording_frames_write_json that reads the frame time, and
   one that names no series. */
#define VKR_RECORDING_COLUMN_FRAME_MS (UINT32_MAX - 1u)
#define VKR_RECORDING_COLUMN_UNKNOWN UINT32_MAX
/* Player columns: x, y, z, then speed. */
#define VKR_RECORDING_COLUMN_PLAYER (UINT32_MAX - 5u)
/* Pass columns: one of these bits with the pass id. */
#define VKR_RECORDING_COLUMN_PASS_GPU 0x40000000u
#define VKR_RECORDING_COLUMN_PASS_CPU 0x20000000u
#define VKR_RECORDING_COLUMN_PASS_MASK 0x0000ffffu

/* ------------------------------------------------------------------------ */
/* Load                                                                      */

typedef struct VkrRecordingDataLoad {
  VkrRecordingData *data;
  VkrRecordingReader reader;
  const VkrCodec *codec;
  VkrRecordingDictionary dictionary;
  bool8_t has_dictionary;

  const VkrRecordingStream *frame;
  const VkrRecordingStream *camera;
  const VkrRecordingStream *marker;
  const VkrRecordingStream *event;
  const VkrRecordingStream *metrics;
  const VkrRecordingStream *pass_name;
  const VkrRecordingStream *passes;
  const VkrRecordingStream *graph;
  const VkrRecordingStream *allocator_name;
  const VkrRecordingStream *allocators;
  const VkrRecordingStream *zone_name;
  const VkrRecordingStream *zone;
  const VkrRecordingStream *draw_bounds;
  const VkrRecordingStream *draw_ids;
  const VkrRecordingStream *draw_states;
  const VkrRecordingStream *hardware;
  const VkrRecordingStream *pipeline;
  const VkrRecordingStream *pass_starts;
  const VkrRecordingStream *gpu_counter_name;
  const VkrRecordingStream *gpu_counters;
  const VkrRecordingStream *player;
  /* All measurements: series after the metrics stream's, named by
     measurement_name records. */
  const VkrRecordingStream *measurement_name;
  const VkrRecordingStream *measurements;
  uint32_t measurement_capacity;
  const VkrRecordingField *player_position;
  const VkrRecordingField *player_velocity;
  const VkrRecordingField *display_dt;
  const VkrRecordingField *position;
  const VkrRecordingField *yaw;
  const VkrRecordingField *pitch;
  const VkrRecordingField *fov;
  const VkrRecordingField *far_clip;
  const VkrRecordingField *valid_mask;
  /* The metrics field of each series, in dictionary order. */
  const VkrRecordingField *metric_fields[VKR_RECORDING_FIELD_MAX];
  uint32_t metric_count;

  /* First pass counts; the second fills up to them. */
  uint64_t frame_count;
  uint32_t marker_capacity;
  uint32_t event_capacity;
  uint32_t pass_capacity;
  uint64_t pass_rows;
  /* Graph records that parse, and the bytes their parsed arrays and names
     take. */
  uint32_t graph_capacity;
  uint64_t graph_bytes;
  /* Parsed bytes the graphs filled so far. */
  uint64_t graph_used;
  /* Allocator ids named, and allocator rows. */
  uint32_t allocator_capacity;
  uint64_t allocator_rows;
  /* Zone ids named and zone records, and the records the fill keeps before
     they are reduced per frame. */
  uint32_t zone_capacity;
  uint64_t zone_records;
  struct VkrRecordingZoneCall *zone_calls;
  uint64_t zone_call_count;
  /* Census records that fit, their objects, ids and state words, and the
     ones filled so far. */
  uint32_t draw_bounds_capacity;
  uint64_t draw_objects;
  uint64_t draw_objects_used;
  uint32_t draw_ids_capacity;
  uint64_t draw_id_values;
  uint64_t draw_id_values_used;
  uint64_t draw_words;
  /* Census frames kept: one of every draw_stride, so a recording whose
     census exceeds the word budget keeps frames across all of it rather
     than its beginning; and the census records seen so far. */
  uint32_t draw_stride;
  uint64_t draw_seen;
  /* Hardware facts and pipeline records that parse, and their
     statistics. */
  uint32_t hardware_capacity;
  uint32_t pipeline_capacity;
  uint32_t pipeline_stat_capacity;
  /* The bundle's imported GPU counters (VKR_RECORDING_GPU_COUNTERS_FILE),
     mapped until the load ends and read in place: the names from
     counter_names, the values from counter_values. */
  FileMapping counter_file;
  const uint8_t *counter_names;
  const uint8_t *counter_values;
  uint32_t counter_count;
  uint32_t counter_buckets;
  uint32_t counter_bucket_ns;
  uint64_t counter_first_ns;
  /* Only with counters: each frame's GPU start and end on the CPU clock
     (zero without), each kept pass row's GPU start in ms after it (NaN
     without), and the kept row of each row of the last passes record
     (UINT32_MAX for a dropped one), whose pass_starts record follows it. */
  void *counter_scratch;
  uint64_t counter_scratch_bytes;
  uint64_t *gpu_spans;
  float32_t *pass_row_starts;
  uint64_t raw_frame;
  uint32_t raw_rows;
  uint32_t raw_kept[VKR_RECORDING_PASS_MAX];
  /* Counters the runtime sampled itself ("gpu_counter_name" and
     "gpu_counters", ADR-130): the names packed as the imported file packs
     them, so the same counter block holds them, and the session records,
     copied by the fill and folded by vkr_recording_data_finish_counters. */
  uint8_t sampled_names[VKR_RECORDING_GPU_COUNTER_MAX]
                       [3u + VKR_RECORDING_GPU_COUNTER_NAME_MAX];
  uint8_t sampled_packed[VKR_RECORDING_GPU_COUNTER_MAX *
                         (2u + VKR_RECORDING_GPU_COUNTER_NAME_MAX)];
  uint32_t sampled_count;
  uint8_t *sessions;
  uint64_t session_bytes;
  uint64_t session_used;
  bool8_t too_large;
} VkrRecordingDataLoad;

vkr_internal float64_t vkr_recording_analysis_load_f64(const uint8_t *src) {
  const uint64_t bits = vkr_load_le_u64(src);
  float64_t value = 0.0;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

/* Element `element` of a scalar or vector field, converted to float64. */
vkr_internal float64_t vkr_recording_field_value(const VkrRecordingField *field,
                                                 const uint8_t *record,
                                                 uint32_t element) {
  const uint8_t *at =
      record + field->offset +
      (uint64_t)element * vkr_recording_field_type_size(field->type);
  switch (field->type) {
  case VKR_RECORDING_FIELD_U8:
    return (float64_t)at[0];
  case VKR_RECORDING_FIELD_U16:
    return (float64_t)((uint32_t)at[0] | ((uint32_t)at[1] << 8u));
  case VKR_RECORDING_FIELD_U32:
    return (float64_t)vkr_load_le_u32(at);
  case VKR_RECORDING_FIELD_U64:
    return (float64_t)vkr_load_le_u64(at);
  case VKR_RECORDING_FIELD_I32:
    return (float64_t)(int32_t)vkr_load_le_u32(at);
  case VKR_RECORDING_FIELD_I64:
    return (float64_t)(int64_t)vkr_load_le_u64(at);
  case VKR_RECORDING_FIELD_F32:
    return (float64_t)vkr_load_le_f32(at);
  case VKR_RECORDING_FIELD_F64:
    return vkr_recording_analysis_load_f64(at);
  }
  return 0.0;
}

vkr_internal const VkrRecordingStream *
vkr_recording_find_stream(const VkrRecordingDictionary *dictionary,
                          const char *name) {
  for (uint32_t i = 0u; i < dictionary->stream_count; ++i) {
    if (string_equals(dictionary->streams[i].name, name)) {
      return &dictionary->streams[i];
    }
  }
  return NULL;
}

/* A field of a fixed-size stream with at least `count` elements, or NULL. */
vkr_internal const VkrRecordingField *
vkr_recording_find_field(const VkrRecordingStream *stream, const char *name,
                         uint16_t count) {
  if (!stream) {
    return NULL;
  }
  for (uint32_t i = 0u; i < stream->field_count; ++i) {
    const VkrRecordingField *field = &stream->fields[i];
    if (string_equals(field->name, name) && field->count >= count) {
      return field;
    }
  }
  return NULL;
}

/* `out_bundle` says `path` is a bundle directory rather than a stream. */
vkr_internal VkrRecordingDataError
vkr_recording_data_open(VkrRecordingReader *reader, VkrAllocator *allocator,
                        const char *path, bool8_t *out_bundle) {
  VkrRecordingReadError error =
      vkr_recording_reader_open(reader, allocator, path);
  *out_bundle = error == VKR_RECORDING_READ_OPEN;
  if (error == VKR_RECORDING_READ_OPEN) {
    /* A bundle directory holds stream.vkrs. */
    char stream[VKR_RECORDING_DATA_PATH_MAX];
    const int32_t length =
        string_format(stream, sizeof(stream), "%s/stream.vkrs", path);
    if (length < 0 || (uint64_t)length >= sizeof(stream)) {
      return VKR_RECORDING_DATA_OPEN;
    }
    error = vkr_recording_reader_open(reader, allocator, stream);
  }
  switch (error) {
  case VKR_RECORDING_READ_OK:
    return VKR_RECORDING_DATA_OK;
  case VKR_RECORDING_READ_OPEN:
    return VKR_RECORDING_DATA_OPEN;
  case VKR_RECORDING_READ_HEADER:
  case VKR_RECORDING_READ_VERSION:
    return VKR_RECORDING_DATA_HEADER;
  case VKR_RECORDING_READ_MEMORY:
    return VKR_RECORDING_DATA_MEMORY;
  }
  return VKR_RECORDING_DATA_HEADER;
}

/* Decodes the dictionary and resolves the streams and fields by name. */
vkr_internal VkrRecordingDataError
vkr_recording_data_dictionary(VkrRecordingDataLoad *load) {
  VkrAllocator *allocator = load->data->allocator;
  const uint32_t index =
      vkr_recording_reader_find(&load->reader, VKR_RECORDING_CHUNK_DICTIONARY);
  uint8_t *payload = NULL;
  uint64_t size = 0u;
  if (index == UINT32_MAX ||
      !vkr_recording_reader_chunk(&load->reader, index, load->codec, allocator,
                                  &payload, &size)) {
    return VKR_RECORDING_DATA_DICTIONARY;
  }
  load->has_dictionary = vkr_recording_dictionary_decode(
      payload, size, allocator, &load->dictionary);
  vkr_recording_reader_chunk_free(allocator, payload, size);
  if (!load->has_dictionary) {
    return VKR_RECORDING_DATA_DICTIONARY;
  }

  const VkrRecordingDictionary *dictionary = &load->dictionary;
  load->frame = vkr_recording_find_stream(dictionary, "frame");
  load->camera = vkr_recording_find_stream(dictionary, "camera");
  load->marker = vkr_recording_find_stream(dictionary, "marker");
  load->event = vkr_recording_find_stream(dictionary, "event");
  load->metrics = vkr_recording_find_stream(dictionary, "metrics");
  load->pass_name = vkr_recording_find_stream(dictionary, "pass_name");
  load->passes = vkr_recording_find_stream(dictionary, "passes");
  load->graph = vkr_recording_find_stream(dictionary, "graph");
  load->allocator_name =
      vkr_recording_find_stream(dictionary, "allocator_name");
  load->allocators = vkr_recording_find_stream(dictionary, "allocators");
  load->zone_name = vkr_recording_find_stream(dictionary, "zone_name");
  load->zone = vkr_recording_find_stream(dictionary, "zone");
  load->draw_bounds = vkr_recording_find_stream(dictionary, "draw_bounds");
  load->draw_ids = vkr_recording_find_stream(dictionary, "draw_ids");
  load->draw_states = vkr_recording_find_stream(dictionary, "draw_states");
  load->hardware = vkr_recording_find_stream(dictionary, "hardware");
  load->pipeline = vkr_recording_find_stream(dictionary, "pipeline");
  load->pass_starts = vkr_recording_find_stream(dictionary, "pass_starts");
  load->gpu_counter_name =
      vkr_recording_find_stream(dictionary, "gpu_counter_name");
  load->gpu_counters = vkr_recording_find_stream(dictionary, "gpu_counters");
  load->player = vkr_recording_find_stream(dictionary, "player");
  load->measurement_name =
      vkr_recording_find_stream(dictionary, "measurement_name");
  load->measurements = vkr_recording_find_stream(dictionary, "measurements");
  load->player_position =
      vkr_recording_find_field(load->player, "position", 3u);
  load->player_velocity =
      vkr_recording_find_field(load->player, "velocity", 3u);
  load->display_dt = vkr_recording_find_field(load->frame, "display_dt", 1u);
  load->position = vkr_recording_find_field(load->camera, "position", 3u);
  load->yaw = vkr_recording_find_field(load->camera, "yaw", 1u);
  load->pitch = vkr_recording_find_field(load->camera, "pitch", 1u);
  load->fov = vkr_recording_find_field(load->camera, "fov", 1u);
  load->far_clip = vkr_recording_find_field(load->camera, "far_clip", 1u);
  load->valid_mask = vkr_recording_find_field(load->metrics, "valid", 1u);

  for (uint32_t i = 0u; load->metrics && i < load->metrics->field_count; ++i) {
    const VkrRecordingField *field = &load->metrics->fields[i];
    if (field == load->valid_mask || field->count != 1u) {
      continue;
    }
    load->metric_fields[load->metric_count++] = field;
  }
  return VKR_RECORDING_DATA_OK;
}

vkr_internal bool8_t vkr_recording_is_stream(const VkrRecordingStream *stream,
                                             const VkrRecordingRecord *record) {
  return stream && record->stream == stream->id;
}

/* A fixed-size record whose size matches its dictionary entry. */
vkr_internal bool8_t vkr_recording_record_fits(
    const VkrRecordingStream *stream, const VkrRecordingRecord *record) {
  return vkr_recording_is_stream(stream, record) && stream->record_size > 0u &&
         record->size == stream->record_size;
}

/* The row count of a passes record whose size matches it, else zero. */
vkr_internal uint32_t
vkr_recording_pass_record_rows(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_PASSES_HEADER_SIZE) {
    return 0u;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  const uint64_t size = VKR_RECORDING_PASSES_HEADER_SIZE +
                        (uint64_t)VKR_RECORDING_PASS_ROW_SIZE * rows;
  return rows <= VKR_RECORDING_PASS_MAX && size == record->size ? rows : 0u;
}

/* The pass id of a pass_name record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_pass_name_id(const VkrRecordingRecord *record) {
  if (record->size < 4u ||
      record->size - 4u > (uint32_t)VKR_RECORDING_PASS_NAME_MAX) {
    return UINT32_MAX;
  }
  const uint32_t id = vkr_load_le_u32(record->data);
  return id < VKR_RECORDING_PASS_MAX ? id : UINT32_MAX;
}

/* One finished zone as the fill keeps it until the reduction. */
typedef struct VkrRecordingZoneCall {
  uint32_t frame;
  uint16_t zone;
  uint8_t thread;
  uint8_t depth;
  float32_t ms;
} VkrRecordingZoneCall;

/* The object count of a draw_bounds record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_draw_bounds_rows(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_DRAW_BOUNDS_HEADER_SIZE) {
    return UINT32_MAX;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  const uint64_t size = VKR_RECORDING_DRAW_BOUNDS_HEADER_SIZE +
                        (uint64_t)VKR_RECORDING_DRAW_BOUNDS_ROW_SIZE * rows;
  return rows <= VKR_RECORDING_DRAW_CANDIDATE_MAX && size == record->size
             ? rows
             : UINT32_MAX;
}

/* The candidate count of a draw_ids record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_draw_ids_count(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_DRAW_IDS_HEADER_SIZE) {
    return UINT32_MAX;
  }
  const uint32_t count = vkr_load_le_u32(record->data);
  const uint64_t size = VKR_RECORDING_DRAW_IDS_HEADER_SIZE + 4ull * count;
  return count <= VKR_RECORDING_DRAW_CANDIDATE_MAX && size == record->size
             ? count
             : UINT32_MAX;
}

/* The state words of a draw_states record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_draw_state_words(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_DRAW_STATES_HEADER_SIZE) {
    return UINT32_MAX;
  }
  const uint32_t count = vkr_load_le_u32(record->data);
  const uint32_t words = (count + VKR_RECORDING_DRAW_STATES_PER_WORD - 1u) /
                         VKR_RECORDING_DRAW_STATES_PER_WORD;
  const uint64_t size = VKR_RECORDING_DRAW_STATES_HEADER_SIZE + 4ull * words;
  return count <= VKR_RECORDING_DRAW_CANDIDATE_MAX && size == record->size
             ? words
             : UINT32_MAX;
}

/* The id of a zone_name record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_zone_name_id(const VkrRecordingRecord *record) {
  if (record->size < 4u || record->size - 4u > VKR_RECORDING_NAME_MAX) {
    return UINT32_MAX;
  }
  const uint32_t id = vkr_load_le_u32(record->data);
  return id < VKR_RECORDING_DATA_ZONE_MAX ? id : UINT32_MAX;
}

/* The row count of an allocators record whose size matches it, else zero. */
vkr_internal uint32_t
vkr_recording_allocator_record_rows(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_ALLOCATORS_HEADER_SIZE) {
    return 0u;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  const uint64_t size = VKR_RECORDING_ALLOCATORS_HEADER_SIZE +
                        (uint64_t)VKR_RECORDING_ALLOCATOR_ROW_SIZE * rows;
  return rows <= VKR_RECORDING_ALLOCATOR_MAX && size == record->size ? rows
                                                                     : 0u;
}

/* The id of an allocator_name record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_allocator_name_id(const VkrRecordingRecord *record) {
  if (record->size < 8u ||
      record->size - 8u != vkr_load_le_u16(record->data + 6u) ||
      record->size - 8u > VKR_RECORDING_ALLOCATOR_NAME_MAX) {
    return UINT32_MAX;
  }
  const uint32_t id = vkr_load_le_u32(record->data);
  return id < VKR_RECORDING_ALLOCATOR_MAX ? id : UINT32_MAX;
}

/* A graph record's counts, read from its header. */
typedef struct VkrRecordingGraphCounts {
  uint32_t passes;
  uint32_t uses;
  uint32_t resources;
  uint32_t name_bytes;
} VkrRecordingGraphCounts;

/* Every parsed part of a graph starts 8-byte aligned in the graph block. */
vkr_internal uint64_t
vkr_recording_graph_parsed_bytes(const VkrRecordingGraphCounts *counts) {
  return vkr_align_up_u64(sizeof(VkrRecordingGraphPass) * counts->passes, 8u) +
         vkr_align_up_u64(sizeof(VkrRecordingGraphUse) * counts->uses, 8u) +
         vkr_align_up_u64(sizeof(VkrRecordingGraphResource) * counts->resources,
                          8u) +
         vkr_align_up_u64(counts->name_bytes, 8u);
}

/* The counts of a graph record of a known version whose size matches them;
   false for any other. */
vkr_internal bool8_t vkr_recording_graph_counts(
    const VkrRecordingRecord *record, VkrRecordingGraphCounts *out) {
  if (record->size < VKR_RECORDING_GRAPH_HEADER_SIZE ||
      vkr_load_le_u32(record->data) != VKR_RECORDING_GRAPH_VERSION) {
    return false_v;
  }
  *out = (VkrRecordingGraphCounts){
      .passes = vkr_load_le_u32(record->data + 4u),
      .uses = vkr_load_le_u32(record->data + 8u),
      .resources = vkr_load_le_u32(record->data + 12u),
      .name_bytes = vkr_load_le_u32(record->data + 16u),
  };
  const uint64_t size =
      VKR_RECORDING_GRAPH_HEADER_SIZE +
      (uint64_t)VKR_RECORDING_GRAPH_PASS_SIZE * out->passes +
      (uint64_t)VKR_RECORDING_GRAPH_USE_SIZE * out->uses +
      (uint64_t)VKR_RECORDING_GRAPH_RESOURCE_SIZE * out->resources +
      out->name_bytes;
  return size == record->size;
}

/* The pipeline and OS the IDENTITY chunk names. */
/* A hardware record's key and value, or false when it does not parse. */
vkr_internal bool8_t vkr_recording_hardware_parse(
    const VkrRecordingRecord *record, VkrRecordingHardwareFact *out) {
  if (record->size < 4u) {
    return false_v;
  }
  const uint32_t key_length = vkr_load_le_u16(record->data);
  const uint32_t value_length = vkr_load_le_u16(record->data + 2u);
  if (key_length == 0u || key_length > VKR_RECORDING_HARDWARE_KEY_MAX ||
      value_length > VKR_RECORDING_HARDWARE_VALUE_MAX ||
      record->size != 4u + key_length + value_length) {
    return false_v;
  }
  if (out) {
    MemZero(out, sizeof(*out));
    MemCopy(out->key, record->data + 4u, key_length);
    MemCopy(out->value, record->data + 4u + key_length, value_length);
  }
  return true_v;
}

/* A pipeline record's fields and statistic count, its statistics written
   to `stats` when given; false when it does not parse. */
vkr_internal bool8_t vkr_recording_pipeline_parse(
    const VkrRecordingRecord *record, VkrRecordingPipeline *out,
    VkrRecordingPipelineStat *stats) {
  if (record->size < VKR_RECORDING_PIPELINE_HEADER_SIZE) {
    return false_v;
  }
  const uint8_t *data = record->data;
  const uint32_t name_length = vkr_load_le_u16(data + 6u);
  const uint32_t stat_count = vkr_load_le_u32(data + 36u);
  if (name_length > VKR_RECORDING_PIPELINE_NAME_MAX ||
      stat_count > VKR_RECORDING_PIPELINE_STATS_PER_RECORD ||
      record->size < VKR_RECORDING_PIPELINE_HEADER_SIZE + name_length) {
    return false_v;
  }
  uint64_t cursor = VKR_RECORDING_PIPELINE_HEADER_SIZE + name_length;
  for (uint32_t i = 0u; i < stat_count; ++i) {
    if (cursor + 2u > record->size) {
      return false_v;
    }
    const uint32_t length = vkr_load_le_u16(data + cursor);
    if (length > VKR_RECORDING_PIPELINE_STAT_NAME_MAX ||
        cursor + 2u + length + 8u > record->size) {
      return false_v;
    }
    if (stats) {
      MemZero(stats[i].name, sizeof(stats[i].name));
      MemCopy(stats[i].name, data + cursor + 2u, length);
      MemCopy(&stats[i].value, data + cursor + 2u + length,
              sizeof(stats[i].value));
    }
    cursor += 2u + length + 8u;
  }
  if (cursor != record->size) {
    return false_v;
  }
  MemZero(out, sizeof(*out));
  out->frame = record->frame;
  out->kind = data[4];
  MemCopy(&out->compile_ms, data + 8u, sizeof(out->compile_ms));
  out->max_threads = vkr_load_le_u32(data + 12u);
  out->execution_width = vkr_load_le_u32(data + 16u);
  out->threadgroup_memory = vkr_load_le_u32(data + 20u);
  out->registers = vkr_load_le_u32(data + 24u);
  out->spill_bytes = vkr_load_le_u32(data + 28u);
  out->instructions = vkr_load_le_u32(data + 32u);
  out->stat_count = stat_count;
  MemCopy(out->name, data + VKR_RECORDING_PIPELINE_HEADER_SIZE, name_length);
  return true_v;
}

vkr_internal void vkr_recording_data_identity(VkrRecordingDataLoad *load) {
  VkrAllocator *allocator = load->data->allocator;
  for (uint32_t i = 0u; i < load->reader.chunk_count; ++i) {
    if (load->reader.chunks[i].type != VKR_RECORDING_CHUNK_IDENTITY) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(&load->reader, i, load->codec, allocator,
                                    &payload, &size)) {
      return;
    }
    VkrJsonReader reader = vkr_json_reader_create(payload, size);
    String8 value = {0};
    if (vkr_json_get_string(&reader, "pipeline", &value)) {
      string_format(load->data->pipeline, sizeof(load->data->pipeline), "%.*s",
                    (int32_t)value.length, (const char *)value.str);
    }
    vkr_json_reader_reset(&reader);
    if (vkr_json_get_string(&reader, "os", &value)) {
      string_format(load->data->os, sizeof(load->data->os), "%.*s",
                    (int32_t)value.length, (const char *)value.str);
    }
    float64_t number = 0.0;
    vkr_json_reader_reset(&reader);
    if (vkr_json_get_double(&reader, "output_width", &number)) {
      load->data->output_width = (uint32_t)Max(number, 0.0);
    }
    vkr_json_reader_reset(&reader);
    if (vkr_json_get_double(&reader, "output_height", &number)) {
      load->data->output_height = (uint32_t)Max(number, 0.0);
    }
    vkr_recording_reader_chunk_free(allocator, payload, size);
    return;
  }
}

typedef void (*VkrRecordingDataVisit)(VkrRecordingDataLoad *load,
                                      const VkrRecordingRecord *record);

/* Visits every record of every readable RECORDS chunk.
   @return The chunks that failed their CRC or decode and were skipped. */
vkr_internal uint32_t vkr_recording_data_records(VkrRecordingDataLoad *load,
                                                 VkrRecordingDataVisit visit) {
  VkrAllocator *allocator = load->data->allocator;
  uint32_t bad_chunks = 0u;
  for (uint32_t i = 0u; i < load->reader.chunk_count; ++i) {
    if (load->reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(&load->reader, i, load->codec, allocator,
                                    &payload, &size)) {
      ++bad_chunks;
      continue;
    }
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      visit(load, &record);
    }
    if (cursor != size) {
      ++bad_chunks;
    }
    vkr_recording_reader_chunk_free(allocator, payload, size);
  }
  return bad_chunks;
}

vkr_internal void vkr_recording_data_count(VkrRecordingDataLoad *load,
                                           const VkrRecordingRecord *record) {
  if (vkr_recording_is_stream(load->marker, record)) {
    if (record->size >= 4u) {
      ++load->marker_capacity;
    }
    return;
  }
  if (vkr_recording_is_stream(load->event, record)) {
    if (record->size >= 2u &&
        load->event_capacity < VKR_RECORDING_DATA_EVENT_MAX) {
      ++load->event_capacity;
    }
    return;
  }
  if (vkr_recording_is_stream(load->pass_name, record)) {
    const uint32_t id = vkr_recording_pass_name_id(record);
    if (id != UINT32_MAX) {
      load->pass_capacity = Max(load->pass_capacity, id + 1u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->passes, record)) {
    load->pass_rows += vkr_recording_pass_record_rows(record);
    return;
  }
  if (vkr_recording_is_stream(load->allocator_name, record)) {
    const uint32_t id = vkr_recording_allocator_name_id(record);
    if (id != UINT32_MAX) {
      load->allocator_capacity = Max(load->allocator_capacity, id + 1u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->zone_name, record)) {
    const uint32_t id = vkr_recording_zone_name_id(record);
    if (id != UINT32_MAX) {
      load->zone_capacity = Max(load->zone_capacity, id + 1u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->zone, record)) {
    load->zone_records += record->size == sizeof(VkrZoneRecord) ? 1u : 0u;
    return;
  }
  if (vkr_recording_is_stream(load->draw_bounds, record)) {
    const uint32_t rows = vkr_recording_draw_bounds_rows(record);
    if (rows != UINT32_MAX) {
      ++load->draw_bounds_capacity;
      load->draw_objects += rows;
    }
    return;
  }
  if (vkr_recording_is_stream(load->draw_ids, record)) {
    const uint32_t count = vkr_recording_draw_ids_count(record);
    if (count != UINT32_MAX) {
      ++load->draw_ids_capacity;
      load->draw_id_values += count;
    }
    return;
  }
  if (vkr_recording_is_stream(load->draw_states, record)) {
    const uint32_t words = vkr_recording_draw_state_words(record);
    load->draw_words += words != UINT32_MAX ? words : 0u;
    return;
  }
  if (vkr_recording_is_stream(load->allocators, record)) {
    load->allocator_rows += vkr_recording_allocator_record_rows(record);
    return;
  }
  if (vkr_recording_is_stream(load->hardware, record)) {
    if (vkr_recording_hardware_parse(record, NULL) &&
        load->hardware_capacity < VKR_RECORDING_PIPELINE_MAX) {
      ++load->hardware_capacity;
    }
    return;
  }
  if (vkr_recording_is_stream(load->pipeline, record)) {
    VkrRecordingPipeline pipeline;
    if (vkr_recording_pipeline_parse(record, &pipeline, NULL) &&
        load->pipeline_capacity < VKR_RECORDING_PIPELINE_MAX) {
      ++load->pipeline_capacity;
      load->pipeline_stat_capacity += pipeline.stat_count;
    }
    return;
  }
  if (vkr_recording_is_stream(load->graph, record)) {
    VkrRecordingGraphCounts counts;
    if (vkr_recording_graph_counts(record, &counts)) {
      ++load->graph_capacity;
      load->graph_bytes += vkr_recording_graph_parsed_bytes(&counts);
    }
    return;
  }
  if (vkr_recording_is_stream(load->gpu_counter_name, record)) {
    /* u8 id, u8 unit, u8 length, name: kept whole for the packed names. */
    const uint32_t id = record->size >= 3u ? record->data[0] : UINT32_MAX;
    if (id < VKR_RECORDING_GPU_COUNTER_MAX &&
        record->data[2] <= VKR_RECORDING_GPU_COUNTER_NAME_MAX &&
        3u + (uint32_t)record->data[2] <= record->size) {
      MemCopy(load->sampled_names[id], record->data, 3u + record->data[2]);
      load->sampled_count = Max(load->sampled_count, id + 1u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->gpu_counters, record)) {
    if (record->size >= VKR_RECORDING_GPU_COUNTERS_SESSION_HEADER_SIZE) {
      load->session_bytes += 8u + vkr_align_up_u64(record->size, 8u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->measurement_name, record)) {
    if (record->size >= 8u) {
      const uint32_t index = vkr_load_le_u32(record->data);
      if (index < VKR_RECORDING_MEASUREMENT_MAX) {
        load->measurement_capacity =
            Max(load->measurement_capacity, index + 1u);
      }
    }
    return;
  }
  const bool8_t framed = vkr_recording_is_stream(load->frame, record) ||
                         vkr_recording_is_stream(load->camera, record) ||
                         vkr_recording_is_stream(load->metrics, record);
  if (!framed) {
    return;
  }
  if (record->frame >= VKR_RECORDING_DATA_FRAME_MAX) {
    load->too_large = true_v;
    return;
  }
  load->frame_count = Max(load->frame_count, record->frame + 1u);
}

vkr_internal void
vkr_recording_data_fill_metrics(VkrRecordingDataLoad *load,
                                const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  uint64_t mask = UINT64_MAX;
  if (load->valid_mask) {
    const uint8_t *at = record->data + load->valid_mask->offset;
    const uint32_t bytes =
        vkr_recording_field_type_size(load->valid_mask->type);
    mask = bytes == 8u   ? vkr_load_le_u64(at)
           : bytes == 4u ? (uint64_t)vkr_load_le_u32(at)
                         : UINT64_MAX;
  }
  for (uint32_t i = 0u; i < load->metric_count; ++i) {
    if (((mask >> i) & 1u) == 0u) {
      continue;
    }
    const float64_t value =
        vkr_recording_field_value(load->metric_fields[i], record->data, 0u);
    if (!vkr_is_finite_f64(value)) {
      continue;
    }
    data->series[i].values[record->frame] = value;
    data->series[i].valid[record->frame] = 1u;
  }
}

/* A measurement_name record: series metric_count + index takes the name and
   unit. */
vkr_internal void
vkr_recording_data_fill_measurement_name(VkrRecordingDataLoad *load,
                                         const VkrRecordingRecord *record) {
  if (record->size < 8u) {
    return;
  }
  const uint32_t index = vkr_load_le_u32(record->data);
  const uint32_t name_length = vkr_load_le_u16(record->data + 4u);
  const uint32_t unit_length = vkr_load_le_u16(record->data + 6u);
  if (index >= load->measurement_capacity ||
      8u + name_length + unit_length > record->size) {
    return;
  }
  VkrRecordingSeries *series = &load->data->series[load->metric_count + index];
  const uint32_t name = Min(name_length, (uint32_t)VKR_RECORDING_NAME_MAX);
  const uint32_t unit = Min(unit_length, (uint32_t)VKR_RECORDING_NAME_MAX);
  MemCopy(series->name, record->data + 8u, name);
  series->name[name] = '\0';
  MemCopy(series->unit, record->data + 8u + name_length, unit);
  series->unit[unit] = '\0';
}

/* A measurements record: up to a page of values under its valid mask. */
vkr_internal void
vkr_recording_data_fill_measurements(VkrRecordingDataLoad *load,
                                     const VkrRecordingRecord *record) {
  if (record->size < 16u || record->frame >= load->data->frame_count) {
    return;
  }
  const uint32_t first = vkr_load_le_u32(record->data);
  const uint32_t count = vkr_load_le_u32(record->data + 4u);
  const uint64_t valid = vkr_load_le_u64(record->data + 8u);
  if (count > VKR_RECORDING_MEASUREMENT_PAGE ||
      16u + 8u * count != record->size) {
    return;
  }
  for (uint32_t i = 0u; i < count && first + i < load->measurement_capacity;
       ++i) {
    if (((valid >> i) & 1u) == 0u) {
      continue;
    }
    const float64_t value =
        vkr_recording_analysis_load_f64(record->data + 16u + 8u * i);
    if (!vkr_is_finite_f64(value)) {
      continue;
    }
    VkrRecordingSeries *series =
        &load->data->series[load->metric_count + first + i];
    series->values[record->frame] = value;
    series->valid[record->frame] = 1u;
  }
}

vkr_internal void
vkr_recording_data_fill_passes(VkrRecordingDataLoad *load,
                               const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  const uint64_t frame = record->frame;
  const uint32_t rows = vkr_recording_pass_record_rows(record);
  load->raw_frame = UINT64_MAX;
  if (!data->pass_block || rows == 0u || frame >= data->frame_count ||
      data->pass_row_counts[frame] > 0u) {
    return;
  }
  if (data->pass_row_count + rows > load->pass_rows) {
    data->pass_rows_cut += rows;
    return;
  }
  const uint64_t first = data->pass_row_count;
  const uint8_t *at = record->data + VKR_RECORDING_PASSES_HEADER_SIZE;
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < rows; ++i, at += VKR_RECORDING_PASS_ROW_SIZE) {
    const uint16_t pass = (uint16_t)(at[0] | (at[1] << 8u));
    if (i < VKR_RECORDING_PASS_MAX) {
      load->raw_kept[i] = UINT32_MAX;
    }
    /* A row whose name record was lost names no pass. */
    if (pass >= data->pass_count || data->passes[pass].name[0] == '\0') {
      continue;
    }
    if (i < VKR_RECORDING_PASS_MAX) {
      load->raw_kept[i] = (uint32_t)(first + kept);
    }
    const uint16_t flags = (uint16_t)(at[2] | (at[3] << 8u));
    const float32_t cpu_ms = vkr_load_le_f32(at + 4u);
    const float32_t gpu_ms = vkr_load_le_f32(at + 8u);
    const bool8_t gpu_valid = (flags & VKR_RECORDING_PASS_GPU_VALID) != 0u &&
                              vkr_is_finite_f32(gpu_ms) && gpu_ms >= 0.0f;
    data->pass_rows[first + kept++] = (VkrRecordingPassRow){
        .pass = pass,
        .flags = gpu_valid ? VKR_RECORDING_PASS_GPU_VALID : 0u,
        .cpu_ms = vkr_is_finite_f32(cpu_ms) ? Max(cpu_ms, 0.0f) : 0.0f,
        .gpu_ms = gpu_valid ? gpu_ms : 0.0f,
    };
  }
  data->pass_row_count += kept;
  data->pass_first[frame] = first;
  data->pass_row_counts[frame] = (uint16_t)kept;
  data->pass_frame_flags[frame] = (uint8_t)(vkr_load_le_u32(record->data + 4u) &
                                            VKR_RECORDING_PASSES_GPU_TIMED);
  load->raw_frame = frame;
  load->raw_rows = Min(rows, VKR_RECORDING_PASS_MAX);
}

/* A pass_starts record: the frame's GPU time from its first command
   buffer's start to its last's end, and, to place imported GPU counters,
   that span on the CPU clock and its rows' starts when the passes record
   before it was the same frame's. */
vkr_internal void
vkr_recording_data_fill_pass_starts(VkrRecordingDataLoad *load,
                                    const VkrRecordingRecord *record) {
  const uint64_t frame = record->frame;
  if (!load->data->pass_block || frame >= load->data->frame_count ||
      record->size < VKR_RECORDING_PASS_STARTS_HEADER_SIZE) {
    return;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  const uint32_t flags = vkr_load_le_u32(record->data + 4u);
  if ((uint64_t)VKR_RECORDING_PASS_STARTS_HEADER_SIZE + (uint64_t)rows * 4u !=
          record->size ||
      !(flags & VKR_RECORDING_PASS_STARTS_HOST_CLOCK)) {
    return;
  }
  const uint64_t start_ns = vkr_load_le_u64(record->data + 8u);
  const uint64_t end_ns = vkr_load_le_u64(record->data + 16u);
  if (start_ns == 0u || end_ns <= start_ns) {
    return;
  }
  load->data->gpu_frame_ms[frame] =
      (float32_t)((float64_t)(end_ns - start_ns) * 1.0e-6);
  if (!load->gpu_spans) {
    return;
  }
  load->gpu_spans[frame * 2u] = start_ns;
  load->gpu_spans[frame * 2u + 1u] = end_ns;
  if (load->raw_frame != frame) {
    return;
  }
  const uint8_t *at = record->data + VKR_RECORDING_PASS_STARTS_HEADER_SIZE;
  for (uint32_t i = 0u; i < Min(rows, load->raw_rows); ++i, at += 4u) {
    if (load->raw_kept[i] != UINT32_MAX) {
      load->pass_row_starts[load->raw_kept[i]] = vkr_load_le_f32(at);
    }
  }
}

/* An allocators record: the frame's rows, after the rows of earlier
   records. A second record for a frame is ignored. */
vkr_internal void
vkr_recording_data_fill_allocators(VkrRecordingDataLoad *load,
                                   const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  const uint64_t frame = record->frame;
  const uint32_t rows = vkr_recording_allocator_record_rows(record);
  if (!data->allocator_block || rows == 0u || frame >= data->frame_count ||
      data->allocator_row_counts[frame] > 0u ||
      data->allocator_row_count + rows > load->allocator_rows) {
    return;
  }
  const uint64_t first = data->allocator_row_count;
  const uint8_t *at = record->data + VKR_RECORDING_ALLOCATORS_HEADER_SIZE;
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < rows; ++i, at += VKR_RECORDING_ALLOCATOR_ROW_SIZE) {
    const uint32_t id = vkr_load_le_u32(at);
    if (id >= data->allocator_count) {
      continue;
    }
    VkrRecordingAllocatorRow *row = &data->allocator_rows[first + kept++];
    *row = (VkrRecordingAllocatorRow){
        .allocator = id,
        .scope_depth_high = vkr_load_le_u32(at + 4u),
        .allocs = vkr_load_le_u32(at + 8u),
        .frees = vkr_load_le_u32(at + 12u),
        .reallocs = vkr_load_le_u32(at + 16u),
        .scopes = vkr_load_le_u32(at + 20u),
        .live_bytes = vkr_load_le_u64(at + 24u),
        .used_bytes = vkr_load_le_u64(at + 32u),
        .committed_bytes = vkr_load_le_u64(at + 40u),
        .reserved_bytes = vkr_load_le_u64(at + 48u),
        .peak_used_bytes = vkr_load_le_u64(at + 56u),
    };
  }
  data->allocator_row_count += kept;
  data->allocator_first[frame] = first;
  data->allocator_row_counts[frame] = (uint16_t)kept;
}

/* A census record: a box set or candidate order into the next slot, or a
   frame's states after the words of earlier frames. */
vkr_internal void
vkr_recording_data_fill_draws(VkrRecordingDataLoad *load,
                              const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  if (!data->draw_block || record->frame >= data->frame_count) {
    return;
  }
  if (vkr_recording_is_stream(load->draw_bounds, record)) {
    const uint32_t rows = vkr_recording_draw_bounds_rows(record);
    if (rows == UINT32_MAX ||
        data->draw_bounds_count >= load->draw_bounds_capacity ||
        load->draw_objects_used + rows > load->draw_objects) {
      return;
    }
    VkrRecordingDrawBounds *bounds =
        &data->draw_bounds[data->draw_bounds_count++];
    VkrRecordingDrawObject *objects =
        (VkrRecordingDrawObject *)(data->draw_bounds +
                                   load->draw_bounds_capacity) +
        load->draw_objects_used;
    load->draw_objects_used += rows;
    *bounds = (VkrRecordingDrawBounds){
        .frame = record->frame,
        .cell_size = vkr_load_le_f32(record->data + 8u),
        .objects = objects,
        .object_count = rows,
    };
    const uint8_t *at = record->data + VKR_RECORDING_DRAW_BOUNDS_HEADER_SIZE;
    for (uint32_t i = 0u; i < rows;
         ++i, at += VKR_RECORDING_DRAW_BOUNDS_ROW_SIZE) {
      objects[i].object_id = vkr_load_le_u32(at);
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        objects[i].min[axis] = vkr_load_le_f32(at + 4u + 4u * axis);
        objects[i].max[axis] = vkr_load_le_f32(at + 16u + 4u * axis);
      }
    }
    return;
  }
  if (vkr_recording_is_stream(load->draw_ids, record)) {
    const uint32_t count = vkr_recording_draw_ids_count(record);
    if (count == UINT32_MAX ||
        data->draw_ids_count >= load->draw_ids_capacity ||
        load->draw_id_values_used + count > load->draw_id_values) {
      return;
    }
    VkrRecordingDrawIds *ids = &data->draw_ids[data->draw_ids_count++];
    uint32_t *values = (uint32_t *)(data->draw_ids + load->draw_ids_capacity) +
                       load->draw_id_values_used;
    load->draw_id_values_used += count;
    *ids = (VkrRecordingDrawIds){
        .frame = record->frame,
        .generation = vkr_load_le_u64(record->data + 8u),
        .ids = values,
        .count = count,
    };
    MemCopy(values, record->data + VKR_RECORDING_DRAW_IDS_HEADER_SIZE,
            4ull * count);
    return;
  }
  const uint32_t words = vkr_recording_draw_state_words(record);
  const uint64_t frame = record->frame;
  if (words == UINT32_MAX || data->draw_id_table[frame] != UINT32_MAX) {
    return;
  }
  if (load->draw_seen++ % load->draw_stride != 0u ||
      data->draw_word_count + words > load->draw_words) {
    data->draw_words_cut += words;
    return;
  }
  /* The ids it follows: the newest of its generation recorded so far. */
  const uint64_t generation = vkr_load_le_u64(record->data + 16u);
  const uint32_t count = vkr_load_le_u32(record->data);
  uint32_t table = UINT32_MAX;
  for (uint32_t i = data->draw_ids_count; i-- > 0u;) {
    if (data->draw_ids[i].generation == generation &&
        data->draw_ids[i].count == count) {
      table = i;
      break;
    }
  }
  if (table == UINT32_MAX) {
    return;
  }
  data->draw_first[frame] = data->draw_word_count;
  data->draw_candidates[frame] = count;
  data->draw_id_table[frame] = table;
  data->draw_flags[frame] = vkr_load_le_u32(record->data + 4u);
  data->draw_overflow[frame] = vkr_load_le_u32(record->data + 8u);
  MemCopy(data->draw_words + data->draw_word_count,
          record->data + VKR_RECORDING_DRAW_STATES_HEADER_SIZE, 4ull * words);
  data->draw_word_count += words;
  ++data->draw_frames;
}

/* Text at `offset` of a record, kept when it lies inside the names. */
vkr_internal VkrRecordingGraphText vkr_recording_graph_load_text(
    const uint8_t *at, uint32_t length, uint32_t name_bytes) {
  const uint32_t offset = vkr_load_le_u32(at);
  if (offset > name_bytes || length > name_bytes - offset) {
    return (VkrRecordingGraphText){0};
  }
  return (VkrRecordingGraphText){.offset = offset, .length = length};
}

/* A graph record, parsed into the next graph of the graph block. */
vkr_internal void
vkr_recording_data_fill_graph(VkrRecordingDataLoad *load,
                              const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  VkrRecordingGraphCounts counts;
  if (!data->graph_block || data->graph_count >= load->graph_capacity ||
      !vkr_recording_graph_counts(record, &counts)) {
    return;
  }
  /* The parsed parts follow the graphs and every graph parsed before. */
  uint8_t *storage =
      (uint8_t *)data->graph_block +
      vkr_align_up_u64(sizeof(VkrRecordingGraph) * load->graph_capacity, 8u) +
      load->graph_used;
  load->graph_used += vkr_recording_graph_parsed_bytes(&counts);
  VkrRecordingGraph *graph = &data->graphs[data->graph_count++];
  const uint8_t *header = record->data;
  *graph = (VkrRecordingGraph){
      .frame = record->frame,
      .viewport_width = vkr_load_le_u32(header + 24u),
      .viewport_height = vkr_load_le_u32(header + 28u),
      .render_scale = vkr_load_le_f32(header + 32u),
      .truncated =
          (vkr_load_le_u32(header + 20u) & VKR_RECORDING_GRAPH_TRUNCATED) != 0u,
      .pass_count = counts.passes,
      .use_count = counts.uses,
      .resource_count = counts.resources,
      .name_bytes = counts.name_bytes,
  };
  graph->passes = (VkrRecordingGraphPass *)storage;
  storage +=
      vkr_align_up_u64(sizeof(VkrRecordingGraphPass) * counts.passes, 8u);
  graph->uses = (VkrRecordingGraphUse *)storage;
  storage += vkr_align_up_u64(sizeof(VkrRecordingGraphUse) * counts.uses, 8u);
  graph->resources = (VkrRecordingGraphResource *)storage;
  storage += vkr_align_up_u64(
      sizeof(VkrRecordingGraphResource) * counts.resources, 8u);
  char *names = (char *)storage;
  graph->names = names;

  const uint8_t *at = header + VKR_RECORDING_GRAPH_HEADER_SIZE;
  const uint8_t *use_at =
      at + (uint64_t)VKR_RECORDING_GRAPH_PASS_SIZE * counts.passes;
  const uint8_t *resource_at =
      use_at + (uint64_t)VKR_RECORDING_GRAPH_USE_SIZE * counts.uses;
  const uint8_t *name_at =
      resource_at +
      (uint64_t)VKR_RECORDING_GRAPH_RESOURCE_SIZE * counts.resources;
  MemCopy(names, name_at, counts.name_bytes);

  for (uint32_t i = 0u; i < counts.passes;
       ++i, at += VKR_RECORDING_GRAPH_PASS_SIZE) {
    const uint32_t first_use = vkr_load_le_u32(at + 12u);
    const uint32_t use_count = vkr_load_le_u32(at + 16u);
    const bool8_t uses_fit =
        first_use <= counts.uses && use_count <= counts.uses - first_use;
    const uint32_t order = vkr_load_le_u32(at + 8u);
    graph->passes[i] = (VkrRecordingGraphPass){
        .name = vkr_recording_graph_load_text(at, vkr_load_le_u16(at + 4u),
                                              counts.name_bytes),
        .kind = at[6],
        .flags = at[7],
        .order = order,
        .first_use = uses_fit ? first_use : 0u,
        .use_count = uses_fit ? use_count : 0u,
        .pass = UINT32_MAX,
    };
    graph->scheduled_count += order != UINT32_MAX ? 1u : 0u;
  }
  for (uint32_t i = 0u; i < counts.uses;
       ++i, use_at += VKR_RECORDING_GRAPH_USE_SIZE) {
    graph->uses[i] = (VkrRecordingGraphUse){
        .resource = UINT32_MAX,
        .role = use_at[6],
        .flags = use_at[7],
    };
  }
  for (uint32_t i = 0u; i < counts.resources;
       ++i, resource_at += VKR_RECORDING_GRAPH_RESOURCE_SIZE) {
    const uint8_t *r = resource_at;
    uint64_t bytes = 0u;
    MemCopy(&bytes, r + 40u, sizeof(bytes));
    graph->resources[i] = (VkrRecordingGraphResource){
        .name = vkr_recording_graph_load_text(r, vkr_load_le_u16(r + 4u),
                                              counts.name_bytes),
        .flags = vkr_load_le_u16(r + 6u),
        .format = vkr_recording_graph_load_text(
            r + 8u, vkr_load_le_u16(r + 12u), counts.name_bytes),
        .lifetime = vkr_recording_graph_load_text(
            r + 16u, vkr_load_le_u16(r + 14u), counts.name_bytes),
        .width = vkr_load_le_u32(r + 20u),
        .height = vkr_load_le_u32(r + 24u),
        .depth = vkr_load_le_u32(r + 28u),
        .layers = vkr_load_le_u16(r + 32u),
        .mip_levels = vkr_load_le_u16(r + 34u),
        .samples = vkr_load_le_u16(r + 36u),
        .bytes = bytes,
        .first_order = vkr_load_le_u32(r + 48u),
        .last_order = vkr_load_le_u32(r + 52u),
    };
  }
  /* Each use names its resource; resolve it to the graph's own. */
  const uint8_t *name_use =
      header + VKR_RECORDING_GRAPH_HEADER_SIZE +
      (uint64_t)VKR_RECORDING_GRAPH_PASS_SIZE * counts.passes;
  for (uint32_t i = 0u; i < counts.uses;
       ++i, name_use += VKR_RECORDING_GRAPH_USE_SIZE) {
    const VkrRecordingGraphText text = vkr_recording_graph_load_text(
        name_use, vkr_load_le_u16(name_use + 4u), counts.name_bytes);
    for (uint32_t r = 0u; text.length > 0u && r < counts.resources; ++r) {
      const VkrRecordingGraphText known = graph->resources[r].name;
      if (known.length == text.length &&
          MemCompare(names + known.offset, names + text.offset, text.length) ==
              0) {
        graph->uses[i].resource = r;
        break;
      }
    }
  }
}

/* The index of event name `name` (`length` bytes), added when new;
   UINT32_MAX when the table is full. */
vkr_internal uint32_t vkr_recording_data_event_name(VkrRecordingData *data,
                                                    const uint8_t *name,
                                                    uint32_t length) {
  length = Min(length, (uint32_t)VKR_RECORDING_NAME_MAX);
  for (uint32_t i = 0u; i < data->event_name_count; ++i) {
    const char *known = data->event_names[i].name;
    if (string_length(known) == length &&
        MemCompare(known, name, length) == 0) {
      return i;
    }
  }
  if (data->event_name_count == VKR_RECORDING_DATA_EVENT_NAME_MAX) {
    return UINT32_MAX;
  }
  VkrRecordingDataEventName *entry = &data->event_names[data->event_name_count];
  MemCopy(entry->name, name, length);
  entry->name[length] = '\0';
  return data->event_name_count++;
}

/* An event record, named in the data's name table. */
vkr_internal void
vkr_recording_data_fill_event(VkrRecordingDataLoad *load,
                              const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  VkrRecordingEventView view;
  if (!vkr_recording_event_parse(record->data, record->size, &view)) {
    return;
  }
  const uint32_t name_index =
      data->event_count < load->event_capacity
          ? vkr_recording_data_event_name(data, view.name, view.name_length)
          : UINT32_MAX;
  if (name_index == UINT32_MAX) {
    ++data->events_dropped;
    return;
  }
  data->events[data->event_count++] = (VkrRecordingDataEvent){
      .frame = record->frame,
      .name = name_index,
      .count = view.count,
      .ms = view.ms,
      .engine = view.engine,
  };
}

/* Keeps the first fact of each key: a fact written again after a full ring
   repeats it. */
vkr_internal void
vkr_recording_data_fill_hardware(VkrRecordingDataLoad *load,
                                 const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  VkrRecordingHardwareFact fact;
  if (!data->hardware_block ||
      data->hardware_count >= load->hardware_capacity ||
      !vkr_recording_hardware_parse(record, &fact)) {
    return;
  }
  for (uint32_t i = 0u; i < data->hardware_count; ++i) {
    if (string_equals(data->hardware[i].key, fact.key)) {
      return;
    }
  }
  data->hardware[data->hardware_count++] = fact;
}

vkr_internal void
vkr_recording_data_fill_pipeline(VkrRecordingDataLoad *load,
                                 const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  if (!data->hardware_block ||
      data->pipeline_count >= load->pipeline_capacity) {
    return;
  }
  VkrRecordingPipeline pipeline;
  if (!vkr_recording_pipeline_parse(record, &pipeline, NULL) ||
      data->pipeline_stat_count + pipeline.stat_count >
          load->pipeline_stat_capacity) {
    return;
  }
  (void)vkr_recording_pipeline_parse(
      record, &pipeline, data->pipeline_stats + data->pipeline_stat_count);
  pipeline.stat_first = data->pipeline_stat_count;
  data->pipeline_stat_count += pipeline.stat_count;
  data->pipelines[data->pipeline_count++] = pipeline;
}

vkr_internal void vkr_recording_data_fill(VkrRecordingDataLoad *load,
                                          const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  if (vkr_recording_is_stream(load->pass_name, record)) {
    const uint32_t id = vkr_recording_pass_name_id(record);
    if (data->pass_block && id < data->pass_count) {
      const uint32_t length = record->size - 4u;
      MemCopy(data->passes[id].name, record->data + 4u, length);
      data->passes[id].name[length] = '\0';
    }
    return;
  }
  if (vkr_recording_is_stream(load->passes, record)) {
    vkr_recording_data_fill_passes(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->graph, record)) {
    vkr_recording_data_fill_graph(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->hardware, record)) {
    vkr_recording_data_fill_hardware(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->pipeline, record)) {
    vkr_recording_data_fill_pipeline(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->pass_starts, record)) {
    vkr_recording_data_fill_pass_starts(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->gpu_counters, record)) {
    const uint64_t bytes = 8u + vkr_align_up_u64(record->size, 8u);
    if (load->sessions &&
        record->size >= VKR_RECORDING_GPU_COUNTERS_SESSION_HEADER_SIZE &&
        load->session_used + bytes <= load->session_bytes) {
      const uint64_t size = record->size;
      MemCopy(load->sessions + load->session_used, &size, sizeof(size));
      MemCopy(load->sessions + load->session_used + 8u, record->data,
              record->size);
      load->session_used += bytes;
    }
    return;
  }
  if (vkr_recording_is_stream(load->allocator_name, record)) {
    const uint32_t id = vkr_recording_allocator_name_id(record);
    if (data->allocator_block && id < data->allocator_count) {
      VkrRecordingAllocator *allocator = &data->allocators[id];
      const uint32_t length = record->size - 8u;
      allocator->type = record->data[4];
      allocator->arena = record->data[5];
      MemCopy(allocator->name, record->data + 8u, length);
      allocator->name[length] = '\0';
    }
    return;
  }
  if (vkr_recording_is_stream(load->allocators, record)) {
    vkr_recording_data_fill_allocators(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->draw_bounds, record) ||
      vkr_recording_is_stream(load->draw_ids, record) ||
      vkr_recording_is_stream(load->draw_states, record)) {
    vkr_recording_data_fill_draws(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->zone_name, record)) {
    const uint32_t id = vkr_recording_zone_name_id(record);
    if (data->zone_block && id < data->zone_name_count) {
      const uint32_t length = record->size - 4u;
      MemCopy(data->zone_names[id].name, record->data + 4u, length);
      data->zone_names[id].name[length] = '\0';
    }
    return;
  }
  if (vkr_recording_is_stream(load->zone, record)) {
    if (!load->zone_calls || record->size != sizeof(VkrZoneRecord) ||
        record->frame >= data->frame_count) {
      return;
    }
    if (load->zone_call_count >= load->zone_records) {
      ++data->zones_cut;
      return;
    }
    VkrZoneRecord zone;
    MemCopy(&zone, record->data, sizeof(zone));
    if (zone.zone >= data->zone_name_count || zone.end_ns < zone.begin_ns) {
      return;
    }
    load->zone_calls[load->zone_call_count++] = (VkrRecordingZoneCall){
        .frame = (uint32_t)record->frame,
        .zone = zone.zone,
        .thread = zone.thread == VKR_ZONE_THREAD_MAIN
                      ? VKR_RECORDING_ZONE_THREAD_MAIN
                  : zone.thread == VKR_ZONE_THREAD_RENDER
                      ? VKR_RECORDING_ZONE_THREAD_RENDER
                      : VKR_RECORDING_ZONE_THREAD_WORKER,
        .depth = (uint8_t)Min(zone.depth, 255u),
        .ms = (float32_t)((float64_t)(zone.end_ns - zone.begin_ns) * 1.0e-6),
    };
    return;
  }
  if (vkr_recording_is_stream(load->measurement_name, record)) {
    vkr_recording_data_fill_measurement_name(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->measurements, record)) {
    vkr_recording_data_fill_measurements(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->event, record)) {
    vkr_recording_data_fill_event(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->marker, record)) {
    if (record->size < 4u || data->marker_count >= load->marker_capacity) {
      return;
    }
    VkrRecordingDataMarker *marker = &data->markers[data->marker_count++];
    marker->frame = record->frame;
    marker->kind = vkr_load_le_u32(record->data);
    const uint32_t length =
        Min(record->size - 4u, (uint32_t)VKR_RECORDING_DATA_LABEL_MAX);
    MemCopy(marker->label, record->data + 4u, length);
    marker->label[length] = '\0';
    return;
  }
  if (record->frame >= data->frame_count) {
    return;
  }
  const uint64_t frame = record->frame;
  if (vkr_recording_record_fits(load->frame, record) && load->display_dt) {
    const float64_t display_dt =
        vkr_recording_field_value(load->display_dt, record->data, 0u);
    if (vkr_is_finite_f64(display_dt)) {
      data->display_dt[frame] = display_dt;
      data->frame_valid[frame] = 1u;
    }
  } else if (vkr_recording_record_fits(load->camera, record) &&
             load->position) {
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      data->positions[frame * 3u + axis] =
          vkr_recording_field_value(load->position, record->data, axis);
    }
    data->yaws[frame] =
        load->yaw
            ? (float32_t)vkr_recording_field_value(load->yaw, record->data, 0u)
            : 0.0f;
    data->pitches[frame] = load->pitch ? (float32_t)vkr_recording_field_value(
                                             load->pitch, record->data, 0u)
                                       : 0.0f;
    data->fovs[frame] =
        load->fov
            ? (float32_t)vkr_recording_field_value(load->fov, record->data, 0u)
            : 0.0f;
    data->far_clips[frame] =
        load->far_clip ? (float32_t)vkr_recording_field_value(load->far_clip,
                                                              record->data, 0u)
                       : 0.0f;
    data->camera_valid[frame] = 1u;
  } else if (vkr_recording_record_fits(load->metrics, record)) {
    vkr_recording_data_fill_metrics(load, record);
  } else if (vkr_recording_record_fits(load->player, record) &&
             load->player_position) {
    /* Ticks arrive in order, so the frame keeps its last one. */
    float64_t speed_squared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      data->player_positions[frame * 3u + axis] =
          vkr_recording_field_value(load->player_position, record->data, axis);
      const float64_t velocity =
          load->player_velocity ? vkr_recording_field_value(
                                      load->player_velocity, record->data, axis)
                                : 0.0;
      speed_squared += velocity * velocity;
    }
    data->player_speeds[frame] = (float32_t)vkr_sqrt_f64(speed_squared);
    data->player_valid[frame] = 1u;
    data->has_player = true_v;
  }
}

/* One allocation: series and markers, then the float64 arrays, the float32
   yaws and speeds, and the byte flags. */
vkr_internal bool8_t vkr_recording_data_allocate(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  const uint64_t frames = load->frame_count;
  /* The metrics, the measurements, then the derived Scene frame time. */
  const uint64_t series =
      (uint64_t)load->metric_count + load->measurement_capacity + 1u;
  const uint64_t series_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingSeries) * series, 8u);
  const uint64_t marker_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataMarker) * (uint64_t)load->marker_capacity, 8u);
  const uint64_t event_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataEvent) * (uint64_t)load->event_capacity +
          sizeof(VkrRecordingDataEventName) *
              (uint64_t)VKR_RECORDING_DATA_EVENT_NAME_MAX,
      8u);
  const uint64_t f64_bytes = sizeof(float64_t) * frames * (8u + series);
  const uint64_t f32_bytes =
      vkr_align_up_u64(sizeof(float32_t) * frames * 5u, 8u);
  const uint64_t flag_bytes = frames * (3u + series);
  const uint64_t block_bytes = series_bytes + marker_bytes + event_bytes +
                               f64_bytes + f32_bytes + flag_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->block = block;
  data->block_bytes = block_bytes;
  data->frame_count = frames;
  data->series_count = (uint32_t)series;

  uint8_t *cursor = block;
  data->series = (VkrRecordingSeries *)cursor;
  cursor += series_bytes;
  data->markers = (VkrRecordingDataMarker *)cursor;
  cursor += marker_bytes;
  data->event_names = (VkrRecordingDataEventName *)cursor;
  data->events = (VkrRecordingDataEvent *)(data->event_names +
                                           VKR_RECORDING_DATA_EVENT_NAME_MAX);
  cursor += event_bytes;
  float64_t *f64 = (float64_t *)cursor;
  data->times = f64;
  data->display_dt = f64 + frames;
  data->positions = f64 + frames * 2u;
  data->player_positions = f64 + frames * 5u;
  for (uint64_t i = 0u; i < series; ++i) {
    data->series[i].values = f64 + frames * (8u + i);
  }
  cursor += f64_bytes;
  data->yaws = (float32_t *)cursor;
  data->player_speeds = data->yaws + frames;
  data->pitches = data->yaws + frames * 2u;
  data->fovs = data->yaws + frames * 3u;
  data->far_clips = data->yaws + frames * 4u;
  cursor += f32_bytes;
  data->frame_valid = cursor;
  data->camera_valid = cursor + frames;
  data->player_valid = cursor + frames * 2u;
  for (uint64_t i = 0u; i < series; ++i) {
    data->series[i].valid = cursor + frames * (3u + i);
    if (i >= load->metric_count) {
      continue;
    }
    const VkrRecordingField *field = load->metric_fields[i];
    MemCopy(data->series[i].name, field->name, string_length(field->name));
    MemCopy(data->series[i].unit, field->unit, string_length(field->unit));
  }
  VkrRecordingSeries *scene = &data->series[series - 1u];
  string_format(scene->name, sizeof(scene->name), "%s",
                VKR_RECORDING_SCENE_FRAME_SERIES);
  string_format(scene->unit, sizeof(scene->unit), "ms");
  return true_v;
}

/* The pass block: names, then rows, then the per-frame row index, counts
   and flags. Nothing without pass names. */
vkr_internal bool8_t
vkr_recording_data_allocate_passes(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->pass_capacity == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  load->pass_rows = Min(load->pass_rows, VKR_RECORDING_DATA_PASS_ROW_MAX);
  const uint64_t name_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataPass) * (uint64_t)load->pass_capacity, 8u);
  const uint64_t row_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingPassRow) * load->pass_rows, 8u);
  const uint64_t first_bytes = sizeof(uint64_t) * frames;
  const uint64_t count_bytes = sizeof(uint16_t) * frames;
  const uint64_t span_bytes = sizeof(float32_t) * frames;
  const uint64_t block_bytes =
      name_bytes + row_bytes + first_bytes + span_bytes + count_bytes + frames;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->pass_block = block;
  data->pass_block_bytes = block_bytes;
  data->passes = (VkrRecordingDataPass *)block;
  data->pass_count = load->pass_capacity;
  data->pass_rows = (VkrRecordingPassRow *)(block + name_bytes);
  data->pass_first = (uint64_t *)(block + name_bytes + row_bytes);
  data->gpu_frame_ms = (float32_t *)(data->pass_first + frames);
  data->pass_row_counts = (uint16_t *)(data->gpu_frame_ms + frames);
  data->pass_frame_flags = (uint8_t *)(data->pass_row_counts + frames);
  for (uint64_t i = 0u; i < frames; ++i) {
    data->gpu_frame_ms[i] = (float32_t)VKR_FLOAT_NAN;
  }
  return true_v;
}

/* The hardware block: facts, pipelines, then their statistics. Nothing
   without either. */
vkr_internal bool8_t
vkr_recording_data_allocate_hardware(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->hardware_capacity == 0u && load->pipeline_capacity == 0u) {
    return true_v;
  }
  load->pipeline_stat_capacity =
      Min(load->pipeline_stat_capacity, VKR_RECORDING_PIPELINE_STATS_MAX);
  const uint64_t fact_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingHardwareFact) * (uint64_t)load->hardware_capacity, 8u);
  const uint64_t pipeline_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingPipeline) * (uint64_t)load->pipeline_capacity, 8u);
  const uint64_t stat_bytes =
      sizeof(VkrRecordingPipelineStat) * (uint64_t)load->pipeline_stat_capacity;
  const uint64_t block_bytes = fact_bytes + pipeline_bytes + stat_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->hardware_block = block;
  data->hardware_block_bytes = block_bytes;
  data->hardware = (VkrRecordingHardwareFact *)block;
  data->pipelines = (VkrRecordingPipeline *)(block + fact_bytes);
  data->pipeline_stats =
      (VkrRecordingPipelineStat *)(block + fact_bytes + pipeline_bytes);
  return true_v;
}

/* The allocator block: names, rows, then the per-frame row index and
   counts. Nothing without allocator names. */
vkr_internal bool8_t
vkr_recording_data_allocate_allocators(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->allocator_capacity == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  load->allocator_rows =
      Min(load->allocator_rows, VKR_RECORDING_DATA_PASS_ROW_MAX);
  const uint64_t name_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingAllocator) * (uint64_t)load->allocator_capacity, 8u);
  const uint64_t row_bytes =
      sizeof(VkrRecordingAllocatorRow) * load->allocator_rows;
  const uint64_t first_bytes = sizeof(uint64_t) * frames;
  const uint64_t count_bytes = sizeof(uint16_t) * frames;
  const uint64_t block_bytes =
      name_bytes + row_bytes + first_bytes + count_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->allocator_block = block;
  data->allocator_block_bytes = block_bytes;
  data->allocators = (VkrRecordingAllocator *)block;
  data->allocator_count = load->allocator_capacity;
  data->allocator_rows = (VkrRecordingAllocatorRow *)(block + name_bytes);
  data->allocator_first = (uint64_t *)(block + name_bytes + row_bytes);
  data->allocator_row_counts = (uint16_t *)(data->allocator_first + frames);
  return true_v;
}

/* The zone block: names, then the per-frame row index and counts; the rows
   come once the calls are reduced. The calls take scratch until then.
   Nothing without zone names. */
vkr_internal bool8_t
vkr_recording_data_allocate_zones(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->zone_capacity == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  load->zone_records = Min(load->zone_records, VKR_RECORDING_DATA_PASS_ROW_MAX);
  /* Every call may be its own row. */
  const uint64_t name_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingZoneName) * (uint64_t)load->zone_capacity, 8u);
  const uint64_t row_bytes = sizeof(VkrRecordingZoneRow) * load->zone_records;
  const uint64_t first_bytes = sizeof(uint64_t) * frames;
  const uint64_t count_bytes = sizeof(uint32_t) * frames;
  const uint64_t block_bytes =
      name_bytes + row_bytes + first_bytes + count_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->zone_block = block;
  data->zone_block_bytes = block_bytes;
  data->zone_names = (VkrRecordingZoneName *)block;
  data->zone_name_count = load->zone_capacity;
  data->zone_rows = (VkrRecordingZoneRow *)(block + name_bytes);
  data->zone_first = (uint64_t *)(block + name_bytes + row_bytes);
  data->zone_row_counts = (uint32_t *)(data->zone_first + frames);
  if (load->zone_records > 0u) {
    load->zone_calls = vkr_allocator_alloc(
        data->allocator, sizeof(VkrRecordingZoneCall) * load->zone_records,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!load->zone_calls) {
      return false_v;
    }
  }
  return true_v;
}

/* By frame, zone and thread. */
vkr_internal int32_t vkr_recording_compare_zone_calls(const void *lhs,
                                                      const void *rhs) {
  const VkrRecordingZoneCall *a = lhs;
  const VkrRecordingZoneCall *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  if (a->zone != b->zone) {
    return a->zone < b->zone ? -1 : 1;
  }
  return a->thread < b->thread ? -1 : (a->thread > b->thread ? 1 : 0);
}

/* Reduces the calls to a row per frame, zone and thread, then frees them. */
vkr_internal void vkr_recording_data_finish_zones(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (!load->zone_calls) {
    return;
  }
  VkrRecordingZoneCall *calls = load->zone_calls;
  const uint64_t count = load->zone_call_count;
  vkr_sort(calls, count, sizeof(*calls), vkr_recording_compare_zone_calls);
  uint64_t i = 0u;
  while (i < count) {
    const VkrRecordingZoneCall *head = &calls[i];
    VkrRecordingZoneRow row = {
        .zone = head->zone, .thread = head->thread, .depth = head->depth};
    uint64_t j = i;
    while (j < count && calls[j].frame == head->frame &&
           calls[j].zone == head->zone && calls[j].thread == head->thread) {
      ++row.calls;
      row.total_ms += calls[j].ms;
      row.max_ms = Max(row.max_ms, calls[j].ms);
      row.depth = (uint8_t)Min(row.depth, calls[j].depth);
      ++j;
    }
    if (data->zone_row_counts[head->frame] == 0u) {
      data->zone_first[head->frame] = data->zone_row_count;
    }
    data->zone_rows[data->zone_row_count++] = row;
    ++data->zone_row_counts[head->frame];
    i = j;
  }
  vkr_allocator_free(data->allocator, load->zone_calls,
                     sizeof(VkrRecordingZoneCall) * load->zone_records,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  load->zone_calls = NULL;
}

/* The census block: the box sets and their objects, the candidate orders
   and their ids, the state words, then the per-frame index. Nothing without
   census records. */
vkr_internal bool8_t
vkr_recording_data_allocate_draws(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->draw_words == 0u && load->draw_bounds_capacity == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  /* A census over the budget keeps every draw_stride-th frame, with a
     frame's worth of room for records that differ in size. */
  const uint64_t budget = 4ull * VKR_RECORDING_DATA_PASS_ROW_MAX;
  load->draw_stride =
      load->draw_words > budget
          ? (uint32_t)((load->draw_words + budget - 1u) / budget) + 1u
          : 1u;
  data->draw_stride = load->draw_stride;
  load->draw_words = Min(load->draw_words, budget);
  const uint64_t bounds_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDrawBounds) * load->draw_bounds_capacity +
          sizeof(VkrRecordingDrawObject) * load->draw_objects,
      8u);
  const uint64_t ids_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingDrawIds) * load->draw_ids_capacity +
                           sizeof(uint32_t) * load->draw_id_values,
                       8u);
  const uint64_t word_bytes =
      vkr_align_up_u64(sizeof(uint32_t) * load->draw_words, 8u);
  const uint64_t frame_bytes =
      (sizeof(uint64_t) + 4u * sizeof(uint32_t)) * frames;
  const uint64_t block_bytes =
      bounds_bytes + ids_bytes + word_bytes + frame_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->draw_block = block;
  data->draw_block_bytes = block_bytes;
  data->draw_bounds = (VkrRecordingDrawBounds *)block;
  data->draw_ids = (VkrRecordingDrawIds *)(block + bounds_bytes);
  data->draw_words = (uint32_t *)(block + bounds_bytes + ids_bytes);
  data->draw_first =
      (uint64_t *)(block + bounds_bytes + ids_bytes + word_bytes);
  data->draw_candidates = (uint32_t *)(data->draw_first + frames);
  data->draw_id_table = data->draw_candidates + frames;
  data->draw_flags = data->draw_id_table + frames;
  data->draw_overflow = data->draw_flags + frames;
  for (uint64_t f = 0u; f < frames; ++f) {
    data->draw_id_table[f] = UINT32_MAX;
  }
  return true_v;
}

/* The graph block: the graphs, then each one's parsed parts. Nothing
   without graph records. */
vkr_internal bool8_t
vkr_recording_data_allocate_graphs(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->graph_capacity == 0u) {
    return true_v;
  }
  const uint64_t block_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingGraph) * load->graph_capacity, 8u) +
      load->graph_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->graph_block = block;
  data->graph_block_bytes = block_bytes;
  data->graphs = (VkrRecordingGraph *)block;
  return true_v;
}

/* Orders the graphs by frame, keeping record order for equal frames, and
   joins each pass to the recording's pass of the same name. */
vkr_internal void vkr_recording_data_finish_graphs(VkrRecordingData *data) {
  for (uint32_t i = 1u; i < data->graph_count; ++i) {
    const VkrRecordingGraph moved = data->graphs[i];
    uint32_t j = i;
    while (j > 0u && data->graphs[j - 1u].frame > moved.frame) {
      data->graphs[j] = data->graphs[j - 1u];
      --j;
    }
    data->graphs[j] = moved;
  }
  for (uint32_t g = 0u; g < data->graph_count; ++g) {
    VkrRecordingGraph *graph = &data->graphs[g];
    for (uint32_t i = 0u; i < graph->pass_count; ++i) {
      const String8 name =
          vkr_recording_graph_text(graph, graph->passes[i].name);
      for (uint32_t p = 0u; name.length > 0u && p < data->pass_count; ++p) {
        const char *known = data->passes[p].name;
        if (string_length(known) == name.length &&
            MemCompare(known, name.str, name.length) == 0) {
          graph->passes[i].pass = p;
          break;
        }
      }
    }
  }
}

/* By frame, then by name, then by count: a total order, so equal inputs
   sort alike. */
vkr_internal int32_t vkr_recording_compare_events(const void *lhs,
                                                  const void *rhs) {
  const VkrRecordingDataEvent *a = lhs;
  const VkrRecordingDataEvent *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  if (a->name != b->name) {
    return a->name < b->name ? -1 : 1;
  }
  if (a->count != b->count) {
    return a->count < b->count ? -1 : 1;
  }
  return a->ms < b->ms ? -1 : (a->ms > b->ms ? 1 : 0);
}

/* Whether series `series` has a sample. */
vkr_internal bool8_t
vkr_recording_data_has_samples(const VkrRecordingData *data, uint32_t series) {
  for (uint64_t i = 0u; series != UINT32_MAX && i < data->frame_count; ++i) {
    if (data->series[series].valid[i]) {
      return true_v;
    }
  }
  return false_v;
}

/* Which frames rendered a Scene, and the Scene's frame time when it renders
   on fewer frames than the loop runs (the editor presenter, ADR-111): the
   recorded frame.scene_interval, from GPU completions, or for recordings
   without it the derived frame.scene, the loop time between the frames that
   built a Scene. Either becomes the frame-time series; the derived series
   is dropped when unused. cpu.graph_build is measured only while a Scene
   frame is built. */
vkr_internal void vkr_recording_data_finish_scene(VkrRecordingData *data) {
  const uint32_t derived = data->series_count - 1u;
  data->rendered_series =
      vkr_recording_data_find_series(data, "cpu.graph_build");
  data->rendered_frames = data->frame_count;
  if (data->rendered_series != UINT32_MAX) {
    uint64_t rendered = 0u;
    for (uint64_t i = 0u; i < data->frame_count; ++i) {
      rendered += data->series[data->rendered_series].valid[i] ? 1u : 0u;
    }
    /* Every frame renders outside the presenter, save a few around loads. */
    data->scene_decoupled =
        rendered > 0u && rendered * 20u < data->frame_count * 19u;
    data->rendered_frames =
        data->scene_decoupled ? rendered : data->frame_count;
  }
  /* A render-side figure (draws, passes, GPU) is a counter that reads zero
     on frames that rendered no Scene, which is not a measurement: series
     that are never non-zero there leave those frames unsampled, so their
     means and medians cover rendered frames only. */
  if (data->scene_decoupled) {
    const uint8_t *rendered = data->series[data->rendered_series].valid;
    for (uint32_t s = 0u; s < derived; ++s) {
      VkrRecordingSeries *series = &data->series[s];
      bool8_t idle_value = false_v;
      for (uint64_t i = 0u; i < data->frame_count && !idle_value; ++i) {
        idle_value =
            !rendered[i] && series->valid[i] && series->values[i] != 0.0;
      }
      for (uint64_t i = 0u; i < data->frame_count && !idle_value; ++i) {
        series->valid[i] = rendered[i] ? series->valid[i] : 0u;
      }
    }
  }
  const uint32_t interval =
      vkr_recording_data_find_series(data, "frame.scene_interval");
  if (vkr_recording_data_has_samples(data, interval)) {
    data->frame_time_series = interval;
  } else if (data->scene_decoupled) {
    VkrRecordingSeries *scene = &data->series[derived];
    const uint8_t *rendered = data->series[data->rendered_series].valid;
    uint64_t previous = UINT64_MAX;
    for (uint64_t i = 0u; i < data->frame_count; ++i) {
      if (!rendered[i]) {
        continue;
      }
      if (previous != UINT64_MAX) {
        scene->values[i] = (data->times[i] - data->times[previous]) * 1000.0;
        scene->valid[i] = 1u;
      }
      previous = i;
    }
    data->frame_time_series = derived;
    return;
  }
  data->series_count = derived;
}

/* Repeats the last frame and pose over lost ones, builds the clock and picks
   the frame-time series. */
vkr_internal void vkr_recording_data_finish(VkrRecordingData *data) {
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (!data->frame_valid[i]) {
      ++data->missing_frames;
      if (i > 0u) {
        data->display_dt[i] = data->display_dt[i - 1u];
      }
    }
    if (!data->camera_valid[i] && i > 0u) {
      MemCopy(&data->positions[i * 3u], &data->positions[(i - 1u) * 3u],
              sizeof(float64_t) * 3u);
      data->yaws[i] = data->yaws[i - 1u];
      data->pitches[i] = data->pitches[i - 1u];
      data->fovs[i] = data->fovs[i - 1u];
      data->far_clips[i] = data->far_clips[i - 1u];
    }
    data->times[i] = i == 0u ? 0.0 : data->times[i - 1u] + data->display_dt[i];
  }
  for (uint32_t i = 0u; i < data->marker_count; ++i) {
    VkrRecordingDataMarker *marker = &data->markers[i];
    marker->time = data->times[Min(marker->frame, data->frame_count - 1u)];
  }
  /* The renderer's events arrive with their publication, a frame or two
     after events of later frames. */
  vkr_sort(data->events, data->event_count, sizeof(*data->events),
           vkr_recording_compare_events);
  for (uint64_t i = 0u; data->pass_block && i < data->frame_count; ++i) {
    if (data->pass_row_counts[i] == 0u) {
      continue;
    }
    ++data->pass_frames;
    data->pass_gpu_frames +=
        (data->pass_frame_flags[i] & VKR_RECORDING_PASSES_GPU_TIMED) ? 1u : 0u;
  }

  data->frame_time_series = UINT32_MAX;
  vkr_recording_data_finish_scene(data);
  if (data->frame_time_series != UINT32_MAX) {
    return;
  }
  const uint32_t wall = vkr_recording_data_find_series(data, "frame.wall");
  if (wall == UINT32_MAX) {
    return;
  }
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (data->series[wall].valid[i]) {
      data->frame_time_series = wall;
      return;
    }
  }
}

/* Maps the bundle's GPU counter file and checks its layout; without the
   file, or with one that does not parse (data->gpu_counters_bad), the
   recording loads without counters. */
vkr_internal void vkr_recording_data_open_counters(VkrRecordingDataLoad *load,
                                                   const char *bundle) {
  char path[VKR_RECORDING_DATA_PATH_MAX];
  const int32_t length = string_format(path, sizeof(path), "%s/%s", bundle,
                                       VKR_RECORDING_GPU_COUNTERS_FILE);
  if (length < 0 || (uint64_t)length >= sizeof(path)) {
    return;
  }
  const FilePath file = file_path_view(path);
  if (!file_exists(&file)) {
    return;
  }
  load->data->gpu_counters_bad = true_v;
  if (file_map_readonly(&file, &load->counter_file) != FILE_ERROR_NONE) {
    MemZero(&load->counter_file, sizeof(load->counter_file));
    return;
  }
  const uint8_t *bytes = load->counter_file.data;
  const uint64_t size = load->counter_file.size;
  if (size < VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE ||
      MemCompare(bytes, VKR_RECORDING_GPU_COUNTERS_MAGIC, 8u) != 0 ||
      vkr_load_le_u32(bytes + 8u) != VKR_RECORDING_GPU_COUNTERS_VERSION) {
    return;
  }
  const uint32_t counters = vkr_load_le_u32(bytes + 12u);
  const uint32_t buckets = vkr_load_le_u32(bytes + 16u);
  const uint32_t bucket_ns = vkr_load_le_u32(bytes + 20u);
  if (counters == 0u || counters > VKR_RECORDING_GPU_COUNTER_MAX ||
      buckets == 0u || buckets > VKR_RECORDING_GPU_COUNTER_BUCKET_MAX ||
      bucket_ns == 0u) {
    return;
  }
  uint64_t at = VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE;
  for (uint32_t i = 0u; i < counters; ++i) {
    if (at + 2u > size || bytes[at] > VKR_RECORDING_GPU_COUNTER_COUNT ||
        bytes[at + 1u] == 0u ||
        bytes[at + 1u] > VKR_RECORDING_GPU_COUNTER_NAME_MAX) {
      return;
    }
    at += 2u + bytes[at + 1u];
  }
  at = vkr_align_up_u64(at, 4u);
  if (at + (uint64_t)counters * buckets * sizeof(float32_t) != size) {
    return;
  }
  load->counter_names = bytes + VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE;
  load->counter_values = bytes + at;
  load->counter_count = counters;
  load->counter_buckets = buckets;
  load->counter_bucket_ns = bucket_ns;
  load->counter_first_ns = vkr_load_le_u64(bytes + 24u);
  load->data->gpu_counter_bucket_ns = bucket_ns;
  load->data->gpu_counter_anchors = vkr_load_le_u32(bytes + 32u);
  load->data->gpu_counter_spread_ns = vkr_load_le_u32(bytes + 36u);
  load->data->gpu_counters_bad = false_v;
}

/* The counter block (counters, then the frame and pass means, then each
   pass's frames) and the load's scratch for GPU spans and pass starts.
   Nothing without counters. Runs after the pass block is allocated. */
vkr_internal bool8_t
vkr_recording_data_allocate_counters(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  /* Counters the runtime sampled stand in for an imported file's; a
     recording has one or the other. Names left unwritten pack empty. */
  if (load->counter_count == 0u && load->sampled_count > 0u &&
      load->session_bytes > 0u) {
    uint8_t *at = load->sampled_packed;
    for (uint32_t i = 0u; i < load->sampled_count; ++i) {
      const uint8_t *named = load->sampled_names[i];
      at[0] = named[1];
      at[1] = named[2];
      MemCopy(at + 2u, named + 3u, named[2]);
      at += 2u + named[2];
    }
    load->counter_names = load->sampled_packed;
    load->counter_count = load->sampled_count;
    load->sessions = vkr_allocator_alloc(data->allocator, load->session_bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!load->sessions) {
      return false_v;
    }
  }
  const uint32_t counters = load->counter_count;
  if (counters == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  const uint64_t passes = data->pass_count;
  const uint64_t counter_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingGpuCounter) * counters, 8u);
  const uint64_t frame_values = frames * counters;
  const uint64_t pass_values = passes * counters;
  const uint64_t block_bytes =
      counter_bytes + sizeof(float32_t) * (frame_values + pass_values) +
      sizeof(uint32_t) * passes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->gpu_counter_block = block;
  data->gpu_counter_block_bytes = block_bytes;
  data->gpu_counters = (VkrRecordingGpuCounter *)block;
  data->gpu_counter_count = counters;
  data->gpu_counter_frames = (float32_t *)(block + counter_bytes);
  data->gpu_counter_passes = data->gpu_counter_frames + frame_values;
  data->gpu_counter_pass_frames =
      (uint32_t *)(data->gpu_counter_passes + pass_values);
  for (uint64_t i = 0u; i < frame_values + pass_values; ++i) {
    data->gpu_counter_frames[i] = (float32_t)VKR_FLOAT_NAN;
  }
  const uint8_t *at = load->counter_names;
  for (uint32_t i = 0u; i < counters; ++i) {
    VkrRecordingGpuCounter *counter = &data->gpu_counters[i];
    counter->unit = at[0];
    MemCopy(counter->name, at + 2u, at[1]);
    counter->name[at[1]] = '\0';
    counter->mean = (float32_t)VKR_FLOAT_NAN;
    counter->p95 = (float32_t)VKR_FLOAT_NAN;
    counter->max = (float32_t)VKR_FLOAT_NAN;
    at += 2u + at[1];
  }

  const uint64_t span_bytes = sizeof(uint64_t) * 2u * frames;
  const uint64_t scratch_bytes =
      span_bytes + sizeof(float32_t) * load->pass_rows;
  uint8_t *scratch = vkr_allocator_alloc(data->allocator, scratch_bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    return false_v;
  }
  MemZero(scratch, span_bytes);
  load->counter_scratch = scratch;
  load->counter_scratch_bytes = scratch_bytes;
  load->gpu_spans = (uint64_t *)scratch;
  load->pass_row_starts = (float32_t *)(scratch + span_bytes);
  for (uint64_t i = 0u; i < load->pass_rows; ++i) {
    load->pass_row_starts[i] = (float32_t)VKR_FLOAT_NAN;
  }
  return true_v;
}

/* The buckets an interval on the CPU clock covers, first and last; false
   when any of it lies outside the trace. */
vkr_internal bool8_t vkr_recording_counter_buckets(
    const VkrRecordingDataLoad *load, uint64_t start_ns, uint64_t end_ns,
    uint32_t *out_first, uint32_t *out_last) {
  const uint64_t trace_end =
      load->counter_first_ns +
      (uint64_t)load->counter_buckets * load->counter_bucket_ns;
  if (start_ns < load->counter_first_ns || end_ns > trace_end ||
      end_ns < start_ns) {
    return false_v;
  }
  *out_first =
      (uint32_t)((start_ns - load->counter_first_ns) / load->counter_bucket_ns);
  *out_last =
      (uint32_t)Min((end_ns - load->counter_first_ns) / load->counter_bucket_ns,
                    (uint64_t)load->counter_buckets - 1u);
  return true_v;
}

/* Adds counter `counter`'s bucket means from `first` to `last` to `sum`
   and returns how many there were. */
vkr_internal uint32_t
vkr_recording_counter_sum(const VkrRecordingDataLoad *load, uint32_t counter,
                          uint32_t first, uint32_t last, float64_t *sum) {
  const uint8_t *values = load->counter_values + (uint64_t)counter *
                                                     load->counter_buckets *
                                                     sizeof(float32_t);
  uint32_t samples = 0u;
  for (uint32_t b = first; b <= last; ++b) {
    const float32_t value = vkr_load_le_f32(values + (uint64_t)b * 4u);
    if (vkr_is_finite_f32(value)) {
      *sum += value;
      ++samples;
    }
  }
  return samples;
}

vkr_internal int32_t vkr_recording_compare_f32(const void *lhs,
                                               const void *rhs) {
  const float32_t a = *(const float32_t *)lhs;
  const float32_t b = *(const float32_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Folds the sampled counter sessions into the counter block: each pass's
   value sums into pass_sums and pass_samples, and each frame a session
   covered takes, per counter, its passes' mean weighted by their GPU time
   in the session's frames (a percent) or their sum (a count). Counters a
   session did not sample stay NaN on its frames. */
vkr_internal bool8_t vkr_recording_data_fold_sessions(
    VkrRecordingDataLoad *load, float64_t *pass_sums, uint32_t *pass_samples) {
  VkrRecordingData *data = load->data;
  const uint32_t counters = data->gpu_counter_count;
  const uint64_t frames = data->frame_count;
  const uint32_t passes = data->pass_count;
  /* Each pass's GPU time and frames in the session being folded. */
  const uint64_t weight_bytes = sizeof(float64_t) * passes;
  const uint64_t ran_bytes = sizeof(uint32_t) * passes;
  uint8_t *scratch =
      vkr_allocator_alloc(data->allocator, weight_bytes + ran_bytes,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    return false_v;
  }
  float64_t *weights = (float64_t *)scratch;
  uint32_t *ran = (uint32_t *)(scratch + weight_bytes);
  for (uint64_t at = 0u; at < load->session_used;) {
    uint64_t size = 0u;
    MemCopy(&size, load->sessions + at, sizeof(size));
    const uint8_t *record = load->sessions + at + 8u;
    at += 8u + vkr_align_up_u64(size, 8u);
    uint64_t first = 0u;
    uint64_t last = 0u;
    MemCopy(&first, record, sizeof(first));
    MemCopy(&last, record + 8u, sizeof(last));
    const uint32_t count = record[16];
    const uint32_t rows = vkr_load_le_u16(record + 18u);
    const uint64_t header = VKR_RECORDING_GPU_COUNTERS_SESSION_HEADER_SIZE +
                            vkr_align_up_u64(count, 4u);
    const uint64_t row_size = 4u + sizeof(float32_t) * (uint64_t)count;
    if (count == 0u || first > last || last >= frames ||
        header + rows * row_size > size) {
      continue;
    }
    const uint8_t *ids =
        record + VKR_RECORDING_GPU_COUNTERS_SESSION_HEADER_SIZE;

    MemZero(weights, weight_bytes);
    MemZero(ran, ran_bytes);
    for (uint64_t f = first; data->pass_block && f <= last; ++f) {
      for (uint32_t r = 0u; r < data->pass_row_counts[f]; ++r) {
        const VkrRecordingPassRow *row =
            &data->pass_rows[data->pass_first[f] + r];
        if (row->pass < passes) {
          ++ran[row->pass];
          if (row->flags & VKR_RECORDING_PASS_GPU_VALID) {
            weights[row->pass] += row->gpu_ms;
          }
        }
      }
    }

    for (uint32_t e = 0u; e < count; ++e) {
      const uint32_t c = ids[e];
      if (c >= counters) {
        continue;
      }
      const bool8_t percent =
          data->gpu_counters[c].unit == VKR_RECORDING_GPU_COUNTER_PERCENT;
      float64_t weighted = 0.0;
      float64_t weight = 0.0;
      float64_t sum = 0.0;
      uint32_t samples = 0u;
      for (uint32_t r = 0u; r < rows; ++r) {
        const uint8_t *row = record + header + r * row_size;
        const uint32_t pass = vkr_load_le_u16(row);
        float32_t value = 0.0f;
        MemCopy(&value, row + 4u + sizeof(float32_t) * e, sizeof(value));
        if (pass >= passes || !vkr_is_finite_f32(value)) {
          continue;
        }
        const uint64_t slot = (uint64_t)pass * counters + c;
        pass_sums[slot] += value;
        ++pass_samples[slot];
        weighted += (float64_t)value * weights[pass];
        weight += weights[pass];
        sum += value;
        ++samples;
      }
      if (samples == 0u) {
        continue;
      }
      /* Without GPU times the passes count alike. */
      const float64_t frame_value = !percent       ? sum
                                    : weight > 0.0 ? weighted / weight
                                                   : sum / (float64_t)samples;
      for (uint64_t f = first; f <= last; ++f) {
        data->gpu_counter_frames[(uint64_t)c * frames + f] =
            (float32_t)frame_value;
      }
    }
    for (uint32_t r = 0u; r < rows; ++r) {
      const uint32_t pass = vkr_load_le_u16(record + header + r * row_size);
      if (pass < passes) {
        data->gpu_counter_pass_frames[pass] += ran[pass];
      }
    }
    data->gpu_counter_frame_count += last - first + 1u;
    data->gpu_counter_first_frame = Min(data->gpu_counter_first_frame, first);
    data->gpu_counter_last_frame = Max(data->gpu_counter_last_frame, last);
  }
  vkr_allocator_free(data->allocator, scratch, weight_bytes + ran_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

/* Each counter's mean over each frame's GPU span the trace covered, and over
   each pass's GPU work in the GPU-timed frames among them, then each
   counter's statistics over the covered frames. */
vkr_internal bool8_t
vkr_recording_data_finish_counters(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (!data->gpu_counter_block) {
    return true_v;
  }
  const uint32_t counters = data->gpu_counter_count;
  const uint64_t frames = data->frame_count;
  const uint64_t pass_values = (uint64_t)data->pass_count * counters;
  const uint64_t sum_bytes = sizeof(float64_t) * pass_values;
  const uint64_t count_bytes = sizeof(uint32_t) * pass_values;
  const uint64_t sort_bytes = sizeof(float32_t) * frames;
  const uint64_t scratch_bytes = sum_bytes + count_bytes + sort_bytes;
  uint8_t *scratch = vkr_allocator_alloc(data->allocator, scratch_bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    return false_v;
  }
  MemZero(scratch, sum_bytes + count_bytes);
  float64_t *pass_sums = (float64_t *)scratch;
  uint32_t *pass_samples = (uint32_t *)(scratch + sum_bytes);
  float32_t *sorted = (float32_t *)(scratch + sum_bytes + count_bytes);

  data->gpu_counter_first_frame = UINT64_MAX;
  if (load->sessions &&
      !vkr_recording_data_fold_sessions(load, pass_sums, pass_samples)) {
    vkr_allocator_free(data->allocator, scratch, scratch_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  for (uint64_t f = 0u; !load->sessions && f < frames; ++f) {
    const uint64_t start_ns = load->gpu_spans[f * 2u];
    const uint64_t end_ns = load->gpu_spans[f * 2u + 1u];
    uint32_t first = 0u;
    uint32_t last = 0u;
    if (start_ns == 0u ||
        !vkr_recording_counter_buckets(load, start_ns, end_ns, &first, &last)) {
      continue;
    }
    bool8_t covered = false_v;
    for (uint32_t c = 0u; c < counters; ++c) {
      float64_t sum = 0.0;
      const uint32_t samples =
          vkr_recording_counter_sum(load, c, first, last, &sum);
      if (samples > 0u) {
        data->gpu_counter_frames[(uint64_t)c * frames + f] =
            (float32_t)(sum / (float64_t)samples);
        covered = true_v;
      }
    }
    if (!covered) {
      continue;
    }
    ++data->gpu_counter_frame_count;
    data->gpu_counter_first_frame = Min(data->gpu_counter_first_frame, f);
    data->gpu_counter_last_frame = f;

    /* A pass's GPU time comes from its timestamps, which a frame takes
       only when its rows are GPU-timed. */
    const uint32_t rows = data->pass_block ? data->pass_row_counts[f] : 0u;
    if (rows == 0u ||
        !(data->pass_frame_flags[f] & VKR_RECORDING_PASSES_GPU_TIMED)) {
      continue;
    }
    for (uint32_t r = 0u; r < rows; ++r) {
      const uint64_t index = data->pass_first[f] + r;
      const VkrRecordingPassRow *row = &data->pass_rows[index];
      const float32_t start_ms = load->pass_row_starts[index];
      if (!(row->flags & VKR_RECORDING_PASS_GPU_VALID) ||
          !vkr_is_finite_f32(start_ms) || start_ms < 0.0f) {
        continue;
      }
      const uint64_t pass_start =
          start_ns + (uint64_t)((float64_t)start_ms * 1.0e6);
      const uint64_t pass_end =
          pass_start + (uint64_t)((float64_t)row->gpu_ms * 1.0e6);
      if (!vkr_recording_counter_buckets(load, pass_start, pass_end, &first,
                                         &last)) {
        continue;
      }
      ++data->gpu_counter_pass_frames[row->pass];
      for (uint32_t c = 0u; c < counters; ++c) {
        const uint64_t slot = (uint64_t)row->pass * counters + c;
        pass_samples[slot] +=
            vkr_recording_counter_sum(load, c, first, last, &pass_sums[slot]);
      }
    }
  }
  if (data->gpu_counter_frame_count == 0u) {
    data->gpu_counter_first_frame = 0u;
  }
  for (uint64_t i = 0u; i < pass_values; ++i) {
    if (pass_samples[i] > 0u) {
      data->gpu_counter_passes[i] =
          (float32_t)(pass_sums[i] / (float64_t)pass_samples[i]);
    }
  }

  for (uint32_t c = 0u; c < counters; ++c) {
    const float32_t *values = data->gpu_counter_frames + (uint64_t)c * frames;
    uint64_t count = 0u;
    float64_t sum = 0.0;
    for (uint64_t f = 0u; f < frames; ++f) {
      if (vkr_is_finite_f32(values[f])) {
        sorted[count++] = values[f];
        sum += values[f];
      }
    }
    if (count == 0u) {
      continue;
    }
    vkr_sort(sorted, count, sizeof(*sorted), vkr_recording_compare_f32);
    VkrRecordingGpuCounter *counter = &data->gpu_counters[c];
    counter->mean = (float32_t)(sum / (float64_t)count);
    counter->p95 = sorted[Min(count - 1u, (count * 95u) / 100u)];
    counter->max = sorted[count - 1u];
  }
  vkr_allocator_free(data->allocator, scratch, scratch_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

VkrRecordingDataError vkr_recording_data_load(VkrRecordingData *out,
                                              VkrAllocator *allocator,
                                              const char *path,
                                              const VkrCodec *codec) {
  vkr_assert(out != NULL && allocator != NULL && path != NULL);
  MemZero(out, sizeof(*out));
  out->allocator = allocator;
  out->frame_time_series = UINT32_MAX;
  VkrRecordingDataLoad load = {
      .data = out, .codec = codec, .raw_frame = UINT64_MAX};
  bool8_t bundle = false_v;
  VkrRecordingDataError error =
      vkr_recording_data_open(&load.reader, allocator, path, &bundle);
  if (error != VKR_RECORDING_DATA_OK) {
    return error;
  }
  out->truncated = load.reader.truncated;

  error = vkr_recording_data_dictionary(&load);
  if (error != VKR_RECORDING_DATA_OK) {
    goto cleanup;
  }
  vkr_recording_data_identity(&load);
  out->bad_chunks = vkr_recording_data_records(&load, vkr_recording_data_count);
  if (load.too_large) {
    error = VKR_RECORDING_DATA_TOO_LARGE;
    goto cleanup;
  }
  if (load.frame_count == 0u) {
    error = VKR_RECORDING_DATA_EMPTY;
    goto cleanup;
  }
  if (!vkr_recording_data_allocate(&load) ||
      !vkr_recording_data_allocate_passes(&load) ||
      !vkr_recording_data_allocate_graphs(&load) ||
      !vkr_recording_data_allocate_allocators(&load) ||
      !vkr_recording_data_allocate_zones(&load) ||
      !vkr_recording_data_allocate_draws(&load) ||
      !vkr_recording_data_allocate_hardware(&load)) {
    error = VKR_RECORDING_DATA_MEMORY;
    goto cleanup;
  }
  if (bundle) {
    vkr_recording_data_open_counters(&load, path);
  }
  if (!vkr_recording_data_allocate_counters(&load)) {
    error = VKR_RECORDING_DATA_MEMORY;
    goto cleanup;
  }
  (void)vkr_recording_data_records(&load, vkr_recording_data_fill);
  vkr_recording_data_finish(out);
  vkr_recording_data_finish_graphs(out);
  vkr_recording_data_finish_zones(&load);
  if (!vkr_recording_data_finish_counters(&load)) {
    error = VKR_RECORDING_DATA_MEMORY;
    goto cleanup;
  }

cleanup:
  if (load.sessions) {
    vkr_allocator_free(allocator, load.sessions, load.session_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (load.counter_scratch) {
    vkr_allocator_free(allocator, load.counter_scratch,
                       load.counter_scratch_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (load.counter_file.data) {
    file_unmap(&load.counter_file);
  }
  if (load.zone_calls) {
    vkr_allocator_free(allocator, load.zone_calls,
                       sizeof(VkrRecordingZoneCall) * load.zone_records,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (load.has_dictionary) {
    vkr_recording_dictionary_free(allocator, &load.dictionary);
  }
  vkr_recording_reader_close(&load.reader);
  if (error != VKR_RECORDING_DATA_OK) {
    vkr_recording_data_free(out);
  }
  return error;
}

void vkr_recording_data_free(VkrRecordingData *data) {
  vkr_assert(data != NULL);
  if (data->block) {
    vkr_allocator_free(data->allocator, data->block, data->block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->pass_block) {
    vkr_allocator_free(data->allocator, data->pass_block,
                       data->pass_block_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->graph_block) {
    vkr_allocator_free(data->allocator, data->graph_block,
                       data->graph_block_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->allocator_block) {
    vkr_allocator_free(data->allocator, data->allocator_block,
                       data->allocator_block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->zone_block) {
    vkr_allocator_free(data->allocator, data->zone_block,
                       data->zone_block_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->draw_block) {
    vkr_allocator_free(data->allocator, data->draw_block,
                       data->draw_block_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->hardware_block) {
    vkr_allocator_free(data->allocator, data->hardware_block,
                       data->hardware_block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->gpu_counter_block) {
    vkr_allocator_free(data->allocator, data->gpu_counter_block,
                       data->gpu_counter_block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  MemZero(data, sizeof(*data));
  data->frame_time_series = UINT32_MAX;
}

uint32_t vkr_recording_data_find_series(const VkrRecordingData *data,
                                        const char *name) {
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    if (string_equals(data->series[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool8_t vkr_recording_data_frame_ms(const VkrRecordingData *data,
                                    uint64_t frame, float64_t *out_ms) {
  if (frame >= data->frame_count) {
    return false_v;
  }
  if (data->frame_time_series != UINT32_MAX) {
    const VkrRecordingSeries *series = &data->series[data->frame_time_series];
    if (!series->valid[frame]) {
      return false_v;
    }
    *out_ms = series->values[frame];
    return true_v;
  }
  if (!data->frame_valid[frame]) {
    return false_v;
  }
  *out_ms = data->display_dt[frame] * 1000.0;
  return true_v;
}

const char *vkr_recording_data_frame_source(const VkrRecordingData *data) {
  return data->frame_time_series != UINT32_MAX
             ? data->series[data->frame_time_series].name
             : "display_dt";
}

const VkrRecordingPassRow *
vkr_recording_data_pass_rows(const VkrRecordingData *data, uint64_t frame,
                             uint32_t *out_count) {
  *out_count = 0u;
  if (!data->pass_block || frame >= data->frame_count ||
      data->pass_row_counts[frame] == 0u) {
    return NULL;
  }
  *out_count = data->pass_row_counts[frame];
  return &data->pass_rows[data->pass_first[frame]];
}

uint32_t vkr_recording_data_find_pass(const VkrRecordingData *data,
                                      const char *name) {
  for (uint32_t i = 0u; i < data->pass_count; ++i) {
    if (data->passes[i].name[0] != '\0' &&
        string_equals(data->passes[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

vkr_internal bool8_t vkr_recording_gpu_timed(const VkrRecordingData *data,
                                             uint64_t frame) {
  return data->pass_block && frame < data->frame_count &&
         data->pass_row_counts[frame] > 0u &&
         (data->pass_frame_flags[frame] & VKR_RECORDING_PASSES_GPU_TIMED);
}

uint64_t vkr_recording_data_gpu_frame_near(const VkrRecordingData *data,
                                           uint64_t frame, uint64_t reach) {
  if (!data->pass_block || frame >= data->frame_count) {
    return UINT64_MAX;
  }
  for (uint64_t distance = 0u; distance <= reach; ++distance) {
    if (distance <= frame && vkr_recording_gpu_timed(data, frame - distance)) {
      return frame - distance;
    }
    if (vkr_recording_gpu_timed(data, frame + distance)) {
      return frame + distance;
    }
    if (distance > frame && frame + distance >= data->frame_count) {
      break;
    }
  }
  return UINT64_MAX;
}

uint64_t vkr_recording_data_draw_frame_near(const VkrRecordingData *data,
                                            uint64_t frame) {
  if (!data->draw_block || frame >= data->frame_count) {
    return UINT64_MAX;
  }
  const uint64_t reach = 8u + 4u * (uint64_t)data->draw_stride;
  for (uint64_t f = frame, n = 0u; n <= reach; --f, ++n) {
    if (data->draw_id_table[f] != UINT32_MAX) {
      return f;
    }
    if (f == 0u) {
      break;
    }
  }
  return UINT64_MAX;
}

bool8_t vkr_recording_data_draw_frame(const VkrRecordingData *data,
                                      uint64_t frame,
                                      VkrRecordingDrawFrame *out) {
  if (!data->draw_block || frame >= data->frame_count ||
      data->draw_id_table[frame] == UINT32_MAX) {
    return false_v;
  }
  *out = (VkrRecordingDrawFrame){
      .words = data->draw_words + data->draw_first[frame],
      .count = data->draw_candidates[frame],
      .ids = &data->draw_ids[data->draw_id_table[frame]],
      .flags = data->draw_flags[frame],
      .overflow = data->draw_overflow[frame],
  };
  return true_v;
}

uint32_t vkr_recording_data_draw_bounds_at(const VkrRecordingData *data,
                                           uint64_t frame) {
  if (data->draw_bounds_count == 0u) {
    return UINT32_MAX;
  }
  uint32_t at = 0u;
  for (uint32_t i = 1u; i < data->draw_bounds_count; ++i) {
    if (data->draw_bounds[i].frame > frame) {
      break;
    }
    at = i;
  }
  return at;
}

bool8_t vkr_recording_data_gpu_counter(const VkrRecordingData *data,
                                       uint32_t counter, uint64_t frame,
                                       float32_t *out_value) {
  if (!data->gpu_counter_block || counter >= data->gpu_counter_count ||
      frame >= data->frame_count) {
    return false_v;
  }
  const float32_t value =
      data->gpu_counter_frames[(uint64_t)counter * data->frame_count + frame];
  *out_value = value;
  return vkr_is_finite_f32(value);
}

bool8_t vkr_recording_data_gpu_counter_pass(const VkrRecordingData *data,
                                            uint32_t pass, uint32_t counter,
                                            float32_t *out_value) {
  if (!data->gpu_counter_block || counter >= data->gpu_counter_count ||
      pass >= data->pass_count) {
    return false_v;
  }
  const float32_t value =
      data->gpu_counter_passes[(uint64_t)pass * data->gpu_counter_count +
                               counter];
  *out_value = value;
  return vkr_is_finite_f32(value);
}

const VkrRecordingZoneRow *
vkr_recording_data_zone_rows(const VkrRecordingData *data, uint64_t frame,
                             uint32_t *out_count) {
  *out_count = 0u;
  if (!data->zone_block || frame >= data->frame_count ||
      data->zone_row_counts[frame] == 0u) {
    return NULL;
  }
  *out_count = data->zone_row_counts[frame];
  return &data->zone_rows[data->zone_first[frame]];
}

const VkrRecordingAllocatorRow *
vkr_recording_data_allocator_rows(const VkrRecordingData *data, uint64_t frame,
                                  uint32_t *out_count) {
  *out_count = 0u;
  if (!data->allocator_block || frame >= data->frame_count ||
      data->allocator_row_counts[frame] == 0u) {
    return NULL;
  }
  *out_count = data->allocator_row_counts[frame];
  return &data->allocator_rows[data->allocator_first[frame]];
}

uint32_t vkr_recording_data_graph_at(const VkrRecordingData *data,
                                     uint64_t frame) {
  if (data->graph_count == 0u) {
    return UINT32_MAX;
  }
  uint32_t at = 0u;
  for (uint32_t i = 1u; i < data->graph_count; ++i) {
    if (data->graphs[i].frame > frame) {
      break;
    }
    at = i;
  }
  return at;
}

String8 vkr_recording_graph_text(const VkrRecordingGraph *graph,
                                 VkrRecordingGraphText text) {
  if (!graph->names || text.length == 0u ||
      text.offset + text.length > graph->name_bytes) {
    return (String8){0};
  }
  return (String8){.str = (uint8_t *)(graph->names + text.offset),
                   .length = text.length};
}

/* ------------------------------------------------------------------------ */
/* Analysis                                                                  */

typedef struct VkrRecordingSpikeRun {
  uint64_t first;
  uint64_t last;
  uint64_t worst;
  float64_t excess;
} VkrRecordingSpikeRun;

vkr_internal int32_t vkr_recording_compare_f64(const void *lhs,
                                               const void *rhs) {
  const float64_t a = *(const float64_t *)lhs;
  const float64_t b = *(const float64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Largest excess first; the lower frame breaks ties. */
vkr_internal int32_t vkr_recording_compare_runs(const void *lhs,
                                                const void *rhs) {
  const VkrRecordingSpikeRun *a = lhs;
  const VkrRecordingSpikeRun *b = rhs;
  if (a->excess != b->excess) {
    return a->excess > b->excess ? -1 : 1;
  }
  return a->worst < b->worst ? -1 : (a->worst > b->worst ? 1 : 0);
}

/* The ceil(per_mille * count / 1000)-th smallest sample. */
vkr_internal float64_t vkr_recording_rank(const float64_t *sorted,
                                          uint64_t count, uint64_t per_mille) {
  uint64_t rank = (per_mille * count + 999u) / 1000u;
  rank = Clamp(rank, 1u, count);
  return sorted[rank - 1u];
}

/* Sorts `samples` and fills `out` from them. */
vkr_internal void vkr_recording_stats_compute(float64_t *samples,
                                              uint64_t count,
                                              VkrRecordingStats *out) {
  MemZero(out, sizeof(*out));
  out->valid = count;
  if (count == 0u) {
    return;
  }
  vkr_sort(samples, count, sizeof(float64_t), vkr_recording_compare_f64);
  float64_t sum = 0.0;
  for (uint64_t i = 0u; i < count; ++i) {
    sum += samples[i];
  }
  out->min = samples[0];
  out->max = samples[count - 1u];
  out->mean = sum / (float64_t)count;
  out->p50 = vkr_recording_rank(samples, count, 500u);
  out->p95 = vkr_recording_rank(samples, count, 950u);
  out->p99 = vkr_recording_rank(samples, count, 990u);
}

/* First frame of the window of VKR_RECORDING_ANALYSIS_WINDOW frames
   centred on `centre`, shifted inward at the ends of the recording. */
vkr_internal uint64_t vkr_recording_window_first(uint64_t count,
                                                 uint64_t centre) {
  const uint64_t window = VKR_RECORDING_ANALYSIS_WINDOW;
  if (count <= window) {
    return 0u;
  }
  const uint64_t first = centre > window / 2u ? centre - window / 2u : 0u;
  return Min(first, count - window);
}

/* The nearest-rank median of the valid frame times in the window around
   `centre` (vkr_recording_window_first). */
vkr_internal bool8_t vkr_recording_window_median(const float64_t *frame_ms,
                                                 const uint8_t *valid,
                                                 uint64_t count,
                                                 uint64_t centre,
                                                 float64_t *out_median) {
  const uint64_t first = vkr_recording_window_first(count, centre);
  const uint64_t end = Min(first + VKR_RECORDING_ANALYSIS_WINDOW, count);
  float64_t samples[VKR_RECORDING_ANALYSIS_WINDOW];
  uint32_t sample_count = 0u;
  for (uint64_t i = first; i < end; ++i) {
    if (!valid[i]) {
      continue;
    }
    /* Insertion keeps the window sorted. */
    uint32_t at = sample_count;
    while (at > 0u && samples[at - 1u] > frame_ms[i]) {
      samples[at] = samples[at - 1u];
      --at;
    }
    samples[at] = frame_ms[i];
    ++sample_count;
  }
  if (sample_count == 0u) {
    return false_v;
  }
  *out_median = samples[(sample_count + 1u) / 2u - 1u];
  return true_v;
}

vkr_internal bool8_t vkr_recording_spike_candidate(float64_t frame_ms,
                                                   float64_t median,
                                                   float64_t budget_ms) {
  if (frame_ms > 2.0 * budget_ms) {
    return true_v;
  }
  return frame_ms > VKR_RECORDING_SPIKE_RATIO * median &&
         frame_ms - median >= VKR_RECORDING_SPIKE_MIN_EXCESS_MS;
}

/* Frame-time statistics, the time-based 1% low and budget misses. */
vkr_internal void vkr_recording_frame_stats(VkrRecordingAnalysis *analysis,
                                            const float64_t *frame_ms,
                                            const uint8_t *valid,
                                            uint64_t count,
                                            float64_t *samples) {
  uint64_t sample_count = 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    if (!valid[i]) {
      continue;
    }
    samples[sample_count++] = frame_ms[i];
    analysis->over_budget += frame_ms[i] > analysis->budget_ms ? 1u : 0u;
    analysis->over_double_budget +=
        frame_ms[i] > 2.0 * analysis->budget_ms ? 1u : 0u;
  }
  vkr_recording_stats_compute(samples, sample_count, &analysis->frame_time);
  if (sample_count == 0u) {
    return;
  }

  const float64_t total = analysis->frame_time.mean * (float64_t)sample_count;
  const float64_t target = total * 0.01;
  float64_t covered = 0.0;
  analysis->low_1pct_ms = samples[0];
  for (uint64_t i = sample_count; i > 0u; --i) {
    covered += samples[i - 1u];
    if (covered >= target) {
      analysis->low_1pct_ms = samples[i - 1u];
      break;
    }
  }
}

/* The "ms" series, other than the frame time, that grew most at `frame`. */
vkr_internal void
vkr_recording_spike_contributors(const VkrRecordingData *data,
                                 const VkrRecordingAnalysis *analysis,
                                 VkrRecordingSpike *spike) {
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    const VkrRecordingSeries *series = &data->series[i];
    /* cpu.frame_work is the frame time on the CPU, not one of its causes. */
    if (i == data->frame_time_series || !string_equals(series->unit, "ms") ||
        string_equals(series->name, "cpu.frame_work") ||
        !series->valid[spike->frame] || analysis->series[i].valid == 0u) {
      continue;
    }
    const float64_t value = series->values[spike->frame];
    const float64_t excess = value - analysis->series[i].p50;
    if (excess <= 0.0) {
      continue;
    }
    /* Insert in descending order; an equal excess stays behind the earlier
       series. */
    uint32_t at = spike->contributor_count;
    if (at == VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX) {
      if (excess <= spike->contributors[at - 1u].excess) {
        continue;
      }
      --at;
    } else {
      ++spike->contributor_count;
    }
    while (at > 0u && spike->contributors[at - 1u].excess < excess) {
      spike->contributors[at] = spike->contributors[at - 1u];
      --at;
    }
    spike->contributors[at] = (VkrRecordingContributor){
        .series = i, .value = value, .excess = excess};
  }
}

/* For a spike the frame loop's wait for the renderer leads: the frame in
   flight whose GPU submission grew most (VkrRecordingSpike.gpu_frame). */
vkr_internal void
vkr_recording_spike_gpu_wait(const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             VkrRecordingSpike *spike) {
  spike->gpu_frame = UINT64_MAX;
  if (spike->contributor_count == 0u ||
      !string_equals(data->series[spike->contributors[0].series].name,
                     "cpu.render_wait")) {
    return;
  }
  const uint32_t gpu = vkr_recording_data_find_series(data, "gpu.submission");
  if (gpu == UINT32_MAX || analysis->series[gpu].valid == 0u) {
    return;
  }
  const VkrRecordingSeries *series = &data->series[gpu];
  const float64_t floor = spike->contributors[0].excess * 0.25;
  const uint64_t first =
      spike->frame > VKR_RECORDING_ANALYSIS_GPU_LOOKBACK
          ? spike->frame - VKR_RECORDING_ANALYSIS_GPU_LOOKBACK
          : 0u;
  for (uint64_t f = first; f <= spike->frame; ++f) {
    if (!series->valid[f]) {
      continue;
    }
    const float64_t excess = series->values[f] - analysis->series[gpu].p50;
    if (excess >= floor &&
        (spike->gpu_frame == UINT64_MAX || excess > spike->gpu_excess)) {
      spike->gpu_frame = f;
      spike->gpu_ms = series->values[f];
      spike->gpu_excess = excess;
    }
  }
}

/* Largest GPU total first, then CPU total, then the lower id. A pass that
   runs in few frames ranks by what it cost over the recording, not by its
   cost when it ran. */
vkr_internal int32_t vkr_recording_compare_passes(const void *lhs,
                                                  const void *rhs) {
  const VkrRecordingPassStats *a = lhs;
  const VkrRecordingPassStats *b = rhs;
  const float64_t a_gpu = a->gpu.mean * (float64_t)a->gpu.valid;
  const float64_t b_gpu = b->gpu.mean * (float64_t)b->gpu.valid;
  if (a_gpu != b_gpu) {
    return a_gpu > b_gpu ? -1 : 1;
  }
  const float64_t a_cpu = a->cpu.mean * (float64_t)a->cpu.valid;
  const float64_t b_cpu = b->cpu.mean * (float64_t)b->cpu.valid;
  if (a_cpu != b_cpu) {
    return a_cpu > b_cpu ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

/* Per-pass CPU and GPU statistics. Each pass's samples go to its own run of
   `scratch` (two per row), are sorted there and reduced. */
vkr_internal void vkr_recording_pass_stats(const VkrRecordingData *data,
                                           VkrRecordingAnalysis *out,
                                           uint64_t *offsets,
                                           float64_t *scratch) {
  const uint32_t passes = data->pass_count;
  const uint64_t rows = data->pass_row_count;
  float64_t *cpu = scratch;
  float64_t *gpu = scratch + rows;
  uint64_t *cpu_count = offsets;
  uint64_t *gpu_count = offsets + passes;
  uint64_t *start = offsets + 2u * passes;
  MemZero(offsets, sizeof(uint64_t) * 3u * passes);
  for (uint64_t i = 0u; i < rows; ++i) {
    ++start[data->pass_rows[i].pass];
  }
  uint64_t sum = 0u;
  for (uint32_t p = 0u; p < passes; ++p) {
    const uint64_t count = start[p];
    start[p] = sum;
    sum += count;
  }
  for (uint64_t i = 0u; i < rows; ++i) {
    const VkrRecordingPassRow *row = &data->pass_rows[i];
    const uint64_t at = start[row->pass];
    cpu[at + cpu_count[row->pass]++] = (float64_t)row->cpu_ms;
    if (row->flags & VKR_RECORDING_PASS_GPU_VALID) {
      gpu[at + gpu_count[row->pass]++] = (float64_t)row->gpu_ms;
    }
  }
  out->pass_count = 0u;
  for (uint32_t p = 0u; p < passes; ++p) {
    if (cpu_count[p] == 0u) {
      continue;
    }
    VkrRecordingPassStats *stats = &out->passes[out->pass_count++];
    stats->pass = p;
    vkr_recording_stats_compute(cpu + start[p], cpu_count[p], &stats->cpu);
    vkr_recording_stats_compute(gpu + start[p], gpu_count[p], &stats->gpu);
  }
  vkr_sort(out->passes, out->pass_count, sizeof(VkrRecordingPassStats),
           vkr_recording_compare_passes);
}

/* The frame whose pass rows explain a spike (VkrRecordingSpike.pass_frame). */
vkr_internal uint64_t vkr_recording_spike_pass_frame(
    const VkrRecordingData *data, const float64_t *frame_ms,
    const uint8_t *valid, const VkrRecordingSpike *spike, bool8_t *out_gpu) {
  *out_gpu = true_v;
  if (vkr_recording_gpu_timed(data, spike->frame)) {
    return spike->frame;
  }
  uint64_t best = UINT64_MAX;
  for (uint64_t f = spike->first_frame; f <= spike->last_frame; ++f) {
    if (valid[f] && vkr_recording_gpu_timed(data, f) &&
        (best == UINT64_MAX || frame_ms[f] > frame_ms[best])) {
      best = f;
    }
  }
  if (best != UINT64_MAX) {
    return best;
  }
  *out_gpu = false_v;
  uint32_t count = 0u;
  (void)vkr_recording_data_pass_rows(data, spike->frame, &count);
  return count > 0u ? spike->frame : UINT64_MAX;
}

/* The statistics of pass id `pass`: through `rank`, which maps a pass id to
   its index in analysis->passes, when given, else by search. */
vkr_internal const VkrRecordingPassStats *
vkr_recording_pass_stats_of(const VkrRecordingAnalysis *analysis,
                            const uint32_t *rank, uint32_t pass) {
  if (rank) {
    return &analysis->passes[rank[pass]];
  }
  for (uint32_t i = 0u; i < analysis->pass_count; ++i) {
    if (analysis->passes[i].pass == pass) {
      return &analysis->passes[i];
    }
  }
  return NULL;
}

/* The passes of `pass_frame` that grew most over their medians, GPU or CPU
   times as `gpu` says, at most `capacity`, largest first; returns how
   many. `rank` is as vkr_recording_pass_stats_of takes it. */
vkr_internal uint32_t vkr_recording_frame_passes(
    const VkrRecordingData *data, const VkrRecordingAnalysis *analysis,
    const uint32_t *rank, uint64_t pass_frame, bool8_t gpu,
    VkrRecordingPassContributor *out, uint32_t capacity) {
  uint32_t kept = 0u;
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, pass_frame, &count);
  for (uint32_t i = 0u; i < count && capacity > 0u; ++i) {
    const VkrRecordingPassRow *row = &rows[i];
    if (gpu && !(row->flags & VKR_RECORDING_PASS_GPU_VALID)) {
      continue;
    }
    const VkrRecordingPassStats *stats =
        vkr_recording_pass_stats_of(analysis, rank, row->pass);
    if (!stats) {
      continue;
    }
    const float64_t value =
        gpu ? (float64_t)row->gpu_ms : (float64_t)row->cpu_ms;
    const float64_t excess = value - (gpu ? stats->gpu.p50 : stats->cpu.p50);
    if (excess <= 0.0) {
      continue;
    }
    /* Insert in descending order; an equal excess stays behind the earlier
       row. */
    uint32_t at = kept;
    if (at == capacity) {
      if (excess <= out[at - 1u].excess) {
        continue;
      }
      --at;
    } else {
      ++kept;
    }
    while (at > 0u && out[at - 1u].excess < excess) {
      out[at] = out[at - 1u];
      --at;
    }
    out[at] = (VkrRecordingPassContributor){
        .pass = row->pass, .value = value, .excess = excess};
  }
  return kept;
}

/* The passes of the spike's pass frame that grew most over their medians. */
vkr_internal void
vkr_recording_spike_passes(const VkrRecordingData *data,
                           const VkrRecordingAnalysis *analysis,
                           const uint32_t *rank, const float64_t *frame_ms,
                           const uint8_t *valid, VkrRecordingSpike *spike) {
  spike->pass_frame = vkr_recording_spike_pass_frame(data, frame_ms, valid,
                                                     spike, &spike->pass_gpu);
  spike->pass_contributor_count = vkr_recording_frame_passes(
      data, analysis, rank, spike->pass_frame, spike->pass_gpu, spike->passes,
      VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX);
}

/* Bits of the flag series slow frames are compared on: why a slot packed
   its static candidate rows again, which publications moved their
   generation, and whether the world waited on a publication. */
typedef struct VkrRecordingFlagName {
  const char *series;
  uint32_t bit;
  const char *name;
} VkrRecordingFlagName;

static const VkrRecordingFlagName VKR_RECORDING_FLAG_NAMES[] = {
    {"packet.static_repack_reasons", VKR_STATIC_REPACK_UNPACKED,
     "repack.unpacked"},
    {"packet.static_repack_reasons", VKR_STATIC_REPACK_STATIC_CONTENT,
     "repack.static_content"},
    {"packet.static_repack_reasons", VKR_STATIC_REPACK_PUBLICATION,
     "repack.publication"},
    {"packet.static_repack_reasons", VKR_STATIC_REPACK_RESOURCE,
     "repack.resource"},
    {"packet.static_repack_reasons", VKR_STATIC_REPACK_CULL, "repack.cull"},
    {"packet.publication_changes", VKR_PUBLICATION_CHANGE_TEXTURE_READINESS,
     "publication.texture_readiness"},
    {"packet.publication_changes", VKR_PUBLICATION_CHANGE_GEOMETRY,
     "publication.geometry"},
    {"packet.publication_changes", VKR_PUBLICATION_CHANGE_GEOMETRY_RETIRE,
     "publication.geometry_retire"},
    {"packet.publication_changes", VKR_PUBLICATION_CHANGE_MATERIAL,
     "publication.material"},
    {"packet.publication_changes", VKR_PUBLICATION_CHANGE_SAMPLER,
     "publication.sampler"},
    {"packet.publication_pending", 1u, "publication.pending"},
};

const char *vkr_recording_analysis_flag_name(uint32_t flag) {
  return flag < ArrayCount(VKR_RECORDING_FLAG_NAMES)
             ? VKR_RECORDING_FLAG_NAMES[flag].name
             : "";
}

/* Frame classes of the slow-frame comparison. */
#define VKR_RECORDING_FRAME_OTHER 0u
#define VKR_RECORDING_FRAME_NORMAL 1u
#define VKR_RECORDING_FRAME_SLOW 2u

/* Sums and counts per class (normal, slow) and lag (0, 1). */
typedef struct VkrRecordingSlowSums {
  float64_t sum[2][2];
  uint64_t count[2][2];
} VkrRecordingSlowSums;

/* The lag whose slow mean exceeds its normal mean most; false_v when
   neither does or a class has no sample. */
vkr_internal bool8_t vkr_recording_slow_pick(const VkrRecordingSlowSums *sums,
                                             VkrRecordingSlowDelta *out) {
  bool8_t found = false_v;
  float64_t best = 0.0;
  for (uint32_t lag = 0u; lag < 2u; ++lag) {
    if (sums->count[0][lag] == 0u || sums->count[1][lag] == 0u) {
      continue;
    }
    const float64_t normal = sums->sum[0][lag] / (float64_t)sums->count[0][lag];
    const float64_t slow = sums->sum[1][lag] / (float64_t)sums->count[1][lag];
    if (slow - normal > best) {
      best = slow - normal;
      out->lag = lag;
      out->normal_ms = normal;
      out->slow_ms = slow;
      found = true_v;
    }
  }
  return found;
}

/* Inserts `delta` into `list`, largest difference first, keeping at most
   VKR_RECORDING_ANALYSIS_SLOW_MAX. */
vkr_internal void vkr_recording_slow_keep(VkrRecordingSlowDelta *list,
                                          uint32_t *count,
                                          VkrRecordingSlowDelta delta) {
  const float64_t difference = delta.slow_ms - delta.normal_ms;
  uint32_t at = *count;
  while (at > 0u &&
         list[at - 1u].slow_ms - list[at - 1u].normal_ms < difference) {
    --at;
  }
  if (at >= VKR_RECORDING_ANALYSIS_SLOW_MAX) {
    return;
  }
  const uint32_t kept = Min(*count, VKR_RECORDING_ANALYSIS_SLOW_MAX - 1u);
  for (uint32_t i = kept; i > at; --i) {
    list[i] = list[i - 1u];
  }
  list[at] = delta;
  *count = kept + 1u;
}

/* Whether an "ms" series can explain a slow frame: not the frame time
   itself, nor the interval it is measured against. */
vkr_internal bool8_t vkr_recording_slow_series(const VkrRecordingData *data,
                                               uint32_t index) {
  const VkrRecordingSeries *series = &data->series[index];
  return string_equals(series->unit, "ms") &&
         !string_equals(series->name, vkr_recording_data_frame_source(data)) &&
         !string_equals(series->name, "cpu.frame_work") &&
         !string_equals(series->name, VKR_RECORDING_PRESENT_INTERVAL_SERIES);
}

/* Adds row `row` of `series` to `sums` under class `slow` and `lag`; flag
   series add whether `bit` is set instead of the value. */
vkr_internal void vkr_recording_slow_add(const VkrRecordingSeries *series,
                                         uint64_t row, uint32_t slow,
                                         uint32_t lag, uint32_t bit,
                                         VkrRecordingSlowSums *sums) {
  if (!series->valid[row]) {
    return;
  }
  const float64_t value = series->values[row];
  sums->sum[slow][lag] += bit ? (((uint64_t)value & bit) ? 1.0 : 0.0) : value;
  ++sums->count[slow][lag];
}

/* `series` (or one bit of it) over the frames of each class, at both
   lags. */
vkr_internal void
vkr_recording_slow_series_sums(const VkrRecordingData *data,
                               const uint8_t *classes,
                               const VkrRecordingSeries *series, uint32_t bit,
                               VkrRecordingSlowSums *sums) {
  MemZero(sums, sizeof(*sums));
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (classes[i] == VKR_RECORDING_FRAME_OTHER) {
      continue;
    }
    const uint32_t slow = classes[i] == VKR_RECORDING_FRAME_SLOW ? 1u : 0u;
    vkr_recording_slow_add(series, i, slow, 0u, bit, sums);
    if (i > 0u) {
      vkr_recording_slow_add(series, i - 1u, slow, 1u, bit, sums);
    }
  }
}

/* Counts the slow frames and their runs by length. */
vkr_internal void vkr_recording_slow_runs(const uint8_t *classes,
                                          uint64_t count,
                                          VkrRecordingSlowFrames *out) {
  uint64_t run = 0u;
  for (uint64_t i = 0u; i <= count; ++i) {
    const uint8_t frame_class =
        i < count ? classes[i] : VKR_RECORDING_FRAME_OTHER;
    out->frames += frame_class == VKR_RECORDING_FRAME_SLOW ? 1u : 0u;
    out->normal_frames += frame_class == VKR_RECORDING_FRAME_NORMAL ? 1u : 0u;
    if (frame_class == VKR_RECORDING_FRAME_SLOW) {
      ++run;
      continue;
    }
    if (run == 0u) {
      continue;
    }
    ++out->runs;
    ++out->run_bins[Min(run, (uint64_t)VKR_RECORDING_ANALYSIS_RUN_BINS) - 1u];
    if (run > out->longest_run) {
      out->longest_run = run;
      out->longest_run_first = i - run;
    }
    run = 0u;
  }
}

/* GPU pass times over the frames of each class, at both lags; a frame
   without the pass adds zero. `pass_sums` holds one entry per pass. */
vkr_internal void vkr_recording_slow_passes(const VkrRecordingData *data,
                                            const uint8_t *classes,
                                            VkrRecordingSlowSums *pass_sums,
                                            VkrRecordingSlowFrames *out) {
  MemZero(pass_sums, sizeof(*pass_sums) * data->pass_count);
  uint64_t frames[2][2] = {{0u}};
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (classes[i] == VKR_RECORDING_FRAME_OTHER) {
      continue;
    }
    const uint32_t slow = classes[i] == VKR_RECORDING_FRAME_SLOW ? 1u : 0u;
    for (uint32_t lag = 0u; lag < 2u && lag <= i; ++lag) {
      if (!vkr_recording_gpu_timed(data, i - lag)) {
        continue;
      }
      ++frames[slow][lag];
      uint32_t row_count = 0u;
      const VkrRecordingPassRow *rows =
          vkr_recording_data_pass_rows(data, i - lag, &row_count);
      for (uint32_t r = 0u; r < row_count; ++r) {
        if (rows[r].pass < data->pass_count &&
            (rows[r].flags & VKR_RECORDING_PASS_GPU_VALID)) {
          pass_sums[rows[r].pass].sum[slow][lag] += rows[r].gpu_ms;
        }
      }
    }
  }
  for (uint32_t pass = 0u; pass < data->pass_count; ++pass) {
    VkrRecordingSlowSums *sums = &pass_sums[pass];
    MemCopy(sums->count, frames, sizeof(frames));
    VkrRecordingSlowDelta delta = {.index = pass};
    if (vkr_recording_slow_pick(sums, &delta)) {
      vkr_recording_slow_keep(out->passes, &out->pass_count, delta);
    }
  }
}

/* Compares the slow frames, classed in `classes`, with the normal ones:
   runs of slow frames, then series, pass GPU times and flag bits. */
vkr_internal void vkr_recording_slow_frames(const VkrRecordingData *data,
                                            const uint8_t *classes,
                                            VkrRecordingSlowSums *pass_sums,
                                            VkrRecordingSlowFrames *out) {
  vkr_recording_slow_runs(classes, data->frame_count, out);
  if (out->frames == 0u || out->normal_frames == 0u) {
    return;
  }

  VkrRecordingSlowSums sums;
  for (uint32_t s = 0u; s < data->series_count; ++s) {
    if (!vkr_recording_slow_series(data, s)) {
      continue;
    }
    vkr_recording_slow_series_sums(data, classes, &data->series[s], 0u, &sums);
    VkrRecordingSlowDelta delta = {.index = s};
    if (vkr_recording_slow_pick(&sums, &delta)) {
      vkr_recording_slow_keep(out->series, &out->series_count, delta);
    }
  }

  if (data->pass_block && data->pass_count > 0u) {
    vkr_recording_slow_passes(data, classes, pass_sums, out);
  }

  /* Flags rank by their share difference, as deltas do; a delta's index
     names the flag, and its means are the shares. */
  VkrRecordingSlowDelta shares[VKR_RECORDING_ANALYSIS_SLOW_MAX];
  uint32_t share_count = 0u;
  for (uint32_t f = 0u; f < ArrayCount(VKR_RECORDING_FLAG_NAMES); ++f) {
    const VkrRecordingFlagName *name = &VKR_RECORDING_FLAG_NAMES[f];
    const uint32_t s = vkr_recording_data_find_series(data, name->series);
    if (s == UINT32_MAX) {
      continue;
    }
    vkr_recording_slow_series_sums(data, classes, &data->series[s], name->bit,
                                   &sums);
    VkrRecordingSlowDelta share = {.index = f};
    if (vkr_recording_slow_pick(&sums, &share)) {
      vkr_recording_slow_keep(shares, &share_count, share);
    }
  }
  for (uint32_t i = 0u; i < share_count; ++i) {
    out->flags[i] = (VkrRecordingSlowFlag){
        .series = vkr_recording_data_find_series(
            data, VKR_RECORDING_FLAG_NAMES[shares[i].index].series),
        .flag = shares[i].index,
        .lag = shares[i].lag,
        .normal_share = shares[i].normal_ms,
        .slow_share = shares[i].slow_ms,
    };
  }
  out->flag_count = share_count;
}

/* ------------------------------------------------------------------------ */
/* Causes                                                                    */

static const char *const VKR_RECORDING_CAUSE_KIND_NAMES[] = {
    [VKR_RECORDING_CAUSE_UNKNOWN] = "unknown",
    [VKR_RECORDING_CAUSE_CPU_STAGE] = "cpu_stage",
    [VKR_RECORDING_CAUSE_GPU_PASS] = "gpu_pass",
    [VKR_RECORDING_CAUSE_CPU_PASS] = "cpu_pass",
    [VKR_RECORDING_CAUSE_GPU_FRAME] = "gpu_frame",
    [VKR_RECORDING_CAUSE_PRESENT] = "present",
    [VKR_RECORDING_CAUSE_METRIC] = "metric",
    [VKR_RECORDING_CAUSE_PIPELINE_CREATION] = "pipeline_creation",
    [VKR_RECORDING_CAUSE_SHADER_LOAD] = "shader_load",
    [VKR_RECORDING_CAUSE_ASSET_LOAD] = "asset_load",
    [VKR_RECORDING_CAUSE_SCRIPT_RELOAD] = "script_reload",
    [VKR_RECORDING_CAUSE_THERMAL] = "thermal",
    [VKR_RECORDING_CAUSE_MEMORY_PRESSURE] = "memory_pressure",
    [VKR_RECORDING_CAUSE_PAGE_FAULTS] = "page_faults",
    [VKR_RECORDING_CAUSE_ERRORS] = "errors",
};
_Static_assert(ArrayCount(VKR_RECORDING_CAUSE_KIND_NAMES) ==
                   VKR_RECORDING_CAUSE_KIND_COUNT,
               "a name per cause kind");

const char *vkr_recording_cause_kind_name(VkrRecordingCauseKind kind) {
  return (uint32_t)kind < VKR_RECORDING_CAUSE_KIND_COUNT
             ? VKR_RECORDING_CAUSE_KIND_NAMES[kind]
             : "unknown";
}

/* Engine events whose time adds to a frame, and what they cause. Late
   pipelines skip draws and carry no time; cell unloads free memory. */
typedef struct VkrRecordingEventCause {
  const char *event;
  VkrRecordingCauseKind kind;
} VkrRecordingEventCause;

static const VkrRecordingEventCause VKR_RECORDING_EVENT_CAUSES[] = {
    {VKR_RECORDING_EVENT_PIPELINE, VKR_RECORDING_CAUSE_PIPELINE_CREATION},
    {VKR_RECORDING_EVENT_SHADER, VKR_RECORDING_CAUSE_SHADER_LOAD},
    {VKR_RECORDING_EVENT_TEXTURE, VKR_RECORDING_CAUSE_ASSET_LOAD},
    {VKR_RECORDING_EVENT_MESH, VKR_RECORDING_CAUSE_ASSET_LOAD},
    {VKR_RECORDING_EVENT_MATERIAL, VKR_RECORDING_CAUSE_ASSET_LOAD},
    {VKR_RECORDING_EVENT_SCENE, VKR_RECORDING_CAUSE_ASSET_LOAD},
    {VKR_RECORDING_EVENT_CELL_LOAD, VKR_RECORDING_CAUSE_ASSET_LOAD},
    {VKR_RECORDING_EVENT_SCRIPT_RELOAD, VKR_RECORDING_CAUSE_SCRIPT_RELOAD},
};

/* A condition that slows frames without a time of its own: a series whose
   sample reaches `threshold`. */
typedef struct VkrRecordingCondition {
  const char *series;
  VkrRecordingCauseKind kind;
  float64_t threshold;
  const char *text;
} VkrRecordingCondition;

/* In rank order (ADR-125): thermal level 2 is serious and 3 critical,
   memory pressure 2 critical; the counters count the frame's events. */
static const VkrRecordingCondition VKR_RECORDING_CONDITIONS[] = {
    {"os.thermal.level", VKR_RECORDING_CAUSE_THERMAL, 2.0,
     "thermal level (2 serious, 3 critical): the system throttles"},
    {"os.memory.pressure", VKR_RECORDING_CAUSE_MEMORY_PRESSURE, 2.0,
     "critical memory pressure"},
    {"os.page_faults.major", VKR_RECORDING_CAUSE_PAGE_FAULTS, 1.0,
     "major page faults, each a read from disk"},
    {"gpu.errors", VKR_RECORDING_CAUSE_ERRORS, 1.0, "GPU errors"},
    {"log.errors", VKR_RECORDING_CAUSE_ERRORS, 1.0, "errors logged"},
    {"gpu.present.missed", VKR_RECORDING_CAUSE_PRESENT, 1.0,
     "presents that never reached the display"},
};
_Static_assert(ArrayCount(VKR_RECORDING_CONDITIONS) <
                   VKR_RECORDING_EXPLAIN_CAUSE_MAX,
               "room for every condition and a measured cause");

/* The kind of cause an "ms" series names. */
vkr_internal VkrRecordingCauseKind
vkr_recording_series_cause(const char *name) {
  if (string_n_equals(name, "cpu.", 4u)) {
    return VKR_RECORDING_CAUSE_CPU_STAGE;
  }
  if (string_equals(name, "gpu.submission")) {
    return VKR_RECORDING_CAUSE_GPU_FRAME;
  }
  if (string_n_equals(name, "gpu.present.", 12u) ||
      string_equals(name, VKR_RECORDING_PRESENT_INTERVAL_SERIES)) {
    return VKR_RECORDING_CAUSE_PRESENT;
  }
  return VKR_RECORDING_CAUSE_METRIC;
}

/* The kind of cause an engine event's time is; UNKNOWN for none. */
vkr_internal VkrRecordingCauseKind vkr_recording_event_cause(const char *name) {
  for (uint32_t i = 0u; i < ArrayCount(VKR_RECORDING_EVENT_CAUSES); ++i) {
    if (string_equals(name, VKR_RECORDING_EVENT_CAUSES[i].event)) {
      return VKR_RECORDING_EVENT_CAUSES[i].kind;
    }
  }
  return VKR_RECORDING_CAUSE_UNKNOWN;
}

/* The engine events of `frame`, merged by name in first-seen order, at most
   VKR_RECORDING_EXPLAIN_EVENT_MAX names; returns how many. */
vkr_internal uint32_t
vkr_recording_frame_events(const VkrRecordingData *data, uint64_t frame,
                           VkrRecordingExplainEvent *out) {
  /* Events are sorted by frame: the first of `frame` by bisection. */
  uint32_t low = 0u;
  uint32_t high = data->event_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (data->events[middle].frame < frame) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  uint32_t count = 0u;
  for (uint32_t i = low;
       i < data->event_count && data->events[i].frame == frame; ++i) {
    const VkrRecordingDataEvent *event = &data->events[i];
    if (!event->engine) {
      continue;
    }
    uint32_t at = 0u;
    while (at < count && out[at].name != event->name) {
      ++at;
    }
    if (at == count) {
      if (count == VKR_RECORDING_EXPLAIN_EVENT_MAX) {
        continue;
      }
      out[count++] = (VkrRecordingExplainEvent){.name = event->name};
    }
    out[at].count += event->count;
    out[at].ms += (float64_t)event->ms;
  }
  return count;
}

/* Inserts `cause` into `list` by excess, largest first, an equal excess
   behind the earlier cause, keeping at most `capacity`. */
vkr_internal void vkr_recording_keep_cause(VkrRecordingCause *list,
                                           uint32_t *count, uint32_t capacity,
                                           const VkrRecordingCause *cause) {
  if (capacity == 0u) {
    return;
  }
  uint32_t at = *count;
  if (at == capacity) {
    if (cause->excess_ms <= list[at - 1u].excess_ms) {
      return;
    }
    --at;
  } else {
    ++*count;
  }
  while (at > 0u && list[at - 1u].excess_ms < cause->excess_ms) {
    list[at] = list[at - 1u];
    --at;
  }
  list[at] = *cause;
}

/* The series that grew in the spike's frame, its frame-loop wait named as
   the GPU frame it stalled on. */
vkr_internal void vkr_recording_series_causes(const VkrRecordingData *data,
                                              const VkrRecordingSpike *spike,
                                              VkrRecordingCause *out,
                                              uint32_t *count,
                                              uint32_t capacity) {
  for (uint32_t i = 0u; i < spike->contributor_count; ++i) {
    const VkrRecordingContributor *contributor = &spike->contributors[i];
    if (contributor->excess < VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS) {
      continue;
    }
    const char *name = data->series[contributor->series].name;
    VkrRecordingCause cause = {
        .kind = vkr_recording_series_cause(name),
        .frame = spike->frame,
        .measured = true_v,
        .excess_ms = contributor->excess,
        .value = contributor->value,
    };
    /* gpu_frame is set only when the first contributor is the wait. */
    if (i == 0u && spike->gpu_frame != UINT64_MAX) {
      cause.kind = VKR_RECORDING_CAUSE_GPU_FRAME;
      cause.frame = spike->gpu_frame;
      cause.value = spike->gpu_ms;
      string_format(cause.name, sizeof(cause.name), "%s", "gpu.submission");
      string_format(cause.evidence, sizeof(cause.evidence),
                    "%s waited %.3g ms over its median for frame %llu, "
                    "whose gpu.submission took %.3g ms (+%.3g)",
                    name, contributor->excess,
                    (unsigned long long)spike->gpu_frame, spike->gpu_ms,
                    spike->gpu_excess);
    } else {
      string_format(cause.name, sizeof(cause.name), "%s", name);
      string_format(cause.evidence, sizeof(cause.evidence),
                    "%s took %.3g ms against a median of %.3g ms", name,
                    contributor->value,
                    contributor->value - contributor->excess);
    }
    vkr_recording_keep_cause(out, count, capacity, &cause);
  }
}

/* The passes that grew in the spike's pass frame, and the timed engine
   events of its frame. */
vkr_internal void vkr_recording_pass_event_causes(
    const VkrRecordingData *data, const VkrRecordingSpike *spike,
    VkrRecordingCause *out, uint32_t *count, uint32_t capacity) {
  for (uint32_t i = 0u; i < spike->pass_contributor_count; ++i) {
    const VkrRecordingPassContributor *pass = &spike->passes[i];
    if (pass->excess < VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS) {
      continue;
    }
    const char *name = data->passes[pass->pass].name;
    VkrRecordingCause cause = {
        .kind = spike->pass_gpu ? VKR_RECORDING_CAUSE_GPU_PASS
                                : VKR_RECORDING_CAUSE_CPU_PASS,
        .frame = spike->pass_frame,
        .measured = true_v,
        .excess_ms = pass->excess,
        .value = pass->value,
    };
    string_format(cause.name, sizeof(cause.name), "%s", name);
    string_format(cause.evidence, sizeof(cause.evidence),
                  "pass %s took %.3g ms of %s time against a median of "
                  "%.3g ms in frame %llu",
                  name, pass->value, spike->pass_gpu ? "GPU" : "CPU",
                  pass->value - pass->excess,
                  (unsigned long long)spike->pass_frame);
    vkr_recording_keep_cause(out, count, capacity, &cause);
  }

  VkrRecordingExplainEvent events[VKR_RECORDING_EXPLAIN_EVENT_MAX];
  const uint32_t event_count =
      vkr_recording_frame_events(data, spike->frame, events);
  for (uint32_t i = 0u; i < event_count; ++i) {
    const char *name = data->event_names[events[i].name].name;
    const VkrRecordingCauseKind kind = vkr_recording_event_cause(name);
    if (kind == VKR_RECORDING_CAUSE_UNKNOWN ||
        events[i].ms < VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS) {
      continue;
    }
    /* A typical frame has no such event: all its time is excess. */
    VkrRecordingCause cause = {
        .kind = kind,
        .frame = spike->frame,
        .measured = true_v,
        .excess_ms = events[i].ms,
        .value = (float64_t)events[i].count,
    };
    string_format(cause.name, sizeof(cause.name), "%s", name);
    string_format(cause.evidence, sizeof(cause.evidence),
                  "%u %s event%s took %.3g ms in the frame", events[i].count,
                  name, events[i].count == 1u ? "" : "s", events[i].ms);
    vkr_recording_keep_cause(out, count, capacity, &cause);
  }
}

/* The conditions that hold in `frame`, in table order; returns how many. */
vkr_internal uint32_t vkr_recording_conditions(const VkrRecordingData *data,
                                               uint64_t frame,
                                               VkrRecordingCause *out) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < ArrayCount(VKR_RECORDING_CONDITIONS); ++i) {
    const VkrRecordingCondition *condition = &VKR_RECORDING_CONDITIONS[i];
    const uint32_t s = vkr_recording_data_find_series(data, condition->series);
    if (s == UINT32_MAX || !data->series[s].valid[frame] ||
        data->series[s].values[frame] < condition->threshold) {
      continue;
    }
    VkrRecordingCause *cause = &out[count++];
    *cause = (VkrRecordingCause){
        .kind = condition->kind,
        .frame = frame,
        .value = data->series[s].values[frame],
    };
    string_format(cause->name, sizeof(cause->name), "%s", condition->series);
    string_format(cause->evidence, sizeof(cause->evidence), "%s: %g",
                  condition->text, cause->value);
  }
  return count;
}

/* The ranked causes of the spike's frame (the rule above
   vkr_recording_explain); returns how many, at least one. `spike` holds
   the frame's time, window median, contributors, GPU wait and passes; a
   frame without a time or median holds zero for it. */
vkr_internal uint32_t vkr_recording_rank_causes(
    const VkrRecordingData *data, const VkrRecordingAnalysis *analysis,
    const VkrRecordingSpike *spike, VkrRecordingCause *out) {
  VkrRecordingCause conditions[ArrayCount(VKR_RECORDING_CONDITIONS)];
  const uint32_t condition_count =
      vkr_recording_conditions(data, spike->frame, conditions);
  const uint32_t capacity = VKR_RECORDING_EXPLAIN_CAUSE_MAX - condition_count;
  /* The typical frame is the lower median, so a frame on a slow plateau,
     at its window median, is still measured against the recording. */
  const float64_t typical_ms = Min(spike->median_ms, analysis->frame_time.p50);
  const bool8_t slower = spike->frame_ms > typical_ms;
  uint32_t count = 0u;
  if (slower) {
    vkr_recording_series_causes(data, spike, out, &count, capacity);
    vkr_recording_pass_event_causes(data, spike, out, &count, capacity);
  }
  MemCopy(out + count, conditions, sizeof(*conditions) * condition_count);
  count += condition_count;
  if (count > 0u) {
    return count;
  }
  out[0] = (VkrRecordingCause){
      .kind = VKR_RECORDING_CAUSE_UNKNOWN,
      .frame = spike->frame,
  };
  if (slower) {
    string_format(out[0].evidence, sizeof(out[0].evidence),
                  "no series, pass or engine event grew %.3g ms over its "
                  "median and no condition holds",
                  VKR_RECORDING_EXPLAIN_MIN_EXCESS_MS);
  } else if (spike->frame_ms > 0.0) {
    string_format(out[0].evidence, sizeof(out[0].evidence),
                  "the frame took %.3g ms, not above its typical %.3g ms, "
                  "and no condition holds",
                  spike->frame_ms, typical_ms);
  } else {
    string_format(out[0].evidence, sizeof(out[0].evidence), "%s",
                  "the frame has no frame time and no condition holds");
  }
  return 1u;
}

bool8_t vkr_recording_analyze(const VkrRecordingData *data,
                              const VkrRecordingAnalysisOptions *options,
                              VkrAllocator *allocator,
                              VkrRecordingAnalysis *out) {
  vkr_assert(data != NULL && allocator != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  out->allocator = allocator;
  out->budget_ms = VKR_RECORDING_ANALYSIS_BUDGET_MS;
  out->budget_source = "default";
  const uint32_t spike_max = options && options->spike_max > 0u
                                 ? options->spike_max
                                 : VKR_RECORDING_ANALYSIS_SPIKE_MAX;
  const uint64_t count = data->frame_count;

  /* Owned results: series statistics, the kept spikes, pass statistics. */
  const uint64_t stats_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingStats) * (uint64_t)data->series_count, 8u);
  const uint64_t spike_bytes = sizeof(VkrRecordingSpike) * (uint64_t)spike_max;
  const uint64_t block_bytes =
      stats_bytes + spike_bytes +
      sizeof(VkrRecordingPassStats) * (uint64_t)data->pass_count;
  uint8_t *block = vkr_allocator_alloc(allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  out->block = block;
  out->block_bytes = block_bytes;
  out->series = (VkrRecordingStats *)block;
  out->spikes = (VkrRecordingSpike *)(block + stats_bytes);
  out->passes = (VkrRecordingPassStats *)(block + stats_bytes + spike_bytes);

  /* Scratch: frame times, sorted samples, window medians, spike runs, pass
     samples (two per row), pass counts and ranks, the slow-frame pass sums,
     the frame-time flags and the frame classes. Adjacent candidates merge,
     so runs are at most half the frames, rounded up. */
  const uint64_t run_capacity = (count + 1u) / 2u;
  const uint64_t pass_sample_count = 2u * data->pass_row_count;
  const uint64_t pass_offset_count = 3u * (uint64_t)data->pass_count;
  const uint64_t scratch_bytes =
      sizeof(float64_t) * (count * 3u + pass_sample_count) +
      sizeof(VkrRecordingSpikeRun) * run_capacity +
      sizeof(uint64_t) * pass_offset_count +
      vkr_align_up_u64(sizeof(uint32_t) * (uint64_t)data->pass_count, 8u) +
      sizeof(VkrRecordingSlowSums) * (uint64_t)data->pass_count + count * 2u;
  uint8_t *scratch = vkr_allocator_alloc(allocator, scratch_bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    vkr_recording_analysis_free(out);
    return false_v;
  }
  float64_t *frame_ms = (float64_t *)scratch;
  float64_t *samples = frame_ms + count;
  float64_t *medians = samples + count;
  float64_t *pass_samples = medians + count;
  VkrRecordingSpikeRun *runs =
      (VkrRecordingSpikeRun *)(pass_samples + pass_sample_count);
  uint64_t *pass_offsets = (uint64_t *)(runs + run_capacity);
  uint32_t *pass_rank = (uint32_t *)(pass_offsets + pass_offset_count);
  VkrRecordingSlowSums *pass_sums =
      (VkrRecordingSlowSums *)((uint8_t *)pass_rank +
                               vkr_align_up_u64(sizeof(uint32_t) *
                                                    (uint64_t)data->pass_count,
                                                8u));
  uint8_t *valid = (uint8_t *)(pass_sums + data->pass_count);
  uint8_t *classes = valid + count;

  /* The budget: the option, else the interval presentation paced the
     frames to, else the default. */
  const uint32_t interval = vkr_recording_data_find_series(
      data, VKR_RECORDING_PRESENT_INTERVAL_SERIES);
  if (options && options->budget_ms > 0.0) {
    out->budget_ms = options->budget_ms;
    out->budget_source = "option";
  } else if (interval != UINT32_MAX) {
    uint64_t interval_count = 0u;
    for (uint64_t i = 0u; i < count; ++i) {
      if (data->series[interval].valid[i] &&
          data->series[interval].values[i] > 0.0) {
        samples[interval_count++] = data->series[interval].values[i];
      }
    }
    VkrRecordingStats interval_stats;
    vkr_recording_stats_compute(samples, interval_count, &interval_stats);
    if (interval_stats.valid > 0u) {
      out->budget_ms = interval_stats.p50;
      out->budget_source = VKR_RECORDING_PRESENT_INTERVAL_SERIES;
    }
  }

  out->slow.threshold_ms = VKR_RECORDING_ANALYSIS_SLOW_FACTOR * out->budget_ms;
  for (uint64_t i = 0u; i < count; ++i) {
    valid[i] = vkr_recording_data_frame_ms(data, i, &frame_ms[i]) ? 1u : 0u;
    if (!valid[i]) {
      classes[i] = VKR_RECORDING_FRAME_OTHER;
    } else if (frame_ms[i] > out->slow.threshold_ms) {
      classes[i] = VKR_RECORDING_FRAME_SLOW;
    } else {
      classes[i] = VKR_RECORDING_FRAME_NORMAL;
    }
  }
  vkr_recording_frame_stats(out, frame_ms, valid, count, samples);
  for (uint32_t i = 0u; i < data->event_count; ++i) {
    const VkrRecordingDataEvent *event = &data->events[i];
    VkrRecordingEventTotal *total = &out->event_totals[event->name];
    ++total->records;
    total->count += event->count;
    total->ms += event->ms;
  }

  for (uint32_t s = 0u; s < data->series_count; ++s) {
    const VkrRecordingSeries *series = &data->series[s];
    uint64_t sample_count = 0u;
    for (uint64_t i = 0u; i < count; ++i) {
      if (series->valid[i]) {
        samples[sample_count++] = series->values[i];
      }
    }
    vkr_recording_stats_compute(samples, sample_count, &out->series[s]);
  }
  vkr_recording_pass_stats(data, out, pass_offsets, pass_samples);
  for (uint32_t i = 0u; i < data->pass_count; ++i) {
    pass_rank[i] = UINT32_MAX;
  }
  for (uint32_t i = 0u; i < out->pass_count; ++i) {
    pass_rank[out->passes[i].pass] = i;
  }

  /* Candidates, merged into runs that keep their worst frame. */
  uint64_t run_count = 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    if (!valid[i] ||
        !vkr_recording_window_median(frame_ms, valid, count, i, &medians[i]) ||
        !vkr_recording_spike_candidate(frame_ms[i], medians[i],
                                       out->budget_ms)) {
      continue;
    }
    VkrRecordingSpikeRun *run = run_count > 0u ? &runs[run_count - 1u] : NULL;
    if (run && run->last + 1u == i) {
      run->last = i;
      if (frame_ms[i] > frame_ms[run->worst]) {
        run->worst = i;
      }
      continue;
    }
    runs[run_count++] =
        (VkrRecordingSpikeRun){.first = i, .last = i, .worst = i};
  }
  for (uint64_t r = 0u; r < run_count; ++r) {
    runs[r].excess = frame_ms[runs[r].worst] - medians[runs[r].worst];
  }
  vkr_sort(runs, run_count, sizeof(VkrRecordingSpikeRun),
           vkr_recording_compare_runs);

  out->spikes_found = run_count;
  out->spike_count = (uint32_t)Min(run_count, (uint64_t)spike_max);
  for (uint32_t k = 0u; k < out->spike_count; ++k) {
    const VkrRecordingSpikeRun *run = &runs[k];
    VkrRecordingSpike *spike = &out->spikes[k];
    const float64_t median = medians[run->worst];
    *spike = (VkrRecordingSpike){
        .frame = run->worst,
        .first_frame = run->first,
        .last_frame = run->last,
        .time = data->times[run->worst],
        .frame_ms = frame_ms[run->worst],
        .median_ms = median,
        .ratio = median > 0.0 ? frame_ms[run->worst] / median : 0.0,
        .excess_ms = run->excess,
        .pass_frame = UINT64_MAX,
    };
    vkr_recording_spike_contributors(data, out, spike);
    vkr_recording_spike_gpu_wait(data, out, spike);
    vkr_recording_spike_passes(data, out, pass_rank, frame_ms, valid, spike);
    VkrRecordingCause causes[VKR_RECORDING_EXPLAIN_CAUSE_MAX];
    (void)vkr_recording_rank_causes(data, out, spike, causes);
    spike->cause = causes[0];
  }
  vkr_recording_slow_frames(data, classes, pass_sums, &out->slow);

  vkr_allocator_free(allocator, scratch, scratch_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

void vkr_recording_analysis_free(VkrRecordingAnalysis *analysis) {
  vkr_assert(analysis != NULL);
  if (analysis->block) {
    vkr_allocator_free(analysis->allocator, analysis->block,
                       analysis->block_bytes, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  MemZero(analysis, sizeof(*analysis));
}

bool8_t vkr_recording_segment_stats(const VkrRecordingData *data,
                                    const VkrRecordingAnalysis *analysis,
                                    uint64_t first, uint64_t last,
                                    VkrAllocator *scratch,
                                    VkrRecordingSegmentStats *out) {
  vkr_assert(data != NULL && analysis != NULL && scratch != NULL &&
             out != NULL);
  MemZero(out, sizeof(*out));
  if (data->frame_count == 0u || first >= data->frame_count) {
    return false_v;
  }
  out->first = Min(first, last);
  out->last = Min(Max(first, last), data->frame_count - 1u);
  out->start_seconds = data->times[out->first];
  out->end_seconds = data->times[out->last];
  const uint64_t count = out->last - out->first + 1u;
  const uint64_t bytes = sizeof(float64_t) * count;
  float64_t *samples =
      vkr_allocator_alloc(scratch, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!samples) {
    return false_v;
  }
  uint64_t sample_count = 0u;
  for (uint64_t frame = out->first; frame <= out->last; ++frame) {
    float64_t ms = 0.0;
    if (!vkr_recording_data_frame_ms(data, frame, &ms)) {
      continue;
    }
    samples[sample_count++] = ms;
    out->over_budget += ms > analysis->budget_ms ? 1u : 0u;
    out->over_double_budget += ms > 2.0 * analysis->budget_ms ? 1u : 0u;
    if (sample_count == 1u || ms > out->worst_ms) {
      out->worst_ms = ms;
      out->worst_frame = frame;
    }
  }
  vkr_recording_stats_compute(samples, sample_count, &out->frame_time);
  vkr_allocator_free(scratch, samples, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  for (uint32_t i = 0u; i < analysis->spike_count; ++i) {
    const uint64_t frame = analysis->spikes[i].frame;
    out->spikes += frame >= out->first && frame <= out->last ? 1u : 0u;
  }
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Explanation                                                               */

/* The CPU stages of the Tier 0 metrics an explanation lists. */
static const char *const VKR_RECORDING_EXPLAIN_STAGES[] = {
    "cpu.update",
    "cpu.simulation",
    "cpu.physics",
    "cpu.render_prepare",
    "cpu.render_submit",
    "cpu.render_wait",
    "cpu.shadow_update",
    "cpu.world_payload_build",
    "cpu.graph_build",
    "cpu.graph_compile",
    "cpu.packet_candidate_pack",
    "cpu.backend_frame_wait",
    "cpu.backend_present",
    "cpu.drawable_acquire",
    "cpu.process_time",
};
_Static_assert(ArrayCount(VKR_RECORDING_EXPLAIN_STAGES) <=
                   VKR_RECORDING_EXPLAIN_STAGE_MAX,
               "room for every stage");

/* Platform and health series (ADR-125) an explanation lists. */
#define VKR_RECORDING_FOOTPRINT_SERIES "os.memory.footprint_bytes"
static const char *const VKR_RECORDING_EXPLAIN_SYSTEM_SERIES[] = {
    "os.thermal.level",
    "os.memory.pressure",
    "os.power.low_power",
    "os.page_faults.major",
    VKR_RECORDING_FOOTPRINT_SERIES,
    "gpu.present.latency",
    "gpu.present.missed",
    "gpu.errors",
    "log.errors",
};
_Static_assert(ArrayCount(VKR_RECORDING_EXPLAIN_SYSTEM_SERIES) <=
                   VKR_RECORDING_EXPLAIN_SYSTEM_MAX,
               "room for every system series");
_Static_assert(2u * ArrayCount(VKR_RECORDING_FLAG_NAMES) <=
                   VKR_RECORDING_EXPLAIN_FLAG_MAX,
               "room for every flag at both lags");

/* The window median of `frame` (vkr_recording_window_median), read from
   the data. */
vkr_internal bool8_t vkr_recording_frame_median(const VkrRecordingData *data,
                                                uint64_t frame,
                                                float64_t *out_median) {
  const uint64_t first = vkr_recording_window_first(data->frame_count, frame);
  const uint64_t end =
      Min(first + VKR_RECORDING_ANALYSIS_WINDOW, data->frame_count);
  float64_t frame_ms[VKR_RECORDING_ANALYSIS_WINDOW] = {0};
  uint8_t valid[VKR_RECORDING_ANALYSIS_WINDOW] = {0};
  for (uint64_t i = first; i < end; ++i) {
    valid[i - first] =
        vkr_recording_data_frame_ms(data, i, &frame_ms[i - first]) ? 1u : 0u;
  }
  return vkr_recording_window_median(frame_ms, valid, end - first,
                                     frame - first, out_median);
}

/* The frame whose pass rows explain a frame outside a spike: itself or the
   nearest within reach with GPU times, else its own CPU times. */
vkr_internal uint64_t vkr_recording_explain_pass_frame(
    const VkrRecordingData *data, uint64_t frame, bool8_t *out_gpu) {
  *out_gpu = true_v;
  const uint64_t near = vkr_recording_data_gpu_frame_near(
      data, frame, VKR_RECORDING_EXPLAIN_PASS_REACH);
  if (near != UINT64_MAX) {
    return near;
  }
  *out_gpu = false_v;
  uint32_t count = 0u;
  (void)vkr_recording_data_pass_rows(data, frame, &count);
  return count > 0u ? frame : UINT64_MAX;
}

/* The stages valid in the frame, largest excess first. */
vkr_internal void
vkr_recording_explain_stages(const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             VkrRecordingExplanation *out) {
  for (uint32_t i = 0u; i < ArrayCount(VKR_RECORDING_EXPLAIN_STAGES); ++i) {
    const uint32_t s =
        vkr_recording_data_find_series(data, VKR_RECORDING_EXPLAIN_STAGES[i]);
    if (s == UINT32_MAX || !data->series[s].valid[out->frame]) {
      continue;
    }
    const float64_t value = data->series[s].values[out->frame];
    const float64_t median = analysis->series[s].p50;
    const VkrRecordingExplainValue stage = {
        .index = s, .value = value, .median = median, .excess = value - median};
    uint32_t at = out->stage_count++;
    while (at > 0u && out->stages[at - 1u].excess < stage.excess) {
      out->stages[at] = out->stages[at - 1u];
      --at;
    }
    out->stages[at] = stage;
  }
}

/* The system series the recording has, and the flags set in the frame or
   the row before it. */
vkr_internal void vkr_recording_explain_system(const VkrRecordingData *data,
                                               VkrRecordingExplanation *out) {
  const uint64_t frame = out->frame;
  for (uint32_t i = 0u; i < ArrayCount(VKR_RECORDING_EXPLAIN_SYSTEM_SERIES);
       ++i) {
    const char *name = VKR_RECORDING_EXPLAIN_SYSTEM_SERIES[i];
    const uint32_t s = vkr_recording_data_find_series(data, name);
    if (s == UINT32_MAX) {
      continue;
    }
    const VkrRecordingSeries *series = &data->series[s];
    VkrRecordingExplainSystem *entry = &out->system[out->system_count++];
    *entry = (VkrRecordingExplainSystem){
        .series = s,
        .valid = series->valid[frame],
        .value = series->values[frame],
    };
    if (entry->valid && frame > 0u && series->valid[frame - 1u] &&
        string_equals(name, VKR_RECORDING_FOOTPRINT_SERIES)) {
      entry->has_delta = true_v;
      entry->delta = series->values[frame] - series->values[frame - 1u];
    }
  }

  for (uint32_t f = 0u; f < ArrayCount(VKR_RECORDING_FLAG_NAMES); ++f) {
    const VkrRecordingFlagName *name = &VKR_RECORDING_FLAG_NAMES[f];
    const uint32_t s = vkr_recording_data_find_series(data, name->series);
    if (s == UINT32_MAX) {
      continue;
    }
    const VkrRecordingSeries *series = &data->series[s];
    for (uint32_t lag = 0u; lag < 2u && lag <= frame; ++lag) {
      const uint64_t row = frame - lag;
      if (series->valid[row] && ((uint64_t)series->values[row] & name->bit)) {
        out->flags[out->flag_count++] =
            (VkrRecordingExplainFlag){.flag = f, .lag = lag};
      }
    }
  }
}

bool8_t vkr_recording_explain(const VkrRecordingData *data,
                              const VkrRecordingAnalysis *analysis,
                              uint64_t frame, VkrRecordingExplanation *out) {
  vkr_assert(data != NULL && analysis != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  if (frame >= data->frame_count) {
    return false_v;
  }
  out->frame = frame;
  out->time = data->times[frame];
  out->budget_ms = analysis->budget_ms;
  out->frame_valid = vkr_recording_data_frame_ms(data, frame, &out->frame_ms);
  out->median_valid = vkr_recording_frame_median(data, frame, &out->median_ms);

  /* A kept spike's own findings, so its cause stays the first one here;
     any other frame takes the same rules alone. */
  VkrRecordingSpike spike = {
      .frame = frame,
      .first_frame = frame,
      .last_frame = frame,
      .frame_ms = out->frame_ms,
      .median_ms = out->median_ms,
  };
  for (uint32_t k = 0u; k < analysis->spike_count; ++k) {
    if (analysis->spikes[k].frame == frame) {
      spike = analysis->spikes[k];
      out->spike_rank = k + 1u;
      break;
    }
  }
  if (out->spike_rank == 0u) {
    vkr_recording_spike_contributors(data, analysis, &spike);
    vkr_recording_spike_gpu_wait(data, analysis, &spike);
    spike.pass_frame =
        vkr_recording_explain_pass_frame(data, frame, &spike.pass_gpu);
    /* Rows borrowed from another frame describe that frame: they fill the
       pass table but rank no cause. A spike borrows only within its run. */
    if (spike.pass_frame == frame) {
      spike.pass_contributor_count = vkr_recording_frame_passes(
          data, analysis, NULL, spike.pass_frame, spike.pass_gpu, spike.passes,
          VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX);
    }
  }
  out->gpu_wait_frame = spike.gpu_frame;
  out->gpu_wait_ms = spike.gpu_ms;
  out->gpu_wait_excess = spike.gpu_excess;

  /* The same pass rule as the spike's, keeping more passes. */
  out->pass_frame = spike.pass_frame;
  out->pass_gpu = spike.pass_gpu;
  VkrRecordingPassContributor passes[VKR_RECORDING_EXPLAIN_PASS_MAX];
  out->pass_count = vkr_recording_frame_passes(
      data, analysis, NULL, spike.pass_frame, spike.pass_gpu, passes,
      VKR_RECORDING_EXPLAIN_PASS_MAX);
  for (uint32_t i = 0u; i < out->pass_count; ++i) {
    out->passes[i] = (VkrRecordingExplainValue){
        .index = passes[i].pass,
        .value = passes[i].value,
        .median = passes[i].value - passes[i].excess,
        .excess = passes[i].excess,
    };
  }

  vkr_recording_explain_stages(data, analysis, out);
  const uint32_t gpu = vkr_recording_data_find_series(data, "gpu.submission");
  if (gpu != UINT32_MAX && data->series[gpu].valid[frame]) {
    out->gpu_valid = true_v;
    out->gpu_ms = data->series[gpu].values[frame];
    out->gpu_median = analysis->series[gpu].p50;
  }
  out->event_count = vkr_recording_frame_events(data, frame, out->events);
  vkr_recording_explain_system(data, out);
  out->cause_count =
      vkr_recording_rank_causes(data, analysis, &spike, out->causes);
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

vkr_internal String8 vkr_recording_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

vkr_internal void vkr_recording_json_key(VkrJsonWriter *json,
                                         const char *name) {
  vkr_json_writer_name(json, vkr_recording_str(name));
}

vkr_internal void vkr_recording_json_text(VkrJsonWriter *json,
                                          const char *text) {
  vkr_json_writer_string(json, vkr_recording_str(text));
}

/* Integers print as integers; other values keep a few significant digits.
   Non-finite values print as null. */
vkr_internal void vkr_recording_json_number(VkrJsonWriter *json,
                                            float64_t value) {
  if (!vkr_is_finite_f64(value)) {
    vkr_json_writer_null(json);
    return;
  }
  if (value == vkr_round_f64(value) && vkr_abs_f64(value) < 9.0e15) {
    vkr_json_writer_i64(json, (int64_t)value);
    return;
  }
  vkr_json_writer_f64_digits(json, value, VKR_RECORDING_JSON_DIGITS);
}

/* The number, or null when `present` is false. */
vkr_internal void vkr_recording_json_optional(VkrJsonWriter *json,
                                              bool8_t present,
                                              float64_t value) {
  if (present) {
    vkr_recording_json_number(json, value);
  } else {
    vkr_json_writer_null(json);
  }
}

vkr_internal void vkr_recording_json_named_optional(VkrJsonWriter *json,
                                                    const char *name,
                                                    bool8_t present,
                                                    float64_t value) {
  vkr_recording_json_key(json, name);
  vkr_recording_json_optional(json, present, value);
}

vkr_internal void vkr_recording_json_named_number(VkrJsonWriter *json,
                                                  const char *name,
                                                  float64_t value) {
  vkr_recording_json_key(json, name);
  vkr_recording_json_number(json, value);
}

vkr_internal void vkr_recording_json_named_u64(VkrJsonWriter *json,
                                               const char *name,
                                               uint64_t value) {
  vkr_recording_json_key(json, name);
  vkr_json_writer_u64(json, value);
}

vkr_internal bool8_t vkr_recording_is_bytes(const VkrRecordingSeries *series) {
  return string_equals(series->unit, "bytes");
}

vkr_internal void
vkr_recording_write_frame_time(VkrJsonWriter *json,
                               const VkrRecordingData *data,
                               const VkrRecordingAnalysis *analysis) {
  const VkrRecordingStats *stats = &analysis->frame_time;
  const bool8_t any = stats->valid > 0u;
  vkr_recording_json_key(json, "frame_time");
  vkr_json_writer_begin_object(json);
  vkr_recording_json_key(json, "source");
  vkr_recording_json_text(json, vkr_recording_data_frame_source(data));
  vkr_recording_json_key(json, "unit");
  vkr_recording_json_text(json, "ms");
  vkr_recording_json_named_u64(json, "valid", stats->valid);
  vkr_recording_json_named_optional(json, "min", any, stats->min);
  vkr_recording_json_named_optional(json, "max", any, stats->max);
  vkr_recording_json_named_optional(json, "mean", any, stats->mean);
  vkr_recording_json_named_optional(json, "p50", any, stats->p50);
  vkr_recording_json_named_optional(json, "p95", any, stats->p95);
  vkr_recording_json_named_optional(json, "p99", any, stats->p99);
  vkr_recording_json_named_optional(json, "low_1pct", any,
                                    analysis->low_1pct_ms);
  vkr_recording_json_named_number(json, "budget_ms", analysis->budget_ms);
  vkr_recording_json_key(json, "budget_source");
  vkr_recording_json_text(json, analysis->budget_source);
  vkr_recording_json_named_u64(json, "over_budget", analysis->over_budget);
  vkr_recording_json_named_u64(json, "over_2x_budget",
                               analysis->over_double_budget);
  vkr_json_writer_end_object(json);
}

/* [name, lag, normal, slow] per kept delta. */
vkr_internal void vkr_recording_write_slow_deltas(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingSlowDelta *deltas, uint32_t count, bool8_t passes) {
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingSlowDelta *delta = &deltas[i];
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, passes ? data->passes[delta->index].name
                                         : data->series[delta->index].name);
    vkr_json_writer_u64(json, delta->lag);
    vkr_recording_json_number(json, delta->normal_ms);
    vkr_recording_json_number(json, delta->slow_ms);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
}

/* The slow frames against the others: how many, their runs by length, and
   what differs most in them. */
vkr_internal void
vkr_recording_write_slow_frames(VkrJsonWriter *json,
                                const VkrRecordingData *data,
                                const VkrRecordingAnalysis *analysis) {
  const VkrRecordingSlowFrames *slow = &analysis->slow;
  vkr_recording_json_key(json, "slow_frames");
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_number(json, "threshold_ms", slow->threshold_ms);
  vkr_recording_json_named_u64(json, "frames", slow->frames);
  vkr_recording_json_named_u64(json, "normal_frames", slow->normal_frames);
  vkr_recording_json_named_u64(json, "runs", slow->runs);
  vkr_recording_json_key(json, "run_lengths");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < VKR_RECORDING_ANALYSIS_RUN_BINS; ++i) {
    vkr_json_writer_u64(json, slow->run_bins[i]);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_named_u64(json, "longest_run", slow->longest_run);
  vkr_recording_json_named_u64(json, "longest_run_first",
                               slow->longest_run_first);
  vkr_recording_json_key(json, "columns");
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, "name");
  vkr_recording_json_text(json, "lag");
  vkr_recording_json_text(json, "normal");
  vkr_recording_json_text(json, "slow");
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "series");
  vkr_recording_write_slow_deltas(json, data, slow->series, slow->series_count,
                                  false_v);
  vkr_recording_json_key(json, "passes");
  vkr_recording_write_slow_deltas(json, data, slow->passes, slow->pass_count,
                                  true_v);
  /* The same columns; normal and slow are the shares of frames with the
     flag set. */
  vkr_recording_json_key(json, "flags");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < slow->flag_count; ++i) {
    const VkrRecordingSlowFlag *flag = &slow->flags[i];
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, vkr_recording_analysis_flag_name(flag->flag));
    vkr_json_writer_u64(json, flag->lag);
    vkr_recording_json_number(json, flag->normal_share);
    vkr_recording_json_number(json, flag->slow_share);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
}

/* "name": [unit, valid, min, max, mean, p50, p95, p99] */
vkr_internal void vkr_recording_write_series_entry(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis, uint32_t index) {
  const VkrRecordingSeries *series = &data->series[index];
  const VkrRecordingStats *stats = &analysis->series[index];
  const bool8_t any = stats->valid > 0u;
  vkr_recording_json_key(json, series->name);
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, series->unit);
  vkr_json_writer_u64(json, stats->valid);
  vkr_recording_json_optional(json, any, stats->min);
  vkr_recording_json_optional(json, any, stats->max);
  vkr_recording_json_optional(json, any, stats->mean);
  vkr_recording_json_optional(json, any, stats->p50);
  vkr_recording_json_optional(json, any, stats->p95);
  vkr_recording_json_optional(json, any, stats->p99);
  vkr_json_writer_end_array(json);
}

vkr_internal void vkr_recording_write_spike_markers(
    VkrJsonWriter *json, const VkrRecordingData *data, uint64_t frame) {
  const uint64_t reach = VKR_RECORDING_ANALYSIS_MARKER_FRAMES;
  uint32_t written = 0u;
  vkr_recording_json_key(json, "markers");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->marker_count &&
                        written < VKR_RECORDING_ANALYSIS_SPIKE_MARKER_MAX;
       ++i) {
    const VkrRecordingDataMarker *marker = &data->markers[i];
    if (marker->frame + reach < frame || marker->frame > frame + reach) {
      continue;
    }
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_u64(json, "frame", marker->frame);
    vkr_recording_json_key(json, "kind");
    vkr_recording_json_text(json, vkr_recording_marker_kind_name(marker->kind));
    vkr_recording_json_key(json, "label");
    vkr_recording_json_text(json, marker->label);
    vkr_json_writer_end_object(json);
    ++written;
  }
  vkr_json_writer_end_array(json);
}

/* An event as [name, frame, count, ms], ms null when not measured. */
vkr_internal void
vkr_recording_write_event(VkrJsonWriter *json, const VkrRecordingData *data,
                          const VkrRecordingDataEvent *event) {
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, data->event_names[event->name].name);
  vkr_json_writer_u64(json, event->frame);
  vkr_json_writer_u64(json, event->count);
  vkr_recording_json_optional(json, event->engine, (float64_t)event->ms);
  vkr_json_writer_end_array(json);
}

/* Engine events within the marker reach of `frame`, then script events,
   each in frame order, at most VKR_RECORDING_ANALYSIS_SPIKE_EVENT_MAX. */
vkr_internal void vkr_recording_write_spike_events(VkrJsonWriter *json,
                                                   const VkrRecordingData *data,
                                                   uint64_t frame) {
  const uint64_t reach = VKR_RECORDING_ANALYSIS_MARKER_FRAMES;
  uint32_t written = 0u;
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    for (uint32_t i = 0u; i < data->event_count &&
                          written < VKR_RECORDING_ANALYSIS_SPIKE_EVENT_MAX;
         ++i) {
      const VkrRecordingDataEvent *event = &data->events[i];
      if (event->engine != (pass == 0u) || event->frame + reach < frame ||
          event->frame > frame + reach) {
        continue;
      }
      vkr_recording_write_event(json, data, event);
      ++written;
    }
  }
  vkr_json_writer_end_array(json);
}

/* {kind, name, frame, excess_ms or value[, evidence]}: excess_ms for a
   measured cause, value for a condition. */
vkr_internal void vkr_recording_write_cause(VkrJsonWriter *json,
                                            const VkrRecordingCause *cause,
                                            bool8_t evidence) {
  vkr_json_writer_begin_object(json);
  vkr_recording_json_key(json, "kind");
  vkr_recording_json_text(json, vkr_recording_cause_kind_name(cause->kind));
  vkr_recording_json_key(json, "name");
  vkr_recording_json_text(json, cause->name);
  vkr_recording_json_named_u64(json, "frame", cause->frame);
  if (cause->measured) {
    vkr_recording_json_named_number(json, "excess_ms", cause->excess_ms);
  } else if (cause->kind != VKR_RECORDING_CAUSE_UNKNOWN) {
    vkr_recording_json_named_number(json, "value", cause->value);
  }
  if (evidence) {
    vkr_recording_json_key(json, "evidence");
    vkr_recording_json_text(json, cause->evidence);
  }
  vkr_json_writer_end_object(json);
}

vkr_internal void
vkr_recording_write_spike(VkrJsonWriter *json, const VkrRecordingData *data,
                          const VkrRecordingAnalysis *analysis, uint32_t rank) {
  const VkrRecordingSpike *spike = &analysis->spikes[rank];
  const uint64_t frame = spike->frame;
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "rank", rank + 1u);
  vkr_recording_json_named_u64(json, "frame", frame);
  vkr_recording_json_named_u64(json, "frames",
                               spike->last_frame - spike->first_frame + 1u);
  vkr_recording_json_named_number(json, "time", spike->time);
  vkr_recording_json_named_number(json, "ms", spike->frame_ms);
  vkr_recording_json_named_number(json, "median", spike->median_ms);
  vkr_recording_json_named_number(json, "ratio", spike->ratio);
  /* The first cause rec.explain and vkr_rec explain rank for the frame;
     they also give its evidence. */
  vkr_recording_json_key(json, "cause");
  vkr_recording_write_cause(json, &spike->cause, false_v);

  vkr_recording_json_key(json, "position");
  if (data->camera_valid[frame]) {
    vkr_json_writer_begin_array(json);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      vkr_recording_json_number(json, data->positions[frame * 3u + axis]);
    }
    vkr_json_writer_end_array(json);
  } else {
    vkr_json_writer_null(json);
  }
  vkr_recording_json_named_optional(json, "yaw", data->camera_valid[frame],
                                    (float64_t)data->yaws[frame]);
  if (data->has_player) {
    vkr_recording_json_key(json, "player");
    if (data->player_valid[frame]) {
      vkr_json_writer_begin_array(json);
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        vkr_recording_json_number(json,
                                  data->player_positions[frame * 3u + axis]);
      }
      vkr_json_writer_end_array(json);
    } else {
      vkr_json_writer_null(json);
    }
  }

  vkr_recording_json_key(json, "contributors");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < spike->contributor_count; ++i) {
    const VkrRecordingContributor *contributor = &spike->contributors[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_json_key(json, "name");
    vkr_recording_json_text(json, data->series[contributor->series].name);
    vkr_recording_json_named_number(json, "ms", contributor->value);
    vkr_recording_json_named_number(json, "excess", contributor->excess);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
  /* The GPU work the frame loop waited on, a few frames back. */
  vkr_recording_json_key(json, "gpu_wait");
  if (spike->gpu_frame != UINT64_MAX) {
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_number(json, "frame", (float64_t)spike->gpu_frame);
    vkr_recording_json_named_number(json, "ms", spike->gpu_ms);
    vkr_recording_json_named_number(json, "excess", spike->gpu_excess);
    vkr_json_writer_end_object(json);
  } else {
    vkr_json_writer_null(json);
  }

  if (data->pass_block) {
    vkr_recording_json_named_optional(json, "pass_frame",
                                      spike->pass_frame != UINT64_MAX,
                                      (float64_t)spike->pass_frame);
    vkr_recording_json_key(json, "pass_time");
    vkr_recording_json_text(json, spike->pass_gpu ? "gpu" : "cpu");
    vkr_recording_json_key(json, "passes");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < spike->pass_contributor_count; ++i) {
      const VkrRecordingPassContributor *pass = &spike->passes[i];
      vkr_json_writer_begin_object(json);
      vkr_recording_json_key(json, "name");
      vkr_recording_json_text(json, data->passes[pass->pass].name);
      vkr_recording_json_named_number(json, "ms", pass->value);
      vkr_recording_json_named_number(json, "excess", pass->excess);
      vkr_json_writer_end_object(json);
    }
    vkr_json_writer_end_array(json);
  }

  vkr_recording_write_spike_markers(json, data, frame);
  vkr_recording_write_spike_events(json, data, frame);

  /* Aligned with the analysis "bytes_series" names. */
  vkr_recording_json_key(json, "bytes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    const VkrRecordingSeries *series = &data->series[i];
    if (!vkr_recording_is_bytes(series)) {
      continue;
    }
    vkr_recording_json_optional(json, series->valid[frame],
                                series->values[frame]);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
}

/* [name, frames, gpu_frames, gpu_mean, gpu_p50, gpu_p95, gpu_max,
   cpu_mean, cpu_p95] */
vkr_internal void vkr_recording_write_pass_entry(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis, uint32_t index) {
  const VkrRecordingPassStats *stats = &analysis->passes[index];
  const bool8_t gpu = stats->gpu.valid > 0u;
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, data->passes[stats->pass].name);
  vkr_json_writer_u64(json, stats->cpu.valid);
  vkr_json_writer_u64(json, stats->gpu.valid);
  vkr_recording_json_optional(json, gpu, stats->gpu.mean);
  vkr_recording_json_optional(json, gpu, stats->gpu.p50);
  vkr_recording_json_optional(json, gpu, stats->gpu.p95);
  vkr_recording_json_optional(json, gpu, stats->gpu.max);
  vkr_recording_json_number(json, stats->cpu.mean);
  vkr_recording_json_number(json, stats->cpu.p95);
  vkr_json_writer_end_array(json);
}

vkr_internal void
vkr_recording_write_passes(VkrJsonWriter *json, const VkrRecordingData *data,
                           const VkrRecordingAnalysis *analysis,
                           uint32_t pass_limit) {
  static const char *const pass_columns[] = {
      "name",    "frames",  "gpu_frames", "gpu_mean", "gpu_p50",
      "gpu_p95", "gpu_max", "cpu_mean",   "cpu_p95"};
  vkr_recording_json_named_u64(json, "pass_frames", data->pass_frames);
  vkr_recording_json_named_u64(json, "pass_gpu_frames", data->pass_gpu_frames);
  vkr_recording_json_named_u64(json, "pass_rows_cut", data->pass_rows_cut);
  vkr_recording_json_key(json, "pass_columns");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < ArrayCount(pass_columns); ++i) {
    vkr_recording_json_text(json, pass_columns[i]);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "passes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < pass_limit; ++i) {
    vkr_recording_write_pass_entry(json, data, analysis, i);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "passes_truncated");
  vkr_json_writer_bool(json, pass_limit < analysis->pass_count);
}

vkr_internal void
vkr_recording_write_analysis(VkrJsonWriter *json, const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             bool8_t include_series, uint32_t series_limit,
                             uint32_t spike_limit, uint32_t pass_limit) {
  static const char *const series_columns[] = {"unit", "valid", "min", "max",
                                               "mean", "p50",   "p95", "p99"};
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "frames", data->frame_count);
  vkr_recording_json_named_u64(json, "missing_frames", data->missing_frames);
  vkr_recording_write_frame_time(json, data, analysis);
  vkr_recording_write_slow_frames(json, data, analysis);

  if (include_series) {
    vkr_recording_json_key(json, "series_columns");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < ArrayCount(series_columns); ++i) {
      vkr_recording_json_text(json, series_columns[i]);
    }
    vkr_json_writer_end_array(json);
    vkr_recording_json_key(json, "series");
    vkr_json_writer_begin_object(json);
    for (uint32_t i = 0u; i < series_limit; ++i) {
      vkr_recording_write_series_entry(json, data, analysis, i);
    }
    vkr_json_writer_end_object(json);
    vkr_recording_json_key(json, "series_truncated");
    vkr_json_writer_bool(json, series_limit < data->series_count);
  }
  if (data->pass_block) {
    vkr_recording_write_passes(json, data, analysis, pass_limit);
  }

  vkr_recording_json_key(json, "bytes_series");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    if (vkr_recording_is_bytes(&data->series[i])) {
      vkr_recording_json_text(json, data->series[i].name);
    }
  }
  vkr_json_writer_end_array(json);

  /* [name, records, count, ms] per event name; ms null for script events. */
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->event_name_count; ++i) {
    const VkrRecordingEventTotal *total = &analysis->event_totals[i];
    const bool8_t engine = string_n_equals(
        data->event_names[i].name, VKR_RECORDING_ENGINE_EVENT_PREFIX,
        string_length(VKR_RECORDING_ENGINE_EVENT_PREFIX));
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, data->event_names[i].name);
    vkr_json_writer_u64(json, total->records);
    vkr_json_writer_u64(json, total->count);
    vkr_recording_json_optional(json, engine, total->ms);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_named_u64(json, "events_dropped", data->events_dropped);

  vkr_recording_json_named_u64(json, "spikes_found", analysis->spikes_found);
  vkr_recording_json_key(json, "spikes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < spike_limit; ++i) {
    vkr_recording_write_spike(json, data, analysis, i);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "spikes_truncated");
  vkr_json_writer_bool(json, spike_limit < analysis->spikes_found);
  vkr_json_writer_end_object(json);
}

vkr_internal bool8_t vkr_recording_count_sink(void *context,
                                              const uint8_t *data,
                                              uint64_t length) {
  (void)data;
  *(uint64_t *)context += length;
  return true_v;
}

typedef enum VkrRecordingMeasure {
  VKR_RECORDING_MEASURE_HEADER,
  VKR_RECORDING_MEASURE_SPIKE,
  VKR_RECORDING_MEASURE_SERIES,
  VKR_RECORDING_MEASURE_PASS,
} VkrRecordingMeasure;

/* Bytes one part of the analysis object takes when written. */
vkr_internal uint64_t vkr_recording_measure(
    const VkrRecordingData *data, const VkrRecordingAnalysis *analysis,
    bool8_t include_series, VkrRecordingMeasure part, uint32_t index) {
  uint64_t bytes = 0u;
  VkrJsonWriter counter;
  vkr_json_writer_init(&counter, vkr_recording_count_sink, &bytes);
  switch (part) {
  case VKR_RECORDING_MEASURE_HEADER:
    vkr_recording_write_analysis(&counter, data, analysis, include_series, 0u,
                                 0u, 0u);
    break;
  case VKR_RECORDING_MEASURE_SPIKE:
    vkr_recording_write_spike(&counter, data, analysis, index);
    break;
  case VKR_RECORDING_MEASURE_SERIES:
    /* A member needs an object around it; the braces are not counted. */
    vkr_json_writer_begin_object(&counter);
    vkr_recording_write_series_entry(&counter, data, analysis, index);
    vkr_json_writer_end_object(&counter);
    break;
  case VKR_RECORDING_MEASURE_PASS:
    /* An element needs an array around it; the brackets are not counted. */
    vkr_json_writer_begin_array(&counter);
    vkr_recording_write_pass_entry(&counter, data, analysis, index);
    vkr_json_writer_end_array(&counter);
    break;
  }
  vkr_json_writer_complete(&counter);
  const bool8_t wrapped = part == VKR_RECORDING_MEASURE_SERIES ||
                          part == VKR_RECORDING_MEASURE_PASS;
  return wrapped && bytes >= 2u ? bytes - 2u : bytes;
}

/* Takes parts `first` onward of one kind while they fit in `budget`, a
   comma before every part after a list's first; returns the new count. */
vkr_internal uint32_t vkr_recording_fit(const VkrRecordingData *data,
                                        const VkrRecordingAnalysis *analysis,
                                        bool8_t include_series,
                                        VkrRecordingMeasure part,
                                        uint32_t first, uint32_t last,
                                        uint64_t budget, uint64_t *used) {
  uint32_t count = first;
  for (uint32_t i = first; i < last; ++i) {
    const uint64_t bytes =
        vkr_recording_measure(data, analysis, include_series, part, i) +
        (i > 0u ? 1u : 0u);
    if (*used + bytes > budget) {
      break;
    }
    *used += bytes;
    ++count;
  }
  return count;
}

bool8_t vkr_recording_analysis_write_json(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis,
    const VkrRecordingAnalysisJsonOptions *options) {
  vkr_assert(json != NULL && data != NULL && analysis != NULL);
  const VkrRecordingAnalysisJsonOptions defaults = {
      .budget_bytes = VKR_RECORDING_ANALYSIS_JSON_BUDGET,
      .include_series = true_v,
  };
  const VkrRecordingAnalysisJsonOptions *chosen = options ? options : &defaults;
  const bool8_t include_series = chosen->include_series;
  uint32_t series_limit = include_series ? data->series_count : 0u;
  uint32_t spike_limit = analysis->spike_count;
  uint32_t pass_limit = analysis->pass_count;

  if (chosen->budget_bytes > 0u) {
    /* Series come first, in dictionary order, then the first passes, then
       spikes in rank order, then the other passes; each part goes in while
       the object stays within the budget. */
    uint64_t used = vkr_recording_measure(data, analysis, include_series,
                                          VKR_RECORDING_MEASURE_HEADER, 0u) +
                    VKR_RECORDING_JSON_FLAG_SLACK;
    const uint32_t series_count = series_limit;
    series_limit = vkr_recording_fit(data, analysis, include_series,
                                     VKR_RECORDING_MEASURE_SERIES, 0u,
                                     series_count, chosen->budget_bytes, &used);
    pass_limit = vkr_recording_fit(
        data, analysis, include_series, VKR_RECORDING_MEASURE_PASS, 0u,
        Min(analysis->pass_count, VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST),
        chosen->budget_bytes, &used);
    spike_limit = vkr_recording_fit(
        data, analysis, include_series, VKR_RECORDING_MEASURE_SPIKE, 0u,
        analysis->spike_count, chosen->budget_bytes, &used);
    /* The rest of the passes continue the first ones' list. */
    if (pass_limit ==
        Min(analysis->pass_count, VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST)) {
      pass_limit = vkr_recording_fit(
          data, analysis, include_series, VKR_RECORDING_MEASURE_PASS,
          pass_limit, analysis->pass_count, chosen->budget_bytes, &used);
    }
  }

  vkr_recording_write_analysis(json, data, analysis, include_series,
                               series_limit, spike_limit, pass_limit);
  return !json->failed;
}

/* The series a frames column reads, VKR_RECORDING_COLUMN_FRAME_MS or
   VKR_RECORDING_COLUMN_UNKNOWN. */
vkr_internal uint32_t vkr_recording_column(const VkrRecordingData *data,
                                           const char *name) {
  if (string_equals(name, VKR_RECORDING_FRAME_MS_FIELD)) {
    return VKR_RECORDING_COLUMN_FRAME_MS;
  }
  static const char *const player_fields[] = {
      VKR_RECORDING_PLAYER_X_FIELD, VKR_RECORDING_PLAYER_Y_FIELD,
      VKR_RECORDING_PLAYER_Z_FIELD, VKR_RECORDING_PLAYER_SPEED_FIELD};
  for (uint32_t i = 0u; data->has_player && i < ArrayCount(player_fields);
       ++i) {
    if (string_equals(name, player_fields[i])) {
      return VKR_RECORDING_COLUMN_PLAYER + i;
    }
  }
  static const struct {
    const char *prefix;
    uint32_t bit;
  } pass_fields[] = {
      {VKR_RECORDING_PASS_GPU_FIELD, VKR_RECORDING_COLUMN_PASS_GPU},
      {VKR_RECORDING_PASS_CPU_FIELD, VKR_RECORDING_COLUMN_PASS_CPU},
  };
  for (uint32_t i = 0u; i < ArrayCount(pass_fields); ++i) {
    const uint64_t length = string_length(pass_fields[i].prefix);
    if (string_length(name) > length &&
        MemCompare(name, pass_fields[i].prefix, length) == 0) {
      const uint32_t pass = vkr_recording_data_find_pass(data, name + length);
      return pass == UINT32_MAX ? VKR_RECORDING_COLUMN_UNKNOWN
                                : pass_fields[i].bit | pass;
    }
  }
  return vkr_recording_data_find_series(data, name);
}

/* One pass's time in `frame`: false_v when the frame has no row for it or,
   for GPU, no GPU time. */
vkr_internal bool8_t vkr_recording_pass_cell(const VkrRecordingData *data,
                                             uint32_t column, uint64_t frame,
                                             float64_t *out_ms) {
  const uint32_t pass = column & VKR_RECORDING_COLUMN_PASS_MASK;
  const bool8_t gpu = (column & VKR_RECORDING_COLUMN_PASS_GPU) != 0u;
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, frame, &count);
  for (uint32_t i = 0u; i < count; ++i) {
    if (rows[i].pass != pass) {
      continue;
    }
    if (gpu && !(rows[i].flags & VKR_RECORDING_PASS_GPU_VALID)) {
      return false_v;
    }
    *out_ms = gpu ? (float64_t)rows[i].gpu_ms : (float64_t)rows[i].cpu_ms;
    return true_v;
  }
  return false_v;
}

vkr_internal void vkr_recording_write_cell(VkrJsonWriter *json,
                                           const VkrRecordingData *data,
                                           uint32_t column, uint64_t frame) {
  if (column == VKR_RECORDING_COLUMN_FRAME_MS) {
    float64_t frame_ms = 0.0;
    const bool8_t present = vkr_recording_data_frame_ms(data, frame, &frame_ms);
    vkr_recording_json_optional(json, present, frame_ms);
    return;
  }
  if (column == VKR_RECORDING_COLUMN_UNKNOWN) {
    vkr_json_writer_null(json);
    return;
  }
  if (column >= VKR_RECORDING_COLUMN_PLAYER &&
      column < VKR_RECORDING_COLUMN_FRAME_MS) {
    const uint32_t axis = column - VKR_RECORDING_COLUMN_PLAYER;
    const bool8_t present = data->player_valid[frame] != 0u;
    vkr_recording_json_optional(json, present,
                                axis < 3u
                                    ? data->player_positions[frame * 3u + axis]
                                    : (float64_t)data->player_speeds[frame]);
    return;
  }
  if (column &
      (VKR_RECORDING_COLUMN_PASS_GPU | VKR_RECORDING_COLUMN_PASS_CPU)) {
    float64_t ms = 0.0;
    const bool8_t present = vkr_recording_pass_cell(data, column, frame, &ms);
    vkr_recording_json_optional(json, present, ms);
    return;
  }
  const VkrRecordingSeries *series = &data->series[column];
  vkr_recording_json_optional(json, series->valid[frame],
                              series->values[frame]);
}

bool8_t vkr_recording_data_has_field(const VkrRecordingData *data,
                                     const char *name) {
  return vkr_recording_column(data, name) != VKR_RECORDING_COLUMN_UNKNOWN;
}

/* Slowest GPU time first, then slowest CPU time, then the lower pass. */
vkr_internal int32_t vkr_recording_compare_rows(const void *lhs,
                                                const void *rhs) {
  const VkrRecordingPassRow *a = lhs;
  const VkrRecordingPassRow *b = rhs;
  if (a->gpu_ms != b->gpu_ms) {
    return a->gpu_ms > b->gpu_ms ? -1 : 1;
  }
  if (a->cpu_ms != b->cpu_ms) {
    return a->cpu_ms > b->cpu_ms ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

bool8_t vkr_recording_passes_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        const VkrRecordingAnalysis *analysis,
                                        uint32_t top, uint64_t frame,
                                        uint64_t reach) {
  vkr_assert(json != NULL && data != NULL && analysis != NULL);
  const uint32_t limit =
      top > 0u ? Min(top, analysis->pass_count) : analysis->pass_count;
  vkr_json_writer_begin_object(json);
  vkr_recording_write_passes(json, data, analysis, limit);
  if (frame != UINT64_MAX) {
    uint64_t shown = frame;
    if (!vkr_recording_gpu_timed(data, frame)) {
      const uint64_t near =
          vkr_recording_data_gpu_frame_near(data, frame, reach);
      shown = near != UINT64_MAX ? near : frame;
    }
    uint32_t count = 0u;
    const VkrRecordingPassRow *rows =
        vkr_recording_data_pass_rows(data, shown, &count);
    VkrRecordingPassRow sorted[VKR_RECORDING_PASS_MAX];
    if (count > 0u) {
      MemCopy(sorted, rows, sizeof(*rows) * count);
      vkr_sort(sorted, count, sizeof(*sorted), vkr_recording_compare_rows);
    }
    vkr_recording_json_key(json, "frame");
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_u64(json, "requested", frame);
    vkr_recording_json_named_u64(json, "frame", shown);
    vkr_recording_json_key(json, "gpu");
    vkr_json_writer_bool(json, vkr_recording_gpu_timed(data, shown));
    vkr_recording_json_key(json, "rows");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < count; ++i) {
      vkr_json_writer_begin_array(json);
      vkr_recording_json_text(json, data->passes[sorted[i].pass].name);
      vkr_recording_json_number(json, (float64_t)sorted[i].cpu_ms);
      vkr_recording_json_optional(
          json, (sorted[i].flags & VKR_RECORDING_PASS_GPU_VALID) != 0u,
          (float64_t)sorted[i].gpu_ms);
      vkr_json_writer_end_array(json);
    }
    vkr_json_writer_end_array(json);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_object(json);
  return !json->failed;
}

bool8_t vkr_recording_frames_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        uint64_t first, uint64_t last,
                                        const char *const *fields,
                                        uint32_t field_count,
                                        uint32_t row_max) {
  vkr_assert(json != NULL && data != NULL);
  vkr_assert(fields != NULL || field_count == 0u);
  static const char *const fixed_columns[] = {"frame", "time", "x", "y", "z"};
  uint32_t columns[VKR_RECORDING_FIELD_MAX];
  const uint32_t column_count =
      Min(field_count, (uint32_t)VKR_RECORDING_FIELD_MAX);
  for (uint32_t i = 0u; i < column_count; ++i) {
    columns[i] = vkr_recording_column(data, fields[i]);
  }

  const uint64_t rows_cap =
      row_max > 0u ? row_max : VKR_RECORDING_FRAMES_ROW_MAX;
  uint64_t rows_total = 0u;
  if (data->frame_count > 0u && first <= last && first < data->frame_count) {
    rows_total = Min(last, data->frame_count - 1u) - first + 1u;
  }
  const uint64_t rows = Min(rows_total, rows_cap);

  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "frames", data->frame_count);
  vkr_recording_json_named_u64(json, "first", first);
  vkr_recording_json_named_u64(json, "last", last);
  vkr_recording_json_key(json, "frame_source");
  vkr_recording_json_text(json, vkr_recording_data_frame_source(data));
  vkr_recording_json_key(json, "columns");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < ArrayCount(fixed_columns); ++i) {
    vkr_recording_json_text(json, fixed_columns[i]);
  }
  for (uint32_t i = 0u; i < column_count; ++i) {
    vkr_recording_json_text(json, fields[i]);
  }
  vkr_json_writer_end_array(json);

  vkr_recording_json_key(json, "rows");
  vkr_json_writer_begin_array(json);
  for (uint64_t row = 0u; row < rows; ++row) {
    const uint64_t frame = first + row;
    vkr_json_writer_begin_array(json);
    vkr_json_writer_u64(json, frame);
    vkr_recording_json_number(json, data->times[frame]);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      vkr_recording_json_optional(json, data->camera_valid[frame],
                                  data->positions[frame * 3u + axis]);
    }
    for (uint32_t i = 0u; i < column_count; ++i) {
      vkr_recording_write_cell(json, data, columns[i], frame);
    }
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_named_u64(json, "rows_total", rows_total);
  vkr_recording_json_key(json, "truncated");
  vkr_json_writer_bool(json, rows < rows_total);

  /* The events of the written rows, as the analysis writes them. */
  const uint64_t end = first + rows;
  uint32_t events = 0u;
  uint64_t events_total = 0u;
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->event_count; ++i) {
    const VkrRecordingDataEvent *event = &data->events[i];
    if (event->frame < first || event->frame >= end) {
      continue;
    }
    ++events_total;
    if (events < VKR_RECORDING_FRAMES_EVENT_MAX) {
      vkr_recording_write_event(json, data, event);
      ++events;
    }
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "events_truncated");
  vkr_json_writer_bool(json, events < events_total);
  vkr_json_writer_end_object(json);
  return !json->failed;
}

/* [name, ms, median, excess] per value, names from the series or, for
   passes, the pass table. */
vkr_internal void vkr_recording_write_explain_values(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingExplainValue *values, uint32_t count, bool8_t passes) {
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingExplainValue *value = &values[i];
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, passes ? data->passes[value->index].name
                                         : data->series[value->index].name);
    vkr_recording_json_number(json, value->value);
    vkr_recording_json_number(json, value->median);
    vkr_recording_json_number(json, value->excess);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
}

/* "gpu": the frame's submission against its median, the GPU frame its wait
   stalled on, and the passes that grew in the pass frame. */
vkr_internal void
vkr_recording_write_explain_gpu(VkrJsonWriter *json,
                                const VkrRecordingData *data,
                                const VkrRecordingExplanation *explanation) {
  vkr_recording_json_key(json, "gpu");
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_optional(
      json, "submission_ms", explanation->gpu_valid, explanation->gpu_ms);
  vkr_recording_json_named_optional(json, "median_ms", explanation->gpu_valid,
                                    explanation->gpu_median);
  vkr_recording_json_key(json, "wait");
  if (explanation->gpu_wait_frame != UINT64_MAX) {
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_u64(json, "frame", explanation->gpu_wait_frame);
    vkr_recording_json_named_number(json, "ms", explanation->gpu_wait_ms);
    vkr_recording_json_named_number(json, "excess",
                                    explanation->gpu_wait_excess);
    vkr_json_writer_end_object(json);
  } else {
    vkr_json_writer_null(json);
  }
  const bool8_t rows = explanation->pass_frame != UINT64_MAX;
  vkr_recording_json_named_optional(json, "pass_frame", rows,
                                    (float64_t)explanation->pass_frame);
  vkr_recording_json_key(json, "pass_time");
  if (rows) {
    vkr_recording_json_text(json, explanation->pass_gpu ? "gpu" : "cpu");
  } else {
    vkr_json_writer_null(json);
  }
  vkr_recording_json_key(json, "passes");
  vkr_recording_write_explain_values(json, data, explanation->passes,
                                     explanation->pass_count, true_v);
  vkr_json_writer_end_object(json);
}

/* "events", "flags" and "system": what else happened in the frame. */
vkr_internal void vkr_recording_write_explain_context(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingExplanation *explanation) {
  /* [name, count, ms] per engine event name. */
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < explanation->event_count; ++i) {
    const VkrRecordingExplainEvent *event = &explanation->events[i];
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, data->event_names[event->name].name);
    vkr_json_writer_u64(json, event->count);
    vkr_recording_json_number(json, event->ms);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);

  /* [name, lag] per flag set in the frame (0) or the row before it (1). */
  vkr_recording_json_key(json, "flags");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < explanation->flag_count; ++i) {
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(
        json, vkr_recording_analysis_flag_name(explanation->flags[i].flag));
    vkr_json_writer_u64(json, explanation->flags[i].lag);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);

  /* name: sample or null; the footprint adds its change from the previous
     frame. */
  vkr_recording_json_key(json, "system");
  vkr_json_writer_begin_object(json);
  for (uint32_t i = 0u; i < explanation->system_count; ++i) {
    const VkrRecordingExplainSystem *entry = &explanation->system[i];
    vkr_recording_json_named_optional(json, data->series[entry->series].name,
                                      entry->valid, entry->value);
    if (string_equals(data->series[entry->series].name,
                      VKR_RECORDING_FOOTPRINT_SERIES)) {
      vkr_recording_json_named_optional(json, "os.memory.footprint_delta_bytes",
                                        entry->has_delta, entry->delta);
    }
  }
  vkr_json_writer_end_object(json);
}

bool8_t
vkr_recording_explain_write_json(VkrJsonWriter *json,
                                 const VkrRecordingData *data,
                                 const VkrRecordingAnalysis *analysis,
                                 const VkrRecordingExplanation *explanation) {
  vkr_assert(json != NULL && data != NULL && analysis != NULL &&
             explanation != NULL);
  const bool8_t compared =
      explanation->frame_valid && explanation->median_valid;
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "frame", explanation->frame);
  vkr_recording_json_named_number(json, "time", explanation->time);
  vkr_recording_json_named_optional(json, "spike", explanation->spike_rank > 0u,
                                    (float64_t)explanation->spike_rank);
  vkr_recording_json_named_optional(json, "frame_ms", explanation->frame_valid,
                                    explanation->frame_ms);
  vkr_recording_json_named_optional(
      json, "median_ms", explanation->median_valid, explanation->median_ms);
  vkr_recording_json_named_u64(json, "median_frames",
                               VKR_RECORDING_ANALYSIS_WINDOW);
  vkr_recording_json_named_number(json, "budget_ms", explanation->budget_ms);
  vkr_recording_json_key(json, "budget_source");
  vkr_recording_json_text(json, analysis->budget_source);
  vkr_recording_json_named_optional(
      json, "ratio", compared && explanation->median_ms > 0.0,
      explanation->frame_ms / explanation->median_ms);
  vkr_recording_json_named_optional(json, "excess_ms", compared,
                                    explanation->frame_ms -
                                        explanation->median_ms);

  /* Stage and pass rows; medians are over the recording. */
  vkr_recording_json_key(json, "columns");
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, "name");
  vkr_recording_json_text(json, "ms");
  vkr_recording_json_text(json, "median");
  vkr_recording_json_text(json, "excess");
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "cpu");
  vkr_recording_write_explain_values(json, data, explanation->stages,
                                     explanation->stage_count, false_v);
  vkr_recording_write_explain_gpu(json, data, explanation);
  vkr_recording_write_explain_context(json, data, explanation);

  vkr_recording_json_key(json, "causes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < explanation->cause_count; ++i) {
    vkr_recording_write_cause(json, &explanation->causes[i], true_v);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
  return !json->failed;
}
