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

/*
 * GPU pass starts (stream "pass_starts" of runtime recordings with Tier 1
 * metrics): per rendered frame, under its number, a u32 row count, u32
 * VKR_RECORDING_PASS_STARTS_* flags, u64 GPU start and u64 GPU end on the
 * CPU's monotonic clock in ns, then per row of the frame's passes record, in
 * its order, an f32 start in ms after the GPU start (NaN for an untimed
 * pass).
 */
#define VKR_RECORDING_PASS_STARTS_HEADER_SIZE 24u
/* The GPU start and end are on the CPU clock. */
#define VKR_RECORDING_PASS_STARTS_HOST_CLOCK 1u

/*
 * Render graph shape (stream "graph" of runtime recordings with Tier 1
 * metrics), written for the first rendered frame and again whenever the
 * frame's passes change, under the recorded frame it describes. Every
 * integer is little-endian. A record is a VKR_RECORDING_GRAPH_HEADER_SIZE
 * header of u32 fields: version, pass count, use count, resource count, name
 * bytes, VKR_RECORDING_GRAPH_* flags, viewport width and height, and an f32
 * render scale. Then come the passes in execution order and then the
 * unscheduled ones, each VKR_RECORDING_GRAPH_PASS_SIZE bytes: u32 name offset,
 * u16 name length, u8 VKR_RECORDING_GRAPH_KIND_*, u8 VKR_RECORDING_GRAPH_PASS_*
 * flags, u32 order (UINT32_MAX unscheduled), u32 first use, u32 use count.
 * Then the uses, each VKR_RECORDING_GRAPH_USE_SIZE bytes: u32 resource name
 * offset, u16 its length, u8 VKR_RECORDING_GRAPH_ROLE_*, u8
 * VKR_RECORDING_GRAPH_USE_* flags. Then the resources, each
 * VKR_RECORDING_GRAPH_RESOURCE_SIZE bytes: u32 name offset, u16 name length,
 * u16 VKR_RECORDING_GRAPH_RESOURCE_* flags, u32 format
 * offset, u16 format length, u16 lifetime length, u32 lifetime offset, u32
 * width, height and depth, u16 layers, mip levels and samples, u16 zero, u64
 * bytes, u32 first and last order. Then the names: offsets count from the
 * first name byte.
 */
#define VKR_RECORDING_GRAPH_VERSION 1u
#define VKR_RECORDING_GRAPH_HEADER_SIZE 36u
#define VKR_RECORDING_GRAPH_PASS_SIZE 20u
#define VKR_RECORDING_GRAPH_USE_SIZE 8u
#define VKR_RECORDING_GRAPH_RESOURCE_SIZE 56u
/* Something did not fit the renderer's snapshot. */
#define VKR_RECORDING_GRAPH_TRUNCATED 1u
/* A pass's kind. */
#define VKR_RECORDING_GRAPH_KIND_GRAPHICS 0u
#define VKR_RECORDING_GRAPH_KIND_COMPUTE 1u
#define VKR_RECORDING_GRAPH_KIND_TRANSFER 2u
/* A pass's flags: not scheduled, disabled, more uses than recorded. */
#define VKR_RECORDING_GRAPH_PASS_CULLED 1u
#define VKR_RECORDING_GRAPH_PASS_DISABLED 2u
#define VKR_RECORDING_GRAPH_PASS_USES_CUT 4u
/* How a pass uses a resource. */
#define VKR_RECORDING_GRAPH_ROLE_READ 0u
#define VKR_RECORDING_GRAPH_ROLE_WRITE 1u
#define VKR_RECORDING_GRAPH_ROLE_COLOR 2u
#define VKR_RECORDING_GRAPH_ROLE_DEPTH 3u
#define VKR_RECORDING_GRAPH_ROLE_DEPTH_READ 4u
#define VKR_RECORDING_GRAPH_USE_BUFFER 1u
#define VKR_RECORDING_GRAPH_USE_WRITE 2u
/*
 * Named allocators (streams "allocator_name" and "allocators" of runtime
 * recordings with Tier 1 metrics), from the lib's allocator registry. An
 * allocator_name record is a u32 allocator id, u8 VkrAllocatorType, u8 owner
 * (0 an allocator, 1 a raw arena), u16 name length, then the UTF-8 name; it
 * comes before the first allocators record that uses the id, and a slot the
 * registry reuses gets a new id. An allocators record holds one frame's
 * samples, under that frame's number: a u32 row count, u32 zero, then per row
 * VKR_RECORDING_ALLOCATOR_ROW_SIZE bytes: u32 id, u32 deepest scope since the
 * last row, u32 allocations, frees, reallocations and scopes begun since the
 * last row, u64 live bytes, then the arena's u64 used, committed and
 * reserved bytes and u64 highest used since the last row (all zero without
 * an arena).
 */
#define VKR_RECORDING_ALLOCATOR_MAX 256u
#define VKR_RECORDING_ALLOCATORS_HEADER_SIZE 8u
#define VKR_RECORDING_ALLOCATOR_ROW_SIZE 64u
#define VKR_RECORDING_ALLOCATOR_NAME_MAX 47u

/*
 * Draw census (streams "draw_bounds", "draw_ids" and "draw_states" of
 * runtime recordings with Tier 1 metrics): what the GPU's camera culling did
 * with every draw candidate. A draw_bounds record is a header of a u32 row
 * count, u32 zero, f32 world partition cell size in metres (zero without a
 * partition) and u32 zero, then per object VKR_RECORDING_DRAW_BOUNDS_ROW_SIZE
 * bytes: u32 object id, f32 min x, y, z and f32 max x, y, z of its world box;
 * written when the world's objects change, under the frame that built them.
 * A draw_ids record is a u32 candidate count, u32 zero, u64 ids generation,
 * then each candidate's u32 object id in the census's packed order; written
 * when the ids change. A draw_states record holds one rendered frame's census
 * under that frame's number: u32 candidate count, u32
 * VKR_RECORDING_DRAW_STATES_* flags, u32 draws the compaction pool cut, u32
 * zero, u64 the generation of the ids it follows, then 4-bit
 * VKR_RECORDING_DRAW_STATE_* values, eight per u32 from the low bits.
 */
#define VKR_RECORDING_DRAW_BOUNDS_HEADER_SIZE 16u
#define VKR_RECORDING_DRAW_BOUNDS_ROW_SIZE 28u
#define VKR_RECORDING_DRAW_IDS_HEADER_SIZE 16u
#define VKR_RECORDING_DRAW_STATES_HEADER_SIZE 24u
#define VKR_RECORDING_DRAW_STATES_PER_WORD 8u
/* Candidates a census record holds at most. */
#define VKR_RECORDING_DRAW_CANDIDATE_MAX (1u << 20u)
#define VKR_RECORDING_DRAW_STATE_NOT_CAMERA 0u
#define VKR_RECORDING_DRAW_STATE_DRAWN 1u
/* Deferred by the tiled camera's first phase, then drawn by its second. */
#define VKR_RECORDING_DRAW_STATE_DRAWN_LATE 2u
#define VKR_RECORDING_DRAW_STATE_OCCLUDED 3u
#define VKR_RECORDING_DRAW_STATE_FRUSTUM 4u
/* Past the draw distance, too small, or in a hidden room. */
#define VKR_RECORDING_DRAW_STATE_DISTANCE 5u
#define VKR_RECORDING_DRAW_STATES_TWO_PHASE 1u
#define VKR_RECORDING_DRAW_STATES_HZB_VALID 2u
#define VKR_RECORDING_DRAW_STATES_FROZEN 4u

/*
 * Hardware (streams "hardware" and "pipeline" of runtime recordings with
 * Tier 1 metrics, ADR-130). A hardware record is one fact about the device
 * the recording ran on, written once under its first measured frame: a u16
 * key length, a u16 value length, then the UTF-8 key and value, such as
 * "GPU cores" and "14". A pipeline record is one pipeline the renderer
 * created, in creation order, under the frame that saw it (pipelines
 * created before the recording come under its first frame):
 * VKR_RECORDING_PIPELINE_HEADER_SIZE bytes of u32 index, u8
 * VkrPipelineKind, u8 zero, u16 name length, f32 compile ms (negative when
 * untimed), then u32 max threads per threadgroup, execution width,
 * threadgroup memory bytes, registers, spill bytes, instructions and
 * statistic count (zero where the API reports none); then the name; then
 * per statistic a u16 name length, the name and an unaligned f64 value.
 */
#define VKR_RECORDING_HARDWARE_KEY_MAX 47u
#define VKR_RECORDING_HARDWARE_VALUE_MAX 95u
#define VKR_RECORDING_PIPELINE_HEADER_SIZE 40u
#define VKR_RECORDING_PIPELINE_NAME_MAX 63u
#define VKR_RECORDING_PIPELINE_STAT_NAME_MAX 47u
/* Statistics one pipeline record carries at most (the renderer's
   VKR_PIPELINE_STATS_PER_ENTRY). */
#define VKR_RECORDING_PIPELINE_STATS_PER_RECORD 64u
/* Pipelines and statistics a recording carries at most. */
#define VKR_RECORDING_PIPELINE_MAX 4096u
#define VKR_RECORDING_PIPELINE_STATS_MAX 32768u

typedef struct VkrRecordingHardwareFact {
  char key[VKR_RECORDING_HARDWARE_KEY_MAX + 1u];
  char value[VKR_RECORDING_HARDWARE_VALUE_MAX + 1u];
} VkrRecordingHardwareFact;

/*
 * GPU counters imported from an Instruments trace (ADR-130): the file
 * VKR_RECORDING_GPU_COUNTERS_FILE beside a bundle's stream.vkrs, which
 * tools/recording/vkr_xctrace.py counters writes after the recording. It
 * holds each counter's samples averaged into buckets of equal length on the
 * CPU's monotonic clock, the clock pass_starts records use. Little-endian:
 *
 *   header   magic "VKRGPUCT", u32 version, u32 counter count, u32 bucket
 *            count, u32 bucket ns, u64 first bucket's start ns, u32 clock
 *            anchors the trace held, u32 their spread in ns       40 bytes
 *   counters per counter a u8 VkrRecordingGpuCounterUnit, a u8 name length
 *            and the UTF-8 name; then zero padding to 4 bytes
 *   values   per counter, per bucket an f32 mean (NaN where no sample fell)
 */
#define VKR_RECORDING_GPU_COUNTERS_FILE "gpu_counters.bin"
#define VKR_RECORDING_GPU_COUNTERS_MAGIC "VKRGPUCT"
#define VKR_RECORDING_GPU_COUNTERS_VERSION 1u
#define VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE 40u
#define VKR_RECORDING_GPU_COUNTER_MAX 64u
#define VKR_RECORDING_GPU_COUNTER_NAME_MAX 63u
/* Buckets one file holds at most: 100 us buckets over about 100 s. */
#define VKR_RECORDING_GPU_COUNTER_BUCKET_MAX (1u << 20u)

typedef enum VkrRecordingGpuCounterUnit {
  /* Percent of the counter's peak, 0 to 100. */
  VKR_RECORDING_GPU_COUNTER_PERCENT = 0,
  VKR_RECORDING_GPU_COUNTER_GB_PER_SECOND = 1,
  /* A plain count, such as partial renders. */
  VKR_RECORDING_GPU_COUNTER_COUNT = 2,
} VkrRecordingGpuCounterUnit;

/* A resource's flags. */
#define VKR_RECORDING_GRAPH_RESOURCE_BUFFER 1u
#define VKR_RECORDING_GRAPH_RESOURCE_IMPORTED 2u
#define VKR_RECORDING_GRAPH_RESOURCE_EXPORTED 4u
#define VKR_RECORDING_GRAPH_RESOURCE_PRESENT 8u

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
