#include "editor_graphics.h"

#include "editor_internal.h"

static VkrUiWidgetConfig graphics_widget(float32_t x, float32_t y,
                                         float32_t height) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {.top = y, .left = x},
  };
  config.style.min_size_pt.y = height;
  config.style.max_size_pt.y = height;
  config.style.padding_pt = (VkrUiEdges){3.0f, 6.0f, 3.0f, 6.0f};
  config.style.corner_radius_pt = (Vec4){3.0f, 3.0f, 3.0f, 3.0f};
  return config;
}

static void graphics_heading(VkrUiSystem *ui, String8 id, String8 text,
                             float32_t y, VkrFontHandle font) {
  VkrUiWidgetConfig config = graphics_widget(0.0f, y, 22.0f);
  config.style.text_color = (Vec4){0.47f, 0.80f, 1.0f, 1.0f};
  config.text.font = font;
  vkr_ui_label(ui, id, text, &config);
}

static bool8_t graphics_checkbox(VkrUiSystem *ui, String8 id, String8 text,
                                 bool8_t *value, float32_t y, bool8_t disabled,
                                 String8 tooltip) {
  VkrUiWidgetConfig config = graphics_widget(0.0f, y, 24.0f);
  config.disabled = disabled;
  config.tooltip = tooltip;
  return vkr_ui_checkbox(ui, id, text, value, &config);
}

static bool8_t graphics_slider(VkrUiSystem *ui, String8 id, String8 text,
                               String8 value_text, float32_t *value,
                               float32_t minimum, float32_t maximum,
                               float32_t y, String8 tooltip) {
  if (!vkr_ui_push_id_label(ui, id))
    return false_v;
  VkrUiWidgetConfig label = graphics_widget(0.0f, y, 20.0f);
  label.style.text_color = (Vec4){0.78f, 0.83f, 0.90f, 1.0f};
  const String8 content = string8_create_formatted(
      ui->frame_allocator, "%.*s  %.*s", (int32_t)text.length, text.str,
      (int32_t)value_text.length, value_text.str);
  vkr_ui_label(ui, string8_lit("label"), content, &label);
  VkrUiWidgetConfig slider = graphics_widget(0.0f, y + 20.0f, 20.0f);
  slider.tooltip = tooltip;
  const bool8_t changed = vkr_ui_slider_f32(ui, string8_lit("value"), value,
                                            minimum, maximum, &slider);
  (void)vkr_ui_pop_id(ui);
  return changed;
}

static bool8_t graphics_quality_button(VkrUiSystem *ui, String8 id,
                                       String8 text, uint32_t value,
                                       uint32_t *selected, float32_t y,
                                       bool8_t disabled) {
  VkrUiWidgetConfig config = graphics_widget(0.0f, y, 25.0f);
  config.disabled = disabled;
  config.style.background_color = *selected == value
                                      ? (Vec4){0.16f, 0.36f, 0.52f, 1.0f}
                                      : (Vec4){0.09f, 0.12f, 0.17f, 1.0f};
  if (!vkr_ui_button(ui, id, text, &config))
    return false_v;
  *selected = value;
  return true_v;
}

static bool8_t graphics_build_display(VkrEditorUi *editor, VkrUiSystem *ui,
                                      const VkrGraphicsSettingsState *state,
                                      VkrGraphicsSettings *settings) {
  bool8_t changed = false_v;
  graphics_heading(ui, string8_lit("display.heading"), string8_lit("Display"),
                   0.0f, editor->heading_font);
  changed |= graphics_checkbox(
      ui, string8_lit("vsync"), string8_lit("Vertical sync"), &settings->vsync,
      28.0f, false_v,
      string8_lit("Match presentation to the display refresh rate"));
  changed |= graphics_checkbox(
      ui, string8_lit("hdr"), string8_lit("High dynamic range"), &settings->hdr,
      56.0f, false_v,
      string8_lit(
          "Use the display's extended brightness range when available"));
  float32_t frame_limit = (float32_t)settings->frame_limit;
  const String8 frame_limit_text =
      settings->frame_limit
          ? string8_create_formatted(ui->frame_allocator, "%u FPS",
                                     settings->frame_limit)
          : string8_lit("Unlimited");
  const bool8_t frame_limit_changed = graphics_slider(
      ui, string8_lit("frame.limit"), string8_lit("Frame limit"),
      frame_limit_text, &frame_limit, 0.0f, 240.0f, 88.0f,
      string8_lit("0 leaves the frame rate unrestricted"));
  if (frame_limit_changed)
    settings->frame_limit = (uint32_t)(frame_limit + 0.5f);
  changed |= frame_limit_changed;
  const String8 temporal_label = state->temporal_upscaling_name.length
                                     ? state->temporal_upscaling_name
                                     : string8_lit("Temporal upscaling");
  changed |= graphics_checkbox(
      ui, string8_lit("temporal"), temporal_label,
      &settings->temporal_upscaling, 132.0f,
      !state->temporal_upscaling_available,
      string8_lit("Reconstruct a higher-resolution image from temporal data"));
  if (settings->temporal_upscaling)
    settings->anti_aliasing = true_v;
  if (!settings->temporal_upscaling && settings->dynamic_resolution) {
    settings->dynamic_resolution = false_v;
    changed = true_v;
  }
  changed |= graphics_checkbox(
      ui, string8_lit("dynamic.resolution"), string8_lit("Dynamic resolution"),
      &settings->dynamic_resolution, 160.0f,
      !state->dynamic_resolution_available || !settings->temporal_upscaling,
      string8_lit("Adjust internal resolution to keep frame pacing stable"));
  const String8 render_scale_text = string8_create_formatted(
      ui->frame_allocator, "%.0f%%", settings->render_scale * 100.0f);
  changed |= graphics_slider(ui, string8_lit("render.scale"),
                             string8_lit("Render scale"), render_scale_text,
                             &settings->render_scale, 1.0f / 3.0f, 1.0f, 192.0f,
                             string8_lit("Lower values render fewer pixels"));
  return changed;
}

static bool8_t graphics_build_quality(VkrEditorUi *editor, VkrUiSystem *ui,
                                      VkrGraphicsSettings *settings) {
  bool8_t changed = false_v;
  graphics_heading(ui, string8_lit("quality.heading"), string8_lit("Quality"),
                   0.0f, editor->heading_font);
  changed |= graphics_checkbox(
      ui, string8_lit("anti.aliasing"), string8_lit("Anti-aliasing"),
      &settings->anti_aliasing, 28.0f, settings->temporal_upscaling,
      string8_lit("Smooth visible edges"));
  graphics_heading(ui, string8_lit("shadows.heading"), string8_lit("Shadows"),
                   82.0f, editor->heading_font);
  changed |= graphics_quality_button(
      ui, string8_lit("shadows.off"), string8_lit("Off"), 0u,
      &settings->shadow_quality, 110.0f, false_v);
  changed |= graphics_quality_button(
      ui, string8_lit("shadows.balanced"), string8_lit("Balanced"), 1u,
      &settings->shadow_quality, 138.0f, false_v);
  changed |= graphics_quality_button(
      ui, string8_lit("shadows.high"), string8_lit("High"), 2u,
      &settings->shadow_quality, 166.0f, false_v);
  changed |= graphics_checkbox(
      ui, string8_lit("soft.shadows"), string8_lit("Soft shadows"),
      &settings->soft_shadows, 198.0f, settings->shadow_quality == 0u,
      string8_lit("Soften shadow edges"));
  changed |= graphics_checkbox(
      ui, string8_lit("local.shadows"), string8_lit("Local light shadows"),
      &settings->local_shadows, 226.0f, settings->shadow_quality == 0u,
      string8_lit("Enable shadows from nearby lights"));
  return changed;
}

static bool8_t graphics_build_lighting(VkrEditorUi *editor, VkrUiSystem *ui,
                                       VkrGraphicsSettings *settings) {
  bool8_t changed = false_v;
  graphics_heading(ui, string8_lit("lighting.heading"), string8_lit("Lighting"),
                   0.0f, editor->heading_font);
  changed |= graphics_checkbox(
      ui, string8_lit("ambient.occlusion"), string8_lit("Ambient occlusion"),
      &settings->ambient_occlusion, 28.0f, false_v,
      string8_lit("Add contact shading where surfaces meet"));
  changed |= graphics_checkbox(
      ui, string8_lit("screen.gi"),
      string8_lit("Screen-space global illumination"),
      &settings->screen_space_gi, 56.0f, false_v,
      string8_lit("Add indirect light from visible surfaces"));
  changed |= graphics_checkbox(
      ui, string8_lit("screen.reflections"),
      string8_lit("Screen-space reflections"),
      &settings->screen_space_reflections, 84.0f, false_v,
      string8_lit("Reflect visible surroundings on glossy surfaces"));
  changed |= graphics_checkbox(
      ui, string8_lit("reflection.probes"), string8_lit("Reflection probes"),
      &settings->reflection_probes, 112.0f, false_v,
      string8_lit("Use authored environment reflections"));
  changed |= graphics_checkbox(
      ui, string8_lit("subsurface"), string8_lit("Subsurface scattering"),
      &settings->subsurface_scattering, 140.0f, false_v,
      string8_lit("Soften light through skin, wax, and similar surfaces"));
  changed |= graphics_checkbox(ui, string8_lit("fog"), string8_lit("Fog"),
                               &settings->fog, 168.0f, false_v,
                               string8_lit("Add distance and height fog"));
  changed |= graphics_checkbox(
      ui, string8_lit("volumetric.fog"), string8_lit("Volumetric fog"),
      &settings->volumetric_fog, 196.0f, !settings->fog,
      string8_lit("Render light through fog"));
  return changed;
}

static bool8_t graphics_build_effects(VkrEditorUi *editor, VkrUiSystem *ui,
                                      VkrGraphicsSettings *settings) {
  bool8_t changed = false_v;
  graphics_heading(ui, string8_lit("effects.heading"), string8_lit("Effects"),
                   0.0f, editor->heading_font);
  changed |= graphics_checkbox(ui, string8_lit("bloom"), string8_lit("Bloom"),
                               &settings->bloom, 28.0f, false_v,
                               string8_lit("Glow around bright image details"));
  const String8 bloom_intensity_text = string8_create_formatted(
      ui->frame_allocator, "%.0f%%", settings->bloom_intensity * 100.0f);
  changed |= graphics_slider(
      ui, string8_lit("bloom.intensity"), string8_lit("Bloom intensity"),
      bloom_intensity_text, &settings->bloom_intensity, 0.0f, 1.0f, 56.0f,
      string8_lit("Set the strength of bloom"));
  changed |= graphics_checkbox(
      ui, string8_lit("depth.of.field"), string8_lit("Depth of field"),
      &settings->depth_of_field, 100.0f, false_v,
      string8_lit("Blur distant and near image regions"));
  changed |=
      graphics_checkbox(ui, string8_lit("motion.blur"),
                        string8_lit("Motion blur"), &settings->motion_blur,
                        128.0f, false_v, string8_lit("Blur fast movement"));
  const String8 motion_blur_amount_text = string8_create_formatted(
      ui->frame_allocator, "%.0f%%", settings->motion_blur_amount * 100.0f);
  changed |= graphics_slider(
      ui, string8_lit("motion.blur.amount"), string8_lit("Motion blur amount"),
      motion_blur_amount_text, &settings->motion_blur_amount, 0.0f, 1.0f,
      156.0f, string8_lit("Set the amount of motion blur"));
  return changed;
}

static bool8_t graphics_build_color(VkrEditorUi *editor, VkrUiSystem *ui,
                                    VkrGraphicsSettings *settings) {
  bool8_t changed = false_v;
  graphics_heading(ui, string8_lit("color.heading"), string8_lit("Color"), 0.0f,
                   editor->heading_font);
  const String8 brightness_text = string8_create_formatted(
      ui->frame_allocator, "%+.1f EV", settings->brightness);
  changed |=
      graphics_slider(ui, string8_lit("brightness"), string8_lit("Brightness"),
                      brightness_text, &settings->brightness, -3.0f, 3.0f,
                      28.0f, string8_lit("Adjust image brightness"));
  const String8 contrast_text = string8_create_formatted(
      ui->frame_allocator, "%.2fx", settings->contrast);
  changed |= graphics_slider(
      ui, string8_lit("contrast"), string8_lit("Contrast"), contrast_text,
      &settings->contrast, 0.5f, 1.5f, 72.0f,
      string8_lit("Adjust the difference between light and dark"));
  const String8 saturation_text = string8_create_formatted(
      ui->frame_allocator, "%.2fx", settings->saturation);
  changed |=
      graphics_slider(ui, string8_lit("saturation"), string8_lit("Saturation"),
                      saturation_text, &settings->saturation, 0.0f, 2.0f,
                      116.0f, string8_lit("Adjust color intensity"));
  const String8 temperature_text = string8_create_formatted(
      ui->frame_allocator, "%+.2f", settings->temperature);
  changed |= graphics_slider(
      ui, string8_lit("temperature"), string8_lit("Temperature"),
      temperature_text, &settings->temperature, -1.0f, 1.0f, 160.0f,
      string8_lit("Shift color toward cool or warm tones"));
  const String8 tint_text =
      string8_create_formatted(ui->frame_allocator, "%+.2f", settings->tint);
  changed |= graphics_slider(
      ui, string8_lit("tint"), string8_lit("Tint"), tint_text, &settings->tint,
      -1.0f, 1.0f, 204.0f, string8_lit("Shift color toward green or magenta"));
  const String8 sharpness_text = string8_create_formatted(
      ui->frame_allocator, "%.0f%%", settings->sharpness * 100.0f);
  changed |=
      graphics_slider(ui, string8_lit("sharpness"), string8_lit("Sharpness"),
                      sharpness_text, &settings->sharpness, 0.0f, 1.0f, 248.0f,
                      string8_lit("Increase image edge definition"));
  return changed;
}

static float32_t graphics_content_height(VkrEditorGraphicsTab tab) {
  switch (tab) {
  case VKR_EDITOR_GRAPHICS_TAB_DISPLAY:
    return 300.0f;
  case VKR_EDITOR_GRAPHICS_TAB_QUALITY:
    return 310.0f;
  case VKR_EDITOR_GRAPHICS_TAB_LIGHTING:
    return 280.0f;
  case VKR_EDITOR_GRAPHICS_TAB_EFFECTS:
    return 265.0f;
  case VKR_EDITOR_GRAPHICS_TAB_COLOR:
    return 360.0f;
  default:
    return 100.0f;
  }
}

void vkr_editor_graphics_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->graphics || !frame->graphics_request) {
    VkrUiWidgetConfig message = graphics_widget(12.0f, 12.0f, 48.0f);
    vkr_ui_label(ui, string8_lit("graphics.unavailable"),
                 string8_lit("Graphics settings are unavailable."), &message);
    return;
  }

  const VkrUiTrack columns[] = {
      {.value = 118.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
  };
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig content = vkr_ui_panel_config_default();
  content.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 1u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  content.columns = columns;
  content.column_count = ArrayCount(columns);
  content.rows = &one_track;
  content.row_count = 1u;
  content.style.padding_pt = (VkrUiEdges){0};
  if (!vkr_ui_panel_begin(ui, string8_lit("graphics.content"), &content))
    return;

  VkrUiPanelConfig tabs = vkr_ui_panel_config_default();
  tabs.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  tabs.columns = &one_track;
  tabs.column_count = 1u;
  tabs.rows = &one_track;
  tabs.row_count = 1u;
  tabs.style.padding_pt = (VkrUiEdges){8.0f, 7.0f, 8.0f, 7.0f};
  tabs.style.border_pt = (VkrUiEdges){0.0f, 1.0f, 0.0f, 0.0f};
  tabs.style.border_color = (Vec4){0.22f, 0.31f, 0.42f, 0.7f};
  if (vkr_ui_panel_begin(ui, string8_lit("tabs"), &tabs)) {
    const String8 tab_names[VKR_EDITOR_GRAPHICS_TAB_COUNT] = {
        [VKR_EDITOR_GRAPHICS_TAB_DISPLAY] = string8_lit("Display"),
        [VKR_EDITOR_GRAPHICS_TAB_QUALITY] = string8_lit("Quality"),
        [VKR_EDITOR_GRAPHICS_TAB_LIGHTING] = string8_lit("Lighting"),
        [VKR_EDITOR_GRAPHICS_TAB_EFFECTS] = string8_lit("Effects"),
        [VKR_EDITOR_GRAPHICS_TAB_COLOR] = string8_lit("Color"),
    };
    for (uint32_t i = 0u; i < VKR_EDITOR_GRAPHICS_TAB_COUNT; ++i) {
      VkrUiWidgetConfig tab = graphics_widget(0.0f, i * 31.0f, 27.0f);
      tab.style.text_color = (Vec4){0.78f, 0.83f, 0.90f, 1.0f};
      tab.style.background_color = editor->graphics_tab == i
                                       ? (Vec4){0.14f, 0.33f, 0.49f, 1.0f}
                                       : (Vec4){0.07f, 0.09f, 0.13f, 0.2f};
      if (vkr_ui_button(ui, tab_names[i], tab_names[i], &tab))
        editor->graphics_tab = (VkrEditorGraphicsTab)i;
    }
    (void)vkr_ui_panel_end(ui);
  }

  const VkrUiTrack content_track = {
      .value = graphics_content_height(editor->graphics_tab),
      .unit = VKR_UI_TRACK_PX,
  };
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  scroll.placement = (VkrUiPlacement){
      .column = 1u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  scroll.columns = &one_track;
  scroll.column_count = 1u;
  scroll.rows = &content_track;
  scroll.row_count = 1u;
  const bool8_t show_notice =
      frame->graphics->restart_required || frame->graphics->message.length;
  scroll.style.padding_pt = show_notice
                                ? (VkrUiEdges){36.0f, 12.0f, 10.0f, 12.0f}
                                : (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  scroll.clip_children = true_v;
  (void)vkr_ui_push_id_u64(ui, editor->graphics_tab);
  if (vkr_ui_scroll_area_begin(ui, string8_lit("graphics.scroll"), &scroll)) {
    VkrGraphicsSettings settings = frame->graphics->settings;
    bool8_t changed = false_v;
    switch (editor->graphics_tab) {
    case VKR_EDITOR_GRAPHICS_TAB_DISPLAY:
      changed = graphics_build_display(editor, ui, frame->graphics, &settings);
      break;
    case VKR_EDITOR_GRAPHICS_TAB_QUALITY:
      changed = graphics_build_quality(editor, ui, &settings);
      break;
    case VKR_EDITOR_GRAPHICS_TAB_LIGHTING:
      changed = graphics_build_lighting(editor, ui, &settings);
      break;
    case VKR_EDITOR_GRAPHICS_TAB_EFFECTS:
      changed = graphics_build_effects(editor, ui, &settings);
      break;
    case VKR_EDITOR_GRAPHICS_TAB_COLOR:
      changed = graphics_build_color(editor, ui, &settings);
      break;
    default:
      break;
    }
    VkrUiWidgetConfig reset = graphics_widget(
        0.0f, graphics_content_height(editor->graphics_tab) - 36.0f, 25.0f);
    reset.style.background_color = (Vec4){0.16f, 0.22f, 0.29f, 1.0f};
    if (vkr_ui_button(ui, string8_lit("restore.defaults"),
                      string8_lit("Restore defaults"), &reset)) {
      *frame->graphics_request =
          (VkrGraphicsSettingsRequest){.reset_defaults = true_v};
    } else if (changed) {
      *frame->graphics_request =
          (VkrGraphicsSettingsRequest){.settings = settings, .apply = true_v};
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_pop_id(ui);

  if (show_notice) {
    VkrUiWidgetConfig notice = graphics_widget(0.0f, 5.0f, 24.0f);
    notice.placement.column = 1u;
    notice.placement.row = 0u;
    notice.style.background_color = (Vec4){0.25f, 0.18f, 0.07f, 0.92f};
    notice.style.text_color = (Vec4){1.0f, 0.82f, 0.50f, 1.0f};
    const String8 text =
        frame->graphics->message.length
            ? frame->graphics->message
            : string8_lit("Restart required to apply display settings.");
    vkr_ui_label(ui, string8_lit("restart.notice"), text, &notice);
  }
  (void)vkr_ui_panel_end(ui);
}
