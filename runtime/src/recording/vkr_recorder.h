#pragma once

#include "assets/vkr_recording_format.h"
#include "core/vkr_codec.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/*
 * The recorder (docs/proposals/recordings.md, "Frame cost contract").
 *
 * Producer threads copy fixed records into their own preallocated ring and
 * never block: a full ring drops a droppable record or loses another one and
 * the writer records a gap. One writer thread at utility priority drains the
 * rings, groups records into chunks of about one second, compresses them
 * with the codec the executable passes and appends them to the stream file.
 * The writer is the only thread that allocates, compresses or does I/O.
 */

/* Job workers that get a producer of their own; later workers record no
   zones. */
#define VKR_RECORDER_WORKER_PRODUCERS 16u

typedef enum VkrRecorderProducer {
  /* The frame-loop thread: update, input, state. */
  VKR_RECORDER_PRODUCER_MAIN = 0,
  /* The render thread: acquisition, completion, metrics. */
  VKR_RECORDER_PRODUCER_RENDER,
  /* Job workers, one each, for CPU zones (vkr_zone.h); their rings exist
     only when the configuration sizes them. */
  VKR_RECORDER_PRODUCER_WORKER_FIRST,
  VKR_RECORDER_PRODUCER_COUNT =
      VKR_RECORDER_PRODUCER_WORKER_FIRST + VKR_RECORDER_WORKER_PRODUCERS,
} VkrRecorderProducer;

/* Recorder-owned record of stream VKR_RECORDING_STREAM_RECORDER: records a
   producer lost while its ring was full, written by the writer thread. */
typedef struct VkrRecorderGap {
  uint32_t producer;
  uint32_t reserved;
  uint64_t lost_records;
  uint64_t first_frame;
  uint64_t last_frame;
} VkrRecorderGap;

typedef struct VkrRecorderConfig {
  /* The stream file to create; copied. */
  const char *stream_path;
  /* NULL stores chunks raw. */
  const VkrCodec *codec;
  /* UTF-8 JSON for the IDENTITY chunk; copied. NULL writes "{}". */
  const char *identity_json;
  /* Stream ids are 1 to VKR_RECORDING_STREAM_MAX - 1 and unique. */
  const VkrRecordingStream *streams;
  uint32_t stream_count;
  /* Ring bytes per producer, a power of two; zero selects 8 MiB. */
  uint64_t ring_bytes;
  /* Ring bytes per job-worker producer, a power of two; zero creates no
     worker rings, and their records are dropped. */
  uint64_t worker_ring_bytes;
  /* A chunk closes at this many raw bytes; zero selects 1 MiB. */
  uint64_t chunk_bytes;
  /* A chunk closes after this long; zero selects one second. */
  float64_t chunk_seconds;
  /* Rolling mode: write no file; keep the closed chunks of about this many
     seconds in memory until vkr_recorder_save writes them out. Zero records
     to `stream_path` as usual. */
  float64_t rolling_seconds;
} VkrRecorderConfig;

typedef struct VkrRecorderStats {
  uint64_t records;
  uint64_t dropped_records;
  uint64_t lost_records;
  uint64_t raw_bytes;
  uint64_t stored_bytes;
  uint64_t chunks;
  float64_t seconds;
  float64_t writer_seconds;
  /* The stream file could not be written; records are discarded. */
  bool8_t failed;
} VkrRecorderStats;

/* What vkr_recorder_save wrote: the recorded frames it kept, renumbered
   from zero, and the seconds of recording they span. */
typedef struct VkrRecorderSaveResult {
  uint64_t first_frame;
  uint64_t frame_count;
  float64_t seconds;
  uint64_t records;
} VkrRecorderSaveResult;

typedef struct s_VkrRecorder VkrRecorder;

/**
 * @brief Creates the stream file, writes its identity and dictionary and
 * starts the writer thread.
 * @param allocator Holds the recorder until vkr_recorder_stop. The writer
 * thread allocates from it while producers run, so no other thread may use it
 * at the same time; give each recording its own.
 * @return NULL when the configuration is invalid or the file, rings or thread
 * cannot be created.
 */
VkrRecorder *vkr_recorder_start(VkrAllocator *allocator,
                                const VkrRecorderConfig *config);

/**
 * @brief Producer: reserves a record of `size` payload bytes in `producer`'s
 * ring. Only one thread may use each producer.
 * @return The payload to fill before vkr_recorder_commit, or NULL when the
 * ring is full or the producer has no ring; the loss is counted.
 */
void *vkr_recorder_reserve(VkrRecorder *recorder, VkrRecorderProducer producer,
                           uint16_t stream, uint64_t frame, uint32_t size);

/** Producer: publishes the record vkr_recorder_reserve returned. */
void vkr_recorder_commit(VkrRecorder *recorder, VkrRecorderProducer producer);

/** Producer: reserves, copies and commits one record. */
bool8_t vkr_recorder_write(VkrRecorder *recorder, VkrRecorderProducer producer,
                           uint16_t stream, uint64_t frame, const void *data,
                           uint32_t size);

/**
 * @brief Rolling mode: writes the kept chunks as a complete stream file at
 * `stream_path`, frames renumbered from zero, after `preamble`: records in
 * the chunk layout that later records need, such as their names, written
 * first under frame zero. The writer thread does the I/O; the caller waits,
 * up to a few seconds, for it. The recording keeps running.
 * @return false_v when the recorder is not rolling, holds no chunks, or the
 * file could not be written.
 */
bool8_t vkr_recorder_save(VkrRecorder *recorder, const char *stream_path,
                          const void *preamble, uint64_t preamble_size,
                          VkrRecorderSaveResult *out_result);

/** Live counters for an overlay; any thread, values may lag by a chunk. */
void vkr_recorder_stats(const VkrRecorder *recorder,
                        VkrRecorderStats *out_stats);

/**
 * @brief Stops the writer after it drains the rings, writes the END chunk,
 * the index and the trailer, and releases the recorder. Producers must have
 * stopped writing.
 * @return false_v when any part of the stream could not be written.
 */
bool8_t vkr_recorder_stop(VkrRecorder *recorder, VkrRecorderStats *out_stats);
