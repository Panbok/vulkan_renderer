#include "editor_graphics.h"

#include "editor_details.h"
#include "editor_internal.h"

/* Preferences: machine-local graphics gates drawn from their type descriptor
 * (ADR-076). Changes apply through the runtime's graphics request; the
 * descriptor's state hook disables rows the platform cannot honor. */

static void graphics_notice(VkrUiSystem *ui, const VkrSampleUiFrame *frame,
                            float32_t width) {
  VkrUiWidgetConfig notice = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, 6.0f, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      24.0f);
  notice.style.background_color =
      vkr_ui_color_alpha(vkr_ui_theme()->warning, 0.16f);
  notice.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  notice.style.border_color = vkr_ui_color_alpha(vkr_ui_theme()->warning, 0.5f);
  notice.style.text_color = vkr_ui_theme()->text;
  notice.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
  notice.icon = VKR_UI_ICON_WARNING_FILL;
  notice.icon_size_pt = 13.0f;
  notice.icon_color = vkr_ui_theme()->warning;
  const String8 text =
      frame->graphics->message.length
          ? frame->graphics->message
          : string8_lit("Restart required to apply display settings.");
  vkr_ui_label(ui, string8_lit("restart.notice"), text, &notice);
}

void vkr_editor_graphics_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame, VkrUiRect rect) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->graphics || !frame->graphics_request) {
    VkrUiWidgetConfig message =
        vkr_editor_details_widget(12.0f, 12.0f, 240.0f, 48.0f);
    vkr_ui_label(ui, string8_lit("graphics.unavailable"),
                 string8_lit("Preferences are unavailable."), &message);
    return;
  }
  const float32_t width = rect.width / Max(ui->content_scale, 0.001f);
  const float32_t height = rect.height / Max(ui->content_scale, 0.001f);
  if (width < 32.0f || height < 24.0f) {
    return;
  }
  const bool8_t show_notice =
      frame->graphics->restart_required || frame->graphics->message.length;
  const float32_t top = show_notice ? 36.0f : 0.0f;
  if (show_notice) {
    graphics_notice(ui, frame, width);
  }

  if (ui->mouse_input_layer == ui->input_layer && !ui->mouse_captured &&
      ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
      ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height) {
    editor->preferences_scroll -= ui->mouse_wheel * 40.0f;
  }
  const float32_t visible = Max(1.0f, height - top);
  const float32_t content = Max(visible, editor->preferences_height);
  const float32_t scroll_limit = Max(0.0f, content - visible);
  const float32_t scroll = editor->preferences_scroll;
  editor->preferences_scroll = Clamp(scroll, 0.0f, scroll_limit);

  const VkrUiTrack content_track = {.value = content, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig area = vkr_ui_panel_config_default();
  /* The notice occupies the window's single cell; the rows share it below
     the notice's padding instead of looking for a free cell. */
  area.placement.column = 0u;
  area.placement.row = 0u;
  area.rows = &content_track;
  area.row_count = 1u;
  area.clip_children = true_v;
  area.style.padding_pt = (VkrUiEdges){top, 0.0f, 0.0f, 0.0f};
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("preferences.scroll"), &area)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &editor->preferences_scroll);

  VkrGraphicsSettings settings = frame->graphics->settings;
  VkrEditorDetails *details = &editor->preferences_details;
  float32_t y = 4.0f;
  vkr_editor_details_begin(details);
  vkr_editor_details_error(details, ui, width, &y);
  const VkrEditorDetailsResult result = vkr_editor_details_type(
      details, ui, frame->input, width, &y, &vkr_graphics_settings_type,
      &settings, frame->graphics, false_v);
  vkr_editor_details_end(details);

  y += 10.0f;
  VkrUiWidgetConfig reset = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, y, Min(180.0f, width - 20.0f), 26.0f);
  vkr_editor_action_style(&reset, editor->heading_font);
  reset.icon = VKR_UI_ICON_RESET;
  reset.icon_size_pt = 13.0f;
  if (vkr_ui_button(ui, string8_lit("restore.defaults"),
                    string8_lit("Restore defaults"), &reset)) {
    vkr_editor_details_cancel(details, ui);
    *frame->graphics_request =
        (VkrGraphicsSettingsRequest){.reset_defaults = true_v};
  } else if (result.changed) {
    *frame->graphics_request =
        (VkrGraphicsSettingsRequest){.settings = settings, .apply = true_v};
  }
  editor->preferences_height = y + 40.0f;
  (void)vkr_ui_scroll_area_end(ui);
}
