/*
 * vkr_rec: reads a recording without the editor or the GPU
 * (docs/proposals/recordings.md, "Offline command").
 *
 *   vkr_rec summary <bundle|stream>   JSON: identity, streams, counters
 *   vkr_rec verify <bundle|stream>    decodes every chunk and record
 *
 * Exit status: 0 for a whole recording, 2 for a recording cut off by a crash
 * whose whole chunks are valid, 1 for an unreadable or damaged recording.
 */
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

static int rec_report(const char *path, bool8_t verbose) {
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

static void rec_usage(void) {
  fprintf(stderr, "usage: vkr_rec summary <bundle|stream>\n"
                  "       vkr_rec verify <bundle|stream>\n");
}

VKR_MAIN(argc, argv) {
  if (argc != 3) {
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
  if (strcmp(argv[1], "summary") == 0) {
    result = rec_report(argv[2], true_v);
  } else if (strcmp(argv[1], "verify") == 0) {
    result = rec_report(argv[2], false_v);
  } else {
    rec_usage();
  }

  vkr_dmemory_allocator_destroy(&rec_allocator);
  vkr_platform_shutdown();
  return result;
}
