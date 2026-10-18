#pragma once

#include "core/vkr_codec.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_allocator.h"

/*
 * Recording stream file (docs/proposals/recordings.md, "File format").
 *
 * A stream file is a fixed header followed by chunks and, after a clean stop,
 * an index chunk and a trailer. Every chunk header and payload carries a
 * CRC-32, so a reader keeps every whole chunk of a file that a crash cut off.
 * All integers are little-endian.
 *
 *   header   magic "VKRRECST", version, reserved           32 bytes
 *   chunk    header (48 bytes) + stored payload            repeated
 *   index    chunk of type INDEX listing every chunk       clean stop only
 *   trailer  magic "VRIX", reserved, index chunk offset    16 bytes
 *
 * RECORDS payloads hold records in the layout producers write them:
 *
 *   u16 stream, u16 flags, u32 size, u64 frame, payload, zero padding to 8
 *
 * The DICTIONARY chunk names every stream and the fields of its records, so a
 * reader decodes streams it was not built with.
 */

#define VKR_RECORDING_FORMAT_VERSION 1u
#define VKR_RECORDING_FILE_HEADER_SIZE 32u
#define VKR_RECORDING_CHUNK_HEADER_SIZE 48u
#define VKR_RECORDING_TRAILER_SIZE 16u
#define VKR_RECORDING_RECORD_HEADER_SIZE 16u
#define VKR_RECORDING_INDEX_ENTRY_SIZE 32u
#define VKR_RECORDING_END_SIZE 80u
/* Largest decoded chunk payload a reader accepts. */
#define VKR_RECORDING_CHUNK_MAX (64u * 1024u * 1024u)
#define VKR_RECORDING_NAME_MAX 63u
#define VKR_RECORDING_FIELD_MAX 64u
#define VKR_RECORDING_STREAM_MAX 256u

typedef enum VkrRecordingChunkType {
  /* UTF-8 JSON: build, scene, backend and settings at record start. */
  VKR_RECORDING_CHUNK_IDENTITY = 1u,
  VKR_RECORDING_CHUNK_DICTIONARY = 2u,
  VKR_RECORDING_CHUNK_RECORDS = 3u,
  /* Final counters; present only after a clean stop. */
  VKR_RECORDING_CHUNK_END = 4u,
  VKR_RECORDING_CHUNK_INDEX = 5u,
} VkrRecordingChunkType;

typedef enum VkrRecordingCodecId {
  VKR_RECORDING_CODEC_RAW = 0u,
  VKR_RECORDING_CODEC_ZSTD = 1u,
} VkrRecordingCodecId;

typedef enum VkrRecordingFieldType {
  VKR_RECORDING_FIELD_U8 = 1u,
  VKR_RECORDING_FIELD_U16,
  VKR_RECORDING_FIELD_U32,
  VKR_RECORDING_FIELD_U64,
  VKR_RECORDING_FIELD_I32,
  VKR_RECORDING_FIELD_I64,
  VKR_RECORDING_FIELD_F32,
  VKR_RECORDING_FIELD_F64,
} VkrRecordingFieldType;

/* A stream whose records may be dropped when the recorder falls behind,
   such as metrics; other streams record a gap instead. */
#define VKR_RECORDING_STREAM_DROPPABLE 1u

/* Stream 0 belongs to the recorder: gaps and its own counters. */
#define VKR_RECORDING_STREAM_RECORDER 0u

/* Marker records (stream "marker" of runtime recordings) are a u32 kind
   followed by a UTF-8 label. */
typedef enum VkrRecordingMarkerKind {
  VKR_RECORDING_MARKER_NOTE = 1u,
  VKR_RECORDING_MARKER_BUG = 2u,
  VKR_RECORDING_MARKER_STEP = 3u,
  /* The scene was edited at this frame. Playback shows the scene as it is
     now, not as it was (edit replay comes with the world state stream). */
  VKR_RECORDING_MARKER_EDIT = 4u,
} VkrRecordingMarkerKind;

/** "note", "bug", "step", "edit" or "unknown". */
const char *vkr_recording_marker_kind_name(uint32_t kind);

/*
 * Engine events (stream "event" of runtime recordings, beside the events
 * scripts write with vkr_rec_event): a record named with the "vkr." prefix,
 * which scripts must not use, whose payload is a VkrRecordingEngineEvent.
 * The renderer's events come from the frame's published metrics: pipeline
 * builds, shader library loads and asset loads with their time, and draws
 * skipped while their pipeline compiles (pipelines.late) with none.
 */
#define VKR_RECORDING_ENGINE_EVENT_PREFIX "vkr."
#define VKR_RECORDING_EVENT_PIPELINE "vkr.pipeline"
#define VKR_RECORDING_EVENT_SHADER "vkr.shader"
#define VKR_RECORDING_EVENT_PIPELINE_LATE "vkr.pipeline_late"
#define VKR_RECORDING_EVENT_TEXTURE "vkr.texture"
#define VKR_RECORDING_EVENT_MESH "vkr.mesh"
#define VKR_RECORDING_EVENT_MATERIAL "vkr.material"
#define VKR_RECORDING_EVENT_SCENE "vkr.scene"
#define VKR_RECORDING_EVENT_CELL_LOAD "vkr.cell_load"
#define VKR_RECORDING_EVENT_CELL_UNLOAD "vkr.cell_unload"
#define VKR_RECORDING_EVENT_SCRIPT_RELOAD "vkr.script_reload"

/* How many happened in the frame and the milliseconds they took, zero when
   not measured. */
typedef struct VkrRecordingEngineEvent {
  uint32_t count;
  float32_t ms;
} VkrRecordingEngineEvent;

/* An event record read in place: its name, borrowed from the record, and
   for an engine event its count and milliseconds; a script's event counts
   one and has no time. */
typedef struct VkrRecordingEventView {
  const uint8_t *name;
  uint32_t name_length;
  bool8_t engine;
  uint32_t count;
  float32_t ms;
} VkrRecordingEventView;

/** Reads an event record's payload; false for a malformed one. */
bool8_t vkr_recording_event_parse(const uint8_t *data, uint32_t size,
                                  VkrRecordingEventView *out);

/*
 * Render pass rows (streams "pass_name" and "passes" of runtime recordings
 * with Tier 1 metrics). A pass_name record is a u32 pass id then the UTF-8
 * name; it comes before the first passes record that uses the id. A passes
 * record holds the executed passes of one rendered frame, under that frame's
 * number: a u32 row count, u32 VKR_RECORDING_PASSES_* flags, then per row a
 * u16 pass id, u16 VKR_RECORDING_PASS_* flags, f32 CPU ms and f32 GPU ms.
 */
/* All measurements (the recorder's measurement_name and measurements
 * streams): series a recording may name, and values one record holds. */
#define VKR_RECORDING_MEASUREMENT_MAX 1024u
#define VKR_RECORDING_MEASUREMENT_PAGE 64u
#define VKR_RECORDING_PASS_NAME_MAX 63u
#define VKR_RECORDING_PASS_MAX 512u
#define VKR_RECORDING_PASSES_HEADER_SIZE 8u
#define VKR_RECORDING_PASS_ROW_SIZE 12u
/* The frame took per-pass GPU timestamps. */
#define VKR_RECORDING_PASSES_GPU_TIMED 1u
/* The row's GPU time is a measurement. */
#define VKR_RECORDING_PASS_GPU_VALID 1u

typedef struct VkrRecordingField {
  const char *name;
  /* Empty or a unit such as "ms", "bytes", "m". */
  const char *unit;
  VkrRecordingFieldType type;
  /* Elements of `type`; 3 for a position. */
  uint16_t count;
  /* Byte offset in the record payload. */
  uint32_t offset;
} VkrRecordingField;

typedef struct VkrRecordingStream {
  uint16_t id;
  uint16_t flags;
  const char *name;
  /* Payload size of every record of this stream; zero for a stream of
     variable-size opaque records, such as UTF-8 text, with no fields. */
  uint32_t record_size;
  uint32_t field_count;
  const VkrRecordingField *fields;
} VkrRecordingStream;

/* A decoded dictionary. Streams, fields and names share one allocation that
   vkr_recording_dictionary_free releases. */
typedef struct VkrRecordingDictionary {
  VkrRecordingStream *streams;
  uint32_t stream_count;
  void *block;
  uint64_t block_bytes;
} VkrRecordingDictionary;

typedef struct VkrRecordingEnd {
  uint64_t first_frame;
  uint64_t last_frame;
  uint64_t records;
  /* Droppable records the recorder discarded when its ring was full. */
  uint64_t dropped_records;
  /* Other records lost to a full ring; each loss also wrote a gap record. */
  uint64_t lost_records;
  uint64_t raw_bytes;
  uint64_t stored_bytes;
  uint64_t chunks;
  float64_t seconds;
  /* Writer thread time spent compressing and writing. */
  float64_t writer_seconds;
} VkrRecordingEnd;

typedef struct VkrRecordingRecord {
  uint16_t stream;
  uint16_t flags;
  uint32_t size;
  uint64_t frame;
  const uint8_t *data;
} VkrRecordingRecord;

/* Writes a record's 16-byte header in front of its payload. */
void vkr_recording_record_header_store(uint8_t *header, uint16_t stream,
                                       uint16_t flags, uint32_t size,
                                       uint64_t frame);

/** Bytes one record occupies in a RECORDS payload, padding included. */
uint64_t vkr_recording_record_bytes(uint32_t payload_size);

/**
 * @brief Reads the record at `*cursor` and advances past it.
 * @return false_v at the end of the payload or on a malformed record; a
 * malformed record also sets `*cursor` to `size`.
 */
bool8_t vkr_recording_records_next(const uint8_t *payload, uint64_t size,
                                   uint64_t *cursor,
                                   VkrRecordingRecord *out_record);

/**
 * @brief Encodes stream descriptions for a DICTIONARY chunk.
 * @return false_v when a description is invalid or the allocation fails.
 */
bool8_t vkr_recording_dictionary_encode(const VkrRecordingStream *streams,
                                        uint32_t stream_count,
                                        VkrAllocator *allocator,
                                        uint8_t **out_data, uint64_t *out_size);

bool8_t vkr_recording_dictionary_decode(const uint8_t *data, uint64_t size,
                                        VkrAllocator *allocator,
                                        VkrRecordingDictionary *out);

void vkr_recording_dictionary_free(VkrAllocator *allocator,
                                   VkrRecordingDictionary *dictionary);

/** The stream with `id`, or NULL. */
const VkrRecordingStream *
vkr_recording_dictionary_find(const VkrRecordingDictionary *dictionary,
                              uint16_t id);

void vkr_recording_end_encode(const VkrRecordingEnd *end,
                              uint8_t out[VKR_RECORDING_END_SIZE]);
bool8_t vkr_recording_end_decode(const uint8_t *data, uint64_t size,
                                 VkrRecordingEnd *out);

/** Bytes of one element of `type`. */
uint32_t vkr_recording_field_type_size(VkrRecordingFieldType type);
const char *vkr_recording_field_type_name(VkrRecordingFieldType type);

/* ------------------------------------------------------------------------ */
/* Writer: one thread, append-only.                                          */

typedef struct VkrRecordingIndexEntry {
  uint64_t offset;
  uint64_t first_frame;
  uint64_t last_frame;
  uint32_t type;
  uint32_t raw_size;
} VkrRecordingIndexEntry;

typedef struct VkrRecordingWriter {
  VkrAllocator *allocator;
  const VkrCodec *codec;
  int32_t level;
  FilePath path;
  FileHandle file;
  uint64_t offset;
  uint8_t *scratch;
  uint64_t scratch_capacity;
  VkrRecordingIndexEntry *index;
  uint32_t index_count;
  uint32_t index_capacity;
  uint64_t raw_bytes;
  uint64_t stored_bytes;
  bool8_t open;
  bool8_t failed;
} VkrRecordingWriter;

/**
 * @brief Creates `path` and writes the file header.
 * @param writer Must not move while open: its file handle points at its path.
 * @param path Must stay valid until the writer closes.
 * @param codec NULL stores every chunk raw.
 */
bool8_t vkr_recording_writer_open(VkrRecordingWriter *writer,
                                  VkrAllocator *allocator, const char *path,
                                  const VkrCodec *codec);

/** Appends one chunk; compressed when the codec makes it smaller. */
bool8_t vkr_recording_writer_chunk(VkrRecordingWriter *writer,
                                   VkrRecordingChunkType type,
                                   uint64_t first_frame, uint64_t last_frame,
                                   const void *payload, uint64_t size);

/**
 * @brief Writes the index and the trailer, syncs and closes the file.
 * A writer that failed earlier closes without them.
 */
bool8_t vkr_recording_writer_close(VkrRecordingWriter *writer);

/** Closes the file as a crash would: no index, no trailer, no sync. */
void vkr_recording_writer_abandon(VkrRecordingWriter *writer);

/* ------------------------------------------------------------------------ */
/* Reader                                                                    */

typedef struct VkrRecordingChunkInfo {
  uint64_t offset;
  uint64_t first_frame;
  uint64_t last_frame;
  uint32_t type;
  uint32_t codec;
  uint32_t raw_size;
  uint32_t stored_size;
  uint32_t payload_crc;
} VkrRecordingChunkInfo;

typedef enum VkrRecordingReadError {
  VKR_RECORDING_READ_OK = 0,
  VKR_RECORDING_READ_OPEN,
  VKR_RECORDING_READ_HEADER,
  VKR_RECORDING_READ_VERSION,
  VKR_RECORDING_READ_MEMORY,
} VkrRecordingReadError;

typedef struct VkrRecordingReader {
  VkrAllocator *allocator;
  FileMapping mapping;
  const uint8_t *data;
  uint64_t size;
  uint32_t version;
  VkrRecordingChunkInfo *chunks;
  uint32_t chunk_count;
  /* The trailer and index were present and matched the chunks. */
  bool8_t indexed;
  /* Bytes after the last whole chunk, or no END chunk: a crash or a kill. */
  bool8_t truncated;
  /* Offset just past the last whole chunk. */
  uint64_t valid_bytes;
} VkrRecordingReader;

VkrRecordingReadError vkr_recording_reader_open(VkrRecordingReader *reader,
                                                VkrAllocator *allocator,
                                                const char *path);
void vkr_recording_reader_close(VkrRecordingReader *reader);

/**
 * @brief Checks a chunk's payload CRC and decodes it into the allocator.
 * @param codec Needed only for compressed chunks; its decompress is used.
 * @return false_v on a CRC mismatch, a missing codec or a decode failure.
 */
bool8_t vkr_recording_reader_chunk(const VkrRecordingReader *reader,
                                   uint32_t index, const VkrCodec *codec,
                                   VkrAllocator *allocator, uint8_t **out_data,
                                   uint64_t *out_size);

/** Frees a payload returned by vkr_recording_reader_chunk. */
void vkr_recording_reader_chunk_free(VkrAllocator *allocator, uint8_t *data,
                                     uint64_t size);

/** The first chunk of `type`, or UINT32_MAX. */
uint32_t vkr_recording_reader_find(const VkrRecordingReader *reader,
                                   VkrRecordingChunkType type);

const char *vkr_recording_chunk_type_name(uint32_t type);
