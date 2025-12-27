#include "text_test.h"

static Arena *arena = NULL;
static VkrAllocator allocator = {0};
static const uint64_t ARENA_SIZE = MB(1);

static void setup_suite(void) {
  arena = arena_create(ARENA_SIZE, ARENA_SIZE);
  allocator = (VkrAllocator){.ctx = arena};
  vkr_allocator_arena(&allocator);
}

static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
    allocator = (VkrAllocator){0};
  }
}

static void assert_f32_eq(float32_t a, float32_t b, float32_t epsilon,
                          const char *message) {
  if (fabsf(a - b) > epsilon) {
    fprintf(stderr, "Float assertion failed: %s (%.5f vs %.5f)\n", message, a,
            b);
    assert(0 && "Float comparison failed");
  }
}

static void test_utf8_decode_encode(void) {
  printf("  Running test_utf8_decode_encode...\n");

  uint8_t ascii[] = {0x24};
  VkrCodepoint cp_ascii = vkr_utf8_decode(ascii, sizeof(ascii));
  assert(cp_ascii.value == 0x24 && cp_ascii.byte_length == 1);

  uint8_t euro[] = {0xE2, 0x82, 0xAC};
  VkrCodepoint cp_euro = vkr_utf8_decode(euro, sizeof(euro));
  assert(cp_euro.value == 0x20AC && cp_euro.byte_length == 3);

  uint8_t invalid[] = {0xE2, 0x28, 0xA1};
  VkrCodepoint cp_invalid = vkr_utf8_decode(invalid, sizeof(invalid));
  assert(cp_invalid.byte_length == 0);

  uint8_t encoded[4] = {0};
  uint8_t bytes_written = vkr_utf8_encode(0x1F600, encoded, sizeof(encoded));
  uint8_t expected[] = {0xF0, 0x9F, 0x98, 0x80};
  assert(bytes_written == 4 && memcmp(encoded, expected, 4) == 0);

  printf("  test_utf8_decode_encode PASSED\n");
}

static void test_codepoint_iteration(void) {
  printf("  Running test_codepoint_iteration...\n");

  const uint8_t data[] = {'A', 0xE2, 0x98, 0x83, 'B'};
  String8 s = string8_create((uint8_t *)data, sizeof(data));

  uint64_t count = vkr_string8_codepoint_count(&s);
  assert(count == 3);

  VkrCodepointIter iter = vkr_codepoint_iter_begin(&s);
  uint32_t expected_values[] = {'A', 0x2603, 'B'};
  uint32_t idx = 0;
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    assert(cp.byte_length > 0);
    assert(cp.value == expected_values[idx]);
    idx++;
  }
  assert(idx == 3);

  printf("  test_codepoint_iteration PASSED\n");
}

static void test_utf8_validation(void) {
  printf("  Running test_utf8_validation...\n");

  String8 valid = string8_lit("Valid");
  assert(vkr_string8_is_valid_utf8(&valid));

  uint8_t invalid_bytes[] = {0xF0, 0x28, 0x8C, 0xBC};
  String8 invalid = string8_create(invalid_bytes, sizeof(invalid_bytes));
  assert(!vkr_string8_is_valid_utf8(&invalid));

  printf("  test_utf8_validation PASSED\n");
}

static void test_text_creation_and_destroy(void) {
  printf("  Running test_text_creation_and_destroy...\n");
  setup_suite();

  VkrFontHandle test_font = {.id = 1, .generation = 0};
  VkrTextStyle style = vkr_text_style_new(test_font, 14.0f, VKR_TEXT_COLOR_RED);
  String8 view = string8_lit("sample");
  VkrText view_text = vkr_text_from_view(view, &style);
  assert(view_text.owns_content == false_v);
  assert(view_text.content.str == view.str);

  VkrText copy_text = vkr_text_from_copy(&allocator, view, NULL);
  assert(copy_text.owns_content == true_v);
  assert(copy_text.content.length == view.length);
  assert(copy_text.content.str != view.str);

  VkrText literal_text = vkr_text_from_cstr("hello", NULL);
  assert(literal_text.content.length == 5);
  assert(literal_text.owns_content == false_v);

  VkrText formatted = vkr_text_formatted(&allocator, NULL, "num: %d", 42);
  assert(formatted.owns_content == true_v);
  assert(formatted.content.length > 0);

  vkr_text_destroy(&allocator, &view_text);
  vkr_text_destroy(&allocator, &copy_text);
  vkr_text_destroy(&allocator, &literal_text);
  vkr_text_destroy(&allocator, &formatted);

  teardown_suite();
  printf("  test_text_creation_and_destroy PASSED\n");
}

static void test_text_measurement(void) {
  printf("  Running test_text_measurement...\n");

  VkrTextStyle style =
      vkr_text_style_new(VKR_FONT_HANDLE_INVALID, 10.0f, VKR_TEXT_COLOR_WHITE);
  VkrText text = vkr_text_from_cstr("abcd", &style);

  VkrTextBounds bounds = vkr_text_measure(&text);
  assert_f32_eq(bounds.size.x, 24.0f, 0.001f, "width without wrap");
  assert_f32_eq(bounds.size.y, 10.0f, 0.001f, "height without wrap");
  assert_f32_eq(bounds.ascent, 8.0f, 0.001f, "ascent");
  assert_f32_eq(bounds.descent, 2.0f, 0.001f, "descent");

  VkrTextBounds wrapped = vkr_text_measure_wrapped(&text, 12.0f);
  assert_f32_eq(wrapped.size.x, 12.0f, 0.001f, "wrapped width");
  assert_f32_eq(wrapped.size.y, 20.0f, 0.001f, "wrapped height");

  printf("  test_text_measurement PASSED\n");
}

static void test_text_layout(void) {
  printf("  Running test_text_layout...\n");
  setup_suite();

  VkrTextStyle style =
      vkr_text_style_new(VKR_FONT_HANDLE_INVALID, 10.0f, VKR_TEXT_COLOR_WHITE);
  VkrText text = vkr_text_from_cstr("ab", &style);
  VkrTextLayoutOptions opts = vkr_text_layout_options_default();
  opts.word_wrap = false_v;

  VkrTextLayout layout = vkr_text_layout_compute(&allocator, &text, &opts);

  assert(layout.glyphs.length == 2);
  assert(layout.line_count == 1);
  assert_f32_eq(layout.bounds.x, 12.0f, 0.001f, "layout width");
  assert_f32_eq(layout.bounds.y, 10.0f, 0.001f, "layout height");
  assert_f32_eq(layout.baseline.y, 0.0f, 0.001f, "baseline y");

  assert_f32_eq(layout.glyphs.data[0].position.x, 0.0f, 0.001f,
                "glyph 0 x position");
  assert_f32_eq(layout.glyphs.data[0].position.y, layout.baseline.y, 0.001f,
                "glyph 0 y position");
  assert_f32_eq(layout.glyphs.data[1].position.x, 6.0f, 0.001f,
                "glyph 1 x position");

  vkr_text_layout_destroy(&layout);
  vkr_text_destroy(&allocator, &text);
  teardown_suite();
  printf("  test_text_layout PASSED\n");
}

static void test_rich_text_spans(void) {
  printf("  Running test_rich_text_spans...\n");
  setup_suite();

  VkrTextStyle base = vkr_text_style_default();
  String8 content = string8_lit("Hello World");
  VkrRichText rt = vkr_rich_text_create(&allocator, content, &base);

  vkr_rich_text_add_span(&rt, 0, 5, &base);
  vkr_rich_text_add_span(&rt, 6, 11, &base);
  assert(rt.spans.length == 2);
  assert(rt.spans.capacity >= 2);

  vkr_rich_text_clear_spans(&rt);
  assert(rt.spans.length == 0);

  vkr_rich_text_destroy(&rt);
  teardown_suite();
  printf("  test_rich_text_spans PASSED\n");
}

bool32_t run_text_tests(void) {
  printf("--- Starting Text Tests ---\n");

  test_utf8_decode_encode();
  test_codepoint_iteration();
  test_utf8_validation();
  test_text_creation_and_destroy();
  test_text_measurement();
  test_text_layout();
  test_rich_text_spans();

  return true_v;
}
