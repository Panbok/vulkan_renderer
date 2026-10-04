#include "editor_partition.h"

#include "editor_details.h"
#include "editor_internal.h"
#include "renderer/systems/vkr_scene_partition.h"

#include <stdio.h>

#define PARTITION_PAD_PT 10.0f
#define PARTITION_ROW_PT 26.0f
/* Cells a side of the map, centred on the camera's cell. */
#define PARTITION_MAP_CELLS 15

bool8_t vkr_editor_partition_request(const VkrSampleUiFrame *frame,
                                     bool8_t unload, bool8_t all,
                                     const int32_t cells[4], char *message,
                                     uint32_t capacity) {
  SceneWorldPartition settings;
  if (!frame->scene || !frame->scene_edit ||
      !vkr_scene_partition_settings(frame->scene, &settings)) {
    snprintf(message, capacity, "The open scene has no world partition");
    return false_v;
  }
  VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_PARTITION,
                                 .partition_unload = unload,
                                 .partition_all = all};
  MemCopy(request.partition_cells, cells, sizeof(request.partition_cells));
  *frame->scene_edit = request;
  if (all) {
    snprintf(message, capacity, "Unloading every loaded cell");
  } else {
    snprintf(message, capacity, "%s cells %d,%d to %d,%d",
             unload ? "Unloading" : "Loading", cells[0], cells[1], cells[2],
             cells[3]);
  }
  return true_v;
}

static void partition_label(VkrUiSystem *ui, String8 id, float32_t y,
                            float32_t width, String8 text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      PARTITION_PAD_PT, y, width - PARTITION_PAD_PT * 2.0f,
      PARTITION_ROW_PT - 4.0f);
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, id, text, &label);
}

void vkr_editor_partition_window_build(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect bounds) {
  (void)editor;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / ui->content_scale;
  const VkrScene *scene = frame->scene;
  SceneWorldPartition settings;
  float32_t y = 8.0f;
  if (!scene || !vkr_scene_partition_settings(scene, &settings)) {
    partition_label(ui, string8_lit("partition.none"), y, width,
                    string8_lit("This scene has no World partition "
                                "component; add one to stream its cells"));
    return;
  }
  uint32_t count = 0u;
  const VkrScenePartitionCellRecord *records =
      vkr_scene_partition_cells(scene, &count);
  uint32_t loaded = 0u;
  uint32_t documents = 0u;
  uint32_t proxies = 0u;
  uint32_t pinned = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    loaded += (records[i].flags & VKR_SCENE_PARTITION_CELL_LOADED) != 0u;
    documents += (records[i].flags & VKR_SCENE_PARTITION_CELL_ON_DISK) != 0u;
    proxies += records[i].proxy.u64 != 0u;
    pinned += (records[i].flags & VKR_SCENE_PARTITION_CELL_PINNED) != 0u;
  }
  partition_label(
      ui, string8_lit("partition.settings"), y, width,
      string8_create_formatted(ui->frame_allocator,
                               "Cells %.0f m, load within %.0f m, proxies "
                               "within %.0f m",
                               (float64_t)settings.cell_size,
                               (float64_t)settings.load_radius,
                               (float64_t)settings.proxy_radius));
  y += PARTITION_ROW_PT - 4.0f;
  partition_label(
      ui, string8_lit("partition.counts"), y, width,
      string8_create_formatted(ui->frame_allocator,
                               "%u loaded (%u pinned), %u documents, %u "
                               "proxies",
                               loaded, pinned, documents, proxies));
  y += PARTITION_ROW_PT;

  /* The map: pinned cells in the accent, other loaded ones in the success
     colour, documents waiting in the raised surface, proxies in the info
     colour. A click
     loads and pins a cell, or unloads a pinned one. */
  /* Cells are in document space, which an origin rebase offsets. */
  const Vec3 at = vec3_add(scene->stream_source_count ? scene->stream_sources[0]
                                                      : vec3_zero(),
                           scene->origin_offset);
  const VkrScenePartitionCell centre =
      vkr_scene_partition_cell_at(&settings, at);
  const float32_t map_w = width - PARTITION_PAD_PT * 2.0f;
  const float32_t step = map_w / (float32_t)PARTITION_MAP_CELLS;
  const int32_t half = PARTITION_MAP_CELLS / 2;
  for (int32_t row = 0; row < PARTITION_MAP_CELLS; ++row) {
    for (int32_t column = 0; column < PARTITION_MAP_CELLS; ++column) {
      /* North (-Z) at the top. */
      const VkrScenePartitionCell cell = {centre.x + column - half,
                                          centre.z + row - half};
      const VkrScenePartitionCellRecord *record = NULL;
      for (uint32_t i = 0; i < count && !record; ++i) {
        if (records[i].cell.x == cell.x && records[i].cell.z == cell.z) {
          record = &records[i];
        }
      }
      const uint32_t flags = record ? record->flags : 0u;
      Vec4 colour = vkr_ui_color_alpha(theme->field, 0.6f);
      if (flags & VKR_SCENE_PARTITION_CELL_PINNED) {
        colour = theme->accent;
      } else if (flags & VKR_SCENE_PARTITION_CELL_LOADED) {
        colour = vkr_ui_color_alpha(theme->success, 0.6f);
      } else if (record && record->proxy.u64) {
        colour = vkr_ui_color_alpha(theme->info, 0.5f);
      } else if (flags & VKR_SCENE_PARTITION_CELL_ON_DISK) {
        colour = theme->raised_hover;
      }
      VkrUiWidgetConfig square = vkr_editor_details_widget(
          PARTITION_PAD_PT + step * (float32_t)column,
          y + step * (float32_t)row, step - 2.0f, step - 2.0f);
      square.style.background_color = colour;
      square.style.hover_background_color =
          vkr_ui_color_alpha(theme->accent_hover, 0.7f);
      if (row == half && column == half) {
        square.icon = VKR_UI_ICON_CAMERA;
        square.icon_size_pt = Min(12.0f, step - 4.0f);
      }
      square.tooltip = string8_create_formatted(
          ui->frame_allocator, "Cell %d,%d: %s", cell.x, cell.z,
          flags & VKR_SCENE_PARTITION_CELL_PINNED    ? "pinned for editing"
          : flags & VKR_SCENE_PARTITION_CELL_LOADED  ? "loaded"
          : record && record->proxy.u64              ? "proxy"
          : flags & VKR_SCENE_PARTITION_CELL_ON_DISK ? "saved, not loaded"
                                                     : "empty");
      (void)vkr_ui_push_id_u64(ui,
                               (uint64_t)(row * PARTITION_MAP_CELLS + column));
      if (vkr_ui_button(ui, string8_lit("partition.cell"), (String8){0},
                        &square)) {
        const int32_t range[4] = {cell.x, cell.z, cell.x, cell.z};
        char message[128];
        (void)vkr_editor_partition_request(
            frame, (flags & VKR_SCENE_PARTITION_CELL_PINNED) != 0u, false_v,
            range, message, sizeof(message));
      }
      (void)vkr_ui_pop_id(ui);
    }
  }
  y += step * (float32_t)PARTITION_MAP_CELLS + 8.0f;
  VkrUiWidgetConfig release = vkr_editor_details_widget(
      PARTITION_PAD_PT, y, width - PARTITION_PAD_PT * 2.0f, PARTITION_ROW_PT);
  release.tooltip = string8_lit("Unpin every cell; those without unsaved or "
                                "undoable edits unload");
  if (vkr_ui_button(ui, string8_lit("partition.release"),
                    string8_lit("Release pinned cells"), &release)) {
    const int32_t range[4] = {0, 0, 0, 0};
    char message[128];
    (void)vkr_editor_partition_request(frame, true_v, true_v, range, message,
                                       sizeof(message));
  }
}
