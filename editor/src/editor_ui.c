#include "editor_internal.h"

#include "renderer/systems/vkr_editor_viewport.h"

VkrUiStyle vkr_editor_glass_style(void) {
  VkrUiStyle style = vkr_ui_style_default();
  style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  style.corner_radius_pt = (Vec4){7.0f, 7.0f, 7.0f, 7.0f};
  style.gap_pt = 7.0f;
  style.background_color = (Vec4){0.035f, 0.045f, 0.065f, 0.78f};
  style.border_color = (Vec4){0.32f, 0.40f, 0.52f, 0.42f};
  style.text_color = (Vec4){0.88f, 0.91f, 0.96f, 1.0f};
  return style;
}

VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.justify = VKR_UI_ALIGN_START;
  config.placement.align = VKR_UI_ALIGN_START;
  config.style.font_size_pt = size_pt;
  config.style.text_color = color;
  config.text.font_size = size_pt;
  return config;
}

void vkr_editor_field_style(VkrUiWidgetConfig *config) {
  config->style.background_color = config->read_only
                                       ? (Vec4){0.075f, 0.09f, 0.11f, 1.0f}
                                       : (Vec4){0.10f, 0.12f, 0.15f, 1.0f};
  config->style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config->style.border_color = config->read_only
                                   ? (Vec4){0.18f, 0.21f, 0.25f, 1.0f}
                                   : (Vec4){0.28f, 0.33f, 0.39f, 1.0f};
  if (config->read_only)
    config->style.text_color = (Vec4){0.67f, 0.72f, 0.77f, 1.0f};
}

void vkr_editor_action_style(VkrUiWidgetConfig *config, VkrFontHandle heading) {
  config->style.background_color = (Vec4){0.18f, 0.22f, 0.28f, 1.0f};
  config->style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config->style.border_color = (Vec4){0.31f, 0.37f, 0.44f, 1.0f};
  config->text.font = heading;
}

void vkr_editor_ui_init(VkrEditorUi *editor) {
  *editor = (VkrEditorUi){
      .menu = VKR_EDITOR_MENU_NONE,
      .windows =
          {
              [VKR_EDITOR_WINDOW_DRAWS] =
                  {
                      .position_pt = {235.0f, 340.0f},
                      .size_pt = {430.0f, 205.0f},
                      .z_order = 1u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_MEMORY] =
                  {
                      .position_pt = {485.0f, 145.0f},
                      .size_pt = {300.0f, 390.0f},
                      .z_order = 2u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_HELP] =
                  {
                      .position_pt = {280.0f, 105.0f},
                      .size_pt = {390.0f, 145.0f},
                      .z_order = 3u,
                      .visible = false_v,
                  },
          },
  };
}

static void vkr_editor_ui_build_camera(VkrUiSystem *ui, bool8_t scene_only,
                                       bool8_t scene_rendering_stopped,
                                       const VkrViewportMapping *mapping,
                                       const VkrSampleUiText *text) {
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const float32_t width_pt = 288.0f;
  const float32_t height_pt = 98.0f;
  const float32_t inset_px = 8.0f * ui->content_scale;
  const float32_t top_inset_px =
      (scene_only ? 43.0f : 8.0f) * ui->content_scale;
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
              .top =
                  (mapping->panel_rect_px.y + top_inset_px) / ui->content_scale,
              .left = Max(0.0f,
                          (mapping->panel_rect_px.x + mapping->panel_rect_px.z -
                           width_pt * ui->content_scale - inset_px) /
                              ui->content_scale),
          },
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = vkr_editor_glass_style();
  panel.style.min_size_pt = (Vec2){width_pt, height_pt};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.camera.performance"), &panel))
    return;

  VkrUiWidgetConfig title =
      vkr_editor_text_config(9.0f, (Vec4){0.43f, 0.80f, 1.0f, 1.0f});
  title.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
  };
  vkr_ui_label(ui, string8_lit("title"),
               scene_rendering_stopped ? string8_lit("VIEWPORT / FROZEN")
                                       : string8_lit("VIEWPORT / LIVE"),
               &title);
  VkrUiWidgetConfig body =
      vkr_editor_text_config(11.0f, (Vec4){0.86f, 0.89f, 0.94f, 1.0f});
  body.placement = title.placement;
  body.placement.row = 1u;
  vkr_ui_label(ui, string8_lit("camera"), text->camera, &body);
  body.placement.row = 2u;
  body.style.text_color = (Vec4){0.54f, 0.88f, 0.72f, 1.0f};
  vkr_ui_label(ui, string8_lit("performance"), text->performance, &body);
  (void)vkr_ui_panel_end(ui);
}

VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  VkrUiDockInputCapture dock_capture = {0};
  vkr_editor_bakery_update(editor->bakery);
  vkr_editor_commands_update(editor, frame);
  vkr_editor_windows_register_input_layers(editor, frame->ui);
  vkr_editor_scene_toolbar_update(editor, frame);
  if (frame->mapping_valid) {
    if (!frame->scene_only) {
      dock_capture = vkr_ui_dock_update_input(
          frame->dock, frame->input,
          frame->mouse_captured || frame->ui->mouse_input_layer > 0u ||
              (frame->ui->active_id != VKR_UI_ID_NONE &&
               frame->dock->interaction.tab_leaf == VKR_UI_DOCK_NODE_NONE &&
               frame->dock->interaction.resize_split == VKR_UI_DOCK_NODE_NONE));
    }
  }

  (void)vkr_ui_input_layer_set(frame->ui, 0u);
  if (!frame->scene_only)
    vkr_editor_dock_build(editor, frame);
  vkr_editor_windows_build_navigation(editor, frame);
  vkr_editor_scene_toolbar_build(editor, frame);
  if (frame->scene_only && frame->mapping.target_width > 0u)
    vkr_editor_ui_build_camera(frame->ui, frame->scene_only,
                               frame->scene_rendering_stopped, &frame->mapping,
                               &frame->text);
  vkr_editor_windows_build_floating(editor, frame->ui, frame->input,
                                    &frame->text);
  vkr_editor_windows_build_menu(editor, frame->ui);
  vkr_editor_commands_build(editor, frame);
  if (frame->scene_keyboard_focus) {
    VkrUiSystem *ui = frame->ui;
    if (!frame->mapping_valid || frame->scene_rendering_stopped ||
        editor->commands_open ||
        (ui->keyboard_layer_claimed && ui->keyboard_input_layer != 0u))
      *frame->scene_keyboard_focus = false_v;
    if (ui->mouse_pressed && !frame->mouse_captured) {
      int32_t x = 0, y = 0;
      input_get_button_press_position(frame->input, BUTTON_LEFT, &x, &y);
      Vec4 rect = frame->mapping.panel_rect_px;
      if (frame->scene_only) {
        const float32_t top =
            VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale;
        rect.y += top;
        rect.w = Max(0.0f, rect.w - top);
      }
      const bool8_t scene_click =
          frame->mapping_valid && !frame->scene_rendering_stopped &&
          (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.z &&
          (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.w &&
          !ui->capture.mouse && !dock_capture.mouse &&
          ui->mouse_input_layer == 0u && !editor->commands_open;
      *frame->scene_keyboard_focus = scene_click;
      if (scene_click) {
        ui->focused_id = VKR_UI_ID_NONE;
        ui->focused_is_text = false_v;
        (void)vkr_ui_keyboard_layer_set(ui, 0u);
      }
    }
    vkr_ui_keyboard_navigation_enabled(ui, !*frame->scene_keyboard_focus);
  }
  return dock_capture;
}
