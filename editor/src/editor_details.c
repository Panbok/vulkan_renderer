#include "editor_details.h"

#include "editor_internal.h"
#include "vkr_color_transfer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DETAILS_ROW_PT VKR_EDITOR_DETAILS_ROW_PT
#define DETAILS_PAD_PT VKR_EDITOR_DETAILS_PAD_PT

/* One vkr_editor_details_type call: the value being edited and what this
 * build has done to it. */
typedef struct DetailsBuild {
  VkrEditorDetails *details;
  VkrUiSystem *ui;
  InputState *input;
  const VkrTypeDesc *type;
  void *value;
  float32_t width;
  bool8_t focused;
} DetailsBuild;

VkrUiWidgetConfig vkr_editor_details_widget(float32_t x, float32_t y,
                                            float32_t width, float32_t height) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = (VkrUiPlacement){.column = 0,
                                      .row = 0,
                                      .column_span = 1,
                                      .row_span = 1,
                                      .justify = VKR_UI_ALIGN_START,
                                      .align = VKR_UI_ALIGN_START,
                                      .margin_pt = {y, 0, 0, x}};
  config.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  config.style.max_size_pt = config.style.min_size_pt;
  config.style.font_size_pt = 12.0f;
  config.style.padding_pt = (VkrUiEdges){3, 5, 3, 5};
  return config;
}

float32_t vkr_editor_details_label_width(float32_t width) {
  return vkr_clamp_f32(width * 0.36f, 72.0f, 132.0f);
}

static String8 details_cstr(const char *text) {
  return text ? string8_create((uint8_t *)text, strlen(text)) : (String8){0};
}

void vkr_editor_details_begin(VkrEditorDetails *details) {
  details->gesture_held = details->gesture_active;
  details->gesture_active = false_v;
}

void vkr_editor_details_end(VkrEditorDetails *details) {
  if (!details->gesture_active) {
    details->gesture = 0u;
  }
}

void vkr_editor_details_cancel(VkrEditorDetails *details, VkrUiSystem *ui) {
  if (details->edit_field && ui) {
    if (ui->focused_id == details->edit_field) {
      ui->focused_id = 0;
    }
    if (ui->active_id == details->edit_field) {
      ui->active_id = 0;
    }
  }
  details->edit_field = 0;
  details->edit_text[0] = 0;
  details->edit_original[0] = 0;
  details->choice_requested = false_v;
  details->choice_picked = false_v;
  details->color_requested = false_v;
  details->color_pending = false_v;
  details->color_dragging = false_v;
  /* An open picker closes once its field is gone. */
  details->color_field = 0;
  details->gesture_active = false_v;
  details->gesture_held = false_v;
  details->gesture = 0u;
}

/* Starts or continues the gesture owned by `owner`; a drag that continues
 * from the previous frame keeps its id so it folds into one undo entry. */
static uint64_t details_gesture(VkrEditorDetails *details, VkrUiId owner) {
  if (!details->gesture_held || details->gesture_owner != owner ||
      !details->gesture) {
    details->gesture = ++details->gesture_counter;
    details->gesture_owner = owner;
  }
  details->gesture_active = true_v;
  return details->gesture;
}

// =============================================================================
// Display values
// =============================================================================

static bool8_t details_integer(const VkrPropertyDesc *property) {
  return property->kind == VKR_PROPERTY_I32 ||
         property->kind == VKR_PROPERTY_U32;
}

/* Display value of one component: degrees for angles, directions and
 * rotations, and the display scale elsewhere. */
static float64_t details_get(const VkrPropertyDesc *property, const void *value,
                             uint32_t component) {
  float32_t floats[4] = {0};
  switch (property->kind) {
  case VKR_PROPERTY_DIRECTION: {
    float32_t angles[2];
    (void)vkr_property_get_floats(property, value, floats);
    vkr_property_direction_angles((Vec3){floats[0], floats[1], floats[2], 0},
                                  &angles[0], &angles[1]);
    return angles[Min(component, 1u)];
  }
  case VKR_PROPERTY_QUAT: {
    float32_t degrees[3];
    (void)vkr_property_get_floats(property, value, floats);
    vkr_property_quat_euler((Vec4){floats[0], floats[1], floats[2], floats[3]},
                            degrees);
    return degrees[Min(component, 2u)];
  }
  case VKR_PROPERTY_VEC2:
  case VKR_PROPERTY_VEC3:
  case VKR_PROPERTY_VEC4:
  case VKR_PROPERTY_COLOR:
    (void)vkr_property_get_floats(property, value, floats);
    return floats[component] * vkr_property_display_scale(property);
  default: {
    float64_t number = 0.0;
    (void)vkr_property_get_number(property, value, &number);
    return number * vkr_property_display_scale(property);
  }
  }
}

static bool8_t details_set(const VkrPropertyDesc *property, void *value,
                           uint32_t component, float64_t display) {
  if (!isfinite(display)) {
    return false_v;
  }
  float32_t floats[4] = {0};
  switch (property->kind) {
  case VKR_PROPERTY_DIRECTION: {
    float32_t angles[2];
    (void)vkr_property_get_floats(property, value, floats);
    vkr_property_direction_angles((Vec3){floats[0], floats[1], floats[2], 0},
                                  &angles[0], &angles[1]);
    angles[Min(component, 1u)] = (float32_t)display;
    const Vec3 direction =
        vkr_property_direction_from_angles(angles[0], angles[1]);
    return vkr_property_set_floats(property, value, direction.elements);
  }
  case VKR_PROPERTY_QUAT: {
    float32_t degrees[3];
    (void)vkr_property_get_floats(property, value, floats);
    vkr_property_quat_euler((Vec4){floats[0], floats[1], floats[2], floats[3]},
                            degrees);
    degrees[Min(component, 2u)] = (float32_t)display;
    const Vec4 rotation = vkr_property_quat_from_euler(degrees);
    return vkr_property_set_floats(property, value, rotation.elements);
  }
  case VKR_PROPERTY_VEC2:
  case VKR_PROPERTY_VEC3:
  case VKR_PROPERTY_VEC4:
  case VKR_PROPERTY_COLOR:
    (void)vkr_property_get_floats(property, value, floats);
    floats[component] =
        (float32_t)(display / vkr_property_display_scale(property));
    return vkr_property_set_floats(property, value, floats);
  default:
    return vkr_property_set_number(
        property, value, display / vkr_property_display_scale(property));
  }
}

/* Display-unit bounds of one component; false when unbounded. */
static bool8_t details_bounds(const VkrPropertyDesc *property,
                              uint32_t component, float64_t *low,
                              float64_t *high) {
  if (property->kind == VKR_PROPERTY_DIRECTION) {
    *low = component ? -90.0 : -180.0;
    *high = component ? 90.0 : 180.0;
    return true_v;
  }
  if (property->kind == VKR_PROPERTY_QUAT || !(property->min < property->max)) {
    return false_v;
  }
  const float64_t scale = property->kind == VKR_PROPERTY_ANGLE
                              ? 1.0
                              : vkr_property_display_scale(property);
  *low = property->min * scale;
  *high = property->max * scale;
  return true_v;
}

/* Unfocused fields show `digits` significant digits so cells stay readable;
 * an entry starts from nine, enough to round-trip a float. */
static void details_format(const VkrPropertyDesc *property, float64_t display,
                           bool8_t with_unit, int32_t digits, char *out,
                           uint32_t capacity) {
  const char *unit = with_unit ? property->unit : NULL;
  const bool8_t tight =
      unit && (strcmp(unit, "%") == 0 || strcmp(unit, "x") == 0);
  if (details_integer(property)) {
    snprintf(out, capacity, "%.0f%s%s", display, unit && !tight ? " " : "",
             unit ? unit : "");
  } else {
    snprintf(out, capacity, "%.*g%s%s", with_unit ? digits : 9, display,
             unit && !tight ? " " : "", unit ? unit : "");
  }
}

/* A number optionally followed by the property's unit. */
static bool8_t details_parse(const VkrPropertyDesc *property, const char *text,
                             float64_t *out) {
  char *end = NULL;
  const float64_t number = strtod(text, &end);
  if (end == text || !isfinite(number)) {
    return false_v;
  }
  while (*end == ' ' || *end == '\t') {
    ++end;
  }
  if (*end && property->unit && strcmp(end, property->unit) == 0) {
    end += strlen(end);
  }
  while (*end == ' ' || *end == '\t') {
    ++end;
  }
  if (*end) {
    return false_v;
  }
  *out = number;
  return true_v;
}

// =============================================================================
// Widgets
// =============================================================================

/* Drag handle: while held, horizontal mouse motion changes the component by
 * its step per point (Shift 10x faster, Alt 10x finer). */
static void details_scrub(DetailsBuild *build, uint32_t index,
                          uint32_t component, VkrUiId handle) {
  VkrUiSystem *ui = build->ui;
  if (ui->active_id != handle) {
    return;
  }
  (void)details_gesture(build->details, handle);
  int32_t dx = 0;
  int32_t dy = 0;
  input_get_mouse_delta(build->input, &dx, &dy);
  if (!dx) {
    return;
  }
  const VkrPropertyDesc *property = &build->type->properties[index];
  float64_t step = property->step;
  if (step <= 0.0) {
    step = details_integer(property) ? 1.0 : 0.01;
  }
  step /= Max(ui->content_scale, 1.0f);
  if (input_is_key_down(build->input, KEY_SHIFT) ||
      input_is_key_down(build->input, KEY_LSHIFT) ||
      input_is_key_down(build->input, KEY_RSHIFT)) {
    step *= 10.0;
  }
  if (input_is_key_down(build->input, KEY_LMENU) ||
      input_is_key_down(build->input, KEY_RMENU)) {
    step *= 0.1;
  }
  float64_t display =
      details_get(property, build->value, component) + (float64_t)dx * step;
  float64_t low = 0.0;
  float64_t high = 0.0;
  if (details_bounds(property, component, &low, &high)) {
    /* Below the range a value with an "Off" zero turns off. */
    display = property->zero_label && display < low
                  ? (low > 0.0 && display > 0.0 && dx > 0 ? low : 0.0)
                  : Min(display, high);
  }
  (void)details_set(property, build->value, component, display);
}

/* Parse the active text entry into its component. */
static void details_commit_text(DetailsBuild *build, uint32_t index,
                                uint32_t component) {
  const VkrPropertyDesc *property = &build->type->properties[index];
  if (strcmp(build->details->edit_text, build->details->edit_original) == 0) {
    return;
  }
  if (property->kind == VKR_PROPERTY_STRING) {
    char *target = (char *)build->value + property->offset;
    const uint64_t length = strlen(build->details->edit_text);
    if (length < property->capacity) {
      MemCopy(target, build->details->edit_text, length + 1u);
    } else {
      snprintf(build->details->error, sizeof(build->details->error),
               "%s is longer than %u bytes.", property->label,
               property->capacity - 1u);
    }
    return;
  }
  float64_t display = 0.0;
  if (!details_parse(property, build->details->edit_text, &display) ||
      !details_set(property, build->value, component, display)) {
    snprintf(build->details->error, sizeof(build->details->error),
             "Enter a number for %s.", property->label);
  }
}

static void details_edit_text_reset(DetailsBuild *build, uint32_t index,
                                    uint32_t component) {
  const VkrPropertyDesc *property = &build->type->properties[index];
  VkrEditorDetails *details = build->details;
  if (property->kind == VKR_PROPERTY_STRING) {
    snprintf(details->edit_text, sizeof(details->edit_text), "%s",
             (const char *)build->value + property->offset);
  } else {
    details_format(property, details_get(property, build->value, component),
                   false_v, 9, details->edit_text, sizeof(details->edit_text));
  }
  MemCopy(details->edit_original, details->edit_text,
          sizeof(details->edit_original));
}

/* Text field for one component. Unfocused it shows the formatted value;
 * focused it edits the panel's single entry buffer, which applies on Enter or
 * when focus leaves and is discarded by Escape. */
static void details_text_field(DetailsBuild *build, uint32_t index,
                               uint32_t component, float32_t x, float32_t y,
                               float32_t width, bool8_t read_only) {
  VkrUiSystem *ui = build->ui;
  VkrEditorDetails *details = build->details;
  const VkrPropertyDesc *property = &build->type->properties[index];
  (void)vkr_ui_push_id_u64(ui, component);
  const VkrUiId id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("value"));
  const bool8_t focused = ui->focused_id == id && !read_only;
  if (details->edit_field == id && !focused) {
    details_commit_text(build, index, component);
    details->edit_field = 0;
  }
  if (focused && input_key_just_pressed(build->input, KEY_ESCAPE)) {
    details->edit_field = 0;
    ui->focused_id = 0;
    (void)vkr_ui_pop_id(ui);
    return;
  }
  if (focused && details->edit_field != id) {
    details->edit_field = id;
    details_edit_text_reset(build, index, component);
  }

  char display[VKR_EDITOR_DETAILS_TEXT_CAPACITY];
  VkrUiTextEditBuffer buffer;
  if (details->edit_field == id) {
    buffer = (VkrUiTextEditBuffer){(uint8_t *)details->edit_text,
                                   (uint32_t)strlen(details->edit_text),
                                   sizeof(details->edit_text)};
  } else {
    if (property->kind == VKR_PROPERTY_STRING) {
      snprintf(display, sizeof(display), "%s",
               (const char *)build->value + property->offset);
    } else {
      const float64_t shown = details_get(property, build->value, component);
      if (shown == 0.0 && property->zero_label &&
          vkr_property_components(property) == 1u) {
        snprintf(display, sizeof(display), "%s", property->zero_label);
      } else {
        /* Narrow vector cells keep four digits visible. */
        details_format(property, shown, true_v, width < 80.0f ? 4 : 5, display,
                       sizeof(display));
      }
    }
    buffer = (VkrUiTextEditBuffer){(uint8_t *)display,
                                   (uint32_t)strlen(display), sizeof(display)};
  }
  VkrUiWidgetConfig config =
      vkr_editor_details_widget(x, y + 1.0f, width, DETAILS_ROW_PT - 4.0f);
  config.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
  config.read_only = read_only;
  vkr_editor_field_style(&config);
  (void)vkr_ui_text_field(ui, string8_lit("value"), &buffer, &config);
  if (details->edit_field == id && ui->focused_id == id) {
    build->focused = true_v;
    if (input_key_just_pressed(build->input, KEY_ENTER)) {
      details_commit_text(build, index, component);
      details_edit_text_reset(build, index, component);
    }
  }
  (void)vkr_ui_pop_id(ui);
}

/* Label in secondary text; with `scrub` it drags `component`. */
static void details_label(DetailsBuild *build, uint32_t index,
                          uint32_t component, String8 text, float32_t y,
                          float32_t width, bool8_t scrub, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = build->ui;
  VkrUiWidgetConfig handle = vkr_editor_details_widget(
      DETAILS_PAD_PT, y, Max(8.0f, width), DETAILS_ROW_PT - 2.0f);
  handle.placement.align = VKR_UI_ALIGN_START;
  handle.style.padding_pt = (VkrUiEdges){5, 2, 5, 2};
  handle.style.font_size_pt = theme->font_body;
  handle.style.text_color = theme->text_secondary;
  handle.tooltip = tooltip;
  if (scrub) {
    vkr_editor_ghost_style(&handle);
    handle.style.padding_pt = (VkrUiEdges){5, 2, 5, 2};
    handle.style.text_color = theme->text_secondary;
    handle.cursor = VKR_WINDOW_CURSOR_RESIZE_EW;
    if (!tooltip.length) {
      handle.tooltip = string8_lit("Drag to change (Shift faster, Alt finer)");
    }
    const VkrUiId handle_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("handle"));
    (void)vkr_ui_button(ui, string8_lit("handle"), (String8){0}, &handle);
    details_scrub(build, index, component, handle_id);
    VkrUiWidgetConfig label = handle;
    label.placement.justify = VKR_UI_ALIGN_START;
    label.style.background_color = (Vec4){0};
    label.tooltip = (String8){0};
    vkr_ui_label(ui, string8_lit("label"), text, &label);
    return;
  }
  vkr_ui_label(ui, string8_lit("label"), text, &handle);
}

static bool8_t details_scalar_row(DetailsBuild *build, uint32_t index,
                                  uint32_t component, String8 label,
                                  float32_t *y, bool8_t read_only,
                                  String8 tooltip) {
  const VkrPropertyDesc *property = &build->type->properties[index];
  VkrUiSystem *ui = build->ui;
  const float32_t w = build->width;
  const float32_t label_w = vkr_editor_details_label_width(w);
  const float32_t left = DETAILS_PAD_PT + label_w;
  const float32_t available = Max(60.0f, w - left - DETAILS_PAD_PT);
  float64_t low = 0.0;
  float64_t high = 0.0;
  const bool8_t slider = !read_only &&
                         details_bounds(property, component, &low, &high) &&
                         (property->kind == VKR_PROPERTY_DIRECTION ||
                          (property->flags & VKR_PROPERTY_FLAG_SLIDER)) &&
                         isfinite(low) && isfinite(high) && low < high;
  const float32_t field_w = slider ? Min(78.0f, available * 0.42f) : available;
  (void)vkr_ui_push_id_u64(ui, 0x5ca10000u + component);
  /* The label itself scrubs, as in UE5 and Unity. */
  details_label(build, index, component, label, *y, label_w - 4.0f, !read_only,
                tooltip);
  if (slider) {
    const float64_t current = details_get(property, build->value, component);
    float32_t slider_value = (float32_t)Clamp(current, low, high);
    VkrUiWidgetConfig bar = vkr_editor_details_widget(
        left, *y + 2.0f, available - field_w - 8.0f, DETAILS_ROW_PT - 6.0f);
    bar.tooltip = tooltip;
    const VkrUiId bar_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("slider"));
    if (vkr_ui_slider_f32(ui, string8_lit("slider"), &slider_value,
                          (float32_t)low, (float32_t)high, &bar)) {
      float64_t next = slider_value;
      if (details_integer(property)) {
        next = round(next);
      }
      (void)details_set(property, build->value, component, next);
    }
    if (ui->active_id == bar_id) {
      (void)details_gesture(build->details, bar_id);
    }
  }
  details_text_field(build, index, component, w - DETAILS_PAD_PT - field_w, *y,
                     field_w, read_only);
  (void)vkr_ui_pop_id(ui);
  *y += DETAILS_ROW_PT + 2.0f;
  return true_v;
}

/* Opaque display swatch of a color property's value, clamped to the
   displayable range. */
static Vec4 details_swatch(const VkrPropertyDesc *property,
                           const float32_t floats[4]) {
  Vec4 swatch = {0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t i = 0; i < 3; ++i) {
    const float32_t c = Clamp(floats[i], 0.0f, 1.0f);
    swatch.elements[i] =
        (property->flags & VKR_PROPERTY_FLAG_SRGB) ? c : vkr_linear_to_srgb(c);
  }
  return swatch;
}

static void details_vector_row(DetailsBuild *build, uint32_t index,
                               String8 label, float32_t *y, bool8_t read_only,
                               String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrPropertyDesc *property = &build->type->properties[index];
  VkrUiSystem *ui = build->ui;
  const bool8_t color = vkr_editor_details_is_color(property);
  const uint32_t count = property->kind == VKR_PROPERTY_QUAT
                             ? 3u
                             : vkr_property_components(property);
  static const char *const axis_names[2][4] = {{"X", "Y", "Z", "W"},
                                               {"R", "G", "B", "A"}};
  const Vec4 axis_colors[4] = {theme->axis_x, theme->axis_y, theme->axis_z,
                               theme->text_secondary};
  float32_t w = build->width;
  if (color) {
    VkrEditorDetails *details = build->details;
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("swatch"));
    /* The picker's value applies here, one gesture per picker drag. */
    if (details->color_field == id && !read_only) {
      if (details->color_pending) {
        (void)vkr_property_set_floats(property, build->value,
                                      details->color_value.elements);
        details->color_pending = false_v;
      }
      if (details->color_dragging) {
        (void)details_gesture(details, id);
      }
    }
    float32_t floats[4] = {0, 0, 0, 1};
    (void)vkr_property_get_floats(property, build->value, floats);
    VkrUiWidgetConfig swatch = vkr_editor_details_widget(
        w - DETAILS_PAD_PT - 22.0f, *y + 3.0f, 22.0f, DETAILS_ROW_PT - 6.0f);
    swatch.style.background_color = details_swatch(property, floats);
    swatch.style.hover_background_color = VKR_UI_COLOR_NONE;
    swatch.style.active_background_color = VKR_UI_COLOR_NONE;
    swatch.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
    swatch.style.border_color = theme->border_strong;
    swatch.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
    swatch.tooltip = read_only ? details_cstr(property->tooltip)
                               : string8_lit("Pick a color");
    swatch.disabled = read_only;
    if (vkr_ui_button(ui, string8_lit("swatch"), (String8){0}, &swatch) &&
        !read_only) {
      VkrUiRect rect = {0};
      (void)vkr_ui_widget_rect(ui, id, &rect);
      details->color_requested = true_v;
      details->color_field = id;
      details->color_property = property;
      details->color_value = (Vec4){floats[0], floats[1], floats[2], floats[3]};
      details->color_anchor_pt =
          (Vec2){(rect.x + rect.width) / ui->content_scale,
                 (rect.y + rect.height) / ui->content_scale};
    }
    w -= 28.0f;
  }
  /* Narrow panels move the label above the fields so each axis keeps a
   * readable width. */
  const bool8_t stacked = w < 260.0f;
  const float32_t label_w =
      stacked ? w - DETAILS_PAD_PT * 2.0f : vkr_editor_details_label_width(w);
  const float32_t left = DETAILS_PAD_PT + (stacked ? 0.0f : label_w);
  const float32_t available = Max(60.0f, w - left - DETAILS_PAD_PT);
  const float32_t cell = available / (float32_t)count;
  details_label(build, index, 0u, label, *y, label_w - 4.0f, false_v, tooltip);
  if (stacked) {
    *y += DETAILS_ROW_PT - 6.0f;
  }
  for (uint32_t axis = 0; axis < count; ++axis) {
    const float32_t x = left + cell * (float32_t)axis;
    VkrUiWidgetConfig tag =
        vkr_editor_details_widget(x, *y + 1.0f, 18.0f, DETAILS_ROW_PT - 4.0f);
    tag.style.corner_radius_pt = (Vec4){theme->radius, 0, 0, theme->radius};
    tag.style.background_color =
        vkr_ui_color_alpha(axis_colors[axis], read_only ? 0.35f : 0.85f);
    tag.style.hover_background_color = axis_colors[axis];
    tag.style.text_color = theme->text_on_accent;
    tag.style.font_size_pt = theme->font_caption;
    tag.style.padding_pt = (VkrUiEdges){2, 0, 2, 0};
    tag.disabled = read_only;
    tag.tooltip = string8_lit("Drag to change (Shift faster, Alt finer)");
    tag.cursor = VKR_WINDOW_CURSOR_RESIZE_EW;
    (void)vkr_ui_push_id_u64(ui, 0x7a900000u + axis);
    const VkrUiId tag_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("tag"));
    (void)vkr_ui_button(ui, string8_lit("tag"),
                        details_cstr(axis_names[color ? 1 : 0][axis]), &tag);
    (void)vkr_ui_pop_id(ui);
    if (!read_only) {
      details_scrub(build, index, axis, tag_id);
    }
    details_text_field(build, index, axis, x + 17.0f, *y, cell - 20.0f,
                       read_only);
  }
  *y += DETAILS_ROW_PT + 2.0f;
}

/* A label in the label column and a checkbox at the value column's start,
   like every other row. */
static void details_bool_row(DetailsBuild *build, uint32_t index, String8 label,
                             float32_t *y, bool8_t read_only, String8 tooltip) {
  const VkrPropertyDesc *property = &build->type->properties[index];
  const float32_t label_w = vkr_editor_details_label_width(build->width);
  details_label(build, index, 0u, label, *y, label_w - 4.0f, false_v, tooltip);
  VkrUiWidgetConfig config =
      vkr_editor_details_widget(DETAILS_PAD_PT + label_w, *y + 2.0f,
                                DETAILS_ROW_PT - 4.0f, DETAILS_ROW_PT - 4.0f);
  config.placement.align = VKR_UI_ALIGN_START;
  config.style.padding_pt = (VkrUiEdges){3, 0, 3, 0};
  config.disabled = read_only;
  config.tooltip = tooltip;
  bool8_t *target = (bool8_t *)((uint8_t *)build->value + property->offset);
  bool8_t checked = *target;
  if (vkr_ui_checkbox(build->ui, string8_lit("check"), (String8){0}, &checked,
                      &config) &&
      !read_only) {
    *target = checked ? true_v : false_v;
  }
  *y += DETAILS_ROW_PT + 2.0f;
}

bool8_t vkr_editor_details_is_color(const VkrPropertyDesc *property) {
  return property->kind == VKR_PROPERTY_COLOR ||
         (property->kind == VKR_PROPERTY_VEC4 &&
          (property->flags & VKR_PROPERTY_FLAG_COLOR));
}

/* Shown name of choice `i`: its label, else its name capitalized. */
static String8 details_choice_text(VkrUiSystem *ui,
                                   const VkrPropertyDesc *property,
                                   uint32_t i) {
  const uint32_t count = vkr_property_enum_count(property);
  if (i >= count) {
    return string8_lit("?");
  }
  if (property->labels) {
    return details_cstr(property->labels[i]);
  }
  String8 text =
      string8_create_formatted(ui->frame_allocator, "%s", property->names[i]);
  if (text.length && text.str[0] >= 'a' && text.str[0] <= 'z') {
    text.str[0] = (uint8_t)(text.str[0] - 'a' + 'A');
  }
  return text;
}

/* Named choices: up to three as a segmented control, more as a dropdown
 * whose menu the panel's owner opens; read-only choices show the name. */
static void details_choice_row(DetailsBuild *build, uint32_t index,
                               String8 label, float32_t *y, bool8_t read_only,
                               String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrPropertyDesc *property = &build->type->properties[index];
  VkrUiSystem *ui = build->ui;
  VkrEditorDetails *details = build->details;
  const float32_t w = build->width;
  const float32_t label_w = vkr_editor_details_label_width(w);
  const float32_t left = DETAILS_PAD_PT + label_w;
  const float32_t available = Max(60.0f, w - left - DETAILS_PAD_PT);
  const uint32_t count = vkr_property_enum_count(property);
  float64_t current = 0.0;
  (void)vkr_property_get_number(property, build->value, &current);
  details_label(build, index, 0u, label, *y, label_w - 4.0f, false_v, tooltip);
  if (read_only || count == 0u) {
    VkrUiWidgetConfig text =
        vkr_editor_details_widget(left, *y, available, DETAILS_ROW_PT - 2.0f);
    text.style.padding_pt = (VkrUiEdges){5, 6, 5, 6};
    text.style.font_size_pt = theme->font_body;
    text.style.text_color = theme->text;
    vkr_ui_label(ui, string8_lit("choice"),
                 details_choice_text(ui, property, (uint32_t)current), &text);
    *y += DETAILS_ROW_PT + 2.0f;
    return;
  }
  if (count > 3u) {
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("dropdown"));
    if (details->choice_picked && details->choice_field == id) {
      (void)vkr_property_set_number(property, build->value,
                                    (float64_t)details->choice_pick);
      details->choice_picked = false_v;
    }
    VkrUiWidgetConfig button = vkr_editor_details_widget(
        left, *y + 1.0f, available, DETAILS_ROW_PT - 4.0f);
    vkr_editor_field_style(&button);
    button.style.font_size_pt = theme->font_body;
    button.style.padding_pt = (VkrUiEdges){2, 8, 2, 8};
    button.leading = true_v;
    button.trailing_icon = VKR_UI_ICON_CHEVRON_DOWN;
    button.tooltip = tooltip;
    if (vkr_ui_button(ui, string8_lit("dropdown"),
                      details_choice_text(ui, property, (uint32_t)current),
                      &button)) {
      VkrUiRect rect = {0};
      (void)vkr_ui_widget_rect(ui, id, &rect);
      details->choice_requested = true_v;
      details->choice_field = id;
      details->choice_property = property;
      details->choice_current = (uint32_t)current;
      details->choice_anchor_pt =
          (Vec2){rect.x / ui->content_scale,
                 (rect.y + rect.height) / ui->content_scale};
    }
    *y += DETAILS_ROW_PT + 2.0f;
    return;
  }
  const float32_t cell = available / (float32_t)count;
  for (uint32_t i = 0; i < count; ++i) {
    const bool8_t selected = (uint32_t)current == i;
    VkrUiWidgetConfig button =
        vkr_editor_details_widget(left + cell * (float32_t)i, *y + 1.0f,
                                  cell - 2.0f, DETAILS_ROW_PT - 4.0f);
    vkr_editor_action_style(&button, button.text.font);
    button.style.font_size_pt = theme->font_body;
    button.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
    button.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
    button.style.border_color = selected ? theme->accent : theme->border;
    button.style.background_color =
        selected ? vkr_ui_color_alpha(theme->accent, 0.22f) : theme->raised;
    button.tooltip = tooltip;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("option"),
                      details_choice_text(ui, property, i), &button) &&
        !selected) {
      (void)vkr_property_set_number(property, build->value, (float64_t)i);
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += DETAILS_ROW_PT + 2.0f;
}

/* Multi-line text area under its label, growing from three to eight lines.
 * Each keystroke applies to the value, in one gesture while the area keeps
 * focus; Escape restores the text the entry started from. */
static void details_text_area(DetailsBuild *build, uint32_t index,
                              String8 label, float32_t *y, bool8_t read_only,
                              String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = build->ui;
  VkrEditorDetails *details = build->details;
  const VkrPropertyDesc *property = &build->type->properties[index];
  char *target = (char *)build->value + property->offset;
  const float32_t w = build->width;
  details_label(build, index, 0u, label, *y, w - DETAILS_PAD_PT * 2.0f, false_v,
                tooltip);
  *y += DETAILS_ROW_PT;

  const VkrUiId id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("area"));
  bool8_t focused = ui->focused_id == id && !read_only;
  if (details->edit_field == id && !focused) {
    details->edit_field = 0;
  }
  if (focused && details->edit_field == id &&
      input_key_just_pressed(build->input, KEY_ESCAPE)) {
    MemCopy(target, details->edit_original,
            strlen(details->edit_original) + 1u);
    (void)details_gesture(details, id);
    details->edit_field = 0;
    ui->focused_id = 0;
    focused = false_v;
  }
  if (focused && details->edit_field != id) {
    details->edit_field = id;
    details_edit_text_reset(build, index, 0u);
  }

  char display[VKR_EDITOR_DETAILS_TEXT_CAPACITY];
  VkrUiTextEditBuffer buffer;
  if (details->edit_field == id) {
    buffer = (VkrUiTextEditBuffer){(uint8_t *)details->edit_text,
                                   (uint32_t)strlen(details->edit_text),
                                   sizeof(details->edit_text)};
  } else {
    snprintf(display, sizeof(display), "%s", target);
    buffer = (VkrUiTextEditBuffer){(uint8_t *)display,
                                   (uint32_t)strlen(display), sizeof(display)};
  }
  uint32_t lines = 1u;
  for (uint32_t i = 0; i < buffer.length; ++i) {
    lines += buffer.data[i] == '\n' ? 1u : 0u;
  }
  float32_t line_pt =
      vkr_ui_code_cell_size(ui, VKR_FONT_HANDLE_INVALID, theme->font_body).y;
  if (!(line_pt > 0.0f)) {
    line_pt = theme->font_body * 1.25f;
  }
  const float32_t height = (float32_t)Clamp(lines, 3u, 8u) * line_pt + 12.0f;
  VkrUiWidgetConfig config = vkr_editor_details_widget(
      DETAILS_PAD_PT, *y, w - DETAILS_PAD_PT * 2.0f, height);
  config.style.padding_pt = (VkrUiEdges){5, 6, 5, 6};
  config.style.font_size_pt = theme->font_body;
  config.read_only = read_only;
  config.multiline = true_v;
  config.tooltip = tooltip;
  vkr_editor_field_style(&config);
  const bool8_t typed =
      vkr_ui_text_field(ui, string8_lit("area"), &buffer, &config);
  if (details->edit_field == id && ui->focused_id == id) {
    build->focused = true_v;
    (void)details_gesture(details, id);
    if (typed) {
      if (buffer.length < property->capacity) {
        MemCopy(target, details->edit_text, buffer.length + 1u);
      } else {
        snprintf(details->error, sizeof(details->error),
                 "%s is longer than %u bytes.", property->label,
                 property->capacity - 1u);
      }
    }
  }
  *y += height + 4.0f;
}

static void details_string_row(DetailsBuild *build, uint32_t index,
                               String8 label, float32_t *y, bool8_t read_only,
                               String8 tooltip) {
  if (build->type->properties[index].flags & VKR_PROPERTY_FLAG_MULTILINE) {
    details_text_area(build, index, label, y, read_only, tooltip);
    return;
  }
  const float32_t w = build->width;
  const float32_t label_w = vkr_editor_details_label_width(w);
  const float32_t left = DETAILS_PAD_PT + label_w;
  details_label(build, index, 0u, label, *y, label_w - 4.0f, false_v, tooltip);
  details_text_field(build, index, 0u, left, *y,
                     Max(60.0f, w - left - DETAILS_PAD_PT), read_only);
  *y += DETAILS_ROW_PT + 2.0f;
}

static void details_group(DetailsBuild *build, const char *group,
                          float32_t *y) {
  const VkrUiTheme *theme = vkr_ui_theme();
  *y += 6.0f;
  VkrUiWidgetConfig config = vkr_editor_details_widget(
      DETAILS_PAD_PT, *y, build->width - DETAILS_PAD_PT * 2.0f, 20.0f);
  config.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
  config.style.font_size_pt = theme->font_caption;
  config.style.text_color = theme->text_secondary;
  config.style.border_pt = (VkrUiEdges){0, 0, 1, 0};
  config.style.border_color = theme->separator;
  config.text.letter_spacing = 0.4f;
  vkr_ui_label(build->ui, string8_lit("group"), details_cstr(group), &config);
  *y += 24.0f;
}

// =============================================================================
// Types
// =============================================================================

VkrEditorDetailsResult
vkr_editor_details_type(VkrEditorDetails *details, VkrUiSystem *ui,
                        InputState *input, float32_t width, float32_t *y,
                        const VkrTypeDesc *type, void *value,
                        const void *context, bool8_t read_only) {
  VkrEditorDetailsResult result = {0};
  if (!details || !ui || !type || !value || type->size > VKR_TYPE_VALUE_MAX) {
    return result;
  }
  uint8_t before[VKR_TYPE_VALUE_MAX];
  MemCopy(before, value, type->size);
  DetailsBuild build = {.details = details,
                        .ui = ui,
                        .input = input,
                        .type = type,
                        .value = value,
                        .width = width};
  (void)vkr_ui_push_id_label(ui, details_cstr(type->name));
  for (uint32_t i = 0; i < type->property_count; ++i) {
    const VkrPropertyDesc *property = &type->properties[i];
    if (property->flags & VKR_PROPERTY_FLAG_HIDDEN) {
      continue;
    }
    const VkrPropertyState state =
        vkr_type_property_state(type, value, i, context);
    if (state.flags & VKR_PROPERTY_STATE_HIDDEN) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    if (property->group) {
      details_group(&build, property->group, y);
    }
    const bool8_t row_read_only =
        read_only ||
        (property->flags &
         (VKR_PROPERTY_FLAG_READ_ONLY | VKR_PROPERTY_FLAG_TRANSIENT)) ||
        (state.flags & VKR_PROPERTY_STATE_DISABLED);
    const String8 label =
        state.label.length ? state.label : details_cstr(property->label);
    const String8 tooltip = details_cstr(property->tooltip);
    switch (property->kind) {
    case VKR_PROPERTY_BOOL:
      details_bool_row(&build, i, label, y, row_read_only, tooltip);
      break;
    case VKR_PROPERTY_ENUM:
      details_choice_row(&build, i, label, y, row_read_only, tooltip);
      break;
    case VKR_PROPERTY_U32:
      if (vkr_property_enum_count(property)) {
        details_choice_row(&build, i, label, y, row_read_only, tooltip);
      } else {
        (void)details_scalar_row(&build, i, 0u, label, y, row_read_only,
                                 tooltip);
      }
      break;
    case VKR_PROPERTY_I32:
    case VKR_PROPERTY_F32:
    case VKR_PROPERTY_ANGLE:
      (void)details_scalar_row(&build, i, 0u, label, y, row_read_only, tooltip);
      break;
    case VKR_PROPERTY_DIRECTION:
      (void)details_scalar_row(&build, i, 0u, string8_lit("Yaw"), y,
                               row_read_only, tooltip);
      (void)details_scalar_row(&build, i, 1u, string8_lit("Elevation"), y,
                               row_read_only, tooltip);
      break;
    case VKR_PROPERTY_STRING:
      details_string_row(&build, i, label, y, row_read_only, tooltip);
      break;
    default:
      details_vector_row(&build, i, label, y, row_read_only, tooltip);
      break;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_pop_id(ui);

  result.focused = build.focused && !ui->mouse_captured &&
                   ui->keyboard_input_layer == ui->input_layer;
  if (MemCompare(before, value, type->size) != 0) {
    if (type->normalize) {
      type->normalize(value);
    }
    char error[sizeof(details->error)] = {0};
    if (!vkr_type_validate(type, value, error, sizeof(error))) {
      MemCopy(value, before, type->size);
      snprintf(details->error, sizeof(details->error), "%s", error);
    } else {
      details->error[0] = 0;
      result.changed = true_v;
      result.gesture = details->gesture_active ? details->gesture : 0u;
    }
  }
  return result;
}

bool8_t vkr_editor_details_section(VkrUiSystem *ui, String8 id, float32_t width,
                                   float32_t *y, VkrUiIcon icon,
                                   Vec4 icon_color, String8 title,
                                   VkrFontHandle heading, bool8_t *collapsed) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t expanded = !*collapsed;
  *y += VKR_EDITOR_DETAILS_SECTION_GAP_PT;
  const float32_t bar = VKR_EDITOR_DETAILS_SECTION_PT;
  VkrUiWidgetConfig header = vkr_editor_details_widget(0, *y, width, bar);
  header.style.background_color = theme->header;
  header.style.hover_background_color = theme->raised;
  header.style.border_pt = (VkrUiEdges){1, 0, 1, 0};
  header.style.border_color = theme->separator;
  header.tooltip = expanded ? string8_lit("Collapse section")
                            : string8_lit("Expand section");
  (void)vkr_ui_push_id_label(ui, id);
  if (vkr_ui_button(ui, string8_lit("section"), (String8){0}, &header)) {
    *collapsed = expanded;
  }
  VkrUiWidgetConfig caret =
      vkr_editor_details_widget(DETAILS_PAD_PT - 2.0f, *y, 14.0f, bar);
  caret.placement.align = VKR_UI_ALIGN_START;
  caret.style.padding_pt = (VkrUiEdges){9, 0, 9, 0};
  caret.icon =
      expanded ? VKR_UI_ICON_DISCLOSURE_OPEN : VKR_UI_ICON_DISCLOSURE_CLOSED;
  caret.icon_size_pt = 10.0f;
  caret.icon_color = theme->text_secondary;
  vkr_ui_label(ui, string8_lit("caret"), (String8){0}, &caret);
  /* The title leaves room for header actions on the right. */
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      DETAILS_PAD_PT + 14.0f, *y, width - DETAILS_PAD_PT - 74.0f, bar);
  label.placement.align = VKR_UI_ALIGN_START;
  label.style.padding_pt = (VkrUiEdges){6, 4, 6, 2};
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text;
  label.text.font = heading;
  label.icon = icon;
  label.icon_size_pt = 14.0f;
  label.icon_color = icon_color;
  vkr_ui_label(ui, string8_lit("title"), title, &label);
  (void)vkr_ui_pop_id(ui);
  *y += bar + 4.0f;
  return expanded;
}

bool8_t vkr_editor_details_section_action(VkrUiSystem *ui, String8 id,
                                          float32_t right, float32_t section_y,
                                          VkrUiIcon icon, String8 tooltip) {
  const float32_t size = VKR_EDITOR_DETAILS_ACTION_PT;
  VkrUiWidgetConfig action = vkr_editor_details_widget(
      right - size,
      section_y + VKR_EDITOR_DETAILS_SECTION_GAP_PT +
          (VKR_EDITOR_DETAILS_SECTION_PT - size) * 0.5f,
      size, size);
  vkr_editor_ghost_style(&action);
  action.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
  action.icon = icon;
  action.icon_size_pt = 13.0f;
  action.icon_color = vkr_ui_theme()->text_secondary;
  action.tooltip = tooltip;
  return vkr_ui_button(ui, id, (String8){0}, &action);
}

void vkr_editor_details_error(VkrEditorDetails *details, VkrUiSystem *ui,
                              float32_t width, float32_t *y) {
  if (!details->error[0]) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig config = vkr_editor_details_widget(
      DETAILS_PAD_PT, *y, width - DETAILS_PAD_PT * 2.0f, 36.0f);
  config.style.background_color = vkr_ui_color_alpha(theme->error, 0.14f);
  config.style.corner_radius_pt = (Vec4){5, 5, 5, 5};
  config.style.padding_pt = (VkrUiEdges){5, 8, 5, 8};
  config.style.text_color = theme->text;
  config.style.font_size_pt = theme->font_caption;
  config.text.layout.word_wrap = true_v;
  config.text.layout.max_width = Max(1.0f, width - 60.0f);
  config.icon = VKR_UI_ICON_WARNING_FILL;
  config.icon_size_pt = 14.0f;
  config.icon_color = theme->error;
  vkr_ui_label(ui, string8_lit("details.error"), details_cstr(details->error),
               &config);
  *y += 42.0f;
}
