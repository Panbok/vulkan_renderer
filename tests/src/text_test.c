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
      .atlas = {.id = 9u, .generation = 3u},
      .sdf_distance_range = 4.0f,
      .mtsdf_unit_range = {1.0f, 1.0f},
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
  renderer.scratch_allocator = allocator;
  renderer.last_window_width = 200u;
  renderer.last_window_height = 100u;
  renderer.font_system.fonts =
      (Array_VkrFont){.length = 1u, .data = &fixture.font};
  renderer.font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiSystem system = {0};
  assert(vkr_ui_system_init(&renderer, &system));
  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 200u, 100u);

  InputState input = {0};
  const float32_t scales[] = {1.25f, 1.5f, 2.0f, 1.0f};
  for (uint32_t i = 0; i < ArrayCount(scales); ++i) {
    vkr_ui_system_set_offscreen_content_scale(&renderer, &system, scales[i]);
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
    assert(vkr_allocator_scope_is_valid(&scope));
    assert(vkr_ui_begin(&renderer, &system, &input, false_v, 1.0 / 60.0, NULL));
    VkrUiWidgetConfig label = vkr_ui_widget_config_default();
    label.placement.justify = VKR_UI_ALIGN_START;
    label.placement.align = VKR_UI_ALIGN_START;
    label.placement.margin_pt = (VkrUiEdges){4.0f, 0.0f, 0.0f, 3.0f};
    label.style.font_size_pt = 10.0f;
    label.text.font = (VkrFontHandle){.id = fixture.font.id,
                                      .generation = fixture.font.generation};
    label.text.font_size = 10.0f;
    vkr_ui_label(&system, string8_lit("scaled-label"), string8_lit("A"),
                 &label);
    (void)vkr_ui_end(&system);
    VkrPreparedUiDrawList draw_list = {0};
    assert(vkr_ui_system_prepare_draw_list(&system, &allocator, 200u, 100u,
                                           &draw_list));
    assert(draw_list.vertex_count == 4u);
    assert(system.content_scale_revision ==
           system.offscreen_content_scale_revision);
    float32_t min_x = draw_list.vertices[0].position.x;
    float32_t max_x = min_x;
    for (uint32_t vertex = 1u; vertex < draw_list.vertex_count; ++vertex) {
      min_x = Min(min_x, draw_list.vertices[vertex].position.x);
      max_x = Max(max_x, draw_list.vertices[vertex].position.x);
    }
    assert_f32_eq(min_x, 3.0f * scales[i], 0.0001f, "scaled grid x offset");
    assert_f32_eq(max_x - min_x, 5.0f * scales[i], 0.0001f,
                  "scaled glyph width");
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vkr_ui_system_resize(&renderer, &system, 320u, 180u);
  assert_f32_eq(system.content_scale, 1.0f, 0.0f,
                "extent resize preserves density");
  vkr_ui_system_shutdown(&renderer, &system);
  teardown_suite();
  printf("  test_ui_system_scale_revision_and_offsets PASSED\n");
}

static void test_ui_system_reuses_unchanged_draw_geometry(void) {
  printf("  Running test_ui_system_reuses_unchanged_draw_geometry...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  RendererFrontend renderer = {0};
  renderer.allocator = allocator;
  renderer.scratch_allocator = allocator;
  renderer.last_window_width = 200u;
  renderer.last_window_height = 100u;
  renderer.font_system.fonts =
      (Array_VkrFont){.length = 1u, .data = &fixture.font};
  renderer.font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiSystem system = {0};
  assert(vkr_ui_system_init(&renderer, &system));
  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 200u, 100u);

  InputState input = {0};
  const VkrUiVertex *cached_vertices = NULL;
  uint64_t label_hash = 0u;
  for (uint32_t frame = 0u; frame < 4u; ++frame) {
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
    assert(vkr_allocator_scope_is_valid(&scope));
    assert(vkr_ui_begin(&renderer, &system, &input, true_v, 1.0 / 60.0, NULL));
    vkr_ui_label(&system, string8_lit("cached-label"),
                 frame < 2u ? string8_lit("A") : string8_lit("B"), NULL);
    (void)vkr_ui_end(&system);
    assert(system.frame_reuses_cached_draw_list ==
           (frame == 1u || frame == 3u));

    VkrPreparedUiDrawList draw_list = {0};
    assert(vkr_ui_system_prepare_draw_list(&system, &allocator, 200u, 100u,
                                           &draw_list));
    assert(draw_list.vertex_count == 4u);
    assert(draw_list.vertices == system.cached_vertices);
    if (frame == 0u) {
      cached_vertices = draw_list.vertices;
      label_hash = system.cached_draw_hash;
    } else {
      assert(draw_list.vertices == cached_vertices);
      if (frame == 1u)
        assert(system.cached_draw_hash == label_hash);
      if (frame == 2u)
        assert(system.cached_draw_hash != label_hash);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  bool8_t checked = false_v;
  uint64_t unchecked_hash = 0u;
  for (uint32_t frame = 0u; frame < 3u; ++frame) {
    if (frame == 2u)
      checked = true_v;
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
    assert(vkr_allocator_scope_is_valid(&scope));
    assert(vkr_ui_begin(&renderer, &system, &input, true_v, 1.0 / 60.0, NULL));
    (void)vkr_ui_checkbox(&system, string8_lit("cached-checkbox"),
                          string8_lit("Visible"), &checked, NULL);
    (void)vkr_ui_end(&system);
    assert(system.frame_reuses_cached_draw_list == (frame == 1u));
    VkrPreparedUiDrawList draw_list = {0};
    assert(vkr_ui_system_prepare_draw_list(&system, &allocator, 200u, 100u,
                                           &draw_list));
    if (frame == 0u)
      unchecked_hash = system.cached_draw_hash;
    if (frame == 2u) {
      assert(system.cached_draw_hash != unchecked_hash);
      assert(draw_list.vertex_count > 4u);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 220u, 120u);
  VkrAllocatorScope resize_scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&resize_scope));
  assert(vkr_ui_begin(&renderer, &system, &input, true_v, 1.0 / 60.0, NULL));
  vkr_ui_label(&system, string8_lit("cached-label"), string8_lit("B"), NULL);
  (void)vkr_ui_end(&system);
  assert(!system.frame_reuses_cached_draw_list);
  VkrPreparedUiDrawList resized_draw_list = {0};
  assert(vkr_ui_system_prepare_draw_list(&system, &allocator, 220u, 120u,
                                         &resized_draw_list));
  vkr_allocator_end_scope(&resize_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  vkr_ui_system_set_offscreen_content_scale(&renderer, &system, 2.0f);
  VkrAllocatorScope scale_scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scale_scope));
  assert(vkr_ui_begin(&renderer, &system, &input, true_v, 1.0 / 60.0, NULL));
  vkr_ui_label(&system, string8_lit("cached-label"), string8_lit("B"), NULL);
  (void)vkr_ui_end(&system);
  assert(!system.frame_reuses_cached_draw_list);
  VkrPreparedUiDrawList scaled_draw_list = {0};
  assert(vkr_ui_system_prepare_draw_list(&system, &allocator, 220u, 120u,
                                         &scaled_draw_list));
  vkr_allocator_end_scope(&scale_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  vkr_ui_system_shutdown(&renderer, &system);
  teardown_suite();
  printf("  test_ui_system_reuses_unchanged_draw_geometry PASSED\n");
}

static bool8_t test_ui_text_field_frame(RendererFrontend *renderer,
                                        VkrUiSystem *system, InputState *input,
                                        VkrUiTextEditBuffer *buffer,
                                        float64_t delta,
                                        VkrUiInputCapture *out_capture) {
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scope));
  assert(vkr_ui_begin(renderer, system, input, false_v, delta, NULL));
  const bool8_t changed =
      vkr_ui_text_field(system, string8_lit("edit"), buffer, NULL);
  *out_capture = vkr_ui_end(system);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return changed;
}

static void test_ui_text_field_character_input_and_repeat(void) {
  printf("  Running test_ui_text_field_character_input_and_repeat...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  RendererFrontend renderer = {0};
  renderer.allocator = allocator;
  renderer.scratch_allocator = allocator;
  renderer.last_window_width = 200u;
  renderer.last_window_height = 100u;
  renderer.font_system.fonts =
      (Array_VkrFont){.length = 1u, .data = &fixture.font};
  renderer.font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiSystem system = {0};
  assert(vkr_ui_system_init(&renderer, &system));
  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 200u, 100u);

  uint8_t bytes[32];
  MemSet(bytes, 0xcc, sizeof(bytes));
  VkrUiTextEditBuffer edit = {.data = bytes, .capacity = sizeof(bytes)};
  EventManager event_manager = {0};
  event_manager_create(&event_manager);
  InputState input = input_init(&event_manager);
  VkrUiInputCapture capture = {0};
  assert(!test_ui_text_field_frame(&renderer, &system, &input, &edit,
                                   1.0 / 60.0, &capture));

  input_process_mouse_move(&input, 10, 10);
  input_process_button(&input, BUTTON_LEFT, true_v);
  assert(input_process_char(&input, 'A'));
  assert(input_process_char(&input, 0x00e9u));
  assert(test_ui_text_field_frame(&renderer, &system, &input, &edit, 1.0 / 60.0,
                                  &capture));
  assert(capture.keyboard && capture.text);
  assert(edit.length == 3u);
  assert(MemCompare(edit.data, "A\xc3\xa9", 3u) == 0);
  assert(edit.data[edit.length] == 0u);

  input_update(&input);
  input_process_button(&input, BUTTON_LEFT, false_v);
  input_process_key(&input, KEY_BACKSPACE, true_v);
  assert(test_ui_text_field_frame(&renderer, &system, &input, &edit, 1.0 / 60.0,
                                  &capture));
  assert(edit.length == 1u && edit.data[0] == 'A');

  input_update(&input);
  assert(!test_ui_text_field_frame(&renderer, &system, &input, &edit, 0.39,
                                   &capture));
  input_update(&input);
  assert(test_ui_text_field_frame(&renderer, &system, &input, &edit, 0.02,
                                  &capture));
  assert(edit.length == 0u && edit.data[0] == 0u);

  input_update(&input);
  input_process_key(&input, KEY_BACKSPACE, false_v);
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scope));
  assert(vkr_ui_begin(&renderer, &system, &input, false_v, 1.0 / 60.0, NULL));
  capture = vkr_ui_end(&system);
  assert(!capture.keyboard && !capture.text);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  input_shutdown(&input);
  event_manager_destroy(&event_manager);
  vkr_ui_system_shutdown(&renderer, &system);
  teardown_suite();
  printf("  test_ui_text_field_character_input_and_repeat PASSED\n");
}

static void test_ui_input_layer_blocks_click_through(void) {
  printf("  Running test_ui_input_layer_blocks_click_through...\n");
  setup_suite();
  TestCookedFont fixture;
  test_cooked_font_init(&fixture);
  RendererFrontend renderer = {0};
  renderer.allocator = allocator;
  renderer.scratch_allocator = allocator;
  renderer.last_window_width = 200u;
  renderer.last_window_height = 100u;
  renderer.font_system.fonts =
      (Array_VkrFont){.length = 1u, .data = &fixture.font};
  renderer.font_system.default_mtsdf_font_handle = (VkrFontHandle){
      .id = fixture.font.id, .generation = fixture.font.generation};
  VkrUiSystem system = {0};
  assert(vkr_ui_system_init(&renderer, &system));
  vkr_ui_system_set_offscreen_size(&renderer, &system, true_v, 200u, 100u);

  EventManager event_manager = {0};
  event_manager_create(&event_manager);
  InputState input = input_init(&event_manager);
  const VkrUiRect overlay_rect = {0.0f, 0.0f, 200.0f, 100.0f};
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  VkrUiId overlay_id = VKR_UI_ID_NONE;
  for (uint32_t frame = 0u; frame < 3u; ++frame) {
    if (frame == 1u) {
      input_process_mouse_move(&input, 10, 10);
      input_process_button(&input, BUTTON_LEFT, true_v);
    } else if (frame == 2u) {
      input_update(&input);
      input_process_button(&input, BUTTON_LEFT, false_v);
    }
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
    assert(vkr_allocator_scope_is_valid(&scope));
    assert(vkr_ui_begin(&renderer, &system, &input, false_v, 1.0 / 60.0, NULL));
    assert(vkr_ui_input_layer_register(&system, 2u, overlay_rect));
    assert(vkr_ui_input_layer_register(&system, 1u, overlay_rect));
    assert(vkr_ui_input_layer_set(&system, 0u));
    const bool8_t base_clicked = vkr_ui_button(&system, string8_lit("base"),
                                               string8_lit("base"), &button);
    assert(vkr_ui_input_layer_set(&system, 1u));
    const bool8_t lower_clicked = vkr_ui_button(&system, string8_lit("lower"),
                                                string8_lit("lower"), &button);
    assert(vkr_ui_input_layer_set(&system, 2u));
    overlay_id =
        vkr_ui_id_stack_widget_label(&system.id_stack, string8_lit("overlay"));
    const bool8_t overlay_clicked = vkr_ui_button(
        &system, string8_lit("overlay"), string8_lit("overlay"), &button);
    const VkrUiInputCapture capture = vkr_ui_end(&system);
    assert(!base_clicked && !lower_clicked);
    if (frame == 1u) {
      assert(capture.active_id == overlay_id);
      assert(capture.mouse);
    }
    if (frame == 2u)
      assert(overlay_clicked);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (frame != 1u)
      input_update(&input);
  }

  input_shutdown(&input);
  event_manager_destroy(&event_manager);
  vkr_ui_system_shutdown(&renderer, &system);
  teardown_suite();
  printf("  test_ui_input_layer_blocks_click_through PASSED\n");
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
  test_ui_system_reuses_unchanged_draw_geometry();
  test_ui_text_field_character_input_and_repeat();
  test_ui_input_layer_blocks_click_through();

  return true_v;
}
