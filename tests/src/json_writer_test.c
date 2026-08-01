#include "json_writer_test.h"

#include "core/vkr_json_writer.h"
#include "core/vkr_threads.h"

typedef struct JsonMemorySink {
  uint8_t data[4096];
  uint64_t length;
} JsonMemorySink;

static bool8_t json_memory_sink_write(void *context, const uint8_t *data,
                                      uint64_t length) {
  JsonMemorySink *sink = context;
  if (!sink || sink->length + length > sizeof(sink->data)) {
    return false_v;
  }
  MemCopy(sink->data + sink->length, data, length);
  sink->length += length;
  return true_v;
}

static void test_json_writer_structure_and_escaping(void) {
  printf("  Running test_json_writer_structure_and_escaping...\n");
  JsonMemorySink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  uint8_t raw[] = {'a', '"', '\n', '\0', 'z'};

  assert(vkr_json_writer_begin_object(&writer));
  assert(vkr_json_writer_name(&writer, string8_lit("text")));
  assert(vkr_json_writer_string(&writer, string8_create(raw, sizeof(raw))));
  assert(vkr_json_writer_name(&writer, string8_lit("items")));
  assert(vkr_json_writer_begin_array(&writer));
  assert(vkr_json_writer_u64(&writer, UINT64_MAX));
  assert(vkr_json_writer_i64(&writer, -42));
  assert(vkr_json_writer_f64(&writer, 0.25));
  assert(vkr_json_writer_bool(&writer, true_v));
  assert(vkr_json_writer_null(&writer));
  assert(vkr_json_writer_end_array(&writer));
  assert(vkr_json_writer_end_object(&writer));
  assert(vkr_json_writer_complete(&writer));

  const char *expected = "{\"text\":\"a\\\"\\n\\u0000z\",\"items\":["
                         "18446744073709551615,-42,0.25,true,null]}";
  assert(sink.length == string_length(expected));
  assert(MemCompare(sink.data, expected, sink.length) == 0);
  printf("  test_json_writer_structure_and_escaping PASSED\n");
}

static void test_json_writer_utf8(void) {
  printf("  Running test_json_writer_utf8...\n");
  JsonMemorySink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);

  // Valid multi-byte sequences pass through untouched; a lone continuation
  // byte and a truncated lead become U+FFFD rather than corrupting the file.
  uint8_t raw[] = {0xE2, 0x82, 0xAC, 0x80, 'a', 0xF0, 0x9F};
  assert(vkr_json_writer_string(&writer, string8_create(raw, sizeof(raw))));
  assert(vkr_json_writer_complete(&writer));

  const char *expected = "\"\xE2\x82\xAC\\ufffda\\ufffd\\ufffd\"";
  assert(sink.length == string_length(expected));
  assert(MemCompare(sink.data, expected, sink.length) == 0);
  printf("  test_json_writer_utf8 PASSED\n");
}

static void test_json_writer_depth_limit(void) {
  printf("  Running test_json_writer_depth_limit...\n");
  JsonMemorySink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  for (uint32_t i = 0; i < VKR_JSON_WRITER_MAX_DEPTH; ++i) {
    assert(vkr_json_writer_begin_array(&writer));
  }
  // Exceeding the bound fails and stays failed; it never silently flattens.
  assert(!vkr_json_writer_begin_array(&writer));
  assert(!vkr_json_writer_end_array(&writer));
  assert(!vkr_json_writer_complete(&writer));
  printf("  test_json_writer_depth_limit PASSED\n");
}

static void test_json_writer_failure_is_sticky(void) {
  printf("  Running test_json_writer_failure_is_sticky...\n");
  JsonMemorySink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  assert(vkr_json_writer_begin_object(&writer));
  // A name outside an object, or a null string, must poison the writer so a
  // truncated document can never be completed as if it were whole.
  // Built by hand: string8_create() rejects a null pointer up front.
  const String8 null_string = {.str = NULL, .length = 4};
  assert(!vkr_json_writer_string(&writer, null_string));
  assert(!vkr_json_writer_name(&writer, string8_lit("after")));
  assert(!vkr_json_writer_end_object(&writer));
  assert(!vkr_json_writer_complete(&writer));

  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  assert(!vkr_json_writer_name(&writer, string8_lit("no_container")));
  assert(!vkr_json_writer_begin_object(&writer));
  assert(!vkr_json_writer_complete(&writer));
  printf("  test_json_writer_failure_is_sticky PASSED\n");
}

static void test_json_writer_rejects_invalid_state(void) {
  printf("  Running test_json_writer_rejects_invalid_state...\n");
  JsonMemorySink sink = {0};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  assert(vkr_json_writer_begin_object(&writer));
  assert(vkr_json_writer_name(&writer, string8_lit("bad")));
  assert(!vkr_json_writer_f64(&writer, NAN));
  assert(!vkr_json_writer_complete(&writer));

  vkr_json_writer_init(&writer, json_memory_sink_write, &sink);
  assert(vkr_json_writer_begin_array(&writer));
  assert(!vkr_json_writer_end_object(&writer));
  assert(!vkr_json_writer_complete(&writer));
  printf("  test_json_writer_rejects_invalid_state PASSED\n");
}

static void test_json_file_writer_atomic_commit_and_abort(void) {
  printf("  Running test_json_file_writer_atomic_commit_and_abort...\n");
  uint8_t embedded_nul_path[] = {'/', 't', 'm', 'p', '/', 'x', '\0', 'y'};
  VkrJsonFileWriter file_writer;
  assert(!vkr_json_file_writer_begin(
      &file_writer,
      string8_create(embedded_nul_path, sizeof(embedded_nul_path))));

  char path[256];
  snprintf(path, sizeof(path), "/tmp/vkr_json_writer_test_%llu.json",
           (unsigned long long)vkr_thread_current_id());
  remove(path);
  String8 path_string =
      string8_create_from_cstr((const uint8_t *)path, string_length(path));

  assert(vkr_json_file_writer_begin(&file_writer, path_string));
  assert(vkr_json_writer_begin_object(&file_writer.writer));
  assert(vkr_json_writer_name(&file_writer.writer, string8_lit("ok")));
  assert(vkr_json_writer_bool(&file_writer.writer, true_v));
  assert(vkr_json_writer_end_object(&file_writer.writer));
  assert(vkr_json_file_writer_commit(&file_writer));

  FILE *file = fopen(path, "rb");
  assert(file);
  char data[32] = {0};
  const size_t length = fread(data, 1u, sizeof(data), file);
  fclose(file);
  assert(length == string_length("{\"ok\":true}"));
  assert(MemCompare(data, "{\"ok\":true}", length) == 0);

  assert(vkr_json_file_writer_begin(&file_writer, path_string));
  assert(vkr_json_writer_begin_array(&file_writer.writer));
  vkr_json_file_writer_abort(&file_writer);
  file = fopen(path, "rb");
  assert(file);
  MemZero(data, sizeof(data));
  assert(fread(data, 1u, sizeof(data), file) == length);
  fclose(file);
  assert(MemCompare(data, "{\"ok\":true}", length) == 0);
  remove(path);
  printf("  test_json_file_writer_atomic_commit_and_abort PASSED\n");
}

bool32_t run_json_writer_tests(void) {
  printf("--- Running JSON Writer tests... ---\n");
  test_json_writer_structure_and_escaping();
  test_json_writer_utf8();
  test_json_writer_depth_limit();
  test_json_writer_failure_is_sticky();
  test_json_writer_rejects_invalid_state();
  test_json_file_writer_atomic_commit_and_abort();
  printf("--- JSON Writer tests completed. ---\n");
  return true_v;
}
