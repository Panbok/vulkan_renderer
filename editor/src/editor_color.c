#include "editor_internal.h"
#include "vkr_color_transfer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The color picker popup: a saturation-value square, a hue bar, an alpha bar
 * for RGBA properties, the old and new colors with a hex field, a palette
 * and the session's recent colors. It edits the display-encoded color and
 * writes the property's stored units back through its Details swatch. */

#define COLOR_PAD_PT 10.0f
#define COLOR_WIDTH_PT 236.0f
#define COLOR_SQUARE_PT 150.0f
#define COLOR_BAR_PT 12.0f
#define COLOR_GAP_PT 8.0f
#define COLOR_ROW_PT 24.0f
#define COLOR_CELL_PT 18.0f
#define COLOR_PALETTE_COLUMNS 8u
/* Gradient slices of the square; endpoints are exact display colors, so
   linear interpolation between them stays close to the display ramp. */
#define COLOR_SLICES 8u

enum {
  COLOR_DRAG_NONE = 0,
  COLOR_DRAG_SQUARE,
  COLOR_DRAG_HUE,
  COLOR_DRAG_ALPHA,
};

/* Display sRGB presets: grays, then hues around the wheel. */
static const Vec4 s_palette[] = {
    {1.00f, 1.00f, 1.00f, 1.0f}, {0.75f, 0.75f, 0.75f, 1.0f},
    {0.50f, 0.50f, 0.50f, 1.0f}, {0.25f, 0.25f, 0.25f, 1.0f},
    {0.00f, 0.00f, 0.00f, 1.0f}, {0.55f, 0.35f, 0.20f, 1.0f},
    {0.95f, 0.40f, 0.70f, 1.0f}, {0.65f, 0.30f, 0.85f, 1.0f},
    {0.90f, 0.20f, 0.20f, 1.0f}, {0.95f, 0.55f, 0.15f, 1.0f},
    {0.98f, 0.85f, 0.20f, 1.0f}, {0.30f, 0.80f, 0.30f, 1.0f},
    {0.15f, 0.70f, 0.65f, 1.0f}, {0.20f, 0.75f, 0.95f, 1.0f},
    {0.20f, 0.40f, 0.90f, 1.0f}, {0.40f, 0.30f, 0.85f, 1.0f},
};

static Vec4 color_from_hsv(float32_t hue, float32_t saturation, float32_t value,
                           float32_t alpha) {
  const float32_t h = (hue >= 1.0f ? 0.0f : hue) * 6.0f;
  const uint32_t sector = (uint32_t)h;
  const float32_t f = h - (float32_t)sector;
  const float32_t p = value * (1.0f - saturation);
  const float32_t q = value * (1.0f - saturation * f);
  const float32_t t = value * (1.0f - saturation * (1.0f - f));
  switch (sector) {
  case 0:
    return (Vec4){value, t, p, alpha};
  case 1:
    return (Vec4){q, value, p, alpha};
  case 2:
    return (Vec4){p, value, t, alpha};
  case 3:
    return (Vec4){p, q, value, alpha};
  case 4:
    return (Vec4){t, p, value, alpha};
  default:
    return (Vec4){value, p, q, alpha};
  }
}

/* Hue and saturation stay as they are when the color has none. */
static void color_to_hsv(VkrEditorColorPicker *picker, Vec4 display) {
  const float32_t peak = Max(display.x, Max(display.y, display.z));
  const float32_t low = Min(display.x, Min(display.y, display.z));
  const float32_t range = peak - low;
  picker->value = peak;
  picker->alpha = display.w;
  if (peak <= 0.0f) {
    return;
  }
  picker->saturation = range / peak;
  if (range <= 0.0f) {
    return;
  }
  float32_t hue = 0.0f;
  if (peak == display.x) {
    hue = (display.y - display.z) / range;
  } else if (peak == display.y) {
    hue = 2.0f + (display.z - display.x) / range;
  } else {
    hue = 4.0f + (display.x - display.y) / range;
  }
  hue /= 6.0f;
  picker->hue = hue < 0.0f ? hue + 1.0f : hue;
}

static bool8_t color_srgb(const VkrEditorColorPicker *picker) {
  return (picker->property->flags & VKR_PROPERTY_FLAG_SRGB) != 0u;
}

static bool8_t color_has_alpha(const VkrEditorColorPicker *picker) {
  return vkr_property_components(picker->property) == 4u;
}

/* The display color of a stored value; a linear color past 1 sets the
   intensity the picker keeps. */
static Vec4 color_display(VkrEditorColorPicker *picker, Vec4 stored) {
  Vec4 display = {0.0f, 0.0f, 0.0f, color_has_alpha(picker) ? stored.w : 1.0f};
  picker->intensity = 1.0f;
  if (!color_srgb(picker)) {
    const float32_t peak = Max(stored.x, Max(stored.y, stored.z));
    picker->intensity = peak > 1.0f ? peak : 1.0f;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const float32_t c =
        Clamp(stored.elements[i] / picker->intensity, 0.0f, 1.0f);
    display.elements[i] = color_srgb(picker) ? c : vkr_linear_to_srgb(c);
  }
  return display;
}

static Vec4 color_stored(const VkrEditorColorPicker *picker, Vec4 display) {
  Vec4 stored = {0.0f, 0.0f, 0.0f, display.w};
  for (uint32_t i = 0; i < 3; ++i) {
    stored.elements[i] =
        color_srgb(picker)
            ? display.elements[i]
            : vkr_srgb_to_linear(display.elements[i]) * picker->intensity;
  }
  return stored;
}

static Vec4 color_current(const VkrEditorColorPicker *picker) {
  return color_from_hsv(picker->hue, picker->saturation, picker->value,
                        picker->alpha);
}

static void color_format_hex(VkrEditorColorPicker *picker) {
  const Vec4 display = color_current(picker);
  uint32_t bytes[4];
  for (uint32_t i = 0; i < 4; ++i) {
    bytes[i] = (uint32_t)lroundf(Clamp(display.elements[i], 0.0f, 1.0f) * 255);
  }
  if (color_has_alpha(picker)) {
    snprintf(picker->hex, sizeof(picker->hex), "#%02X%02X%02X%02X", bytes[0],
             bytes[1], bytes[2], bytes[3]);
  } else {
    snprintf(picker->hex, sizeof(picker->hex), "#%02X%02X%02X", bytes[0],
             bytes[1], bytes[2]);
  }
}

/* "#RRGGBB" or "#RRGGBBAA", the hash optional; alpha keeps its value when
   omitted. */
static bool8_t color_parse_hex(const char *text, Vec4 *display) {
  if (text[0] == '#') {
    ++text;
  }
  const uint64_t length = strlen(text);
  if (length != 6u && length != 8u) {
    return false_v;
  }
  for (uint64_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < length / 2u; ++i) {
    char pair[3] = {text[i * 2u], text[i * 2u + 1u], 0};
    display->elements[i] = (float32_t)strtoul(pair, NULL, 16) / 255.0f;
  }
  return true_v;
}

static void color_remember(VkrEditorColorPicker *picker) {
  const Vec4 display = color_current(picker);
  uint32_t keep = 0u;
  Vec4 recent[VKR_EDITOR_COLOR_RECENT_MAX];
  recent[keep++] = display;
  for (uint32_t i = 0;
       i < picker->recent_count && keep < VKR_EDITOR_COLOR_RECENT_MAX; ++i) {
    if (MemCompare(&picker->recent[i], &display, sizeof(display)) != 0) {
      recent[keep++] = picker->recent[i];
    }
  }
  MemCopy(picker->recent, recent, sizeof(Vec4) * keep);
  picker->recent_count = keep;
}

/* The swatch's next Details build applies the picker's color. */
static void color_publish(VkrEditorColorPicker *picker, bool8_t dragging) {
  VkrEditorDetails *details = picker->details;
  details->color_value = color_stored(picker, color_current(picker));
  details->color_pending = true_v;
  details->color_dragging = dragging;
}

static void color_close(VkrEditorUi *editor, VkrUiSystem *ui) {
  VkrEditorColorPicker *picker = &editor->color_picker;
  if (picker->details && picker->details->color_field == picker->field) {
    picker->details->color_dragging = false_v;
  }
  picker->open = false_v;
  picker->drag = COLOR_DRAG_NONE;
  (void)vkr_ui_keyboard_layer_set(ui, 0u);
}

void vkr_editor_color_picker_open(VkrEditorUi *editor,
                                  VkrEditorDetails *details) {
  if (!details->color_requested) {
    return;
  }
  details->color_requested = false_v;
  VkrEditorColorPicker *picker = &editor->color_picker;
  picker->open = true_v;
  picker->details = details;
  picker->field = details->color_field;
  picker->property = details->color_property;
  picker->anchor_pt = details->color_anchor_pt;
  picker->original = details->color_value;
  picker->drag = COLOR_DRAG_NONE;
  picker->saturation = 0.0f;
  picker->hue = 0.0f;
  color_to_hsv(picker, color_display(picker, details->color_value));
  color_format_hex(picker);
}

/* A label at points inside the popup panel. */
static VkrUiWidgetConfig color_widget(float32_t x, float32_t y, float32_t w,
                                      float32_t h) {
  VkrUiWidgetConfig config = vkr_editor_details_widget(x, y, w, h);
  config.style.padding_pt = (VkrUiEdges){0};
  config.style.hover_background_color = VKR_UI_COLOR_NONE;
  config.style.active_background_color = VKR_UI_COLOR_NONE;
  return config;
}

static void color_gradient(VkrUiSystem *ui, String8 id, float32_t x,
                           float32_t y, float32_t w, float32_t h, Vec4 start,
                           Vec4 end, bool8_t vertical) {
  VkrUiWidgetConfig config = color_widget(x, y, w, h);
  config.style.background_color = start;
  config.style.gradient_color = end;
  config.style.gradient = vertical ? VKR_UI_DRAW_GRADIENT_VERTICAL
                                   : VKR_UI_DRAW_GRADIENT_HORIZONTAL;
  vkr_ui_label(ui, id, (String8){0}, &config);
}

/* A ring marking a position on the square or a bar. */
static void color_marker(VkrUiSystem *ui, String8 id, float32_t x, float32_t y,
                         float32_t w, float32_t h) {
  VkrUiWidgetConfig config = color_widget(x, y, w, h);
  const float32_t radius = Min(w, h) * 0.5f;
  config.style.corner_radius_pt = (Vec4){radius, radius, radius, radius};
  config.style.border_pt = (VkrUiEdges){2, 2, 2, 2};
  config.style.border_color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
  config.style.shadow_color = (Vec4){0.0f, 0.0f, 0.0f, 0.6f};
  config.style.shadow_blur_pt = 3.0f;
  vkr_ui_label(ui, id, (String8){0}, &config);
}

/* A clickable color cell; returns whether it was pressed. */
static bool8_t color_cell(VkrUiSystem *ui, String8 id, float32_t x, float32_t y,
                          float32_t size, Vec4 display, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig config = color_widget(x, y, size, size);
  config.style.background_color = (Vec4){display.x, display.y, display.z, 1};
  config.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config.style.border_color = theme->border_strong;
  config.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
  config.tooltip = tooltip;
  return vkr_ui_button(ui, id, (String8){0}, &config);
}

static bool8_t color_point_in(VkrUiSystem *ui, VkrUiRect rect) {
  return ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
         ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height;
}

void vkr_editor_color_picker_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  VkrEditorColorPicker *picker = &editor->color_picker;
  VkrUiSystem *ui = frame->ui;
  if (!picker->open) {
    return;
  }
  if (!picker->details || picker->details->color_field != picker->field ||
      !picker->property) {
    color_close(editor, ui);
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const bool8_t alpha = color_has_alpha(picker);
  const float32_t inner = COLOR_WIDTH_PT - COLOR_PAD_PT * 2.0f;
  /* Rows from the top, in points. */
  const float32_t square_y = COLOR_PAD_PT;
  const float32_t hue_y = square_y + COLOR_SQUARE_PT + COLOR_GAP_PT;
  const float32_t alpha_y = hue_y + COLOR_BAR_PT + COLOR_GAP_PT;
  const float32_t row_y =
      (alpha ? alpha_y + COLOR_BAR_PT : hue_y + COLOR_BAR_PT) + COLOR_GAP_PT;
  const float32_t palette_y = row_y + COLOR_ROW_PT + COLOR_GAP_PT;
  const uint32_t palette_rows =
      (ArrayCount(s_palette) + COLOR_PALETTE_COLUMNS - 1u) /
      COLOR_PALETTE_COLUMNS;
  const float32_t cell_step =
      (inner - COLOR_CELL_PT) / (float32_t)(COLOR_PALETTE_COLUMNS - 1u);
  const float32_t recent_y =
      palette_y + (float32_t)palette_rows * (COLOR_CELL_PT + 4.0f) + 4.0f;
  const float32_t height =
      (picker->recent_count ? recent_y + COLOR_CELL_PT : recent_y - 4.0f) +
      COLOR_PAD_PT;

  /* Beside the swatch, right-aligned to it, kept on screen. */
  const float32_t screen_w = (float32_t)ui->target_width / scale;
  const float32_t screen_h = (float32_t)ui->target_height / scale;
  const float32_t x = Max(0.0f, Min(picker->anchor_pt.x - COLOR_WIDTH_PT,
                                    screen_w - COLOR_WIDTH_PT));
  const float32_t y =
      Max(0.0f, Min(picker->anchor_pt.y + 4.0f, screen_h - height));
  const VkrUiRect rect = {x * scale, y * scale, COLOR_WIDTH_PT * scale,
                          height * scale};
  const VkrUiRect square_px = {(x + COLOR_PAD_PT) * scale,
                               (y + square_y) * scale, inner * scale,
                               COLOR_SQUARE_PT * scale};
  const VkrUiRect hue_px = {(x + COLOR_PAD_PT) * scale, (y + hue_y) * scale,
                            inner * scale, COLOR_BAR_PT * scale};
  const VkrUiRect alpha_px = {(x + COLOR_PAD_PT) * scale, (y + alpha_y) * scale,
                              inner * scale, COLOR_BAR_PT * scale};

  /* Escape restores the color it opened with; a press outside keeps the
     pick. */
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    picker->details->color_value = picker->original;
    picker->details->color_pending = true_v;
    color_close(editor, ui);
    return;
  }
  if (ui->mouse_pressed && picker->drag == COLOR_DRAG_NONE &&
      !color_point_in(ui, rect)) {
    color_close(editor, ui);
    return;
  }

  /* Dragging on the square, hue or alpha bar edits the color live; the
     release ends the drag's single undo step. */
  if (ui->mouse_pressed) {
    picker->drag = color_point_in(ui, square_px)           ? COLOR_DRAG_SQUARE
                   : color_point_in(ui, hue_px)            ? COLOR_DRAG_HUE
                   : alpha && color_point_in(ui, alpha_px) ? COLOR_DRAG_ALPHA
                                                           : COLOR_DRAG_NONE;
  }
  if (picker->drag != COLOR_DRAG_NONE) {
    const bool8_t held = input_is_button_down(frame->input, BUTTON_LEFT);
    const VkrUiRect area = picker->drag == COLOR_DRAG_SQUARE ? square_px
                           : picker->drag == COLOR_DRAG_HUE  ? hue_px
                                                             : alpha_px;
    const float32_t u =
        Clamp(((float32_t)ui->mouse_x - area.x) / area.width, 0.0f, 1.0f);
    const float32_t v =
        Clamp(((float32_t)ui->mouse_y - area.y) / area.height, 0.0f, 1.0f);
    if (picker->drag == COLOR_DRAG_SQUARE) {
      picker->saturation = u;
      picker->value = 1.0f - v;
    } else if (picker->drag == COLOR_DRAG_HUE) {
      picker->hue = Min(u, 0.9999f);
    } else {
      picker->alpha = u;
    }
    color_publish(picker, held);
    color_format_hex(picker);
    if (!held) {
      color_remember(picker);
      picker->drag = COLOR_DRAG_NONE;
    }
  }

  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_POPUP_LAYER, rect);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_POPUP_LAYER);
  (void)vkr_ui_keyboard_layer_set(ui, VKR_EDITOR_POPUP_LAYER);
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement.column = popup.placement.row = 0u;
  popup.placement.justify = popup.placement.align = VKR_UI_ALIGN_START;
  popup.placement.margin_pt = (VkrUiEdges){y, 0, 0, x};
  popup.style = vkr_editor_glass_style();
  popup.style.background_color.w = Max(popup.style.background_color.w, 0.97f);
  popup.style.padding_pt = (VkrUiEdges){0};
  popup.style.min_size_pt = popup.style.max_size_pt =
      (Vec2){COLOR_WIDTH_PT, height};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.color.picker"), &popup)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }

  /* The square: white to the full hue across, then black down, each in
     slices whose endpoints are exact display colors. Black's coverage
     follows the display ramp, since blending happens in linear space.
     Slices meet edge to edge: an overlap would darken black twice. */
  const float32_t slice_w = inner / (float32_t)COLOR_SLICES;
  const float32_t slice_h = COLOR_SQUARE_PT / (float32_t)COLOR_SLICES;
  for (uint32_t i = 0; i < COLOR_SLICES; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const float32_t s0 = (float32_t)i / COLOR_SLICES;
    const float32_t s1 = (float32_t)(i + 1u) / COLOR_SLICES;
    color_gradient(ui, string8_lit("saturation"),
                   COLOR_PAD_PT + slice_w * (float32_t)i, square_y, slice_w,
                   COLOR_SQUARE_PT, color_from_hsv(picker->hue, s0, 1.0f, 1.0f),
                   color_from_hsv(picker->hue, s1, 1.0f, 1.0f), false_v);
    (void)vkr_ui_pop_id(ui);
  }
  for (uint32_t i = 0; i < COLOR_SLICES; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const float32_t v0 = 1.0f - (float32_t)i / COLOR_SLICES;
    const float32_t v1 = 1.0f - (float32_t)(i + 1u) / COLOR_SLICES;
    color_gradient(ui, string8_lit("value"), COLOR_PAD_PT,
                   square_y + slice_h * (float32_t)i, inner, slice_h,
                   (Vec4){0, 0, 0, 1.0f - vkr_srgb_to_linear(v0)},
                   (Vec4){0, 0, 0, 1.0f - vkr_srgb_to_linear(v1)}, true_v);
    (void)vkr_ui_pop_id(ui);
  }
  color_marker(ui, string8_lit("square.marker"),
               COLOR_PAD_PT + picker->saturation * inner - 6.0f,
               square_y + (1.0f - picker->value) * COLOR_SQUARE_PT - 6.0f,
               12.0f, 12.0f);

  /* The hue bar, red around to red. */
  for (uint32_t i = 0; i < 6u; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    color_gradient(
        ui, string8_lit("hue"), COLOR_PAD_PT + inner / 6.0f * (float32_t)i,
        hue_y, inner / 6.0f, COLOR_BAR_PT,
        color_from_hsv((float32_t)i / 6.0f, 1.0f, 1.0f, 1.0f),
        color_from_hsv((float32_t)(i + 1u) / 6.0f, 1.0f, 1.0f, 1.0f), false_v);
    (void)vkr_ui_pop_id(ui);
  }
  color_marker(ui, string8_lit("hue.marker"),
               COLOR_PAD_PT + picker->hue * inner - 4.0f, hue_y - 2.0f, 8.0f,
               COLOR_BAR_PT + 4.0f);

  const Vec4 current = color_current(picker);
  if (alpha) {
    /* Over a light band and a dark band, so transparency reads. */
    VkrUiWidgetConfig light =
        color_widget(COLOR_PAD_PT, alpha_y, inner, COLOR_BAR_PT * 0.5f);
    light.style.background_color = (Vec4){0.80f, 0.80f, 0.80f, 1.0f};
    vkr_ui_label(ui, string8_lit("alpha.light"), (String8){0}, &light);
    VkrUiWidgetConfig dark =
        color_widget(COLOR_PAD_PT, alpha_y + COLOR_BAR_PT * 0.5f, inner,
                     COLOR_BAR_PT * 0.5f);
    dark.style.background_color = (Vec4){0.35f, 0.35f, 0.35f, 1.0f};
    vkr_ui_label(ui, string8_lit("alpha.dark"), (String8){0}, &dark);
    color_gradient(ui, string8_lit("alpha"), COLOR_PAD_PT, alpha_y, inner,
                   COLOR_BAR_PT, (Vec4){current.x, current.y, current.z, 0.0f},
                   (Vec4){current.x, current.y, current.z, 1.0f}, false_v);
    color_marker(ui, string8_lit("alpha.marker"),
                 COLOR_PAD_PT + picker->alpha * inner - 4.0f, alpha_y - 2.0f,
                 8.0f, COLOR_BAR_PT + 4.0f);
  }

  /* The color it opened with restores on click, beside the current one and
     its hex code. */
  VkrEditorColorPicker probe = *picker;
  const Vec4 original_display = color_display(&probe, picker->original);
  if (color_cell(ui, string8_lit("original"), COLOR_PAD_PT, row_y, COLOR_ROW_PT,
                 original_display,
                 string8_lit("Restore the color the picker opened with"))) {
    picker->intensity = probe.intensity;
    color_to_hsv(picker, original_display);
    color_publish(picker, false_v);
    color_format_hex(picker);
  }
  (void)color_cell(ui, string8_lit("current"),
                   COLOR_PAD_PT + COLOR_ROW_PT + 2.0f, row_y, COLOR_ROW_PT,
                   current, string8_lit("Current color"));
  VkrUiWidgetConfig hex = vkr_editor_details_widget(
      COLOR_PAD_PT + COLOR_ROW_PT * 2.0f + 10.0f, row_y,
      inner - COLOR_ROW_PT * 2.0f - 10.0f, COLOR_ROW_PT);
  hex.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
  hex.style.font_size_pt = theme->font_body;
  hex.tooltip = string8_lit("Display sRGB as #RRGGBB, or #RRGGBBAA");
  vkr_editor_field_style(&hex);
  VkrUiTextEditBuffer buffer = {(uint8_t *)picker->hex,
                                (uint32_t)strlen(picker->hex),
                                sizeof(picker->hex)};
  if (vkr_ui_text_field(ui, string8_lit("hex"), &buffer, &hex)) {
    Vec4 display = current;
    if (color_parse_hex(picker->hex, &display)) {
      color_to_hsv(picker, display);
      color_publish(picker, false_v);
    }
  }

  for (uint32_t i = 0; i < ArrayCount(s_palette); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const uint32_t column = i % COLOR_PALETTE_COLUMNS;
    const uint32_t row = i / COLOR_PALETTE_COLUMNS;
    if (color_cell(ui, string8_lit("palette"),
                   COLOR_PAD_PT + cell_step * (float32_t)column,
                   palette_y + (COLOR_CELL_PT + 4.0f) * (float32_t)row,
                   COLOR_CELL_PT, s_palette[i], (String8){0})) {
      color_to_hsv(picker, (Vec4){s_palette[i].x, s_palette[i].y,
                                  s_palette[i].z, picker->alpha});
      color_publish(picker, false_v);
      color_format_hex(picker);
      color_remember(picker);
    }
    (void)vkr_ui_pop_id(ui);
  }
  for (uint32_t i = 0; i < picker->recent_count; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const Vec4 recent = picker->recent[i];
    if (color_cell(ui, string8_lit("recent"),
                   COLOR_PAD_PT + cell_step * (float32_t)i, recent_y,
                   COLOR_CELL_PT, recent, string8_lit("Recent color"))) {
      color_to_hsv(picker, (Vec4){recent.x, recent.y, recent.z,
                                  alpha ? recent.w : picker->alpha});
      color_publish(picker, false_v);
      color_format_hex(picker);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
}
