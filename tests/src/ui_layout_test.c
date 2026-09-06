#include "ui_layout_test.h"

#include "core/ui/vkr_ui_dock.h"
#include "core/ui/vkr_ui_draw.h"
#include "core/ui/vkr_ui_grid.h"
#include "core/ui/vkr_ui_id.h"
#include "core/ui/vkr_ui_style.h"
#include "core/ui/vkr_ui_tile.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool8_t ui_near(float32_t a, float32_t b) {
  return fabsf(a - b) < 0.001f;
}

typedef struct UiJsonSink {
  uint8_t data[VKR_UI_DOCK_JSON_CAPACITY];
  uint64_t length;
} UiJsonSink;

static bool8_t ui_json_sink_write(void *context, const uint8_t *data,
                                  uint64_t length) {
  UiJsonSink *sink = context;
  if (!sink || sink->length + length > sizeof(sink->data))
    return false_v;
  MemCopy(sink->data + sink->length, data, length);
  sink->length += length;
  return true_v;
}

static void test_ui_id_stability(void) {
  printf("  Running test_ui_id_stability...\n");
  VkrUiIdStack stack = {0};
  vkr_ui_id_stack_init(&stack);
  const VkrUiId root = vkr_ui_id_stack_current(&stack);
  const VkrUiId alpha =
      vkr_ui_id_stack_widget_label(&stack, string8_lit("alpha"));
  const VkrUiId beta_before =
      vkr_ui_id_stack_widget_label(&stack, string8_lit("beta"));
  assert(alpha != VKR_UI_ID_NONE && alpha != beta_before);
  assert(vkr_ui_id_stack_widget_label(&stack, string8_lit("alpha")) == alpha);
  assert(vkr_ui_id_stack_widget_label(&stack, string8_lit("beta")) ==
         beta_before);

  assert(vkr_ui_id_stack_push_label(&stack, string8_lit("panel")));
  const VkrUiId nested =
      vkr_ui_id_stack_widget_label(&stack, string8_lit("alpha"));
  assert(nested != alpha);
  assert(vkr_ui_id_stack_pop(&stack));
  assert(vkr_ui_id_stack_current(&stack) == root);
  assert(!vkr_ui_id_stack_pop(&stack));

  const uint8_t label_bytes[8] = {42u};
  const String8 binary_label = {.str = (uint8_t *)label_bytes,
                                .length = sizeof(label_bytes)};
  assert(vkr_ui_id_from_label(root, binary_label) !=
         vkr_ui_id_from_u64(root, 42u));

  for (uint32_t i = 1u; i < VKR_UI_ID_STACK_CAPACITY; ++i)
    assert(vkr_ui_id_stack_push_u64(&stack, i));
  assert(!vkr_ui_id_stack_push_u64(&stack, 99u));
  printf("  test_ui_id_stability PASSED\n");
}

static void test_ui_grid_track_resolution(void) {
  printf("  Running test_ui_grid_track_resolution...\n");
  const VkrUiTrack tracks[] = {
      {.value = 100.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 0.25f, .unit = VKR_UI_TRACK_PCT},
      {.unit = VKR_UI_TRACK_AUTO},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 2.0f, .unit = VKR_UI_TRACK_FR},
  };
  const float32_t intrinsic[] = {0.0f, 0.0f, 80.0f, 0.0f, 0.0f};
  float32_t offsets[ArrayCount(tracks)] = {0};
  float32_t sizes[ArrayCount(tracks)] = {0};
  VkrUiGridAxisOutput output = {
      .offsets_px = offsets,
      .sizes_px = sizes,
      .capacity = ArrayCount(tracks),
  };
  assert(vkr_ui_grid_resolve_tracks(tracks, ArrayCount(tracks), 800.0f, 10.0f,
                                    intrinsic, &output));
  assert(output.fr_iterations == 1u);
  assert(ui_near(sizes[0], 100.0f));
  assert(ui_near(sizes[1], 200.0f));
  assert(ui_near(sizes[2], 80.0f));
  assert(ui_near(sizes[3], 126.6667f));
  assert(ui_near(sizes[4], 253.3333f));
  assert(ui_near(output.resolved_extent_px, 800.0f));
  printf("  test_ui_grid_track_resolution PASSED\n");
}

static void test_ui_grid_fr_iteration_bound(void) {
  printf("  Running test_ui_grid_fr_iteration_bound...\n");
  const VkrUiTrack tracks[] = {
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR, .max_px = 100.0f},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR, .max_px = 280.0f},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR, .max_px = 305.0f},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
  };
  float32_t offsets[ArrayCount(tracks)] = {0};
  float32_t sizes[ArrayCount(tracks)] = {0};
  VkrUiGridAxisOutput output = {
      .offsets_px = offsets,
      .sizes_px = sizes,
      .capacity = ArrayCount(tracks),
  };
  assert(vkr_ui_grid_resolve_tracks(tracks, ArrayCount(tracks), 1000.0f, 0.0f,
                                    NULL, &output));
  assert(output.fr_iterations == 3u);
  assert(ui_near(sizes[0], 100.0f));
  assert(ui_near(sizes[1], 280.0f));
  assert(ui_near(sizes[2], 305.0f));
  assert(ui_near(sizes[3], 310.0f));
  assert(ui_near(output.resolved_extent_px, 995.0f));
  printf("  test_ui_grid_fr_iteration_bound PASSED\n");
}

static void test_ui_grid_arrangement(void) {
  printf("  Running test_ui_grid_arrangement...\n");
  const float32_t column_offsets[] = {0.0f, 110.0f};
  const float32_t column_sizes[] = {100.0f, 100.0f};
  const float32_t row_offsets[] = {0.0f, 55.0f};
  const float32_t row_sizes[] = {50.0f, 50.0f};
  const VkrUiGridItem items[] = {
      {.column = 0u,
       .row = 0u,
       .column_span = 1u,
       .row_span = 1u,
       .max_size_px = {40.0f, 20.0f}},
      {.column = VKR_UI_GRID_AUTO,
       .row = VKR_UI_GRID_AUTO,
       .column_span = 1u,
       .row_span = 1u,
       .justify = VKR_UI_ALIGN_CENTER,
       .align = VKR_UI_ALIGN_END,
       .intrinsic_size_px = {20.0f, 10.0f}},
      {.column = VKR_UI_GRID_AUTO,
       .row = VKR_UI_GRID_AUTO,
       .column_span = 2u,
       .row_span = 1u},
  };
  uint8_t occupancy[4] = {0};
  VkrUiRect rects[ArrayCount(items)] = {0};
  assert(vkr_ui_grid_arrange_items(
      (VkrUiRect){10.0f, 20.0f, 210.0f, 105.0f},
      (VkrUiGridAxisView){column_offsets, column_sizes, 2u},
      (VkrUiGridAxisView){row_offsets, row_sizes, 2u}, items, ArrayCount(items),
      occupancy, ArrayCount(occupancy), rects, ArrayCount(rects)));
  assert(ui_near(rects[0].x, 10.0f) && ui_near(rects[0].y, 20.0f));
  assert(ui_near(rects[0].width, 40.0f) && ui_near(rects[0].height, 20.0f));
  assert(ui_near(rects[1].x, 160.0f) && ui_near(rects[1].y, 60.0f));
  assert(ui_near(rects[2].x, 10.0f) && ui_near(rects[2].y, 75.0f));
  assert(ui_near(rects[2].width, 210.0f));
  printf("  test_ui_grid_arrangement PASSED\n");
}

static void test_ui_grid_auto_placement_cells(void) {
  printf("  Running test_ui_grid_auto_placement_cells...\n");
  const VkrUiGridItem items[] = {
      {.column = VKR_UI_GRID_AUTO,
       .row = VKR_UI_GRID_AUTO,
       .column_span = 1u,
       .row_span = 1u},
      {.column = VKR_UI_GRID_AUTO,
       .row = VKR_UI_GRID_AUTO,
       .column_span = 1u,
       .row_span = 1u},
      {.column = 0u, .row = 1u, .column_span = 2u, .row_span = 1u},
  };
  uint8_t occupancy[4] = {0};
  VkrUiGridCell cells[ArrayCount(items)] = {0};
  assert(vkr_ui_grid_resolve_placements(2u, 2u, items, ArrayCount(items),
                                        occupancy, ArrayCount(occupancy), cells,
                                        ArrayCount(cells)));
  assert(cells[0].column == 0u && cells[0].row == 0u);
  assert(cells[1].column == 1u && cells[1].row == 0u);
  assert(cells[2].column == 0u && cells[2].row == 1u);
  printf("  test_ui_grid_auto_placement_cells PASSED\n");
}

static void test_ui_grid_intrinsic_measurement(void) {
  printf("  Running test_ui_grid_intrinsic_measurement...\n");
  const VkrUiTrack columns[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
  };
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.value = 12.0f, .unit = VKR_UI_TRACK_PX},
  };
  const VkrUiGridItem items[] = {
      {.column = 0u,
       .row = 0u,
       .column_span = 1u,
       .row_span = 1u,
       .intrinsic_size_px = {20.0f, 10.0f},
       .margin_px = {1.0f, 2.0f, 3.0f, 4.0f}},
      {.column = 1u,
       .row = 0u,
       .column_span = 1u,
       .row_span = 2u,
       .intrinsic_size_px = {30.0f, 40.0f}},
      {.column = 0u,
       .row = 0u,
       .column_span = 2u,
       .row_span = 1u,
       .intrinsic_size_px = {70.0f, 8.0f}},
  };
  const VkrUiGridCell cells[] = {{0u, 0u}, {1u, 0u}, {0u, 0u}};
  float32_t column_sizes[ArrayCount(columns)] = {0};
  float32_t row_sizes[ArrayCount(rows)] = {0};
  VkrUiGridIntrinsicOutput intrinsic = {0};
  assert(vkr_ui_grid_measure_intrinsic(
      columns, ArrayCount(columns), rows, ArrayCount(rows), 5.0f, items, cells,
      ArrayCount(items), column_sizes, ArrayCount(column_sizes), row_sizes,
      ArrayCount(row_sizes), &intrinsic));
  assert(ui_near(column_sizes[0], 35.0f));
  assert(ui_near(column_sizes[1], 30.0f));
  assert(ui_near(intrinsic.width_px, 70.0f));
  assert(ui_near(row_sizes[0], 23.0f));
  assert(ui_near(row_sizes[1], 12.0f));
  assert(ui_near(intrinsic.height_px, 40.0f));
  printf("  test_ui_grid_intrinsic_measurement PASSED\n");
}

static void test_ui_style_content_scale(void) {
  printf("  Running test_ui_style_content_scale...\n");
  VkrUiStyle style = vkr_ui_style_default();
  style.margin_pt = (VkrUiEdges){1.0f, 2.0f, 3.0f, 4.0f};
  style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  style.padding_pt = (VkrUiEdges){2.0f, 3.0f, 4.0f, 5.0f};
  style.corner_radius_pt = (Vec4){2.0f, 3.0f, 4.0f, 5.0f};
  style.min_size_pt = (Vec2){20.0f, 10.0f};
  style.max_size_pt = (Vec2){100.0f, 50.0f};
  style.gap_pt = 6.0f;
  VkrUiResolvedStyle resolved = {0};
  assert(vkr_ui_style_resolve(&style, 2.0f, &resolved));
  assert(resolved.margin_px.left == 8.0f);
  assert(resolved.padding_px.bottom == 8.0f);
  assert(resolved.corner_radius_px.w == 10.0f);
  assert(resolved.font_size_px == 28.0f);
  assert(resolved.gap_px == 12.0f);
  const VkrUiRect content = vkr_ui_style_content_rect(
      (VkrUiRect){10.0f, 20.0f, 100.0f, 80.0f}, &resolved);
  assert(content.x == 22.0f && content.y == 26.0f);
  assert(content.width == 80.0f && content.height == 64.0f);
  assert(!vkr_ui_style_resolve(&style, 0.0f, &resolved));
  printf("  test_ui_style_content_scale PASSED\n");
}

static void test_ui_draw_build(void) {
  printf("  Running test_ui_draw_build...\n");
  VkrUiDrawCommand commands[8] = {0};
  VkrUiDrawBuffer buffer = {0};
  assert(
      vkr_ui_draw_buffer_begin(&buffer, commands, ArrayCount(commands),
                               (VkrUiRect){.width = 100.0f, .height = 80.0f}));
  assert(vkr_ui_draw_buffer_solid(&buffer,
                                  (VkrUiRect){10.0f, 20.0f, 30.0f, 10.0f},
                                  (Vec4){1.0f, 0.0f, 0.0f, 1.0f}));
  assert(vkr_ui_draw_buffer_solid(&buffer,
                                  (VkrUiRect){40.0f, 20.0f, 20.0f, 10.0f},
                                  (Vec4){0.0f, 1.0f, 0.0f, 1.0f}));
  assert(vkr_ui_draw_buffer_push_clip(&buffer,
                                      (VkrUiRect){20.2f, 10.2f, 30.2f, 20.2f}));
  assert(vkr_ui_draw_buffer_image(
      &buffer, (VkrUiRect){20.0f, 10.0f, 30.0f, 20.0f},
      (Vec4){0.0f, 0.0f, 1.0f, 1.0f}, (Vec4){1.0f, 1.0f, 1.0f, 1.0f},
      (VkrUiTextureRef){3u, 7u}));
  assert(vkr_ui_draw_buffer_pop_clip(&buffer));
  assert(vkr_ui_draw_buffer_rounded_rect(
      &buffer, (VkrUiRect){70.0f, 60.0f, 20.0f, 10.0f},
      (Vec4){0.2f, 0.2f, 0.2f, 1.0f}, (Vec4){20.0f, 4.0f, 3.0f, 2.0f}));

  VkrUiVertex vertices[16] = {0};
  uint32_t indices[24] = {0};
  VkrUiDrawBatch batches[4] = {0};
  VkrUiDrawOutput output = {
      .vertices = vertices,
      .vertex_capacity = ArrayCount(vertices),
      .indices = indices,
      .index_capacity = ArrayCount(indices),
      .batches = batches,
      .batch_capacity = ArrayCount(batches),
  };
  const VkrUiDrawBuildResult result =
      vkr_ui_draw_build(&buffer, 100u, 80u, &output);
  assert(result.status == VKR_UI_DRAW_BUILD_OK);
  assert(output.vertex_count == 16u && output.index_count == 24u);
  assert(output.batch_count == 3u && batches[0].index_count == 12u);
  assert(vertices[0].position.x == 10.0f && vertices[0].position.y == 50.0f);
  assert(vertices[2].position.x == 40.0f && vertices[2].position.y == 60.0f);
  assert(batches[1].scissor_rect_px.x == 20.0f);
  assert(batches[1].scissor_rect_px.y == 10.0f);
  assert(batches[1].scissor_rect_px.width == 31.0f);
  assert(batches[1].scissor_rect_px.height == 21.0f);
  assert(batches[2].corner_radius_px.x == 5.0f);

  output.vertex_capacity = 4u;
  output.index_capacity = 6u;
  const VkrUiDrawBuildResult truncated =
      vkr_ui_draw_build(&buffer, 100u, 80u, &output);
  assert(truncated.status == VKR_UI_DRAW_BUILD_TRUNCATED);
  assert(truncated.dropped_command_count == 3u);
  printf("  test_ui_draw_build PASSED\n");
}

static void test_ui_icon_coverage_and_capacity(void) {
  printf("  Running test_ui_icon_coverage_and_capacity...\n");
  VkrUiDrawCommand commands[2] = {0};
  VkrUiDrawBuffer buffer = {0};
  assert(vkr_ui_draw_buffer_begin(&buffer, commands, 2u,
                                  (VkrUiRect){0, 0, 100, 100}));
  const Vec2 square[] = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
  const Vec2 triangle[] = {{30, 10}, {30, 20}, {40, 15}, {40, 15}};
  const Vec4 color = {0.2f, 0.4f, 0.6f, 0.8f};
  assert(vkr_ui_draw_buffer_polygon(&buffer, square, color));
  assert(vkr_ui_draw_buffer_polygon(&buffer, triangle, color));
  VkrUiVertex vertices[14];
  uint32_t indices[51];
  VkrUiDrawBatch batches[1];
  VkrUiDrawOutput output = {.vertices = vertices,
                            .vertex_capacity = 14u,
                            .indices = indices,
                            .index_capacity = 51u,
                            .batches = batches,
                            .batch_capacity = 1u};
  assert(vkr_ui_draw_build(&buffer, 100u, 100u, &output).status ==
         VKR_UI_DRAW_BUILD_OK);
  assert(output.vertex_count == 14u && output.index_count == 51u);
  assert(output.batch_count == 1u && batches[0].index_count == 51u);
  // Coverage transitions over exactly one physical pixel, centered on the
  // authored edge. RGB remains linear and unchanged as alpha falls to zero.
  assert(ui_near(vertices[0].position.x, 10.5f));
  assert(ui_near(vertices[0].position.y, 89.5f));
  assert(ui_near(vertices[4].position.x, 9.5f));
  assert(ui_near(vertices[4].position.y, 90.5f));
  assert(ui_near(vertices[0].color.w, 0.8f) && vertices[4].color.w == 0.0f);
  assert(ui_near((vertices[0].color.w + vertices[4].color.w) * 0.5f, 0.4f));
  assert(ui_near(vertices[4].color.x, color.x) &&
         ui_near(vertices[4].color.z, color.z));
  assert(ui_near(commands[0].rect_px.x, 9.5f) &&
         ui_near(commands[0].rect_px.width, 11.0f));
  for (uint32_t i = 0u; i < output.index_count; ++i)
    assert(indices[i] < output.vertex_count);
  // Capacity rejection preserves complete shapes; a partial fringe is visible
  // corruption, even when its opaque center would fit the remaining storage.
  output.index_capacity = 50u;
  VkrUiDrawBuildResult result = vkr_ui_draw_build(&buffer, 100u, 100u, &output);
  assert(result.status == VKR_UI_DRAW_BUILD_TRUNCATED &&
         result.dropped_command_count == 1u);
  assert(output.vertex_count == 8u && output.index_count == 30u);
  output.vertex_capacity = 7u;
  result = vkr_ui_draw_build(&buffer, 100u, 100u, &output);
  assert(result.status == VKR_UI_DRAW_BUILD_TRUNCATED &&
         result.dropped_command_count == 2u);
  assert(output.vertex_count == 0u && output.index_count == 0u);
  // Reverse winding produces the same inset, not an opaque expanded outline.
  const Vec2 reversed[] = {{10, 10}, {10, 20}, {20, 20}, {20, 10}};
  assert(vkr_ui_draw_buffer_begin(&buffer, commands, 2u,
                                  (VkrUiRect){0, 0, 100, 100}));
  assert(vkr_ui_draw_buffer_polygon(&buffer, reversed, color));
  output.vertex_capacity = 14u;
  assert(vkr_ui_draw_build(&buffer, 100u, 100u, &output).status ==
         VKR_UI_DRAW_BUILD_OK);
  assert(ui_near(vertices[0].position.x, 10.5f) &&
         ui_near(vertices[0].position.y, 89.5f));
  printf("  test_ui_icon_coverage_and_capacity PASSED\n");
}

static void test_ui_tile_hashing_and_motion_damage(void) {
  printf("  Running test_ui_tile_hashing_and_motion_damage...\n");
  Arena *arena = arena_create(MB(1), MB(1));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  VkrUiTileCache cache = {0};
  vkr_ui_tile_cache_init(&cache, &allocator);
  VkrUiDrawCommand command = {
      .rect_px = {65.0f, 10.0f, 1.0f, 1.0f},
      .clip_rect_px = {0.0f, 0.0f, 130.0f, 70.0f},
      .uv_rect = {0.0f, 0.0f, 1.0f, 1.0f},
      .color = {1.0f, 1.0f, 1.0f, 1.0f},
      .texture = {1u, 1u},
      .mode = VKR_UI_DRAW_MODE_MTSDF_TEXT,
      .screen_px_range = 2.0f,
      .sdf_unit_range = {0.01f, 0.01f},
  };
  VkrUiTileFrame frame = {0};
  assert(vkr_ui_tile_build(&cache, &allocator, 130u, 70u, 64u, &command, 1u,
                           NULL, 0u, &frame));
  assert(frame.column_count == 3u && frame.row_count == 2u);
  assert(frame.tile_count == 6u && frame.bin_entry_count == 2u);
  assert(frame.dirty_tile_count == 6u && frame.dirty_tile_ratio == 1.0f);

  assert(vkr_ui_tile_build(&cache, &allocator, 130u, 70u, 64u, &command, 1u,
                           NULL, 0u, &frame));
  assert(frame.dirty_tile_count == 0u && frame.dirty_tile_ratio == 0.0f);

  command.color.x = 0.5f;
  assert(vkr_ui_tile_build(&cache, &allocator, 130u, 70u, 64u, &command, 1u,
                           NULL, 0u, &frame));
  assert(frame.dirty_tile_count == 2u);
  assert(frame.dirty_tiles[0] && frame.dirty_tiles[1]);

  const VkrUiRect previous = vkr_ui_tile_command_aabb(&command, 130u, 70u);
  command.rect_px.x = 127.0f;
  const VkrUiRect current = vkr_ui_tile_command_aabb(&command, 130u, 70u);
  const VkrUiTileDamage motion = {
      .previous_aabb_px = previous,
      .current_aabb_px = current,
  };
  assert(vkr_ui_tile_build(&cache, &allocator, 130u, 70u, 64u, &command, 1u,
                           &motion, 1u, &frame));
  assert(frame.dirty_tile_count == 3u);
  assert(frame.dirty_tiles[0] && frame.dirty_tiles[1] && frame.dirty_tiles[2]);

  const VkrUiTileDamage removed = {.previous_aabb_px = current};
  assert(vkr_ui_tile_build(&cache, &allocator, 130u, 70u, 64u, NULL, 0u,
                           &removed, 1u, &frame));
  assert(frame.dirty_tile_count == 2u);
  assert(frame.dirty_tiles[1] && frame.dirty_tiles[2]);

  VkrUiDrawCommand ordered[2] = {
      command,
      command,
  };
  ordered[0].rect_px = (VkrUiRect){4.0f, 4.0f, 8.0f, 8.0f};
  ordered[1].rect_px = (VkrUiRect){16.0f, 4.0f, 8.0f, 8.0f};
  assert(vkr_ui_tile_build(&cache, &allocator, 64u, 64u, 64u, ordered, 2u, NULL,
                           0u, &frame));
  assert(frame.tile_count == 1u && frame.bin_entry_count == 2u);
  assert(frame.tile_offsets[0] == 0u && frame.tile_offsets[1] == 2u);
  assert(frame.command_indices[0] == 0u && frame.command_indices[1] == 1u);
  assert(frame.dirty_tile_count == 1u);
  uint32_t *cached_offsets = frame.tile_offsets;
  uint32_t *cached_indices = frame.command_indices;
  assert(vkr_ui_tile_build(&cache, &allocator, 64u, 64u, 64u, ordered, 2u, NULL,
                           0u, &frame));
  assert(frame.dirty_tile_count == 0u);
  assert(frame.bin_entry_count == 2u && frame.tile_offsets == cached_offsets);
  assert(frame.command_indices == cached_indices);
  const VkrUiTileDamage forced_motion = {
      .previous_aabb_px = {4.0f, 4.0f, 8.0f, 8.0f},
      .current_aabb_px = {32.0f, 4.0f, 8.0f, 8.0f},
  };
  assert(vkr_ui_tile_build(&cache, &allocator, 64u, 64u, 64u, ordered, 2u,
                           &forced_motion, 1u, &frame));
  assert(frame.dirty_tile_count == 1u);
  const VkrUiDrawCommand first = ordered[0];
  ordered[0] = ordered[1];
  ordered[1] = first;
  assert(vkr_ui_tile_build(&cache, &allocator, 64u, 64u, 64u, ordered, 2u, NULL,
                           0u, &frame));
  assert(frame.dirty_tile_count == 1u);

  VkrUiDrawCommand edge = ordered[0];
  edge.mode = VKR_UI_DRAW_MODE_QUAD;
  edge.screen_px_range = 0.0f;
  edge.rect_px = (VkrUiRect){63.0f, 4.0f, 1.0f, 1.0f};
  edge.clip_rect_px = (VkrUiRect){0.0f, 0.0f, 64.0f, 64.0f};
  assert(vkr_ui_tile_build(&cache, &allocator, 64u, 64u, 64u, &edge, 1u, NULL,
                           0u, &frame));
  assert(frame.bin_entry_count == 1u && frame.dirty_tile_count == 1u);
  assert(vkr_ui_tile_build(&cache, &allocator, 63u, 64u, 64u, &edge, 1u, NULL,
                           0u, &frame));
  assert(frame.tile_count == 1u && frame.bin_entry_count == 0u);
  assert(frame.dirty_tile_count == 1u);

  vkr_ui_tile_cache_destroy(&cache);
  arena_destroy(arena);
  printf("  test_ui_tile_hashing_and_motion_damage PASSED\n");
}

static void test_ui_dock_layout_drag_and_json_round_trip(void) {
  printf("  Running test_ui_dock_layout_drag_and_json_round_trip...\n");
  VkrUiDockTree tree = {0};
  vkr_ui_dock_default_editor_layout(&tree);
  assert(vkr_ui_dock_validate(&tree));
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 1000.0f, 800.0f},
                            8.0f, 28.0f));
  assert(ui_near(tree.nodes[1u].rect_px.height, 35.0f));
  assert(!vkr_ui_dock_set_split_ratio(&tree, 0u, 0.25f));
  uint32_t scene_leaf = VKR_UI_DOCK_NODE_NONE;
  VkrUiRect scene = {0};
  assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT,
                                &scene_leaf, &scene));
  assert(scene_leaf == 7u && vkr_ui_rect_has_area(scene));
  assert(scene.y > tree.nodes[scene_leaf].rect_px.y);

  assert(vkr_ui_dock_set_split_ratio(&tree, 3u, 0.25f));
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 1000.0f, 800.0f},
                            8.0f, 28.0f));
  const float32_t moved_scene_x = tree.nodes[scene_leaf].rect_px.x;

  assert(vkr_ui_dock_move_tab(&tree, 1u, 0u, 8u, 1u, VKR_UI_DOCK_DROP_CENTER));
  assert(tree.root == 2u);
  assert(vkr_ui_dock_move_tab(&tree, 8u, 1u, 5u, 0u, VKR_UI_DOCK_DROP_LEFT));
  assert(vkr_ui_dock_validate(&tree));

  UiJsonSink sink = {0};
  VkrJsonWriter writer = {0};
  vkr_json_writer_init(&writer, ui_json_sink_write, &sink);
  assert(vkr_ui_dock_write_json(&writer, &tree));
  assert(vkr_json_writer_complete(&writer));
  VkrUiDockTree restored = {0};
  assert(
      vkr_ui_dock_read_json(string8_create(sink.data, sink.length), &restored));
  assert(vkr_ui_dock_validate(&restored));
  assert(vkr_ui_dock_layout(&restored, (VkrUiRect){0.0f, 0.0f, 1000.0f, 800.0f},
                            8.0f, 28.0f));
  assert(vkr_ui_dock_find_panel(&restored, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT,
                                &scene_leaf, &scene));
  assert(ui_near(restored.nodes[scene_leaf].rect_px.x, moved_scene_x));

  restored.interaction.tab_leaf = scene_leaf;
  restored.interaction.resize_split = 3u;
  restored.interaction.dragging_tab = true_v;
  InputState input = {0};
  const VkrUiDockInputCapture capture =
      vkr_ui_dock_update_input(&restored, &input, true_v);
  assert(!capture.mouse && !capture.dragging_tab && !capture.resizing_split);
  assert(restored.interaction.tab_leaf == VKR_UI_DOCK_NODE_NONE);
  assert(restored.interaction.resize_split == VKR_UI_DOCK_NODE_NONE);
  assert(!restored.interaction.dragging_tab);
  printf("  test_ui_dock_layout_drag_and_json_round_trip PASSED\n");
}

static void test_ui_dock_close_nested_sibling_focus(void) {
  printf("  Running test_ui_dock_close_nested_sibling_focus...\n");
  VkrUiDockTree tree = {0};
  vkr_ui_dock_default_editor_layout(&tree);
  const uint64_t closed_id = tree.nodes[4u].as.leaf.tabs[0u].id;
  tree.focused_tab_id = closed_id;

  assert(vkr_ui_dock_close_tab(&tree, 4u, 0u));
  assert(vkr_ui_dock_validate(&tree));
  assert(tree.nodes[tree.root].kind == VKR_UI_DOCK_NODE_SPLIT);
  bool8_t focused_live = false_v;
  for (uint32_t leaf = 0u; leaf < tree.node_high_water; ++leaf) {
    const VkrUiDockNode *node = &tree.nodes[leaf];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab)
      focused_live |= node->as.leaf.tabs[tab].id == tree.focused_tab_id;
  }
  assert(focused_live && tree.focused_tab_id != closed_id);
  printf("  test_ui_dock_close_nested_sibling_focus PASSED\n");
}

static void test_ui_dock_compact_tabs_and_stack_interaction(void) {
  printf("  Running test_ui_dock_compact_tabs_and_stack_interaction...\n");
  VkrUiDockTree tree = {0};
  vkr_ui_dock_default_editor_layout(&tree);
  assert(vkr_ui_dock_move_tab(&tree, 5u, 0u, 4u, 0u, VKR_UI_DOCK_DROP_CENTER));
  assert(vkr_ui_dock_move_tab(&tree, 8u, 0u, 4u, 1u, VKR_UI_DOCK_DROP_CENTER));
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 1000.0f, 800.0f},
                            3.0f, 28.0f));
  /* A wide stack has compact tabs, followed by noninteractive empty strip. */
  VkrUiRect first = vkr_ui_dock_tab_rect(&tree, 4u, 0u);
  VkrUiRect third = vkr_ui_dock_tab_rect(&tree, 4u, 2u);
  assert(ui_near(first.width, 116.0f));
  assert(ui_near(third.x, 232.0f) && ui_near(third.width, 102.0f));
  InputState input = {0};
  input.current_buttons = (ButtonsState){.x = 260, .y = (int32_t)third.y + 12};
  input.current_buttons.buttons[BUTTON_LEFT] = true_v;
  VkrUiDockInputCapture capture =
      vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(capture.mouse && tree.interaction.tab_index == 2u);
  assert(tree.nodes[4u].as.leaf.active_tab == 2u);

  /* Move Console before Inspector by crossing the insertion midpoint. */
  input.previous_buttons = input.current_buttons;
  input.current_buttons.x = 130;
  capture = vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(capture.dragging_tab);
  assert(tree.interaction.drop_leaf == 4u && tree.interaction.drop_index == 1u);
  assert(tree.interaction.drop_zone == VKR_UI_DOCK_DROP_CENTER);
  input.previous_buttons = input.current_buttons;
  input.current_buttons.buttons[BUTTON_LEFT] = false_v;
  (void)vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(tree.nodes[4u].as.leaf.tabs[0u].panel_kind ==
         VKR_UI_DOCK_PANEL_HIERARCHY);
  assert(tree.nodes[4u].as.leaf.tabs[1u].panel_kind ==
         VKR_UI_DOCK_PANEL_CONSOLE);
  assert(tree.nodes[4u].as.leaf.tabs[2u].panel_kind ==
         VKR_UI_DOCK_PANEL_INSPECTOR);
  assert(tree.nodes[4u].as.leaf.active_tab == 1u);

  input.previous_buttons = input.current_buttons;
  input.current_buttons.x = 600;
  input.current_buttons.buttons[BUTTON_LEFT] = true_v;
  (void)vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(tree.interaction.tab_leaf == VKR_UI_DOCK_NODE_NONE);

  /* Removing an earlier inactive tab must keep Console selected. */
  const uint64_t console_id = tree.nodes[4u].as.leaf.tabs[1u].id;
  assert(vkr_ui_dock_move_tab(&tree, 4u, 0u, 7u, 1u, VKR_UI_DOCK_DROP_CENTER));
  assert(tree.nodes[4u].as.leaf.tabs[tree.nodes[4u].as.leaf.active_tab].id ==
         console_id);

  /* A tab may split out of its own stack without losing its identity. */
  assert(vkr_ui_dock_move_tab(&tree, 4u, 0u, 4u, 0u, VKR_UI_DOCK_DROP_TOP));
  assert(vkr_ui_dock_validate(&tree));
  uint32_t console_leaf = VKR_UI_DOCK_NODE_NONE;
  assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_CONSOLE, &console_leaf,
                                0));
  assert(console_leaf != 4u);
  assert(tree.nodes[console_leaf].as.leaf.tabs[0u].id == console_id);
  /* Resizing starts anywhere in the wider hit target without moving on press,
     then publishes new rectangles during the same input update. */
  vkr_ui_dock_default_editor_layout(&tree);
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 1000.0f, 800.0f},
                            3.0f, 28.0f));
  const float32_t split_x = tree.nodes[5u].rect_px.width;
  const float32_t initial_ratio = tree.nodes[3u].as.split.ratio;
  input = (InputState){0};
  input.current_buttons.x = (int32_t)split_x - 1;
  input.current_buttons.y = (int32_t)tree.nodes[5u].rect_px.y + 40;
  input.current_buttons.buttons[BUTTON_LEFT] = true_v;
  capture = vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(capture.resizing_split && tree.interaction.resize_split == 3u);
  assert(tree.nodes[3u].as.split.ratio == initial_ratio);
  input.previous_buttons = input.current_buttons;
  input.current_buttons.x += 40;
  (void)vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(ui_near(tree.nodes[5u].rect_px.width, split_x + 40.0f));
  /* An extreme top/bottom drag preserves 64pt of content below the 28pt tab
     bar. Nested horizontal splits must reserve 96pt for each descendant. */
  vkr_ui_dock_default_editor_layout(&tree);
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 800.0f, 600.0f},
                            3.0f, 28.0f));
  const VkrUiRect separator = vkr_ui_dock_split_bar_rect(&tree, 2u);
  assert(ui_near(separator.height, 3.0f));
  assert(ui_near(separator.y + separator.height, tree.nodes[4u].rect_px.y));
  input = (InputState){0};
  input.current_buttons.x = 400;
  input.current_buttons.y = (int32_t)separator.y + 1;
  input.current_buttons.buttons[BUTTON_LEFT] = true_v;
  capture = vkr_ui_dock_update_input(&tree, &input, false_v);
  assert(capture.resizing_split && tree.interaction.resize_split == 2u);
  input.previous_buttons = input.current_buttons;
  input.current_buttons.y = -1000;
  (void)vkr_ui_dock_update_input(&tree, &input, false_v);
  VkrUiRect scene_content;
  assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT, NULL,
                                &scene_content));
  assert(ui_near(scene_content.height, 64.0f));
  assert(ui_near(tree.nodes[1u].rect_px.height, 35.0f));
  assert(vkr_ui_dock_set_split_ratio(&tree, 3u, 1.0f));
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 800.0f, 600.0f},
                            3.0f, 28.0f));
  assert(tree.nodes[5u].rect_px.width >= 96.0f);
  assert(tree.nodes[7u].rect_px.width >= 96.0f);
  assert(tree.nodes[8u].rect_px.width >= 96.0f);
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 1600.0f, 1200.0f},
                            6.0f, 56.0f));
  assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT, NULL,
                                &scene_content));
  assert(scene_content.height >= 128.0f);
  assert(tree.nodes[7u].rect_px.width >= 192.0f);
  assert(tree.nodes[8u].rect_px.width >= 192.0f);
  /* When the root cannot fit the minima, all descendants stay inside it. */
  assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0.0f, 0.0f, 80.0f, 60.0f}, 3.0f,
                            28.0f));
  for (uint32_t i = 0u; i < tree.node_high_water; ++i) {
    const VkrUiRect rect = tree.nodes[i].rect_px;
    assert(rect.width >= 0.0f && rect.height >= 0.0f);
    assert(rect.x >= 0.0f && rect.y >= 0.0f);
    assert(rect.x + rect.width <= 80.0f && rect.y + rect.height <= 60.0f);
  }
  printf("  test_ui_dock_compact_tabs_and_stack_interaction PASSED\n");
}

/* Native event drains may deliver movement and release without an intervening
   frame. The same 40px resize and Console->Inspector drop must result for a
   held gesture, a release-only endpoint, and a complete same-frame gesture. */
static void test_ui_dock_release_endpoint_and_coalesced_gesture(void) {
  for (uint32_t mode = 0; mode < 3; ++mode) {
    VkrUiDockTree tree = {0};
    vkr_ui_dock_default_editor_layout(&tree);
    assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0, 0, 1000, 800}, 3, 28));
    const VkrUiRect separator = vkr_ui_dock_split_bar_rect(&tree, 3u);
    const float32_t original_width = tree.nodes[5u].rect_px.width;
    InputState input = {0};
    input.current_buttons.x = (int32_t)separator.x + 1;
    input.current_buttons.y = (int32_t)separator.y + 50;
    input.current_buttons.buttons[BUTTON_LEFT] = true_v;
    input.pressed_buttons[BUTTON_LEFT] = true_v;
    input.button_press_x[BUTTON_LEFT] = input.current_buttons.x;
    input.button_press_y[BUTTON_LEFT] = input.current_buttons.y;
    if (mode != 2) {
      (void)vkr_ui_dock_update_input(&tree, &input, false_v);
      input_update(&input);
    }
    input.current_buttons.x += 40;
    if (mode == 0) {
      (void)vkr_ui_dock_update_input(&tree, &input, false_v);
      input_update(&input);
    }
    input.current_buttons.buttons[BUTTON_LEFT] = false_v;
    input.released_buttons[BUTTON_LEFT] = true_v;
    VkrUiDockInputCapture capture =
        vkr_ui_dock_update_input(&tree, &input, false_v);
    assert(capture.mouse &&
           ui_near(tree.nodes[5u].rect_px.width, original_width + 40));
    assert(tree.interaction.resize_split == VKR_UI_DOCK_NODE_NONE);

    vkr_ui_dock_default_editor_layout(&tree);
    assert(vkr_ui_dock_layout(&tree, (VkrUiRect){0, 0, 1000, 800}, 3, 28));
    uint32_t console, inspector;
    assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_CONSOLE, &console,
                                  NULL));
    assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_INSPECTOR,
                                  &inspector, NULL));
    const uint64_t console_id = tree.nodes[console].as.leaf.tabs[0].id;
    const VkrUiRect tab = vkr_ui_dock_tab_rect(&tree, console, 0);
    const VkrUiRect target = tree.nodes[inspector].rect_px;
    input = (InputState){0};
    input.current_buttons.x = (int32_t)tab.x + 20;
    input.current_buttons.y = (int32_t)tab.y + 12;
    input.current_buttons.buttons[BUTTON_LEFT] = true_v;
    input.pressed_buttons[BUTTON_LEFT] = true_v;
    input.button_press_x[BUTTON_LEFT] = input.current_buttons.x;
    input.button_press_y[BUTTON_LEFT] = input.current_buttons.y;
    if (mode != 2) {
      (void)vkr_ui_dock_update_input(&tree, &input, false_v);
      input_update(&input);
    }
    input.current_buttons.x = (int32_t)(target.x + target.width * 0.5f);
    input.current_buttons.y = (int32_t)(target.y + target.height * 0.5f);
    if (mode == 0) {
      (void)vkr_ui_dock_update_input(&tree, &input, false_v);
      input_update(&input);
    }
    input.current_buttons.buttons[BUTTON_LEFT] = false_v;
    input.released_buttons[BUTTON_LEFT] = true_v;
    capture = vkr_ui_dock_update_input(&tree, &input, false_v);
    assert(capture.mouse && vkr_ui_dock_validate(&tree));
    assert(vkr_ui_dock_find_panel(&tree, VKR_UI_DOCK_PANEL_CONSOLE, &console,
                                  NULL));
    /* find_panel locates visible content. Inspector is now inactive, so verify
       its retained tab in the original destination stack instead. */
    assert(console == inspector && tree.nodes[console].as.leaf.tab_count == 2);
    assert(tree.nodes[console].as.leaf.tabs[0].panel_kind ==
           VKR_UI_DOCK_PANEL_INSPECTOR);
    assert(tree.nodes[console]
               .as.leaf.tabs[tree.nodes[console].as.leaf.active_tab]
               .id == console_id);
    assert(tree.interaction.tab_leaf == VKR_UI_DOCK_NODE_NONE);
  }
}

bool32_t run_ui_layout_tests(void) {
  printf("Running UI layout tests...\n");
  test_ui_id_stability();
  test_ui_grid_track_resolution();
  test_ui_grid_fr_iteration_bound();
  test_ui_grid_arrangement();
  test_ui_grid_auto_placement_cells();
  test_ui_grid_intrinsic_measurement();
  test_ui_style_content_scale();
  test_ui_draw_build();
  test_ui_icon_coverage_and_capacity();
  test_ui_tile_hashing_and_motion_damage();
  test_ui_dock_layout_drag_and_json_round_trip();
  test_ui_dock_close_nested_sibling_focus();
  test_ui_dock_compact_tabs_and_stack_interaction();
  test_ui_dock_release_endpoint_and_coalesced_gesture();
  printf("UI layout tests PASSED\n");
  return true_v;
}
