#include "editor_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* These controls own editor interaction only. The runtime validates and applies
 * camera/render requests; grid geometry follows its final unjittered camera. */
enum {
  VIEW_POPUP_NONE,
  VIEW_POPUP_CAMERA,
  VIEW_POPUP_RENDER,
  VIEW_POPUP_GRID,
  VIEW_POPUP_OVERFLOW,
};

static const char *const view_camera_names[] = {"Perspective", "Top", "Left",
                                                "Right", "Bottom"};

static const struct {
  const char *name;
  VkrRenderMode mode;
} view_render_modes[] = {
    {"Lit", VKR_RENDER_MODE_DEFAULT},
    {"Unlit", VKR_RENDER_MODE_UNLIT},
    {"Detail lighting", VKR_RENDER_MODE_DETAIL_LIGHTING},
    {"Lighting only", VKR_RENDER_MODE_LIGHTING_ONLY},
    {"Wireframe", VKR_RENDER_MODE_WIREFRAME},
};

static String8 view_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

static const char *view_render_name(VkrRenderMode mode) {
  for (uint32_t i = 0; i < ArrayCount(view_render_modes); ++i) {
    if (view_render_modes[i].mode == mode) {
      return view_render_modes[i].name;
    }
  }
  return "Diagnostic";
}

static Vec2 view_text_size(const VkrUiSystem *ui, VkrFontHandle font_handle,
                           const char *text, float32_t size) {
  VkrFont *font = vkr_font_system_get_by_handle(ui->fonts, font_handle);
  if (!font) {
    font = vkr_font_system_get_default_mtsdf_font(ui->fonts);
  }
  if (!font) {
    font = vkr_font_system_get_default_bitmap_font(ui->fonts);
  }
  VkrTextStyle style =
      vkr_text_style_new(font_handle, size, (Vec4){1, 1, 1, 1});
  style.font_data = font;
  const VkrText value = vkr_text_from_view(view_string(text), &style);
  return vkr_text_measure(&value).size;
}

static Vec2 view_button_size(const VkrEditorUi *editor, const VkrUiSystem *ui,
                             const char *text) {
  const Vec2 size = view_text_size(ui, editor->heading_font, text, 11.0f);
  return (Vec2){ceilf(size.x) + 18.0f, ceilf(size.y) + 12.0f};
}

static bool8_t view_contains(Vec4 rect, Vec2 point) {
  return rect.z > 0 && rect.w > 0 && point.x >= rect.x && point.y >= rect.y &&
         point.x < rect.x + rect.z && point.y < rect.y + rect.w;
}

static Vec4 view_scene_rect(const VkrSampleUiFrame *frame) {
  const float32_t scale = frame->ui->content_scale;
  const float32_t top = frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0;
  const Vec4 scene = frame->mapping.panel_rect_px;
  return (Vec4){scene.x / scale, scene.y / scale + top, scene.z / scale,
                Max(0.0f, scene.w / scale - top)};
}

/* The grid button reports the drawn cell size, which zoom may coarsen above the
 * requested size so every visible cell keeps a readable label. */
static void view_control_text(const VkrSampleViewState *state,
                              float32_t drawn_spacing, char text[3][80]) {
  const uint32_t camera = (uint32_t)state->camera_view;
  snprintf(text[0], 80, "%s v",
           camera < ArrayCount(view_camera_names) ? view_camera_names[camera]
                                                  : "Perspective");
  snprintf(text[1], 80, "%s v", view_render_name(state->render_mode));
  snprintf(text[2], 80, state->grid_enabled ? "Grid %.4g u v" : "Grid off v",
           (double)(drawn_spacing > 0 ? drawn_spacing : state->grid_spacing));
}

static uint32_t view_popup_items(uint32_t popup,
                                 const VkrSampleViewState *state,
                                 float32_t drawn_spacing, char text[5][80]) {
  switch (popup) {
  case VIEW_POPUP_CAMERA:
    for (uint32_t i = 0; i < ArrayCount(view_camera_names); ++i) {
      snprintf(text[i], 80, "%s%s", view_camera_names[i],
               i ? " (orthographic)" : "");
    }
    return ArrayCount(view_camera_names);
  case VIEW_POPUP_RENDER:
    for (uint32_t i = 0; i < ArrayCount(view_render_modes); ++i) {
      snprintf(text[i], 80, "%s", view_render_modes[i].name);
    }
    return ArrayCount(view_render_modes);
  case VIEW_POPUP_GRID:
    snprintf(text[0], 80, "Grid %s", state->grid_enabled ? "on" : "off");
    snprintf(text[1], 80, "Smaller cells (%.4g u)",
             (double)Max(0.001f, state->grid_spacing * 0.5f));
    snprintf(text[2], 80, "Larger cells (%.4g u)",
             (double)Min(10000.0f, state->grid_spacing * 2.0f));
    return 3;
  case VIEW_POPUP_OVERFLOW: {
    char controls[3][80];
    view_control_text(state, drawn_spacing, controls);
    snprintf(text[0], 80, "Camera: %.68s", controls[0]);
    snprintf(text[1], 80, "View: %.70s", controls[1]);
    snprintf(text[2], 80, "%.79s", controls[2]);
    return 3;
  }
  default:
    return 0;
  }
}

static void view_popup_layout(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  char text[5][80];
  const uint32_t count = view_popup_items(
      editor->view_popup, &frame->view_state, editor->grid_spacing, text);
  if (!count) {
    editor->view_popup_rect_pt = (Vec4){0};
    return;
  }
  Vec2 size = {8, 8};
  for (uint32_t i = 0; i < count; ++i) {
    const Vec2 button = view_button_size(editor, frame->ui, text[i]);
    size.x = Max(size.x, button.x + 8);
    size.y += button.y + (i ? 2 : 0);
  }
  /* Dropdowns may extend over docked panels. Clamping to a short Scene would
   * clip the lower camera/render choices, making them impossible to select. */
  const float32_t scale = frame->ui->content_scale;
  const Vec4 bounds = {0, VKR_EDITOR_NAVIGATION_HEIGHT_PT,
                       (float32_t)frame->ui->target_width / scale,
                       Max(0.0f, (float32_t)frame->ui->target_height / scale -
                                     VKR_EDITOR_NAVIGATION_HEIGHT_PT)};
  const Vec4 toolbar = editor->view_toolbar_rect_pt;
  const float32_t x = vkr_clamp_f32(
      toolbar.x, bounds.x, Max(bounds.x, bounds.x + bounds.z - size.x));
  float32_t y = toolbar.y + toolbar.w + 4;
  if (y + size.y > bounds.y + bounds.w) {
    y = Max(bounds.y, toolbar.y - size.y - 4);
  }
  editor->view_popup_rect_pt =
      (Vec4){x, y, Min(size.x, bounds.z), Min(size.y, bounds.w)};
}

static void view_register_rect(VkrUiSystem *ui, Vec4 rect) {
  const float32_t scale = ui->content_scale;
  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
                                    (VkrUiRect){rect.x * scale, rect.y * scale,
                                                rect.z * scale,
                                                rect.w * scale});
}

void vkr_editor_viewport_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->mapping_valid) {
    editor->view_toolbar_dragging = false_v;
    editor->view_toolbar_rect_pt = (Vec4){0};
    editor->view_popup = VIEW_POPUP_NONE;
    return;
  }
  const Vec4 scene = view_scene_rect(frame);
  char text[3][80];
  view_control_text(&frame->view_state, editor->grid_spacing, text);
  Vec2 size = {30, 30};
  for (uint32_t i = 0; i < ArrayCount(text); ++i) {
    const Vec2 button = view_button_size(editor, ui, text[i]);
    size.x += button.x + 4;
    size.y = Max(size.y, button.y + 8);
  }
  const float32_t available = Max(0.0f, scene.z - 16);
  editor->view_toolbar_overflow = size.x > available;
  if (editor->view_toolbar_overflow) {
    const Vec2 button = view_button_size(editor, ui, "Viewport v");
    size.x = 34 + button.x;
    size.y = Max(30.0f, button.y + 8);
  }
  size.x = Min(size.x, available);
  size.y = Min(size.y, Max(0.0f, scene.w - 16));
  const Vec2 limit = {Max(0.0f, scene.z - size.x - 16),
                      Max(0.0f, scene.w - size.y - 16)};
  if (!editor->view_toolbar_initialized) {
    editor->view_toolbar_offset_pt = (Vec2){0, 46};
    editor->view_toolbar_initialized = true_v;
  }
  const float32_t scale = ui->content_scale;
  bool8_t gesture = editor->view_toolbar_dragging;
  if (frame->mouse_captured || editor->commands_open ||
      editor->menu != VKR_EDITOR_MENU_NONE) {
    editor->view_toolbar_dragging = false_v;
    editor->view_popup = VIEW_POPUP_NONE;
  } else if (ui->mouse_pressed) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(frame->input, BUTTON_LEFT, &press_x,
                                    &press_y);
    const Vec2 press = {(float32_t)press_x / scale, (float32_t)press_y / scale};
    const Vec4 previous = editor->view_toolbar_rect_pt;
    const Vec4 grip = {previous.x + 4, previous.y + 4, 22, previous.w - 8};
    if (ui->mouse_input_layer <= VKR_EDITOR_VIEW_TOOLBAR_LAYER &&
        view_contains(grip, press) && !editor->toolbar_dragging) {
      editor->view_toolbar_grab_pt =
          (Vec2){press.x - previous.x, press.y - previous.y};
      editor->view_toolbar_dragging = true_v;
      gesture = true_v;
      editor->view_popup = VIEW_POPUP_NONE;
    } else if (!view_contains(previous, press) &&
               !view_contains(editor->view_popup_rect_pt, press)) {
      editor->view_popup = VIEW_POPUP_NONE;
    }
  }
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->view_popup = VIEW_POPUP_NONE;
  }
  if (editor->view_toolbar_dragging) {
    editor->view_toolbar_offset_pt =
        (Vec2){(float32_t)ui->mouse_x / scale - editor->view_toolbar_grab_pt.x -
                   scene.x - 8,
               (float32_t)ui->mouse_y / scale - editor->view_toolbar_grab_pt.y -
                   scene.y - 8};
    if (!input_is_button_down(frame->input, BUTTON_LEFT)) {
      editor->view_toolbar_dragging = false_v;
    }
  }
  editor->view_toolbar_offset_pt.x =
      vkr_clamp_f32(editor->view_toolbar_offset_pt.x, 0, limit.x);
  editor->view_toolbar_offset_pt.y =
      vkr_clamp_f32(editor->view_toolbar_offset_pt.y, 0, limit.y);
  editor->view_toolbar_rect_pt =
      (Vec4){scene.x + 8 + editor->view_toolbar_offset_pt.x,
             scene.y + 8 + editor->view_toolbar_offset_pt.y, size.x, size.y};
  view_popup_layout(editor, frame);
  view_register_rect(ui, editor->view_toolbar_rect_pt);
  view_register_rect(ui, editor->view_popup_rect_pt);
  if (gesture) {
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){0, 0, (float32_t)ui->target_width,
                    (float32_t)ui->target_height});
  }
}

static VkrUiPanelConfig view_panel(Vec4 rect) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.placement.justify = VKR_UI_ALIGN_START;
  panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){rect.z, rect.w};
  panel.style.gap_pt = 0;
  panel.style.background_color.w = 0.96f;
  panel.clip_children = true_v;
  return panel;
}

static bool8_t view_button(VkrEditorUi *editor, VkrUiSystem *ui, const char *id,
                           const char *text, Vec2 position, bool8_t active,
                           bool8_t disabled, const char *tooltip) {
  VkrUiWidgetConfig button =
      vkr_editor_menu_button_config(0, active, editor->heading_font);
  button.placement.justify = VKR_UI_ALIGN_START;
  button.placement.align = VKR_UI_ALIGN_START;
  button.placement.margin_pt = (VkrUiEdges){position.y, 0, 0, position.x};
  button.style.font_size_pt = 11;
  button.style.padding_pt = (VkrUiEdges){5, 8, 5, 8};
  button.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  button.style.border_color =
      active ? (Vec4){0.30f, 0.55f, 0.72f, 1} : (Vec4){0.19f, 0.23f, 0.29f, 1};
  button.disabled = disabled;
  button.tooltip = view_string(tooltip);
  return vkr_ui_button(ui, view_string(id), view_string(text), &button);
}

void vkr_editor_viewport_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  const Vec4 rect = editor->view_toolbar_rect_pt;
  if (rect.z <= 0 || rect.w <= 0) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  VkrUiPanelConfig panel = view_panel(rect);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.viewport.toolbar"), &panel)) {
    (void)vkr_ui_input_layer_set(ui, 0);
    return;
  }
  VkrUiWidgetConfig grip = vkr_ui_widget_config_default();
  grip.placement.column = 0;
  grip.placement.row = 0;
  grip.placement.justify = VKR_UI_ALIGN_START;
  grip.placement.align = VKR_UI_ALIGN_START;
  grip.style.min_size_pt = grip.style.max_size_pt = (Vec2){22, rect.w - 8};
  grip.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
  grip.icon = VKR_UI_ICON_GRIP;
  grip.icon_size_pt = 16;
  grip.tooltip = string8_lit("Drag viewport controls");
  (void)vkr_ui_button(ui, string8_lit("grip"), (String8){0}, &grip);
  const bool8_t disabled =
      !frame->view_request || frame->scene_rendering_stopped;
  if (editor->view_toolbar_overflow) {
    /* An open dropdown replaces its button tooltip, which would cover it. */
    if (view_button(editor, ui, "overflow", "Viewport v", (Vec2){26, 0},
                    editor->view_popup != VIEW_POPUP_NONE, disabled,
                    editor->view_popup != VIEW_POPUP_NONE
                        ? ""
                        : "Camera, view mode and grid controls")) {
      editor->view_popup =
          editor->view_popup ? VIEW_POPUP_NONE : VIEW_POPUP_OVERFLOW;
    }
  } else {
    char text[3][80];
    view_control_text(&frame->view_state, editor->grid_spacing, text);
    const char *ids[] = {"camera", "render", "grid"};
    const char *tips[] = {
        "Camera projection and orthographic direction",
        "Viewport rendering mode",
        "World grid: numbered columns, lettered rows and cell size"};
    float32_t x = 26;
    for (uint32_t i = 0; i < ArrayCount(text); ++i) {
      const uint32_t popup = i + VIEW_POPUP_CAMERA;
      if (view_button(
              editor, ui, ids[i], text[i], (Vec2){x, 0},
              editor->view_popup == popup ||
                  (popup == VIEW_POPUP_GRID && frame->view_state.grid_enabled),
              disabled, editor->view_popup == popup ? "" : tips[i])) {
        editor->view_popup =
            editor->view_popup == popup ? VIEW_POPUP_NONE : popup;
      }
      x += view_button_size(editor, ui, text[i]).x + 4;
    }
  }
  (void)vkr_ui_panel_end(ui);
  view_popup_layout(editor, frame);
  const uint32_t popup = editor->view_popup;
  if (popup) {
    view_register_rect(ui, editor->view_popup_rect_pt);
    panel = view_panel(editor->view_popup_rect_pt);
    if (vkr_ui_panel_begin(ui, string8_lit("editor.viewport.options"),
                           &panel)) {
      char text[5][80];
      const uint32_t count = view_popup_items(popup, &frame->view_state,
                                              editor->grid_spacing, text);
      float32_t y = 0;
      VkrSampleViewState next = frame->view_state;
      for (uint32_t i = 0; i < count; ++i) {
        const bool8_t active =
            popup == VIEW_POPUP_CAMERA ? (uint32_t)next.camera_view == i
            : popup == VIEW_POPUP_RENDER
                ? next.render_mode == view_render_modes[i].mode
            : popup == VIEW_POPUP_GRID && i == 0 ? next.grid_enabled
                                                 : false_v;
        (void)vkr_ui_push_id_u64(ui, (uint64_t)popup * 8 + i);
        if (view_button(editor, ui, "option", text[i], (Vec2){0, y}, active,
                        disabled, "")) {
          if (popup == VIEW_POPUP_OVERFLOW) {
            editor->view_popup = i + VIEW_POPUP_CAMERA;
          } else {
            if (popup == VIEW_POPUP_CAMERA) {
              next.camera_view = (VkrSampleCameraView)i;
            } else if (popup == VIEW_POPUP_RENDER) {
              next.render_mode = view_render_modes[i].mode;
            } else if (i == 0) {
              next.grid_enabled = !next.grid_enabled;
            } else {
              next.grid_spacing = vkr_clamp_f32(
                  next.grid_spacing * (i == 1 ? 0.5f : 2.0f), 0.001f, 10000.0f);
              next.grid_enabled = true_v;
            }
            if (frame->view_request) {
              *frame->view_request =
                  (VkrSampleViewRequest){.value = next, .apply = true_v};
            }
            if (popup != VIEW_POPUP_GRID) {
              editor->view_popup = VIEW_POPUP_NONE;
            }
          }
        }
        (void)vkr_ui_pop_id(ui);
        y += view_button_size(editor, ui, text[i]).y + 2;
      }
      (void)vkr_ui_panel_end(ui);
    }
  }
  (void)vkr_ui_input_layer_set(ui, 0);
}

/* Solve the chosen plane's projective mapping directly. A ray parallel to the
 * plane, including the perspective horizon, has no unique intersection. */
static bool8_t grid_plane_point(const VkrSampleUiFrame *frame, Vec2 ndc,
                                bool8_t side, Vec2 *point) {
  const Mat4 matrix = frame->view_projection;
  const Vec4 origin = mat4_mul_vec4(matrix, (Vec4){0, 0, 0, 1});
  const Vec4 u =
      mat4_mul_vec4(matrix, side ? (Vec4){0, 0, 1, 0} : (Vec4){1, 0, 0, 0});
  const Vec4 v =
      mat4_mul_vec4(matrix, side ? (Vec4){0, 1, 0, 0} : (Vec4){0, 0, 1, 0});
  const float64_t a = u.x - ndc.x * u.w;
  const float64_t b = v.x - ndc.x * v.w;
  const float64_t c = u.y - ndc.y * u.w;
  const float64_t d = v.y - ndc.y * v.w;
  const float64_t x = ndc.x * origin.w - origin.x;
  const float64_t y = ndc.y * origin.w - origin.y;
  const float64_t determinant = a * d - b * c;
  if (!isfinite(determinant) ||
      fabs(determinant) <= 1.0e-8 * (fabs(a * d) + fabs(b * c))) {
    return false_v;
  }
  *point = (Vec2){(float32_t)((d * x - b * y) / determinant),
                  (float32_t)((a * y - c * x) / determinant)};
  return isfinite(point->x) && isfinite(point->y);
}

static Vec3 grid_world(Vec2 point, bool8_t side) {
  return side ? (Vec3){0, point.y, point.x} : (Vec3){point.x, 0, point.y};
}

static bool8_t grid_screen(const VkrSampleUiFrame *frame, Vec3 world,
                           Vec2 *point) {
  const Vec4 clip =
      mat4_mul_vec4(frame->view_projection, vec3_to_vec4(world, 1));
  if (!isfinite(clip.w) || clip.w <= 0.000001f) {
    return false_v;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  *point = (Vec2){(clip.x / clip.w * 0.5f + 0.5f) * image.z / scale,
                  (clip.y / clip.w * 0.5f + 0.5f) * image.w / scale};
  return isfinite(point->x) && isfinite(point->y);
}

static float32_t grid_clip_distance(Vec4 point, uint32_t plane) {
  switch (plane) {
  case 0:
    return point.w + point.x;
  case 1:
    return point.w - point.x;
  case 2:
    return point.w + point.y;
  case 3:
    return point.w - point.y;
  case 4:
    return point.z;
  default:
    return point.w - point.z;
  }
}

static bool8_t grid_project_line(const VkrSampleUiFrame *frame, Vec3 from,
                                 Vec3 to, Vec2 *start, Vec2 *end) {
  Vec4 a = mat4_mul_vec4(frame->view_projection, vec3_to_vec4(from, 1));
  Vec4 b = mat4_mul_vec4(frame->view_projection, vec3_to_vec4(to, 1));
  for (uint32_t plane = 0; plane < 6; ++plane) {
    const float32_t da = grid_clip_distance(a, plane);
    const float32_t db = grid_clip_distance(b, plane);
    if (!isfinite(da) || !isfinite(db) || (da < 0 && db < 0)) {
      return false_v;
    }
    if ((da < 0) != (db < 0)) {
      const Vec4 intersection =
          vec4_add(a, vec4_scale(vec4_sub(b, a), da / (da - db)));
      if (da < 0) {
        a = intersection;
      } else {
        b = intersection;
      }
    }
  }
  if (a.w <= 0.000001f || b.w <= 0.000001f) {
    return false_v;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  *start = (Vec2){(a.x / a.w * 0.5f + 0.5f) * image.z / scale,
                  (a.y / a.w * 0.5f + 0.5f) * image.w / scale};
  *end = (Vec2){(b.x / b.w * 0.5f + 0.5f) * image.z / scale,
                (b.y / b.w * 0.5f + 0.5f) * image.w / scale};
  return true_v;
}

/* Letters use bijective base 26 (Z, AA, ZZ, AAA), so every positive ordinal
 * has exactly one label and denser grids simply use longer labels. */
static void grid_label_text(char text[24], uint32_t ordinal, bool8_t number) {
  if (number) {
    snprintf(text, 24, "%u", ordinal);
    return;
  }
  char reverse[16];
  uint32_t length = 0;
  while (ordinal && length < ArrayCount(reverse)) {
    --ordinal;
    reverse[length++] = (char)('A' + ordinal % 26);
    ordinal /= 26;
  }
  uint32_t out = 0;
  while (length) {
    text[out++] = reverse[--length];
  }
  text[out] = 0;
}

static Vec2 grid_label_size(const VkrEditorUi *editor, const VkrUiSystem *ui,
                            uint32_t ordinal, bool8_t number) {
  char text[24];
  grid_label_text(text, Max(1u, ordinal), number);
  const Vec2 measured = view_text_size(ui, editor->heading_font, text, 10);
  return (Vec2){ceilf(measured.x) + 8, ceilf(measured.y) + 4};
}

/* Places a label where its cell-center line meets the top or right image edge.
 * A perspective line can end before that edge; its label then stays at the
 * segment end nearest the edge. Each axis stays out of the other's reserved
 * strip, so the top-right corner never holds two labels. */
static bool8_t grid_label_rect(const VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               const VkrEditorGridLine *line,
                               VkrUiRect *out_rect, float32_t *out_order) {
  Vec2 a;
  Vec2 b;
  if (!grid_project_line(frame, vec3_add(line->from, line->label_offset),
                         vec3_add(line->to, line->label_offset), &a, &b)) {
    return false_v;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  const Vec2 size = {image.z / scale, image.w / scale};
  const float32_t top = frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0;
  const Vec2 label = line->label_size_pt;
  const Vec2 reserved = editor->grid_reserved_pt;
  const Vec2 delta = {b.x - a.x, b.y - a.y};
  const float32_t along = line->top_label ? delta.y : delta.x;
  if (fabsf(along) <= 0.0001f) {
    return false_v;
  }
  const float32_t t =
      vkr_clamp_f32(line->top_label ? (top + 2 - a.y) / delta.y
                                    : (size.x - 2 - a.x) / delta.x,
                    0, 1);
  const Vec2 anchor = {a.x + delta.x * t, a.y + delta.y * t};
  VkrUiRect rect = {0, 0, label.x, label.y};
  if (line->top_label) {
    const float32_t right = size.x - reserved.x - 4;
    if (anchor.x < 0 || anchor.x > right || label.x > right ||
        top + 2 + label.y > size.y) {
      return false_v;
    }
    rect.x = vkr_clamp_f32(anchor.x - label.x * 0.5f, 0, right - label.x);
    rect.y =
        vkr_clamp_f32(anchor.y - label.y * 0.5f, top + 2, size.y - label.y);
    *out_order = anchor.x;
  } else {
    const float32_t low = top + reserved.y + 4;
    if (anchor.y < low || anchor.y > size.y || low + label.y > size.y ||
        label.x + 2 > size.x) {
      return false_v;
    }
    rect.x = vkr_clamp_f32(anchor.x - label.x * 0.5f, 0, size.x - label.x - 2);
    rect.y = vkr_clamp_f32(anchor.y - label.y * 0.5f, low, size.y - label.y);
    *out_order = anchor.y;
  }
  *out_rect = rect;
  return true_v;
}

void vkr_editor_grid_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  editor->grid_line_count = 0;
  editor->grid_spacing = 0;
  editor->grid_frame = frame->ui->frame_index;
  editor->grid_camera_view = frame->view_state.camera_view;
  if (!frame->mapping_valid || !frame->view_state.grid_enabled ||
      !frame->scene || frame->scene_rendering_stopped ||
      (frame->scene_backdrop_blur && *frame->scene_backdrop_blur)) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const bool8_t side =
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_LEFT ||
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_RIGHT;
  const bool8_t perspective =
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
  const float32_t base_spacing = frame->view_state.grid_spacing;
  float32_t spacing = base_spacing;
  if (!isfinite(spacing) || spacing <= 0) {
    return;
  }
  Vec2 minimum = {INFINITY, INFINITY};
  Vec2 maximum = {-INFINITY, -INFINITY};
  Vec2 center_plane;
  if (!grid_plane_point(frame, (Vec2){0, 0}, side, &center_plane)) {
    if (!perspective ||
        !grid_plane_point(frame, (Vec2){0, 0.75f}, side, &center_plane)) {
      return;
    }
  }
  Vec3 center = grid_world(center_plane, side);
  if (perspective) {
    Vec2 projected;
    if (!grid_screen(frame, center, &projected)) {
      if (!grid_plane_point(frame, (Vec2){0, 0.75f}, side, &center_plane)) {
        return;
      }
      center = grid_world(center_plane, side);
      if (!grid_screen(frame, center, &projected)) {
        return;
      }
    }
    Vec2 a;
    Vec2 b;
    if (grid_screen(frame, center, &a) &&
        grid_screen(frame, vec3_add(center, (Vec3){spacing, 0, spacing}), &b)) {
      const float32_t pixels = hypotf(b.x - a.x, b.y - a.y);
      if (pixels > 0.0001f && pixels < 20) {
        spacing *= powf(2, ceilf(log2f(20 / pixels)));
      }
    }
    const float32_t radius = spacing * 22;
    minimum = (Vec2){center.x - radius, center.z - radius};
    maximum = (Vec2){center.x + radius, center.z + radius};
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      Vec2 point;
      if (!grid_plane_point(frame, (Vec2){i & 1 ? 1 : -1, i & 2 ? 1 : -1}, side,
                            &point)) {
        return;
      }
      minimum.x = Min(minimum.x, point.x);
      minimum.y = Min(minimum.y, point.y);
      maximum.x = Max(maximum.x, point.x);
      maximum.y = Max(maximum.y, point.y);
    }
    /* Coarsen by powers of two until every visible cell can carry its own
     * label. Axis views keep the first grid axis horizontal on screen. */
    const Vec4 image = frame->mapping.image_rect_px;
    const Vec2 extent = {maximum.x - minimum.x, maximum.y - minimum.y};
    if (!(extent.x > 0 && extent.y > 0)) {
      return;
    }
    const Vec2 pt_per_unit = {image.z / ui->content_scale / extent.x,
                              image.w / ui->content_scale / extent.y};
    for (uint32_t step = 0; step < 64; ++step) {
      const float32_t columns = ceilf(extent.x / spacing) + 1;
      const float32_t rows = ceilf(extent.y / spacing) + 1;
      const Vec2 number =
          grid_label_size(editor, ui, (uint32_t)Min(columns, 1.0e9f), true_v);
      if (columns <= 44 && rows <= 44 &&
          spacing * pt_per_unit.x >= number.x + 6 &&
          spacing * pt_per_unit.y >= number.y + 4) {
        break;
      }
      spacing *= 2;
    }
  }
  if (!isfinite(spacing) || spacing <= 0 ||
      fabsf(minimum.x / spacing) > 1000000000 ||
      fabsf(minimum.y / spacing) > 1000000000 ||
      fabsf(maximum.x / spacing) > 1000000000 ||
      fabsf(maximum.y / spacing) > 1000000000) {
    return;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  VkrUiPanelConfig panel = view_panel(
      (Vec4){image.x / ui->content_scale, image.y / ui->content_scale,
             image.z / ui->content_scale, image.w / ui->content_scale});
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.border_pt = (VkrUiEdges){0};
  panel.style.background_color = (Vec4){0};
  editor->grid_panel = vkr_ui_id_stack_widget_label(
      &ui->id_stack, string8_lit("editor.world.grid"));
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.world.grid"), &panel)) {
    return;
  }
  /* Leave bounded room for physics, light labels and all interactive controls.
   */
  const uint32_t available =
      ui->frame_node_count + 320 < ui->frame_node_capacity
          ? (ui->frame_node_capacity - ui->frame_node_count - 320) / 2
          : 0;
  const uint32_t capacity = Min(VKR_EDITOR_GRID_LINE_CAPACITY, available);
  bool8_t first_axis_top = true_v;
  if (perspective) {
    Vec2 origin;
    Vec2 along_u;
    Vec2 along_v;
    if (grid_screen(frame, center, &origin) &&
        grid_screen(frame, vec3_add(center, (Vec3){spacing, 0, 0}), &along_u) &&
        grid_screen(frame, vec3_add(center, (Vec3){0, 0, spacing}), &along_v)) {
      const Vec2 u = {along_u.x - origin.x, along_u.y - origin.y};
      const Vec2 v = {along_v.x - origin.x, along_v.y - origin.y};
      first_axis_top =
          fabsf(v.y) * hypotf(u.x, u.y) >= fabsf(u.y) * hypotf(v.x, v.y);
    }
  }
  editor->grid_spacing = spacing;
  uint32_t axis_lines[2] = {0, 0};
  for (uint32_t axis = 0; axis < 2; ++axis) {
    const float32_t low = axis ? minimum.y : minimum.x;
    const float32_t high = axis ? maximum.y : maximum.x;
    const int64_t first = (int64_t)floorf(low / spacing);
    const int64_t last = (int64_t)ceilf(high / spacing);
    for (int64_t cell = first;
         cell <= last && editor->grid_line_count < capacity; ++cell) {
      const float32_t coordinate = (float32_t)cell * spacing;
      const Vec2 from =
          axis ? (Vec2){minimum.x, coordinate} : (Vec2){coordinate, minimum.y};
      const Vec2 to =
          axis ? (Vec2){maximum.x, coordinate} : (Vec2){coordinate, maximum.y};
      editor->grid_lines[editor->grid_line_count++] = (VkrEditorGridLine){
          .from = grid_world(from, side),
          .to = grid_world(to, side),
          .label_offset = grid_world(axis ? (Vec2){0, spacing * 0.5f}
                                          : (Vec2){spacing * 0.5f, 0},
                                     side),
          .top_label = axis == 0 ? first_axis_top : !first_axis_top,
          .world_axis = cell == 0,
      };
      ++axis_lines[axis];
    }
  }

  /* Labels are uniform per edge, sized for the largest possible ordinal. */
  const uint32_t top_axis = first_axis_top ? 0 : 1;
  const Vec2 top_size =
      grid_label_size(editor, ui, axis_lines[top_axis], true_v);
  const Vec2 right_size =
      grid_label_size(editor, ui, axis_lines[1 - top_axis], false_v);
  editor->grid_reserved_pt = (Vec2){right_size.x, top_size.y};
  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    VkrEditorGridLine *line = &editor->grid_lines[i];
    line->label_size_pt = line->top_label ? top_size : right_size;
  }

  /* Number the placed labels in screen order so visible cells read 1..N
   * across the top and A.. down the right, whatever the view orientation.
   * Perspective fans can crowd labels; those cells stay unlabeled. */
  VkrUiRect placed[VKR_EDITOR_GRID_LINE_CAPACITY];
  uint32_t placed_count = 0;
  for (uint32_t edge = 0; edge < 2; ++edge) {
    const bool8_t top_edge = edge == 0;
    struct {
      float32_t order;
      uint32_t index;
      VkrUiRect rect;
    } candidates[VKR_EDITOR_GRID_LINE_CAPACITY];
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
      const VkrEditorGridLine *line = &editor->grid_lines[i];
      float32_t order = 0;
      VkrUiRect rect;
      if (line->top_label != top_edge ||
          !grid_label_rect(editor, frame, line, &rect, &order)) {
        continue;
      }
      uint32_t slot = candidate_count++;
      while (slot && candidates[slot - 1].order > order) {
        candidates[slot] = candidates[slot - 1];
        --slot;
      }
      candidates[slot].order = order;
      candidates[slot].index = i;
      candidates[slot].rect = rect;
    }
    uint32_t ordinal = 0;
    for (uint32_t i = 0; i < candidate_count; ++i) {
      const VkrUiRect rect = candidates[i].rect;
      const VkrUiRect expanded = {rect.x - 3, rect.y - 2, rect.width + 6,
                                  rect.height + 4};
      bool8_t free = true_v;
      for (uint32_t j = 0; free && j < placed_count; ++j) {
        free =
            !vkr_ui_rect_has_area(vkr_ui_rect_intersect(expanded, placed[j]));
      }
      if (free) {
        placed[placed_count++] = rect;
        editor->grid_lines[candidates[i].index].ordinal = ++ordinal;
      }
    }
  }

  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    VkrEditorGridLine *line = &editor->grid_lines[i];
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
    widget.placement.column = 0;
    widget.placement.row = 0;
    widget.placement.justify = VKR_UI_ALIGN_START;
    widget.placement.align = VKR_UI_ALIGN_START;
    widget.style.min_size_pt = widget.style.max_size_pt = (Vec2){1, 1};
    widget.style.text_color = line->world_axis
                                  ? (Vec4){0.42f, 0.72f, 0.88f, 0.8f}
                                  : (Vec4){0.59f, 0.67f, 0.74f, 0.32f};
    const Vec2 hidden[4] = {{0}, {0}, {0}, {0}};
    vkr_ui_bezier(ui, string8_lit("line"), hidden,
                  line->world_axis ? 1.25f : 0.75f, &widget);
    line->widget =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("line"));
    line->label = VKR_UI_ID_NONE;
    if (line->ordinal) {
      char text[24];
      grid_label_text(text, line->ordinal, line->top_label);
      widget.style.min_size_pt = widget.style.max_size_pt = line->label_size_pt;
      widget.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
      widget.style.font_size_pt = 10;
      widget.style.text_color = (Vec4){0.84f, 0.89f, 0.94f, 1};
      widget.style.background_color = (Vec4){0.035f, 0.045f, 0.06f, 0.86f};
      widget.style.corner_radius_pt = (Vec4){2, 2, 2, 2};
      widget.text.font = editor->heading_font;
      widget.placement.margin_pt = (VkrUiEdges){100000, 0, 0, 100000};
      vkr_ui_label(ui, string8_lit("cell"), view_string(text), &widget);
      line->label =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("cell"));
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_grid_project(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  if (editor->grid_frame != frame->ui->frame_index ||
      !editor->grid_line_count) {
    return;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  (void)vkr_ui_widget_set_rect(frame->ui, editor->grid_panel,
                               (VkrUiRect){image.x / scale, image.y / scale,
                                           image.z / scale, image.w / scale});
  const bool8_t same_view =
      editor->grid_camera_view == frame->view_state.camera_view;
  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    const VkrEditorGridLine *line = &editor->grid_lines[i];
    Vec2 points[4] = {{0}, {0}, {0}, {0}};
    Vec2 a;
    Vec2 b;
    const bool8_t visible =
        same_view && grid_project_line(frame, line->from, line->to, &a, &b);
    if (visible) {
      points[0] = points[1] = a;
      points[2] = points[3] = b;
    }
    (void)vkr_ui_bezier_set_points(frame->ui, line->widget, points);
    if (line->label == VKR_UI_ID_NONE) {
      continue;
    }
    /* Build placed and numbered labels with the UI-time camera; the final
     * camera only moves them, so ordinals never change mid-frame. */
    VkrUiRect label = {100000, 100000, line->label_size_pt.x,
                       line->label_size_pt.y};
    float32_t order = 0;
    /* The label marks the cell center, which can be visible while the cell's
     * leading line lies just outside the image. */
    if (same_view) {
      (void)grid_label_rect(editor, frame, line, &label, &order);
    }
    (void)vkr_ui_widget_set_rect(frame->ui, line->label, label);
  }
}
