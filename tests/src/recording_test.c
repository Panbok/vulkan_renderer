#include "recording_test.h"

#include "assets/vkr_recording_format.h"
#include "containers/vkr_spsc_ring.h"
#include "core/vkr_threads.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "recording/vkr_recorder.h"
#include "vkr_depot_zstd.h"

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

bool32_t run_recording_tests(void) {
  printf("--- Starting Recording Tests ---\n");
  test_spsc_ring_wrap_and_full();
  test_spsc_ring_threads();
  test_recording_format_round_trip();
  test_recording_truncated_recovery();
  test_recorder_accounting();
  printf("--- Recording Tests Completed ---\n");
  return true_v;
}
