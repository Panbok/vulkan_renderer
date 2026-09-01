#include "text_test.h"
#include "platform/vkr_window_internal.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/systems/vkr_ui_system.h"

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

static float32_t test_mtsdf_screen_range(Vec2 unit_range, Vec2 dx, Vec2 dy) {
  Vec2 gradient_squared = {
      Max(dx.x * dx.x + dy.x * dy.x, 1e-12f),
      Max(dx.y * dx.y + dy.y * dy.y, 1e-12f),
  };
  Vec2 screen_tex_size = {
      1.0f / sqrtf(gradient_squared.x),
      1.0f / sqrtf(gradient_squared.y),
  };
  return Max(0.5f * (unit_range.x * screen_tex_size.x +
                     unit_range.y * screen_tex_size.y),
             1.0f);
}

static void test_mtsdf_unit_range_and_derivatives(void) {
  printf("  Running test_mtsdf_unit_range_and_derivatives...\n");

  const Vec2 unit_range = vkr_text_mtsdf_unit_range(8.0f, 1024u, 512u);
  assert_f32_eq(unit_range.x, 8.0f / 1024.0f, 0.000001f,
                "rectangular unit range x");
  assert_f32_eq(unit_range.y, 8.0f / 512.0f, 0.000001f,
                "rectangular unit range y");

  const float32_t axis_aligned = test_mtsdf_screen_range(
      unit_range, vec2_new(1.0f / 512.0f, 0.0f), vec2_new(0.0f, 1.0f / 256.0f));
  assert_f32_eq(axis_aligned, 4.0f, 0.0001f, "axis-aligned derivative range");

  const float32_t rotated = test_mtsdf_screen_range(
      unit_range, vec2_new(0.0f, 1.0f / 256.0f), vec2_new(1.0f / 512.0f, 0.0f));
  assert_f32_eq(rotated, 4.0f, 0.0001f, "rotated derivative range");

  printf("  test_mtsdf_unit_range_and_derivatives PASSED\n");
}

static void test_mtsdf_fractional_uv_bounds_remain_exact(void) {
  printf("  Running test_mtsdf_fractional_uv_bounds_remain_exact...\n");

  const float32_t inv_width = 1.0f / 1024.0f;
  const float32_t inv_height = 1.0f / 1024.0f;
  const float32_t left = 948.5f * inv_width;
  const float32_t bottom = 973.5f * inv_height;
  const float32_t right = 965.5f * inv_width;
  const float32_t top = 1023.5f * inv_height;
  const float32_t inset = vkr_text_uv_inset(1.0f, true_v);

  assert_f32_eq(inset, 0.0f, 0.0f, "MTSDF inset disabled");
  assert_f32_eq(left + inset * inv_width, left, 0.0f,
                "fractional left preserved");
  assert_f32_eq(bottom + inset * inv_height, bottom, 0.0f,
                "fractional bottom preserved");
  assert_f32_eq(right - inset * inv_width, right, 0.0f,
                "fractional right preserved");
  assert_f32_eq(top - inset * inv_height, top, 0.0f,
                "fractional top preserved");
  assert_f32_eq(vkr_text_uv_inset(-1.0f, false_v), 0.0f, 0.0f,
                "negative bitmap inset clamped");

  printf("  test_mtsdf_fractional_uv_bounds_remain_exact PASSED\n");
}

typedef struct TestCookedFont {
  VkrFont font;
  VkrFontGlyphId glyphs[4];
  VkrFontCodepointMapEntry mappings[4];
  VkrFontGlyphKerning kernings[1];
} TestCookedFont;

static void test_cooked_font_init(TestCookedFont *fixture) {
  MemZero(fixture, sizeof(*fixture));
  fixture->glyphs[0] = (VkrFontGlyphId){
      .glyph_id = 3u,
      .has_geometry = true_v,
      .advance = 0.55f,
      .plane_left = 0.0f,
      .plane_bottom = -0.2f,
      .plane_right = 0.5f,
      .plane_top = 0.8f,
      .uv_right = 0.25f,
      .uv_top = 0.25f,
  };
  fixture->glyphs[1] = fixture->glyphs[0];
  fixture->glyphs[1].glyph_id = 5u;
  fixture->glyphs[1].advance = 0.6f;
  fixture->glyphs[1].uv_left = 0.25f;
  fixture->glyphs[1].uv_right = 0.5f;
  fixture->glyphs[2] = fixture->glyphs[1];
  fixture->glyphs[2].glyph_id = 9u;
  fixture->glyphs[2].advance = 0.65f;
  fixture->glyphs[2].uv_left = 0.5f;
  fixture->glyphs[2].uv_right = 0.75f;
  fixture->glyphs[3] = fixture->glyphs[2];
  fixture->glyphs[3].glyph_id = 12u;
  fixture->glyphs[3].advance = 0.5f;
  fixture->glyphs[3].plane_left = -0.2f;
  fixture->glyphs[3].plane_bottom = -0.25f;
  fixture->glyphs[3].plane_right = 0.4f;
  fixture->glyphs[3].plane_top = 0.75f;
  fixture->glyphs[3].uv_left = 0.75f;
  fixture->glyphs[3].uv_right = 1.0f;
  fixture->mappings[0] = (VkrFontCodepointMapEntry){
      .codepoint = '?', .glyph_id = 3u, .glyph_index = 0u};
  fixture->mappings[1] = (VkrFontCodepointMapEntry){
      .codepoint = 'A', .glyph_id = 5u, .glyph_index = 1u};
  fixture->mappings[2] = (VkrFontCodepointMapEntry){
      .codepoint = 'V', .glyph_id = 9u, .glyph_index = 2u};
  fixture->mappings[3] = (VkrFontCodepointMapEntry){
      .codepoint = 0x00e9u, .glyph_id = 12u, .glyph_index = 3u};
  fixture->kernings[0] = (VkrFontGlyphKerning){
      .left_glyph_id = 5u, .right_glyph_id = 9u, .amount = -0.075f};
  fixture->font = (VkrFont){
      .id = 1u,
      .generation = 7u,
      .type = VKR_FONT_TYPE_MTSDF,
      .size = 32u,
      .line_height = 40,
      .ascent = 26,
      .descent = 6,
      .atlas_size_x = 4,
      .atlas_size_y = 4,
      .page_count = 1u,
      .glyphs_by_id = {.length = ArrayCount(fixture->glyphs),
                       .data = fixture->glyphs},
      .codepoint_map = {.length = ArrayCount(fixture->mappings),
                        .data = fixture->mappings},
      .glyph_kernings = {.length = ArrayCount(fixture->kernings),
                         .data = fixture->kernings},
      .fallback_glyph_id = 3u,
      .fallback_glyph_index = 0u,
      .em_line_height = 1.25f,
      .em_ascender = 0.8f,
      .em_descender = -0.2f,
      .tab_x_advance = 2.4f,
  };
}

static void test_cooked_float_layout_contract(void) {
  printf("  Running test_cooked_float_layout_contract...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  VkrTextStyle style =
      vkr_text_style_new(VKR_FONT_HANDLE_INVALID, 13.5f, VKR_TEXT_COLOR_WHITE);
  style.letter_spacing = 0.25f;
  style = vkr_text_style_with_font_data(&style, &fixture.font);
  VkrText text = vkr_text_from_cstr("AV", &style);
  VkrTextLayoutOptions options = vkr_text_layout_options_default();
  options.word_wrap = false_v;
  VkrTextLayout layout = vkr_text_layout_compute(&allocator, &text, &options);
  assert(layout.glyphs.length == 2u);
  assert(layout.glyphs.data[0].glyph_id == 5u);
  assert(layout.glyphs.data[1].glyph_id == 9u);
  assert_f32_eq(layout.glyphs.data[1].position.x, 7.3375f, 0.0001f,
                "glyph-ID kerning at fractional size");
  assert_f32_eq(layout.bounds.x, 16.3625f, 0.0001f,
                "float advance accumulation");
  assert_f32_eq(layout.bounds.y, 16.875f, 0.0001f, "em line height");
  vkr_text_layout_destroy(&layout);

  style.letter_spacing = 0.0f;
  const uint8_t missing_utf8[] = {0xf0u, 0x9fu, 0x98u, 0x80u};
  VkrText missing = vkr_text_from_view(
      string8_create((uint8_t *)missing_utf8, sizeof(missing_utf8)), &style);
  layout = vkr_text_layout_compute(&allocator, &missing, &options);
  assert(layout.glyphs.length == 1u);
  assert(layout.glyphs.data[0].glyph_id == fixture.font.fallback_glyph_id);
  assert(layout.glyphs.data[0].glyph_index ==
         fixture.font.fallback_glyph_index);
  assert_f32_eq(layout.bounds.x, 7.425f, 0.0001f, "fallback advance");
  vkr_text_layout_destroy(&layout);

  VkrText multiline = vkr_text_from_cstr("A\nV", &style);
  options.max_height = 16.875f;
  options.clip = true_v;
  layout = vkr_text_layout_compute(&allocator, &multiline, &options);
  assert(layout.line_count == 2u);
  assert(layout.glyphs.length == 1u);
  vkr_text_layout_destroy(&layout);
  teardown_suite();
  printf("  test_cooked_float_layout_contract PASSED\n");
}

static void test_cooked_long_run_accumulation(void) {
  printf("  Running test_cooked_long_run_accumulation...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  enum { glyph_count = 8192 };
  uint8_t content[glyph_count];
  MemSet(content, 'A', sizeof(content));
  VkrTextStyle style =
      vkr_text_style_new(VKR_FONT_HANDLE_INVALID, 13.5f, VKR_TEXT_COLOR_WHITE);
  style = vkr_text_style_with_font_data(&style, &fixture.font);
  VkrText text =
      vkr_text_from_view(string8_create(content, sizeof(content)), &style);
  VkrTextLayoutOptions options = vkr_text_layout_options_default();
  options.word_wrap = false_v;
  VkrTextLayout layout = vkr_text_layout_compute(&allocator, &text, &options);
  const float64_t advance = (float64_t)(fixture.glyphs[1].advance * 13.5f);
  assert(layout.glyphs.length == glyph_count);
  assert_f32_eq(layout.bounds.x, (float32_t)(advance * glyph_count), 0.01f,
                "long-run width uses wide accumulation");
  assert_f32_eq(layout.glyphs.data[glyph_count - 1u].position.x,
                (float32_t)(advance * (glyph_count - 1u)), 0.01f,
                "long-run final glyph position");
  vkr_text_layout_destroy(&layout);
  teardown_suite();
  printf("  test_cooked_long_run_accumulation PASSED\n");
}

static void test_cooked_negative_bearing_geometry(void) {
  printf("  Running test_cooked_negative_bearing_geometry...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  VkrFontSystem font_system = {0};
  font_system.fonts = (Array_VkrFont){.length = 1u, .data = &fixture.font};
  font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiTextConfig config = VKR_UI_TEXT_CONFIG_DEFAULT;
  config.font_size = 10.0f;
  VkrUiText ui_text = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_ui_text_create(&allocator, &font_system, string8_lit("\xc3\xa9"),
                            &config, &ui_text, &error));
  assert(vkr_ui_text_prepare_geometry(&ui_text));
  assert(ui_text.geometry.vertex_count == 4u);
  assert_f32_eq(ui_text.geometry.vertices[0].position.x, -2.0f, 0.0001f,
                "negative plane bearing");
  assert_f32_eq(ui_text.geometry.vertices[0].position.y, 2.0f, 0.0001f,
                "cooked plane top");
  assert_f32_eq(ui_text.geometry.vertices[1].position.x, 4.0f, 0.0001f,
                "cooked plane right");
  assert_f32_eq(ui_text.geometry.vertices[1].position.y, 12.0f, 0.0001f,
                "cooked plane bottom");
  assert_f32_eq(ui_text.geometry.vertices[0].texcoord.x, 0.75f, 0.0f,
                "cooked UV left");
  assert_f32_eq(ui_text.geometry.vertices[1].texcoord.x, 1.0f, 0.0f,
                "cooked UV right");
  vkr_ui_text_destroy(&ui_text);
  teardown_suite();
  printf("  test_cooked_negative_bearing_geometry PASSED\n");
}

static void test_ui_text_reselects_default_font(void) {
  printf("  Running test_ui_text_reselects_default_font...\n");
  setup_suite();
  TestCookedFont default_fixture;
  TestCookedFont explicit_fixture;
  test_cooked_font_init(&default_fixture);
  test_cooked_font_init(&explicit_fixture);
  explicit_fixture.font.id = 2u;
  explicit_fixture.font.generation = 11u;
  VkrFont fonts[] = {default_fixture.font, explicit_fixture.font};
  VkrFontSystem font_system = {0};
  font_system.fonts =
      (Array_VkrFont){.length = ArrayCount(fonts), .data = fonts};
  font_system.default_mtsdf_font_handle =
      (VkrFontHandle){.id = fonts[0].id, .generation = fonts[0].generation};

  VkrUiTextConfig config = VKR_UI_TEXT_CONFIG_DEFAULT;
  config.font =
      (VkrFontHandle){.id = fonts[1].id, .generation = fonts[1].generation};
  VkrUiText ui_text = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_ui_text_create(&allocator, &font_system, string8_lit("A"), &config,
                            &ui_text, &error));
  assert(ui_text.resolved_font == &fonts[1]);

  config.font = VKR_FONT_HANDLE_INVALID;
  vkr_ui_text_set_config(&ui_text, &config);
  assert(ui_text.resolved_font == &fonts[0]);
  assert(ui_text.layout_dirty && ui_text.buffers_dirty);

  vkr_ui_text_destroy(&ui_text);
  teardown_suite();
  printf("  test_ui_text_reselects_default_font PASSED\n");
}

static void test_window_content_scale_snapshot(void) {
  printf("  Running test_window_content_scale_snapshot...\n");
  VkrWindow window = {0};
  vkr_window_content_scale_init(&window);
  VkrWindowContentScale snapshot = vkr_window_get_content_scale(&window);
  assert_f32_eq(snapshot.value, 1.0f, 0.0f, "window scale default");
  assert(snapshot.revision == 1u);
  assert(vkr_window_content_scale_publish(&window, 1.25f));
  snapshot = vkr_window_get_content_scale(&window);
  assert_f32_eq(snapshot.value, 1.25f, 0.0f, "window scale publish");
  assert(snapshot.revision == 2u);
  assert(!vkr_window_content_scale_publish(&window, 1.25f));
  assert(!vkr_window_content_scale_publish(&window, 0.0f));
  assert(vkr_window_get_content_scale(&window).revision == 2u);
  printf("  test_window_content_scale_snapshot PASSED\n");
}

static void test_ui_text_content_scale_contract(void) {
  printf("  Running test_ui_text_content_scale_contract...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  const float32_t scales[] = {1.0f, 1.25f, 1.5f, 2.0f};
  for (uint32_t i = 0; i < ArrayCount(scales); ++i) {
    const float32_t scale = scales[i];
    VkrFontSystem font_system = {0};
    font_system.fonts = (Array_VkrFont){.length = 1u, .data = &fixture.font};
    font_system.default_mtsdf_font_handle = (VkrFontHandle){
        .id = fixture.font.id, .generation = fixture.font.generation};
    VkrUiTextConfig config = VKR_UI_TEXT_CONFIG_DEFAULT;
    config.font_size = 10.0f;
    config.letter_spacing = 0.25f;
    config.layout.max_width = 12.0f;
    config.layout.word_wrap = true_v;
    VkrUiText ui_text = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    assert(vkr_ui_text_create(&allocator, &font_system, string8_lit("AA"),
                              &config, &ui_text, &error));
    vkr_ui_text_set_content_scale(&ui_text, scale);
    assert(vkr_ui_text_prepare_geometry(&ui_text));
    assert(ui_text.layout.line_count == 2u);
    assert(ui_text.layout.glyphs.length == 2u);
    assert_f32_eq(ui_text.layout.glyphs.data[0].advance, 6.25f * scale, 0.0001f,
                  "scaled advance and letter spacing");
    assert_f32_eq(ui_text.bounds.size.x, 6.25f * scale, 0.0001f,
                  "scaled wrapping width");
    assert_f32_eq(ui_text.bounds.size.y, 25.0f * scale, 0.0001f,
                  "scaled multiline height");
    assert_f32_eq(ui_text.geometry.vertices[1].position.x, 5.0f * scale,
                  0.0001f, "scaled glyph geometry");

    config.layout.max_width = 0.0f;
    config.layout.max_height = 12.5f;
    config.layout.word_wrap = false_v;
    config.layout.clip = true_v;
    vkr_ui_text_set_config(&ui_text, &config);
    assert(vkr_ui_text_set_content(&ui_text, string8_lit("A\nA")));
    assert(vkr_ui_text_prepare_geometry(&ui_text));
    assert(ui_text.layout.line_count == 2u);
    assert(ui_text.layout.glyphs.length == 1u);
    vkr_ui_text_destroy(&ui_text);
  }
  teardown_suite();
  printf("  test_ui_text_content_scale_contract PASSED\n");
}

static void test_ui_system_scale_revision_and_offsets(void) {
  printf("  Running test_ui_system_scale_revision_and_offsets...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  RendererFrontend renderer = {0};
  renderer.allocator = allocator;
  renderer.last_window_width = 200u;
  renderer.last_window_height = 100u;
  renderer.font_system.fonts =
      (Array_VkrFont){.length = 1u, .data = &fixture.font};
  renderer.font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiSystem system = {0};
  assert(vkr_ui_system_init(&renderer, &system));
  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 200u, 100u);

  VkrUiTextConfig config = VKR_UI_TEXT_CONFIG_DEFAULT;
  config.font_size = 10.0f;
  VkrUiTextCreateData payload = {
      .text_id = VKR_INVALID_ID,
      .content = string8_lit("A"),
      .config = &config,
      .anchor = VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT,
      .padding = vec2_new(3.0f, 4.0f),
  };
  uint32_t text_id = VKR_INVALID_ID;
  assert(vkr_ui_system_text_create(&renderer, &system, &payload, &text_id));
  VkrUiTextSlot *slot = &system.text_slots.data[text_id];
  assert(vkr_ui_text_prepare_geometry(&slot->text));

  const float32_t scales[] = {1.25f, 1.5f, 2.0f, 1.0f};
  for (uint32_t i = 0; i < ArrayCount(scales); ++i) {
    const uint32_t geometry_revision = slot->text.geometry.revision;
    vkr_ui_system_set_offscreen_content_scale(&renderer, &system, scales[i]);
    assert(system.content_scale_revision ==
           system.offscreen_content_scale_revision);
    assert(slot->text.buffers_dirty);
    assert(vkr_ui_text_prepare_geometry(&slot->text));
    assert(slot->text.geometry.revision == geometry_revision + 1u);
    assert_f32_eq(slot->text.bounds.size.x, 6.0f * scales[i], 0.0001f,
                  "system scaled font size");
    assert_f32_eq(slot->text.transform.position.x, 3.0f * scales[i], 0.0001f,
                  "scaled slot x offset");
    assert_f32_eq(slot->text.transform.position.y, 4.0f * scales[i], 0.0001f,
                  "scaled slot y offset");
    assert_f32_eq(slot->text.transform.scale.x, 1.0f, 0.0f,
                  "content scale absent from transform x");
    assert_f32_eq(slot->text.transform.scale.y, 1.0f, 0.0f,
                  "content scale absent from transform y");
  }

  const uint32_t geometry_revision = slot->text.geometry.revision;
  vkr_ui_system_resize(&renderer, &system, 320u, 180u);
  assert(!slot->text.layout_dirty && !slot->text.buffers_dirty);
  assert(slot->text.geometry.revision == geometry_revision);
  assert_f32_eq(system.text_content_scale, 1.0f, 0.0f,
                "extent resize preserves density");
  vkr_ui_system_shutdown(&renderer, &system);
  teardown_suite();
  printf("  test_ui_system_scale_revision_and_offsets PASSED\n");
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
  test_mtsdf_unit_range_and_derivatives();
  test_mtsdf_fractional_uv_bounds_remain_exact();
  test_cooked_float_layout_contract();
  test_cooked_long_run_accumulation();
  test_cooked_negative_bearing_geometry();
  test_ui_text_reselects_default_font();
  test_window_content_scale_snapshot();
  test_ui_text_content_scale_contract();
  test_ui_system_scale_revision_and_offsets();

  return true_v;
}
