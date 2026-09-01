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
  test_ui_tile_hashing_and_motion_damage();
  test_ui_dock_layout_drag_and_json_round_trip();
  printf("UI layout tests PASSED\n");
  return true_v;
}
