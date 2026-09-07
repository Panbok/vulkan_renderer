#include "editor_internal.h"

#include <stdio.h>
#include <string.h>

#define EDITOR_COMMAND_LAYER (VKR_EDITOR_WINDOW_COUNT + 3u)

static bool8_t editor_menu_button(VkrUiSystem *ui, String8 id, String8 content,
                                  uint32_t column, bool8_t active,
                                  VkrFontHandle heading_font) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = (VkrUiPlacement){
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  button.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 10.0f};
  button.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  button.style.font_size_pt = 12.0f;
  button.text.font = heading_font;
  button.style.background_color = active ? (Vec4){0.12f, 0.30f, 0.46f, 0.82f}
                                         : (Vec4){0.07f, 0.09f, 0.13f, 0.30f};
  button.style.text_color = active ? (Vec4){0.82f, 0.94f, 1.0f, 1.0f}
                                   : (Vec4){0.72f, 0.77f, 0.84f, 1.0f};
  return vkr_ui_button(ui, id, content, &button);
}

static void editor_window_raise(VkrEditorUi *editor, VkrEditorWindowKind kind) {
  VkrEditorWindowState *window = &editor->windows[kind];
  const uint32_t old_z = window->z_order;
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    if (editor->windows[i].z_order > old_z)
      editor->windows[i].z_order--;
  }
  window->z_order = VKR_EDITOR_WINDOW_COUNT;
  window->visible = true_v;
}

static bool8_t editor_toolbar_button(VkrUiSystem *ui, String8 id,
                                     VkrUiIcon icon, String8 tooltip,
                                     uint32_t index, uint32_t columns,
                                     Vec4 color, bool8_t disabled) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = VKR_UI_PLACEMENT_DEFAULT;
  button.placement.column = index % columns;
  button.placement.row = index / columns;
  button.placement.align = VKR_UI_ALIGN_CENTER;
  button.style.min_size_pt = (Vec2){26.0f, 26.0f};
  button.style.max_size_pt = button.style.min_size_pt;
  button.style.padding_pt = (VkrUiEdges){5.0f, 5.0f, 5.0f, 5.0f};
  button.style.corner_radius_pt = (Vec4){3.0f, 3.0f, 3.0f, 3.0f};
  button.style.background_color =
      (Vec4){color.x * 0.25f, color.y * 0.25f, color.z * 0.25f, 1.0f};
  button.style.text_color = color;
  button.icon = icon;
  button.icon_size_pt = 16.0f;
  button.disabled = disabled;
  button.tooltip = tooltip;
  return vkr_ui_button(ui, id, (String8){0}, &button);
}

void vkr_editor_scene_toolbar_update(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->mapping_valid) {
    editor->toolbar_dragging = false_v;
    editor->toolbar_rect_pt = (Vec4){0};
    return;
  }
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const float32_t top = frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0;
  const float32_t width = viewport.z / scale;
  const float32_t height = Max(0.0f, viewport.w / scale - top);
  const float32_t inset = Min(8.0f, Min(width, height) * 0.5f);
  const float32_t available = Max(0.0f, width - 2.0f * inset);
  uint32_t columns = 8u;
  while (columns > 1u && (float32_t)columns * 30.0f + 8.0f > available)
    columns /= 2u;
  editor->toolbar_columns = columns;
  const Vec2 size = {Min(available, (float32_t)columns * 30.0f + 8.0f),
                     Min(Max(0.0f, height - 2.0f * inset),
                         (float32_t)(8u / columns) * 30.0f + 8.0f)};
  const Vec2 limit = {Max(0.0f, width - size.x - 2.0f * inset),
                      Max(0.0f, height - size.y - 2.0f * inset)};
  if (!editor->toolbar_initialized) {
    editor->toolbar_offset_pt = (Vec2){limit.x * 0.5f, 0};
    editor->toolbar_anchor_y = -1;
    editor->toolbar_initialized = true_v;
  }
  bool8_t gesture = editor->toolbar_dragging;
  if (ui->mouse_pressed && !frame->mouse_captured && !editor->commands_open &&
      editor->menu == VKR_EDITOR_MENU_NONE) {
    int32_t press_x = 0, press_y = 0;
    input_get_button_press_position(frame->input, BUTTON_LEFT, &press_x,
                                    &press_y);
    const float32_t x = (float32_t)press_x / scale;
    const float32_t y = (float32_t)press_y / scale;
    const Vec4 previous = editor->toolbar_rect_pt;
    bool8_t occluded = false_v;
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      const VkrEditorWindowState *window = &editor->windows[i];
      occluded |= window->visible && x >= window->position_pt.x &&
                  x < window->position_pt.x + window->size_pt.x &&
                  y >= window->position_pt.y &&
                  y < window->position_pt.y + window->size_pt.y;
    }
    if (!occluded && previous.z > 0 && previous.w > 0 && x >= previous.x + 4 &&
        x < previous.x + Min(previous.z, 34.0f) && y >= previous.y + 4 &&
        y < previous.y + Min(previous.w, 34.0f)) {
      editor->toolbar_grab_pt = (Vec2){x - previous.x, y - previous.y};
      editor->toolbar_dragging = gesture = true_v;
    }
  }
  if (editor->toolbar_dragging && !frame->mouse_captured) {
    /* Current position includes a release processed in the same frame as its
       press. Preserve the initial grip offset rather than accumulating deltas.
     */
    editor->toolbar_offset_pt =
        (Vec2){(float32_t)ui->mouse_x / scale - editor->toolbar_grab_pt.x -
                   viewport.x / scale - inset,
               (float32_t)ui->mouse_y / scale - editor->toolbar_grab_pt.y -
                   viewport.y / scale - top - inset};
    editor->toolbar_anchor_x = editor->toolbar_anchor_y = 0;
    if (!input_is_button_down(frame->input, BUTTON_LEFT)) {
      editor->toolbar_dragging = false_v;
      const Vec2 offset = editor->toolbar_offset_pt;
      editor->toolbar_anchor_x = offset.x < 16.0f             ? -1
                                 : limit.x - offset.x < 16.0f ? 1
                                                              : 0;
      editor->toolbar_anchor_y = offset.y < 16.0f             ? -1
                                 : limit.y - offset.y < 16.0f ? 1
                                                              : 0;
    }
  } else if (frame->mouse_captured) {
    editor->toolbar_dragging = false_v;
    gesture = false_v;
  }
  if (editor->toolbar_anchor_x)
    editor->toolbar_offset_pt.x = editor->toolbar_anchor_x < 0 ? 0 : limit.x;
  if (editor->toolbar_anchor_y)
    editor->toolbar_offset_pt.y = editor->toolbar_anchor_y < 0 ? 0 : limit.y;
  editor->toolbar_offset_pt.x =
      vkr_clamp_f32(editor->toolbar_offset_pt.x, 0, limit.x);
  editor->toolbar_offset_pt.y =
      vkr_clamp_f32(editor->toolbar_offset_pt.y, 0, limit.y);
  editor->toolbar_rect_pt =
      (Vec4){viewport.x / scale + inset + editor->toolbar_offset_pt.x,
             viewport.y / scale + top + inset + editor->toolbar_offset_pt.y,
             size.x, size.y};
  const Vec4 rect = editor->toolbar_rect_pt;
  const VkrUiRect capture = gesture
                                ? (VkrUiRect){0, 0, (float32_t)ui->target_width,
                                              (float32_t)ui->target_height}
                                : (VkrUiRect){rect.x * scale, rect.y * scale,
                                              rect.z * scale, rect.w * scale};
  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER,
                                    capture);
}

static VkrUiRect editor_scene_error_rect(const VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  if (frame->scene_error == VKR_RENDERER_ERROR_NONE ||
      !frame->scene_rendering_stopped)
    return (VkrUiRect){0};
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const float32_t top =
      viewport.y / scale +
      (frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0.0f) + 8.0f;
  const float32_t bottom = (viewport.y + viewport.w) / scale - 8.0f;
  const float32_t width = Min(480.0f, Max(0.0f, viewport.z / scale - 16.0f));
  const float32_t height = Min(72.0f, Max(0.0f, bottom - top));
  if (width <= 0 || height <= 0)
    return (VkrUiRect){0};
  float32_t y = editor->toolbar_rect_pt.y + editor->toolbar_rect_pt.w + 8.0f;
  if (y + height > bottom)
    y = editor->toolbar_rect_pt.y - height - 8.0f;
  y = vkr_clamp_f32(y, top, Max(top, bottom - height));
  return (VkrUiRect){viewport.x / scale + (viewport.z / scale - width) * 0.5f,
                     y, width, height};
}

static void editor_scene_error_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  const VkrUiRect rect = editor_scene_error_rect(editor, frame);
  if (!vkr_ui_rect_has_area(rect))
    return;
  VkrUiSystem *ui = frame->ui;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.style = vkr_editor_glass_style();
  panel.style.background_color = (Vec4){0.20f, 0.07f, 0.06f, 0.96f};
  panel.style.border_color = (Vec4){0.70f, 0.29f, 0.24f, 1.0f};
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){rect.width, rect.height};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.scene.error"), &panel))
    return;
  const bool8_t narrow = rect.width < 320.0f;
  VkrUiWidgetConfig label = vkr_editor_text_config(
      narrow ? 10.0f : 12.0f, (Vec4){1.0f, 0.88f, 0.82f, 1.0f});
  label.placement.column = label.placement.row = 0u;
  label.placement.justify = VKR_UI_ALIGN_STRETCH;
  label.text.layout.word_wrap = true_v;
  label.text.font = editor->heading_font;
  vkr_ui_label(
      ui, string8_lit("message"),
      frame->scene_error == VKR_RENDERER_ERROR_OUT_OF_MEMORY
          ? (narrow ? string8_lit("Scene memory limit.\nUnload or retry.")
                    : string8_lit("Scene memory limit reached.\n"
                                  "Unload or retry rendering."))
          : (narrow ? string8_lit("Scene render failed.\nUnload or retry.")
                    : string8_lit("Scene rendering failed.\n"
                                  "Unload or retry rendering.")),
      &label);
  (void)vkr_ui_panel_end(ui);
}

static void editor_scene_resolution_build(const VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid || frame->scene_render_width == 0u)
    return;
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const bool8_t fallback = frame->scene_output_scale < 1.0f;
  const float32_t width = Min(320.0f, viewport.z / scale - 16.0f);
  const bool8_t incomplete =
      frame->texture_pending_count || frame->texture_demanded_missing_count;
  const float32_t height =
      (fallback ? 58.0f : 42.0f) + 114.0f + (incomplete ? 28.0f : 0.0f);
  const float32_t left = viewport.x / scale + 8.0f;
  const float32_t right = (viewport.x + viewport.z) / scale - width - 8.0f;
  const float32_t top =
      viewport.y / scale + 8.0f +
      (frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0.0f);
  const float32_t bottom = (viewport.y + viewport.w) / scale - height - 8.0f;
  if (width < 100.0f || top > bottom)
    return;
  const VkrUiRect candidates[] = {
      {right, bottom, width, height},
      {left, bottom, width, height},
      {right, top, width, height},
      {left, top, width, height},
  };
  const Vec4 toolbar = editor->toolbar_rect_pt;
  const VkrUiRect occupied[] = {
      {toolbar.x - 4.0f, toolbar.y - 4.0f, toolbar.z + 8.0f, toolbar.w + 8.0f},
      editor_scene_error_rect(editor, frame),
  };
  uint32_t corner = 0u;
  for (; corner < ArrayCount(candidates); ++corner) {
    bool8_t overlaps = false_v;
    for (uint32_t i = 0u; i < ArrayCount(occupied); ++i)
      overlaps |= vkr_ui_rect_has_area(
          vkr_ui_rect_intersect(candidates[corner], occupied[i]));
    if (!overlaps)
      break;
  }
  if (corner == ArrayCount(candidates))
    return;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt =
      (VkrUiEdges){candidates[corner].y, 0, 0, candidates[corner].x};
  panel.style = vkr_editor_glass_style();
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){width, height};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.scene.resolution"), &panel))
    return;
  String8 text =
      fallback ? string8_create_formatted(
                     ui->frame_allocator,
                     "Render %ux%u\nOutput %ux%u\nMemory fallback: %.0f%%",
                     frame->scene_render_width, frame->scene_render_height,
                     frame->scene_output_width, frame->scene_output_height,
                     (double)(frame->scene_output_scale * 100.0f))
               : string8_create_formatted(
                     ui->frame_allocator, "Render %ux%u\nOutput %ux%u",
                     frame->scene_render_width, frame->scene_render_height,
                     frame->scene_output_width, frame->scene_output_height);
  const String8 performance = frame->scene_rendering_stopped
                                  ? string8_lit("Scene stopped")
                              : !frame->scene ? string8_lit("No Scene loaded")
                                              : frame->text.performance;
  text = string8_create_formatted(ui->frame_allocator, "%.*s\n%.*s\n%.*s",
                                  (int32_t)performance.length, performance.str,
                                  (int32_t)text.length, text.str,
                                  (int32_t)frame->text.system.length,
                                  frame->text.system.str);
  if (incomplete)
    text = string8_create_formatted(
        ui->frame_allocator, "%.*s\nTexture wait: %u\nMissing: %u",
        (int32_t)text.length, text.str, frame->texture_pending_count,
        frame->texture_demanded_missing_count);
  VkrUiWidgetConfig label = vkr_editor_text_config(
      10.0f, (fallback || incomplete) ? (Vec4){1.0f, 0.79f, 0.35f, 1.0f}
                                      : (Vec4){0.78f, 0.84f, 0.90f, 1.0f});
  label.placement.column = label.placement.row = 0u;
  label.placement.justify = VKR_UI_ALIGN_STRETCH;
  label.text.layout.word_wrap = true_v;
  vkr_ui_label(ui, string8_lit("resolution"), text, &label);
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_scene_toolbar_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  const Vec4 rect = editor->toolbar_rect_pt;
  if (rect.z <= 0.0f || rect.w <= 0.0f)
    return;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack tracks[8] = {{.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = 0u;
  panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.columns = tracks;
  panel.column_count = editor->toolbar_columns;
  panel.rows = tracks;
  panel.row_count = 8u / editor->toolbar_columns;
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
  panel.style.gap_pt = 0;
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){rect.z, rect.w};
  panel.style.background_color.w = 0.95f;
  panel.clip_children = true_v;
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.scene.toolbar"), &panel)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }
  const Vec4 green = {0.65f, 0.84f, 0.67f, 1};
  const Vec4 amber = {0.84f, 0.69f, 0.43f, 1};
  const Vec4 blue = {0.58f, 0.77f, 0.91f, 1};
  const Vec4 red = {0.95f, 0.65f, 0.66f, 1};
  const uint32_t cols = editor->toolbar_columns;
  (void)editor_toolbar_button(
      ui, string8_lit("grip"), VKR_UI_ICON_GRIP,
      string8_lit("Drag Scene controls; release near an edge to snap"), 0, cols,
      (Vec4){0.65f, 0.70f, 0.75f, 1}, false_v);
  if (editor_toolbar_button(ui, string8_lit("scene.load"),
                            VKR_UI_ICON_SCENE_LOAD, string8_lit("Load scene"),
                            1, cols, green, frame->scene != NULL))
    *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_LOAD};
  if (editor_toolbar_button(
          ui, string8_lit("scene.unload"), VKR_UI_ICON_SCENE_UNLOAD,
          string8_lit("Unload scene"), 2, cols, red, frame->scene == NULL))
    *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_UNLOAD};
  if (editor_toolbar_button(ui, string8_lit("simulation.start"),
                            VKR_UI_ICON_PLAY,
                            string8_lit("Start / resume simulation"), 3, cols,
                            green, frame->simulation_running))
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_SIMULATION;
  if (editor_toolbar_button(ui, string8_lit("simulation.pause"),
                            VKR_UI_ICON_PAUSE, string8_lit("Pause simulation"),
                            4, cols, amber, !frame->simulation_running))
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION;
  if (editor_toolbar_button(ui, string8_lit("render.start"),
                            VKR_UI_ICON_MONITOR_PLAY,
                            string8_lit("Start scene rendering"), 5, cols, blue,
                            !frame->scene_rendering_stopped))
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_RENDERING;
  if (editor_toolbar_button(ui, string8_lit("render.stop"),
                            VKR_UI_ICON_MONITOR_STOP,
                            string8_lit("Stop scene rendering"), 6, cols, red,
                            frame->scene_rendering_stopped))
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_STOP_RENDERING;
  if (editor_toolbar_button(
          ui, string8_lit("camera.enter"), VKR_UI_ICON_CAMERA,
          frame->mouse_captured
              ? string8_lit("Free camera active; Escape to release")
              : string8_lit("Hold RMB to fly; Tab / F3 toggles camera; Escape "
                            "releases"),
          7, cols, frame->mouse_captured ? amber : blue,
          frame->scene_rendering_stopped))
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_TOGGLE_CAMERA;
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
  editor_scene_resolution_build(editor, frame);
  editor_scene_error_build(editor, frame);
}

void vkr_editor_windows_build_navigation(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack nav_columns[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_START,
  };
  bar.columns = nav_columns;
  bar.column_count = ArrayCount(nav_columns);
  bar.rows = &one_track;
  bar.row_count = 1u;
  bar.style.padding_pt = (VkrUiEdges){4.0f, 8.0f, 4.0f, 8.0f};
  bar.style.gap_pt = 3.0f;
  bar.style.min_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.max_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  bar.style.background_color = (Vec4){0.094f, 0.106f, 0.118f, 1.0f};
  bar.style.border_color = (Vec4){0.255f, 0.282f, 0.298f, 1.0f};
  (void)vkr_ui_input_layer_set(ui, editor->commands_open ? EDITOR_COMMAND_LAYER
                                                         : 0u);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.navigation"), &bar))
    return;

  VkrUiWidgetConfig brand =
      vkr_editor_text_config(12.0f, (Vec4){0.58f, 0.77f, 0.91f, 1.0f});
  brand.text.font = editor->heading_font;
  brand.icon = VKR_UI_ICON_SCENE;
  brand.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 7.0f, 0.0f, 0.0f},
  };
  vkr_ui_label(ui, string8_lit("brand"), string8_lit("VKR / EDITOR"), &brand);

  if (editor_menu_button(
          ui, string8_lit("menu.metrics"), string8_lit("Metrics"), 1u,
          editor->menu == VKR_EDITOR_MENU_METRICS, editor->heading_font)) {
    editor->commands_open = false_v;
    editor->menu = editor->menu == VKR_EDITOR_MENU_METRICS
                       ? VKR_EDITOR_MENU_NONE
                       : VKR_EDITOR_MENU_METRICS;
  }
  if (editor_menu_button(ui, string8_lit("menu.debug"), string8_lit("Debug"),
                         2u, editor->menu == VKR_EDITOR_MENU_DEBUG,
                         editor->heading_font)) {
    editor->commands_open = false_v;
    editor->menu = editor->menu == VKR_EDITOR_MENU_DEBUG
                       ? VKR_EDITOR_MENU_NONE
                       : VKR_EDITOR_MENU_DEBUG;
  }
  if (editor_menu_button(ui, string8_lit("menu.help"), string8_lit("Help"), 3u,
                         editor->windows[VKR_EDITOR_WINDOW_HELP].visible,
                         editor->heading_font)) {
    editor->commands_open = false_v;
    if (editor->windows[VKR_EDITOR_WINDOW_HELP].visible)
      editor->windows[VKR_EDITOR_WINDOW_HELP].visible = false_v;
    else
      editor_window_raise(editor, VKR_EDITOR_WINDOW_HELP);
    editor->menu = VKR_EDITOR_MENU_NONE;
  }
  const bool8_t bakery_visible =
      vkr_ui_dock_find_panel(frame->dock, VKR_UI_DOCK_PANEL_BAKERY, NULL, NULL);
  if (editor_menu_button(ui, string8_lit("menu.bakery"), string8_lit("Bakery"),
                         4u, bakery_visible, editor->heading_font)) {
    editor->commands_open = false_v;
    editor->menu = VKR_EDITOR_MENU_NONE;
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_BAKERY);
  }
  if (editor_menu_button(ui, string8_lit("menu.commands"),
                         string8_lit("Commands"), 5u, editor->commands_open,
                         editor->heading_font)) {
    editor->commands_open = !editor->commands_open;
    editor->commands_focus_search = editor->commands_open;
    editor->commands_cursor = 0;
    editor->commands_first_row = 0;
    editor->commands_repeat_direction = 0;
    if (!editor->commands_open) {
      ui->focused_id = ui->active_id = VKR_UI_ID_NONE;
      (void)vkr_ui_keyboard_layer_set(ui, 0u);
    }
    editor->menu = VKR_EDITOR_MENU_NONE;
  }

  VkrUiWidgetConfig status =
      vkr_editor_text_config(11.0f, (Vec4){0.68f, 0.72f, 0.74f, 1.0f});
  status.placement = (VkrUiPlacement){
      .column = 7u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_END,
      .align = VKR_UI_ALIGN_CENTER,
  };
  if ((float32_t)ui->target_width / ui->content_scale >= 880.0f) {
    const String8 content = string8_create_formatted(
        ui->frame_allocator, "%s / %s  %.1fs",
        frame->simulation_running ? "SIM" : "PAUSED",
        frame->scene_rendering_stopped ? "FROZEN" : "LIVE",
        frame->simulation_time);
    vkr_ui_label(ui, string8_lit("status"), content, &status);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
}

static VkrUiRect editor_menu_popup_rect(const VkrEditorUi *editor,
                                        const VkrUiSystem *ui) {
  const float32_t scale = ui->content_scale;
  const float32_t target_width = (float32_t)ui->target_width / scale;
  const float32_t target_height = (float32_t)ui->target_height / scale;
  const bool8_t debug = editor->menu == VKR_EDITOR_MENU_DEBUG;
  const float32_t width = Min(debug ? 230.0f : 210.0f, target_width);
  const float32_t height =
      Min(debug ? (editor->labels_expanded ? 184.0f : 50.0f) : 94.0f,
          Max(0.0f, target_height - VKR_EDITOR_NAVIGATION_HEIGHT_PT));
  const float32_t x = Min(VKR_EDITOR_METRICS_MENU_X_PT + (debug ? 72.0f : 0),
                          Max(0.0f, target_width - width));
  return (VkrUiRect){x * scale, VKR_EDITOR_NAVIGATION_HEIGHT_PT * scale,
                     width * scale, height * scale};
}

void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui) {
  if (editor->menu == VKR_EDITOR_MENU_NONE)
    return;

  vkr_ui_keyboard_layer_set(ui, VKR_EDITOR_WINDOW_COUNT + 2u);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_WINDOW_COUNT + 2u);
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO}, {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO}, {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const VkrUiRect rect = editor_menu_popup_rect(editor, ui);
  const bool8_t debug = editor->menu == VKR_EDITOR_MENU_DEBUG;
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {rect.y / ui->content_scale, 0.0f, 0.0f,
                    rect.x / ui->content_scale},
  };
  popup.columns = &one_track;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = debug ? (editor->labels_expanded ? 5u : 1u) : 3u;
  popup.style = vkr_editor_glass_style();
  popup.style.background_color.w = 0.98f;
  popup.style.min_size_pt =
      (Vec2){rect.width / ui->content_scale, rect.height / ui->content_scale};
  popup.style.max_size_pt = popup.style.min_size_pt;
  popup.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.menu.popup"), &popup)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }

  if (debug) {
    VkrUiWidgetConfig item =
        vkr_editor_text_config(12.0f, (Vec4){0.80f, 0.84f, 0.90f, 1.0f});
    item.placement.column = item.placement.row = 0u;
    item.placement.justify = VKR_UI_ALIGN_STRETCH;
    item.style.padding_pt = (VkrUiEdges){4.0f, 3.0f, 4.0f, 3.0f};
    item.text.font = editor->heading_font;
    const bool8_t toggle_labels =
        vkr_ui_button(ui, string8_lit("labels"),
                      editor->labels_expanded ? string8_lit("Labels  -")
                                              : string8_lit("Labels  +"),
                      &item);
    if (editor->labels_expanded) {
      item.placement.row = 1u;
      (void)vkr_ui_checkbox(ui, string8_lit("labels.enabled"),
                            string8_lit("All light labels"),
                            &editor->labels_enabled, &item);
      item.placement.row = 2u;
      item.placement.margin_pt.left = 12.0f;
      item.disabled = !editor->labels_enabled;
      (void)vkr_ui_checkbox(ui, string8_lit("labels.directional"),
                            string8_lit("Directional lights"),
                            &editor->labels_directional, &item);
      item.placement.row = 3u;
      (void)vkr_ui_checkbox(ui, string8_lit("labels.spot"),
                            string8_lit("Spot lights"), &editor->labels_spot,
                            &item);
      item.placement.row = 4u;
      (void)vkr_ui_checkbox(ui, string8_lit("labels.point"),
                            string8_lit("Point lights"), &editor->labels_point,
                            &item);
    }
    (void)vkr_ui_panel_end(ui);
    if (toggle_labels)
      editor->labels_expanded = !editor->labels_expanded;
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }

  VkrUiWidgetConfig heading =
      vkr_editor_text_config(12.0f, (Vec4){0.84f, 0.69f, 0.43f, 1.0f});
  heading.text.font = editor->heading_font;
  heading.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
  };
  vkr_ui_label(ui, string8_lit("heading"), string8_lit("METRICS WINDOWS"),
               &heading);

  VkrUiWidgetConfig item =
      vkr_editor_text_config(12.0f, (Vec4){0.80f, 0.84f, 0.90f, 1.0f});
  item.style.padding_pt = (VkrUiEdges){4.0f, 3.0f, 4.0f, 3.0f};
  item.placement = heading.placement;
  item.placement.row = 1u;
  VkrEditorWindowState *draws = &editor->windows[VKR_EDITOR_WINDOW_DRAWS];
  if (vkr_ui_checkbox(ui, string8_lit("draws"), string8_lit("Draws"),
                      &draws->visible, &item) &&
      draws->visible)
    editor_window_raise(editor, VKR_EDITOR_WINDOW_DRAWS);
  item.placement.row = 2u;
  VkrEditorWindowState *memory = &editor->windows[VKR_EDITOR_WINDOW_MEMORY];
  if (vkr_ui_checkbox(ui, string8_lit("memory"), string8_lit("Memory"),
                      &memory->visible, &item) &&
      memory->visible)
    editor_window_raise(editor, VKR_EDITOR_WINDOW_MEMORY);
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
}

static VkrUiRect editor_window_rect(const VkrUiSystem *ui,
                                    const VkrEditorWindowState *window) {
  const float32_t scale = ui->content_scale;
  return (VkrUiRect){
      .x = window->position_pt.x * scale,
      .y = window->position_pt.y * scale,
      .width = window->size_pt.x * scale,
      .height = window->size_pt.y * scale,
  };
}

static bool8_t editor_point_in_rect(int32_t x, int32_t y, VkrUiRect rect) {
  return (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.width &&
         (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.height;
}

static void editor_window_clamp(VkrUiSystem *ui, VkrEditorWindowState *window) {
  const float32_t width_pt = (float32_t)ui->target_width / ui->content_scale;
  const float32_t height_pt = (float32_t)ui->target_height / ui->content_scale;
  const float32_t visible_title_pt = 72.0f;
  window->position_pt.x =
      vkr_clamp_f32(window->position_pt.x, 0.0f,
                    vkr_max_f32(0.0f, width_pt - visible_title_pt));
  window->position_pt.y = vkr_clamp_f32(window->position_pt.y, 35.0f,
                                        vkr_max_f32(35.0f, height_pt - 28.0f));
}

void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui) {
  if (editor->commands_open) {
    (void)vkr_ui_input_layer_register(
        ui, EDITOR_COMMAND_LAYER,
        (VkrUiRect){0, 0, (float32_t)ui->target_width,
                    (float32_t)ui->target_height});
    (void)vkr_ui_keyboard_layer_set(ui, EDITOR_COMMAND_LAYER);
    ui->capture.keyboard = true_v;
    ui->capture.mouse = true_v;
    return;
  }
  if (ui->keyboard_input_layer == EDITOR_COMMAND_LAYER)
    (void)vkr_ui_keyboard_layer_set(ui, 0);

  if (editor->menu != VKR_EDITOR_MENU_NONE)
    vkr_ui_keyboard_layer_set(ui, VKR_EDITOR_WINDOW_COUNT + 2u);
  else if (ui->keyboard_input_layer == VKR_EDITOR_WINDOW_COUNT + 2u)
    vkr_ui_keyboard_layer_set(ui, 0u);
  const bool8_t popup_contains_pointer =
      editor->menu != VKR_EDITOR_MENU_NONE &&
      editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                           editor_menu_popup_rect(editor, ui));
  if (ui->mouse_pressed && !popup_contains_pointer) {
    uint32_t top_z = 0u;
    VkrEditorWindowKind top_kind = VKR_EDITOR_WINDOW_COUNT;
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      VkrEditorWindowState *window = &editor->windows[i];
      editor_window_clamp(ui, window);
      if (window->visible && window->z_order > top_z &&
          editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                               editor_window_rect(ui, window))) {
        top_z = window->z_order;
        top_kind = (VkrEditorWindowKind)i;
      }
    }
    if (top_kind < VKR_EDITOR_WINDOW_COUNT)
      editor_window_raise(editor, top_kind);
  }

  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    VkrEditorWindowState *window = &editor->windows[i];
    editor_window_clamp(ui, window);
    if (window->visible)
      (void)vkr_ui_input_layer_register(ui, window->z_order + 1u,
                                        editor_window_rect(ui, window));
  }
  if (editor->menu != VKR_EDITOR_MENU_NONE)
    (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_WINDOW_COUNT + 2u,
                                      editor_menu_popup_rect(editor, ui));
}

static void editor_build_window(VkrEditorUi *editor, VkrUiSystem *ui,
                                InputState *input, VkrEditorWindowKind kind,
                                const VkrSampleUiText *text) {
  VkrEditorWindowState *window = &editor->windows[kind];
  String8 title_text = {0};
  String8 body_text = {0};
  float32_t font_size_pt = 10.0f;
  switch (kind) {
  case VKR_EDITOR_WINDOW_DRAWS:
    title_text = string8_lit("DRAWS / RENDER GRAPH");
    body_text = text->metrics;
    break;
  case VKR_EDITOR_WINDOW_MEMORY:
    title_text = string8_lit("MEMORY / LIVE");
    body_text = text->memory;
    break;
  case VKR_EDITOR_WINDOW_HELP:
    title_text = string8_lit("EDITOR CONTROLS");
    body_text =
        string8_lit("Cmd/Ctrl+P   Commands: scene, panels, transport\n"
                    "Hold RMB in Scene to fly; release RMB to stop\n"
                    "Tab/F3 or Camera button toggles fly; Esc releases\n"
                    "F8  Cycle IBL mode\n"
                    "F9 / F10  IBL intensity     G   Camera snapshot\n"
                    "Click a title bar to focus and drag a window.");
    font_size_pt = 11.0f;
    break;
  default:
    return;
  }

  editor_window_clamp(ui, window);
  (void)vkr_ui_input_layer_set(ui, window->z_order + 1u);
  (void)vkr_ui_push_id_u64(ui, kind);
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = 28.0f, .unit = VKR_UI_TRACK_PX},
      one_track,
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt =
          {
              .top = window->position_pt.y,
              .left = window->position_pt.x,
          },
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.min_size_pt = window->size_pt;
  panel.style.max_size_pt = window->size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("window"), &panel)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }

  const VkrUiTrack header_columns[] = {
      one_track,
      {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  header.columns = header_columns;
  header.column_count = ArrayCount(header_columns);
  header.rows = &one_track;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  header.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  header.style.background_color = (Vec4){0.055f, 0.075f, 0.105f, 0.91f};
  header.style.border_color = (Vec4){0.24f, 0.42f, 0.58f, 0.50f};
  if (vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    VkrUiWidgetConfig drag = vkr_ui_widget_config_default();
    drag.placement = (VkrUiPlacement){
        .column = 0u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    drag.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 10.0f};
    drag.style.corner_radius_pt = (Vec4){0};
    drag.style.background_color = (Vec4){0};
    drag.style.text_color = (Vec4){0.43f, 0.80f, 1.0f, 1.0f};
    drag.style.font_size_pt = 11.0f;
    drag.text.font = editor->heading_font;
    const VkrUiId drag_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("drag"));
    (void)vkr_ui_button(ui, string8_lit("drag"), title_text, &drag);
    if (ui->active_id == drag_id && input_is_button_down(input, BUTTON_LEFT)) {
      int32_t dx = 0;
      int32_t dy = 0;
      input_get_mouse_delta(input, &dx, &dy);
      window->position_pt.x += (float32_t)dx / ui->content_scale;
      window->position_pt.y += (float32_t)dy / ui->content_scale;
      editor_window_clamp(ui, window);
    }

    VkrUiWidgetConfig close = vkr_ui_widget_config_default();
    close.placement = (VkrUiPlacement){
        .column = 1u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    close.style.padding_pt = (VkrUiEdges){0};
    close.style.corner_radius_pt = (Vec4){0};
    close.style.background_color = (Vec4){0.10f, 0.12f, 0.16f, 0.50f};
    close.style.text_color = (Vec4){0.76f, 0.80f, 0.86f, 1.0f};
    close.style.font_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("close"), string8_lit("x"), &close))
      window->visible = false_v;
    (void)vkr_ui_panel_end(ui);
  }

  VkrUiWidgetConfig body =
      vkr_editor_text_config(font_size_pt, (Vec4){0.84f, 0.87f, 0.92f, 1.0f});
  body.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 1u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {10.0f, 12.0f, 10.0f, 12.0f},
  };
  vkr_ui_label(ui, string8_lit("body"), body_text, &body);
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiText *text) {
  for (uint32_t z = 1u; z <= VKR_EDITOR_WINDOW_COUNT; ++z) {
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      if (editor->windows[i].visible && editor->windows[i].z_order == z) {
        editor_build_window(editor, ui, input, (VkrEditorWindowKind)i, text);
        break;
      }
    }
  }
}

typedef enum EditorCommand {
  CMD_LOAD,
  CMD_RELOAD,
  CMD_UNLOAD,
  CMD_SAVE,
  CMD_UNDO,
  CMD_REDO,
  CMD_FRAME,
  CMD_HIERARCHY,
  CMD_INSPECTOR,
  CMD_CONSOLE,
  CMD_BAKERY,
  CMD_RESET_LAYOUT,
  CMD_SIM_START,
  CMD_SIM_PAUSE,
  CMD_RENDER_START,
  CMD_RENDER_STOP,
  CMD_COUNT
} EditorCommand;

static const char *s_command_names[CMD_COUNT] = {"Load scene",
                                                 "Reload scene",
                                                 "Unload scene",
                                                 "Save scene edits",
                                                 "Undo scene edit",
                                                 "Redo scene edit",
                                                 "Frame selected node",
                                                 "Show Hierarchy",
                                                 "Show Inspector",
                                                 "Show Console",
                                                 "Show Bakery",
                                                 "Reset panel layout",
                                                 "Start / resume simulation",
                                                 "Pause simulation",
                                                 "Start scene rendering",
                                                 "Stop scene rendering"};

static bool8_t editor_command_enabled(EditorCommand command,
                                      const VkrSampleUiFrame *frame) {
  switch (command) {
  case CMD_LOAD:
    return frame->scene == NULL;
  case CMD_RELOAD:
  case CMD_UNLOAD:
  case CMD_SAVE:
    return frame->scene != NULL;
  case CMD_UNDO:
    return frame->scene && frame->edits->undo_cursor > 0;
  case CMD_REDO:
    return frame->scene && frame->edits->undo_cursor < frame->edits->undo_count;
  case CMD_FRAME:
    return frame->scene &&
           vkr_scene_entity_alive(frame->scene, frame->selected_entity);
  case CMD_SIM_START:
    return !frame->simulation_running;
  case CMD_SIM_PAUSE:
    return frame->simulation_running;
  case CMD_RENDER_START:
    return frame->scene_rendering_stopped;
  case CMD_RENDER_STOP:
    return !frame->scene_rendering_stopped;
  default:
    return true_v;
  }
}

static void editor_command_execute(EditorCommand command,
                                   const VkrSampleUiFrame *frame) {
  const VkrSceneEditAction actions[] = {
      VKR_SCENE_EDIT_LOAD, VKR_SCENE_EDIT_RELOAD, VKR_SCENE_EDIT_UNLOAD,
      VKR_SCENE_EDIT_SAVE, VKR_SCENE_EDIT_UNDO,   VKR_SCENE_EDIT_REDO,
      VKR_SCENE_EDIT_FRAME};
  if (command <= CMD_FRAME) {
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = actions[command], .entity = frame->selected_entity};
    return;
  }
  switch (command) {
  case CMD_HIERARCHY:
    vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_HIERARCHY);
    break;
  case CMD_INSPECTOR:
    vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_INSPECTOR);
    break;
  case CMD_CONSOLE:
    vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_CONSOLE);
    break;
  case CMD_BAKERY:
    vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_BAKERY);
    break;
  case CMD_RESET_LAYOUT:
    vkr_ui_dock_default_editor_layout(frame->dock);
    break;
  case CMD_SIM_START:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_SIMULATION;
    break;
  case CMD_SIM_PAUSE:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION;
    break;
  case CMD_RENDER_START:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_RENDERING;
    break;
  case CMD_RENDER_STOP:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_STOP_RENDERING;
    break;
  default:
    break;
  }
}

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  if (editor->menu != VKR_EDITOR_MENU_NONE) {
    VkrUiSystem *ui = frame->ui;
    const bool8_t outside =
        ui->mouse_pressed &&
        (float32_t)ui->mouse_y >=
            VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale &&
        !editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                              editor_menu_popup_rect(editor, ui));
    if (outside || input_key_just_pressed(frame->input, KEY_ESCAPE)) {
      editor->menu = VKR_EDITOR_MENU_NONE;
      ui->focused_id = ui->active_id = VKR_UI_ID_NONE;
      (void)vkr_ui_keyboard_layer_set(ui, 0u);
    }
  }
  const bool8_t modifier = input_key_shortcut_modifier(frame->input, KEY_P);
  if (!frame->mouse_captured && modifier &&
      input_key_just_pressed(frame->input, KEY_P) && !editor->commands_open) {
    editor->commands_open = true_v;
    editor->commands_focus_search = true_v;
    editor->commands_cursor = 0;
    editor->commands_first_row = 0;
    editor->commands_repeat_direction = 0;
    editor->commands_query_length = 0;
    editor->commands_query[0] = 0;
    editor->menu = VKR_EDITOR_MENU_NONE;
  }
}

static bool8_t editor_command_matches(const char *name, const uint8_t *query,
                                      uint32_t length) {
  const size_t name_length = strlen(name);
  if (!length)
    return true_v;
  if (length > name_length)
    return false_v;
  for (size_t i = 0; i + length <= name_length; ++i) {
    uint32_t j = 0;
    for (; j < length; ++j) {
      uint8_t a = (uint8_t)name[i + j], b = query[j];
      if (a >= 'A' && a <= 'Z')
        a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z')
        b += 'a' - 'A';
      if (a != b)
        break;
    }
    if (j == length)
      return true_v;
  }
  return false_v;
}

static int32_t editor_commands_direction(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         bool8_t opening) {
  VkrUiSystem *ui = frame->ui;
  int32_t direction = 0;
  const int32_t held = (int32_t)input_is_key_down(frame->input, KEY_DOWN) -
                       (int32_t)input_is_key_down(frame->input, KEY_UP);
  if (!opening && !ui->mouse_captured) {
    direction = (int32_t)input_key_just_pressed(frame->input, KEY_DOWN) -
                (int32_t)input_key_just_pressed(frame->input, KEY_UP);
    if (direction || held != editor->commands_repeat_direction) {
      editor->commands_repeat_remaining = 0.35;
    } else if (held) {
      editor->commands_repeat_remaining -= ui->delta_time;
      if (editor->commands_repeat_remaining <= 0) {
        direction = held;
        editor->commands_repeat_remaining = 0.055;
      }
    }
  }
  editor->commands_repeat_direction = held;
  return direction;
}

void vkr_editor_commands_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (!editor->commands_open)
    return;
  VkrUiSystem *ui = frame->ui;
  (void)vkr_ui_keyboard_layer_set(ui, EDITOR_COMMAND_LAYER);
  (void)vkr_ui_input_layer_set(ui, EDITOR_COMMAND_LAYER);
  (void)vkr_ui_input_layer_register(ui, EDITOR_COMMAND_LAYER,
                                    (VkrUiRect){0, 0,
                                                (float32_t)ui->target_width,
                                                (float32_t)ui->target_height});
  ui->capture.keyboard = true_v;
  ui->capture.mouse = true_v;
  const float32_t screen_w = ui->target_width / ui->content_scale,
                  screen_h = ui->target_height / ui->content_scale;
  const float32_t width = Min(500.0f, Max(1.0f, screen_w - 24.0f));
  const float32_t height = Min(516.0f, Max(100.0f, screen_h - 72.0f));
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0,
      .row = 0,
      .column_span = 1,
      .row_span = 1,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {48, 0, 0, Max(0.0f, (screen_w - width) * 0.5f)}};
  panel.style = vkr_editor_glass_style();
  panel.style.gap_pt = 3;
  panel.style.background_color = (Vec4){0.085f, 0.10f, 0.12f, 1};
  panel.style.min_size_pt = (Vec2){width, height};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  const VkrUiTrack rows[3] = {{.value = 28, .unit = VKR_UI_TRACK_PX},
                              {.value = 28, .unit = VKR_UI_TRACK_PX},
                              {.value = 1, .unit = VKR_UI_TRACK_FR}};
  panel.rows = rows;
  panel.row_count = 3;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.commands"), &panel))
    return;
  VkrUiWidgetConfig title =
      vkr_editor_text_config(12, (Vec4){0.91f, 0.79f, 0.58f, 1});
  title.placement.column = 0;
  title.placement.row = 0;
  title.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("title"),
               string8_lit("Commands  |  Up/Down select, Enter run, Esc close"),
               &title);
  VkrUiWidgetConfig search = vkr_ui_widget_config_default();
  search.placement.column = 0;
  search.placement.row = 1;
  search.style.font_size_pt = 12;
  search.style.padding_pt = (VkrUiEdges){4, 6, 4, 6};
  search.style.background_color = (Vec4){0.035f, 0.045f, 0.055f, 1};

  const VkrUiId search_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("search"));
  const bool8_t opening = editor->commands_focus_search;
  if (editor->commands_focus_search) {
    ui->focused_id = search_id;
    ui->active_id = 0;
    ui->focused_is_text = false_v;
    ui->focus_claimed = true_v;
  }
  VkrUiTextEditBuffer query = {editor->commands_query,
                               editor->commands_query_length,
                               sizeof(editor->commands_query)};
  if (vkr_ui_text_field(ui, string8_lit("search"), &query, &search)) {
    editor->commands_query_length = query.length;
    editor->commands_cursor = 0;
    editor->commands_first_row = 0;
    editor->commands_repeat_direction = 0;
  }
  // A newly created field has no retained rectangle until its first layout.
  // Keep the focus request until the text field accepts keyboard input.
  if (opening && ui->focused_id == search_id && ui->focused_is_text)
    editor->commands_focus_search = false_v;
  EditorCommand matches[CMD_COUNT];
  uint32_t count = 0;
  for (uint32_t i = 0; i < CMD_COUNT; ++i)
    if (editor_command_matches(s_command_names[i], editor->commands_query,
                               editor->commands_query_length))
      matches[count++] = (EditorCommand)i;
  editor->commands_cursor =
      count ? Min(editor->commands_cursor, count - 1u) : 0;
  const int32_t direction = editor_commands_direction(editor, frame, opening);
  if (count && direction) {
    editor->commands_cursor =
        (uint32_t)Max(0, Min((int32_t)count - 1,
                             (int32_t)editor->commands_cursor + direction));
    ui->focused_id = search_id;
  }
  const uint32_t visible =
      Min((uint32_t)CMD_COUNT, (uint32_t)Max(1.0f, (height - 86.0f) / 26.0f));
  const uint32_t max_first = count > visible ? count - visible : 0u;
  if (direction) {
    if (editor->commands_cursor < editor->commands_first_row)
      editor->commands_first_row = editor->commands_cursor;
    else if (editor->commands_cursor >= editor->commands_first_row + visible)
      editor->commands_first_row = editor->commands_cursor - visible + 1u;
  }
  const VkrUiRect results_rect = {
      (screen_w - width) * 0.5f * ui->content_scale, 116.0f * ui->content_scale,
      width * ui->content_scale, Max(0.0f, height - 68.0f) * ui->content_scale};
  if (ui->mouse_input_layer == EDITOR_COMMAND_LAYER && ui->mouse_wheel &&
      editor_point_in_rect(ui->mouse_x, ui->mouse_y, results_rect))
    editor->commands_first_row = (uint32_t)Max(
        0, Min((int32_t)max_first,
               (int32_t)editor->commands_first_row - ui->mouse_wheel * 3));
  const uint32_t first = editor->commands_first_row =
      Min(editor->commands_first_row, max_first);
  int32_t mouse_dx = 0, mouse_dy = 0;
  input_get_mouse_delta(frame->input, &mouse_dx, &mouse_dy);
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement.column = 0;
  list.placement.row = 2;
  list.clip_children = true_v;
  VkrUiTrack item_rows[CMD_COUNT];
  for (uint32_t i = 0; i < visible; ++i)
    item_rows[i] = (VkrUiTrack){.value = 26, .unit = VKR_UI_TRACK_PX};
  list.rows = item_rows;
  list.row_count = visible;
  EditorCommand execute = CMD_COUNT;
  if (vkr_ui_panel_begin(ui, string8_lit("results"), &list)) {
    for (uint32_t i = first; i < Min(count, first + visible); ++i) {
      const EditorCommand command = matches[i];
      VkrUiWidgetConfig item = vkr_ui_widget_config_default();
      item.placement.column = 0;
      item.placement.row = i - first;
      item.style.font_size_pt = 12;
      item.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
      item.style.background_color = i == editor->commands_cursor
                                        ? (Vec4){0.22f, 0.29f, 0.35f, 1}
                                        : (Vec4){0.13f, 0.17f, 0.21f, 1};
      item.disabled = !editor_command_enabled(command, frame);
      item.tooltip =
          item.disabled
              ? string8_lit(
                    "Unavailable in the current scene or transport state")
              : (String8){0};
      (void)vkr_ui_push_id_u64(ui, command);
      const VkrUiId item_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("run"));
      if (vkr_ui_button(ui, string8_lit("run"),
                        string8_create((uint8_t *)s_command_names[command],
                                       strlen(s_command_names[command])),
                        &item))
        execute = command;
      if (ui->focused_id == item_id ||
          (ui->hot_id == item_id &&
           (mouse_dx || mouse_dy || ui->mouse_pressed)))
        editor->commands_cursor = i;
      (void)vkr_ui_pop_id(ui);
    }
    if (!count) {
      VkrUiWidgetConfig empty =
          vkr_editor_text_config(12, (Vec4){0.75f, 0.78f, 0.82f, 1});
      vkr_ui_label(ui, string8_lit("empty"),
                   string8_lit("No matching commands"), &empty);
    }
    (void)vkr_ui_panel_end(ui);
  }
  if (count && !opening && !ui->mouse_captured && ui->focused_id == search_id &&
      input_key_just_pressed(frame->input, KEY_ENTER) &&
      editor_command_enabled(matches[editor->commands_cursor], frame))
    execute = matches[editor->commands_cursor];
  if (execute != CMD_COUNT)
    editor_command_execute(execute, frame);
  if (execute != CMD_COUNT ||
      input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->commands_open = false_v;
    ui->focused_id = 0;
    ui->active_id = 0;
  }
  (void)vkr_ui_panel_end(ui);
}
