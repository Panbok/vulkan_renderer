#include "json_test.h"

static void test_reader_create_and_reset(void) {
  printf("  Running test_reader_create_and_reset...\n");

  const char *json = "{ \"a\": 1 }";
  uint64_t length = string_length(json);
  VkrJsonReader reader = vkr_json_reader_create((const uint8_t *)json, length);

  assert(reader.data == (const uint8_t *)json &&
         "Reader data pointer mismatch");
  assert(reader.length == length && "Reader length mismatch");
  assert(reader.pos == 0 && "Reader pos not initialized to 0");

  reader.pos = 5;
  vkr_json_reader_reset(&reader);
  assert(reader.pos == 0 && "Reader reset did not set pos to 0");

  String8 str = string8_lit("{\"b\":2}");
  VkrJsonReader reader2 = vkr_json_reader_from_string(str);
  assert(reader2.data == str.str && "Reader2 data pointer mismatch");
  assert(reader2.length == str.length && "Reader2 length mismatch");
  assert(reader2.pos == 0 && "Reader2 pos not initialized to 0");

  printf("  test_reader_create_and_reset PASSED\n");
}

static void test_skip_whitespace_and_skip_to(void) {
  printf("  Running test_skip_whitespace_and_skip_to...\n");

  const char *json = " \t\n\r{\"a\":1}";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  vkr_json_skip_whitespace(&reader);
  const char *brace = strchr(json, '{');
  assert(brace && "Expected '{' in test JSON");
  assert(reader.pos == (uint64_t)(brace - json) && "Whitespace skip failed");

  vkr_json_reader_reset(&reader);
  vkr_json_skip_to(&reader, '{');
  assert(reader.pos == (uint64_t)(brace - json) &&
         "Skip-to did not land on target");

  const char *json2 = "abc";
  VkrJsonReader reader2 =
      vkr_json_reader_create((const uint8_t *)json2, string_length(json2));
  vkr_json_skip_to(&reader2, 'z');
  assert(reader2.pos == reader2.length &&
         "Skip-to should move to end when target is missing");

  printf("  test_skip_whitespace_and_skip_to PASSED\n");
}

static void test_find_field_and_parse_values(void) {
  printf("  Running test_find_field_and_parse_values...\n");

  const char *json = "{ \"name\": \"Widget\", \"count\": 42, \"delta\": -7, "
                     "\"price\": -12.5e1, \"ratio\": 0.125, \"active\": true }";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  assert(vkr_json_find_field(&reader, "name"));
  String8 name = {0};
  assert(vkr_json_parse_string(&reader, &name));
  assert(vkr_string8_equals_cstr(&name, "Widget"));

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_field(&reader, "count"));
  int32_t count = 0;
  assert(vkr_json_parse_int(&reader, &count));
  assert(count == 42);

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_field(&reader, "delta"));
  int32_t delta = 0;
  assert(vkr_json_parse_int(&reader, &delta));
  assert(delta == -7);

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_field(&reader, "price"));
  float64_t price = 0.0;
  assert(vkr_json_parse_double(&reader, &price));
  assert(vkr_abs_f64(price + 125.0) < 0.0001);

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_field(&reader, "ratio"));
  float32_t ratio = 0.0f;
  assert(vkr_json_parse_float(&reader, &ratio));
  assert(vkr_abs_f32(ratio - 0.125f) < 0.00001f);

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_field(&reader, "active"));
  bool8_t active = false_v;
  assert(vkr_json_parse_bool(&reader, &active));
  assert(active == true_v);

  printf("  test_find_field_and_parse_values PASSED\n");
}

static void test_parse_string_with_escapes(void) {
  printf("  Running test_parse_string_with_escapes...\n");

  const char *json = "{ \"text\": \"Hello\\\\n\\\"World\\\"\" }";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  assert(vkr_json_find_field(&reader, "text"));
  String8 text = {0};
  assert(vkr_json_parse_string(&reader, &text));
  assert(vkr_string8_equals_cstr(&text, "Hello\\\\n\\\"World\\\""));

  printf("  test_parse_string_with_escapes PASSED\n");
}

static void test_missing_field_and_helpers(void) {
  printf("  Running test_missing_field_and_helpers...\n");

  const char *json =
      "{ \"a\": 1, \"b\": \"text\", \"pi\": 3.14159, \"ratio\": 0.5, "
      "\"ok\": false }";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  reader.pos = 2;
  uint64_t saved_pos = reader.pos;
  assert(!vkr_json_find_field(&reader, "missing"));
  assert(reader.pos == saved_pos &&
         "find_field should restore position on miss");

  reader.pos = 4;
  saved_pos = reader.pos;
  int32_t missing = 0;
  assert(!vkr_json_get_int(&reader, "missing", &missing));
  assert(reader.pos == saved_pos && "get_int should restore position on miss");

  vkr_json_reader_reset(&reader);
  saved_pos = reader.pos;
  int32_t wrong_type = 0;
  assert(!vkr_json_get_int(&reader, "b", &wrong_type));
  assert(reader.pos == saved_pos &&
         "get_int should restore position on parse failure");

  vkr_json_reader_reset(&reader);
  int32_t a_val = 0;
  assert(vkr_json_get_int(&reader, "a", &a_val));
  assert(a_val == 1);

  vkr_json_reader_reset(&reader);
  String8 b_val = {0};
  assert(vkr_json_get_string(&reader, "b", &b_val));
  assert(vkr_string8_equals_cstr(&b_val, "text"));

  vkr_json_reader_reset(&reader);
  float64_t pi = 0.0;
  assert(vkr_json_get_double(&reader, "pi", &pi));
  assert(vkr_abs_f64(pi - 3.14159) < 0.000001);

  vkr_json_reader_reset(&reader);
  float32_t ratio = 0.0f;
  assert(vkr_json_get_float(&reader, "ratio", &ratio));
  assert(vkr_abs_f32(ratio - 0.5f) < 0.00001f);

  vkr_json_reader_reset(&reader);
  bool8_t ok = true_v;
  assert(vkr_json_get_bool(&reader, "ok", &ok));
  assert(ok == false_v);

  printf("  test_missing_field_and_helpers PASSED\n");
}

static void test_parse_bool_invalid_true_suffix(void) {
  printf("  Running test_parse_bool_invalid_true_suffix...\n");

  const char *json = "truex";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  bool8_t value = false_v;
  assert(!vkr_json_parse_bool(&reader, &value));
  assert(reader.pos == 0 && "Reader position should not advance on failure");

  printf("  test_parse_bool_invalid_true_suffix PASSED\n");
}

static void test_array_iteration_objects(void) {
  printf("  Running test_array_iteration_objects...\n");

  const char *json = "{ \"items\": [ {\"id\":1,\"name\":\"alpha\"}, "
                     "{\"id\":2,\"name\":\"beta\"} ], \"empty\": [] }";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  assert(vkr_json_find_array(&reader, "items"));

  const int32_t expected_ids[] = {1, 2};
  const char *expected_names[] = {"alpha", "beta"};
  uint32_t index = 0;

  while (vkr_json_next_array_element(&reader)) {
    VkrJsonReader obj_reader = {0};
    assert(vkr_json_enter_object(&reader, &obj_reader));

    int32_t id = 0;
    String8 name = {0};
    assert(vkr_json_get_int(&obj_reader, "id", &id));
    assert(vkr_json_get_string(&obj_reader, "name", &name));

    assert(index < ArrayCount(expected_ids));
    assert(id == expected_ids[index]);
    assert(vkr_string8_equals_cstr(&name, expected_names[index]));
    index++;
  }

  assert(index == ArrayCount(expected_ids));

  vkr_json_reader_reset(&reader);
  assert(vkr_json_find_array(&reader, "empty"));
  assert(!vkr_json_next_array_element(&reader));

  printf("  test_array_iteration_objects PASSED\n");
}

static void test_enter_object_nested(void) {
  printf("  Running test_enter_object_nested...\n");

  const char *json = "{ \"outer\": { \"inner\": { \"value\": 3 }, "
                     "\"text\": \"brace } in text\" }, \"other\": 1 }";
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)json, string_length(json));

  assert(vkr_json_find_field(&reader, "outer"));
  VkrJsonReader outer_reader = {0};
  assert(vkr_json_enter_object(&reader, &outer_reader));

  assert(vkr_json_find_field(&outer_reader, "inner"));
  VkrJsonReader inner_reader = {0};
  assert(vkr_json_enter_object(&outer_reader, &inner_reader));

  int32_t value = 0;
  assert(vkr_json_get_int(&inner_reader, "value", &value));
  assert(value == 3);

  String8 text = {0};
  assert(vkr_json_get_string(&outer_reader, "text", &text));
  assert(vkr_string8_equals_cstr(&text, "brace } in text"));

  printf("  test_enter_object_nested PASSED\n");
}

bool32_t run_json_tests(void) {
  printf("--- Starting JSON Tests ---\n");

  test_reader_create_and_reset();
  test_skip_whitespace_and_skip_to();
  test_find_field_and_parse_values();
  test_parse_string_with_escapes();
  test_missing_field_and_helpers();
  test_parse_bool_invalid_true_suffix();
  test_array_iteration_objects();
  test_enter_object_nested();

  printf("--- JSON Tests Completed ---\n");
  return true;
}
