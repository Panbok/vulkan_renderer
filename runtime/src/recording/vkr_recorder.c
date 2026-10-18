#include "recording/vkr_recorder.h"

#include "containers/str.h"
#include "containers/vkr_spsc_ring.h"
#include "core/vkr_atomic.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

#define VKR_RECORDER_DEFAULT_RING_BYTES (8ull * 1024ull * 1024ull)
#define VKR_RECORDER_DEFAULT_CHUNK_BYTES (1024ull * 1024ull)
#define VKR_RECORDER_DEFAULT_CHUNK_SECONDS 1.0
/* The writer sleeps this long when every ring is empty. */
#define VKR_RECORDER_IDLE_MS 2u

typedef struct VkrRecorderLane {
  VkrSpscRing ring;
  /* Producer-written counters the writer and stats read. */
  VkrAtomicUint64 dropped;
  VkrAtomicUint64 lost;
  VkrAtomicUint64 last_lost_frame;
  /* The one thread allowed to produce into this lane; zero until first use. */
  VkrThreadId owner;
  /* Writer-owned. */
  uint64_t lost_reported;
  uint64_t last_frame;
  bool8_t any_frame;
} VkrRecorderLane;

struct s_VkrRecorder {
  VkrAllocator *allocator;
  char *path;
  uint64_t path_bytes;
  uint16_t stream_flags[VKR_RECORDING_STREAM_MAX];
  uint32_t stream_size[VKR_RECORDING_STREAM_MAX];
  bool8_t stream_known[VKR_RECORDING_STREAM_MAX];
  uint64_t chunk_bytes;
  float64_t chunk_seconds;

  VkrRecorderLane lanes[VKR_RECORDER_PRODUCER_COUNT];
  VkrRecordingWriter writer;
  VkrThread thread;
  VkrAtomicBool stop;

  /* Writer-owned chunk being filled. */
  uint8_t *chunk;
  uint64_t chunk_size;
  uint64_t chunk_capacity;
  uint64_t chunk_first;
  uint64_t chunk_last;
  float64_t chunk_started;
  uint64_t first_frame;
  uint64_t last_frame;
  bool8_t any_frame;

  float64_t start_time;
  /* Published by the writer for vkr_recorder_stats. */
  VkrAtomicUint64 records;
  VkrAtomicUint64 raw_bytes;
  VkrAtomicUint64 stored_bytes;
  VkrAtomicUint64 chunks;
  VkrAtomicUint64 writer_nanoseconds;
  VkrAtomicBool failed;
};

vkr_internal const VkrRecordingField VKR_RECORDER_GAP_FIELDS[] = {
    {.name = "producer",
     .type = VKR_RECORDING_FIELD_U32,
     .count = 1u,
     .offset = 0u},
    {.name = "lost_records",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 8u},
    {.name = "first_frame",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 16u},
    {.name = "last_frame",
     .type = VKR_RECORDING_FIELD_U64,
     .count = 1u,
     .offset = 24u},
};

vkr_internal const VkrRecordingStream VKR_RECORDER_GAP_STREAM = {
    .id = VKR_RECORDING_STREAM_RECORDER,
    .name = "recorder.gap",
    .record_size = sizeof(VkrRecorderGap),
    .field_count = ArrayCount(VKR_RECORDER_GAP_FIELDS),
    .fields = VKR_RECORDER_GAP_FIELDS,
};

/* ------------------------------------------------------------------------ */
/* Producers                                                                 */

void *vkr_recorder_reserve(VkrRecorder *recorder, VkrRecorderProducer producer,
                           uint16_t stream, uint64_t frame, uint32_t size) {
  vkr_assert(recorder != NULL && producer < VKR_RECORDER_PRODUCER_COUNT);
  vkr_assert(stream < VKR_RECORDING_STREAM_MAX &&
             recorder->stream_known[stream]);
  vkr_assert(recorder->stream_size[stream] == 0u ||
             recorder->stream_size[stream] == size);
  VkrRecorderLane *lane = &recorder->lanes[producer];
  if (!lane->ring.data) {
    vkr_atomic_uint64_fetch_add(&lane->dropped, 1u, VKR_MEMORY_ORDER_RELAXED);
    return NULL;
  }
#if !defined(NDEBUG)
  const VkrThreadId thread = vkr_thread_current_id();
  if (lane->owner == 0u) {
    lane->owner = thread;
  }
  vkr_assert(lane->owner == thread && "one thread per recorder producer");
#endif

  uint8_t *record = vkr_spsc_ring_reserve(
      &lane->ring, VKR_RECORDING_RECORD_HEADER_SIZE + size);
  if (!record) {
    if (recorder->stream_flags[stream] & VKR_RECORDING_STREAM_DROPPABLE) {
      vkr_atomic_uint64_fetch_add(&lane->dropped, 1u, VKR_MEMORY_ORDER_RELAXED);
    } else {
      vkr_atomic_uint64_store(&lane->last_lost_frame, frame,
                              VKR_MEMORY_ORDER_RELAXED);
      vkr_atomic_uint64_fetch_add(&lane->lost, 1u, VKR_MEMORY_ORDER_RELEASE);
    }
    return NULL;
  }
  vkr_recording_record_header_store(record, stream, 0u, size, frame);
  return record + VKR_RECORDING_RECORD_HEADER_SIZE;
}

void vkr_recorder_commit(VkrRecorder *recorder, VkrRecorderProducer producer) {
  vkr_assert(recorder != NULL && producer < VKR_RECORDER_PRODUCER_COUNT);
  vkr_spsc_ring_commit(&recorder->lanes[producer].ring);
}

bool8_t vkr_recorder_write(VkrRecorder *recorder, VkrRecorderProducer producer,
                           uint16_t stream, uint64_t frame, const void *data,
                           uint32_t size) {
  void *payload = vkr_recorder_reserve(recorder, producer, stream, frame, size);
  if (!payload) {
    return false_v;
  }
  if (size > 0u) {
    MemCopy(payload, data, size);
  }
  vkr_recorder_commit(recorder, producer);
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Writer thread                                                             */

vkr_internal bool8_t vkr_recorder_chunk_reserve(VkrRecorder *recorder,
                                                uint64_t bytes) {
  const uint64_t needed = recorder->chunk_size + bytes;
  if (needed <= recorder->chunk_capacity) {
    return true_v;
  }
  uint64_t capacity = Max(recorder->chunk_capacity * 2u, (uint64_t)KB(64));
  while (capacity < needed) {
    capacity *= 2u;
  }
  uint8_t *grown = vkr_allocator_realloc(recorder->allocator, recorder->chunk,
                                         recorder->chunk_capacity, capacity,
                                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!grown) {
    return false_v;
  }
  recorder->chunk = grown;
  recorder->chunk_capacity = capacity;
  return true_v;
}

vkr_internal void vkr_recorder_flush(VkrRecorder *recorder) {
  if (recorder->chunk_size == 0u) {
    return;
  }
  const float64_t start = vkr_platform_get_absolute_time();
  if (!vkr_recording_writer_chunk(
          &recorder->writer, VKR_RECORDING_CHUNK_RECORDS, recorder->chunk_first,
          recorder->chunk_last, recorder->chunk, recorder->chunk_size)) {
    vkr_atomic_bool_store(&recorder->failed, true_v, VKR_MEMORY_ORDER_RELEASE);
  }
  const float64_t elapsed = vkr_platform_get_absolute_time() - start;
  vkr_atomic_uint64_fetch_add(&recorder->writer_nanoseconds,
                              (uint64_t)(elapsed * 1.0e9),
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&recorder->raw_bytes, recorder->writer.raw_bytes,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&recorder->stored_bytes,
                          recorder->writer.stored_bytes,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_fetch_add(&recorder->chunks, 1u, VKR_MEMORY_ORDER_RELAXED);
  recorder->chunk_size = 0u;
}

/* Copies one record, already in the chunk layout, into the open chunk. */
vkr_internal void vkr_recorder_append(VkrRecorder *recorder,
                                      const uint8_t *record, uint32_t size) {
  VkrRecordingRecord parsed;
  uint64_t cursor = 0u;
  const uint64_t padded = vkr_align_up_u64(size, 8u);
  if (!vkr_recorder_chunk_reserve(recorder, padded)) {
    vkr_atomic_bool_store(&recorder->failed, true_v, VKR_MEMORY_ORDER_RELEASE);
    return;
  }
  uint8_t *at = recorder->chunk + recorder->chunk_size;
  MemCopy(at, record, size);
  MemZero(at + size, padded - size);
  vkr_recording_records_next(at, padded, &cursor, &parsed);

  if (recorder->chunk_size == 0u) {
    recorder->chunk_first = parsed.frame;
    recorder->chunk_last = parsed.frame;
    recorder->chunk_started = vkr_platform_get_absolute_time();
  }
  recorder->chunk_first = Min(recorder->chunk_first, parsed.frame);
  recorder->chunk_last = Max(recorder->chunk_last, parsed.frame);
  recorder->chunk_size += padded;
  if (!recorder->any_frame) {
    recorder->first_frame = parsed.frame;
    recorder->last_frame = parsed.frame;
    recorder->any_frame = true_v;
  }
  recorder->first_frame = Min(recorder->first_frame, parsed.frame);
  recorder->last_frame = Max(recorder->last_frame, parsed.frame);
  vkr_atomic_uint64_fetch_add(&recorder->records, 1u, VKR_MEMORY_ORDER_RELAXED);
}

/* Writes a gap record for records a lane lost since the last report. */
vkr_internal void vkr_recorder_report_gap(VkrRecorder *recorder,
                                          uint32_t producer) {
  VkrRecorderLane *lane = &recorder->lanes[producer];
  const uint64_t lost =
      vkr_atomic_uint64_load(&lane->lost, VKR_MEMORY_ORDER_ACQUIRE);
  if (lost == lane->lost_reported) {
    return;
  }
  const uint64_t last =
      vkr_atomic_uint64_load(&lane->last_lost_frame, VKR_MEMORY_ORDER_RELAXED);
  const VkrRecorderGap gap = {
      .producer = producer,
      .lost_records = lost - lane->lost_reported,
      .first_frame = lane->any_frame ? lane->last_frame + 1u : 0u,
      .last_frame = Max(last, lane->any_frame ? lane->last_frame + 1u : 0u),
  };
  lane->lost_reported = lost;
  uint8_t record[VKR_RECORDING_RECORD_HEADER_SIZE + sizeof(gap)];
  vkr_recording_record_header_store(record, VKR_RECORDING_STREAM_RECORDER, 0u,
                                    sizeof(gap), gap.last_frame);
  MemCopy(record + VKR_RECORDING_RECORD_HEADER_SIZE, &gap, sizeof(gap));
  vkr_recorder_append(recorder, record, sizeof(record));
}

/* Moves every committed record of one lane into chunks. */
vkr_internal uint64_t vkr_recorder_drain(VkrRecorder *recorder,
                                         uint32_t producer) {
  VkrRecorderLane *lane = &recorder->lanes[producer];
  uint64_t drained = 0u;
  uint32_t size = 0u;
  const uint8_t *record = NULL;
  while ((record = vkr_spsc_ring_peek(&lane->ring, &size)) != NULL) {
    if (!vkr_atomic_bool_load(&recorder->failed, VKR_MEMORY_ORDER_ACQUIRE)) {
      lane->last_frame = vkr_load_le_u64(record + 8u);
      lane->any_frame = true_v;
      vkr_recorder_append(recorder, record, size);
      if (recorder->chunk_size >= recorder->chunk_bytes) {
        vkr_recorder_flush(recorder);
      }
    }
    vkr_spsc_ring_pop(&lane->ring);
    ++drained;
  }
  vkr_recorder_report_gap(recorder, producer);
  return drained;
}

vkr_internal void *vkr_recorder_thread(void *argument) {
  VkrRecorder *recorder = (VkrRecorder *)argument;
  vkr_thread_set_utility();
  for (;;) {
    /* Read the stop flag before draining, so records committed before
       vkr_recorder_stop set it are drained on this pass. */
    const bool32_t stopping =
        vkr_atomic_bool_load(&recorder->stop, VKR_MEMORY_ORDER_ACQUIRE);
    uint64_t drained = 0u;
    for (uint32_t i = 0u; i < VKR_RECORDER_PRODUCER_COUNT; ++i) {
      if (recorder->lanes[i].ring.data) {
        drained += vkr_recorder_drain(recorder, i);
      }
    }
    if (recorder->chunk_size > 0u &&
        vkr_platform_get_absolute_time() - recorder->chunk_started >=
            recorder->chunk_seconds) {
      vkr_recorder_flush(recorder);
    }
    if (drained == 0u) {
      if (stopping) {
        break;
      }
      vkr_thread_sleep(VKR_RECORDER_IDLE_MS);
    }
  }
  vkr_recorder_flush(recorder);
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                  */

vkr_internal bool8_t vkr_recorder_configure(VkrRecorder *recorder,
                                            const VkrRecorderConfig *config) {
  if (!config->stream_path || config->stream_count == 0u ||
      config->stream_count >= VKR_RECORDING_STREAM_MAX || !config->streams) {
    return false_v;
  }
  recorder->stream_known[VKR_RECORDING_STREAM_RECORDER] = true_v;
  recorder->stream_size[VKR_RECORDING_STREAM_RECORDER] =
      VKR_RECORDER_GAP_STREAM.record_size;
  for (uint32_t i = 0u; i < config->stream_count; ++i) {
    const VkrRecordingStream *stream = &config->streams[i];
    if (stream->id == VKR_RECORDING_STREAM_RECORDER ||
        stream->id >= VKR_RECORDING_STREAM_MAX ||
        recorder->stream_known[stream->id]) {
      return false_v;
    }
    recorder->stream_known[stream->id] = true_v;
    recorder->stream_flags[stream->id] = stream->flags;
    recorder->stream_size[stream->id] = stream->record_size;
  }
  recorder->chunk_bytes = config->chunk_bytes
                              ? config->chunk_bytes
                              : VKR_RECORDER_DEFAULT_CHUNK_BYTES;
  recorder->chunk_seconds = config->chunk_seconds > 0.0
                                ? config->chunk_seconds
                                : VKR_RECORDER_DEFAULT_CHUNK_SECONDS;
  recorder->path_bytes = string_length(config->stream_path) + 1u;
  recorder->path =
      vkr_allocator_alloc(recorder->allocator, recorder->path_bytes,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!recorder->path) {
    return false_v;
  }
  MemCopy(recorder->path, config->stream_path, recorder->path_bytes);
  return true_v;
}

/* Writes the IDENTITY and DICTIONARY chunks; the recorder's gap stream
   comes first in the dictionary. */
vkr_internal bool8_t vkr_recorder_write_preamble(
    VkrRecorder *recorder, const VkrRecorderConfig *config) {
  const char *identity = config->identity_json ? config->identity_json : "{}";
  if (!vkr_recording_writer_chunk(&recorder->writer,
                                  VKR_RECORDING_CHUNK_IDENTITY, 0u, 0u,
                                  identity, string_length(identity))) {
    return false_v;
  }

  const uint64_t stream_bytes =
      sizeof(VkrRecordingStream) * ((uint64_t)config->stream_count + 1u);
  VkrRecordingStream *streams = vkr_allocator_alloc(
      recorder->allocator, stream_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!streams) {
    return false_v;
  }
  streams[0] = VKR_RECORDER_GAP_STREAM;
  MemCopy(streams + 1u, config->streams,
          sizeof(VkrRecordingStream) * (uint64_t)config->stream_count);
  uint8_t *dictionary = NULL;
  uint64_t dictionary_size = 0u;
  bool8_t ok = vkr_recording_dictionary_encode(
      streams, config->stream_count + 1u, recorder->allocator, &dictionary,
      &dictionary_size);
  vkr_allocator_free(recorder->allocator, streams, stream_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!ok) {
    return false_v;
  }
  ok = vkr_recording_writer_chunk(&recorder->writer,
                                  VKR_RECORDING_CHUNK_DICTIONARY, 0u, 0u,
                                  dictionary, dictionary_size);
  vkr_allocator_free(recorder->allocator, dictionary, dictionary_size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  return ok;
}

vkr_internal void vkr_recorder_release(VkrRecorder *recorder) {
  for (uint32_t i = 0u; i < VKR_RECORDER_PRODUCER_COUNT; ++i) {
    if (recorder->lanes[i].ring.data) {
      vkr_spsc_ring_destroy(recorder->allocator, &recorder->lanes[i].ring);
    }
  }
  if (recorder->chunk) {
    vkr_allocator_free(recorder->allocator, recorder->chunk,
                       recorder->chunk_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  if (recorder->path) {
    vkr_allocator_free(recorder->allocator, recorder->path,
                       recorder->path_bytes, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  vkr_allocator_free(recorder->allocator, recorder, sizeof(*recorder),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

VkrRecorder *vkr_recorder_start(VkrAllocator *allocator,
                                const VkrRecorderConfig *config) {
  vkr_assert(allocator != NULL && config != NULL);
  VkrRecorder *recorder = vkr_allocator_alloc(allocator, sizeof(*recorder),
                                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!recorder) {
    return NULL;
  }
  MemZero(recorder, sizeof(*recorder));
  recorder->allocator = allocator;
  const uint64_t ring_bytes =
      config->ring_bytes ? config->ring_bytes : VKR_RECORDER_DEFAULT_RING_BYTES;
  if (!vkr_recorder_configure(recorder, config)) {
    goto cleanup;
  }
  for (uint32_t i = 0u; i < VKR_RECORDER_PRODUCER_COUNT; ++i) {
    const uint64_t lane_bytes = i >= VKR_RECORDER_PRODUCER_WORKER_FIRST
                                    ? config->worker_ring_bytes
                                    : ring_bytes;
    if (lane_bytes > 0u && !vkr_spsc_ring_create(allocator, lane_bytes,
                                                 &recorder->lanes[i].ring)) {
      goto cleanup;
    }
  }
  if (!vkr_recording_writer_open(&recorder->writer, allocator, recorder->path,
                                 config->codec)) {
    goto cleanup;
  }
  if (!vkr_recorder_write_preamble(recorder, config)) {
    vkr_recording_writer_abandon(&recorder->writer);
    goto cleanup;
  }
  recorder->start_time = vkr_platform_get_absolute_time();
  if (!vkr_thread_create(allocator, &recorder->thread, vkr_recorder_thread,
                         recorder)) {
    vkr_recording_writer_abandon(&recorder->writer);
    goto cleanup;
  }
  return recorder;

cleanup:
  vkr_recorder_release(recorder);
  return NULL;
}

void vkr_recorder_stats(const VkrRecorder *recorder,
                        VkrRecorderStats *out_stats) {
  vkr_assert(recorder != NULL && out_stats != NULL);
  uint64_t dropped = 0u;
  uint64_t lost = 0u;
  for (uint32_t i = 0u; i < VKR_RECORDER_PRODUCER_COUNT; ++i) {
    dropped += vkr_atomic_uint64_load(&recorder->lanes[i].dropped,
                                      VKR_MEMORY_ORDER_RELAXED);
    lost += vkr_atomic_uint64_load(&recorder->lanes[i].lost,
                                   VKR_MEMORY_ORDER_RELAXED);
  }
  *out_stats = (VkrRecorderStats){
      .records =
          vkr_atomic_uint64_load(&recorder->records, VKR_MEMORY_ORDER_RELAXED),
      .dropped_records = dropped,
      .lost_records = lost,
      .raw_bytes = vkr_atomic_uint64_load(&recorder->raw_bytes,
                                          VKR_MEMORY_ORDER_RELAXED),
      .stored_bytes = vkr_atomic_uint64_load(&recorder->stored_bytes,
                                             VKR_MEMORY_ORDER_RELAXED),
      .chunks =
          vkr_atomic_uint64_load(&recorder->chunks, VKR_MEMORY_ORDER_RELAXED),
      .seconds = vkr_platform_get_absolute_time() - recorder->start_time,
      .writer_seconds =
          (float64_t)vkr_atomic_uint64_load(&recorder->writer_nanoseconds,
                                            VKR_MEMORY_ORDER_RELAXED) *
          1.0e-9,
      .failed =
          vkr_atomic_bool_load(&recorder->failed, VKR_MEMORY_ORDER_ACQUIRE),
  };
}

bool8_t vkr_recorder_stop(VkrRecorder *recorder, VkrRecorderStats *out_stats) {
  vkr_assert(recorder != NULL);
  vkr_atomic_bool_store(&recorder->stop, true_v, VKR_MEMORY_ORDER_RELEASE);
  vkr_thread_join(recorder->thread);
  vkr_thread_destroy(recorder->allocator, &recorder->thread);

  VkrRecorderStats stats;
  vkr_recorder_stats(recorder, &stats);
  const VkrRecordingEnd end = {
      .first_frame = recorder->first_frame,
      .last_frame = recorder->last_frame,
      .records = stats.records,
      .dropped_records = stats.dropped_records,
      .lost_records = stats.lost_records,
      .raw_bytes = recorder->writer.raw_bytes,
      .stored_bytes = recorder->writer.stored_bytes,
      .chunks = stats.chunks,
      .seconds = stats.seconds,
      .writer_seconds = stats.writer_seconds,
  };
  uint8_t encoded[VKR_RECORDING_END_SIZE];
  vkr_recording_end_encode(&end, encoded);
  bool8_t ok = !stats.failed && vkr_recording_writer_chunk(
                                    &recorder->writer, VKR_RECORDING_CHUNK_END,
                                    recorder->first_frame, recorder->last_frame,
                                    encoded, sizeof(encoded));
  ok = vkr_recording_writer_close(&recorder->writer) && ok;
  stats.failed = !ok;
  if (out_stats) {
    *out_stats = stats;
  }
  vkr_recorder_release(recorder);
  return ok;
}
