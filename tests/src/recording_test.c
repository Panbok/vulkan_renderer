#include "recording_test.h"
#include "core/vkr_zone.h"

#include "assets/vkr_recording_analysis.h"
#include "assets/vkr_recording_annotations.h"
#include "assets/vkr_recording_counters.h"
#include "assets/vkr_recording_format.h"
#include "containers/vkr_spsc_ring.h"
#include "core/vkr_threads.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "recording/vkr_recorder.h"
#include "recording/vkr_recording.h"
#include "recording/vkr_recording_drive.h"
#include "recording/vkr_recording_playback.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_depot_zstd.h"
#include "vkr_frame_input.h"
#include "vkr_pipeline_registry.h"
#include "vkr_renderer_metrics.h"
#include "vkr_rg_debug.h"

/* Recordings (docs/proposals/recordings.md, phase R0): the producer ring,
 * the stream format and the recorder. Each test names the failure it
 * detects. Files live under tests/tmp and are removed. */

typedef struct RecordingTestMemory {
  VkrDMemory memory;
  VkrAllocator allocator;
} RecordingTestMemory;

static void recording_memory_create(RecordingTestMemory *memory) {
  assert(vkr_dmemory_create(MB(4), GB(1), &memory->memory));
  memory->allocator = (VkrAllocator){.ctx = &memory->memory};
  vkr_dmemory_allocator_create(&memory->allocator);
}

static void recording_memory_destroy(RecordingTestMemory *memory) {
  vkr_dmemory_allocator_destroy(&memory->allocator);
}

static uint32_t recording_test_serial = 0u;

static void recording_test_path(const char *label, char *out,
                                uint32_t capacity) {
  snprintf(out, capacity, "%stests/tmp/recording_%u_%u_%s.vkrs",
           PROJECT_SOURCE_DIR, vkr_platform_get_process_id(),
           ++recording_test_serial, label);
}

static void recording_test_remove(const char *path) {
  const FilePath file = file_path_view(path);
  (void)file_remove(&file);
}

/* A record payload whose bytes follow from its sequence number. */
static void recording_fill(uint8_t *out, uint32_t size, uint64_t sequence) {
  for (uint32_t i = 0u; i < size; ++i) {
    out[i] = (uint8_t)(sequence * 31u + i * 7u);
  }
}

static bool8_t recording_matches(const uint8_t *data, uint32_t size,
                                 uint64_t sequence) {
  for (uint32_t i = 0u; i < size; ++i) {
    if (data[i] != (uint8_t)(sequence * 31u + i * 7u)) {
      return false_v;
    }
  }
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Ring                                                                      */

/* Detects: a record split across the buffer end, a lost or reordered record
   after wrap, a reservation that overwrites unread data, and an oversized
   reservation that corrupts the ring instead of failing. */
static void test_spsc_ring_wrap_and_full(void) {
  printf("  Running test_spsc_ring_wrap_and_full...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrSpscRing ring;
  assert(!vkr_spsc_ring_create(&memory.allocator, 100u, &ring));
  assert(vkr_spsc_ring_create(&memory.allocator, 256u, &ring));
  assert(vkr_spsc_ring_max_payload(&ring) == 120u);
  assert(vkr_spsc_ring_reserve(&ring, 121u) == NULL);

  uint8_t payload[128];
  uint64_t pushed = 0u;
  uint64_t popped = 0u;
  uint32_t wraps = 0u;
  for (uint32_t round = 0u; round < 400u; ++round) {
    const uint32_t size = 1u + (round * 37u) % 96u;
    recording_fill(payload, size, pushed);
    const uint64_t before = vkr_spsc_ring_used_bytes(&ring);
    if (vkr_spsc_ring_push(&ring, payload, size)) {
      const uint64_t after = vkr_spsc_ring_used_bytes(&ring);
      wraps += after - before > AlignPow2(8u + size, 8u) ? 1u : 0u;
      ++pushed;
    } else {
      /* Full: the consumer frees space and the same push then fits. */
      uint32_t out_size = 0u;
      const uint8_t *oldest = vkr_spsc_ring_peek(&ring, &out_size);
      assert(oldest != NULL && recording_matches(oldest, out_size, popped));
      vkr_spsc_ring_pop(&ring);
      ++popped;
      --round;
    }
    if (round % 3u == 0u) {
      uint32_t out_size = 0u;
      const uint8_t *oldest = vkr_spsc_ring_peek(&ring, &out_size);
      if (oldest) {
        assert(recording_matches(oldest, out_size, popped));
        vkr_spsc_ring_pop(&ring);
        ++popped;
      }
    }
  }
  uint32_t out_size = 0u;
  const uint8_t *oldest = NULL;
  while ((oldest = vkr_spsc_ring_peek(&ring, &out_size)) != NULL) {
    assert(recording_matches(oldest, out_size, popped));
    vkr_spsc_ring_pop(&ring);
    ++popped;
  }
  assert(popped == pushed && pushed >= 400u && wraps > 0u);
  assert(vkr_spsc_ring_used_bytes(&ring) == 0u);
  vkr_spsc_ring_destroy(&memory.allocator, &ring);
  recording_memory_destroy(&memory);
  printf("  test_spsc_ring_wrap_and_full PASSED\n");
}

#define RECORDING_RING_THREAD_RECORDS 200000u

typedef struct RecordingRingThreadTest {
  VkrSpscRing *ring;
  uint64_t full;
} RecordingRingThreadTest;

static void *recording_ring_producer(void *argument) {
  RecordingRingThreadTest *test = argument;
  uint8_t payload[64];
  for (uint64_t sequence = 0u; sequence < RECORDING_RING_THREAD_RECORDS;) {
    const uint32_t size = 8u + (uint32_t)(sequence % 56u);
    recording_fill(payload, size, sequence);
    if (vkr_spsc_ring_push(test->ring, payload, size)) {
      ++sequence;
    } else {
      ++test->full;
      vkr_thread_yield();
    }
  }
  return NULL;
}

/* Detects: a missing acquire/release pair or a stale cached position, which
   shows as a torn or reordered payload when two threads share the ring. */
static void test_spsc_ring_threads(void) {
  printf("  Running test_spsc_ring_threads...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrSpscRing ring;
  assert(vkr_spsc_ring_create(&memory.allocator, 4096u, &ring));
  RecordingRingThreadTest test = {.ring = &ring};
  VkrThread thread = NULL;
  assert(vkr_thread_create(&memory.allocator, &thread, recording_ring_producer,
                           &test));
  uint64_t sequence = 0u;
  while (sequence < RECORDING_RING_THREAD_RECORDS) {
    uint32_t size = 0u;
    const uint8_t *record = vkr_spsc_ring_peek(&ring, &size);
    if (!record) {
      vkr_thread_yield();
      continue;
    }
    assert(size == 8u + (uint32_t)(sequence % 56u));
    assert(recording_matches(record, size, sequence));
    vkr_spsc_ring_pop(&ring);
    ++sequence;
  }
  assert(vkr_thread_join(thread));
  vkr_thread_destroy(&memory.allocator, &thread);
  uint32_t size = 0u;
  assert(vkr_spsc_ring_peek(&ring, &size) == NULL);
  printf("    %llu full reservations\n", (unsigned long long)test.full);
  vkr_spsc_ring_destroy(&memory.allocator, &ring);
  recording_memory_destroy(&memory);
  printf("  test_spsc_ring_threads PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* Format                                                                    */

typedef struct RecordingTestPose {
  float64_t position[3];
  float32_t yaw;
  float32_t pitch;
} RecordingTestPose;

static const VkrRecordingField RECORDING_TEST_POSE_FIELDS[] = {
    {.name = "position",
     .unit = "m",
     .type = VKR_RECORDING_FIELD_F64,
     .count = 3u,
     .offset = 0u},
    {.name = "yaw",
     .unit = "rad",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 24u},
    {.name = "pitch",
     .unit = "rad",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 28u},
};

static const VkrRecordingField RECORDING_TEST_METRIC_FIELDS[] = {
    {.name = "frame_ms",
     .unit = "ms",
     .type = VKR_RECORDING_FIELD_F32,
     .count = 1u,
     .offset = 0u},
};

static const VkrRecordingStream RECORDING_TEST_STREAMS[] = {
    {.id = 1u,
     .name = "camera",
     .record_size = sizeof(RecordingTestPose),
     .field_count = ArrayCount(RECORDING_TEST_POSE_FIELDS),
     .fields = RECORDING_TEST_POSE_FIELDS},
    {.id = 2u,
     .flags = VKR_RECORDING_STREAM_DROPPABLE,
     .name = "metrics",
     .record_size = sizeof(float32_t),
     .field_count = ArrayCount(RECORDING_TEST_METRIC_FIELDS),
     .fields = RECORDING_TEST_METRIC_FIELDS},
    {.id = 3u, .name = "notes"},
};

static RecordingTestPose recording_test_pose(uint64_t frame) {
  return (RecordingTestPose){
      .position = {(float64_t)frame * 0.25, 1.7, -(float64_t)frame},
      .yaw = (float32_t)frame * 0.01f,
      .pitch = -0.1f,
  };
}

/* Builds a RECORDS payload of `frames` camera records. */
static uint64_t recording_test_records(uint8_t *out, uint64_t first,
                                       uint64_t frames) {
  uint64_t size = 0u;
  for (uint64_t frame = first; frame < first + frames; ++frame) {
    const RecordingTestPose pose = recording_test_pose(frame);
    vkr_recording_record_header_store(out + size, 1u, 0u, sizeof(pose), frame);
    MemCopy(out + size + VKR_RECORDING_RECORD_HEADER_SIZE, &pose, sizeof(pose));
    size += vkr_recording_record_bytes(sizeof(pose));
  }
  return size;
}

/* Writes identity, dictionary, three record chunks and an END chunk. */
static void recording_test_write(const char *path, VkrAllocator *allocator,
                                 bool8_t clean) {
  VkrRecordingWriter writer;
  assert(vkr_recording_writer_open(&writer, allocator, path,
                                   vkr_depot_zstd_codec()));
  const char *identity = "{\"scene\":\"bistro\"}";
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_IDENTITY, 0u,
                                    0u, identity, string_length(identity)));
  uint8_t *dictionary = NULL;
  uint64_t dictionary_size = 0u;
  assert(vkr_recording_dictionary_encode(
      RECORDING_TEST_STREAMS, ArrayCount(RECORDING_TEST_STREAMS), allocator,
      &dictionary, &dictionary_size));
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_DICTIONARY, 0u,
                                    0u, dictionary, dictionary_size));
  vkr_allocator_free(allocator, dictionary, dictionary_size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  static uint8_t records[60u * 64u];
  for (uint64_t chunk = 0u; chunk < 3u; ++chunk) {
    const uint64_t size = recording_test_records(records, chunk * 60u, 60u);
    assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_RECORDS,
                                      chunk * 60u, chunk * 60u + 59u, records,
                                      size));
  }
  const VkrRecordingEnd end = {
      .first_frame = 0u, .last_frame = 179u, .records = 180u, .seconds = 3.0};
  uint8_t encoded[VKR_RECORDING_END_SIZE];
  vkr_recording_end_encode(&end, encoded);
  assert(vkr_recording_writer_chunk(&writer, VKR_RECORDING_CHUNK_END, 0u, 179u,
                                    encoded, sizeof(encoded)));
  if (clean) {
    assert(vkr_recording_writer_close(&writer));
  } else {
    vkr_recording_writer_abandon(&writer);
  }
}

/* Checks that the reader holds `chunks` whole chunks whose records are the
   frames the writer wrote. */
static void recording_test_check(const VkrRecordingReader *reader,
                                 VkrAllocator *allocator, uint32_t chunks) {
  assert(reader->chunk_count == chunks);
  uint64_t next_frame = 0u;
  for (uint32_t i = 0u; i < reader->chunk_count; ++i) {
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    assert(vkr_recording_reader_chunk(reader, i, vkr_depot_zstd_codec(),
                                      allocator, &payload, &size));
    if (reader->chunks[i].type == VKR_RECORDING_CHUNK_RECORDS) {
      uint64_t cursor = 0u;
      VkrRecordingRecord record;
      while (vkr_recording_records_next(payload, size, &cursor, &record)) {
        const RecordingTestPose expected = recording_test_pose(next_frame);
        assert(record.stream == 1u && record.frame == next_frame &&
               record.size == sizeof(expected) &&
               MemCompare(record.data, &expected, sizeof(expected)) == 0);
        ++next_frame;
      }
      assert(cursor == size);
    }
    vkr_recording_reader_chunk_free(allocator, payload, size);
  }
}

/* Detects: a dictionary that loses a stream, field, unit or the variable
   stream; compression applied wrongly; a missing index; and frames or
   payloads that change between writer and reader. */
static void test_recording_format_round_trip(void) {
  printf("  Running test_recording_format_round_trip...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("round_trip", path, sizeof(path));
  recording_test_write(path, &memory.allocator, true_v);

  VkrRecordingReader reader;
  assert(vkr_recording_reader_open(&reader, &memory.allocator, path) ==
         VKR_RECORDING_READ_OK);
  assert(reader.indexed && !reader.truncated);
  recording_test_check(&reader, &memory.allocator, 6u);
  /* The repetitive camera chunks compress; the tiny identity stays raw. */
  assert(reader.chunks[0].codec == VKR_RECORDING_CODEC_RAW);
  assert(reader.chunks[2].codec == VKR_RECORDING_CODEC_ZSTD &&
         reader.chunks[2].stored_size < reader.chunks[2].raw_size);
  assert(reader.chunks[2].first_frame == 0u &&
         reader.chunks[4].last_frame == 179u);

  uint8_t *payload = NULL;
  uint64_t size = 0u;
  assert(vkr_recording_reader_chunk(&reader, 1u, vkr_depot_zstd_codec(),
                                    &memory.allocator, &payload, &size));
  VkrRecordingDictionary dictionary;
  assert(vkr_recording_dictionary_decode(payload, size, &memory.allocator,
                                         &dictionary));
  vkr_recording_reader_chunk_free(&memory.allocator, payload, size);
  assert(dictionary.stream_count == 3u);
  const VkrRecordingStream *camera =
      vkr_recording_dictionary_find(&dictionary, 1u);
  assert(camera && string_equals(camera->name, "camera") &&
         camera->record_size == sizeof(RecordingTestPose) &&
         camera->field_count == 3u);
  assert(string_equals(camera->fields[0].name, "position") &&
         string_equals(camera->fields[0].unit, "m") &&
         camera->fields[0].count == 3u && camera->fields[2].offset == 28u &&
         camera->fields[2].type == VKR_RECORDING_FIELD_F32);
  const VkrRecordingStream *metrics =
      vkr_recording_dictionary_find(&dictionary, 2u);
  assert(metrics && (metrics->flags & VKR_RECORDING_STREAM_DROPPABLE));
  const VkrRecordingStream *notes =
      vkr_recording_dictionary_find(&dictionary, 3u);
  assert(notes && notes->record_size == 0u && notes->field_count == 0u);
  vkr_recording_dictionary_free(&memory.allocator, &dictionary);

  /* A field past the end of its record is refused at encode time. */
  const VkrRecordingField outside = {
      .name = "x", .type = VKR_RECORDING_FIELD_F64, .count = 1u, .offset = 4u};
  const VkrRecordingStream bad = {.id = 9u,
                                  .name = "bad",
                                  .record_size = 8u,
                                  .field_count = 1u,
                                  .fields = &outside};
  assert(!vkr_recording_dictionary_encode(&bad, 1u, &memory.allocator, &payload,
                                          &size));

  vkr_recording_reader_close(&reader);
  recording_test_remove(path);
  recording_memory_destroy(&memory);
  printf("  test_recording_format_round_trip PASSED\n");
}

static void recording_test_copy_prefix(const char *source,
                                       const char *destination, uint64_t bytes,
                                       int64_t flip) {
  FileMapping mapping;
  const FilePath file = file_path_view(source);
  assert(file_map_readonly(&file, &mapping) == FILE_ERROR_NONE);
  assert(bytes <= mapping.size);
  FILE *out = fopen(destination, "wb");
  assert(out);
  assert(fwrite(mapping.data, 1u, (size_t)bytes, out) == bytes);
  if (flip >= 0) {
    assert(fseek(out, (long)flip, SEEK_SET) == 0);
    const uint8_t damaged = (uint8_t)(mapping.data[flip] ^ 0x40u);
    assert(fwrite(&damaged, 1u, 1u, out) == 1u);
  }
  assert(fclose(out) == 0);
  file_unmap(&mapping);
}

/* Detects: a reader that rejects a whole file because its tail is cut off,
   trusts a partial chunk, trusts an index over damaged data, or misses a
   flipped payload byte. */
static void test_recording_truncated_recovery(void) {
  printf("  Running test_recording_truncated_recovery...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  char cut[1024];
  recording_test_path("whole", path, sizeof(path));
  recording_test_path("cut", cut, sizeof(cut));
  recording_test_write(path, &memory.allocator, true_v);

  VkrRecordingReader whole;
  assert(vkr_recording_reader_open(&whole, &memory.allocator, path) ==
         VKR_RECORDING_READ_OK);
  const uint64_t third_records = whole.chunks[4].offset;
  const uint64_t end_chunk = whole.chunks[5].offset;
  const uint64_t size = whole.size;
  const uint64_t valid = whole.valid_bytes;
  vkr_recording_reader_close(&whole);

  typedef struct Cut {
    uint64_t bytes;
    int64_t flip;
    uint32_t chunks;
    bool8_t truncated;
  } Cut;
  const Cut cuts[] = {
      /* Trailer cut: every chunk is whole, END is present. */
      {size - 4u, -1, 6u, true_v},
      /* Index cut in half: the same chunks, no index. */
      {valid - 20u, -1, 6u, true_v},
      /* Process killed before END: a crash recording. */
      {end_chunk, -1, 5u, true_v},
      /* Cut inside the third records chunk's payload. */
      {third_records + VKR_RECORDING_CHUNK_HEADER_SIZE + 10u, -1, 4u, true_v},
      /* Cut inside the third records chunk's header. */
      {third_records + 12u, -1, 4u, true_v},
      /* A flipped payload byte in the third records chunk stops the scan
         there, even though the index lists later chunks. */
      {size, (int64_t)(third_records + VKR_RECORDING_CHUNK_HEADER_SIZE + 3u),
       4u, true_v},
  };
  for (uint32_t i = 0u; i < ArrayCount(cuts); ++i) {
    recording_test_copy_prefix(path, cut, cuts[i].bytes, cuts[i].flip);
    VkrRecordingReader reader;
    assert(vkr_recording_reader_open(&reader, &memory.allocator, cut) ==
           VKR_RECORDING_READ_OK);
    assert(!reader.indexed && reader.truncated == cuts[i].truncated);
    recording_test_check(&reader, &memory.allocator, cuts[i].chunks);
    vkr_recording_reader_close(&reader);
  }

  /* A writer abandoned as a crash leaves every written chunk readable. */
  recording_test_write(cut, &memory.allocator, false_v);
  VkrRecordingReader crashed;
  assert(vkr_recording_reader_open(&crashed, &memory.allocator, cut) ==
         VKR_RECORDING_READ_OK);
  assert(!crashed.indexed && !crashed.truncated &&
         crashed.valid_bytes == crashed.size);
  recording_test_check(&crashed, &memory.allocator, 6u);
  vkr_recording_reader_close(&crashed);

  recording_test_remove(path);
  recording_test_remove(cut);
  recording_memory_destroy(&memory);
  printf("  test_recording_truncated_recovery PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* Recorder                                                                  */

#define RECORDING_TEST_FRAMES 20000u

typedef struct RecordingProducerTest {
  VkrRecorder *recorder;
  VkrRecorderProducer producer;
  uint16_t stream;
  uint64_t written;
} RecordingProducerTest;

static void *recording_render_producer(void *argument) {
  RecordingProducerTest *test = argument;
  for (uint64_t frame = 0u; frame < RECORDING_TEST_FRAMES; ++frame) {
    const float32_t frame_ms = 8.0f + (float32_t)(frame % 7u);
    test->written +=
        vkr_recorder_write(test->recorder, test->producer, test->stream, frame,
                           &frame_ms, sizeof(frame_ms))
            ? 1u
            : 0u;
  }
  return NULL;
}

/* Detects: a producer that blocks or corrupts records when its ring is full,
   counters that do not add up (written + dropped or lost = attempted), gap
   records whose lost counts disagree with the END chunk, and records of one
   lane delivered out of order. */
/* Reads the camera records of `path` in file order: their frames, and the
   frame each pose was written for (its x position over 0.25). */
static uint32_t recording_test_camera_frames(VkrAllocator *allocator,
                                             const char *path, uint64_t *frames,
                                             uint64_t *written,
                                             uint32_t capacity,
                                             VkrRecordingEnd *out_end) {
  VkrRecordingReader reader;
  assert(vkr_recording_reader_open(&reader, allocator, path) ==
         VKR_RECORDING_READ_OK);
  assert(reader.indexed && !reader.truncated);
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < reader.chunk_count; ++i) {
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (reader.chunks[i].type == VKR_RECORDING_CHUNK_END) {
      assert(vkr_recording_reader_chunk(&reader, i, NULL, allocator, &payload,
                                        &size));
      assert(vkr_recording_end_decode(payload, size, out_end));
      vkr_recording_reader_chunk_free(allocator, payload, size);
      continue;
    }
    if (reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    assert(vkr_recording_reader_chunk(&reader, i, vkr_depot_zstd_codec(),
                                      allocator, &payload, &size));
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      if (record.stream != 1u) {
        continue;
      }
      assert(count < capacity);
      RecordingTestPose pose;
      MemCopy(&pose, record.data, sizeof(pose));
      frames[count] = record.frame;
      written[count] = (uint64_t)(pose.position[0] / 0.25 + 0.5);
      ++count;
    }
    vkr_recording_reader_chunk_free(allocator, payload, size);
  }
  vkr_recording_reader_close(&reader);
  return count;
}

static void test_recorder_rolling_save(void) {
  printf("  Running test_recorder_rolling_save...\n");
  RecordingTestMemory memory;
  RecordingTestMemory test_memory;
  recording_memory_create(&memory);
  recording_memory_create(&test_memory);
  char unused[1024];
  char first[1024];
  char second[1024];
  recording_test_path("rolling_unused", unused, sizeof(unused));
  recording_test_path("rolling_first", first, sizeof(first));
  recording_test_path("rolling_second", second, sizeof(second));
  const VkrRecorderConfig config = {
      .stream_path = unused,
      .codec = vkr_depot_zstd_codec(),
      .streams = RECORDING_TEST_STREAMS,
      .stream_count = 2u,
      .chunk_seconds = 0.01,
      .rolling_seconds = 0.15,
  };
  VkrRecorder *recorder = vkr_recorder_start(&memory.allocator, &config);
  assert(recorder != NULL);
  /* Frames 0 to 9 age out of the window during the pause. */
  for (uint64_t frame = 0u; frame < 10u; ++frame) {
    const RecordingTestPose pose = recording_test_pose(frame);
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN, 1u, frame,
                              &pose, sizeof(pose)));
  }
  vkr_thread_sleep(400u);
  for (uint64_t frame = 100u; frame < 110u; ++frame) {
    const RecordingTestPose pose = recording_test_pose(frame);
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN, 1u, frame,
                              &pose, sizeof(pose)));
  }
  vkr_thread_sleep(30u);
  /* The preamble's record goes first, under frame zero. */
  uint8_t
      preamble[VKR_RECORDING_RECORD_HEADER_SIZE + sizeof(RecordingTestPose)];
  const RecordingTestPose named = recording_test_pose(777u);
  vkr_recording_record_header_store(preamble, 1u, 0u, sizeof(named), 777u);
  MemCopy(preamble + VKR_RECORDING_RECORD_HEADER_SIZE, &named, sizeof(named));
  VkrRecorderSaveResult result;
  assert(
      vkr_recorder_save(recorder, first, preamble, sizeof(preamble), &result));
  assert(result.first_frame == 100u && result.frame_count == 10u);

  uint64_t frames[32];
  uint64_t written[32];
  VkrRecordingEnd end;
  uint32_t count = recording_test_camera_frames(
      &test_memory.allocator, first, frames, written, ArrayCount(frames), &end);
  assert(count == 11u);
  assert(frames[0] == 0u && written[0] == 777u);
  for (uint32_t i = 1u; i < count; ++i) {
    assert(frames[i] == i - 1u && written[i] == 99u + i);
  }
  assert(end.first_frame == 0u && end.last_frame == 9u);

  /* The recording keeps running after a save. */
  const RecordingTestPose late = recording_test_pose(110u);
  assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN, 1u, 110u,
                            &late, sizeof(late)));
  vkr_thread_sleep(30u);
  assert(vkr_recorder_save(recorder, second, NULL, 0u, &result));
  count = recording_test_camera_frames(&test_memory.allocator, second, frames,
                                       written, ArrayCount(frames), &end);
  assert(count == 11u && frames[count - 1u] == 10u &&
         written[count - 1u] == 110u);

  VkrRecorderStats stats;
  assert(vkr_recorder_stop(recorder, &stats));
  /* A rolling recorder writes no stream of its own. */
  const FilePath never = file_path_view(unused);
  assert(!file_exists(&never));
  recording_memory_destroy(&test_memory);
  recording_memory_destroy(&memory);
  printf("  test_recorder_rolling_save PASSED\n");
}

static void test_recorder_accounting(void) {
  printf("  Running test_recorder_accounting...\n");
  /* The writer thread allocates from the recorder's memory, so the test's
     own thread and reader use a second one. */
  RecordingTestMemory memory;
  RecordingTestMemory test_memory;
  recording_memory_create(&memory);
  recording_memory_create(&test_memory);
  char path[1024];
  recording_test_path("recorder", path, sizeof(path));
  const VkrRecorderConfig config = {
      .stream_path = path,
      .codec = vkr_depot_zstd_codec(),
      .identity_json = "{\"test\":true}",
      .streams = RECORDING_TEST_STREAMS,
      .stream_count = 2u,
      /* Small rings so bursts overflow while the writer sleeps. */
      .ring_bytes = 4096u,
      .chunk_bytes = KB(16),
  };
  VkrRecorder *recorder = vkr_recorder_start(&memory.allocator, &config);
  assert(recorder != NULL);

  RecordingProducerTest render = {.recorder = recorder,
                                  .producer = VKR_RECORDER_PRODUCER_RENDER,
                                  .stream = 2u};
  VkrThread thread = NULL;
  assert(vkr_thread_create(&test_memory.allocator, &thread,
                           recording_render_producer, &render));
  uint64_t camera_written = 0u;
  for (uint64_t frame = 0u; frame < RECORDING_TEST_FRAMES; ++frame) {
    const RecordingTestPose pose = recording_test_pose(frame);
    camera_written += vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                                         1u, frame, &pose, sizeof(pose))
                          ? 1u
                          : 0u;
  }
  assert(vkr_thread_join(thread));
  vkr_thread_destroy(&test_memory.allocator, &thread);
  VkrRecorderStats stats;
  assert(vkr_recorder_stop(recorder, &stats));
  assert(!stats.failed);
  assert(camera_written + stats.lost_records == RECORDING_TEST_FRAMES);
  assert(render.written + stats.dropped_records == RECORDING_TEST_FRAMES);
  printf("    camera %llu written, %llu lost; metrics %llu written, %llu "
         "dropped\n",
         (unsigned long long)camera_written,
         (unsigned long long)stats.lost_records,
         (unsigned long long)render.written,
         (unsigned long long)stats.dropped_records);

  VkrAllocator *allocator = &test_memory.allocator;
  VkrRecordingReader reader;
  assert(vkr_recording_reader_open(&reader, allocator, path) ==
         VKR_RECORDING_READ_OK);
  assert(reader.indexed && !reader.truncated);
  uint64_t camera = 0u;
  uint64_t metrics = 0u;
  uint64_t gaps = 0u;
  uint64_t gap_lost = 0u;
  uint64_t last_camera_frame = 0u;
  for (uint32_t i = 0u; i < reader.chunk_count; ++i) {
    if (reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    assert(vkr_recording_reader_chunk(&reader, i, vkr_depot_zstd_codec(),
                                      allocator, &payload, &size));
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      if (record.stream == 1u) {
        const RecordingTestPose expected = recording_test_pose(record.frame);
        assert(MemCompare(record.data, &expected, sizeof(expected)) == 0);
        assert(camera == 0u || record.frame > last_camera_frame);
        last_camera_frame = record.frame;
        ++camera;
      } else if (record.stream == 2u) {
        ++metrics;
      } else {
        assert(record.stream == VKR_RECORDING_STREAM_RECORDER &&
               record.size == sizeof(VkrRecorderGap));
        VkrRecorderGap gap;
        MemCopy(&gap, record.data, sizeof(gap));
        assert(gap.producer == VKR_RECORDER_PRODUCER_MAIN &&
               gap.lost_records > 0u && gap.first_frame <= gap.last_frame);
        gap_lost += gap.lost_records;
        ++gaps;
      }
    }
    assert(cursor == size);
    vkr_recording_reader_chunk_free(allocator, payload, size);
  }
  assert(camera == camera_written && metrics == render.written);
  assert(gap_lost == stats.lost_records);

  uint8_t *payload = NULL;
  uint64_t size = 0u;
  const uint32_t end_index =
      vkr_recording_reader_find(&reader, VKR_RECORDING_CHUNK_END);
  assert(end_index != UINT32_MAX);
  assert(vkr_recording_reader_chunk(&reader, end_index, NULL, allocator,
                                    &payload, &size));
  VkrRecordingEnd end;
  assert(vkr_recording_end_decode(payload, size, &end));
  vkr_recording_reader_chunk_free(allocator, payload, size);
  assert(end.lost_records == stats.lost_records &&
         end.dropped_records == stats.dropped_records &&
         end.records == camera + metrics + gaps);
  assert(end.first_frame == 0u && end.last_frame <= RECORDING_TEST_FRAMES);

  vkr_recording_reader_close(&reader);
  recording_test_remove(path);
  recording_memory_destroy(&test_memory);
  recording_memory_destroy(&memory);
  printf("  test_recorder_accounting PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* Pass rows                                                                 */

#define RECORDING_PASS_TEST_FRAMES 12u
/* Renderer frame index of recorded frame 0. */
#define RECORDING_PASS_TEST_BASE 100u

/* A pass table of renderer frame `source`: passes A and B, a culled C, and
   D from source 105 on; GPU times on even sources. */
static uint32_t recording_pass_table(uint64_t source,
                                     VkrRendererMetricsPassSample *out) {
  static const char *const names[] = {"A", "B", "C", "D"};
  const uint32_t count = source >= RECORDING_PASS_TEST_BASE + 5u ? 4u : 3u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrRendererMetricsPassSample *sample = &out[i];
    MemZero(sample, sizeof(*sample));
    sample->name[0] = names[i][0];
    sample->name_length = 1u;
    sample->cpu_ms = 0.25 * (float64_t)(i + 1u) + (float64_t)source * 1.0e-3;
    sample->gpu_ms = (float64_t)(i + 1u) + (float64_t)source * 1.0e-2;
    sample->gpu_valid = source % 2u == 0u;
    sample->gpu_source_frame_index = source;
    sample->culled = i == 2u;
  }
  return count;
}

/* Detects: rows written under the frame that collected them instead of the
   frame that rendered them, a repeated table written twice, a table from
   before the recording written at all, a culled pass recorded or named,
   ids that change between frames, the GPU flag on a frame without GPU
   times, and the sampled-frame schedule. */
static void test_recording_pass_rows(void) {
  printf("  Running test_recording_pass_rows...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  char path[1024];
  recording_test_path("passes", path, sizeof(path));
  /* A bundle directory, not a stream file. */
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .codec = vkr_depot_zstd_codec(),
      .metrics = metrics,
      .passes = true_v,
      .pass_gpu_period = 4u,
  };
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));

  VkrRendererMetricsPassSample samples[4];
  for (uint64_t f = 0u; f < RECORDING_PASS_TEST_FRAMES; ++f) {
    bool8_t timed = false_v;
    assert(vkr_recording_pass_gpu_frame(&recording, &timed));
    assert(timed == (f % 4u == 0u));
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    /* Rows arrive two frames late; before that the table describes a frame
       from before the recording. Frame 5 repeats frame 4's table. */
    const uint64_t index = RECORDING_PASS_TEST_BASE + f;
    uint64_t source = f >= 2u ? index - 2u : RECORDING_PASS_TEST_BASE - 7u;
    if (f == 5u) {
      source = index - 3u;
    }
    const VkrRendererMetricsPassTable table = {
        .samples = samples,
        .count = recording_pass_table(source, samples),
        .capacity = ArrayCount(samples),
        .cpu_frame_index = index,
    };
    vkr_recording_passes(&recording, f, index, &table);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  assert(data.frame_count == RECORDING_PASS_TEST_FRAMES);
  /* Every recording declares the player stream; with no player records
     there is no player path to align by. */
  assert(!data.has_player);
  assert(data.pass_count == 3u);
  const uint32_t a = vkr_recording_data_find_pass(&data, "A");
  const uint32_t b = vkr_recording_data_find_pass(&data, "B");
  const uint32_t d = vkr_recording_data_find_pass(&data, "D");
  assert(a == 0u && b == 1u && d == 2u);
  assert(vkr_recording_data_find_pass(&data, "C") == UINT32_MAX);
  /* Frames 0 to 9 rendered sources 100 to 109; frame 3's table was never
     current, and frames 10 and 11 had not completed. */
  assert(data.pass_frames == 9u);
  for (uint64_t f = 0u; f < RECORDING_PASS_TEST_FRAMES; ++f) {
    uint32_t count = 0u;
    const VkrRecordingPassRow *rows =
        vkr_recording_data_pass_rows(&data, f, &count);
    if (f == 3u || f >= 10u) {
      assert(count == 0u && rows == NULL);
      continue;
    }
    const uint64_t source = RECORDING_PASS_TEST_BASE + f;
    const uint32_t expected = source >= RECORDING_PASS_TEST_BASE + 5u ? 3u : 2u;
    assert(count == expected);
    const bool8_t gpu = source % 2u == 0u;
    assert(((data.pass_frame_flags[f] & VKR_RECORDING_PASSES_GPU_TIMED) !=
            0u) == gpu);
    for (uint32_t i = 0u; i < count; ++i) {
      /* The culled C sits between B and D in the table. */
      const uint32_t slot = rows[i].pass == d ? 3u : rows[i].pass;
      const float32_t cpu = (float32_t)(0.25 * (float64_t)(slot + 1u) +
                                        (float64_t)source * 1.0e-3);
      const float32_t gpu_ms =
          (float32_t)((float64_t)(slot + 1u) + (float64_t)source * 1.0e-2);
      assert(rows[i].cpu_ms == cpu);
      assert(((rows[i].flags & VKR_RECORDING_PASS_GPU_VALID) != 0u) == gpu);
      assert(rows[i].gpu_ms == (gpu ? gpu_ms : 0.0f));
    }
  }
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_pass_rows PASSED\n");
}

/* A graph snapshot of renderer frame `index`: A writes "color" and the
   buffer "args", B reads both and writes "out", and C, culled, reads "out";
   with `wide` D runs after B and the viewport is 1920x1080, else
   1280x720. */
static void recording_graph_snapshot(uint64_t index, bool8_t wide,
                                     VkrRgSnapshot *out) {
  MemZero(out, sizeof(*out));
  out->frame_index = index;
  out->viewport_width = wide ? 1920u : 1280u;
  out->viewport_height = wide ? 1080u : 720u;
  out->render_scale = 0.75f;
  const char *names = "ABCcolorargsoutRGBA16FtransientD";
  MemCopy(out->names, names, string_length(names));
  out->name_bytes = (uint32_t)string_length(names);
  const VkrRgSnapshotName a = {0u, 1u};
  const VkrRgSnapshotName b = {1u, 1u};
  const VkrRgSnapshotName c = {2u, 1u};
  const VkrRgSnapshotName color = {3u, 5u};
  const VkrRgSnapshotName args = {8u, 4u};
  const VkrRgSnapshotName result = {12u, 3u};
  const VkrRgSnapshotName format = {15u, 7u};
  const VkrRgSnapshotName lifetime = {22u, 9u};
  const VkrRgSnapshotName d = {31u, 1u};
  out->passes[0] = (VkrRgSnapshotPass){.name = a,
                                       .type = VKR_RG_PASS_TYPE_GRAPHICS,
                                       .order = 0u,
                                       .first_use = 0u,
                                       .use_count = 2u};
  out->passes[1] = (VkrRgSnapshotPass){.name = b,
                                       .type = VKR_RG_PASS_TYPE_COMPUTE,
                                       .order = 1u,
                                       .first_use = 2u,
                                       .use_count = 3u};
  /* Scheduled passes come first, then the culled C. */
  if (wide) {
    out->passes[2] = (VkrRgSnapshotPass){.name = d,
                                         .type = VKR_RG_PASS_TYPE_GRAPHICS,
                                         .order = 2u,
                                         .first_use = 6u};
  }
  out->passes[wide ? 3u : 2u] =
      (VkrRgSnapshotPass){.name = c,
                          .type = VKR_RG_PASS_TYPE_COMPUTE,
                          .flags = VKR_RG_SNAPSHOT_PASS_CULLED,
                          .order = UINT32_MAX,
                          .first_use = 5u,
                          .use_count = 1u};
  out->pass_count = wide ? 4u : 3u;
  out->uses[0] = (VkrRgSnapshotUse){
      .resource = color, .role = VKR_RG_SNAPSHOT_ROLE_COLOR, .write = true_v};
  out->uses[1] = (VkrRgSnapshotUse){.resource = args,
                                    .role = VKR_RG_SNAPSHOT_ROLE_WRITE,
                                    .buffer = true_v,
                                    .write = true_v};
  out->uses[2] =
      (VkrRgSnapshotUse){.resource = color, .role = VKR_RG_SNAPSHOT_ROLE_READ};
  out->uses[3] = (VkrRgSnapshotUse){
      .resource = args, .role = VKR_RG_SNAPSHOT_ROLE_READ, .buffer = true_v};
  out->uses[4] = (VkrRgSnapshotUse){
      .resource = result, .role = VKR_RG_SNAPSHOT_ROLE_WRITE, .write = true_v};
  out->uses[5] =
      (VkrRgSnapshotUse){.resource = result, .role = VKR_RG_SNAPSHOT_ROLE_READ};
  out->use_count = 6u;
  out->resources[0] = (VkrRgSnapshotResource){.name = color,
                                              .format = format,
                                              .lifetime = lifetime,
                                              .width = out->viewport_width,
                                              .height = out->viewport_height,
                                              .depth = 1u,
                                              .layers = 1u,
                                              .mip_levels = 1u,
                                              .samples = 1u,
                                              .bytes = 8ull * 1920u * 1080u,
                                              .first_order = 0u,
                                              .last_order = 1u};
  out->resources[1] =
      (VkrRgSnapshotResource){.name = args,
                              .lifetime = lifetime,
                              .flags = VKR_RG_SNAPSHOT_RESOURCE_BUFFER,
                              .bytes = 4096u,
                              .first_order = 0u,
                              .last_order = 1u};
  out->resources[2] =
      (VkrRgSnapshotResource){.name = result,
                              .format = format,
                              .lifetime = lifetime,
                              .flags = VKR_RG_SNAPSHOT_RESOURCE_EXPORTED,
                              .width = 64u,
                              .height = 64u,
                              .depth = 1u,
                              .layers = 1u,
                              .mip_levels = 7u,
                              .samples = 1u,
                              .bytes = 65536u,
                              .first_order = 1u,
                              .last_order = 1u};
  out->resource_count = 3u;
}

static bool8_t recording_text_is(const VkrRecordingGraph *graph,
                                 VkrRecordingGraphText text,
                                 const char *expected) {
  const String8 value = vkr_recording_graph_text(graph, text);
  return value.length == string_length(expected) &&
         MemCompare(value.str, expected, value.length) == 0;
}

/* Detects, for the Performance window's tabs: a graph filed under the frame
   that collected it instead of the frame that rendered it, a graph asked
   for again when no new pass ran or not asked when one did, pass,
   use and resource fields read at the wrong offsets, a use resolved to the
   wrong resource or a pass joined to the wrong recorded pass; allocator
   rows whose counts are not per frame or whose id lost its name; and
   census states read against the wrong ids, at the wrong frame, or
   unpacked at the wrong nibble. */
static void test_recording_graph_allocators_census(void) {
  printf("  Running test_recording_graph_allocators_census...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  char path[1024];
  recording_test_path("tabs", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .codec = vkr_depot_zstd_codec(),
      .metrics = metrics,
      .passes = true_v,
      .pass_gpu_period = 1u,
  };
  /* An allocator the registry samples each frame: frame f allocates f + 1
     blocks and frees one from frame 2 on. Registered before the recording,
     it counts from the recording's start, not its own. */
  VkrDMemory tracked_memory;
  assert(vkr_dmemory_create(KB(64), MB(1), &tracked_memory));
  VkrAllocator tracked = {.ctx = &tracked_memory};
  vkr_dmemory_allocator_create(&tracked);
  assert(vkr_allocator_register_name(&tracked, "test.tabs"));
  void *before =
      vkr_allocator_alloc(&tracked, 64u, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *blocks[128];
  uint32_t block_count = 0u;
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));

  static VkrRgSnapshot snapshot;
  VkrRendererMetricsPassSample samples[4];
  uint32_t graphs_asked = 0u;
  /* Census: objects 5, 9 and 12; candidates in the order 9, 5, 12, 9. */
  const VkrWorldObjectBounds bounds[] = {
      {.object_id = 5u, .min = {0.0f, 0.0f, 0.0f}, .max = {1.0f, 2.0f, 3.0f}},
      {.object_id = 9u, .min = {4.0f, 0.0f, 4.0f}, .max = {6.0f, 1.0f, 8.0f}},
      {.object_id = 12u,
       .min = {-2.0f, 0.0f, -2.0f},
       .max = {-1.0f, 1.0f, -1.0f}},
  };
  const uint32_t ids[] = {9u, 5u, 12u, 9u};
  for (uint64_t f = 0u; f < RECORDING_PASS_TEST_FRAMES; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    if (f == 0u) {
      vkr_recording_draw_bounds(&recording, f, 1u, bounds, ArrayCount(bounds),
                                32.0f);
      assert(!vkr_recording_draw_bounds_wanted(&recording, 1u));
      assert(vkr_recording_draw_bounds_wanted(&recording, 2u));
    }
    const uint64_t index = RECORDING_PASS_TEST_BASE + f;
    const uint64_t source = f >= 2u ? index - 2u : index;
    const VkrRendererMetricsPassTable table = {
        .samples = samples,
        .count = recording_pass_table(source, samples),
        .capacity = ArrayCount(samples),
        .cpu_frame_index = index,
    };
    vkr_recording_passes(&recording, f, index, &table);
    /* The first table and the one that adds D (source 105, rendered by
       frame 5 and collected at frame 7) ask for the graph. */
    if (vkr_recording_graph_wanted(&recording)) {
      ++graphs_asked;
      recording_graph_snapshot(source, graphs_asked > 1u, &snapshot);
      vkr_recording_graph(&recording, f, &snapshot);
    }
    for (uint64_t k = 0u; k <= f && block_count < ArrayCount(blocks); ++k) {
      blocks[block_count++] =
          vkr_allocator_alloc(&tracked, 64u, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    if (f >= 2u && block_count > 0u) {
      vkr_allocator_free(&tracked, blocks[--block_count], 64u,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    vkr_recording_allocators(&recording, f);
    /* Census of the source frame: candidate i's state is (source + i) % 6
       but never 0, which no camera candidate has. */
    uint32_t words[1] = {0u};
    for (uint32_t i = 0u; i < ArrayCount(ids); ++i) {
      const uint32_t state = 1u + (uint32_t)((source + i) % 5u);
      words[0] |= state << (4u * i);
    }
    const VkrRendererMetricsDrawCensus census = {
        .cpu_frame_index = index,
        .source_frame_index = source,
        .ids_generation = 7u,
        .states = words,
        .object_ids = ids,
        .candidate_count = ArrayCount(ids),
        .flags = VKR_DRAW_CENSUS_TWO_PHASE,
        .valid = true_v,
    };
    vkr_recording_draws(&recording, &census);
  }
  assert(graphs_asked == 2u);
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);
  for (uint32_t i = 0u; i < block_count; ++i) {
    vkr_allocator_free(&tracked, blocks[i], 64u,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  vkr_allocator_free(&tracked, before, 64u, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_unregister(&tracked);
  vkr_dmemory_allocator_destroy(&tracked);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  /* Graphs: under the frames that rendered sources 100 and 105, in order. */
  assert(data.graph_count == 2u);
  assert(data.graphs[0].frame == 0u && data.graphs[1].frame == 5u);
  assert(vkr_recording_data_graph_at(&data, 2u) == 0u);
  assert(vkr_recording_data_graph_at(&data, 9u) == 1u);
  const VkrRecordingGraph *graph = &data.graphs[1];
  assert(graph->viewport_width == 1920u && graph->viewport_height == 1080u);
  assert(graph->render_scale == 0.75f && !graph->truncated);
  assert(graph->pass_count == 4u && graph->scheduled_count == 3u);
  assert(recording_text_is(graph, graph->passes[1].name, "B"));
  assert(graph->passes[1].kind == VKR_RECORDING_GRAPH_KIND_COMPUTE);
  assert(graph->passes[3].flags & VKR_RECORDING_GRAPH_PASS_CULLED);
  assert(graph->passes[3].order == UINT32_MAX);
  assert(graph->passes[0].pass == vkr_recording_data_find_pass(&data, "A"));
  assert(graph->passes[2].pass == vkr_recording_data_find_pass(&data, "D"));
  /* C is culled, so no rows name it. */
  assert(graph->passes[3].pass == UINT32_MAX);
  assert(graph->passes[1].first_use == 2u && graph->passes[1].use_count == 3u);
  const VkrRecordingGraphUse *reads_args = &graph->uses[3];
  assert(reads_args->resource == 1u);
  assert(reads_args->flags == VKR_RECORDING_GRAPH_USE_BUFFER);
  assert(graph->uses[0].role == VKR_RECORDING_GRAPH_ROLE_COLOR);
  assert(graph->uses[4].flags == VKR_RECORDING_GRAPH_USE_WRITE);
  const VkrRecordingGraphResource *out = &graph->resources[2];
  assert(recording_text_is(graph, out->name, "out"));
  assert(recording_text_is(graph, out->format, "RGBA16F"));
  assert(recording_text_is(graph, out->lifetime, "transient"));
  assert(out->flags == VKR_RECORDING_GRAPH_RESOURCE_EXPORTED);
  assert(out->width == 64u && out->mip_levels == 7u && out->bytes == 65536u);
  assert(out->first_order == 1u && out->last_order == 1u);
  assert(graph->resources[1].flags == VKR_RECORDING_GRAPH_RESOURCE_BUFFER);

  /* Allocator rows: per-frame counts, under the allocator's name. */
  uint32_t tracked_id = UINT32_MAX;
  for (uint32_t i = 0u; i < data.allocator_count; ++i) {
    if (string_equals(data.allocators[i].name, "test.tabs")) {
      tracked_id = i;
    }
  }
  assert(tracked_id != UINT32_MAX);
  for (uint64_t f = 0u; f < RECORDING_PASS_TEST_FRAMES; ++f) {
    uint32_t count = 0u;
    const VkrRecordingAllocatorRow *rows =
        vkr_recording_data_allocator_rows(&data, f, &count);
    bool8_t found = false_v;
    for (uint32_t r = 0u; r < count; ++r) {
      if (rows[r].allocator != tracked_id) {
        continue;
      }
      found = true_v;
      assert(rows[r].allocs == f + 1u);
      assert(rows[r].frees == (f >= 2u ? 1u : 0u));
    }
    assert(found);
  }

  /* Census: the box set, the ids, and each frame's states under the frame
     that rendered its source; frames 10 and 11 rendered sources whose
     census never arrived. */
  assert(data.draw_bounds_count == 1u && data.draw_ids_count == 1u);
  const VkrRecordingDrawBounds *set = &data.draw_bounds[0];
  assert(set->object_count == 3u && set->cell_size == 32.0f);
  assert(set->objects[1].object_id == 9u && set->objects[1].max[2] == 8.0f);
  assert(data.draw_ids[0].generation == 7u && data.draw_ids[0].count == 4u);
  assert(data.draw_ids[0].ids[3] == 9u);
  for (uint64_t f = 0u; f < RECORDING_PASS_TEST_FRAMES; ++f) {
    VkrRecordingDrawFrame census;
    const bool8_t has = vkr_recording_data_draw_frame(&data, f, &census);
    assert(has == (f < 10u));
    if (!has) {
      continue;
    }
    const uint64_t source = RECORDING_PASS_TEST_BASE + f;
    assert(census.count == 4u && census.ids == &data.draw_ids[0]);
    assert(census.flags == VKR_RECORDING_DRAW_STATES_TWO_PHASE);
    for (uint32_t i = 0u; i < census.count; ++i) {
      assert(vkr_recording_draw_state(census.words, i) ==
             1u + (uint32_t)((source + i) % 5u));
    }
  }
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_graph_allocators_census PASSED\n");
}

/* Detects: a hardware or pipeline record the reader does not parse as the
   writer wrote it, facts written twice, a pipeline written again or skipped
   when the registry grows between frames, and statistics attached to the
   wrong pipeline. */
static void test_recording_hardware_pipelines(void) {
  printf("  Running test_recording_hardware_pipelines...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  char path[1024];
  recording_test_path("hardware", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .codec = vkr_depot_zstd_codec(),
      .metrics = metrics,
      .passes = true_v,
      .pass_gpu_period = 1u,
  };
  VkrPipelineRegistry registry;
  assert(vkr_pipeline_registry_create(&memory.allocator, &registry));
  /* Two pipelines before the recording: a register-limited kernel and a
     render pipeline with driver statistics. */
  const VkrPipelineInfo kernel = {
      .name = "cull.classify",
      .kind = VKR_PIPELINE_KIND_COMPUTE,
      .compile_ms = 3.5f,
      .max_threads = 512u,
      .execution_width = 32u,
      .threadgroup_memory = 4096u,
  };
  vkr_pipeline_registry_add(&registry, &kernel, NULL, 0u);
  const VkrPipelineInfo opaque = {
      .name = "tiled.opaque",
      .kind = VKR_PIPELINE_KIND_RENDER,
      .compile_ms = 12.25f,
      .registers = 96u,
      .spill_bytes = 256u,
      .instructions = 1800u,
  };
  const VkrPipelineStat opaque_stats[] = {
      {.name = "Fragment Shader: VGPRs", .value = 96.0},
      {.name = "Fragment Shader: Scratch size", .value = 256.0},
      {.name = "Vertex Shader: Instruction count", .value = 120.0},
  };
  vkr_pipeline_registry_add(&registry, &opaque, opaque_stats,
                            ArrayCount(opaque_stats));
  const VkrRecordingHardwareFact facts[] = {
      {.key = "GPU", .value = "Apple M1 Pro"},
      {.key = "GPU cores", .value = "14"},
  };
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  for (uint64_t f = 0u; f < 3u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    /* A variant built while frame 1 ran. */
    if (f == 1u) {
      const VkrPipelineInfo variant = {
          .name = "tiled.opaque.sun",
          .kind = VKR_PIPELINE_KIND_RENDER,
          .compile_ms = 8.0f,
      };
      const VkrPipelineStat variant_stats[] = {
          {.name = "Fragment Shader: VGPRs", .value = 104.0},
      };
      vkr_pipeline_registry_add(&registry, &variant, variant_stats,
                                ArrayCount(variant_stats));
    }
    vkr_recording_hardware(&recording, f, facts, ArrayCount(facts));
    vkr_recording_pipelines(&recording, f, &registry);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);
  vkr_pipeline_registry_destroy(&registry);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  assert(data.hardware_count == 2u);
  assert(string_equals(data.hardware[0].key, "GPU"));
  assert(string_equals(data.hardware[0].value, "Apple M1 Pro"));
  assert(string_equals(data.hardware[1].value, "14"));
  assert(data.pipeline_count == 3u && data.pipeline_stat_count == 4u);
  const VkrRecordingPipeline *read = &data.pipelines[0];
  assert(string_equals(read->name, "cull.classify") && read->frame == 0u);
  assert(read->kind == VKR_PIPELINE_KIND_COMPUTE && read->compile_ms == 3.5f);
  assert(read->max_threads == 512u && read->execution_width == 32u);
  assert(read->threadgroup_memory == 4096u && read->stat_count == 0u);
  read = &data.pipelines[1];
  assert(string_equals(read->name, "tiled.opaque") && read->frame == 0u);
  assert(read->registers == 96u && read->spill_bytes == 256u);
  assert(read->instructions == 1800u && read->stat_count == 3u);
  const VkrRecordingPipelineStat *stat =
      &data.pipeline_stats[read->stat_first + 2u];
  assert(string_equals(stat->name, "Vertex Shader: Instruction count"));
  assert(stat->value == 120.0);
  read = &data.pipelines[2];
  assert(string_equals(read->name, "tiled.opaque.sun") && read->frame == 1u);
  assert(read->stat_count == 1u);
  assert(data.pipeline_stats[read->stat_first].value == 104.0);
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_hardware_pipelines PASSED\n");
}

/* ------------------------------------------------------------------------ */
/* Entity poses                                                              */

static bool8_t recording_listed(VkrScene *scene, VkrEntityId entity) {
  uint32_t count = 0u;
  bool8_t overflow = false_v;
  const VkrEntityId *changed =
      vkr_scene_changed_entities(scene, &count, &overflow);
  assert(!overflow);
  uint32_t found = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    found += changed[i].u64 == entity.u64 ? 1u : 0u;
  }
  assert(found <= 1u);
  return found == 1u;
}

static uint32_t recording_changed_count(VkrScene *scene) {
  uint32_t count = 0u;
  bool8_t overflow = false_v;
  (void)vkr_scene_changed_entities(scene, &count, &overflow);
  return count;
}

/* Detects: a change lost when a later transform pass in the same frame
   clears WORLD_UPDATED, an entity listed twice, a child its parent carried
   listed although its relative pose stayed, a reparented or transient child
   of a moved parent left out, an evaluated override that is not a change, a
   partial consume that drops or keeps the wrong entries, changes listed
   while untracked, and a start that does not list the whole scene once. */
static void test_scene_change_tracking(void) {
  printf("  Running test_scene_change_tracking...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &memory.allocator, 31, 8, &error));
  VkrEntityId entities[4];
  for (uint32_t i = 0u; i < ArrayCount(entities); ++i) {
    entities[i] = vkr_scene_create_entity(&scene, &error);
    assert(entities[i].u64 && error == VKR_SCENE_ERROR_NONE);
    assert(vkr_scene_set_transform(&scene, entities[i], vec3_zero(),
                                   vkr_quat_identity(), vec3_one()));
  }
  const VkrEntityId parent = entities[0];
  const VkrEntityId child = entities[1];
  const VkrEntityId other = entities[2];
  const VkrEntityId spawned = entities[3];
  vkr_scene_set_parent(&scene, child, parent);
  assert(vkr_scene_set_transient(&scene, spawned));
  vkr_scene_set_parent(&scene, spawned, parent);
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 0u);

  /* Starting lists every entity once. */
  assert(vkr_scene_track_changes(&scene, true_v));
  assert(recording_changed_count(&scene) == 4u &&
         recording_listed(&scene, parent) && recording_listed(&scene, child) &&
         recording_listed(&scene, other) && recording_listed(&scene, spawned));
  vkr_scene_changed_entities_consume(&scene, 1u);
  assert(recording_changed_count(&scene) == 3u);
  vkr_scene_changed_entities_consume(&scene, 3u);
  assert(recording_changed_count(&scene) == 0u);

  /* A move survives a second pass in the same frame. Its child keeps its
     pose relative to it and is not listed; its transient child is. */
  vkr_scene_set_position(&scene, parent, vec3_new(1.0f, 2.0f, 3.0f));
  vkr_scene_update_transforms(&scene);
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 2u &&
         recording_listed(&scene, parent) && !recording_listed(&scene, child) &&
         !recording_listed(&scene, other) && recording_listed(&scene, spawned));
  /* A second move before the drain lists nothing twice. */
  vkr_scene_set_position(&scene, parent, vec3_new(4.0f, 2.0f, 3.0f));
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 2u);
  vkr_scene_changed_entities_consume(&scene, 2u);

  /* A child's own move is listed. */
  vkr_scene_set_position(&scene, child, vec3_new(0.0f, 1.0f, 0.0f));
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 1u &&
         recording_listed(&scene, child));
  vkr_scene_changed_entities_consume(&scene, 1u);

  /* An entity reparented after it was placed is listed whenever its new
     parent carries it. */
  vkr_scene_set_parent(&scene, other, parent);
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 1u &&
         recording_listed(&scene, other));
  vkr_scene_changed_entities_consume(&scene, 1u);
  vkr_scene_set_position(&scene, parent, vec3_new(5.0f, 2.0f, 3.0f));
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 3u &&
         recording_listed(&scene, parent) && recording_listed(&scene, other) &&
         !recording_listed(&scene, child));
  vkr_scene_changed_entities_consume(&scene, 3u);

  /* An evaluated override changes the world matrix. */
  const Mat4 pose = mat4_translate(vec3_new(0.0f, 5.0f, 0.0f));
  assert(vkr_scene_set_evaluated_transform(&scene, other, &pose));
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 1u &&
         recording_listed(&scene, other));

  /* Stopping clears the list; untracked changes list nothing. */
  assert(vkr_scene_track_changes(&scene, false_v));
  assert(recording_changed_count(&scene) == 0u);
  vkr_scene_set_position(&scene, parent, vec3_new(9.0f, 2.0f, 3.0f));
  vkr_scene_update_transforms(&scene);
  assert(recording_changed_count(&scene) == 0u);
  /* Tracking again starts from the whole scene, with no stale marks. */
  assert(vkr_scene_track_changes(&scene, true_v));
  assert(recording_changed_count(&scene) == 4u);

  vkr_scene_shutdown(&scene, NULL);
  recording_memory_destroy(&memory);
  printf("  test_scene_change_tracking PASSED\n");
}

static VkrRecordingPose recording_pose(uint8_t ref_byte, uint32_t node,
                                       float64_t x) {
  VkrRecordingPose pose = {.node = node,
                           .document = VKR_RECORDING_POSE_DOCUMENT_NONE,
                           .container = VKR_RECORDING_CONTAINER_ACTIVE,
                           .translation = {x, 1.0, -2.0},
                           .linear = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
  MemSet(&pose.ref, ref_byte, sizeof(pose.ref));
  return pose;
}

/* Detects: poses of one entity split across keys or mixed with another
   entity's, samples out of frame order after records arrive interleaved,
   a lookup that answers the next sample or one before the entity first
   moved, a node pose merged with its wrapper's own pose, a spawn named
   twice, a spawn's model replaced by a later report, its frames lost, and a
   spawn pose merged with a document one. */
static void test_recording_pose_playback(void) {
  printf("  Running test_recording_pose_playback...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("poses", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path,
      .codec = vkr_depot_zstd_codec(),
      .poses = true_v,
  };
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* Entity 0xAA moves at frames 0, 3 and 5; its node 2 at frame 1 only;
     entity 0x11 from frame 2. */
  for (uint32_t f = 0u; f < 7u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    if (f >= 2u) {
      const VkrRecordingPose pose = recording_pose(
          0x11u, VKR_RECORDING_POSE_NODE_NONE, 100.0 + (float64_t)f);
      vkr_recording_pose(&recording, &pose);
    }
    if (f == 0u || f == 3u || f == 5u) {
      /* Hidden from frame 3 to 5, as a first-person body is. */
      VkrRecordingPose pose =
          recording_pose(0xAAu, VKR_RECORDING_POSE_NODE_NONE, (float64_t)f);
      pose.flags = f == 3u ? VKR_RECORDING_POSE_FLAG_HIDDEN : 0u;
      vkr_recording_pose(&recording, &pose);
    }
    if (f == 1u) {
      const VkrRecordingPose pose = recording_pose(0xAAu, 2u, 50.0);
      vkr_recording_pose(&recording, &pose);
    }
    /* A spawn appears at frame 2, gets its model at 3 and goes at 5; a
       pose of its node 4 shares its id. */
    if (f == 2u) {
      const uint32_t id = vkr_recording_spawn_id(&recording, 0x1234u, 0u);
      assert(id == 0u && vkr_recording_spawn_id(&recording, 0x1234u, 0u) == 0u);
      VkrRecordingPose pose = recording_pose(0u, 4u, 70.0);
      pose.kind = VKR_RECORDING_POSE_KIND_SPAWN;
      pose.document = id;
      vkr_recording_pose(&recording, &pose);
    }
    if (f == 3u) {
      vkr_recording_spawn_look(&recording, 0u, string8_lit("a/body.vkb"),
                               string8_lit("a/body.vka"), NULL);
      vkr_recording_spawn_look(&recording, 0u, string8_lit("ignored.vkb"),
                               (String8){0}, NULL);
      /* A second spawn gets a shape. */
      const uint32_t box = vkr_recording_spawn_id(&recording, 0x5678u, 0u);
      const VkrRecordingSpawnShape shape = {
          .size = {0.6f, 1.8f, 0.6f}, .color = {0.15f, 0.45f, 0.85f, 1.0f}};
      vkr_recording_spawn_look(&recording, box, (String8){0}, (String8){0},
                               &shape);
    }
    if (f == 5u) {
      vkr_recording_despawn(&recording, 0u);
    }
    vkr_recording_poses_flush(&recording);
  }
  assert(vkr_recording_stop(&recording, NULL));

  static VkrRecordingPlayback playback;
  char error[192];
  assert(vkr_recording_playback_open(&playback, path, vkr_depot_zstd_codec(),
                                     NULL, error, sizeof(error)));
  assert(playback.pose_key_count == 4u && playback.pose_sample_count == 10u);
  assert(playback.spawn_count == 2u && playback.spawns[0].first_frame == 2u &&
         playback.spawns[0].last_frame == 5u && !playback.spawns[0].has_shape);
  assert(playback.spawns[1].first_frame == 3u &&
         playback.spawns[1].last_frame == UINT64_MAX &&
         playback.spawns[1].has_shape &&
         playback.spawns[1].shape.size[1] == 1.8f &&
         playback.spawns[1].shape.color[2] == 0.85f);
  String8 none_mesh;
  String8 none_animation;
  assert(!vkr_recording_playback_spawn_model(&playback, 1u, &none_mesh,
                                             &none_animation));
  String8 mesh;
  String8 animation;
  assert(vkr_recording_playback_spawn_model(&playback, 0u, &mesh, &animation));
  assert(vkr_string8_equals_cstr(&mesh, "a/body.vkb") &&
         vkr_string8_equals_cstr(&animation, "a/body.vka"));
  uint32_t spawn_key = UINT32_MAX;
  for (uint32_t k = 0u; k < playback.pose_key_count; ++k) {
    if (playback.pose_keys[k].kind == VKR_RECORDING_POSE_KIND_SPAWN) {
      spawn_key = k;
    }
  }
  assert(spawn_key != UINT32_MAX && playback.pose_keys[spawn_key].node == 4u &&
         playback.pose_keys[spawn_key].document == 0u);
  uint32_t wrapper = UINT32_MAX;
  uint32_t node = UINT32_MAX;
  uint32_t mover = UINT32_MAX;
  for (uint32_t k = 0u; k < playback.pose_key_count; ++k) {
    const VkrRecordingPoseKey *key = &playback.pose_keys[k];
    const uint8_t byte = ((const uint8_t *)&key->ref)[0];
    if (key->kind == VKR_RECORDING_POSE_KIND_SPAWN) {
      continue;
    }
    if (byte == 0xAAu && key->node == VKR_RECORDING_POSE_NODE_NONE) {
      wrapper = k;
    } else if (byte == 0xAAu && key->node == 2u) {
      node = k;
    } else if (byte == 0x11u) {
      mover = k;
    }
  }
  assert(wrapper != UINT32_MAX && node != UINT32_MAX && mover != UINT32_MAX);
  assert(playback.pose_keys[wrapper].count == 3u &&
         playback.pose_keys[node].count == 1u &&
         playback.pose_keys[mover].count == 5u);
  /* At frame 4 the wrapper shows its frame 3 pose, at 6 its frame 5 pose. */
  uint32_t sample = vkr_recording_playback_pose_at(&playback, wrapper, 4u);
  assert(sample != UINT32_MAX && playback.pose_samples[sample].frame == 3u &&
         playback.pose_samples[sample].translation[0] == 3.0);
  assert(playback.pose_samples[sample].flags == VKR_RECORDING_POSE_FLAG_HIDDEN);
  sample = vkr_recording_playback_pose_at(&playback, wrapper, 6u);
  assert(playback.pose_samples[sample].frame == 5u &&
         playback.pose_samples[sample].flags == 0u);
  sample = vkr_recording_playback_pose_at(&playback, wrapper, 0u);
  assert(playback.pose_samples[sample].frame == 0u);
  /* Before its first pose an entity has none. */
  assert(vkr_recording_playback_pose_at(&playback, mover, 1u) == UINT32_MAX);
  sample = vkr_recording_playback_pose_at(&playback, mover, 2u);
  assert(playback.pose_samples[sample].translation[0] == 102.0);
  sample = vkr_recording_playback_pose_at(&playback, node, 6u);
  assert(playback.pose_samples[sample].translation[0] == 50.0);
  for (uint32_t k = 0u; k < playback.pose_key_count; ++k) {
    const VkrRecordingPoseKey *key = &playback.pose_keys[k];
    for (uint32_t i = 1u; i < key->count; ++i) {
      assert(playback.pose_samples[key->first + i - 1u].frame <
             playback.pose_samples[key->first + i].frame);
    }
  }
  vkr_recording_playback_close(&playback);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_pose_playback PASSED\n");
}

/* Detects: a frame that keeps its first tick's player instead of its last,
   a player on a frame it did not run on, a wrong speed, player columns
   offered for a recording without a player, and tags or events whose
   lengths, text or payload do not survive the stream. */
static void test_recording_player_records(void) {
  printf("  Running test_recording_player_records...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("player", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec()};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* Frames 1 and 2 run two ticks each; frame 0 and 3 run none. */
  uint64_t tick = 1u;
  for (uint32_t f = 0u; f < 4u; ++f) {
    for (uint32_t t = 0u; f >= 1u && f <= 2u && t < 2u; ++t) {
      const VkrRecordingPlayer player = {
          .tick = tick,
          .position = {(float64_t)tick, 0.5, -2.0},
          .velocity = {3.0f, 0.0f, 4.0f},
          .flags = 1u,
      };
      vkr_recording_player(&recording, &player);
      ++tick;
    }
    if (f == 2u) {
      vkr_recording_tag(&recording, string8_lit("weapon"),
                        string8_lit("rifle"));
      const uint32_t damage = 37u;
      vkr_recording_event(&recording, string8_lit("hit"), &damage,
                          sizeof(damage));
    }
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
  }
  assert(vkr_recording_stop(&recording, NULL));

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  assert(data.frame_count == 4u && data.has_player);
  assert(!data.player_valid[0] && !data.player_valid[3]);
  assert(data.player_valid[1] && data.player_positions[1u * 3u] == 2.0);
  assert(data.player_valid[2] && data.player_positions[2u * 3u] == 4.0 &&
         data.player_positions[2u * 3u + 1u] == 0.5);
  assert(data.player_speeds[2] == 5.0f);
  assert(vkr_recording_data_has_field(&data, VKR_RECORDING_PLAYER_X_FIELD) &&
         vkr_recording_data_has_field(&data, VKR_RECORDING_PLAYER_SPEED_FIELD));
  vkr_recording_data_free(&data);

  /* The tag and the event, read back from the stream. */
  VkrRecordingReader reader;
  char stream[1100];
  string_format(stream, sizeof(stream), "%s/%s", path,
                VKR_RECORDING_STREAM_FILE);
  assert(vkr_recording_reader_open(&reader, &memory.allocator, stream) ==
         VKR_RECORDING_READ_OK);
  uint32_t tags = 0u;
  uint32_t events = 0u;
  for (uint32_t i = 0u; i < reader.chunk_count; ++i) {
    if (reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    assert(vkr_recording_reader_chunk(&reader, i, vkr_depot_zstd_codec(),
                                      &memory.allocator, &payload, &size));
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      if (record.stream == VKR_RECORDING_STREAM_TAG) {
        assert(record.frame == 2u && record.size == 2u + 6u + 5u &&
               record.data[0] == 6u && record.data[1] == 0u &&
               MemCompare(record.data + 2u, "weaponrifle", 11u) == 0);
        ++tags;
      } else if (record.stream == VKR_RECORDING_STREAM_EVENT) {
        uint32_t damage = 0u;
        MemCopy(&damage, record.data + 2u + 3u, sizeof(damage));
        assert(record.frame == 2u && record.size == 2u + 3u + 4u &&
               MemCompare(record.data + 2u, "hit", 3u) == 0 && damage == 37u);
        ++events;
      }
    }
    vkr_recording_reader_chunk_free(&memory.allocator, payload, size);
  }
  vkr_recording_reader_close(&reader);
  assert(tags == 1u && events == 1u);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_player_records PASSED\n");
}

/* Detects: actions replayed on the wrong driven tick or out of sequence,
   a driven run starting at another tick than the recording, actions of a
   skipped tick replayed late, a distance measured against the wrong
   recorded tick, a divergence reported inside the tolerance or at the
   wrong tick or frame, a drive that never finishes, and a start or a
   reference that is not the recorded player of the replayed tick. */
static void test_recording_drive(void) {
  printf("  Running test_recording_drive...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("drive", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec()};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* Recorded ticks 40 to 49, one per frame: the player walks 1 m a tick
     along x; actions at ticks 41 (two, written out of order) and 45. */
  for (uint64_t tick = 40u; tick < 50u; ++tick) {
    const VkrRecordingPlayer player = {
        .tick = tick, .position = {(float64_t)(tick - 40u), 0.0, 0.0}};
    vkr_recording_player(&recording, &player);
    if (tick == 41u) {
      const VkrRecordingAction second = {
          .tick = 41u, .sequence = 8u, .action = 2u, .pressed = 1u};
      const VkrRecordingAction first = {
          .tick = 41u, .sequence = 7u, .action = 1u, .pressed = 1u, .x = 0.5f};
      vkr_recording_action(&recording, &second);
      vkr_recording_action(&recording, &first);
    }
    if (tick == 45u) {
      const VkrRecordingAction release = {
          .tick = 45u, .sequence = 9u, .action = 1u, .pressed = 0u};
      vkr_recording_action(&recording, &release);
    }
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
  }
  assert(vkr_recording_stop(&recording, NULL));

  static VkrRecordingDrive drive;
  char error[192];
  assert(vkr_recording_drive_open(&drive, path, vkr_depot_zstd_codec(), error,
                                  sizeof(error)));
  assert(drive.first_tick == 40u && drive.last_tick == 49u &&
         drive.action_count == 3u && drive.player_count == 10u);
  /* Drive starts from the first recorded player; before the base tick no
     tick has a reference. */
  assert(vkr_recording_drive_start_player(&drive)->tick == 40u);
  assert(vkr_recording_drive_reference(&drive, 1000u) == NULL);
  /* The driven run starts at tick 1000, which replays tick 40. */
  VkrRecordingAction out[8];
  assert(vkr_recording_drive_actions(&drive, 1000u, out, 8u) == 0u);
  float64_t position[3] = {0.0, 0.0, 0.0};
  vkr_recording_drive_player(&drive, 1000u, position);
  assert(drive.distance == 0.0);
  const uint32_t count = vkr_recording_drive_actions(&drive, 1001u, out, 8u);
  assert(count == 2u && out[0].action == 1u && out[0].x == 0.5f &&
         out[1].action == 2u);
  position[0] = 1.1;
  vkr_recording_drive_player(&drive, 1001u, position);
  assert(drive.divergent_tick == UINT64_MAX && drive.max_distance > 0.09);
  /* Ticks 1002 to 1005 are skipped: the release at recorded 45 is not
     replayed late at 1006. */
  assert(vkr_recording_drive_actions(&drive, 1006u, out, 8u) == 0u);
  const VkrRecordingPlayer *reference =
      vkr_recording_drive_reference(&drive, 1006u);
  assert(reference && reference->tick == 46u && reference->position[0] == 6.0);
  position[0] = 6.5;
  vkr_recording_drive_player(&drive, 1006u, position);
  assert(drive.divergent_tick == 46u && drive.divergent_frame == 6u &&
         drive.distance == 0.5);
  assert(!drive.finished);
  (void)vkr_recording_drive_actions(&drive, 1009u, out, 8u);
  assert(drive.finished && drive.ticks_compared == 3u);
  vkr_recording_drive_close(&drive);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_drive PASSED\n");
}

/* The hashes of re-simulation test tick `tick`: one module, "fps". */
static VkrRecordingTickHash recording_test_tick_hash(uint64_t tick) {
  VkrRecordingTickHash hash = {.tick = tick, .bodies = 7u, .module_count = 1u};
  for (uint32_t b = 0; b < VKR_RECORDING_HASH_BUCKETS; ++b) {
    hash.transforms[b] = (uint32_t)(tick * 31u + b);
  }
  hash.modules[0] = (uint32_t)(tick * 977u);
  return hash;
}

/* Detects: a re-simulation without the recorded seed, a clock that skips or
   repeats a frame, sim frames or hashes kept before the session started or
   after it restarted, hashes compared against another tick, a module hash
   matched by index instead of name, a mismatch that is not reported at its
   tick and frame with its subsystems and buckets, a drive that keeps
   matching after it diverged, and a model join admitted before its
   recorded tick, taken from outside the first session, or matched to a
   wrapper's earlier model; the starting cells or a frame's streaming
   source lost or taken from the restart. */
static void test_recording_resim(void) {
  printf("  Running test_recording_resim...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("resim", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec()};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  const char *names[] = {"fps"};
  /* Frames 0 and 1 run before Play: neither their clock nor a hash
     counts. Play starts at frame 2 with ticks 1 to 6, two a frame; a restart
     at frame 5 starts a session that re-simulation ignores. */
  for (uint64_t f = 0u; f < 7u; ++f) {
    if (f == 2u) {
      vkr_recording_session(&recording, 0x5eedull);
      const int32_t cells[] = {-1, 0, 2, -3};
      vkr_recording_cells(&recording, cells, 2u);
    } else if (f == 5u) {
      vkr_recording_session(&recording, 0xbadull);
      const int32_t other[] = {9, 9};
      vkr_recording_cells(&recording, other, 1u);
    }
    const VkrRecordingSimFrame clock = {.now = 100.0 + (float64_t)f,
                                        .delta = 0.03,
                                        .source = {(float64_t)f, 1.5, -2.0},
                                        .has_source = 1u};
    vkr_recording_sim_frame(&recording, &clock);
    const VkrRecordingTickHash before = recording_test_tick_hash(99u);
    if (f < 2u) {
      vkr_recording_tick_hash(&recording, &before, names);
    }
    /* A model joins before the session (dropped), after 2 ticks of the
       first session (kept) and in the second session (ignored). */
    if (f == 1u || f == 3u || f == 6u) {
      const VkrRecordingJoin join = {.ticks = f == 3u ? 2u : 1u,
                                     .entity = 0x40u + f,
                                     .kind = VKR_RECORDING_JOIN_MODEL};
      vkr_recording_join(&recording, &join);
    }
    /* The same wrapper takes a second model after 4 ticks. */
    if (f == 4u) {
      const VkrRecordingJoin again = {
          .ticks = 4u, .entity = 0x43u, .kind = VKR_RECORDING_JOIN_MODEL};
      vkr_recording_join(&recording, &again);
    }
    for (uint64_t t = 0u; f >= 2u && t < 2u; ++t) {
      const VkrRecordingTickHash hash =
          recording_test_tick_hash((f - 2u) * 2u + t + 1u);
      vkr_recording_tick_hash(&recording, &hash, names);
    }
    const VkrRecordingPlayer player = {.tick = f + 1u};
    vkr_recording_player(&recording, &player);
    const VkrRecordingAction action = {.tick = f + 1u, .sequence = f};
    vkr_recording_action(&recording, &action);
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
  }
  assert(vkr_recording_stop(&recording, NULL));

  static VkrRecordingDrive drive;
  char error[192];
  assert(vkr_recording_drive_open(&drive, path, vkr_depot_zstd_codec(), error,
                                  sizeof(error)));
  /* The first session: frames 2 to 4, ticks 1 to 6. */
  assert(drive.has_session && drive.seed == 0x5eedull);
  /* The first session's starting cells, not the restart's. */
  assert(drive.has_start_cells && drive.start_cell_count == 2u &&
         drive.start_cells[0] == -1 && drive.start_cells[1] == 0 &&
         drive.start_cells[2] == 2 && drive.start_cells[3] == -3);
  assert(drive.sim_frame_count == 3u && drive.hash_count == 6u &&
         drive.module_name_count == 1u &&
         strcmp(drive.module_names[0], "fps") == 0);
  drive.resim = true_v;
  VkrRecordingSimFrame clock;
  for (uint64_t f = 2u; f < 5u; ++f) {
    VkrRecordingSimFrame peeked;
    assert(vkr_recording_drive_peek_sim_frame(&drive, &peeked) &&
           peeked.now == 100.0 + (float64_t)f);
    assert(vkr_recording_drive_sim_frame(&drive, &clock) &&
           clock.now == 100.0 + (float64_t)f && clock.delta == 0.03 &&
           clock.has_source && clock.source[0] == (float64_t)f &&
           clock.source[2] == -2.0);
  }
  assert(!vkr_recording_drive_sim_frame(&drive, &clock) && drive.finished);
  drive.finished = false_v;

  /* Only the first session's join counts; it waits for its 2 ticks, and a
     model the recording never joined waits for good. */
  assert(drive.join_count == 2u);
  assert(!vkr_recording_drive_may_join(&drive, VKR_RECORDING_JOIN_MODEL, 0x43u,
                                       1u));
  assert(vkr_recording_drive_may_join(&drive, VKR_RECORDING_JOIN_MODEL, 0x43u,
                                      2u));
  assert(!vkr_recording_drive_may_join(&drive, VKR_RECORDING_JOIN_MODEL, 0x46u,
                                       9u));
  /* Once the first joined, the wrapper's second model waits for tick 4. */
  vkr_recording_drive_joined(&drive, VKR_RECORDING_JOIN_MODEL, 0x43u);
  assert(drive.join_applied[0] && !drive.join_applied[1]);
  assert(!vkr_recording_drive_may_join(&drive, VKR_RECORDING_JOIN_MODEL, 0x43u,
                                       3u));
  assert(vkr_recording_drive_may_join(&drive, VKR_RECORDING_JOIN_MODEL, 0x43u,
                                      4u));

  /* A live run whose module list holds a module the recording lacks before
     "fps" matches "fps" by name and reports the stranger. */
  const char *live_names[] = {"tools", "fps"};
  const VkrRecordingTickHash recorded = recording_test_tick_hash(1u);
  VkrRecordingTickHash live = recorded;
  live.module_count = 2u;
  live.modules[0] = 12345u;
  live.modules[1] = recorded.modules[0];
  assert(!vkr_recording_drive_check_hash(&drive, &live, live_names));
  assert(!vkr_recording_drive_check_hash(&drive, &live, live_names));
  /* "tools" is not in the recording: that alone differs. */
  assert(drive.hash_diverged && drive.hash_divergent_tick == 1u &&
         drive.hash_divergent_frame == 2u &&
         strcmp(drive.hash_subsystems, "module tools (not in the recording)") ==
             0);

  /* A run with the recording's modules matches until tick 5, where the
     module and bucket 3 differ. */
  drive.hash_diverged = false_v;
  drive.hash_subsystems[0] = '\0';
  drive.hash_cursor = 0u;
  drive.ticks_hashed = 0u;
  for (uint64_t tick = 1u; tick <= 6u; ++tick) {
    VkrRecordingTickHash run = recording_test_tick_hash(tick);
    if (tick == 5u) {
      run.modules[0] ^= 1u;
      run.transforms[3] ^= 1u;
    }
    const bool8_t matched = vkr_recording_drive_check_hash(&drive, &run, names);
    assert(matched == (tick < 5u));
  }
  assert(drive.ticks_hashed == 4u && drive.hash_divergent_tick == 5u &&
         drive.hash_divergent_frame == 4u && drive.hash_buckets == (1u << 3) &&
         strcmp(drive.hash_subsystems, "module fps, transforms") == 0);
  vkr_recording_drive_close(&drive);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_resim PASSED\n");
}

/* Detects: gpu.submission written on the frame that published it instead
   of the frame that rendered it, a repeated completion written twice, a
   GPU time from before the recording filed on a recorded frame, and the
   partial record overwriting the frame's other metrics. */
static void test_recording_gpu_submission(void) {
  printf("  Running test_recording_gpu_submission...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  VkrMetricId wall = VKR_METRIC_ID_INVALID;
  VkrMetricId gpu = VKR_METRIC_ID_INVALID;
  const VkrMetricDescription wall_desc = {
      .name = string8_lit("frame.wall"),
      .domain = VKR_METRIC_DOMAIN_FRAME,
      .kind = VKR_METRIC_KIND_DURATION,
      .unit = VKR_METRIC_UNIT_NANOSECONDS,
      .scalar = VKR_METRIC_SCALAR_U64,
      .writer = VKR_METRIC_WRITER_RENDER_THREAD,
  };
  VkrMetricDescription gpu_desc = wall_desc;
  gpu_desc.name = string8_lit("gpu.submission");
  assert(vkr_metrics_register(metrics, &wall_desc, &wall));
  assert(vkr_metrics_register(metrics, &gpu_desc, &gpu));
  assert(vkr_metrics_seal(metrics));

  char path[1024];
  recording_test_path("gpu", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path, .codec = vkr_depot_zstd_codec(), .metrics = metrics};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* Renderer frames 200 to 211, one per recorded frame; each publishes the
     GPU time of the frame 3 back, (5 + n) ms for renderer frame 200 + n.
     Frame 6 repeats frame 5's completion, so frame 203's time never comes. */
  const uint64_t base = 200u;
  for (uint64_t f = 0u; f < 12u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    const uint64_t index = base + f;
    const uint64_t source = f == 6u ? index - 4u : index - 3u;
    const uint64_t gpu_ns = (5u + source - (base - 3u)) * 1000000u - 3000000u;
    vkr_metrics_begin_frame(metrics, index, index);
    vkr_metrics_duration_add_ns(metrics, wall, (16u + f) * 1000000u);
    vkr_metrics_duration_add_ns(metrics, gpu, gpu_ns);
    assert(vkr_metrics_end_frame(metrics));
    VkrMetricsSnapshotView view = {0};
    assert(vkr_metrics_snapshot_acquire(metrics, &view));
    vkr_recording_metrics(&recording, f, view.frame, NULL);
    vkr_recording_passes(&recording, f, index, NULL);
    vkr_recording_gpu_submission(&recording, source,
                                 (float64_t)gpu_ns * 1.0e-6);
    vkr_metrics_snapshot_release(metrics, &view);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  const uint32_t wall_series =
      vkr_recording_data_find_series(&data, "frame.wall");
  const uint32_t gpu_series =
      vkr_recording_data_find_series(&data, "gpu.submission");
  assert(wall_series != UINT32_MAX && gpu_series != UINT32_MAX);
  for (uint64_t f = 0u; f < 12u; ++f) {
    assert(data.series[wall_series].valid[f]);
    assert(data.series[wall_series].values[f] == (float64_t)(16u + f));
    /* Frames 0 to 8 rendered the sources 200 to 208 that completed; 3 never
       came, and 9 to 11 were in flight at the stop. */
    const bool8_t expected = f <= 8u && f != 3u;
    assert(data.series[gpu_series].valid[f] == expected);
    if (expected) {
      assert(data.series[gpu_series].values[f] == (float64_t)(5u + f));
    }
  }
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_gpu_submission PASSED\n");
}

/* Detects: an all-measurements recording losing a catalog metric past
   the first page of VKR_RECORDING_MEASUREMENT_PAGE, writing a Tier 0 field
   twice, a duration with no samples read as a zero, a nanosecond unit left
   unscaled, and the analysis missing the measurement series. */
static void test_recording_all_measurements(void) {
  printf("  Running test_recording_all_measurements...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  VkrMetricId wall = VKR_METRIC_ID_INVALID;
  const VkrMetricDescription wall_desc = {
      .name = string8_lit("frame.wall"),
      .domain = VKR_METRIC_DOMAIN_FRAME,
      .kind = VKR_METRIC_KIND_DURATION,
      .unit = VKR_METRIC_UNIT_NANOSECONDS,
      .scalar = VKR_METRIC_SCALAR_U64,
      .writer = VKR_METRIC_WRITER_RENDER_THREAD,
  };
  assert(vkr_metrics_register(metrics, &wall_desc, &wall));
  /* More gauges than one page holds, so the last ones land on a second. */
  enum { GAUGE_COUNT = VKR_RECORDING_MEASUREMENT_PAGE + 5u };
  VkrMetricId gauges[GAUGE_COUNT];
  char gauge_names[GAUGE_COUNT][24];
  for (uint32_t i = 0u; i < GAUGE_COUNT; ++i) {
    string_format(gauge_names[i], sizeof(gauge_names[i]), "test.gauge.%02u", i);
    const VkrMetricDescription desc = {
        .name = string8_create_from_cstr((const uint8_t *)gauge_names[i],
                                         string_length(gauge_names[i])),
        .domain = VKR_METRIC_DOMAIN_FRAME,
        .kind = VKR_METRIC_KIND_GAUGE,
        .unit = VKR_METRIC_UNIT_COUNT,
        .scalar = VKR_METRIC_SCALAR_U64,
        .writer = VKR_METRIC_WRITER_RENDER_THREAD,
    };
    assert(vkr_metrics_register(metrics, &desc, &gauges[i]));
  }
  VkrMetricId span = VKR_METRIC_ID_INVALID;
  VkrMetricDescription span_desc = wall_desc;
  span_desc.name = string8_lit("test.span");
  assert(vkr_metrics_register(metrics, &span_desc, &span));
  assert(vkr_metrics_seal(metrics));

  char path[1024];
  recording_test_path("measure", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec(),
                                     .metrics = metrics,
                                     .all_measurements = true_v};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  assert(recording.measurement_count == GAUGE_COUNT + 1u);
  /* Gauge i reads 100 f + i; the span runs on odd frames for (2 + f) ms. */
  const uint64_t frames = 8u;
  for (uint64_t f = 0u; f < frames; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    vkr_metrics_begin_frame(metrics, f, f);
    vkr_metrics_duration_add_ns(metrics, wall, 16000000u);
    for (uint32_t i = 0u; i < GAUGE_COUNT; ++i) {
      vkr_metrics_gauge_set_u64(metrics, gauges[i], 100u * f + i);
    }
    if (f % 2u == 1u) {
      vkr_metrics_duration_add_ns(metrics, span, (2u + f) * 1000000u);
    }
    assert(vkr_metrics_end_frame(metrics));
    VkrMetricsSnapshotView view = {0};
    assert(vkr_metrics_snapshot_acquire(metrics, &view));
    vkr_recording_metrics(&recording, f, view.frame, NULL);
    vkr_metrics_snapshot_release(metrics, &view);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  uint32_t wall_count = 0u;
  for (uint32_t i = 0u; i < data.series_count; ++i) {
    wall_count += string_equals(data.series[i].name, "frame.wall") ? 1u : 0u;
  }
  assert(wall_count == 1u);
  for (uint32_t i = 0u; i < GAUGE_COUNT; ++i) {
    const uint32_t series =
        vkr_recording_data_find_series(&data, gauge_names[i]);
    assert(series != UINT32_MAX);
    assert(string_equals(data.series[series].unit, "count"));
    for (uint64_t f = 0u; f < frames; ++f) {
      assert(data.series[series].valid[f]);
      assert(data.series[series].values[f] == (float64_t)(100u * f + i));
    }
  }
  const uint32_t span_series =
      vkr_recording_data_find_series(&data, "test.span");
  assert(span_series != UINT32_MAX);
  assert(string_equals(data.series[span_series].unit, "ms"));
  for (uint64_t f = 0u; f < frames; ++f) {
    assert(data.series[span_series].valid[f] == (f % 2u == 1u));
    if (f % 2u == 1u) {
      assert(data.series[span_series].values[f] == (float64_t)(2u + f));
    }
  }
  VkrRecordingAnalysis analysis;
  assert(vkr_recording_analyze(&data, NULL, &memory.allocator, &analysis));
  const VkrRecordingStats *span_stats = &analysis.series[span_series];
  assert(span_stats->valid == frames / 2u);
  assert(span_stats->min == 3.0 && span_stats->max == 9.0);
  vkr_recording_analysis_free(&analysis);
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_all_measurements PASSED\n");
}

/* Detects: an idle Play session (tick hashes, no actions or player
   track) refused for re-simulation, its drive spanning no ticks, and a
   recording with neither input nor a session opened. */
static void test_recording_resim_idle(void) {
  printf("  Running test_recording_resim_idle...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("resim_idle", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec()};
  const char *names[] = {"fps"};
  static VkrRecording recording;
  static VkrRecordingDrive drive;
  char error[192];
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    /* Pass 0 records an idle session of ticks 1 to 4; pass 1 records
       frames only. */
    assert(vkr_recording_start(&recording, &config));
    for (uint64_t f = 0u; f < 4u; ++f) {
      if (pass == 0u) {
        if (f == 0u) {
          vkr_recording_session(&recording, 7u);
        }
        const VkrRecordingSimFrame clock = {.now = (float64_t)f,
                                            .delta = 1.0 / 60.0};
        vkr_recording_sim_frame(&recording, &clock);
        const VkrRecordingTickHash hash = recording_test_tick_hash(f + 1u);
        vkr_recording_tick_hash(&recording, &hash, names);
      }
      const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
      const VkrRecordingCamera camera = {.fov = 60.0f};
      vkr_recording_frame(&recording, &frame, &camera);
    }
    assert(vkr_recording_stop(&recording, NULL));
    const bool8_t opened = vkr_recording_drive_open(
        &drive, path, vkr_depot_zstd_codec(), error, sizeof(error));
    if (pass == 0u) {
      assert(opened && drive.action_count == 0u && drive.player_count == 0u);
      assert(drive.first_tick == 1u && drive.last_tick == 4u);
      vkr_recording_drive_close(&drive);
    } else {
      assert(!opened && strstr(error, "no actions") != NULL);
    }
    char file[1100];
    string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
    recording_test_remove(file);
    string_format(file, sizeof(file), "%s/%s", path,
                  VKR_RECORDING_SUMMARY_FILE);
    recording_test_remove(file);
    const FilePath bundle = file_path_view(path);
    assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  }
  recording_memory_destroy(&memory);
  printf("  test_recording_resim_idle PASSED\n");
}

/* Detects: a render-thread publication's frame-loop work (the completing
   iteration's update, and its frame build when it built before the
   completion) left on the published frame instead of the iteration's own,
   so a slow update sits a row before the frame.wall it lengthened; the
   first iteration's update lost with the frame from before the recording
   that it published; and the split losing the published frame's other
   fields. */
/* Records 20 loop frames of 1/120 s whose even frames build a Scene: the
   graph build and the draw count on them, a draw count of zero on the
   others, and with `interval` a frame.scene_interval of 15 ms. */
static void recording_scene_frames(RecordingTestMemory *memory,
                                   const char *path, bool8_t interval) {
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory->allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  VkrMetricDescription desc = {
      .name = string8_lit("frame.wall"),
      .domain = VKR_METRIC_DOMAIN_FRAME,
      .kind = VKR_METRIC_KIND_DURATION,
      .unit = VKR_METRIC_UNIT_NANOSECONDS,
      .scalar = VKR_METRIC_SCALAR_U64,
      .writer = VKR_METRIC_WRITER_RENDER_THREAD,
  };
  VkrMetricId wall = VKR_METRIC_ID_INVALID;
  VkrMetricId graph = VKR_METRIC_ID_INVALID;
  VkrMetricId calls = VKR_METRIC_ID_INVALID;
  VkrMetricId scene = VKR_METRIC_ID_INVALID;
  assert(vkr_metrics_register(metrics, &desc, &wall));
  desc.name = string8_lit("cpu.graph_build");
  assert(vkr_metrics_register(metrics, &desc, &graph));
  desc.name = string8_lit("draw.world.calls_issued");
  desc.kind = VKR_METRIC_KIND_COUNTER;
  desc.unit = VKR_METRIC_UNIT_COUNT;
  assert(vkr_metrics_register(metrics, &desc, &calls));
  desc.name = string8_lit("frame.scene_interval");
  desc.kind = VKR_METRIC_KIND_GAUGE;
  desc.unit = VKR_METRIC_UNIT_NANOSECONDS;
  assert(vkr_metrics_register(metrics, &desc, &scene));
  assert(vkr_metrics_seal(metrics));
  const VkrRecordingConfig config = {
      .bundle_path = path, .codec = vkr_depot_zstd_codec(), .metrics = metrics};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  for (uint64_t f = 0u; f < 20u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 120.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    vkr_metrics_begin_frame(metrics, f, f);
    vkr_metrics_duration_add_ns(metrics, wall, 8000000u);
    if (f % 2u == 0u) {
      vkr_metrics_duration_add_ns(metrics, graph, 50000u);
      vkr_metrics_counter_add(metrics, calls, 100u);
      if (interval && f > 0u) {
        vkr_metrics_gauge_set_u64(metrics, scene, 15000000u);
      }
    }
    assert(vkr_metrics_end_frame(metrics));
    VkrMetricsSnapshotView view = {0};
    assert(vkr_metrics_snapshot_acquire(metrics, &view));
    vkr_recording_metrics(&recording, f, view.frame, NULL);
    vkr_metrics_snapshot_release(metrics, &view);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  vkr_allocator_free(&memory->allocator, metrics, sizeof(VkrMetrics),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

/* Detects: a recording whose Scene renders on every other loop frame (the
   editor presenter) reporting the loop's frame time as its frame time,
   draw counts of zero on frames that rendered nothing pulling means and
   medians down, a recorded Scene interval losing to the derived one, and
   the derived series left in the list when unused. */
static void test_recording_scene_frames(void) {
  printf("  Running test_recording_scene_frames...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  for (uint32_t variant = 0u; variant < 2u; ++variant) {
    char path[1024];
    recording_test_path(variant ? "scene_interval" : "scene_derived", path,
                        sizeof(path));
    path[strlen(path) - strlen(".vkrs")] = '\0';
    string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                  VKR_RECORDING_BUNDLE_EXTENSION);
    recording_scene_frames(&memory, path, variant == 1u);
    VkrRecordingData data;
    assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                   vkr_depot_zstd_codec()) ==
           VKR_RECORDING_DATA_OK);
    assert(data.scene_decoupled && data.rendered_frames == 10u);
    assert(vkr_recording_data_rendered(&data, 4u) &&
           !vkr_recording_data_rendered(&data, 5u) &&
           vkr_recording_data_rendered_at(&data, 5u) == 4u);
    const VkrRecordingSeries *time = &data.series[data.frame_time_series];
    float64_t ms = 0.0;
    if (variant == 0u) {
      /* Two loop frames of 1/120 s between Scenes. */
      assert(string_equals(time->name, VKR_RECORDING_SCENE_FRAME_SERIES));
      assert(vkr_recording_data_frame_ms(&data, 4u, &ms) &&
             vkr_abs_f64(ms - 1000.0 / 60.0) < 1.0e-6);
    } else {
      assert(string_equals(time->name, "frame.scene_interval"));
      assert(vkr_recording_data_frame_ms(&data, 4u, &ms) &&
             vkr_abs_f64(ms - 15.0) < 1.0e-6);
      assert(vkr_recording_data_find_series(
                 &data, VKR_RECORDING_SCENE_FRAME_SERIES) == UINT32_MAX);
    }
    assert(!vkr_recording_data_frame_ms(&data, 5u, &ms));
    /* The draw count only where a Scene was drawn; the loop's time on
       every frame. */
    const VkrRecordingSeries *calls =
        &data.series[vkr_recording_data_find_series(&data,
                                                    "draw.world.calls_issued")];
    const VkrRecordingSeries *walls =
        &data.series[vkr_recording_data_find_series(&data, "frame.wall")];
    for (uint64_t f = 0u; f < 20u; ++f) {
      assert(calls->valid[f] == (f % 2u == 0u));
      assert(walls->valid[f]);
    }
    vkr_recording_data_free(&data);
    char file[1100];
    snprintf(file, sizeof(file), "%s/stream.vkrs", path);
    recording_test_remove(file);
    snprintf(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
    recording_test_remove(file);
    const FilePath bundle = file_path_view(path);
    assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  }
  recording_memory_destroy(&memory);
  printf("  test_recording_scene_frames PASSED\n");
}

static void test_recording_loop_metrics(void) {
  printf("  Running test_recording_loop_metrics...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrMetrics *metrics = vkr_allocator_alloc(
      &memory.allocator, sizeof(VkrMetrics), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(metrics);
  vkr_metrics_init(metrics);
  VkrMetricDescription desc = {
      .name = string8_lit("frame.wall"),
      .domain = VKR_METRIC_DOMAIN_FRAME,
      .kind = VKR_METRIC_KIND_DURATION,
      .unit = VKR_METRIC_UNIT_NANOSECONDS,
      .scalar = VKR_METRIC_SCALAR_U64,
      .writer = VKR_METRIC_WRITER_RENDER_THREAD,
  };
  VkrMetricId wall = VKR_METRIC_ID_INVALID;
  VkrMetricId update = VKR_METRIC_ID_INVALID;
  VkrMetricId payload = VKR_METRIC_ID_INVALID;
  assert(vkr_metrics_register(metrics, &desc, &wall));
  desc.name = string8_lit("cpu.update");
  assert(vkr_metrics_register(metrics, &desc, &update));
  desc.name = string8_lit("cpu.world_payload_build");
  assert(vkr_metrics_register(metrics, &desc, &payload));
  assert(vkr_metrics_seal(metrics));

  char path[1024];
  recording_test_path("loop_metrics", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {
      .bundle_path = path, .codec = vkr_depot_zstd_codec(), .metrics = metrics};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* Iteration f records frame f, then completes frame f - 1: the
     publication holds frame f - 1's wall (10 + f - 1 ms) and iteration f's
     update (100 + f ms). Iteration 0 completes a frame from before the
     recording, whose wall is no recorded frame's. Iterations 3 and 5 built
     a frame first (50 + f ms); only iteration 3 built it before the
     completion. */
  for (uint64_t f = 0u; f < 6u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 1.0 / 60.0};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
    vkr_metrics_begin_frame(metrics, f, f);
    vkr_metrics_duration_add_ns(metrics, wall, (10u + f - 1u) * 1000000u);
    vkr_metrics_duration_add_ns(metrics, update, (100u + f) * 1000000u);
    if (f == 3u || f == 5u) {
      vkr_metrics_duration_add_ns(metrics, payload, (50u + f) * 1000000u);
    }
    assert(vkr_metrics_end_frame(metrics));
    VkrMetricsSnapshotView view = {0};
    assert(vkr_metrics_snapshot_acquire(metrics, &view));
    const VkrRecordingMetricsLoop loop = {.frame = f, .built = f == 3u};
    vkr_recording_metrics(&recording, f == 0u ? UINT64_MAX : f - 1u, view.frame,
                          &loop);
    vkr_metrics_snapshot_release(metrics, &view);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));
  assert(stats.lost_records == 0u && stats.dropped_records == 0u);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  const VkrRecordingSeries *walls =
      &data.series[vkr_recording_data_find_series(&data, "frame.wall")];
  const VkrRecordingSeries *updates =
      &data.series[vkr_recording_data_find_series(&data, "cpu.update")];
  const VkrRecordingSeries *payloads =
      &data.series[vkr_recording_data_find_series(&data,
                                                  "cpu.world_payload_build")];
  for (uint64_t f = 0u; f < 6u; ++f) {
    assert(walls->valid[f] == (f < 5u));
    assert(f == 5u || walls->values[f] == (float64_t)(10u + f));
    assert(updates->valid[f] && updates->values[f] == (float64_t)(100u + f));
  }
  /* Iteration 3's build is its own; iteration 5's came after it completed
     frame 4, so it stays with frame 4. */
  assert(payloads->valid[3] && payloads->values[3] == 53.0);
  assert(payloads->valid[4] && payloads->values[4] == 55.0);
  assert(!payloads->valid[0] && !payloads->valid[1] && !payloads->valid[2] &&
         !payloads->valid[5]);
  vkr_recording_data_free(&data);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_loop_metrics PASSED\n");
}

/* Writes one zone or pass name record. */
static void recording_timeline_name(VkrRecorder *recorder, uint16_t stream,
                                    uint64_t frame, uint32_t id,
                                    const char *name) {
  uint8_t record[4u + 32u];
  const uint32_t length = (uint32_t)strlen(name);
  MemCopy(record, &id, sizeof(id));
  MemCopy(record + 4u, name, length);
  assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN, stream, frame,
                            record, 4u + length));
}

/* Writes a passes record of `rows` rows, every row GPU-timed. */
static void recording_timeline_passes(VkrRecorder *recorder, uint64_t frame,
                                      uint32_t rows, const float32_t *gpu_ms) {
  uint8_t record[VKR_RECORDING_PASSES_HEADER_SIZE +
                 4u * VKR_RECORDING_PASS_ROW_SIZE] = {0};
  const uint32_t flags = VKR_RECORDING_PASSES_GPU_TIMED;
  MemCopy(record, &rows, sizeof(rows));
  MemCopy(record + 4u, &flags, sizeof(flags));
  for (uint32_t i = 0u; i < rows; ++i) {
    uint8_t *row = record + VKR_RECORDING_PASSES_HEADER_SIZE +
                   VKR_RECORDING_PASS_ROW_SIZE * i;
    const uint16_t id = (uint16_t)i;
    const uint16_t row_flags = VKR_RECORDING_PASS_GPU_VALID;
    MemCopy(row, &id, sizeof(id));
    MemCopy(row + 2u, &row_flags, sizeof(row_flags));
    MemCopy(row + 8u, &gpu_ms[i], sizeof(float32_t));
  }
  assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                            VKR_RECORDING_STREAM_PASSES, frame, record,
                            VKR_RECORDING_PASSES_HEADER_SIZE +
                                VKR_RECORDING_PASS_ROW_SIZE * rows));
}

/* Writes a pass_starts record on the CPU clock. */
static void recording_timeline_starts(VkrRecorder *recorder, uint64_t frame,
                                      uint64_t start_ns, uint64_t end_ns,
                                      uint32_t rows,
                                      const float32_t *start_ms) {
  uint8_t record[VKR_RECORDING_PASS_STARTS_HEADER_SIZE + 4u * 4u] = {0};
  const uint32_t flags = VKR_RECORDING_PASS_STARTS_HOST_CLOCK;
  MemCopy(record, &rows, sizeof(rows));
  MemCopy(record + 4u, &flags, sizeof(flags));
  MemCopy(record + 8u, &start_ns, sizeof(start_ns));
  MemCopy(record + 16u, &end_ns, sizeof(end_ns));
  MemCopy(record + VKR_RECORDING_PASS_STARTS_HEADER_SIZE, start_ms,
          sizeof(float32_t) * rows);
  assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                            VKR_RECORDING_STREAM_PASS_STARTS, frame, record,
                            VKR_RECORDING_PASS_STARTS_HEADER_SIZE + 4u * rows));
}

/* Writes a GPU counter file of two counters, "ALU Limiter" (percent) and
   "GPU Read Bandwidth", over `buckets` buckets of 100 us from 9 ms on the
   CPU clock, or `size` bytes of it when smaller. */
static void recording_counter_file(const char *bundle, const float32_t *alu,
                                   uint32_t buckets, uint64_t size) {
  uint8_t bytes[VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE + 64u + 2u * 4u * 140u] =
      {0};
  assert(buckets <= 140u);
  MemCopy(bytes, VKR_RECORDING_GPU_COUNTERS_MAGIC, 8u);
  const uint32_t header[] = {VKR_RECORDING_GPU_COUNTERS_VERSION, 2u, buckets,
                             100000u};
  MemCopy(bytes + 8u, header, sizeof(header));
  const uint64_t first_ns = 9000000u;
  MemCopy(bytes + 24u, &first_ns, sizeof(first_ns));
  const uint32_t anchors[] = {12u, 400u};
  MemCopy(bytes + 32u, anchors, sizeof(anchors));
  uint64_t at = VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE;
  const char *names[] = {"ALU Limiter", "GPU Read Bandwidth"};
  const uint8_t units[] = {VKR_RECORDING_GPU_COUNTER_PERCENT,
                           VKR_RECORDING_GPU_COUNTER_GB_PER_SECOND};
  for (uint32_t i = 0u; i < 2u; ++i) {
    bytes[at] = units[i];
    bytes[at + 1u] = (uint8_t)strlen(names[i]);
    MemCopy(bytes + at + 2u, names[i], strlen(names[i]));
    at += 2u + strlen(names[i]);
  }
  at = (at + 3u) & ~3ull;
  MemCopy(bytes + at, alu, sizeof(float32_t) * buckets);
  at += sizeof(float32_t) * buckets;
  for (uint32_t b = 0u; b < buckets; ++b, at += 4u) {
    const float32_t bandwidth = 6.0f;
    MemCopy(bytes + at, &bandwidth, sizeof(bandwidth));
  }
  char path[1100];
  snprintf(path, sizeof(path), "%s/%s", bundle,
           VKR_RECORDING_GPU_COUNTERS_FILE);
  FILE *file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite(bytes, 1u, (size_t)Min(size, at), file) == Min(size, at));
  fclose(file);
}

/* Detects: GPU counters placed on frames by the wrong clock or bucket,
   buckets without samples counted as zero, a pass's starts taken from the
   wrong row when an earlier row of its record was dropped, frames outside
   the trace counted as covered, and a damaged counter file failing the
   whole load instead of loading without counters. */
static void test_recording_gpu_counters(void) {
  printf("  Running test_recording_gpu_counters...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("counters", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  const FilePath bundle = file_path_view(path);
  assert(file_create_directory(&bundle));
  char stream[1100];
  snprintf(stream, sizeof(stream), "%s/stream.vkrs", path);

  uint32_t base_count = 0u;
  const VkrRecordingStream *base = vkr_recording_streams(&base_count);
  VkrRecordingStream streams[32];
  MemCopy(streams, base, sizeof(*base) * base_count);
  uint32_t count = base_count;
  streams[count++] = (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASS_NAME,
                                          .name = "pass_name"};
  streams[count++] =
      (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASSES, .name = "passes"};
  streams[count++] = (VkrRecordingStream){
      .id = VKR_RECORDING_STREAM_PASS_STARTS, .name = "pass_starts"};
  const VkrRecorderConfig config = {
      .stream_path = stream,
      .codec = vkr_depot_zstd_codec(),
      .streams = streams,
      .stream_count = count,
  };
  VkrRecorder *recorder = vkr_recorder_start(&memory.allocator, &config);
  assert(recorder != NULL);
  for (uint64_t f = 0u; f < 4u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 0.016};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                              VKR_RECORDING_STREAM_FRAME, f, &frame,
                              sizeof(frame)));
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                              VKR_RECORDING_STREAM_CAMERA, f, &camera,
                              sizeof(camera)));
  }
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_PASS_NAME, 0u, 0u,
                          "A");
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_PASS_NAME, 0u, 1u,
                          "B");
  /* Frame 1 runs a row whose pass has no name, then A (10.00 to 10.45 ms)
     and B (10.50 to 10.75 ms) in its GPU span of 10.00 to 10.95 ms. */
  uint8_t passes[VKR_RECORDING_PASSES_HEADER_SIZE +
                 3u * VKR_RECORDING_PASS_ROW_SIZE] = {0};
  const uint32_t rows = 3u;
  const uint32_t passes_flags = VKR_RECORDING_PASSES_GPU_TIMED;
  MemCopy(passes, &rows, sizeof(rows));
  MemCopy(passes + 4u, &passes_flags, sizeof(passes_flags));
  const uint16_t ids[] = {7u, 0u, 1u};
  const float32_t gpu_ms[] = {0.2f, 0.45f, 0.25f};
  for (uint32_t i = 0u; i < rows; ++i) {
    uint8_t *row = passes + VKR_RECORDING_PASSES_HEADER_SIZE +
                   VKR_RECORDING_PASS_ROW_SIZE * i;
    const uint16_t row_flags = VKR_RECORDING_PASS_GPU_VALID;
    MemCopy(row, &ids[i], sizeof(ids[i]));
    MemCopy(row + 2u, &row_flags, sizeof(row_flags));
    MemCopy(row + 8u, &gpu_ms[i], sizeof(float32_t));
  }
  assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                            VKR_RECORDING_STREAM_PASSES, 1u, passes,
                            sizeof(passes)));
  const float32_t start_ms[] = {0.9f, 0.0f, 0.5f};
  recording_timeline_starts(recorder, 1u, 10000000u, 10950000u, 3u, start_ms);
  /* Frame 2 lies in the trace without timed passes, frame 3 after it. */
  recording_timeline_starts(recorder, 2u, 20000000u, 20900000u, 0u, start_ms);
  recording_timeline_starts(recorder, 3u, 30000000u, 30900000u, 0u, start_ms);
  VkrRecorderStats stats;
  assert(vkr_recorder_stop(recorder, &stats));

  /* ALU limiter: 20% under A, 80% under B, no samples from 10.8 to 11.0 ms,
     and 5% elsewhere. */
  float32_t alu[130];
  for (uint32_t b = 0u; b < ArrayCount(alu); ++b) {
    alu[b] = 5.0f;
  }
  for (uint32_t b = 10u; b < 15u; ++b) {
    alu[b] = 20.0f;
  }
  for (uint32_t b = 15u; b < 18u; ++b) {
    alu[b] = 80.0f;
  }
  alu[18] = (float32_t)VKR_FLOAT_NAN;
  alu[19] = (float32_t)VKR_FLOAT_NAN;
  recording_counter_file(path, alu, ArrayCount(alu), UINT64_MAX);

  VkrRecordingData data;
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  assert(!data.gpu_counters_bad && data.gpu_counter_count == 2u);
  assert(string_equals(data.gpu_counters[0].name, "ALU Limiter") &&
         data.gpu_counters[1].unit == VKR_RECORDING_GPU_COUNTER_GB_PER_SECOND);
  assert(data.gpu_counter_bucket_ns == 100000u &&
         data.gpu_counter_anchors == 12u && data.gpu_counter_spread_ns == 400u);
  float32_t value = 0.0f;
  /* Frame 1: five buckets at 20 and three at 80; the two without samples
     do not count. */
  assert(vkr_recording_data_gpu_counter(&data, 0u, 1u, &value) &&
         vkr_abs_f32(value - 42.5f) < 1.0e-4f);
  assert(vkr_recording_data_gpu_counter(&data, 1u, 1u, &value) &&
         vkr_abs_f32(value - 6.0f) < 1.0e-4f);
  assert(vkr_recording_data_gpu_counter(&data, 0u, 2u, &value) &&
         vkr_abs_f32(value - 5.0f) < 1.0e-4f);
  assert(!vkr_recording_data_gpu_counter(&data, 0u, 0u, &value));
  assert(!vkr_recording_data_gpu_counter(&data, 0u, 3u, &value));
  assert(data.gpu_counter_frame_count == 2u &&
         data.gpu_counter_first_frame == 1u &&
         data.gpu_counter_last_frame == 2u);
  assert(vkr_abs_f32(data.gpu_counters[0].mean - 23.75f) < 1.0e-4f &&
         vkr_abs_f32(data.gpu_counters[0].max - 42.5f) < 1.0e-4f);
  /* Each pass reads the buckets under its own start. */
  assert(data.pass_count >= 2u && string_equals(data.passes[0].name, "A"));
  assert(vkr_recording_data_gpu_counter_pass(&data, 0u, 0u, &value) &&
         vkr_abs_f32(value - 20.0f) < 1.0e-4f);
  assert(vkr_recording_data_gpu_counter_pass(&data, 1u, 0u, &value) &&
         vkr_abs_f32(value - 80.0f) < 1.0e-4f);
  assert(data.gpu_counter_pass_frames[0] == 1u &&
         data.gpu_counter_pass_frames[1] == 1u);
  vkr_recording_data_free(&data);

  /* A cut file loads the recording without counters. */
  recording_counter_file(path, alu, ArrayCount(alu), 300u);
  assert(vkr_recording_data_load(&data, &memory.allocator, path,
                                 vkr_depot_zstd_codec()) ==
         VKR_RECORDING_DATA_OK);
  assert(data.gpu_counters_bad && data.gpu_counter_count == 0u &&
         data.frame_count == 4u);
  vkr_recording_data_free(&data);

  char file[1100];
  snprintf(file, sizeof(file), "%s/%s", path, VKR_RECORDING_GPU_COUNTERS_FILE);
  recording_test_remove(file);
  recording_test_remove(stream);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  recording_memory_destroy(&memory);
  printf("  test_recording_gpu_counters PASSED\n");
}

/* The three tables of a synthetic trace, as xctrace exports them. Clock
   events carry the CPU clock 5 s after the trace's: offsets of 0, 100 and
   50 ns above it. */
static const char *const COUNTER_TRACE_TABLES[] = {
    "<?xml version=\"1.0\"?>\n<trace-query-result>\n<node><schema "
    "name=\"os-signpost\"><col><mnemonic>time</mnemonic></col><col>"
    "<mnemonic>thread</mnemonic></col><col><mnemonic>event-type</mnemonic>"
    "</col><col><mnemonic>name</mnemonic></col><col><mnemonic>backtrace"
    "</mnemonic></col><col><mnemonic>subsystem</mnemonic></col><col>"
    "<mnemonic>message</mnemonic></col></schema><row><event-time id=\"1\" "
    "fmt=\"00:00.001\">1000000</event-time><thread id=\"2\" fmt=\"Main\">"
    "<tid id=\"3\" fmt=\"0x1\">1</tid></thread><event-type id=\"4\" "
    "fmt=\"Event\">Event</event-type><signpost-name id=\"5\" fmt=\"Clock\">"
    "Clock</signpost-name><sentinel/><subsystem id=\"6\" fmt=\"dev.vkr\">"
    "dev.vkr</subsystem><os-log-metadata id=\"7\" fmt=\"5\xc2\xa0"
    "001\xc2\xa0"
    "000\xc2\xa0"
    "000\"><uint64 id=\"8\" fmt=\"5\xc2\xa0"
    "001\">"
    "5001000000</uint64></os-log-metadata></row>\n"
    "<row><event-time id=\"9\" fmt=\"00:00.002\">2000000</event-time>"
    "<thread ref=\"2\"/><event-type ref=\"4\"/><signpost-name ref=\"5\"/>"
    "<sentinel/><subsystem ref=\"6\"/><os-log-metadata id=\"10\" fmt=\"x\">"
    "<uint64 id=\"11\" fmt=\"x\">5002000100</uint64></os-log-metadata>"
    "</row>\n<row><event-time id=\"12\" fmt=\"t\">2500000</event-time>"
    "<thread ref=\"2\"/><event-type ref=\"4\"/><signpost-name id=\"13\" "
    "fmt=\"Zone\">Zone</signpost-name><sentinel/><subsystem ref=\"6\"/>"
    "<os-log-metadata id=\"14\" fmt=\"Frame\"><string id=\"15\" "
    "fmt=\"Frame\">Frame</string></os-log-metadata></row>\n<row>"
    "<event-time id=\"16\" fmt=\"t\">2600000</event-time><thread ref=\"2\"/>"
    "<event-type ref=\"4\"/><signpost-name ref=\"5\"/><sentinel/>"
    "<subsystem id=\"17\" fmt=\"com.apple.ane\">com.apple.ane</subsystem>"
    "<os-log-metadata id=\"18\" fmt=\"1\"><uint64 id=\"19\" fmt=\"1\">1"
    "</uint64></os-log-metadata></row>\n<row><event-time id=\"20\" "
    "fmt=\"t\">3000000</event-time><thread ref=\"2\"/><event-type ref=\"4\"/>"
    "<signpost-name ref=\"5\"/><sentinel/><subsystem ref=\"6\"/>"
    "<os-log-metadata id=\"21\" fmt=\"x\"><uint64 id=\"22\" fmt=\"x\">"
    "5003000050</uint64></os-log-metadata></row>\n</node></trace-query-result>"
    "\n",
    "<?xml version=\"1.0\"?>\n<trace-query-result>\n<node><schema "
    "name=\"gpu-counter-info\"><col><mnemonic>timestamp</mnemonic></col>"
    "<col><mnemonic>counter-id</mnemonic></col><col><mnemonic>name"
    "</mnemonic></col><col><mnemonic>type</mnemonic></col></schema><row>"
    "<event-time id=\"1\" fmt=\"0\">0</event-time><uint32 id=\"2\" "
    "fmt=\"0\">0</uint32><gpu-counter-name id=\"3\" fmt=\"ALU Limiter\">ALU "
    "Limiter</gpu-counter-name><string id=\"4\" fmt=\"Percentage\">"
    "Percentage</string></row>\n<row><event-time ref=\"1\"/><uint32 id=\"5\" "
    "fmt=\"2\">2</uint32><gpu-counter-name id=\"6\" fmt=\"Partial Renders "
    "Count\">Partial Renders Count</gpu-counter-name><string id=\"7\" "
    "fmt=\"Value\">Value</string></row>\n<row><event-time ref=\"1\"/><uint32 "
    "id=\"8\" fmt=\"1\">1</uint32><gpu-counter-name id=\"9\" fmt=\"GPU Read "
    "Bandwidth\">GPU Read Bandwidth</gpu-counter-name><string ref=\"7\"/>"
    "</row>\n</node></trace-query-result>\n",
    "<?xml version=\"1.0\"?>\n<trace-query-result>\n<node><schema "
    "name=\"gpu-counter-value\"><col><mnemonic>timestamp</mnemonic></col>"
    "<col><mnemonic>counter-id</mnemonic></col><col><mnemonic>value"
    "</mnemonic></col><col><mnemonic>ring-buffer-index</mnemonic></col>"
    "</schema><row><event-time id=\"1\" fmt=\"t\">100000</event-time>"
    "<uint32 id=\"2\" fmt=\"0\">0</uint32><fixed-decimal id=\"3\" "
    "fmt=\"10,0\">10.000000000</fixed-decimal><uint32 ref=\"2\"/></row>\n"
    "<row><event-time ref=\"1\"/><uint32 id=\"4\" fmt=\"1\">1</uint32>"
    "<fixed-decimal id=\"5\" fmt=\"6,25\">6.250000000</fixed-decimal>"
    "<uint32 ref=\"2\"/></row>\n<row><event-time id=\"6\" fmt=\"t\">110000"
    "</event-time><uint32 ref=\"2\"/><fixed-decimal id=\"7\" fmt=\"30\">"
    "30.000000000</fixed-decimal><uint32 ref=\"4\"/></row>\n<row>"
    "<event-time id=\"8\" fmt=\"t\">160000</event-time><uint32 ref=\"2\"/>"
    "<fixed-decimal id=\"9\" fmt=\"40,5\">40.500000000</fixed-decimal>"
    "<uint32 ref=\"2\"/></row>\n<row><event-time id=\"10\" fmt=\"t\">300000"
    "</event-time><uint32 ref=\"2\"/><fixed-decimal id=\"11\" fmt=\"1\">"
    "1.000000000</fixed-decimal><uint32 ref=\"2\"/></row>\n</node>"
    "</trace-query-result>\n",
};

/* Feeds a table in 7-byte chunks, so rows, elements and characters split
   across chunks. */
static bool8_t recording_counter_source(void *context,
                                        VkrRecordingCountersTable table,
                                        VkrRecordingCountersSink sink,
                                        void *sink_context) {
  (void)context;
  const char *xml = COUNTER_TRACE_TABLES[table];
  const uint64_t length = strlen(xml);
  for (uint64_t at = 0u; at < length; at += 7u) {
    if (!sink(sink_context, (const uint8_t *)xml + at, Min(length - at, 7u))) {
      return false_v;
    }
  }
  return true_v;
}

/* Detects: an import that misreads xctrace's referenced values (ids, refs,
   nested message numbers, grouped digits), takes Clock events of another
   subsystem or name, places buckets from another origin than the median
   clock offset, averages two ring buffers' samples wrongly, writes values
   where no sample fell, or breaks when the export arrives in pieces. */
static void test_recording_counter_import(void) {
  printf("  Running test_recording_counter_import...\n");
  char path[1024];
  recording_test_path("import", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  const FilePath bundle = file_path_view(path);
  assert(file_create_directory(&bundle));
  const VkrRecordingCountersImport import = {
      .bundle_path = path,
      .bucket_us = 50u,
      .source = recording_counter_source,
  };
  VkrRecordingCountersResult result;
  assert(vkr_recording_counters_import(&import, &result));
  assert(result.counters == 3u && result.samples == 5u &&
         result.buckets == 5u && result.anchors == 3u &&
         result.spread_ns == 100u);

  char file[1100];
  snprintf(file, sizeof(file), "%s/%s", path, VKR_RECORDING_GPU_COUNTERS_FILE);
  uint8_t bytes[256] = {0};
  FILE *in = fopen(file, "rb");
  assert(in != NULL);
  const size_t size = fread(bytes, 1u, sizeof(bytes), in);
  fclose(in);
  assert(size == 156u && result.file_bytes == 156u);
  assert(memcmp(bytes, VKR_RECORDING_GPU_COUNTERS_MAGIC, 8u) == 0);
  uint32_t header[4];
  uint64_t first_ns = 0u;
  MemCopy(header, bytes + 8u, sizeof(header));
  MemCopy(&first_ns, bytes + 24u, sizeof(first_ns));
  assert(header[1] == 3u && header[2] == 5u && header[3] == 50000u);
  /* The median offset, plus the two empty buckets before the first
     sample. */
  assert(first_ns == 5000000050u + 2u * 50000u);
  /* Counters in id order with their units. */
  assert(bytes[40] == VKR_RECORDING_GPU_COUNTER_PERCENT && bytes[41] == 11u &&
         memcmp(bytes + 42u, "ALU Limiter", 11u) == 0);
  assert(bytes[53] == VKR_RECORDING_GPU_COUNTER_GB_PER_SECOND &&
         bytes[54] == 18u);
  assert(bytes[73] == VKR_RECORDING_GPU_COUNTER_COUNT && bytes[74] == 21u);
  float32_t values[15];
  MemCopy(values, bytes + 96u, sizeof(values));
  assert(values[0] == 20.0f && values[1] == 40.5f &&
         !vkr_is_finite_f32(values[2]) && !vkr_is_finite_f32(values[3]) &&
         values[4] == 1.0f);
  assert(values[5] == 6.25f && !vkr_is_finite_f32(values[6]));
  for (uint32_t i = 10u; i < 15u; ++i) {
    assert(!vkr_is_finite_f32(values[i]));
  }

  recording_test_remove(file);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  printf("  test_recording_counter_import PASSED\n");
}

/* Detects: zones left in arrival order, a name lost because its record came
   after the zones it names, pass rows joined to the wrong starts, a frame
   whose row counts disagree drawing misplaced passes, a missing "Frame"
   begin, and range queries that miss a zone covering their start. */
static void test_recording_playback_timeline(void) {
  printf("  Running test_recording_playback_timeline...\n");
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  char path[1024];
  recording_test_path("timeline", path, sizeof(path));
  uint32_t base_count = 0u;
  const VkrRecordingStream *base = vkr_recording_streams(&base_count);
  VkrRecordingStream streams[32];
  MemCopy(streams, base, sizeof(*base) * base_count);
  uint32_t count = base_count;
  streams[count++] = (VkrRecordingStream){.id = VKR_RECORDING_STREAM_ZONE_NAME,
                                          .name = "zone_name"};
  streams[count++] = (VkrRecordingStream){.id = VKR_RECORDING_STREAM_ZONE,
                                          .name = "zone",
                                          .record_size = sizeof(VkrZoneRecord)};
  streams[count++] = (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASS_NAME,
                                          .name = "pass_name"};
  streams[count++] =
      (VkrRecordingStream){.id = VKR_RECORDING_STREAM_PASSES, .name = "passes"};
  streams[count++] = (VkrRecordingStream){
      .id = VKR_RECORDING_STREAM_PASS_STARTS, .name = "pass_starts"};
  const VkrRecorderConfig config = {
      .stream_path = path,
      .codec = vkr_depot_zstd_codec(),
      .streams = streams,
      .stream_count = count,
  };
  VkrRecorder *recorder = vkr_recorder_start(&memory.allocator, &config);
  assert(recorder != NULL);
  for (uint64_t f = 0u; f < 4u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 0.016};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                              VKR_RECORDING_STREAM_FRAME, f, &frame,
                              sizeof(frame)));
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                              VKR_RECORDING_STREAM_CAMERA, f, &camera,
                              sizeof(camera)));
  }
  /* Frame 1's zone arrives before frame 0's, and the names after both. */
  const VkrZoneRecord zones[] = {
      {.zone = 0u,
       .thread = 0u,
       .depth = 0u,
       .begin_ns = 2000u,
       .end_ns = 3000u},
      {.zone = 0u,
       .thread = 0u,
       .depth = 0u,
       .begin_ns = 1000u,
       .end_ns = 1900u},
      {.zone = 1u,
       .thread = 2u,
       .depth = 0u,
       .begin_ns = 1100u,
       .end_ns = 1500u},
  };
  const uint64_t zone_frames[] = {1u, 0u, 0u};
  for (uint32_t i = 0u; i < ArrayCount(zones); ++i) {
    assert(vkr_recorder_write(recorder, VKR_RECORDER_PRODUCER_MAIN,
                              VKR_RECORDING_STREAM_ZONE, zone_frames[i],
                              &zones[i], sizeof(zones[i])));
  }
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_ZONE_NAME, 3u, 0u,
                          "Frame");
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_ZONE_NAME, 3u, 1u,
                          "Work");
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_PASS_NAME, 0u, 0u,
                          "A");
  recording_timeline_name(recorder, VKR_RECORDING_STREAM_PASS_NAME, 0u, 1u,
                          "B");
  const float32_t gpu_ms[] = {0.5f, 0.25f};
  const float32_t start_ms[] = {0.0f, 0.5f};
  recording_timeline_passes(recorder, 1u, 2u, gpu_ms);
  recording_timeline_starts(recorder, 1u, 10000000u, 11000000u, 2u, start_ms);
  /* Frame 2's rows and starts disagree in count. */
  recording_timeline_passes(recorder, 2u, 1u, gpu_ms);
  recording_timeline_starts(recorder, 2u, 20000000u, 21000000u, 2u, start_ms);
  VkrRecorderStats stats;
  assert(vkr_recorder_stop(recorder, &stats));

  static VkrRecordingPlayback playback;
  char error[128];
  assert(vkr_recording_playback_open(&playback, path, vkr_depot_zstd_codec(),
                                     NULL, error, sizeof(error)));
  assert(playback.has_timeline && playback.zone_count == 3u);
  assert(playback.zones[0].begin_ns == 1000u &&
         playback.zones[1].begin_ns == 1100u &&
         playback.zones[2].begin_ns == 2000u);
  assert(string_equals(playback.zone_names[0], "Frame") &&
         string_equals(playback.zone_names[1], "Work"));
  assert(playback.zone_threads == ((1u << 0u) | (1u << 2u)));
  assert(playback.frame_begin_ns[0] == 1000u &&
         playback.frame_begin_ns[1] == 2000u &&
         playback.frame_begin_ns[2] == 0u);
  /* Two GPU frames, and frame 1's two passes. */
  assert(playback.gpu_span_count == 4u);
  uint32_t passes_seen = 0u;
  for (uint32_t i = 0u; i < playback.gpu_span_count; ++i) {
    const VkrRecordingPlaybackGpuSpan *span = &playback.gpu_spans[i];
    if (span->pass == VKR_RECORDING_PLAYBACK_GPU_FRAME) {
      continue;
    }
    assert(span->frame == 1u);
    if (span->pass == 0u) {
      assert(span->begin_ns == 10000000u && span->end_ns == 10500000u);
    } else {
      assert(span->pass == 1u && span->begin_ns == 10500000u &&
             span->end_ns == 10750000u);
    }
    ++passes_seen;
  }
  assert(passes_seen == 2u);
  assert(string_equals(playback.pass_names[1], "B"));
  /* A window inside the 1000 to 1900 zone still finds it. */
  uint32_t first = 0u;
  uint32_t found =
      vkr_recording_playback_zones_in(&playback, 1600u, 1800u, &first);
  bool8_t covered = false_v;
  for (uint32_t i = first; i < first + found; ++i) {
    covered = covered || playback.zones[i].begin_ns == 1000u;
  }
  assert(covered);
  found = vkr_recording_playback_gpu_spans_in(&playback, 10600000u, 10700000u,
                                              &first);
  covered = false_v;
  for (uint32_t i = first; i < first + found; ++i) {
    covered = covered || playback.gpu_spans[i].pass == 1u;
  }
  assert(covered);
  vkr_recording_playback_close(&playback);
  recording_test_remove(path);
  recording_memory_destroy(&memory);
  printf("  test_recording_playback_timeline PASSED\n");
}

/* Detects: a loop span that lets the playhead run past its end, a wrap
   that loses the overshoot, a playhead before the span that never enters
   it, and an empty span that stops looping the whole recording. */
static void test_recording_playback_loop_range(void) {
  printf("  Running test_recording_playback_loop_range...\n");
  char path[1024];
  recording_test_path("loop", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const VkrRecordingConfig config = {.bundle_path = path,
                                     .codec = vkr_depot_zstd_codec()};
  static VkrRecording recording;
  assert(vkr_recording_start(&recording, &config));
  /* 21 frames 0.1 s apart: recorded times 0 to 2 s. */
  for (uint32_t f = 0u; f < 21u; ++f) {
    const VkrRecordingFrame frame = {.display_dt = 0.1};
    const VkrRecordingCamera camera = {.fov = 60.0f};
    vkr_recording_frame(&recording, &frame, &camera);
  }
  VkrRecorderStats stats;
  assert(vkr_recording_stop(&recording, &stats));

  static VkrRecordingPlayback playback;
  char error[128];
  assert(vkr_recording_playback_open(&playback, path, vkr_depot_zstd_codec(),
                                     NULL, error, sizeof(error)));
  const float64_t epsilon = 1.0e-9;
  playback.loop = true_v;
  playback.playing = true_v;
  vkr_recording_playback_set_loop_range(&playback, 0.5, 1.0);
  vkr_recording_playback_advance(&playback, 0.2);
  assert(fabs(playback.time - 0.5) < epsilon);
  vkr_recording_playback_advance(&playback, 0.6);
  assert(fabs(playback.time - 0.6) < epsilon);
  /* Several spans in one step keep the remainder. */
  vkr_recording_playback_advance(&playback, 1.25);
  assert(fabs(playback.time - 0.85) < epsilon);
  /* An empty span loops all of it again. */
  vkr_recording_playback_set_loop_range(&playback, 0.0, 0.0);
  vkr_recording_playback_advance(&playback, 1.3);
  assert(fabs(playback.time - 0.15) < epsilon && playback.playing);
  playback.loop = false_v;
  vkr_recording_playback_advance(&playback, 5.0);
  assert(fabs(playback.time - 2.0) < epsilon && !playback.playing);
  vkr_recording_playback_close(&playback);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_STREAM_FILE);
  recording_test_remove(file);
  string_format(file, sizeof(file), "%s/%s", path, VKR_RECORDING_SUMMARY_FILE);
  recording_test_remove(file);
  const FilePath bundle = file_path_view(path);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  printf("  test_recording_playback_loop_range PASSED\n");
}

/* Detects: notes or segments out of order after an add, a note lost or
   reordered on its frame, escapes (quotes, newlines, UTF-8) damaged by the
   write and read, a cut inside a UTF-8 sequence, a reversed segment kept
   reversed, a segment's note lost by the write, an edit that drops the
   field it was not given or accepts an empty title, a version 1 file's
   ranges not read as segments, a missing file read as an error, and a
   foreign file read as annotations. */
static void test_recording_annotations(void) {
  printf("  Running test_recording_annotations...\n");
  char path[1024];
  recording_test_path("annotations", path, sizeof(path));
  path[strlen(path) - strlen(".vkrs")] = '\0';
  string_format(path + strlen(path), sizeof(path) - strlen(path), "%s",
                VKR_RECORDING_BUNDLE_EXTENSION);
  const FilePath bundle = file_path_view(path);
  assert(file_create_directory(&bundle));

  static VkrRecordingAnnotations written;
  static VkrRecordingAnnotations read;
  assert(vkr_recording_annotations_read(path, &read));
  assert(read.note_count == 0u && read.segment_count == 0u &&
         read.review == VKR_RECORDING_REVIEW_UNREVIEWED);

  MemZero(&written, sizeof(written));
  const char *quoted = "first \"quoted\"\nline \xc3\xa9 \xe2\x9c\x93";
  assert(vkr_recording_annotations_add_note(&written, 30u, "user",
                                            string8_lit("second")));
  assert(vkr_recording_annotations_add_note(
      &written, 10u, "agent",
      string8_create_from_cstr((const uint8_t *)quoted, strlen(quoted))));
  assert(vkr_recording_annotations_add_note(&written, 30u, "user",
                                            string8_lit("third")));
  assert(
      !vkr_recording_annotations_add_note(&written, 40u, "user", (String8){0}));
  /* 254 ASCII bytes and a two-byte character: the cut at 255 bytes falls
     inside the character and backs up before it. */
  char long_text[257];
  MemSet(long_text, 'a', 254u);
  long_text[254] = (char)0xc3;
  long_text[255] = (char)0xa9;
  long_text[256] = '\0';
  assert(vkr_recording_annotations_add_note(
      &written, 5u, "user",
      string8_create_from_cstr((const uint8_t *)long_text, 256u)));
  const char *why = "Door opens:\n\"streaming\" hitch \xe2\x9c\x93";
  assert(vkr_recording_annotations_add_segment(
             &written, 50u, 20u, "agent", string8_lit("Hitch"),
             string8_create_from_cstr((const uint8_t *)why, strlen(why))) ==
         0u);
  /* An earlier first frame goes ahead; its index says where. */
  assert(vkr_recording_annotations_add_segment(&written, 5u, 6u, "user",
                                               string8_lit("Start"),
                                               (String8){0}) == 0u);
  assert(vkr_recording_annotations_add_segment(&written, 7u, 8u, "user",
                                               (String8){0},
                                               string8_lit("x")) == UINT32_MAX);
  vkr_recording_annotations_set_review(&written, VKR_RECORDING_REVIEW_FLAGGED,
                                       "agent");
  assert(written.notes[0].frame == 5u && written.notes[1].frame == 10u);
  assert(strcmp(written.notes[2].text, "second") == 0 &&
         strcmp(written.notes[3].text, "third") == 0);
  assert(strlen(written.notes[0].text) == 254u);
  assert(written.segments[0].first == 5u && written.segments[1].first == 20u &&
         written.segments[1].last == 50u);
  assert(vkr_recording_annotations_find_segment(&written,
                                                string8_lit("Hitch")) == 1u);
  assert(vkr_recording_annotations_find_segment(
             &written, string8_lit("missing")) == UINT32_MAX);

  assert(vkr_recording_annotations_write(path, &written));
  assert(vkr_recording_annotations_read(path, &read));
  assert(read.note_count == 4u && read.segment_count == 2u);
  for (uint32_t i = 0u; i < read.note_count; ++i) {
    assert(read.notes[i].frame == written.notes[i].frame);
    assert(read.notes[i].created == written.notes[i].created);
    assert(strcmp(read.notes[i].author, written.notes[i].author) == 0);
    assert(strcmp(read.notes[i].text, written.notes[i].text) == 0);
  }
  assert(strcmp(read.notes[1].text, quoted) == 0);
  for (uint32_t i = 0u; i < read.segment_count; ++i) {
    assert(read.segments[i].first == written.segments[i].first &&
           read.segments[i].last == written.segments[i].last);
    assert(read.segments[i].created == written.segments[i].created);
    assert(strcmp(read.segments[i].name, written.segments[i].name) == 0);
    assert(strcmp(read.segments[i].author, written.segments[i].author) == 0);
    assert(strcmp(read.segments[i].note, written.segments[i].note) == 0);
  }
  assert(strcmp(read.segments[1].note, why) == 0);
  assert(read.review == VKR_RECORDING_REVIEW_FLAGGED &&
         strcmp(read.review_by, "agent") == 0 &&
         read.review_at == written.review_at);

  vkr_recording_annotations_remove_note(&read, 1u);
  vkr_recording_annotations_remove_segment(&read, 0u);
  assert(read.note_count == 3u && read.notes[1].frame == 30u);
  assert(read.segment_count == 1u &&
         strcmp(read.segments[0].name, "Hitch") == 0);
  /* A title alone keeps the note; a note alone keeps the title. */
  const String8 retitle = string8_lit("Hitch at door");
  assert(vkr_recording_annotations_edit_segment(&read, 0u, &retitle, NULL));
  assert(strcmp(read.segments[0].name, "Hitch at door") == 0 &&
         strcmp(read.segments[0].note, why) == 0);
  const String8 renote = string8_lit("Fixed by prefetch");
  assert(vkr_recording_annotations_edit_segment(&read, 0u, NULL, &renote));
  assert(strcmp(read.segments[0].name, "Hitch at door") == 0 &&
         strcmp(read.segments[0].note, "Fixed by prefetch") == 0);
  const String8 empty = {0};
  assert(!vkr_recording_annotations_edit_segment(&read, 0u, &empty, &renote));
  assert(!vkr_recording_annotations_edit_segment(&read, 1u, &retitle, NULL));
  assert(strcmp(read.segments[0].name, "Hitch at door") == 0);

  char file[1100];
  string_format(file, sizeof(file), "%s/%s", path,
                VKR_RECORDING_ANNOTATIONS_FILE);
  /* A version 1 file named its segments "ranges" and had no notes. */
  {
    const FilePath legacy = file_path_view(file);
    FileHandle handle = {0};
    FileMode mode = bitset8_create();
    bitset8_set(&mode, FILE_MODE_WRITE);
    bitset8_set(&mode, FILE_MODE_TRUNCATE);
    bitset8_set(&mode, FILE_MODE_CREATE);
    assert(file_open(&legacy, mode, &handle) == FILE_ERROR_NONE);
    const char *old =
        "{\"kind\":\"vkr.recording.annotations\",\"version\":1,\"notes\":[],"
        "\"ranges\":[{\"first\":3,\"last\":9,\"author\":\"user\","
        "\"name\":\"Old range\"}]}";
    uint64_t wrote = 0u;
    assert(file_write(&handle, strlen(old), (const uint8_t *)old, &wrote) ==
           FILE_ERROR_NONE);
    file_close(&handle);
  }
  assert(vkr_recording_annotations_read(path, &read));
  assert(read.segment_count == 1u && read.segments[0].first == 3u &&
         read.segments[0].last == 9u &&
         strcmp(read.segments[0].name, "Old range") == 0 &&
         read.segments[0].note[0] == '\0');
  {
    const FilePath foreign = file_path_view(file);
    FileHandle handle = {0};
    FileMode mode = bitset8_create();
    bitset8_set(&mode, FILE_MODE_WRITE);
    bitset8_set(&mode, FILE_MODE_TRUNCATE);
    bitset8_set(&mode, FILE_MODE_CREATE);
    assert(file_open(&foreign, mode, &handle) == FILE_ERROR_NONE);
    const char *other = "{\"kind\":\"vkr.recording.request\"}";
    uint64_t wrote = 0u;
    assert(file_write(&handle, strlen(other), (const uint8_t *)other, &wrote) ==
           FILE_ERROR_NONE);
    file_close(&handle);
  }
  assert(!vkr_recording_annotations_read(path, &read));
  assert(read.note_count == 0u);

  recording_test_remove(file);
  assert(file_remove_directory(&bundle) == FILE_ERROR_NONE);
  printf("  test_recording_annotations PASSED\n");
}

/* Detects: a segment's frames not cut to the recording, a frame outside it
   counted, an invalid frame counted in its statistics, budget misses or
   its worst frame miscounted, a spike outside it counted, and a segment
   that starts past the end answering statistics. */
static void test_recording_segment_stats(void) {
  printf("  Running test_recording_segment_stats...\n");
  /* Ten frames of 10 ms, frame 6 at 40 ms, frame 7 invalid. */
  float64_t times[10];
  float64_t display_dt[10];
  uint8_t valid[10];
  for (uint32_t f = 0u; f < 10u; ++f) {
    display_dt[f] = f == 6u ? 0.040 : 0.010;
    times[f] = f == 0u ? 0.0 : times[f - 1u] + display_dt[f - 1u];
    valid[f] = f == 7u ? 0u : 1u;
  }
  const VkrRecordingData data = {.frame_count = 10u,
                                 .times = times,
                                 .display_dt = display_dt,
                                 .frame_valid = valid,
                                 .frame_time_series = UINT32_MAX};
  VkrRecordingSpike spikes[2] = {{.frame = 6u}, {.frame = 1u}};
  const VkrRecordingAnalysis analysis = {
      .budget_ms = 16.0, .spikes = spikes, .spike_count = 2u};
  RecordingTestMemory memory;
  recording_memory_create(&memory);
  VkrRecordingSegmentStats stats;
  /* Frames 4 to 20, cut to 4 to 9; frame 7 is invalid. */
  assert(vkr_recording_segment_stats(&data, &analysis, 4u, 20u,
                                     &memory.allocator, &stats));
  assert(stats.first == 4u && stats.last == 9u);
  assert(stats.start_seconds == times[4] && stats.end_seconds == times[9]);
  assert(stats.frame_time.valid == 5u && stats.frame_time.max == 40.0 &&
         stats.frame_time.p50 == 10.0);
  assert(stats.over_budget == 1u && stats.over_double_budget == 1u);
  assert(stats.worst_frame == 6u && stats.worst_ms == 40.0);
  assert(stats.spikes == 1u);
  /* Reversed frames read the same span. */
  assert(vkr_recording_segment_stats(&data, &analysis, 9u, 4u,
                                     &memory.allocator, &stats));
  assert(stats.first == 4u && stats.last == 9u && stats.spikes == 1u);
  assert(!vkr_recording_segment_stats(&data, &analysis, 10u, 12u,
                                      &memory.allocator, &stats));
  recording_memory_destroy(&memory);
  printf("  test_recording_segment_stats PASSED\n");
}

bool32_t run_recording_tests(void) {
  printf("--- Starting Recording Tests ---\n");
  test_spsc_ring_wrap_and_full();
  test_spsc_ring_threads();
  test_recording_format_round_trip();
  test_recording_truncated_recovery();
  test_recorder_accounting();
  test_recorder_rolling_save();
  test_recording_pass_rows();
  test_recording_graph_allocators_census();
  test_recording_hardware_pipelines();
  test_recording_gpu_counters();
  test_recording_scene_frames();
  test_recording_counter_import();
  test_recording_gpu_submission();
  test_recording_loop_metrics();
  test_recording_all_measurements();
  test_recording_resim_idle();
  test_scene_change_tracking();
  test_recording_pose_playback();
  test_recording_player_records();
  test_recording_drive();
  test_recording_resim();
  test_recording_playback_loop_range();
  test_recording_playback_timeline();
  test_recording_annotations();
  test_recording_segment_stats();
  printf("--- Recording Tests Completed ---\n");
  return true_v;
}
