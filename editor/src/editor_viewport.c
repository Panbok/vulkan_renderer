#include "editor_internal.h"
#include "editor_projects.h"

#include "renderer/systems/vkr_gizmo_system.h"

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
  VIEW_POPUP_SPEED,
  VIEW_POPUP_COUNT,
};

#define VIEW_CHIP_HEIGHT_PT 26.0f
#define VIEW_POPUP_ROW_PT 26.0f
#define VIEW_POPUP_WIDTH_PT 220.0f
#define VIEW_INSET_PT 8.0f

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

static const float32_t view_camera_speeds[] = {0.25f, 0.5f, 1.0f,  2.0f,
                                               4.0f,  8.0f, 16.0f, 32.0f};

static const struct {
  const char *name;
  const char *key;
  VkrUiIcon icon;
  uint32_t mode;
} view_tools[] = {
    {"Select", "Q", VKR_UI_ICON_SELECT, VKR_GIZMO_MODE_NONE},
    {"Move", "W", VKR_UI_ICON_MOVE, VKR_GIZMO_MODE_TRANSLATE},
    {"Rotate", "E", VKR_UI_ICON_ROTATE, VKR_GIZMO_MODE_ROTATE},
    {"Scale", "R", VKR_UI_ICON_SCALE, VKR_GIZMO_MODE_SCALE},
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
    font = vkr_font_system_get_by_handle(ui->fonts, ui->default_font);
  }
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

/* Chip labels: camera, view mode, grid spacing, camera speed. */
static void view_chip_text(const VkrSampleViewState *state,
                           float32_t drawn_spacing, char text[4][48]) {
  const uint32_t camera = (uint32_t)state->camera_view;
  snprintf(text[0], 48, "%s",
           camera < ArrayCount(view_camera_names) ? view_camera_names[camera]
                                                  : "Perspective");
  snprintf(text[1], 48, "%s", view_render_name(state->render_mode));
  if (state->grid_enabled)
    snprintf(text[2], 48, "%.4g",
             (double)(drawn_spacing > 0 ? drawn_spacing : state->grid_spacing));
  else
    snprintf(text[2], 48, "Off");
  snprintf(text[3], 48, "%.3g", (double)state->camera_speed);
}

/* Label width plus leading icon, caret and padding. */
static float32_t view_chip_width(const VkrUiSystem *ui, const char *text,
                                 bool8_t compact) {
  if (compact)
    return 40.0f;
  return ceilf(view_text_size(ui, VKR_FONT_HANDLE_INVALID, text,
                              vkr_ui_theme()->font_body)
                   .x) +
         52.0f;
}

typedef struct ViewHeaderLayout {
  Vec4 left;
  Vec4 right;
  float32_t chips[4];
  bool8_t compact;
  bool8_t show_right;
} ViewHeaderLayout;

static ViewHeaderLayout view_header_layout(const VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame) {
  ViewHeaderLayout layout = {0};
  const Vec4 scene = view_scene_rect(frame);
  char text[4][48];
  view_chip_text(&frame->view_state, editor->grid_spacing, text);
  const float32_t available = Max(0.0f, scene.z - VIEW_INSET_PT * 2.0f);
  const float32_t right_width =
      VIEW_CHIP_HEIGHT_PT * (float32_t)ArrayCount(view_tools) + 6.0f;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    layout.compact = pass == 1;
    float32_t width = 6.0f;
    for (uint32_t i = 0; i < 3; ++i) {
      layout.chips[i] = view_chip_width(frame->ui, text[i], layout.compact);
      width += layout.chips[i] + 2.0f;
    }
    layout.chips[3] = view_chip_width(frame->ui, text[3], layout.compact);
    const float32_t right = right_width + 10.0f + layout.chips[3] + 6.0f;
    layout.show_right = width + right + 12.0f <= available;
    layout.left = (Vec4){scene.x + VIEW_INSET_PT, scene.y + VIEW_INSET_PT,
                         Min(width, available), VIEW_CHIP_HEIGHT_PT + 6.0f};
    layout.right =
        (Vec4){scene.x + scene.z - VIEW_INSET_PT - right,
               scene.y + VIEW_INSET_PT, right, VIEW_CHIP_HEIGHT_PT + 6.0f};
    if (layout.show_right || layout.compact)
      break;
  }
  if (!layout.show_right)
    layout.right = (Vec4){0};
  if (scene.w < VIEW_CHIP_HEIGHT_PT + 24.0f)
    layout.left = layout.right = (Vec4){0};
  return layout;
}

static uint32_t view_popup_count(uint32_t popup) {
  switch (popup) {
  case VIEW_POPUP_CAMERA:
    return ArrayCount(view_camera_names);
  case VIEW_POPUP_RENDER:
    return ArrayCount(view_render_modes);
  case VIEW_POPUP_GRID:
    return 4;
  case VIEW_POPUP_SPEED:
    return ArrayCount(view_camera_speeds);
  default:
    return 0;
  }
}

static void view_popup_layout(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  const uint32_t count = view_popup_count(editor->view_popup);
  if (!count) {
    editor->view_popup_rect_pt = (Vec4){0};
    return;
  }
  const float32_t scale = frame->ui->content_scale;
  const float32_t screen_w = (float32_t)frame->ui->target_width / scale;
  const float32_t screen_h = (float32_t)frame->ui->target_height / scale;
  const Vec2 size = {VIEW_POPUP_WIDTH_PT,
                     (float32_t)count * VIEW_POPUP_ROW_PT + 10.0f};
  const Vec4 anchor = editor->view_popup_anchor_pt;
  const float32_t x = vkr_clamp_f32(anchor.x, 0, Max(0.0f, screen_w - size.x));
  float32_t y = anchor.y + anchor.w + 4.0f;
  if (y + size.y > screen_h)
    y = Max(VKR_EDITOR_NAVIGATION_HEIGHT_PT, anchor.y - size.y - 4.0f);
  editor->view_popup_rect_pt = (Vec4){x, y, size.x, size.y};
}

static void view_register_rect(VkrUiSystem *ui, Vec4 rect) {
  const float32_t scale = ui->content_scale;
  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
                                    (VkrUiRect){rect.x * scale, rect.y * scale,
                                                rect.z * scale,
                                                rect.w * scale});
}

static void view_request(const VkrSampleUiFrame *frame,
                         VkrSampleViewState next) {
  if (frame->view_request)
    *frame->view_request =
        (VkrSampleViewRequest){.value = next, .apply = true_v};
}

/* Q/W/E/R pick transform tools and F frames the selection while the Scene or
 * no widget holds the keyboard. Modified presses stay with other shortcuts. */
static void view_shortcuts(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const bool8_t scene_focus =
      frame->scene_keyboard_focus && *frame->scene_keyboard_focus;
  if (frame->mouse_captured || editor->cmd_active ||
      editor->menu != VKR_EDITOR_MENU_NONE || frame->scene_rendering_stopped ||
      (!scene_focus && ui->focused_id != VKR_UI_ID_NONE))
    return;
  static const Keys keys[] = {KEY_Q, KEY_W, KEY_E, KEY_R};
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    if (input_key_just_pressed(frame->input, keys[i]) &&
        input_key_press_modifiers(frame->input, keys[i]) == 0u) {
      VkrSampleViewState next = frame->view_state;
      next.gizmo_tool = view_tools[i].mode;
      view_request(frame, next);
      ui->capture.keyboard = true_v;
    }
  }
  /* The selection may live in the World or an added scene. */
  const VkrScene *scene =
      vkr_editor_entity_scene(frame, frame->selected_entity);
  const bool8_t selected =
      scene && vkr_scene_entity_alive(scene, frame->selected_entity);
  if (input_key_just_pressed(frame->input, KEY_F) &&
      input_key_press_modifiers(frame->input, KEY_F) == 0u && selected) {
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_FRAME, .entity = frame->selected_entity};
    ui->capture.keyboard = true_v;
  }
  /* Delete, or Backspace on a Mac keyboard, deletes the selected object. */
  const Keys delete_key = input_key_just_pressed(frame->input, KEY_DELETE)
                              ? KEY_DELETE
                              : KEY_BACKSPACE;
  if (input_key_just_pressed(frame->input, delete_key) &&
      input_key_press_modifiers(frame->input, delete_key) == 0u && selected) {
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_DELETE, .entity = frame->selected_entity};
    ui->capture.keyboard = true_v;
  }
}

void vkr_editor_viewport_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->mapping_valid) {
    editor->view_toolbar_rect_pt = (Vec4){0};
    editor->view_popup = VIEW_POPUP_NONE;
    return;
  }
  view_shortcuts(editor, frame);
  const ViewHeaderLayout layout = view_header_layout(editor, frame);
  const float32_t scale = ui->content_scale;
  if (frame->mouse_captured || editor->cmd_active ||
      editor->menu != VKR_EDITOR_MENU_NONE) {
    editor->view_popup = VIEW_POPUP_NONE;
  } else if (ui->mouse_pressed) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(frame->input, BUTTON_LEFT, &press_x,
                                    &press_y);
    const Vec2 press = {(float32_t)press_x / scale, (float32_t)press_y / scale};
    if (!view_contains(layout.left, press) &&
        !view_contains(layout.right, press) &&
        !view_contains(editor->view_popup_rect_pt, press)) {
      editor->view_popup = VIEW_POPUP_NONE;
    }
  }
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->view_popup = VIEW_POPUP_NONE;
  }
  editor->view_toolbar_rect_pt = layout.left;
  view_popup_layout(editor, frame);
  view_register_rect(ui, layout.left);
  view_register_rect(ui, layout.right);
  view_register_rect(ui, editor->view_popup_rect_pt);
}

static VkrUiPanelConfig view_panel(Vec4 rect) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.placement.justify = VKR_UI_ALIGN_START;
  panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.style = vkr_editor_overlay_style();
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){rect.z, rect.w};
  panel.clip_children = true_v;
  return panel;
}

/* Dropdown chip: icon, value and a trailing caret. */
static bool8_t view_chip(VkrEditorUi *editor, VkrUiSystem *ui, const char *id,
                         uint32_t column, VkrUiIcon icon, const char *text,
                         bool8_t open, bool8_t active, bool8_t disabled,
                         bool8_t compact, const char *tooltip, uint32_t popup) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement.column = column;
  chip.placement.row = 0;
  chip.placement.justify = VKR_UI_ALIGN_STRETCH;
  chip.placement.align = VKR_UI_ALIGN_STRETCH;
  chip.fill = true_v;
  vkr_editor_ghost_style(&chip);
  chip.style.padding_pt = (VkrUiEdges){3, 20, 3, 8};
  chip.style.font_size_pt = theme->font_body;
  chip.style.text_color = theme->text;
  chip.icon = icon;
  chip.icon_size_pt = 15.0f;
  chip.icon_color = active ? theme->accent_hover : theme->text_secondary;
  if (open)
    chip.style.background_color = theme->raised_hover;
  chip.disabled = disabled;
  chip.tooltip = open ? (String8){0} : view_string(tooltip);
  (void)vkr_ui_push_id_label(ui, view_string(id));
  const VkrUiId chip_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("chip"));
  const bool8_t clicked =
      vkr_ui_button(ui, string8_lit("chip"),
                    compact ? (String8){0} : view_string(text), &chip);
  VkrUiWidgetConfig caret = vkr_ui_widget_config_default();
  caret.placement.column = column;
  caret.placement.row = 0;
  caret.placement.justify = VKR_UI_ALIGN_END;
  caret.placement.align = VKR_UI_ALIGN_CENTER;
  caret.placement.margin_pt.right = 5.0f;
  caret.icon = VKR_UI_ICON_CHEVRON_DOWN;
  caret.icon_size_pt = 10.0f;
  caret.icon_color = theme->text_secondary;
  caret.disabled = disabled;
  vkr_ui_label(ui, string8_lit("caret"), (String8){0}, &caret);
  (void)vkr_ui_pop_id(ui);
  if (clicked) {
    VkrUiRect rect = {0};
    if (vkr_ui_widget_rect(ui, chip_id, &rect)) {
      const float32_t scale = ui->content_scale;
      editor->view_popup_anchor_pt =
          (Vec4){rect.x / scale, rect.y / scale, rect.width / scale,
                 rect.height / scale};
    }
    editor->view_popup = editor->view_popup == popup ? VIEW_POPUP_NONE : popup;
  }
  return clicked;
}

static void view_popup_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             bool8_t disabled) {
  const uint32_t popup = editor->view_popup;
  const uint32_t count = view_popup_count(popup);
  if (!count)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  view_popup_layout(editor, frame);
  view_register_rect(ui, editor->view_popup_rect_pt);
  VkrUiTrack rows[16];
  for (uint32_t i = 0; i < count; ++i)
    rows[i] = (VkrUiTrack){.value = VIEW_POPUP_ROW_PT, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig panel = view_panel(editor->view_popup_rect_pt);
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){5, 5, 5, 5};
  panel.style.gap_pt = 0;
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){editor->view_popup_rect_pt.z, editor->view_popup_rect_pt.w};
  panel.rows = rows;
  panel.row_count = count;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.viewport.options"), &panel))
    return;
  VkrSampleViewState next = frame->view_state;
  for (uint32_t i = 0; i < count; ++i) {
    char text[64];
    bool8_t checked = false_v;
    VkrUiIcon icon = VKR_UI_ICON_NONE;
    switch (popup) {
    case VIEW_POPUP_CAMERA:
      snprintf(text, sizeof(text), "%s%s", view_camera_names[i],
               i ? "  (orthographic)" : "");
      checked = (uint32_t)next.camera_view == i;
      break;
    case VIEW_POPUP_RENDER:
      snprintf(text, sizeof(text), "%s", view_render_modes[i].name);
      checked = next.render_mode == view_render_modes[i].mode;
      break;
    case VIEW_POPUP_GRID:
      if (i == 0) {
        snprintf(text, sizeof(text), "Show grid");
        checked = next.grid_enabled;
      } else if (i == 3) {
        snprintf(text, sizeof(text), "Cell numbers and letters");
        checked = next.grid_labels;
      } else {
        snprintf(
            text, sizeof(text), "%s cells  (%.4g u)",
            i == 1 ? "Smaller" : "Larger",
            (double)vkr_clamp_f32(next.grid_spacing * (i == 1 ? 0.5f : 2.0f),
                                  0.001f, 10000.0f));
        icon = i == 1 ? VKR_UI_ICON_ZOOM_OUT : VKR_UI_ICON_ZOOM_IN;
      }
      break;
    case VIEW_POPUP_SPEED:
      snprintf(text, sizeof(text), "%.3g units / second",
               (double)view_camera_speeds[i]);
      checked = fabsf(next.camera_speed - view_camera_speeds[i]) < 0.001f;
      break;
    default:
      break;
    }
    VkrUiWidgetConfig item = vkr_ui_widget_config_default();
    item.placement.column = 0;
    item.placement.row = i;
    item.fill = true_v;
    vkr_editor_ghost_style(&item);
    item.style.hover_background_color = theme->accent;
    item.style.padding_pt = (VkrUiEdges){3, 8, 3, 8};
    item.style.text_color = theme->text;
    item.style.font_size_pt = theme->font_body;
    item.icon = checked ? VKR_UI_ICON_CHECK : icon;
    item.icon_size_pt = 14.0f;
    item.icon_color = checked ? theme->accent_hover : theme->text_secondary;
    if (item.icon == VKR_UI_ICON_NONE)
      item.style.padding_pt.left += 20.0f;
    item.disabled = disabled;
    (void)vkr_ui_push_id_u64(ui, (uint64_t)popup * 16 + i);
    if (vkr_ui_button(ui, string8_lit("option"), view_string(text), &item)) {
      if (popup == VIEW_POPUP_CAMERA) {
        next.camera_view = (VkrSampleCameraView)i;
      } else if (popup == VIEW_POPUP_RENDER) {
        next.render_mode = view_render_modes[i].mode;
      } else if (popup == VIEW_POPUP_GRID) {
        if (i == 0) {
          next.grid_enabled = !next.grid_enabled;
        } else if (i == 3) {
          next.grid_labels = !next.grid_labels;
        } else {
          next.grid_spacing = vkr_clamp_f32(
              next.grid_spacing * (i == 1 ? 0.5f : 2.0f), 0.001f, 10000.0f);
          next.grid_enabled = true_v;
        }
      } else {
        next.camera_speed = view_camera_speeds[i];
      }
      view_request(frame, next);
      if (popup != VIEW_POPUP_GRID)
        editor->view_popup = VIEW_POPUP_NONE;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_viewport_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const ViewHeaderLayout layout = view_header_layout(editor, frame);
  if (layout.left.z <= 0 || layout.left.w <= 0)
    return;
  const bool8_t disabled =
      !frame->view_request || frame->scene_rendering_stopped;
  char text[4][48];
  view_chip_text(&frame->view_state, editor->grid_spacing, text);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER);

  const VkrUiTrack left_columns[] = {
      {.value = layout.chips[0], .unit = VKR_UI_TRACK_PX},
      {.value = layout.chips[1], .unit = VKR_UI_TRACK_PX},
      {.value = layout.chips[2], .unit = VKR_UI_TRACK_PX},
  };
  const VkrUiTrack chip_row = {.value = VIEW_CHIP_HEIGHT_PT,
                               .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig left = view_panel(layout.left);
  left.columns = left_columns;
  left.column_count = ArrayCount(left_columns);
  left.rows = &chip_row;
  left.row_count = 1u;
  if (vkr_ui_panel_begin(ui, string8_lit("editor.viewport.toolbar"), &left)) {
    const bool8_t perspective =
        frame->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
    (void)view_chip(editor, ui, "camera", 0, VKR_UI_ICON_PERSPECTIVE, text[0],
                    editor->view_popup == VIEW_POPUP_CAMERA, !perspective,
                    disabled, layout.compact,
                    "Camera projection and orthographic direction",
                    VIEW_POPUP_CAMERA);
    (void)view_chip(editor, ui, "render", 1, VKR_UI_ICON_VIEW_MODE, text[1],
                    editor->view_popup == VIEW_POPUP_RENDER,
                    frame->view_state.render_mode != VKR_RENDER_MODE_DEFAULT,
                    disabled, layout.compact, "Viewport rendering mode",
                    VIEW_POPUP_RENDER);
    (void)view_chip(editor, ui, "grid", 2, VKR_UI_ICON_GRID, text[2],
                    editor->view_popup == VIEW_POPUP_GRID,
                    frame->view_state.grid_enabled, disabled, layout.compact,
                    "World grid: numbered columns, lettered rows and cell size",
                    VIEW_POPUP_GRID);
    (void)vkr_ui_panel_end(ui);
  }

  if (layout.show_right) {
    const VkrUiTrack right_columns[] = {
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = 10.0f, .unit = VKR_UI_TRACK_PX},
        {.value = layout.chips[3], .unit = VKR_UI_TRACK_PX},
    };
    VkrUiPanelConfig right = view_panel(layout.right);
    right.columns = right_columns;
    right.column_count = ArrayCount(right_columns);
    right.rows = &chip_row;
    right.row_count = 1u;
    if (vkr_ui_panel_begin(ui, string8_lit("editor.viewport.tools"), &right)) {
      for (uint32_t i = 0; i < ArrayCount(view_tools); ++i) {
        const bool8_t selected =
            frame->view_state.gizmo_tool == view_tools[i].mode;
        VkrUiWidgetConfig tool = vkr_editor_icon_button_config(
            i, 0, view_tools[i].icon,
            string8_create_formatted(ui->frame_allocator, "%s tool  (%s)",
                                     view_tools[i].name, view_tools[i].key));
        tool.style.min_size_pt = tool.style.max_size_pt =
            (Vec2){VIEW_CHIP_HEIGHT_PT, VIEW_CHIP_HEIGHT_PT};
        tool.style.text_color = theme->text;
        tool.icon_color = theme->text_secondary;
        vkr_editor_toggle_style(&tool, selected);
        if (selected) {
          tool.style.background_color = theme->accent;
          tool.style.hover_background_color = theme->accent_hover;
          tool.icon_color = theme->text_on_accent;
        }
        tool.disabled = disabled;
        (void)vkr_ui_push_id_u64(ui, i);
        if (vkr_ui_button(ui, string8_lit("tool"), (String8){0}, &tool)) {
          VkrSampleViewState next = frame->view_state;
          next.gizmo_tool = view_tools[i].mode;
          view_request(frame, next);
        }
        (void)vkr_ui_pop_id(ui);
      }
      VkrUiPanelConfig divider = vkr_ui_panel_config_default();
      divider.placement.column = 4;
      divider.placement.row = 0;
      divider.placement.justify = VKR_UI_ALIGN_CENTER;
      divider.placement.margin_pt = (VkrUiEdges){5, 0, 5, 0};
      divider.style.min_size_pt = divider.style.max_size_pt =
          (Vec2){1.0f, 16.0f};
      divider.style.background_color = theme->border_strong;
      if (vkr_ui_panel_begin(ui, string8_lit("divider"), &divider))
        (void)vkr_ui_panel_end(ui);
      (void)view_chip(editor, ui, "speed", 5, VKR_UI_ICON_SPEED, text[3],
                      editor->view_popup == VIEW_POPUP_SPEED, false_v, disabled,
                      layout.compact,
                      "Free-camera flight speed (units per second)",
                      VIEW_POPUP_SPEED);
      (void)vkr_ui_panel_end(ui);
    }
  }
  view_popup_build(editor, frame, disabled);
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

/* Returns false when no grid center projects in front of the camera. */
/* The perspective eye: the point the view-projection maps to clip x = y =
   w = 0. */
static bool8_t grid_eye(const VkrSampleUiFrame *frame, Vec3 *eye) {
  const Mat4 m = frame->view_projection;
  const float64_t a[3][3] = {
      {m.m00, m.m01, m.m02}, {m.m10, m.m11, m.m12}, {m.m30, m.m31, m.m32}};
  const float64_t b[3] = {-m.m03, -m.m13, -m.m33};
  const float64_t det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!isfinite(det) || fabs(det) < 1e-12) {
    return false_v;
  }
  float64_t solution[3];
  for (uint32_t column = 0; column < 3; ++column) {
    float64_t replaced[3][3];
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 3; ++c) {
        replaced[r][c] = c == column ? b[r] : a[r][c];
      }
    }
    solution[column] = (replaced[0][0] * (replaced[1][1] * replaced[2][2] -
                                          replaced[1][2] * replaced[2][1]) -
                        replaced[0][1] * (replaced[1][0] * replaced[2][2] -
                                          replaced[1][2] * replaced[2][0]) +
                        replaced[0][2] * (replaced[1][0] * replaced[2][1] -
                                          replaced[1][1] * replaced[2][0])) /
                       det;
  }
  *eye = vec3_new((float32_t)solution[0], (float32_t)solution[1],
                  (float32_t)solution[2]);
  return isfinite(eye->x) && isfinite(eye->y) && isfinite(eye->z);
}

/* The world point under a viewport pixel at clip depth `depth`. */
static bool8_t viewport_unproject(const VkrSampleUiFrame *frame, Vec2 pixel,
                                  float32_t depth, Vec3 *out) {
  const Vec4 image = frame->mapping.image_rect_px;
  if (image.z <= 0.0f || image.w <= 0.0f) {
    return false_v;
  }
  /* NDC is Y-down in this convention, matching UI space. */
  const Vec4 ndc = {(pixel.x - image.x) / image.z * 2.0f - 1.0f,
                    (pixel.y - image.y) / image.w * 2.0f - 1.0f, depth, 1.0f};
  const Vec4 world = mat4_mul_vec4(mat4_inverse(frame->view_projection), ndc);
  if (!isfinite(world.w) || fabsf(world.w) < 1e-9f) {
    return false_v;
  }
  *out = vec3_new(world.x / world.w, world.y / world.w, world.z / world.w);
  return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

bool8_t vkr_editor_viewport_drop_point(const VkrSampleUiFrame *frame,
                                       Vec2 pixel, Vec3 *out) {
  Vec3 origin = {0};
  Vec3 target = {0};
  if (!frame->mapping_valid ||
      !viewport_unproject(frame, pixel, 0.5f, &target)) {
    return false_v;
  }
  /* Perspective rays leave the eye; orthographic rays run through two
     depths of the pixel. */
  if (!grid_eye(frame, &origin) &&
      !viewport_unproject(frame, pixel, 0.25f, &origin)) {
    return false_v;
  }
  const Vec3 ray = vec3_sub(target, origin);
  const float32_t length = vec3_length(ray);
  if (!isfinite(length) || length < 1e-6f) {
    return false_v;
  }
  const Vec3 direction = vec3_scale(ray, 1.0f / length);
  const float32_t t =
      fabsf(direction.y) > 1e-4f ? -origin.y / direction.y : -1.0f;
  const bool8_t ground = t > 0.0f && t < 500.0f;
  *out = vec3_add(origin, vec3_scale(direction, ground ? t : 8.0f));
  if (ground) {
    out->y = 0.0f;
  }
  return true_v;
}

/* Cells from the world origin to each edge of the perspective grid. */
#define GRID_PERSPECTIVE_CELLS 40
/* Every tenth line is a major line. */
#define GRID_MAJOR_CELLS 10

/* A fixed world grid, as in UE5: centered on the world origin with a
   constant extent, one line per grid spacing and a major line every ten.
   It never follows or rescales with the camera. Minor lines fade out as the
   camera rises, so distant views keep only the major lines. */
static bool8_t grid_perspective_lines(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      uint32_t capacity) {
  Vec3 eye;
  const float32_t spacing = frame->view_state.grid_spacing;
  if (!grid_eye(frame, &eye) || !isfinite(spacing) || spacing <= 0.0f) {
    return false_v;
  }
  const float32_t rise = fabsf(eye.y) / (spacing * 40.0f);
  const float32_t minor_alpha =
      vkr_clamp_f32((1.0f - rise) / 0.75f, 0.0f, 1.0f);
  const float32_t extent = spacing * GRID_PERSPECTIVE_CELLS;
  editor->grid_spacing = spacing;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    for (int32_t cell = -GRID_PERSPECTIVE_CELLS;
         cell <= GRID_PERSPECTIVE_CELLS && editor->grid_line_count < capacity;
         ++cell) {
      const bool8_t major = cell % GRID_MAJOR_CELLS == 0;
      if (!major && minor_alpha <= 0.0f) {
        continue;
      }
      const float32_t coordinate = (float32_t)cell * spacing;
      const Vec3 from = axis ? vec3_new(-extent, 0, coordinate)
                             : vec3_new(coordinate, 0, -extent);
      const Vec3 to = axis ? vec3_new(extent, 0, coordinate)
                           : vec3_new(coordinate, 0, extent);
      const float32_t half_major = spacing * GRID_MAJOR_CELLS * 0.5f;
      editor->grid_lines[editor->grid_line_count++] = (VkrEditorGridLine){
          .from = from,
          .to = to,
          .label_offset =
              axis ? vec3_new(0, 0, half_major) : vec3_new(half_major, 0, 0),
          .top_label = axis == 0,
          .world_axis = cell == 0,
          .alpha = major ? 1.0f : 0.55f * minor_alpha,
          .unlabelled = !major,
      };
    }
  }
  return editor->grid_line_count > 0u;
}

/* Returns false when an image corner misses the grid plane or the visible
 * extent is empty. */
static bool8_t grid_fit_orthographic(const VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     bool8_t side, float32_t *spacing,
                                     Vec2 *minimum, Vec2 *maximum) {
  const VkrUiSystem *ui = frame->ui;
  for (uint32_t i = 0; i < 4; ++i) {
    Vec2 point;
    if (!grid_plane_point(frame, (Vec2){i & 1 ? 1 : -1, i & 2 ? 1 : -1}, side,
                          &point)) {
      return false_v;
    }
    minimum->x = Min(minimum->x, point.x);
    minimum->y = Min(minimum->y, point.y);
    maximum->x = Max(maximum->x, point.x);
    maximum->y = Max(maximum->y, point.y);
  }
  /* Coarsen by powers of two until every visible cell can carry its own
   * label. Axis views keep the first grid axis horizontal on screen. */
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 extent = {maximum->x - minimum->x, maximum->y - minimum->y};
  if (!(extent.x > 0 && extent.y > 0)) {
    return false_v;
  }
  const Vec2 pt_per_unit = {image.z / ui->content_scale / extent.x,
                            image.w / ui->content_scale / extent.y};
  for (uint32_t step = 0; step < 64; ++step) {
    const float32_t columns = ceilf(extent.x / *spacing) + 1;
    const float32_t rows = ceilf(extent.y / *spacing) + 1;
    const Vec2 number =
        grid_label_size(editor, ui, (uint32_t)Min(columns, 1.0e9f), true_v);
    if (columns <= 44 && rows <= 44 &&
        *spacing * pt_per_unit.x >= number.x + 6 &&
        *spacing * pt_per_unit.y >= number.y + 4) {
      break;
    }
    *spacing *= 2;
  }
  return true_v;
}

static bool8_t grid_first_axis_top(const VkrSampleUiFrame *frame,
                                   bool8_t perspective, Vec3 center,
                                   float32_t spacing) {
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
  return first_axis_top;
}

/* Number the placed labels in screen order so visible cells read 1..N
 * across the top and A.. down the right, whatever the view orientation.
 * Perspective fans can crowd labels; those cells stay unlabeled. */
static void grid_number_labels(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
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
      if (line->top_label != top_edge || line->unlabelled ||
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
}

static void grid_build_line_widgets(VkrEditorUi *editor, VkrUiSystem *ui) {
  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    VkrEditorGridLine *line = &editor->grid_lines[i];
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
    widget.placement.column = 0;
    widget.placement.row = 0;
    widget.placement.justify = VKR_UI_ALIGN_START;
    widget.placement.align = VKR_UI_ALIGN_START;
    widget.style.min_size_pt = widget.style.max_size_pt = (Vec2){1, 1};
    widget.style.text_color =
        line->world_axis
            ? vkr_ui_color_alpha(vkr_ui_theme()->accent_hover, 0.8f)
            : (Vec4){0.80f, 0.84f, 0.90f, 0.28f * line->alpha};
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
      widget.style.text_color = vkr_ui_theme()->text;
      widget.style.background_color = vkr_ui_theme()->overlay;
      widget.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
      widget.style.corner_radius_pt = (Vec4){2, 2, 2, 2};
      widget.text.font = editor->heading_font;
      widget.placement.margin_pt = (VkrUiEdges){100000, 0, 0, 100000};
      vkr_ui_label(ui, string8_lit("cell"), view_string(text), &widget);
      line->label =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("cell"));
    }
    (void)vkr_ui_pop_id(ui);
  }
}

/* Orthographic lines fill the visible plane at the fitted spacing. */
static void grid_orthographic_lines(VkrEditorUi *editor, bool8_t side,
                                    float32_t spacing, Vec2 minimum,
                                    Vec2 maximum, bool8_t first_axis_top,
                                    uint32_t capacity) {
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
          .alpha = 1.0f,
      };
    }
  }
}

void vkr_editor_grid_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  editor->grid_line_count = 0;
  editor->grid_spacing = 0;
  editor->grid_frame = frame->ui->frame_index;
  editor->grid_camera_view = frame->view_state.camera_view;
  /* A project's World renders alone before any scene opens. */
  if (!frame->mapping_valid || !frame->view_state.grid_enabled ||
      (!frame->scene && !frame->world) || frame->scene_rendering_stopped ||
      (frame->scene_backdrop_blur && *frame->scene_backdrop_blur)) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const bool8_t side =
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_LEFT ||
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_RIGHT;
  const bool8_t perspective =
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
  float32_t spacing = frame->view_state.grid_spacing;
  if (!isfinite(spacing) || spacing <= 0) {
    return;
  }
  Vec2 minimum = {INFINITY, INFINITY};
  Vec2 maximum = {-INFINITY, -INFINITY};
  Vec3 center = vec3_zero();
  if (!perspective) {
    Vec2 center_plane;
    if (!grid_plane_point(frame, (Vec2){0, 0}, side, &center_plane)) {
      return;
    }
    center = grid_world(center_plane, side);
    if (!grid_fit_orthographic(editor, frame, side, &spacing, &minimum,
                               &maximum) ||
        !isfinite(spacing) || spacing <= 0 ||
        fabsf(minimum.x / spacing) > 1000000000 ||
        fabsf(minimum.y / spacing) > 1000000000 ||
        fabsf(maximum.x / spacing) > 1000000000 ||
        fabsf(maximum.y / spacing) > 1000000000) {
      return;
    }
  }
  const Vec4 image = frame->mapping.image_rect_px;
  VkrUiPanelConfig panel = view_panel(
      (Vec4){image.x / ui->content_scale, image.y / ui->content_scale,
             image.z / ui->content_scale, image.w / ui->content_scale});
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.border_pt = (VkrUiEdges){0};
  panel.style.background_color = (Vec4){0};
  panel.style.shadow_color = (Vec4){0};
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
    if (!grid_perspective_lines(editor, frame, capacity)) {
      (void)vkr_ui_panel_end(ui);
      return;
    }
  } else {
    first_axis_top = grid_first_axis_top(frame, perspective, center, spacing);
    editor->grid_spacing = spacing;
    grid_orthographic_lines(editor, side, spacing, minimum, maximum,
                            first_axis_top, capacity);
  }

  /* Cell labels are optional; they read 1..N across the top and A.. down the
     right, sized for the largest possible ordinal. */
  if (frame->view_state.grid_labels) {
    uint32_t axis_lines[2] = {0, 0};
    for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
      const VkrEditorGridLine *line = &editor->grid_lines[i];
      if (!line->unlabelled) {
        ++axis_lines[line->top_label == first_axis_top ? 0 : 1];
      }
    }
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
    grid_number_labels(editor, frame);
  } else {
    editor->grid_reserved_pt = (Vec2){0};
  }

  grid_build_line_widgets(editor, ui);
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

/* ---- Orientation gizmo ---- */

#define VIEW_GIZMO_SIZE_PT 76.0f
#define VIEW_GIZMO_AXIS_PT 26.0f

/* Screen-space direction and depth order of each world axis, derived from
 * the unjittered view-projection around the view center. */
typedef struct ViewGizmoAxis {
  Vec2 direction;
  float32_t depth;
} ViewGizmoAxis;

static bool8_t view_gizmo_axes(const VkrSampleUiFrame *frame,
                               ViewGizmoAxis out_axes[3]) {
  const Mat4 inverse = mat4_inverse(frame->view_projection);
  Vec4 center = mat4_mul_vec4(inverse, (Vec4){0.0f, 0.0f, 0.5f, 1.0f});
  if (!isfinite(center.w) || fabsf(center.w) < 1e-6f)
    return false_v;
  const Vec3 origin = {center.x / center.w, center.y / center.w,
                       center.z / center.w};
  const Vec4 base =
      mat4_mul_vec4(frame->view_projection, vec3_to_vec4(origin, 1.0f));
  if (!isfinite(base.w) || fabsf(base.w) < 1e-6f)
    return false_v;
  const Vec2 base_ndc = {base.x / base.w, base.y / base.w};
  /* A step proportional to the view distance keeps the probe near-linear. */
  const float32_t step = Max(0.01f, fabsf(base.w) * 0.05f);
  static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (uint32_t i = 0; i < 3; ++i) {
    const Vec3 tip = vec3_add(origin, vec3_scale(axes[i], step));
    const Vec4 clip =
        mat4_mul_vec4(frame->view_projection, vec3_to_vec4(tip, 1.0f));
    if (!isfinite(clip.w) || fabsf(clip.w) < 1e-6f)
      return false_v;
    /* NDC is Y-down in this convention, matching UI space. */
    Vec2 delta = {clip.x / clip.w - base_ndc.x, clip.y / clip.w - base_ndc.y};
    const float32_t aspect =
        frame->mapping.image_rect_px.w > 0.0f
            ? frame->mapping.image_rect_px.z / frame->mapping.image_rect_px.w
            : 1.0f;
    delta.x *= aspect;
    const float32_t length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    out_axes[i].direction = length > 1e-6f
                                ? (Vec2){delta.x / length, delta.y / length}
                                : (Vec2){0.0f, 0.0f};
    /* Keep foreshortening: axes pointing at the camera draw shorter. */
    const float32_t reference = step / Max(fabsf(base.w), 1e-6f);
    const float32_t scale = Min(1.0f, length / Max(reference, 1e-6f));
    out_axes[i].direction.x *= scale;
    out_axes[i].direction.y *= scale;
    out_axes[i].depth = clip.w - base.w;
  }
  return true_v;
}

void vkr_editor_orientation_gizmo_build(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid || !frame->scene ||
      frame->scene_rendering_stopped || !frame->view_request)
    return;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const Vec4 image = frame->mapping.image_rect_px;
  if (image.z / scale < 240.0f || image.w / scale < 200.0f)
    return;
  ViewGizmoAxis axes[3];
  if (!view_gizmo_axes(frame, axes))
    return;
  const float32_t size = VIEW_GIZMO_SIZE_PT;
  const Vec2 origin_pt = {image.x / scale + 12.0f,
                          (image.y + image.w) / scale - size - 12.0f};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){origin_pt.y, 0, 0, origin_pt.x};
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){size, size};
  panel.style.corner_radius_pt =
      (Vec4){size * 0.5f, size * 0.5f, size * 0.5f, size * 0.5f};
  panel.style.background_color = vkr_ui_color_alpha(theme->overlay, 0.45f);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER);
  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
                                    (VkrUiRect){origin_pt.x * scale,
                                                origin_pt.y * scale,
                                                size * scale, size * scale});
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.view.gizmo"), &panel)) {
    (void)vkr_ui_input_layer_set(ui, 0);
    return;
  }
  const Vec4 colors[3] = {theme->axis_x, theme->axis_y, theme->axis_z};
  static const char *names[3] = {"X", "Y", "Z"};
  static const VkrSampleCameraView positive_views[3] = {
      VKR_SAMPLE_CAMERA_RIGHT, VKR_SAMPLE_CAMERA_TOP,
      VKR_SAMPLE_CAMERA_PERSPECTIVE};
  static const VkrSampleCameraView negative_views[3] = {
      VKR_SAMPLE_CAMERA_LEFT, VKR_SAMPLE_CAMERA_BOTTOM,
      VKR_SAMPLE_CAMERA_PERSPECTIVE};
  const Vec2 center = {size * 0.5f, size * 0.5f};
  /* Draw back-facing ends first so near ends sit on top. */
  uint32_t order[6];
  float32_t depth[6];
  for (uint32_t i = 0; i < 3; ++i) {
    order[i * 2] = i * 2;
    order[i * 2 + 1] = i * 2 + 1;
    depth[i * 2] = axes[i].depth;
    depth[i * 2 + 1] = -axes[i].depth;
  }
  for (uint32_t a = 0; a < 6; ++a)
    for (uint32_t b = a + 1; b < 6; ++b)
      if (depth[order[b]] > depth[order[a]]) {
        const uint32_t swap = order[a];
        order[a] = order[b];
        order[b] = swap;
      }
  for (uint32_t k = 0; k < 6; ++k) {
    const uint32_t end = order[k];
    const uint32_t axis = end / 2;
    const bool8_t positive = (end % 2) == 0;
    const float32_t sign = positive ? 1.0f : -1.0f;
    const Vec2 tip = {
        center.x + axes[axis].direction.x * VIEW_GIZMO_AXIS_PT * sign,
        center.y + axes[axis].direction.y * VIEW_GIZMO_AXIS_PT * sign};
    (void)vkr_ui_push_id_u64(ui, end);
    if (positive) {
      VkrUiWidgetConfig line = vkr_ui_widget_config_default();
      line.placement.column = line.placement.row = 0;
      line.placement.justify = line.placement.align = VKR_UI_ALIGN_START;
      line.style.min_size_pt = line.style.max_size_pt = (Vec2){size, size};
      line.style.text_color = colors[axis];
      const Vec2 points[4] = {center, center, tip, tip};
      vkr_ui_bezier(ui, string8_lit("axis"), points, 2.0f, &line);
    }
    const float32_t cap = positive ? 16.0f : 11.0f;
    VkrUiWidgetConfig end_cap = vkr_ui_widget_config_default();
    end_cap.placement.column = end_cap.placement.row = 0;
    end_cap.placement.justify = end_cap.placement.align = VKR_UI_ALIGN_START;
    end_cap.placement.margin_pt =
        (VkrUiEdges){tip.y - cap * 0.5f, 0, 0, tip.x - cap * 0.5f};
    end_cap.style.min_size_pt = end_cap.style.max_size_pt = (Vec2){cap, cap};
    end_cap.style.padding_pt = (VkrUiEdges){0};
    end_cap.style.corner_radius_pt =
        (Vec4){cap * 0.5f, cap * 0.5f, cap * 0.5f, cap * 0.5f};
    end_cap.style.background_color =
        positive ? colors[axis] : vkr_ui_color_alpha(colors[axis], 0.25f);
    end_cap.style.border_pt =
        positive ? (VkrUiEdges){0} : (VkrUiEdges){1.5f, 1.5f, 1.5f, 1.5f};
    end_cap.style.border_color = colors[axis];
    end_cap.style.hover_background_color = theme->text;
    end_cap.style.text_color = theme->text_on_accent;
    end_cap.style.font_size_pt = 10.0f;
    end_cap.text.font = editor->heading_font;
    end_cap.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s%s view", positive ? "+" : "-", names[axis]);
    if (vkr_ui_button(ui, string8_lit("cap"),
                      positive ? string8_create((uint8_t *)names[axis], 1u)
                               : (String8){0},
                      &end_cap)) {
      VkrSampleViewState next = frame->view_state;
      next.camera_view = positive ? positive_views[axis] : negative_views[axis];
      view_request(frame, next);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0);
}

/* ---- Viewport documents (ADR-076) ---- */

/* The active document follows whatever the project has open, so opening a
   scene from Content retargets the current tab, as in a level editor. */
static void viewport_tabs_sync(VkrEditorUi *editor) {
  if (!editor->viewport_tab_count) {
    editor->viewport_tab_count = 1u;
    editor->viewport_tab_active = 0u;
  }
  if (!vkr_editor_projects_switch_ready(editor->projects)) {
    return;
  }
  VkrEditorViewportTab *tab =
      &editor->viewport_tabs[editor->viewport_tab_active];
  const String8 id = vkr_editor_projects_scene_id(editor->projects);
  const String8 name = vkr_editor_projects_scene_name(editor->projects);
  snprintf(tab->scene_id, sizeof(tab->scene_id), "%.*s", (int)id.length,
           id.str);
  snprintf(tab->label, sizeof(tab->label), "%.*s",
           (int)(name.length ? name.length : 5u),
           name.length ? (const char *)name.str : "World");
}

static void viewport_tabs_show(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame, uint32_t tab) {
  if (vkr_editor_projects_show_scene(editor->projects, editor, frame,
                                     editor->viewport_tabs[tab].scene_id)) {
    editor->viewport_tab_active = tab;
  }
}

static void viewport_tabs_remove(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, uint32_t tab) {
  if (editor->viewport_tab_count <= 1u) {
    return;
  }
  if (tab == editor->viewport_tab_active) {
    const uint32_t next =
        tab + 1u < editor->viewport_tab_count ? tab + 1u : tab - 1u;
    if (!vkr_editor_projects_show_scene(editor->projects, editor, frame,
                                        editor->viewport_tabs[next].scene_id)) {
      return;
    }
    editor->viewport_tab_active = next;
  }
  MemCopy(&editor->viewport_tabs[tab], &editor->viewport_tabs[tab + 1u],
          (editor->viewport_tab_count - tab - 1u) *
              sizeof(editor->viewport_tabs[0]));
  editor->viewport_tab_count--;
  if (editor->viewport_tab_active > tab) {
    editor->viewport_tab_active--;
  }
}

bool8_t vkr_editor_viewport_tab_new(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  viewport_tabs_sync(editor);
  if (editor->viewport_tab_count >= VKR_EDITOR_VIEWPORT_TAB_MAX ||
      !vkr_editor_projects_switch_ready(editor->projects)) {
    return false_v;
  }
  editor->viewport_tabs[editor->viewport_tab_count] =
      (VkrEditorViewportTab){.label = "World"};
  editor->viewport_tab_count++;
  const uint32_t previous = editor->viewport_tab_active;
  viewport_tabs_show(editor, frame, editor->viewport_tab_count - 1u);
  if (editor->viewport_tab_active == previous) {
    editor->viewport_tab_count--;
    return false_v;
  }
  return true_v;
}

bool8_t vkr_editor_viewport_tab_show(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t tab) {
  viewport_tabs_sync(editor);
  if (tab >= editor->viewport_tab_count ||
      !vkr_editor_projects_switch_ready(editor->projects)) {
    return false_v;
  }
  viewport_tabs_show(editor, frame, tab);
  return editor->viewport_tab_active == tab;
}

void vkr_editor_viewport_tabs_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect strip) {
  VkrUiSystem *ui = frame->ui;
  /* Documents exist in project mode, where the World is always open;
     otherwise one tab names the loaded scene. */
  const bool8_t project = frame->world && editor->projects;
  if (!project) {
    editor->viewport_tab_count = 1u;
    editor->viewport_tab_active = 0u;
    const String8 path = frame->scene_path;
    uint64_t start = path.length;
    while (start && path.str[start - 1u] != '/' && path.str[start - 1u] != '\\')
      --start;
    uint64_t length = path.length - start;
    const String8 suffix = string8_lit(".scene.json");
    if (length > suffix.length &&
        MemCompare(path.str + path.length - suffix.length, suffix.str,
                   suffix.length) == 0)
      length -= suffix.length;
    snprintf(editor->viewport_tabs[0].label,
             sizeof(editor->viewport_tabs[0].label), "%.*s",
             (int)(length ? length : 5u),
             length ? (const char *)path.str + start : "Scene");
    editor->viewport_tabs[0].scene_id[0] = '\0';
  } else {
    viewport_tabs_sync(editor);
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const float32_t tab_width = 150.0f * scale;
  const bool8_t ready =
      project && vkr_editor_projects_switch_ready(editor->projects);
  float32_t x = strip.x;
  uint32_t close = UINT32_MAX;
  uint32_t show = UINT32_MAX;
  for (uint32_t i = 0; i < editor->viewport_tab_count; ++i) {
    if (x + tab_width > strip.x + strip.width) {
      break;
    }
    const bool8_t active = i == editor->viewport_tab_active;
    const VkrEditorViewportTab *tab = &editor->viewport_tabs[i];
    const VkrUiTrack columns[] = {{1, VKR_UI_TRACK_FR}, {20, VKR_UI_TRACK_PX}};
    VkrUiPanelConfig panel = vkr_ui_panel_config_default();
    panel.placement.column = panel.placement.row = 0u;
    panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
    panel.placement.margin_pt =
        (VkrUiEdges){strip.y / scale + 3.0f, 0, 0, x / scale};
    panel.columns = columns;
    panel.column_count = ArrayCount(columns);
    panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){
        tab_width / scale - 4.0f, Max(1.0f, strip.height / scale - 3.0f)};
    panel.style.background_color = active ? theme->panel : (Vec4){0};
    panel.style.corner_radius_pt = (Vec4){5, 5, 0, 0};
    panel.style.border_pt = (VkrUiEdges){0, 1, 0, 1};
    panel.style.border_color = theme->separator;
    (void)vkr_ui_push_id_u64(ui, 0x7ab5000u + i);
    if (vkr_ui_panel_begin(ui, string8_lit("viewport.tab"), &panel)) {
      VkrUiWidgetConfig button = vkr_ui_widget_config_default();
      button.placement.column = button.placement.row = 0u;
      button.fill = true_v;
      vkr_editor_ghost_style(&button);
      button.style.padding_pt = (VkrUiEdges){3, 6, 3, 8};
      button.style.text_color = active ? theme->text : theme->text_secondary;
      button.icon =
          tab->scene_id[0] || !project ? VKR_UI_ICON_SCENE : VKR_UI_ICON_WORLD;
      button.icon_size_pt = 13.0f;
      button.icon_color = active ? theme->accent_hover : theme->text_secondary;
      button.disabled = !ready && !active;
      button.tooltip = string8_lit("Open this document in the Scene");
      if (vkr_ui_button(ui, string8_lit("select"),
                        string8_create_from_cstr((const uint8_t *)tab->label,
                                                 strlen(tab->label)),
                        &button) &&
          !active) {
        show = i;
      }
      if (editor->viewport_tab_count > 1u) {
        VkrUiWidgetConfig shut = vkr_editor_icon_button_config(
            1u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close document"));
        shut.style.min_size_pt = shut.style.max_size_pt = (Vec2){18, 18};
        shut.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
        shut.icon_size_pt = 11.0f;
        shut.disabled = !ready;
        if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &shut)) {
          close = i;
        }
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
    x += tab_width;
  }
  if (project && editor->viewport_tab_count < VKR_EDITOR_VIEWPORT_TAB_MAX &&
      x + 26.0f * scale <= strip.x + strip.width) {
    VkrUiWidgetConfig add = vkr_editor_icon_button_config(
        0u, 0u, VKR_UI_ICON_ADD, string8_lit("New document showing the World"));
    add.placement.justify = add.placement.align = VKR_UI_ALIGN_START;
    add.placement.margin_pt =
        (VkrUiEdges){strip.y / scale + 4.0f, 0, 0, x / scale + 2.0f};
    add.style.min_size_pt = add.style.max_size_pt = (Vec2){22, 22};
    add.disabled = !ready;
    if (vkr_ui_button(ui, string8_lit("viewport.tab.add"), (String8){0},
                      &add)) {
      (void)vkr_editor_viewport_tab_new(editor, frame);
    }
  }
  if (show != UINT32_MAX) {
    viewport_tabs_show(editor, frame, show);
  } else if (close != UINT32_MAX) {
    viewport_tabs_remove(editor, frame, close);
  }
}
