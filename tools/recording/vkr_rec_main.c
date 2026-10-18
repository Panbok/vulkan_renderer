/*
 * vkr_rec: reads a recording without the editor or the GPU
 * (docs/proposals/recordings.md, "Offline command").
 *
 *   vkr_rec summary <bundle|stream> [--budget MS]
 *                                     JSON: identity, streams, counters
 *                                     and, with a metrics stream, the
 *                                     analysis summary.json carries
 *   vkr_rec verify <bundle|stream>    decodes every chunk and record
 *   vkr_rec spikes <bundle|stream> [--top N] [--budget MS]
 *                                     JSON: frame-time statistics and the N
 *                                     worst spikes (default 20)
 *   --budget sets the frame budget in milliseconds, for a recording made
 *   before frame.present_interval, such as one on a 75 Hz display (13.333);
 *   without it the analysis takes that series' median, else 16.667.
 *   vkr_rec frames <bundle|stream> --range A:B [--fields a,b,...]
 *                                     JSON rows of frames A to B inclusive:
 *                                     frame, time, camera position, then
 *                                     the fields (default frame_ms; a
 *                                     pass is pass.gpu:<name> or
 *                                     pass.cpu:<name>, the player
 *                                     player.x, .y, .z or .speed); at
 *                                     most 500 rows,
 *                                     "truncated" says so
 *   vkr_rec passes <bundle|stream> [--top N] [--frame F]
 *                                     JSON: per-pass CPU and GPU statistics
 *                                     of a Tier 1 recording, largest GPU
 *                                     total first, and with --frame that
 *                                     frame's rows (or the nearest frame
 *                                     with GPU times)
 *
 * Every command prints one compact JSON object on stdout. Exit status: 0 for
 * a whole recording, 2 for a recording cut off by a crash whose whole chunks
 * are valid, 1 for an unreadable or damaged recording or a usage error.
 */
#include "assets/vkr_recording_analysis.h"
#include "assets/vkr_recording_annotations.h"
#include "assets/vkr_recording_compare.h"
#include "assets/vkr_recording_format.h"
#include "containers/str.h"
#include "core/vkr_json_writer.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"
#include "vkr_depot_zstd.h"

#include <stdio.h>
#include <string.h>

#define REC_PATH_MAX 4096u
/* Largest --top a spikes query takes. */
#define REC_TOP_MAX 1000u
#define REC_FIELD_MAX 32u
#define REC_FIELDS_TEXT_MAX 2048u

typedef enum RecExit {
  REC_EXIT_OK = 0,
  REC_EXIT_DAMAGED = 1,
  REC_EXIT_TRUNCATED = 2,
} RecExit;

static VkrAllocator rec_allocator;

typedef struct RecStreamCount {
  uint64_t records;
  uint64_t bad_size;
} RecStreamCount;

/* Everything one pass over the chunks learns. */
typedef struct RecScan {
  VkrRecordingDictionary dictionary;
  bool8_t has_dictionary;
  VkrRecordingEnd end;
  bool8_t has_end;
  RecStreamCount streams[VKR_RECORDING_STREAM_MAX];
  uint64_t records;
  uint64_t unknown_stream_records;
  uint64_t first_frame;
  uint64_t last_frame;
  bool8_t any_frame;
  uint32_t bad_chunks;
  uint32_t chunk_counts[8];
  uint64_t gap_records;
  uint64_t gap_lost;
} RecScan;

static bool8_t rec_stdout_sink(void *context, const uint8_t *data,
                               uint64_t length) {
  (void)context;
  return fwrite(data, 1u, (size_t)length, stdout) == (size_t)length;
}

static String8 rec_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

/* A bundle directory holds stream.vkrs; a file path is used as given. */
static VkrRecordingReadError rec_open(const char *path,
                                      VkrRecordingReader *reader) {
  VkrRecordingReadError error =
      vkr_recording_reader_open(reader, &rec_allocator, path);
  if (error != VKR_RECORDING_READ_OPEN) {
    return error;
  }
  char stream[REC_PATH_MAX];
  if (snprintf(stream, sizeof(stream), "%s/stream.vkrs", path) >=
      (int)sizeof(stream)) {
    return VKR_RECORDING_READ_OPEN;
  }
  return vkr_recording_reader_open(reader, &rec_allocator, stream);
}

static void rec_note_records(RecScan *scan, const uint8_t *payload,
                             uint64_t size, bool8_t *out_bad) {
  uint64_t cursor = 0u;
  VkrRecordingRecord record;
  while (vkr_recording_records_next(payload, size, &cursor, &record)) {
    ++scan->records;
    if (!scan->any_frame || record.frame < scan->first_frame) {
      scan->first_frame = record.frame;
    }
    if (!scan->any_frame || record.frame > scan->last_frame) {
      scan->last_frame = record.frame;
    }
    scan->any_frame = true_v;
    const VkrRecordingStream *stream =
        scan->has_dictionary
            ? vkr_recording_dictionary_find(&scan->dictionary, record.stream)
            : NULL;
    if (!stream || record.stream >= VKR_RECORDING_STREAM_MAX) {
      ++scan->unknown_stream_records;
      continue;
    }
    RecStreamCount *count = &scan->streams[record.stream];
    ++count->records;
    if (stream->record_size != 0u && stream->record_size != record.size) {
      ++count->bad_size;
      *out_bad = true_v;
    }
    if (record.stream == VKR_RECORDING_STREAM_RECORDER && record.size >= 16u) {
      uint64_t lost = 0u;
      memcpy(&lost, record.data + 8u, sizeof(lost));
      ++scan->gap_records;
      scan->gap_lost += lost;
    }
  }
  if (cursor != size) {
    *out_bad = true_v;
  }
}

static void rec_scan(const VkrRecordingReader *reader, RecScan *scan) {
  memset(scan, 0, sizeof(*scan));
  const VkrCodec *codec = vkr_depot_zstd_codec();
  for (uint32_t i = 0u; i < reader->chunk_count; ++i) {
    const VkrRecordingChunkInfo *info = &reader->chunks[i];
    if (info->type < ArrayCount(scan->chunk_counts)) {
      ++scan->chunk_counts[info->type];
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(reader, i, codec, &rec_allocator, &payload,
                                    &size)) {
      ++scan->bad_chunks;
      continue;
    }
    bool8_t bad = false_v;
    if (info->type == VKR_RECORDING_CHUNK_DICTIONARY && !scan->has_dictionary) {
      scan->has_dictionary = vkr_recording_dictionary_decode(
          payload, size, &rec_allocator, &scan->dictionary);
      bad = !scan->has_dictionary;
    } else if (info->type == VKR_RECORDING_CHUNK_END) {
      scan->has_end = vkr_recording_end_decode(payload, size, &scan->end);
      bad = !scan->has_end;
    } else if (info->type == VKR_RECORDING_CHUNK_RECORDS) {
      rec_note_records(scan, payload, size, &bad);
    }
    if (bad) {
      ++scan->bad_chunks;
    }
    vkr_recording_reader_chunk_free(&rec_allocator, payload, size);
  }
}

static void rec_write_identity(VkrJsonWriter *json,
                               const VkrRecordingReader *reader) {
  const uint32_t index =
      vkr_recording_reader_find(reader, VKR_RECORDING_CHUNK_IDENTITY);
  uint8_t *payload = NULL;
  uint64_t size = 0u;
  vkr_json_writer_name(json, rec_str("identity"));
  if (index == UINT32_MAX ||
      !vkr_recording_reader_chunk(reader, index, vkr_depot_zstd_codec(),
                                  &rec_allocator, &payload, &size)) {
    vkr_json_writer_null(json);
    return;
  }
  /* The identity is JSON text; it is emitted as a string so a malformed
     identity cannot break this output. */
  vkr_json_writer_string(json, string8_create_from_cstr(payload, size));
  vkr_recording_reader_chunk_free(&rec_allocator, payload, size);
}

static void rec_write_streams(VkrJsonWriter *json, const RecScan *scan) {
  vkr_json_writer_name(json, rec_str("streams"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u;
       scan->has_dictionary && i < scan->dictionary.stream_count; ++i) {
    const VkrRecordingStream *stream = &scan->dictionary.streams[i];
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("id"));
    vkr_json_writer_u64(json, stream->id);
    vkr_json_writer_name(json, rec_str("name"));
    vkr_json_writer_string(json, rec_str(stream->name));
    vkr_json_writer_name(json, rec_str("droppable"));
    vkr_json_writer_bool(json,
                         (stream->flags & VKR_RECORDING_STREAM_DROPPABLE) != 0);
    vkr_json_writer_name(json, rec_str("record_size"));
    vkr_json_writer_u64(json, stream->record_size);
    vkr_json_writer_name(json, rec_str("records"));
    vkr_json_writer_u64(json, scan->streams[stream->id].records);
    vkr_json_writer_name(json, rec_str("fields"));
    vkr_json_writer_begin_array(json);
    for (uint32_t f = 0u; f < stream->field_count; ++f) {
      const VkrRecordingField *field = &stream->fields[f];
      /* "name:type", "name:typexN", then the unit if any. */
      char text[160];
      if (field->count > 1u) {
        snprintf(text, sizeof(text), "%s:%sx%u%s%s", field->name,
                 vkr_recording_field_type_name(field->type),
                 (unsigned)field->count, field->unit[0] ? " " : "",
                 field->unit);
      } else {
        snprintf(text, sizeof(text), "%s:%s%s%s", field->name,
                 vkr_recording_field_type_name(field->type),
                 field->unit[0] ? " " : "", field->unit);
      }
      vkr_json_writer_string(json, rec_str(text));
    }
    vkr_json_writer_end_array(json);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
}

static void rec_write_end(VkrJsonWriter *json, const RecScan *scan) {
  vkr_json_writer_name(json, rec_str("end"));
  if (!scan->has_end) {
    vkr_json_writer_null(json);
    return;
  }
  const VkrRecordingEnd *end = &scan->end;
  vkr_json_writer_begin_object(json);
  vkr_json_writer_name(json, rec_str("records"));
  vkr_json_writer_u64(json, end->records);
  vkr_json_writer_name(json, rec_str("dropped_records"));
  vkr_json_writer_u64(json, end->dropped_records);
  vkr_json_writer_name(json, rec_str("lost_records"));
  vkr_json_writer_u64(json, end->lost_records);
  vkr_json_writer_name(json, rec_str("raw_bytes"));
  vkr_json_writer_u64(json, end->raw_bytes);
  vkr_json_writer_name(json, rec_str("stored_bytes"));
  vkr_json_writer_u64(json, end->stored_bytes);
  vkr_json_writer_name(json, rec_str("seconds"));
  vkr_json_writer_f64(json, end->seconds);
  vkr_json_writer_name(json, rec_str("writer_seconds"));
  vkr_json_writer_f64(json, end->writer_seconds);
  vkr_json_writer_end_object(json);
}

static bool8_t rec_has_stream(const RecScan *scan, const char *name) {
  for (uint32_t i = 0u;
       scan->has_dictionary && i < scan->dictionary.stream_count; ++i) {
    if (strcmp(scan->dictionary.streams[i].name, name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t rec_load(const char *path, VkrRecordingData *data) {
  const VkrRecordingDataError error = vkr_recording_data_load(
      data, &rec_allocator, path, vkr_depot_zstd_codec());
  if (error != VKR_RECORDING_DATA_OK) {
    fprintf(stderr, "vkr_rec: cannot load '%s' (error %d)\n", path, (int)error);
    return false_v;
  }
  return true_v;
}

/* The bounded analysis object summary.json carries, or null. */
static void rec_write_analysis(VkrJsonWriter *json, const char *path,
                               float64_t budget_ms) {
  vkr_json_writer_name(json, rec_str("analysis"));
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    vkr_json_writer_null(json);
    return;
  }
  const VkrRecordingAnalysisOptions options = {.budget_ms = budget_ms};
  VkrRecordingAnalysis analysis;
  if (vkr_recording_analyze(&data, &options, &rec_allocator, &analysis)) {
    vkr_recording_analysis_write_json(json, &data, &analysis, NULL);
    vkr_recording_analysis_free(&analysis);
  } else {
    vkr_json_writer_null(json);
  }
  vkr_recording_data_free(&data);
}

/* The bundle's annotations.json, or null when the bundle has none or it
   cannot be read; `path` is the bundle or its stream file. */
static void rec_write_annotations(VkrJsonWriter *json, const char *path) {
  char bundle[VKR_RECORDING_ANNOTATIONS_PATH_MAX];
  string_format(bundle, sizeof(bundle), "%s", path);
  const uint64_t length = string_length(bundle);
  const uint64_t stream = sizeof("/stream.vkrs") - 1u;
  if (length > stream &&
      string_equals(bundle + length - stream, "/stream.vkrs")) {
    bundle[length - stream] = '\0';
  }
  vkr_json_writer_name(json, rec_str("annotations"));
  static VkrRecordingAnnotations annotations;
  if (!vkr_recording_annotations_read(bundle, &annotations)) {
    vkr_json_writer_null(json);
    return;
  }
  vkr_json_writer_begin_object(json);
  vkr_json_writer_name(json, rec_str("review"));
  vkr_json_writer_string(
      json, rec_str(vkr_recording_review_name(annotations.review)));
  vkr_json_writer_name(json, rec_str("review_by"));
  vkr_json_writer_string(json, rec_str(annotations.review_by));
  vkr_json_writer_name(json, rec_str("notes"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < annotations.note_count; ++i) {
    const VkrRecordingNote *note = &annotations.notes[i];
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("frame"));
    vkr_json_writer_u64(json, note->frame);
    vkr_json_writer_name(json, rec_str("author"));
    vkr_json_writer_string(json, rec_str(note->author));
    vkr_json_writer_name(json, rec_str("text"));
    vkr_json_writer_string(json, rec_str(note->text));
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_name(json, rec_str("ranges"));
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < annotations.range_count; ++i) {
    const VkrRecordingRange *range = &annotations.ranges[i];
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("first"));
    vkr_json_writer_u64(json, range->first);
    vkr_json_writer_name(json, rec_str("last"));
    vkr_json_writer_u64(json, range->last);
    vkr_json_writer_name(json, rec_str("label"));
    vkr_json_writer_string(json, rec_str(range->name));
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
}

static RecExit rec_status(const VkrRecordingReader *reader,
                          const RecScan *scan) {
  if (scan->bad_chunks > 0u || !scan->has_dictionary ||
      scan->unknown_stream_records > 0u) {
    return REC_EXIT_DAMAGED;
  }
  return reader->truncated ? REC_EXIT_TRUNCATED : REC_EXIT_OK;
}

static const char *rec_status_name(RecExit status) {
  switch (status) {
  case REC_EXIT_OK:
    return "complete";
  case REC_EXIT_TRUNCATED:
    return "truncated";
  case REC_EXIT_DAMAGED:
    return "damaged";
  }
  return "damaged";
}

static int rec_report(const char *path, bool8_t verbose, float64_t budget_ms) {
  VkrRecordingReader reader;
  const VkrRecordingReadError error = rec_open(path, &reader);
  if (error != VKR_RECORDING_READ_OK) {
    fprintf(stderr, "vkr_rec: cannot read '%s' (error %d)\n", path, (int)error);
    return REC_EXIT_DAMAGED;
  }
  RecScan scan;
  rec_scan(&reader, &scan);
  const RecExit status = rec_status(&reader, &scan);

  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("version"));
  vkr_json_writer_u64(&json, reader.version);
  vkr_json_writer_name(&json, rec_str("indexed"));
  vkr_json_writer_bool(&json, reader.indexed);
  vkr_json_writer_name(&json, rec_str("truncated"));
  vkr_json_writer_bool(&json, reader.truncated);
  vkr_json_writer_name(&json, rec_str("file_bytes"));
  vkr_json_writer_u64(&json, reader.size);
  vkr_json_writer_name(&json, rec_str("valid_bytes"));
  vkr_json_writer_u64(&json, reader.valid_bytes);
  vkr_json_writer_name(&json, rec_str("chunks"));
  vkr_json_writer_u64(&json, reader.chunk_count);
  vkr_json_writer_name(&json, rec_str("record_chunks"));
  vkr_json_writer_u64(&json, scan.chunk_counts[VKR_RECORDING_CHUNK_RECORDS]);
  vkr_json_writer_name(&json, rec_str("bad_chunks"));
  vkr_json_writer_u64(&json, scan.bad_chunks);
  vkr_json_writer_name(&json, rec_str("records"));
  vkr_json_writer_u64(&json, scan.records);
  vkr_json_writer_name(&json, rec_str("first_frame"));
  vkr_json_writer_u64(&json, scan.first_frame);
  vkr_json_writer_name(&json, rec_str("last_frame"));
  vkr_json_writer_u64(&json, scan.last_frame);
  vkr_json_writer_name(&json, rec_str("gaps"));
  vkr_json_writer_u64(&json, scan.gap_records);
  vkr_json_writer_name(&json, rec_str("gap_lost_records"));
  vkr_json_writer_u64(&json, scan.gap_lost);
  if (verbose) {
    rec_write_identity(&json, &reader);
    rec_write_streams(&json, &scan);
    rec_write_end(&json, &scan);
    if (rec_has_stream(&scan, "metrics")) {
      rec_write_analysis(&json, path, budget_ms);
    }
    rec_write_annotations(&json, path);
  }
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);

  if (scan.has_dictionary) {
    vkr_recording_dictionary_free(&rec_allocator, &scan.dictionary);
  }
  vkr_recording_reader_close(&reader);
  return status;
}

/* Status of a loaded recording, with the same meaning as rec_status. */
static RecExit rec_data_status(const VkrRecordingData *data) {
  if (data->bad_chunks > 0u) {
    return REC_EXIT_DAMAGED;
  }
  return data->truncated ? REC_EXIT_TRUNCATED : REC_EXIT_OK;
}

static int rec_spikes(const char *path, uint32_t top, float64_t budget_ms) {
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    return REC_EXIT_DAMAGED;
  }
  const VkrRecordingAnalysisOptions options = {.spike_max = top,
                                               .budget_ms = budget_ms};
  VkrRecordingAnalysis analysis;
  if (!vkr_recording_analyze(&data, &options, &rec_allocator, &analysis)) {
    fprintf(stderr, "vkr_rec: out of memory analyzing '%s'\n", path);
    vkr_recording_data_free(&data);
    return REC_EXIT_DAMAGED;
  }
  const RecExit status = rec_data_status(&data);
  /* Every requested spike, without the series block. */
  const VkrRecordingAnalysisJsonOptions json_options = {
      .budget_bytes = 0u,
      .include_series = false_v,
  };
  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("top"));
  vkr_json_writer_u64(&json, top);
  vkr_json_writer_name(&json, rec_str("analysis"));
  vkr_recording_analysis_write_json(&json, &data, &analysis, &json_options);
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);

  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  return status;
}

/* "A:B" with A <= B. */
static bool8_t rec_parse_range(const char *text, uint64_t *out_first,
                               uint64_t *out_last) {
  const char *colon = strchr(text, ':');
  if (!colon || colon == text || colon - text >= 32) {
    return false_v;
  }
  char first[32];
  memcpy(first, text, (size_t)(colon - text));
  first[colon - text] = '\0';
  return string_to_u64(first, out_first) &&
         string_to_u64(colon + 1, out_last) && *out_first <= *out_last;
}

/* Splits "a,b,c" in place into `out_fields`; empty names are refused. */
static bool8_t rec_parse_fields(char *text, const char **out_fields,
                                uint32_t *out_count) {
  *out_count = 0u;
  char *name = text;
  for (;;) {
    char *comma = strchr(name, ',');
    if (comma) {
      *comma = '\0';
    }
    if (name[0] == '\0' || *out_count == REC_FIELD_MAX) {
      return false_v;
    }
    out_fields[(*out_count)++] = name;
    if (!comma) {
      return true_v;
    }
    name = comma + 1;
  }
}

static int rec_frames(const char *path, uint64_t first, uint64_t last,
                      const char **fields, uint32_t field_count) {
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    return REC_EXIT_DAMAGED;
  }
  for (uint32_t i = 0u; i < field_count; ++i) {
    if (!vkr_recording_data_has_field(&data, fields[i])) {
      fprintf(stderr, "vkr_rec: unknown field '%s'; fields: %s", fields[i],
              VKR_RECORDING_FRAME_MS_FIELD);
      for (uint32_t s = 0u; s < data.series_count; ++s) {
        fprintf(stderr, ",%s", data.series[s].name);
      }
      if (data.has_player) {
        fprintf(stderr, ",%s,%s,%s,%s", VKR_RECORDING_PLAYER_X_FIELD,
                VKR_RECORDING_PLAYER_Y_FIELD, VKR_RECORDING_PLAYER_Z_FIELD,
                VKR_RECORDING_PLAYER_SPEED_FIELD);
      }
      if (data.pass_count > 0u) {
        fprintf(stderr, ",%s<pass>,%s<pass> (vkr_rec passes lists them)",
                VKR_RECORDING_PASS_GPU_FIELD, VKR_RECORDING_PASS_CPU_FIELD);
      }
      fputc('\n', stderr);
      vkr_recording_data_free(&data);
      return REC_EXIT_DAMAGED;
    }
  }
  const RecExit status = rec_data_status(&data);
  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("range"));
  vkr_recording_frames_write_json(&json, &data, first, last, fields,
                                  field_count, 0u);
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);
  vkr_recording_data_free(&data);
  return status;
}

/* Rows within this many frames stand in for a --frame without GPU times:
   one tiled sampling period. */
#define REC_PASS_FRAME_REACH 8u

static int rec_passes(const char *path, uint32_t top, uint64_t frame) {
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    return REC_EXIT_DAMAGED;
  }
  if (data.pass_count == 0u) {
    fprintf(stderr, "vkr_rec: '%s' has no pass rows (record with tier 1)\n",
            path);
    vkr_recording_data_free(&data);
    return REC_EXIT_DAMAGED;
  }
  if (frame != UINT64_MAX && frame >= data.frame_count) {
    fprintf(stderr, "vkr_rec: --frame must be below %llu\n",
            (unsigned long long)data.frame_count);
    vkr_recording_data_free(&data);
    return REC_EXIT_DAMAGED;
  }
  VkrRecordingAnalysis analysis;
  if (!vkr_recording_analyze(&data, NULL, &rec_allocator, &analysis)) {
    fprintf(stderr, "vkr_rec: out of memory analyzing '%s'\n", path);
    vkr_recording_data_free(&data);
    return REC_EXIT_DAMAGED;
  }
  const RecExit status = rec_data_status(&data);
  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("passes"));
  vkr_recording_passes_write_json(&json, &data, &analysis, top, frame,
                                  REC_PASS_FRAME_REACH);
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);
  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  return status;
}

static void rec_usage(void);

/* Loads and analyzes one side of a comparison; false after a message. */
static bool8_t rec_compare_side(const char *path, VkrRecordingData *data,
                                VkrRecordingAnalysis *analysis) {
  if (!rec_load(path, data)) {
    return false_v;
  }
  if (!vkr_recording_analyze(data, NULL, &rec_allocator, analysis)) {
    fprintf(stderr, "vkr_rec: out of memory analyzing '%s'\n", path);
    vkr_recording_data_free(data);
    return false_v;
  }
  return true_v;
}

static int rec_compare(const char *a_path, const char *b_path,
                       const VkrRecordingCompareOptions *options) {
  VkrRecordingData a;
  VkrRecordingData b;
  VkrRecordingAnalysis a_analysis;
  VkrRecordingAnalysis b_analysis;
  if (!rec_compare_side(a_path, &a, &a_analysis)) {
    return REC_EXIT_DAMAGED;
  }
  if (!rec_compare_side(b_path, &b, &b_analysis)) {
    vkr_recording_analysis_free(&a_analysis);
    vkr_recording_data_free(&a);
    return REC_EXIT_DAMAGED;
  }
  static VkrRecordingCompare compare;
  char error[160];
  int result = REC_EXIT_DAMAGED;
  if (vkr_recording_compare(&a, &a_analysis, &b, &b_analysis, options,
                            &rec_allocator, &compare, error, sizeof(error))) {
    const RecExit a_status = rec_data_status(&a);
    const RecExit b_status = rec_data_status(&b);
    VkrJsonWriter json;
    vkr_json_writer_init(&json, rec_stdout_sink, NULL);
    vkr_json_writer_begin_object(&json);
    vkr_json_writer_name(&json, rec_str("status_a"));
    vkr_json_writer_string(&json, rec_str(rec_status_name(a_status)));
    vkr_json_writer_name(&json, rec_str("status_b"));
    vkr_json_writer_string(&json, rec_str(rec_status_name(b_status)));
    vkr_json_writer_name(&json, rec_str("compare"));
    vkr_recording_compare_write_json(&json, &a, &compare);
    vkr_json_writer_end_object(&json);
    vkr_json_writer_complete(&json);
    fputc('\n', stdout);
    vkr_recording_compare_free(&compare);
    result = Max(a_status, b_status);
  } else {
    fprintf(stderr, "vkr_rec: cannot compare: %s\n", error);
  }
  vkr_recording_analysis_free(&b_analysis);
  vkr_recording_data_free(&b);
  vkr_recording_analysis_free(&a_analysis);
  vkr_recording_data_free(&a);
  return result;
}

static int rec_run_compare(int argc, char **argv) {
  if (argc < 4) {
    rec_usage();
    return REC_EXIT_DAMAGED;
  }
  VkrRecordingCompareOptions options = {.align = VKR_RECORDING_ALIGN_TIME};
  for (int i = 4; i < argc; i += 2) {
    if (i + 1 >= argc) {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
    if (strcmp(argv[i], "--align") == 0) {
      if (!vkr_recording_align_parse(rec_str(argv[i + 1]), &options.align)) {
        fprintf(stderr,
                "vkr_rec: --align takes time, marker, distance or frame\n");
        return REC_EXIT_DAMAGED;
      }
    } else if (strcmp(argv[i], "--marker") == 0) {
      options.marker = argv[i + 1];
      options.align = VKR_RECORDING_ALIGN_MARKER;
    } else {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
  }
  return rec_compare(argv[2], argv[3], &options);
}

static void rec_usage(void) {
  fprintf(
      stderr,
      "usage: vkr_rec summary <bundle|stream> [--budget MS]\n"
      "       vkr_rec verify <bundle|stream>\n"
      "       vkr_rec spikes <bundle|stream> [--top N] [--budget MS]\n"
      "       vkr_rec frames <bundle|stream> --range A:B "
      "[--fields name,name]\n"
      "       vkr_rec passes <bundle|stream> [--top N] [--frame F]\n"
      "       vkr_rec compare <a> <b> [--align time|marker|distance|frame]"
      " [--marker label]\n"
      "A:B is inclusive; frame_ms names the frame time, pass.gpu:<pass> and\n"
      "pass.cpu:<pass> a pass's times; at most 500 rows. --budget is the\n"
      "frame budget in ms (default: the recorded present interval)\n");
}

/* Reads `--budget MS` at argv[at]; true when it is absent or valid. */
static bool8_t rec_parse_budget(int argc, char **argv, int at,
                                float64_t *out_budget_ms) {
  *out_budget_ms = 0.0;
  if (at >= argc) {
    return true_v;
  }
  if (at + 2 != argc || strcmp(argv[at], "--budget") != 0 ||
      !string_to_f64(argv[at + 1], out_budget_ms) || *out_budget_ms <= 0.0 ||
      *out_budget_ms > 1000.0) {
    fprintf(stderr, "vkr_rec: --budget takes milliseconds above 0, up to "
                    "1000\n");
    return false_v;
  }
  return true_v;
}

static int rec_run_spikes(int argc, char **argv) {
  uint32_t top = VKR_RECORDING_ANALYSIS_SPIKE_MAX;
  int next = 3;
  if (argc >= 5 && strcmp(argv[3], "--top") == 0) {
    if (!string_to_u32(argv[4], &top) || top == 0u || top > REC_TOP_MAX) {
      fprintf(stderr, "vkr_rec: --top takes 1 to %u\n", REC_TOP_MAX);
      return REC_EXIT_DAMAGED;
    }
    next = 5;
  }
  float64_t budget_ms = 0.0;
  if (argc < 3 || !rec_parse_budget(argc, argv, next, &budget_ms)) {
    rec_usage();
    return REC_EXIT_DAMAGED;
  }
  return rec_spikes(argv[2], top, budget_ms);
}

static int rec_run_passes(int argc, char **argv) {
  uint32_t top = 0u;
  uint64_t frame = UINT64_MAX;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
    if (strcmp(argv[i], "--top") == 0) {
      if (!string_to_u32(argv[i + 1], &top) || top == 0u || top > REC_TOP_MAX) {
        fprintf(stderr, "vkr_rec: --top takes 1 to %u\n", REC_TOP_MAX);
        return REC_EXIT_DAMAGED;
      }
    } else if (strcmp(argv[i], "--frame") == 0) {
      if (!string_to_u64(argv[i + 1], &frame) || frame == UINT64_MAX) {
        fprintf(stderr, "vkr_rec: --frame takes a frame number\n");
        return REC_EXIT_DAMAGED;
      }
    } else {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
  }
  return rec_passes(argv[2], top, frame);
}

static int rec_run_frames(int argc, char **argv) {
  bool8_t has_range = false_v;
  uint64_t first = 0u;
  uint64_t last = 0u;
  static char fields_text[REC_FIELDS_TEXT_MAX];
  const char *fields[REC_FIELD_MAX] = {VKR_RECORDING_FRAME_MS_FIELD};
  uint32_t field_count = 1u;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
    if (strcmp(argv[i], "--range") == 0) {
      has_range = rec_parse_range(argv[i + 1], &first, &last);
      if (!has_range) {
        fprintf(stderr, "vkr_rec: --range takes A:B with A <= B\n");
        return REC_EXIT_DAMAGED;
      }
    } else if (strcmp(argv[i], "--fields") == 0) {
      const size_t length = strlen(argv[i + 1]);
      if (length >= sizeof(fields_text)) {
        fprintf(stderr, "vkr_rec: --fields is too long\n");
        return REC_EXIT_DAMAGED;
      }
      memcpy(fields_text, argv[i + 1], length + 1u);
      if (!rec_parse_fields(fields_text, fields, &field_count)) {
        fprintf(stderr,
                "vkr_rec: --fields takes up to %u names split by "
                "commas\n",
                REC_FIELD_MAX);
        return REC_EXIT_DAMAGED;
      }
    } else {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
  }
  if (!has_range) {
    rec_usage();
    return REC_EXIT_DAMAGED;
  }
  return rec_frames(argv[2], first, last, fields, field_count);
}

VKR_MAIN(argc, argv) {
  if (argc < 3) {
    rec_usage();
    return REC_EXIT_DAMAGED;
  }
  vkr_platform_init();
  VkrDMemory memory;
  if (!vkr_dmemory_create(MB(16), GB(4), &memory)) {
    fprintf(stderr, "vkr_rec: memory reserve failed\n");
    return REC_EXIT_DAMAGED;
  }
  rec_allocator = (VkrAllocator){.ctx = &memory};
  vkr_dmemory_allocator_create(&rec_allocator);

  int result = REC_EXIT_DAMAGED;
  float64_t budget_ms = 0.0;
  if (strcmp(argv[1], "summary") == 0 && argc >= 3 &&
      rec_parse_budget(argc, argv, 3, &budget_ms)) {
    result = rec_report(argv[2], true_v, budget_ms);
  } else if (strcmp(argv[1], "verify") == 0 && argc == 3) {
    result = rec_report(argv[2], false_v, 0.0);
  } else if (strcmp(argv[1], "spikes") == 0) {
    result = rec_run_spikes(argc, argv);
  } else if (strcmp(argv[1], "frames") == 0) {
    result = rec_run_frames(argc, argv);
  } else if (strcmp(argv[1], "passes") == 0) {
    result = rec_run_passes(argc, argv);
  } else if (strcmp(argv[1], "compare") == 0) {
    result = rec_run_compare(argc, argv);
  } else {
    rec_usage();
  }

  vkr_dmemory_allocator_destroy(&rec_allocator);
  vkr_platform_shutdown();
  return result;
}
