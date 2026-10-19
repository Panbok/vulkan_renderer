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
 *   vkr_rec explain <bundle|stream> (--spike N | --frame F) [--budget MS]
 *                                     JSON: why spike N (in spikes order)
 *                                     or frame F was slow: CPU stages, GPU
 *                                     submission and passes against their
 *                                     medians, engine events, flags,
 *                                     system signals and ranked causes
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
 *   vkr_rec export <bundle|stream> --perfetto OUT [--range A:B]
 *                                     writes OUT, a Chrome trace-event JSON
 *                                     file for ui.perfetto.dev or
 *                                     chrome://tracing: CPU zones per
 *                                     thread, GPU passes, Tier 0 counters
 *                                     and markers of frames A to B
 *                                     inclusive (default all), at the
 *                                     recorder's frame times when the
 *                                     recording has no zones or pass
 *                                     starts; JSON: event counts
 *
 * Every command prints one compact JSON object on stdout. Exit status: 0 for
 * a whole recording, 2 for a recording cut off by a crash whose whole chunks
 * are valid, 1 for an unreadable or damaged recording or a usage error.
 */
#include "assets/vkr_recording_analysis.h"
#include "assets/vkr_recording_annotations.h"
#include "assets/vkr_recording_compare.h"
#include "assets/vkr_recording_format.h"
#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_json_writer.h"
#include "math/vkr_math.h"
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

/* Explains spike `spike` (1 for the largest, in `vkr_rec spikes` order) or,
   with `spike` 0, `frame`. */
static int rec_explain(const char *path, uint32_t spike, uint64_t frame,
                       float64_t budget_ms) {
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    return REC_EXIT_DAMAGED;
  }
  /* Enough spikes to rank the asked one, or to name a frame's spike. */
  const VkrRecordingAnalysisOptions options = {
      .spike_max = spike > 0u ? spike : REC_TOP_MAX,
      .budget_ms = budget_ms,
  };
  VkrRecordingAnalysis analysis;
  if (!vkr_recording_analyze(&data, &options, &rec_allocator, &analysis)) {
    fprintf(stderr, "vkr_rec: out of memory analyzing '%s'\n", path);
    vkr_recording_data_free(&data);
    return REC_EXIT_DAMAGED;
  }
  int result = REC_EXIT_DAMAGED;
  if (spike > analysis.spike_count) {
    fprintf(stderr, "vkr_rec: the recording has %u spikes\n",
            analysis.spike_count);
    goto cleanup;
  }
  if (spike > 0u) {
    frame = analysis.spikes[spike - 1u].frame;
  }
  static VkrRecordingExplanation explanation;
  if (!vkr_recording_explain(&data, &analysis, frame, &explanation)) {
    fprintf(stderr, "vkr_rec: --frame must be below %llu\n",
            (unsigned long long)data.frame_count);
    goto cleanup;
  }
  const RecExit status = rec_data_status(&data);
  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("explain"));
  vkr_recording_explain_write_json(&json, &data, &analysis, &explanation);
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);
  result = status;

cleanup:
  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);
  return result;
}

/* ------------------------------------------------------------------------ */
/* export --perfetto: a Chrome trace-event JSON file                          */

/* Events one export writes; past it the file closes valid and the status
   says "truncated". It also bounds the zones and pass rows held in memory. */
#define REC_EXPORT_EVENT_MAX 4000000u
/* The record layouts of streams "zone" and "pass_starts" (runtime
   recordings with CPU zones and pass start times). */
#define REC_ZONE_RECORD_SIZE 24u
#define REC_PASS_STARTS_HEADER_SIZE 24u
#define REC_PASS_STARTS_CPU_CLOCK 1u
/* Zone ids are u16; names longer than this are cut. */
#define REC_ZONE_ID_COUNT 65536u
#define REC_ZONE_NAME_MAX 255u
#define REC_EVENT_NAME_MAX 192u
#define REC_TRACE_CPU_PID 1u
#define REC_TRACE_GPU_PID 2u
#define REC_TRACE_GPU_QUEUE_TID 1u
#define REC_TRACE_GPU_FRAMES_TID 2u

/* One record of stream "zone", with the frame of its record. */
typedef struct RecZone {
  uint64_t begin_ns;
  uint64_t end_ns;
  uint64_t frame;
  uint16_t zone;
  uint16_t slot;
  uint32_t depth;
} RecZone;

/* A passes record: `count` rows from RecExport.pass_rows[first]. `order`
   keeps the first record of a frame first after sorting. */
typedef struct RecPassRecord {
  uint64_t frame;
  uint64_t first;
  uint32_t count;
  uint32_t order;
} RecPassRecord;

typedef struct RecPassRow {
  uint16_t pass;
  uint16_t flags;
  float32_t cpu_ms;
  float32_t gpu_ms;
} RecPassRow;

/* A pass_starts record on the CPU clock: `count` start times from
   RecExport.pass_starts[first]. */
typedef struct RecPassStarts {
  uint64_t frame;
  uint64_t gpu_start_ns;
  uint64_t gpu_end_ns;
  uint64_t first;
  uint32_t count;
  uint32_t order;
} RecPassStarts;

/* Where a frame starts: its "Frame" zone's begin on the main thread. */
typedef struct RecFrameStart {
  uint64_t frame;
  uint64_t begin_ns;
} RecFrameStart;

typedef struct RecZoneName {
  uint32_t offset;
  uint32_t length;
  bool8_t known;
} RecZoneName;

ArrayConstructor(RecZone, RecZone);
ArrayConstructor(RecPassRecord, RecPassRecord);
ArrayConstructor(RecPassRow, RecPassRow);
ArrayConstructor(RecPassStarts, RecPassStarts);
ArrayConstructor(RecFrameStart, RecFrameStart);

/* A Tier 0 series the trace shows as a counter, and the scale from its
   recorded unit to the counter's. */
typedef struct RecTraceCounter {
  const char *series;
  const char *label;
  float64_t scale;
} RecTraceCounter;

static const RecTraceCounter REC_TRACE_COUNTERS[] = {
    {.series = "frame.wall", .label = "frame.wall (ms)", .scale = 1.0},
    {.series = "gpu.submission", .label = "gpu.submission (ms)", .scale = 1.0},
    {.series = "cpu.process_time",
     .label = "cpu.process_time (ms)",
     .scale = 1.0},
    {.series = "os.memory.footprint_bytes",
     .label = "os.memory.footprint (MiB)",
     .scale = 1.0 / (1024.0 * 1024.0)},
    {.series = "os.thermal.level", .label = "os.thermal.level", .scale = 1.0},
    {.series = "os.memory.pressure",
     .label = "os.memory.pressure",
     .scale = 1.0},
    {.series = "gpu.present.latency",
     .label = "gpu.present.latency (ms)",
     .scale = 1.0},
};

typedef struct RecExport {
  uint64_t first_frame;
  uint64_t last_frame;
  /* Stream ids found by name, UINT32_MAX when the recording lacks one. */
  uint32_t zone_name_stream;
  uint32_t zone_stream;
  uint32_t passes_stream;
  uint32_t pass_starts_stream;

  RecZoneName zone_names[REC_ZONE_ID_COUNT];
  Array_uint8_t zone_name_text;
  Array_RecZone zones;
  Array_RecPassRecord pass_records;
  Array_RecPassRow pass_rows;
  Array_RecPassStarts starts;
  Array_float32_t start_ms;
  Array_RecFrameStart frame_starts;
  bool8_t slot_used[REC_ZONE_ID_COUNT];

  /* Earliest time the trace shows; every ts is relative to it so the
     microsecond values keep their nanosecond digits. */
  uint64_t base_ns;
  uint64_t events;
  bool8_t capped;
  bool8_t out_of_memory;
  uint32_t bad_chunks;
  uint64_t bad_records;
  uint64_t zone_events;
  uint64_t gpu_frames;
  uint64_t gpu_passes;
  uint64_t gpu_frames_unmatched;
  uint64_t counter_events;
  uint64_t marker_events;
  uint64_t markers_untimed;
  /* Frames start at the recorder's frame times, not at Frame zones. */
  bool8_t recorder_clock;
} RecExport;

static RecExport rec_export_state;

static uint32_t rec_find_stream_id(const VkrRecordingDictionary *dictionary,
                                   const char *name) {
  for (uint32_t i = 0u; i < dictionary->stream_count; ++i) {
    if (strcmp(dictionary->streams[i].name, name) == 0) {
      return dictionary->streams[i].id;
    }
  }
  return UINT32_MAX;
}

static bool8_t rec_export_in_range(const RecExport *ex, uint64_t frame) {
  return frame >= ex->first_frame && frame <= ex->last_frame;
}

/* False once the zones and pass rows held reach the event cap. */
static bool8_t rec_export_room(RecExport *ex, uint64_t more) {
  const uint64_t held =
      ex->zones.length + ex->pass_rows.length + ex->start_ms.length;
  if (held + more > REC_EXPORT_EVENT_MAX) {
    ex->capped = true_v;
    return false_v;
  }
  return true_v;
}

static void rec_export_zone_name(RecExport *ex,
                                 const VkrRecordingRecord *record) {
  if (record->size < 4u) {
    ++ex->bad_records;
    return;
  }
  const uint32_t id = vkr_load_le_u32(record->data);
  if (id >= REC_ZONE_ID_COUNT) {
    ++ex->bad_records;
    return;
  }
  RecZoneName *name = &ex->zone_names[id];
  if (name->known) {
    return;
  }
  const uint32_t length = Min(record->size - 4u, REC_ZONE_NAME_MAX);
  const uint64_t offset = ex->zone_name_text.length;
  if (!array_reserve_uint8_t(&ex->zone_name_text, offset + length)) {
    ex->out_of_memory = true_v;
    return;
  }
  MemCopy(ex->zone_name_text.data + offset, record->data + 4u, length);
  ex->zone_name_text.length += length;
  *name = (RecZoneName){
      .offset = (uint32_t)offset,
      .length = length,
      .known = true_v,
  };
}

static void rec_export_zone(RecExport *ex, const VkrRecordingRecord *record) {
  if (record->size != REC_ZONE_RECORD_SIZE) {
    ++ex->bad_records;
    return;
  }
  if (!rec_export_room(ex, 1u)) {
    return;
  }
  const RecZone zone = {
      .zone = vkr_load_le_u16(record->data),
      .slot = vkr_load_le_u16(record->data + 2u),
      .depth = vkr_load_le_u32(record->data + 4u),
      .begin_ns = vkr_load_le_u64(record->data + 8u),
      .end_ns = vkr_load_le_u64(record->data + 16u),
      .frame = record->frame,
  };
  if (!array_push_RecZone(&ex->zones, zone)) {
    ex->out_of_memory = true_v;
  }
}

static void rec_export_passes(RecExport *ex, const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_PASSES_HEADER_SIZE) {
    ++ex->bad_records;
    return;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  if ((uint64_t)VKR_RECORDING_PASSES_HEADER_SIZE +
          (uint64_t)rows * VKR_RECORDING_PASS_ROW_SIZE !=
      record->size) {
    ++ex->bad_records;
    return;
  }
  if (!rec_export_room(ex, rows)) {
    return;
  }
  const uint64_t first = ex->pass_rows.length;
  if (!array_reserve_RecPassRow(&ex->pass_rows, first + rows)) {
    ex->out_of_memory = true_v;
    return;
  }
  for (uint32_t i = 0u; i < rows; ++i) {
    const uint8_t *at = record->data + VKR_RECORDING_PASSES_HEADER_SIZE +
                        (uint64_t)i * VKR_RECORDING_PASS_ROW_SIZE;
    ex->pass_rows.data[first + i] = (RecPassRow){
        .pass = vkr_load_le_u16(at),
        .flags = vkr_load_le_u16(at + 2u),
        .cpu_ms = vkr_load_le_f32(at + 4u),
        .gpu_ms = vkr_load_le_f32(at + 8u),
    };
  }
  ex->pass_rows.length += rows;
  const RecPassRecord pass_record = {
      .frame = record->frame,
      .first = first,
      .count = rows,
      .order = (uint32_t)ex->pass_records.length,
  };
  if (!array_push_RecPassRecord(&ex->pass_records, pass_record)) {
    ex->out_of_memory = true_v;
  }
}

static void rec_export_pass_starts(RecExport *ex,
                                   const VkrRecordingRecord *record) {
  if (record->size < REC_PASS_STARTS_HEADER_SIZE) {
    ++ex->bad_records;
    return;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  if ((uint64_t)REC_PASS_STARTS_HEADER_SIZE + (uint64_t)rows * 4u !=
      record->size) {
    ++ex->bad_records;
    return;
  }
  /* GPU times on another clock cannot sit beside the CPU zones. */
  if ((vkr_load_le_u32(record->data + 4u) & REC_PASS_STARTS_CPU_CLOCK) == 0u) {
    return;
  }
  if (!rec_export_room(ex, rows)) {
    return;
  }
  const uint64_t first = ex->start_ms.length;
  if (!array_reserve_float32_t(&ex->start_ms, first + rows)) {
    ex->out_of_memory = true_v;
    return;
  }
  for (uint32_t i = 0u; i < rows; ++i) {
    ex->start_ms.data[first + i] = vkr_load_le_f32(
        record->data + REC_PASS_STARTS_HEADER_SIZE + (uint64_t)i * 4u);
  }
  ex->start_ms.length += rows;
  const RecPassStarts starts = {
      .frame = record->frame,
      .gpu_start_ns = vkr_load_le_u64(record->data + 8u),
      .gpu_end_ns = vkr_load_le_u64(record->data + 16u),
      .first = first,
      .count = rows,
      .order = (uint32_t)ex->starts.length,
  };
  if (!array_push_RecPassStarts(&ex->starts, starts)) {
    ex->out_of_memory = true_v;
  }
}

static void rec_export_records(RecExport *ex, const uint8_t *payload,
                               uint64_t size) {
  uint64_t cursor = 0u;
  VkrRecordingRecord record;
  while (vkr_recording_records_next(payload, size, &cursor, &record)) {
    /* Names resolve zones of any frame, so they are read outside the
       range too. */
    if (record.stream == ex->zone_name_stream) {
      rec_export_zone_name(ex, &record);
      continue;
    }
    if (!rec_export_in_range(ex, record.frame)) {
      continue;
    }
    if (record.stream == ex->zone_stream) {
      rec_export_zone(ex, &record);
    } else if (record.stream == ex->passes_stream) {
      rec_export_passes(ex, &record);
    } else if (record.stream == ex->pass_starts_stream) {
      rec_export_pass_starts(ex, &record);
    }
  }
  if (cursor != size) {
    ++ex->bad_records;
  }
}

/* Reads the zone and pass streams; false when the stream cannot be read. */
static bool8_t rec_export_collect(const char *path, RecExport *ex) {
  VkrRecordingReader reader;
  const VkrRecordingReadError error = rec_open(path, &reader);
  if (error != VKR_RECORDING_READ_OK) {
    fprintf(stderr, "vkr_rec: cannot read '%s' (error %d)\n", path, (int)error);
    return false_v;
  }
  const VkrCodec *codec = vkr_depot_zstd_codec();
  VkrRecordingDictionary dictionary = {0};
  bool8_t has_dictionary = false_v;
  const uint32_t dictionary_index =
      vkr_recording_reader_find(&reader, VKR_RECORDING_CHUNK_DICTIONARY);
  uint8_t *payload = NULL;
  uint64_t size = 0u;
  if (dictionary_index != UINT32_MAX &&
      vkr_recording_reader_chunk(&reader, dictionary_index, codec,
                                 &rec_allocator, &payload, &size)) {
    has_dictionary = vkr_recording_dictionary_decode(
        payload, size, &rec_allocator, &dictionary);
    vkr_recording_reader_chunk_free(&rec_allocator, payload, size);
  }
  if (!has_dictionary) {
    fprintf(stderr, "vkr_rec: '%s' has no readable dictionary\n", path);
    vkr_recording_reader_close(&reader);
    return false_v;
  }
  ex->zone_name_stream = rec_find_stream_id(&dictionary, "zone_name");
  ex->zone_stream = rec_find_stream_id(&dictionary, "zone");
  ex->passes_stream = rec_find_stream_id(&dictionary, "passes");
  ex->pass_starts_stream = rec_find_stream_id(&dictionary, "pass_starts");
  vkr_recording_dictionary_free(&rec_allocator, &dictionary);

  const bool8_t wanted =
      ex->zone_stream != UINT32_MAX || ex->pass_starts_stream != UINT32_MAX;
  for (uint32_t i = 0u; wanted && i < reader.chunk_count; ++i) {
    if (reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    if (!vkr_recording_reader_chunk(&reader, i, codec, &rec_allocator, &payload,
                                    &size)) {
      ++ex->bad_chunks;
      continue;
    }
    rec_export_records(ex, payload, size);
    vkr_recording_reader_chunk_free(&rec_allocator, payload, size);
  }
  vkr_recording_reader_close(&reader);
  return true_v;
}

static int32_t rec_compare_zones(const void *lhs, const void *rhs) {
  const RecZone *a = lhs;
  const RecZone *b = rhs;
  if (a->begin_ns != b->begin_ns) {
    return a->begin_ns < b->begin_ns ? -1 : 1;
  }
  if (a->slot != b->slot) {
    return a->slot < b->slot ? -1 : 1;
  }
  if (a->depth != b->depth) {
    return a->depth < b->depth ? -1 : 1;
  }
  return 0;
}

static int32_t rec_compare_pass_records(const void *lhs, const void *rhs) {
  const RecPassRecord *a = lhs;
  const RecPassRecord *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

static int32_t rec_compare_pass_starts(const void *lhs, const void *rhs) {
  const RecPassStarts *a = lhs;
  const RecPassStarts *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

static int32_t rec_compare_frame_starts(const void *lhs, const void *rhs) {
  const RecFrameStart *a = lhs;
  const RecFrameStart *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  return a->begin_ns < b->begin_ns ? -1 : (a->begin_ns > b->begin_ns ? 1 : 0);
}

static int32_t rec_compare_frame_key(const void *key, const void *record) {
  const uint64_t frame = *(const uint64_t *)key;
  const RecFrameStart *start = record;
  return frame < start->frame ? -1 : (frame > start->frame ? 1 : 0);
}

static String8 rec_export_zone_label(const RecExport *ex, uint16_t zone,
                                     char *scratch, uint64_t scratch_size) {
  const RecZoneName *name = &ex->zone_names[zone];
  if (name->known && name->length > 0u) {
    return string8_create_from_cstr(ex->zone_name_text.data + name->offset,
                                    name->length);
  }
  string_format(scratch, scratch_size, "zone %u", (unsigned)zone);
  return rec_str(scratch);
}

/* Sorts what the collection read, finds where each frame starts and the
   earliest time the trace shows. */
static void rec_export_prepare(RecExport *ex) {
  vkr_sort(ex->zones.data, ex->zones.length, sizeof(RecZone),
           rec_compare_zones);
  vkr_sort(ex->pass_records.data, ex->pass_records.length,
           sizeof(RecPassRecord), rec_compare_pass_records);
  vkr_sort(ex->starts.data, ex->starts.length, sizeof(RecPassStarts),
           rec_compare_pass_starts);

  static const char frame_name[] = "Frame";
  const uint32_t frame_name_length = sizeof(frame_name) - 1u;
  for (uint64_t i = 0u; i < ex->zones.length; ++i) {
    const RecZone *zone = &ex->zones.data[i];
    ex->slot_used[zone->slot] = true_v;
    const RecZoneName *name = &ex->zone_names[zone->zone];
    if (zone->slot != 0u || !name->known || name->length != frame_name_length ||
        MemCompare(ex->zone_name_text.data + name->offset, frame_name,
                   frame_name_length) != 0) {
      continue;
    }
    const RecFrameStart start = {.frame = zone->frame,
                                 .begin_ns = zone->begin_ns};
    if (!array_push_RecFrameStart(&ex->frame_starts, start)) {
      ex->out_of_memory = true_v;
      break;
    }
  }
  /* The earliest Frame zone of a frame comes first; the lookup takes it. */
  vkr_sort(ex->frame_starts.data, ex->frame_starts.length,
           sizeof(RecFrameStart), rec_compare_frame_starts);
  uint64_t kept = 0u;
  for (uint64_t i = 0u; i < ex->frame_starts.length; ++i) {
    if (kept > 0u && ex->frame_starts.data[kept - 1u].frame ==
                         ex->frame_starts.data[i].frame) {
      continue;
    }
    ex->frame_starts.data[kept++] = ex->frame_starts.data[i];
  }
  ex->frame_starts.length = kept;

  ex->base_ns = UINT64_MAX;
  if (ex->zones.length > 0u) {
    ex->base_ns = ex->zones.data[0].begin_ns;
  }
  for (uint64_t i = 0u; i < ex->starts.length; ++i) {
    ex->base_ns = Min(ex->base_ns, ex->starts.data[i].gpu_start_ns);
  }
  if (ex->base_ns == UINT64_MAX) {
    ex->base_ns = 0u;
  }
}

/* A recording with neither zones nor GPU pass starts, such as a Tier 0
   harness recording, starts each valid frame at its recorder frame time
   instead, so its counters and markers still have times. The trace then
   counts from frame 0's time; nothing else in it reads the monotonic
   clock. */
static void rec_export_recorder_frame_times(RecExport *ex,
                                            const VkrRecordingData *data) {
  if (ex->frame_starts.length > 0u || ex->zones.length > 0u ||
      ex->starts.length > 0u || !data->times) {
    return;
  }
  for (uint64_t frame = ex->first_frame;
       frame <= ex->last_frame && frame < data->frame_count; ++frame) {
    if (!data->frame_valid[frame] || !(data->times[frame] >= 0.0)) {
      continue;
    }
    const RecFrameStart start = {
        .frame = frame,
        .begin_ns = (uint64_t)(data->times[frame] * 1.0e9 + 0.5),
    };
    if (!array_push_RecFrameStart(&ex->frame_starts, start)) {
      ex->out_of_memory = true_v;
      return;
    }
  }
  ex->recorder_clock = ex->frame_starts.length > 0u;
  ex->base_ns = 0u;
}

/* The frame's start time, or false for a frame without a Frame zone. */
static bool8_t rec_export_frame_time(const RecExport *ex, uint64_t frame,
                                     uint64_t *out_ns) {
  const RecFrameStart *start =
      vkr_binary_search(&frame, ex->frame_starts.data, ex->frame_starts.length,
                        sizeof(RecFrameStart), rec_compare_frame_key);
  if (!start) {
    return false_v;
  }
  *out_ns = start->begin_ns;
  return true_v;
}

/* Microseconds after the base with the nanosecond digits: 3 decimals. */
static void rec_trace_time(VkrJsonWriter *json, const RecExport *ex,
                           const char *key, uint64_t ns) {
  const uint64_t relative = ns > ex->base_ns ? ns - ex->base_ns : 0u;
  uint32_t digits = 1u;
  for (uint64_t whole = relative / 1000u; whole >= 10u; whole /= 10u) {
    ++digits;
  }
  vkr_json_writer_name(json, rec_str(key));
  vkr_json_writer_f64_digits(json, (float64_t)relative / 1000.0, digits + 3u);
}

static void rec_trace_duration(VkrJsonWriter *json, uint64_t ns) {
  uint32_t digits = 1u;
  for (uint64_t whole = ns / 1000u; whole >= 10u; whole /= 10u) {
    ++digits;
  }
  vkr_json_writer_name(json, rec_str("dur"));
  vkr_json_writer_f64_digits(json, (float64_t)ns / 1000.0, digits + 3u);
}

/* Opens an event object with its name, phase and ids; false once the trace
   holds REC_EXPORT_EVENT_MAX events. The caller closes the object. */
static bool8_t rec_trace_begin(VkrJsonWriter *json, RecExport *ex, String8 name,
                               const char *phase, uint32_t pid, uint32_t tid) {
  if (ex->events >= REC_EXPORT_EVENT_MAX) {
    ex->capped = true_v;
    return false_v;
  }
  ++ex->events;
  vkr_json_writer_begin_object(json);
  vkr_json_writer_name(json, rec_str("name"));
  vkr_json_writer_string(json, name);
  vkr_json_writer_name(json, rec_str("ph"));
  vkr_json_writer_string(json, rec_str(phase));
  vkr_json_writer_name(json, rec_str("pid"));
  vkr_json_writer_u64(json, pid);
  vkr_json_writer_name(json, rec_str("tid"));
  vkr_json_writer_u64(json, tid);
  return true_v;
}

/* A metadata event: process_name, thread_name or a sort index. */
static void rec_trace_metadata(VkrJsonWriter *json, RecExport *ex,
                               const char *kind, uint32_t pid, uint32_t tid,
                               const char *name, int64_t sort_index) {
  if (!rec_trace_begin(json, ex, rec_str(kind), "M", pid, tid)) {
    return;
  }
  vkr_json_writer_name(json, rec_str("args"));
  vkr_json_writer_begin_object(json);
  if (name) {
    vkr_json_writer_name(json, rec_str("name"));
    vkr_json_writer_string(json, rec_str(name));
  } else {
    vkr_json_writer_name(json, rec_str("sort_index"));
    vkr_json_writer_i64(json, sort_index);
  }
  vkr_json_writer_end_object(json);
  vkr_json_writer_end_object(json);
}

static void rec_trace_write_names(VkrJsonWriter *json, RecExport *ex) {
  rec_trace_metadata(json, ex, "process_name", REC_TRACE_CPU_PID, 0u, "CPU", 0);
  rec_trace_metadata(json, ex, "process_sort_index", REC_TRACE_CPU_PID, 0u,
                     NULL, 0);
  for (uint32_t slot = 0u; slot < REC_ZONE_ID_COUNT; ++slot) {
    if (!ex->slot_used[slot]) {
      continue;
    }
    char name[32];
    if (slot == 0u) {
      string_format(name, sizeof(name), "Main");
    } else if (slot == 1u) {
      string_format(name, sizeof(name), "Render");
    } else {
      string_format(name, sizeof(name), "Worker %u", slot - 2u);
    }
    rec_trace_metadata(json, ex, "thread_name", REC_TRACE_CPU_PID, slot, name,
                       0);
    rec_trace_metadata(json, ex, "thread_sort_index", REC_TRACE_CPU_PID, slot,
                       NULL, slot);
  }
  if (ex->starts.length == 0u) {
    return;
  }
  rec_trace_metadata(json, ex, "process_name", REC_TRACE_GPU_PID, 0u, "GPU", 0);
  rec_trace_metadata(json, ex, "process_sort_index", REC_TRACE_GPU_PID, 0u,
                     NULL, 1);
  rec_trace_metadata(json, ex, "thread_name", REC_TRACE_GPU_PID,
                     REC_TRACE_GPU_QUEUE_TID, "Queue", 0);
  rec_trace_metadata(json, ex, "thread_name", REC_TRACE_GPU_PID,
                     REC_TRACE_GPU_FRAMES_TID, "Frames", 0);
}

static void rec_trace_write_zones(VkrJsonWriter *json, RecExport *ex) {
  for (uint64_t i = 0u; i < ex->zones.length; ++i) {
    const RecZone *zone = &ex->zones.data[i];
    char scratch[32];
    const String8 name =
        rec_export_zone_label(ex, zone->zone, scratch, sizeof(scratch));
    if (!rec_trace_begin(json, ex, name, "X", REC_TRACE_CPU_PID, zone->slot)) {
      return;
    }
    vkr_json_writer_name(json, rec_str("cat"));
    vkr_json_writer_string(json, rec_str("zone"));
    rec_trace_time(json, ex, "ts", zone->begin_ns);
    rec_trace_duration(json, zone->end_ns > zone->begin_ns
                                 ? zone->end_ns - zone->begin_ns
                                 : 0u);
    vkr_json_writer_name(json, rec_str("args"));
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("frame"));
    vkr_json_writer_u64(json, zone->frame);
    vkr_json_writer_name(json, rec_str("depth"));
    vkr_json_writer_u64(json, zone->depth);
    vkr_json_writer_end_object(json);
    vkr_json_writer_end_object(json);
    ++ex->zone_events;
  }
}

static String8 rec_export_pass_label(const VkrRecordingData *data,
                                     uint16_t pass, char *scratch,
                                     uint64_t scratch_size) {
  if (pass < data->pass_count && data->passes[pass].name[0] != '\0') {
    return rec_str(data->passes[pass].name);
  }
  string_format(scratch, scratch_size, "pass %u", (unsigned)pass);
  return rec_str(scratch);
}

/* The timed passes of one frame on the Queue thread. */
static void rec_trace_write_frame_passes(VkrJsonWriter *json, RecExport *ex,
                                         const VkrRecordingData *data,
                                         const RecPassStarts *starts,
                                         const RecPassRecord *record) {
  for (uint32_t i = 0u; i < record->count; ++i) {
    const RecPassRow *row = &ex->pass_rows.data[record->first + i];
    const float32_t start_ms = ex->start_ms.data[starts->first + i];
    if ((row->flags & VKR_RECORDING_PASS_GPU_VALID) == 0u ||
        !vkr_is_finite_f32(start_ms) || !vkr_is_finite_f32(row->gpu_ms)) {
      continue;
    }
    const int64_t offset_ns = (int64_t)((float64_t)start_ms * 1.0e6);
    uint64_t begin_ns = starts->gpu_start_ns;
    if (offset_ns >= 0) {
      begin_ns += (uint64_t)offset_ns;
    } else {
      begin_ns -= Min(begin_ns, (uint64_t)(-offset_ns));
    }
    const float64_t gpu_ns = Max((float64_t)row->gpu_ms * 1.0e6, 0.0);
    char scratch[32];
    const String8 name =
        rec_export_pass_label(data, row->pass, scratch, sizeof(scratch));
    if (!rec_trace_begin(json, ex, name, "X", REC_TRACE_GPU_PID,
                         REC_TRACE_GPU_QUEUE_TID)) {
      return;
    }
    vkr_json_writer_name(json, rec_str("cat"));
    vkr_json_writer_string(json, rec_str("gpu"));
    rec_trace_time(json, ex, "ts", begin_ns);
    rec_trace_duration(json, (uint64_t)(gpu_ns + 0.5));
    vkr_json_writer_name(json, rec_str("args"));
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("frame"));
    vkr_json_writer_u64(json, starts->frame);
    vkr_json_writer_name(json, rec_str("cpu_ms"));
    vkr_json_writer_f64_digits(json, row->cpu_ms, 6u);
    vkr_json_writer_end_object(json);
    vkr_json_writer_end_object(json);
    ++ex->gpu_passes;
  }
}

/* Per pass_starts record: the frame's GPU span, then its passes when the
   frame's first passes record has as many rows. */
static void rec_trace_write_gpu(VkrJsonWriter *json, RecExport *ex,
                                const VkrRecordingData *data) {
  uint64_t r = 0u;
  for (uint64_t s = 0u; s < ex->starts.length; ++s) {
    const RecPassStarts *starts = &ex->starts.data[s];
    if (s > 0u && ex->starts.data[s - 1u].frame == starts->frame) {
      continue;
    }
    char name[48];
    string_format(name, sizeof(name), "GPU frame %llu",
                  (unsigned long long)starts->frame);
    if (!rec_trace_begin(json, ex, rec_str(name), "X", REC_TRACE_GPU_PID,
                         REC_TRACE_GPU_FRAMES_TID)) {
      return;
    }
    vkr_json_writer_name(json, rec_str("cat"));
    vkr_json_writer_string(json, rec_str("gpu"));
    rec_trace_time(json, ex, "ts", starts->gpu_start_ns);
    rec_trace_duration(json, starts->gpu_end_ns > starts->gpu_start_ns
                                 ? starts->gpu_end_ns - starts->gpu_start_ns
                                 : 0u);
    vkr_json_writer_name(json, rec_str("args"));
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("frame"));
    vkr_json_writer_u64(json, starts->frame);
    vkr_json_writer_end_object(json);
    vkr_json_writer_end_object(json);
    ++ex->gpu_frames;

    while (r < ex->pass_records.length &&
           ex->pass_records.data[r].frame < starts->frame) {
      ++r;
    }
    if (r == ex->pass_records.length ||
        ex->pass_records.data[r].frame != starts->frame ||
        ex->pass_records.data[r].count != starts->count) {
      ++ex->gpu_frames_unmatched;
      continue;
    }
    rec_trace_write_frame_passes(json, ex, data, starts,
                                 &ex->pass_records.data[r]);
  }
}

static void rec_trace_write_counters(VkrJsonWriter *json, RecExport *ex,
                                     const VkrRecordingData *data) {
  uint32_t series[ArrayCount(REC_TRACE_COUNTERS)];
  for (uint32_t c = 0u; c < ArrayCount(REC_TRACE_COUNTERS); ++c) {
    series[c] =
        vkr_recording_data_find_series(data, REC_TRACE_COUNTERS[c].series);
  }
  for (uint64_t i = 0u; i < ex->frame_starts.length; ++i) {
    const RecFrameStart *start = &ex->frame_starts.data[i];
    if (start->frame >= data->frame_count) {
      continue;
    }
    for (uint32_t c = 0u; c < ArrayCount(REC_TRACE_COUNTERS); ++c) {
      if (series[c] == UINT32_MAX) {
        continue;
      }
      const VkrRecordingSeries *values = &data->series[series[c]];
      if (!values->valid[start->frame]) {
        continue;
      }
      if (!rec_trace_begin(json, ex, rec_str(REC_TRACE_COUNTERS[c].label), "C",
                           REC_TRACE_CPU_PID, 0u)) {
        return;
      }
      rec_trace_time(json, ex, "ts", start->begin_ns);
      vkr_json_writer_name(json, rec_str("args"));
      vkr_json_writer_begin_object(json);
      vkr_json_writer_name(json, rec_str("value"));
      vkr_json_writer_f64_digits(
          json, values->values[start->frame] * REC_TRACE_COUNTERS[c].scale, 9u);
      vkr_json_writer_end_object(json);
      vkr_json_writer_end_object(json);
      ++ex->counter_events;
    }
  }
}

static void rec_trace_write_markers(VkrJsonWriter *json, RecExport *ex,
                                    const VkrRecordingData *data) {
  for (uint32_t i = 0u; i < data->marker_count; ++i) {
    const VkrRecordingDataMarker *marker = &data->markers[i];
    if (!rec_export_in_range(ex, marker->frame)) {
      continue;
    }
    uint64_t ns = 0u;
    if (!rec_export_frame_time(ex, marker->frame, &ns)) {
      ++ex->markers_untimed;
      continue;
    }
    char name[REC_EVENT_NAME_MAX];
    const char *kind = vkr_recording_marker_kind_name(marker->kind);
    if (marker->label[0] != '\0') {
      string_format(name, sizeof(name), "%s: %s", kind, marker->label);
    } else {
      string_format(name, sizeof(name), "%s", kind);
    }
    if (!rec_trace_begin(json, ex, rec_str(name), "i", REC_TRACE_CPU_PID, 0u)) {
      return;
    }
    vkr_json_writer_name(json, rec_str("s"));
    vkr_json_writer_string(json, rec_str("g"));
    rec_trace_time(json, ex, "ts", ns);
    vkr_json_writer_name(json, rec_str("args"));
    vkr_json_writer_begin_object(json);
    vkr_json_writer_name(json, rec_str("frame"));
    vkr_json_writer_u64(json, marker->frame);
    vkr_json_writer_end_object(json);
    vkr_json_writer_end_object(json);
    ++ex->marker_events;
  }
}

/* The trace file: {"traceEvents": [...], "displayTimeUnit": "ms",
   "otherData": {...}}; false when it cannot be written. */
static bool8_t rec_trace_write(const char *out_path, const char *path,
                               RecExport *ex, const VkrRecordingData *data) {
  static VkrJsonFileWriter file;
  if (!vkr_json_file_writer_begin(&file, rec_str(out_path))) {
    return false_v;
  }
  VkrJsonWriter *json = &file.writer;
  vkr_json_writer_begin_object(json);
  vkr_json_writer_name(json, rec_str("traceEvents"));
  vkr_json_writer_begin_array(json);
  rec_trace_write_names(json, ex);
  rec_trace_write_zones(json, ex);
  rec_trace_write_gpu(json, ex, data);
  rec_trace_write_counters(json, ex, data);
  rec_trace_write_markers(json, ex, data);
  vkr_json_writer_end_array(json);
  vkr_json_writer_name(json, rec_str("displayTimeUnit"));
  vkr_json_writer_string(json, rec_str("ms"));
  vkr_json_writer_name(json, rec_str("otherData"));
  vkr_json_writer_begin_object(json);
  vkr_json_writer_name(json, rec_str("source"));
  vkr_json_writer_string(json, rec_str(path));
  vkr_json_writer_name(json, rec_str("time_base_ns"));
  vkr_json_writer_u64(json, ex->base_ns);
  vkr_json_writer_name(json, rec_str("first_frame"));
  vkr_json_writer_u64(json, ex->first_frame);
  vkr_json_writer_name(json, rec_str("last_frame"));
  vkr_json_writer_u64(json,
                      Min(ex->last_frame, Max(data->frame_count, 1u) - 1u));
  vkr_json_writer_name(json, rec_str("capped"));
  vkr_json_writer_bool(json, ex->capped);
  vkr_json_writer_end_object(json);
  vkr_json_writer_end_object(json);
  return vkr_json_file_writer_commit(&file);
}

static void rec_export_free(RecExport *ex) {
  array_destroy_uint8_t(&ex->zone_name_text);
  array_destroy_RecZone(&ex->zones);
  array_destroy_RecPassRecord(&ex->pass_records);
  array_destroy_RecPassRow(&ex->pass_rows);
  array_destroy_RecPassStarts(&ex->starts);
  array_destroy_float32_t(&ex->start_ms);
  array_destroy_RecFrameStart(&ex->frame_starts);
}

static int rec_export(const char *path, const char *out_path, uint64_t first,
                      uint64_t last) {
  VkrRecordingData data;
  if (!rec_load(path, &data)) {
    return REC_EXIT_DAMAGED;
  }
  RecExport *ex = &rec_export_state;
  MemZero(ex, sizeof(*ex));
  ex->first_frame = first;
  ex->last_frame = last;
  ex->zone_name_text = (Array_uint8_t){.allocator = &rec_allocator};
  ex->zones = (Array_RecZone){.allocator = &rec_allocator};
  ex->pass_records = (Array_RecPassRecord){.allocator = &rec_allocator};
  ex->pass_rows = (Array_RecPassRow){.allocator = &rec_allocator};
  ex->starts = (Array_RecPassStarts){.allocator = &rec_allocator};
  ex->start_ms = (Array_float32_t){.allocator = &rec_allocator};
  ex->frame_starts = (Array_RecFrameStart){.allocator = &rec_allocator};

  int result = REC_EXIT_DAMAGED;
  if (!rec_export_collect(path, ex)) {
    goto cleanup;
  }
  rec_export_prepare(ex);
  rec_export_recorder_frame_times(ex, &data);
  if (ex->out_of_memory) {
    fprintf(stderr, "vkr_rec: out of memory exporting '%s'\n", path);
    goto cleanup;
  }
  if (!rec_trace_write(out_path, path, ex, &data)) {
    fprintf(stderr, "vkr_rec: cannot write '%s'\n", out_path);
    goto cleanup;
  }

  RecExit status = rec_data_status(&data);
  if (ex->bad_chunks > 0u || ex->bad_records > 0u) {
    status = REC_EXIT_DAMAGED;
  } else if (ex->capped) {
    status = REC_EXIT_TRUNCATED;
  }
  VkrJsonWriter json;
  vkr_json_writer_init(&json, rec_stdout_sink, NULL);
  vkr_json_writer_begin_object(&json);
  vkr_json_writer_name(&json, rec_str("status"));
  vkr_json_writer_string(&json, rec_str(rec_status_name(status)));
  vkr_json_writer_name(&json, rec_str("events"));
  vkr_json_writer_u64(&json, ex->events);
  vkr_json_writer_name(&json, rec_str("path"));
  vkr_json_writer_string(&json, rec_str(out_path));
  vkr_json_writer_name(&json, rec_str("capped"));
  vkr_json_writer_bool(&json, ex->capped);
  vkr_json_writer_name(&json, rec_str("zones"));
  vkr_json_writer_u64(&json, ex->zone_events);
  vkr_json_writer_name(&json, rec_str("gpu_frames"));
  vkr_json_writer_u64(&json, ex->gpu_frames);
  vkr_json_writer_name(&json, rec_str("gpu_passes"));
  vkr_json_writer_u64(&json, ex->gpu_passes);
  vkr_json_writer_name(&json, rec_str("gpu_frames_unmatched"));
  vkr_json_writer_u64(&json, ex->gpu_frames_unmatched);
  vkr_json_writer_name(&json, rec_str("counters"));
  vkr_json_writer_u64(&json, ex->counter_events);
  vkr_json_writer_name(&json, rec_str("markers"));
  vkr_json_writer_u64(&json, ex->marker_events);
  vkr_json_writer_name(&json, rec_str("markers_untimed"));
  vkr_json_writer_u64(&json, ex->markers_untimed);
  vkr_json_writer_name(&json, rec_str("frame_clock"));
  vkr_json_writer_string(&json,
                         rec_str(ex->recorder_clock ? "recorder" : "zones"));
  vkr_json_writer_name(&json, rec_str("bad_records"));
  vkr_json_writer_u64(&json, ex->bad_records);
  vkr_json_writer_end_object(&json);
  vkr_json_writer_complete(&json);
  fputc('\n', stdout);
  result = status;

cleanup:
  rec_export_free(ex);
  vkr_recording_data_free(&data);
  return result;
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
      "       vkr_rec explain <bundle|stream> (--spike N | --frame F) "
      "[--budget MS]\n"
      "       vkr_rec frames <bundle|stream> --range A:B "
      "[--fields name,name]\n"
      "       vkr_rec passes <bundle|stream> [--top N] [--frame F]\n"
      "       vkr_rec compare <a> <b> [--align time|marker|distance|frame]"
      " [--marker label]\n"
      "       vkr_rec export <bundle|stream> --perfetto <out.json> "
      "[--range A:B]\n"
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

static int rec_run_explain(int argc, char **argv) {
  uint32_t spike = 0u;
  uint64_t frame = UINT64_MAX;
  float64_t budget_ms = 0.0;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
    if (strcmp(argv[i], "--spike") == 0) {
      if (!string_to_u32(argv[i + 1], &spike) || spike == 0u ||
          spike > REC_TOP_MAX) {
        fprintf(stderr, "vkr_rec: --spike takes a rank from 1 to %u\n",
                REC_TOP_MAX);
        return REC_EXIT_DAMAGED;
      }
    } else if (strcmp(argv[i], "--frame") == 0) {
      if (!string_to_u64(argv[i + 1], &frame) || frame == UINT64_MAX) {
        fprintf(stderr, "vkr_rec: --frame takes a frame number\n");
        return REC_EXIT_DAMAGED;
      }
    } else if (strcmp(argv[i], "--budget") == 0) {
      /* The pair alone, wherever it stands among the options. */
      if (!rec_parse_budget(i + 2, argv, i, &budget_ms)) {
        return REC_EXIT_DAMAGED;
      }
    } else {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
  }
  if ((spike > 0u) == (frame != UINT64_MAX)) {
    fprintf(stderr, "vkr_rec: explain takes --spike N or --frame F\n");
    return REC_EXIT_DAMAGED;
  }
  return rec_explain(argv[2], spike, frame, budget_ms);
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

static int rec_run_export(int argc, char **argv) {
  const char *out_path = NULL;
  uint64_t first = 0u;
  uint64_t last = UINT64_MAX;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
    if (strcmp(argv[i], "--perfetto") == 0) {
      out_path = argv[i + 1];
    } else if (strcmp(argv[i], "--range") == 0) {
      if (!rec_parse_range(argv[i + 1], &first, &last)) {
        fprintf(stderr, "vkr_rec: --range takes A:B with A <= B\n");
        return REC_EXIT_DAMAGED;
      }
    } else {
      rec_usage();
      return REC_EXIT_DAMAGED;
    }
  }
  if (!out_path || out_path[0] == '\0') {
    rec_usage();
    return REC_EXIT_DAMAGED;
  }
  return rec_export(argv[2], out_path, first, last);
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
  } else if (strcmp(argv[1], "explain") == 0) {
    result = rec_run_explain(argc, argv);
  } else if (strcmp(argv[1], "frames") == 0) {
    result = rec_run_frames(argc, argv);
  } else if (strcmp(argv[1], "passes") == 0) {
    result = rec_run_passes(argc, argv);
  } else if (strcmp(argv[1], "compare") == 0) {
    result = rec_run_compare(argc, argv);
  } else if (strcmp(argv[1], "export") == 0) {
    result = rec_run_export(argc, argv);
  } else {
    rec_usage();
  }

  vkr_dmemory_allocator_destroy(&rec_allocator);
  vkr_platform_shutdown();
  return result;
}
