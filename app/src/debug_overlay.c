#include "debug_overlay.h"

static VkrUiStyle debug_overlay_panel_style(void) {
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

static VkrUiWidgetConfig debug_overlay_text(float32_t size_pt, Vec4 color,
                                            uint32_t row) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = (VkrUiPlacement){
      .column = 0u,
      .row = row,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
  };
  config.style.font_size_pt = size_pt;
  config.style.text_color = color;
  config.text.font_size = size_pt;
  return config;
}

static void debug_overlay_build_help(VkrUiSystem *ui) {
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {8.0f, 0.0f, 0.0f, 8.0f},
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = debug_overlay_panel_style();
  panel.style.min_size_pt = (Vec2){360.0f, 116.0f};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("debug.help"), &panel))
    return;

  VkrUiWidgetConfig title =
      debug_overlay_text(9.0f, (Vec4){0.43f, 0.80f, 1.0f, 1.0f}, 0u);
  vkr_ui_label(ui, string8_lit("title"), string8_lit("DEBUG / CONTROLS"),
               &title);
  VkrUiWidgetConfig body =
      debug_overlay_text(11.0f, (Vec4){0.86f, 0.89f, 0.94f, 1.0f}, 1u);
  vkr_ui_label(ui, string8_lit("body"),
               string8_lit("F6  Toggle debug UI       Tab  Toggle free camera\n"
                           "F4 / F5  Texture filter  F7   GPU pass timings\n"
                           "F8  Cycle IBL mode       F9 / F10  IBL intensity\n"
                           "G   Camera snapshot"),
               &body);
  (void)vkr_ui_panel_end(ui);
}

static void debug_overlay_build_camera(VkrUiSystem *ui, String8 camera_text,
                                       String8 performance_text) {
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_END,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {8.0f, 8.0f, 0.0f, 0.0f},
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = debug_overlay_panel_style();
  panel.style.min_size_pt = (Vec2){288.0f, 98.0f};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("debug.camera.performance"), &panel))
    return;

  VkrUiWidgetConfig title =
      debug_overlay_text(9.0f, (Vec4){0.43f, 0.80f, 1.0f, 1.0f}, 0u);
  vkr_ui_label(ui, string8_lit("title"), string8_lit("CAMERA / FPS"), &title);
  VkrUiWidgetConfig body =
      debug_overlay_text(11.0f, (Vec4){0.86f, 0.89f, 0.94f, 1.0f}, 1u);
  vkr_ui_label(ui, string8_lit("camera"), camera_text, &body);
  body.placement.row = 2u;
  body.style.text_color = (Vec4){0.54f, 0.88f, 0.72f, 1.0f};
  vkr_ui_label(ui, string8_lit("performance"), performance_text, &body);
  (void)vkr_ui_panel_end(ui);
}

void vkr_debug_overlay_build(VkrUiSystem *ui, String8 camera_text,
                             String8 performance_text) {
  debug_overlay_build_help(ui);
  debug_overlay_build_camera(ui, camera_text, performance_text);
}
