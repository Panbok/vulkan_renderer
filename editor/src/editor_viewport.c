#include "editor_agent.h"
#include "editor_blockout.h"
#include "editor_brush_grid.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_ops.h"
#include "editor_projects.h"

#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_physics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These controls own editor interaction only. The runtime validates and applies
 * camera/render requests; grid geometry follows its final unjittered camera. */
enum {
  VIEW_POPUP_NONE,
  VIEW_POPUP_CAMERA,
  VIEW_POPUP_RENDER,
  VIEW_POPUP_GRID,
  VIEW_POPUP_SHOW,
  VIEW_POPUP_QUALITY,
  /* The camera speed slider. */
  VIEW_POPUP_SPEED,
  /* Where spawned objects land. */
  VIEW_POPUP_SNAP,
  VIEW_POPUP_COUNT,
};

#define VIEW_CHIP_HEIGHT_PT 26.0f
#define VIEW_CHIP_PAD_PT 8.0f
#define VIEW_CHIP_ICON_PT 15.0f
#define VIEW_CHIP_CARET_PT 10.0f
#define VIEW_POPUP_ROW_PT 24.0f
#define VIEW_POPUP_HEADER_PT 24.0f
#define VIEW_POPUP_WIDTH_PT 244.0f
#define VIEW_POPUP_ROW_MAX 24u
#define VIEW_INSET_PT 8.0f
/* Logarithmic speed range of the slider, in world units per second. */
#define VIEW_SPEED_MIN 0.1f
#define VIEW_SPEED_MAX 100.0f
/* Mouse-look multiplier range of the camera popup; 6 is the default. */
#define VIEW_SENSITIVITY_MIN 0.5f
#define VIEW_SENSITIVITY_MAX 20.0f
/* VKR_EDITOR_GRID_MIN_SPACING_PX of shared/editor_grid_kernel.slangh. */
#define VIEW_GRID_MIN_SPACING_PX 6.0f

static const char *const view_camera_names[] = {
    "Perspective", "Top", "Left", "Right", "Bottom", "Front", "Back"};

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

/* What one Show menu row toggles. */
typedef enum ViewShowTarget {
  VIEW_SHOW_HEADER,
  /* A VKR_SCENE_SHOW_HIDE_* geometry kind. */
  VIEW_SHOW_KIND,
  VIEW_SHOW_ICONS,
  /* One icon category: the offset of its flag in VkrEditorUi. */
  VIEW_SHOW_ICON_KIND,
  VIEW_SHOW_GRID,
  VIEW_SHOW_GRID_THROUGH,
  /* Collision shapes: `value` is the display mode the row selects. */
  VIEW_SHOW_COLLISION,
  VIEW_SHOW_PHYSICS,
} ViewShowTarget;

static const struct {
  const char *name;
  ViewShowTarget target;
  uint32_t value;
} view_show_rows[] = {
    {"Geometry", VIEW_SHOW_HEADER, 0},
    {"Static meshes", VIEW_SHOW_KIND, VKR_SCENE_SHOW_HIDE_STATIC_MESHES},
    {"Animated meshes", VIEW_SHOW_KIND, VKR_SCENE_SHOW_HIDE_ANIMATED_MESHES},
    {"Shapes", VIEW_SHOW_KIND, VKR_SCENE_SHOW_HIDE_SHAPES},
    {"Object icons", VIEW_SHOW_HEADER, 0},
    {"All icons", VIEW_SHOW_ICONS, 0},
    {"Directional lights", VIEW_SHOW_ICON_KIND,
     offsetof(VkrEditorUi, labels_directional)},
    {"Point and rect lights", VIEW_SHOW_ICON_KIND,
     offsetof(VkrEditorUi, labels_point)},
    {"Spot lights", VIEW_SHOW_ICON_KIND, offsetof(VkrEditorUi, labels_spot)},
    {"Sky, fog and post process", VIEW_SHOW_ICON_KIND,
     offsetof(VkrEditorUi, labels_environment)},
    {"Volumes, probes and markers", VIEW_SHOW_ICON_KIND,
     offsetof(VkrEditorUi, labels_markers)},
    {"Text objects", VIEW_SHOW_ICON_KIND, offsetof(VkrEditorUi, labels_text)},
    {"Empty objects", VIEW_SHOW_ICON_KIND, offsetof(VkrEditorUi, labels_empty)},
    {"Overlays", VIEW_SHOW_HEADER, 0},
    {"Grid", VIEW_SHOW_GRID, 0},
    {"Grid through geometry", VIEW_SHOW_GRID_THROUGH, 0},
    {"Collision of the selection", VIEW_SHOW_COLLISION, 1},
    {"Collision of all bodies", VIEW_SHOW_COLLISION, 2},
    {"Physics simulation", VIEW_SHOW_PHYSICS, 0},
};

static const char *const view_snap_targets[VKR_EDITOR_SNAP_COUNT] = {
    "Free", "Surface", "Grid"};

/* Quality popup rows of the open scene's texture limit
   (VkrSceneSettings.texture_max_extent). */
static const struct {
  const char *name;
  uint32_t extent;
} view_texture_limits[] = {
    {"Full resolution", 0u},
    {"4K  (4096)", 4096u},
    {"2K  (2048)", 2048u},
    {"1K  (1024)", 1024u},
};

/* Grid popup: the toggles, cell size halving and doubling, then the grid's
   height with Fit to surface and Reset. */
static const char *const view_grid_rows[] = {
    "Show grid", "Through geometry", "Cell numbers and letters",
    "Cell size", "Smaller cells",    "Larger cells",
    "Height",    "Fit to surface",   "Reset height"};

typedef enum ViewRowKind {
  VIEW_ROW_OPTION,
  VIEW_ROW_HEADER,
  VIEW_ROW_SLIDER,
} ViewRowKind;

/* One popup row as built: an option button, a section header, or a slider
   over [minimum, maximum] with its value drawn in `text`. */
typedef struct ViewRow {
  ViewRowKind kind;
  char text[64];
  VkrUiIcon icon;
  bool8_t checked;
  bool8_t disabled;
  float32_t value;
  float32_t minimum;
  float32_t maximum;
} ViewRow;

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

static float32_t view_speed_fraction(float32_t speed) {
  return vkr_clamp_f32(logf(Max(speed, 1e-6f) / VIEW_SPEED_MIN) /
                           logf(VIEW_SPEED_MAX / VIEW_SPEED_MIN),
                       0.0f, 1.0f);
}

/* Two significant digits keep the value readable on the chip. */
static float32_t view_speed_value(float32_t fraction) {
  const float32_t speed =
      VIEW_SPEED_MIN * powf(VIEW_SPEED_MAX / VIEW_SPEED_MIN,
                            vkr_clamp_f32(fraction, 0.0f, 1.0f));
  const float32_t unit = powf(10.0f, floorf(log10f(speed)) - 1.0f);
  return roundf(speed / unit) * unit;
}

static void view_speed_text(float32_t speed, char text[24]) {
  snprintf(text, 24, "%.3g", (double)speed);
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

void vkr_editor_view_fit_grid(const VkrSampleUiFrame *frame) {
  if (!frame->grid_fit_request) {
    return;
  }
  const Vec4 scene = frame->mapping.panel_rect_px;
  *frame->grid_fit_request = (VkrSampleGridFitRequest){
      .request = true_v,
      .position_px =
          vec2_new(scene.x + scene.z * 0.5f, scene.y + scene.w * 0.5f),
  };
}

/* Graphics settings the Scene renders with, or the editor's draft of the
   render scale while its slider is held. */
static VkrGraphicsSettings view_graphics(const VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  VkrGraphicsSettings settings = frame->graphics->settings;
  if (editor->view_scale_draft > 0.0f) {
    settings.render_scale = editor->view_scale_draft;
  }
  return settings;
}

/* The container whose texture limit applies: the open scene, else the World.
 */
static const VkrScene *view_texture_scene(const VkrSampleUiFrame *frame) {
  return frame->scene ? frame->scene : frame->world;
}

/* Left chip labels: camera, view mode, grid spacing, Show, quality. */
#define VIEW_LEFT_CHIPS 5u

static void view_chip_text(const VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame,
                           char text[VIEW_LEFT_CHIPS][48]) {
  const VkrSampleViewState *state = &frame->view_state;
  const uint32_t camera = (uint32_t)state->camera_view;
  snprintf(text[0], 48, "%s",
           camera < ArrayCount(view_camera_names) ? view_camera_names[camera]
                                                  : "Perspective");
  snprintf(text[1], 48, "%s", view_render_name(state->render_mode));
  if (state->grid_enabled)
    snprintf(text[2], 48, "%.4g",
             (double)(editor->grid_spacing > 0 ? editor->grid_spacing
                                               : state->grid_spacing));
  else
    snprintf(text[2], 48, "Off");
  snprintf(text[3], 48, "Show");
  if (frame->graphics) {
    const VkrGraphicsSettings settings = view_graphics(editor, frame);
    const char *preset =
        vkr_graphics_preset_name(vkr_graphics_settings_preset(&settings));
    if (settings.dynamic_resolution &&
        frame->graphics->dynamic_resolution_available)
      snprintf(text[4], 48, "%s \xc2\xb7 Dynamic %.0f%%", preset,
               (double)(settings.render_scale * 100.0f));
    else
      snprintf(text[4], 48, "%s \xc2\xb7 %.0f%%", preset,
               (double)(vkr_graphics_settings_render_scale(frame->graphics,
                                                           &settings) *
                        100.0f));
  } else {
    snprintf(text[4], 48, "Quality");
  }
}

/* The chip's content: padding, icon, gap, text, gap and caret. */
static float32_t view_chip_width(const VkrUiSystem *ui, const char *text,
                                 bool8_t compact) {
  const float32_t frame =
      VIEW_CHIP_PAD_PT * 2.0f + VIEW_CHIP_ICON_PT + 6.0f + VIEW_CHIP_CARET_PT;
  if (compact)
    return frame;
  return ceilf(view_text_size(ui, VKR_FONT_HANDLE_INVALID, text,
                              vkr_ui_theme()->font_body)
                   .x) +
         frame + 6.0f;
}

typedef struct ViewHeaderLayout {
  Vec4 left;
  Vec4 right;
  float32_t chips[VIEW_LEFT_CHIPS];
  /* Widths of the snapping and camera speed chips on the right. */
  float32_t snap;
  float32_t speed;
  bool8_t compact;
  bool8_t show_right;
} ViewHeaderLayout;

static ViewHeaderLayout view_header_layout(const VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame) {
  ViewHeaderLayout layout = {0};
  const Vec4 scene = view_scene_rect(frame);
  char text[VIEW_LEFT_CHIPS][48];
  view_chip_text(editor, frame, text);
  const float32_t available = Max(0.0f, scene.z - VIEW_INSET_PT * 2.0f);
  /* The tools and the World/Local space toggle, with the panel padding and
     the gaps between them. */
  const float32_t right_width =
      (VIEW_CHIP_HEIGHT_PT + 2.0f) * (float32_t)(ArrayCount(view_tools) + 1u) +
      6.0f;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    layout.compact = pass == 1;
    /* Overlay panels pad 3 points and space their columns 2 apart. */
    float32_t width = 6.0f - 2.0f;
    for (uint32_t i = 0; i < VIEW_LEFT_CHIPS; ++i) {
      layout.chips[i] = view_chip_width(frame->ui, text[i], layout.compact);
      width += layout.chips[i] + 2.0f;
    }
    char speed[24];
    view_speed_text(frame->view_state.camera_speed, speed);
    layout.speed = view_chip_width(frame->ui, speed, false_v);
    layout.snap = view_chip_width(
        frame->ui, view_snap_targets[editor->placement.target], layout.compact);
    const float32_t right =
        right_width + 10.0f + layout.snap + layout.speed + 2.0f * 3.0f;
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

static bool8_t *view_icon_flag(VkrEditorUi *editor, uint32_t offset) {
  return (bool8_t *)((uint8_t *)editor + offset);
}

/* Fill `rows` for `popup`; returns how many. */
static uint32_t view_popup_rows(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, uint32_t popup,
                                ViewRow *rows) {
  const VkrSampleViewState *state = &frame->view_state;
  uint32_t count = 0;
  switch (popup) {
  case VIEW_POPUP_CAMERA:
    for (uint32_t i = 0; i < ArrayCount(view_camera_names); ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){.checked = (uint32_t)state->camera_view == i};
      snprintf(row->text, sizeof(row->text), "%s%s", view_camera_names[i],
               i ? "  (orthographic)" : "");
    }
    break;
  case VIEW_POPUP_RENDER:
    for (uint32_t i = 0; i < ArrayCount(view_render_modes); ++i) {
      ViewRow *row = &rows[count++];
      *row =
          (ViewRow){.checked = state->render_mode == view_render_modes[i].mode};
      snprintf(row->text, sizeof(row->text), "%s", view_render_modes[i].name);
    }
    break;
  case VIEW_POPUP_GRID:
    for (uint32_t i = 0; i < ArrayCount(view_grid_rows); ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){0};
      snprintf(row->text, sizeof(row->text), "%s", view_grid_rows[i]);
      if (i == 0) {
        row->checked = state->grid_enabled;
      } else if (i == 1) {
        row->checked = state->grid_through_geometry;
      } else if (i == 2) {
        row->checked = state->grid_labels;
        row->disabled = state->camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
      } else if (i == 3) {
        row->kind = VIEW_ROW_HEADER;
        snprintf(row->text, sizeof(row->text), "Cell size  %.4g u",
                 (double)state->grid_spacing);
      } else if (i == 6) {
        /* The last fit's outcome while it is fresh, else the height. */
        row->kind = VIEW_ROW_HEADER;
        if (frame->grid_status.length)
          snprintf(row->text, sizeof(row->text), "%.*s",
                   (int)frame->grid_status.length, frame->grid_status.str);
        else
          snprintf(row->text, sizeof(row->text), "Height  %.2f m",
                   (double)state->grid_height);
      } else if (i == 7) {
        row->disabled = !frame->grid_fit_request;
      } else if (i == 8) {
        row->disabled = state->grid_height == 0.0f;
      } else {
        row->icon = i == 4 ? VKR_UI_ICON_ZOOM_OUT : VKR_UI_ICON_ZOOM_IN;
      }
    }
    break;
  case VIEW_POPUP_SHOW:
    for (uint32_t i = 0; i < ArrayCount(view_show_rows); ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){0};
      snprintf(row->text, sizeof(row->text), "%s", view_show_rows[i].name);
      const uint32_t value = view_show_rows[i].value;
      switch (view_show_rows[i].target) {
      case VIEW_SHOW_HEADER:
        row->kind = VIEW_ROW_HEADER;
        break;
      case VIEW_SHOW_KIND:
        row->checked = !(state->hidden_kinds & value);
        break;
      case VIEW_SHOW_ICONS:
        row->checked = editor->labels_enabled;
        break;
      case VIEW_SHOW_ICON_KIND:
        row->checked = *view_icon_flag(editor, value);
        row->disabled = !editor->labels_enabled;
        break;
      case VIEW_SHOW_GRID:
        row->checked = state->grid_enabled;
        break;
      case VIEW_SHOW_GRID_THROUGH:
        row->checked = state->grid_through_geometry;
        row->disabled = !state->grid_enabled;
        break;
      case VIEW_SHOW_COLLISION:
        row->checked = state->collision_display == value;
        break;
      case VIEW_SHOW_PHYSICS:
        row->checked =
            frame->scene && !vkr_scene_physics_is_disabled(frame->scene);
        row->disabled = !frame->scene;
        break;
      }
    }
    break;
  case VIEW_POPUP_QUALITY: {
    if (!frame->graphics) {
      break;
    }
    const VkrGraphicsSettings settings = view_graphics(editor, frame);
    const VkrGraphicsPreset preset = vkr_graphics_settings_preset(&settings);
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Scalability");
    for (uint32_t i = 0; i < VKR_GRAPHICS_PRESET_CUSTOM; ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){.checked = (uint32_t)preset == i};
      snprintf(row->text, sizeof(row->text), "%s",
               vkr_graphics_preset_name((VkrGraphicsPreset)i));
    }
    ViewRow *custom = &rows[count++];
    *custom = (ViewRow){.checked = preset == VKR_GRAPHICS_PRESET_CUSTOM,
                        .icon = VKR_UI_ICON_SETTINGS};
    snprintf(custom->text, sizeof(custom->text), "Custom settings...");
    /* With dynamic resolution the percentage caps the scale it chooses; a
       backend that cannot scale without temporal upscaling renders at 100%. */
    const bool8_t automatic = settings.dynamic_resolution &&
                              frame->graphics->dynamic_resolution_available;
    const float32_t render_scale =
        vkr_graphics_settings_render_scale(frame->graphics, &settings);
    const bool8_t fixed = !settings.temporal_upscaling &&
                          !frame->graphics->spatial_render_scale_available;
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text),
             automatic ? "Screen percentage  up to %.0f%%"
                       : "Screen percentage  %.0f%%",
             (double)(render_scale * 100.0f));
    rows[count++] = (ViewRow){.kind = VIEW_ROW_SLIDER,
                              .disabled = fixed,
                              .value = render_scale,
                              .minimum = 1.0f / 3.0f,
                              .maximum = 1.0f};
    ViewRow *dynamic = &rows[count++];
    *dynamic =
        (ViewRow){.checked = settings.dynamic_resolution,
                  .disabled = !frame->graphics->dynamic_resolution_available};
    snprintf(dynamic->text, sizeof(dynamic->text), "Dynamic resolution");

    /* The scene's texture limit; the Graphics limit still caps it. */
    const VkrScene *scene = view_texture_scene(frame);
    const uint32_t graphics_limit =
        vkr_graphics_settings_texture_max_dimension(&settings);
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    if (graphics_limit) {
      snprintf(rows[count++].text, sizeof(rows[0].text),
               "Scene textures  (Graphics limit %u)", graphics_limit);
    } else {
      snprintf(rows[count++].text, sizeof(rows[0].text), "Scene textures");
    }
    for (uint32_t i = 0; i < ArrayCount(view_texture_limits); ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){
          .checked = scene && scene->settings.texture_max_extent ==
                                  view_texture_limits[i].extent,
          .disabled = !scene,
      };
      snprintf(row->text, sizeof(row->text), "%s", view_texture_limits[i].name);
    }
    break;
  }
  case VIEW_POPUP_SNAP: {
    const VkrEditorPlacement *place = &editor->placement;
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Place new objects");
    static const char *const targets[VKR_EDITOR_SNAP_COUNT] = {
        "Free, on the ground plane", "On surfaces", "On the grid"};
    for (uint32_t i = 0; i < VKR_EDITOR_SNAP_COUNT; ++i) {
      ViewRow *row = &rows[count++];
      *row = (ViewRow){.checked = (uint32_t)place->target == i};
      snprintf(row->text, sizeof(row->text), "%s", targets[i]);
    }
    ViewRow *centers = &rows[count++];
    *centers = (ViewRow){.checked = place->cell_centers,
                         .disabled = place->target != VKR_EDITOR_SNAP_GRID};
    snprintf(centers->text, sizeof(centers->text), "Snap to cell centers");
    ViewRow *align = &rows[count++];
    *align = (ViewRow){.checked = place->align_to_normal,
                       .disabled = place->target != VKR_EDITOR_SNAP_SURFACE};
    snprintf(align->text, sizeof(align->text), "Align to surface normal");
    ViewRow *magnet = &rows[count++];
    *magnet = (ViewRow){.checked = place->magnet};
    snprintf(magnet->text, sizeof(magnet->text),
             "Snap brushes to nearby brushes");
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Move and rotate");
    ViewRow *grid = &rows[count++];
    *grid = (ViewRow){.checked = place->move_grid};
    snprintf(grid->text, sizeof(grid->text), "Snap moves to the %.4g m grid",
             (double)state->grid_spacing);
    ViewRow *turn = &rows[count++];
    *turn = (ViewRow){.checked = place->turn_steps};
    snprintf(turn->text, sizeof(turn->text), "Rotate in %.0f\xc2\xb0 steps",
             (double)VKR_EDITOR_TURN_STEP_DEGREES);
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Rotation  %.0f\xc2\xb0",
             (double)place->yaw_degrees);
    rows[count++] = (ViewRow){.kind = VIEW_ROW_SLIDER,
                              .value = place->yaw_degrees,
                              .minimum = 0.0f,
                              .maximum = 360.0f};
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Offset  %.2f m",
             (double)place->offset);
    rows[count++] = (ViewRow){.kind = VIEW_ROW_SLIDER,
                              .value = place->offset,
                              .minimum = -2.0f,
                              .maximum = 2.0f};
    break;
  }
  case VIEW_POPUP_SPEED: {
    const float32_t speed = state->camera_speed;
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text), "Camera speed  %.3g u/s",
             (double)speed);
    rows[count++] = (ViewRow){.kind = VIEW_ROW_SLIDER,
                              .value = view_speed_fraction(speed),
                              .minimum = 0.0f,
                              .maximum = 1.0f};
    const float32_t sensitivity = state->camera_sensitivity;
    rows[count] = (ViewRow){.kind = VIEW_ROW_HEADER};
    snprintf(rows[count++].text, sizeof(rows[0].text),
             "Mouse sensitivity  %.1f", (double)sensitivity);
    rows[count++] = (ViewRow){.kind = VIEW_ROW_SLIDER,
                              .value = sensitivity,
                              .minimum = VIEW_SENSITIVITY_MIN,
                              .maximum = VIEW_SENSITIVITY_MAX};
    if (frame->graphics) {
      ViewRow *invert = &rows[count++];
      *invert = (ViewRow){.checked = frame->graphics->settings.invert_mouse_y};
      snprintf(invert->text, sizeof(invert->text), "Invert mouse Y");
    }
    break;
  }
  default:
    break;
  }
  return count;
}

static float32_t view_row_height(const ViewRow *row) {
  return row->kind == VIEW_ROW_HEADER ? VIEW_POPUP_HEADER_PT
                                      : VIEW_POPUP_ROW_PT;
}

static void view_popup_layout(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  ViewRow rows[VIEW_POPUP_ROW_MAX];
  const uint32_t count =
      view_popup_rows(editor, frame, editor->view_popup, rows);
  if (!count) {
    editor->view_popup_rect_pt = (Vec4){0};
    return;
  }
  float32_t height = 10.0f;
  for (uint32_t i = 0; i < count; ++i) {
    height += view_row_height(&rows[i]);
  }
  const float32_t scale = frame->ui->content_scale;
  const float32_t screen_w = (float32_t)frame->ui->target_width / scale;
  const float32_t screen_h = (float32_t)frame->ui->target_height / scale;
  const Vec2 size = {VIEW_POPUP_WIDTH_PT, height};
  const Vec4 anchor = editor->view_popup_anchor_pt;
  /* A popup that would leave the Scene opens leftward from its chip. */
  const Vec4 scene = view_scene_rect(frame);
  float32_t x = anchor.x;
  if (x + size.x > scene.x + scene.z) {
    x = anchor.x + anchor.z - size.x;
  }
  x = vkr_clamp_f32(x, 0, Max(0.0f, screen_w - size.x));
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

static bool8_t viewport_ray(const VkrSampleUiFrame *frame, Vec2 pixel,
                            Vec3 *origin, Vec3 *direction);
static bool8_t viewport_unproject(const VkrSampleUiFrame *frame, Vec2 pixel,
                                  float32_t depth, Vec3 *out);
static bool8_t grid_eye(const VkrSampleUiFrame *frame, Vec3 *eye);
static bool8_t face_axis_closest(Vec3 point, Vec3 axis, Vec3 origin,
                                 Vec3 direction, float32_t *out_s,
                                 float32_t *out_gap);
static void viewport_brush_create(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame);
static bool8_t place_raycast_frame(const VkrSampleUiFrame *frame, Vec3 origin,
                                   Vec3 displacement, VkrPhysicsRayHit *hit);

/* The point under a window pixel on the horizontal plane at `height`, moved
   to the nearest grid crossing when `grid` is set. */
static bool8_t brush_plane_point(const VkrSampleUiFrame *frame, Vec2 pixel,
                                 float32_t height, bool8_t grid, Vec3 *out) {
  Vec3 origin = {0};
  Vec3 direction = {0};
  if (!viewport_ray(frame, pixel, &origin, &direction) ||
      fabsf(direction.y) < 1.0e-5f) {
    return false_v;
  }
  const float32_t t = (height - origin.y) / direction.y;
  if (!(t > 0.0f)) {
    return false_v;
  }
  Vec3 point = vec3_add(origin, vec3_scale(direction, t));
  if (grid) {
    const float32_t cell = frame->view_state.grid_spacing > 0.0f
                               ? frame->view_state.grid_spacing
                               : 1.0f;
    point.x = roundf(point.x / cell) * cell;
    point.z = roundf(point.z / cell) * cell;
  }
  point.y = height;
  *out = point;
  return true_v;
}

/* The height a brush drag starts at. With Surface snapping, or `surface`
   (stairs and corridors), it is the upward-facing solid under the pointer,
   so what is drawn on another brush sits on top of it; otherwise, and over a
   wall or nothing, the grid plane. */
static float32_t brush_draw_base(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, Vec2 pixel,
                                 bool8_t surface) {
  const float32_t ground = frame->view_state.grid_height;
  if (!surface && editor->placement.target != VKR_EDITOR_SNAP_SURFACE) {
    return ground;
  }
  Vec3 origin = {0};
  Vec3 direction = {0};
  VkrPhysicsRayHit hit = {0};
  if (!viewport_ray(frame, pixel, &origin, &direction) ||
      !place_raycast_frame(frame, origin, vec3_scale(direction, 1000.0f),
                           &hit) ||
      hit.normal[1] < 0.5f) {
    return ground;
  }
  return hit.position[1];
}

/* A corner of a box being drawn, moved onto the sides of brushes beside it
   (editor_level.h). */
static Vec3 brush_draw_magnet(const VkrEditorUi *editor, Vec3 point, Vec3 eye) {
  point.x = vkr_editor_magnet_value(editor, 0u, point.x, point, point, eye);
  point.z = vkr_editor_magnet_value(editor, 2u, point.z, point, point, eye);
  return point;
}

static void viewport_brush_create(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame);

/* The world axis an orthographic view looks along, or -1 in perspective. */
static int32_t brush_draw_depth_axis(const VkrSampleUiFrame *frame) {
  switch (frame->view_state.camera_view) {
  case VKR_SAMPLE_CAMERA_TOP:
  case VKR_SAMPLE_CAMERA_BOTTOM:
    return 1;
  case VKR_SAMPLE_CAMERA_LEFT:
  case VKR_SAMPLE_CAMERA_RIGHT:
    return 0;
  case VKR_SAMPLE_CAMERA_FRONT:
  case VKR_SAMPLE_CAMERA_BACK:
    return 2;
  default:
    return -1;
  }
}

/* How deep a box drawn flat reaches along `axis`: as deep as the selected
   object's box, so walls drawn in a side view match a floor drawn from the
   top; else 1 m from the surface under the pointer in a top or bottom view,
   or from the depth the view is framed on, in `step` increments. */
static void brush_draw_depth(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             Vec2 pixel, uint32_t axis, float32_t step,
                             float32_t *out_lo, float32_t *out_hi) {
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene = vkr_editor_entity_scene(frame, selected);
  VkrBrushGeometry *scratch =
      scene && vkr_scene_entity_alive(scene, selected)
          ? vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*scratch),
                                VKR_ALLOCATOR_MEMORY_TAG_STRUCT)
          : NULL;
  Vec3 lo = {0};
  Vec3 hi = {0};
  if (scratch &&
      vkr_editor_entity_world_box(scene, selected, scratch, &lo, &hi) &&
      hi.elements[axis] - lo.elements[axis] > 1.0e-3f) {
    *out_lo = lo.elements[axis];
    *out_hi = hi.elements[axis];
    return;
  }
  /* The middle of an orthographic depth range is where the view is framed. */
  const Vec4 image = frame->mapping.image_rect_px;
  Vec3 focus = {0};
  *out_lo = 0.0f;
  if (axis == 1u) {
    *out_lo = brush_draw_base(editor, frame, pixel, false_v);
  } else if (viewport_unproject(
                 frame,
                 (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f},
                 0.5f, &focus)) {
    *out_lo = roundf(focus.elements[axis] / step) * step;
  }
  *out_hi = *out_lo + 1.0f;
}

/* An orthographic view draws a box flat: a drag outlines it on the two
   screen axes in `step` increments and the release creates it, as deep as
   brush_draw_depth says. */
static void viewport_brush_draw_flat(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t axis, float32_t step) {
  VkrUiSystem *ui = frame->ui;
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  Vec3 origin = {0};
  Vec3 direction = {0};
  if (!viewport_ray(frame, mouse, &origin, &direction)) {
    return;
  }
  /* The pointer's place on the screen axes, snapped; the magnet takes the
     sides of brushes beside it. */
  Vec3 point = origin;
  for (uint32_t a = 0; a < 3u; ++a) {
    if (a != axis) {
      point.elements[a] = roundf(point.elements[a] / step) * step;
    }
  }
  if (!editor->brush_dragging) {
    const bool8_t alt = input_is_key_down(frame->input, KEY_LMENU) ||
                        input_is_key_down(frame->input, KEY_RMENU);
    if (!ui->mouse_pressed || !editor->scene_pointer_free ||
        editor->menu != VKR_EDITOR_MENU_NONE || editor->face_dragging ||
        editor->face_handle_hot >= 0 || alt ||
        vkr_editor_brush_grid_busy(editor) ||
        vkr_editor_blockout_busy(editor)) {
      return;
    }
    vkr_editor_magnet_begin(editor, frame);
    float32_t lo = 0.0f;
    float32_t hi = 0.0f;
    brush_draw_depth(editor, frame, mouse, axis, step, &lo, &hi);
    editor->brush_dragging = true_v;
    editor->brush_draw_flat = true_v;
    editor->brush_draw_start = point;
    editor->brush_draw_start.elements[axis] = lo;
    editor->brush_draw_end = point;
    editor->brush_draw_end.elements[axis] = hi;
    return;
  }
  for (uint32_t a = 0; a < 3u; ++a) {
    if (a != axis) {
      editor->brush_draw_end.elements[a] = vkr_editor_magnet_value(
          editor, a, point.elements[a], point, point, origin);
    }
  }
  if (input_is_button_down(frame->input, BUTTON_LEFT) && !ui->mouse_released) {
    return;
  }
  editor->brush_dragging = false_v;
  const Vec3 size = vec3_sub(editor->brush_draw_end, editor->brush_draw_start);
  if (fabsf(size.x) < 1.0e-3f || fabsf(size.y) < 1.0e-3f ||
      fabsf(size.z) < 1.0e-3f) {
    return;
  }
  viewport_brush_create(editor, frame);
}

/* Brush drawing (ADR-084): while it is on,
   the Scene image takes the mouse from picking, a left drag outlines the
   base, then the pointer raises the box, as Chisel and Hammer draw, and a
   click creates and selects it through brush.box. Between boxes the
   selected brush keeps its face handles, and a press on one drags that face
   instead of starting a box. The Snapping menu picks the plane and
   the steps: Surface starts on the solid under the pointer, Grid on the grid
   plane, both on grid crossings and in whole cells; Free draws on the grid
   plane in 1/16 m steps. With the magnet on, corners and the top snap to
   the sides, tops and bottoms of brushes beside them. Escape cancels a box,
   or turns drawing off. An orthographic view draws flat instead
   (viewport_brush_draw_flat). */
static void viewport_brush_draw(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!editor->brush_draw || editor->clip_tool ||
      frame->scene_rendering_stopped || frame->mouse_captured) {
    editor->brush_dragging = false_v;
    editor->brush_raising = false_v;
    return;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  (void)vkr_ui_input_layer_register(
      ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
      (VkrUiRect){image.x, image.y, image.z, image.w});
  if (input_key_just_pressed(frame->input, KEY_ESCAPE) &&
      !editor->face_dragging) {
    if (editor->brush_dragging || editor->brush_raising) {
      editor->brush_dragging = false_v;
      editor->brush_raising = false_v;
    } else {
      editor->brush_draw = false_v;
    }
    return;
  }
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  const bool8_t inside = editor->scene_pointer_free;
  const bool8_t grid = editor->placement.target != VKR_EDITOR_SNAP_FREE;
  const float32_t cell = frame->view_state.grid_spacing > 0.0f
                             ? frame->view_state.grid_spacing
                             : 1.0f;
  const float32_t step = grid ? cell : 0.0625f;
  const int32_t depth_axis = brush_draw_depth_axis(frame);
  if (depth_axis >= 0 && !editor->brush_raising &&
      (!editor->brush_dragging || editor->brush_draw_flat)) {
    viewport_brush_draw_flat(editor, frame, (uint32_t)depth_axis, step);
    return;
  }
  const Vec3 a = editor->brush_draw_start;
  const Vec3 b = editor->brush_draw_end;
  const Vec3 base_center =
      vec3_new((a.x + b.x) * 0.5f, a.y, (a.z + b.z) * 0.5f);
  const Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 origin = {0};
  Vec3 direction = {0};
  float32_t along = 0.0f;
  float32_t gap = 0.0f;
  const bool8_t on_axis =
      viewport_ray(frame, mouse, &origin, &direction) &&
      face_axis_closest(base_center, up, origin, direction, &along, &gap);
  if (editor->brush_raising) {
    if (on_axis) {
      editor->brush_draw_height = Max(
          step, roundf((along - editor->brush_draw_raise_start + step) / step) *
                    step);
      /* The top levels with nearby brushes' tops and bottoms. */
      const float32_t top = a.y + editor->brush_draw_height;
      const Vec3 lo = vec3_new(Min(a.x, b.x), top, Min(a.z, b.z));
      const Vec3 hi = vec3_new(Max(a.x, b.x), top, Max(a.z, b.z));
      const float32_t snapped =
          vkr_editor_magnet_value(editor, 1u, top, lo, hi, origin);
      if (snapped - a.y > 1.0e-3f) {
        editor->brush_draw_height = snapped - a.y;
      }
    }
    if (!ui->mouse_pressed || !inside || editor->menu != VKR_EDITOR_MENU_NONE) {
      return;
    }
    editor->brush_raising = false_v;
    viewport_brush_create(editor, frame);
    return;
  }
  Vec3 point = {0};
  if (!editor->brush_dragging) {
    const bool8_t alt = input_is_key_down(frame->input, KEY_LMENU) ||
                        input_is_key_down(frame->input, KEY_RMENU);
    if (editor->face_dragging || editor->face_handle_hot >= 0 || alt ||
        vkr_editor_brush_grid_busy(editor) ||
        vkr_editor_blockout_busy(editor)) {
      return;
    }
    if (ui->mouse_pressed && inside && editor->menu == VKR_EDITOR_MENU_NONE &&
        brush_plane_point(frame, mouse,
                          brush_draw_base(editor, frame, mouse, false_v), grid,
                          &point)) {
      vkr_editor_magnet_begin(editor, frame);
      point = brush_draw_magnet(editor, point, origin);
      editor->brush_dragging = true_v;
      editor->brush_draw_flat = false_v;
      editor->brush_draw_start = point;
      editor->brush_draw_end = point;
      editor->brush_draw_height = frame->view_state.grid_spacing > 0.0f
                                      ? frame->view_state.grid_spacing
                                      : 1.0f;
    }
    return;
  }
  if (brush_plane_point(frame, mouse, editor->brush_draw_start.y, grid,
                        &point)) {
    editor->brush_draw_end = brush_draw_magnet(editor, point, origin);
  }
  if (input_is_button_down(frame->input, BUTTON_LEFT) && !ui->mouse_released) {
    return;
  }
  editor->brush_dragging = false_v;
  if (fabsf(editor->brush_draw_end.x - a.x) < 1.0e-3f ||
      fabsf(editor->brush_draw_end.z - a.z) < 1.0e-3f) {
    return;
  }
  /* The base is set; the box rises from one step with the pointer. */
  editor->brush_raising = true_v;
  editor->brush_draw_height = step;
  const Vec3 end = editor->brush_draw_end;
  float32_t start_along = 0.0f;
  editor->brush_draw_raise_start =
      viewport_ray(frame, mouse, &origin, &direction) &&
              face_axis_closest(
                  vec3_new((a.x + end.x) * 0.5f, a.y, (a.z + end.z) * 0.5f), up,
                  origin, direction, &start_along, &gap)
          ? start_along
          : 0.0f;
}

const char *const vkr_editor_brush_roles[] = {"solid", "visual", "clip",
                                              "trigger", NULL};

const VkrEditorBrushMaterial
    vkr_editor_brush_materials[VKR_EDITOR_BRUSH_MATERIAL_COUNT] = {
        {"Grid", VKR_SCENE_BRUSH_DEFAULT_MATERIAL},
        {"Floor", "assets/materials/dev/dev_floor.mt"},
        {"Wall", "assets/materials/dev/dev_wall.mt"},
        {"Orange", "assets/materials/dev/dev_orange.mt"},
        {"Blue", "assets/materials/dev/dev_blue.mt"},
        {"Clip", "assets/materials/dev/dev_clip.mt"},
        {"Trigger", "assets/materials/dev/dev_trigger.mt"},
};

void vkr_editor_brush_style(const VkrEditorUi *editor, char *out,
                            uint64_t size) {
  const uint32_t role = editor->brush_role;
  const uint32_t material = editor->brush_material;
  /* The default material is what a brush without one shows. */
  if (role > 1u || material == 0u ||
      material >= VKR_EDITOR_BRUSH_MATERIAL_COUNT) {
    snprintf(out, size, "\"role\":\"%s\"", vkr_editor_brush_roles[role]);
    return;
  }
  snprintf(out, size, "\"role\":\"%s\",\"material\":\"%s\"",
           vkr_editor_brush_roles[role],
           vkr_editor_brush_materials[material].path);
}

VkrEditorSceneTool vkr_editor_scene_tool(const VkrEditorUi *editor) {
  if (editor->brush_draw) {
    return VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW;
  }
  if (editor->clip_tool) {
    return VKR_EDITOR_SCENE_TOOL_CLIP;
  }
  if (editor->terrain_tool) {
    return VKR_EDITOR_SCENE_TOOL_TERRAIN;
  }
  return editor->path_tool;
}

void vkr_editor_scene_tool_set(VkrEditorUi *editor, VkrEditorSceneTool tool) {
  editor->brush_draw = tool == VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW;
  editor->brush_dragging = false_v;
  editor->brush_raising = false_v;
  editor->clip_tool = tool == VKR_EDITOR_SCENE_TOOL_CLIP;
  editor->terrain_tool = tool == VKR_EDITOR_SCENE_TOOL_TERRAIN;
  editor->path_tool = tool == VKR_EDITOR_SCENE_TOOL_STAIRS ||
                              tool == VKR_EDITOR_SCENE_TOOL_CORRIDOR
                          ? tool
                          : VKR_EDITOR_SCENE_TOOL_NONE;
  editor->path_count = 0u;
}

/* The `container` argument of an operation that creates in the container
   new objects go to; false when none is open. */
static bool8_t viewport_target(const VkrSampleUiFrame *frame, char *out,
                               uint64_t size) {
  const uint16_t container = vkr_editor_create_container(frame);
  if (container == UINT16_MAX) {
    return false_v;
  }
  if (container == VKR_SCENE_WORLD_ROOT_ID) {
    snprintf(out, size, "\"world\"");
  } else if (container == 0u) {
    snprintf(out, size, "\"primary\"");
  } else {
    snprintf(out, size, "%u", (unsigned)container);
  }
  return true_v;
}

/* Creates the box the brush tool outlined through brush.box, in the
   container new objects go to, with the palette's brush role and
   material. */
void vkr_editor_brush_draft_box(const VkrEditorUi *editor, Vec3 *out_lo,
                                Vec3 *out_hi) {
  const Vec3 a = editor->brush_draw_start;
  const Vec3 b = editor->brush_draw_end;
  if (editor->brush_draw_flat) {
    *out_lo = vec3_new(Min(a.x, b.x), Min(a.y, b.y), Min(a.z, b.z));
    *out_hi = vec3_new(Max(a.x, b.x), Max(a.y, b.y), Max(a.z, b.z));
    return;
  }
  *out_lo = vec3_new(Min(a.x, b.x), a.y, Min(a.z, b.z));
  *out_hi =
      vec3_new(Max(a.x, b.x), a.y + editor->brush_draw_height, Max(a.z, b.z));
}

static void viewport_brush_create(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame) {
  Vec3 lo = {0};
  Vec3 hi = {0};
  vkr_editor_brush_draft_box(editor, &lo, &hi);
  char target[16];
  if (!viewport_target(frame, target, sizeof(target))) {
    return;
  }
  char style[160];
  vkr_editor_brush_style(editor, style, sizeof(style));
  char line[480];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"draw\",\"op\":\"brush.box\",\"args\":{"
           "\"min\":[%g,%g,%g],\"max\":[%g,%g,%g],\"container\":%s,"
           "%s,\"grid\":0,\"review\":false,\"select\":true}}",
           lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, target, style);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

/* The selected brush: the selection itself, or the brush of a selected
   face. */
static VkrEntityId viewport_selected_brush(const VkrSampleUiFrame *frame) {
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene = vkr_editor_entity_scene(frame, selected);
  if (!scene || !vkr_scene_entity_alive(scene, selected)) {
    return VKR_ENTITY_ID_INVALID;
  }
  if (vkr_scene_get_typed(scene, selected, &vkr_scene_brush_type)) {
    return selected;
  }
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, selected, scene->comp_transform);
  if (transform &&
      vkr_scene_get_typed(scene, selected, &vkr_scene_brush_face_type) &&
      vkr_scene_entity_alive(scene, transform->parent)) {
    return transform->parent;
  }
  return VKR_ENTITY_ID_INVALID;
}

/* The parameter along the line `point + s * axis` (unit axis) closest to
   the ray, and the distance between them; false when they are parallel. */
static bool8_t face_axis_closest(Vec3 point, Vec3 axis, Vec3 origin,
                                 Vec3 direction, float32_t *out_s,
                                 float32_t *out_gap) {
  const Vec3 w = vec3_sub(point, origin);
  const float32_t b = vec3_dot(axis, direction);
  const float32_t denominator = 1.0f - b * b;
  if (denominator < 1.0e-4f) {
    return false_v;
  }
  const float32_t d = vec3_dot(axis, w);
  const float32_t e = vec3_dot(direction, w);
  const float32_t s = (b * e - d) / denominator;
  const float32_t t = (e - b * d) / denominator;
  const Vec3 on_axis = vec3_add(point, vec3_scale(axis, s));
  const Vec3 on_ray = vec3_add(origin, vec3_scale(direction, t));
  *out_s = s;
  *out_gap = t > 0.0f ? vec3_length(vec3_sub(on_axis, on_ray)) : INFINITY;
  return true_v;
}

/* Fills the face handles: every face of `brush`, or only `only_face`
   when it is valid. Each sits at its face's corner average along the face's
   outward normal. */
static void viewport_face_handle_targets(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrScene *scene,
                                         VkrEntityId brush,
                                         VkrEntityId only_face) {
  VkrUiSystem *ui = frame->ui;
  editor->face_handle_count = 0u;
  VkrBrushGeometry *geometry = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(*geometry), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  VkrEntityId faces[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      geometry ? vkr_editor_brush_build(scene, brush, geometry, faces) : 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrBrushPolygon polygon = geometry->polygons[i];
    if ((only_face.u64 && faces[i].u64 != only_face.u64) ||
        polygon.count < 3u) {
      continue;
    }
    Vec3 center = {0};
    for (uint32_t c = 0; c < polygon.count; ++c) {
      center = vec3_add(center, geometry->vertices[polygon.first + c]);
    }
    const uint32_t slot = editor->face_handle_count++;
    editor->face_handle_faces[slot] = faces[i];
    editor->face_handle_centers[slot] =
        vec3_scale(center, 1.0f / (float32_t)polygon.count);
    editor->face_handle_normals[slot] = geometry->normals[i];
  }
}

/* Face handles: a drag on the arrow out of a face center moves that face
   along its normal by whole grid cells, or 1/16 m with Free snapping, and
   the release applies it through brush.move_face. */
static void viewport_face_handles(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  Vec3 mouse_origin, Vec3 mouse_direction,
                                  bool8_t has_ray, bool8_t inside) {
  VkrUiSystem *ui = frame->ui;
  if (!editor->face_handle_count) {
    editor->face_dragging = false_v;
    editor->face_handle_hot = -1;
    return;
  }
  Vec3 eye = {0};
  Vec3 middle = {0};
  for (uint32_t i = 0; i < editor->face_handle_count; ++i) {
    middle = vec3_add(middle, editor->face_handle_centers[i]);
  }
  middle = vec3_scale(middle, 1.0f / (float32_t)editor->face_handle_count);
  const float32_t view_distance =
      grid_eye(frame, &eye) ? vec3_length(vec3_sub(eye, middle)) : 10.0f;
  /* The arrows keep about the same size on screen. */
  editor->face_handle_length = Max(0.25f, 0.08f * view_distance);
  if (editor->face_dragging) {
    /* The dragged face keeps its slot; a rebuild mid-drag could reorder. */
    const int32_t hot = editor->face_handle_hot;
    if (hot < 0 || (uint32_t)hot >= editor->face_handle_count ||
        editor->face_handle_faces[hot].u64 != editor->face_drag_face.u64) {
      editor->face_dragging = false_v;
      editor->face_handle_hot = -1;
      return;
    }
  } else {
    editor->face_handle_hot = -1;
    float32_t best = INFINITY;
    for (uint32_t i = 0; has_ray && i < editor->face_handle_count; ++i) {
      float32_t s = 0.0f;
      float32_t gap = INFINITY;
      if (face_axis_closest(editor->face_handle_centers[i],
                            editor->face_handle_normals[i], mouse_origin,
                            mouse_direction, &s, &gap) &&
          s >= 0.0f && s <= editor->face_handle_length &&
          gap < 0.03f * view_distance && gap < best) {
        best = gap;
        editor->face_handle_hot = (int32_t)i;
      }
    }
  }
  const int32_t hot = editor->face_handle_hot;
  const Vec4 image = frame->mapping.image_rect_px;
  if (hot >= 0 && (editor->face_dragging || inside)) {
    /* The Scene stops picking objects under a handle. */
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
  }
  if (hot < 0) {
    return;
  }
  float32_t s = 0.0f;
  float32_t gap = INFINITY;
  const bool8_t on_axis =
      has_ray && face_axis_closest(editor->face_handle_centers[hot],
                                   editor->face_handle_normals[hot],
                                   mouse_origin, mouse_direction, &s, &gap);
  if (!editor->face_dragging) {
    if (on_axis && inside && ui->mouse_pressed) {
      editor->face_dragging = true_v;
      editor->face_drag_face = editor->face_handle_faces[hot];
      editor->face_drag_start = s;
      editor->face_drag_distance = 0.0f;
    }
    return;
  }
  const float32_t step = editor->placement.target == VKR_EDITOR_SNAP_FREE
                             ? 0.0625f
                             : (frame->view_state.grid_spacing > 0.0f
                                    ? frame->view_state.grid_spacing
                                    : 1.0f);
  if (on_axis) {
    editor->face_drag_distance =
        roundf((s - editor->face_drag_start) / step) * step;
  }
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->face_dragging = false_v;
    return;
  }
  if (input_is_button_down(frame->input, BUTTON_LEFT) && !ui->mouse_released) {
    return;
  }
  editor->face_dragging = false_v;
  if (fabsf(editor->face_drag_distance) < 1.0e-3f) {
    return;
  }
  const VkrEntityId face = editor->face_drag_face;
  char line[256];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"face\",\"op\":\"brush.move_face\","
           "\"args\":{\"face\":\"%u:%u:%u\",\"distance\":%g,"
           "\"review\":false}}",
           (unsigned)face.parts.world, (unsigned)face.parts.index,
           (unsigned)face.parts.generation, editor->face_drag_distance);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

/* The world axis nearest `direction`, signed; `level` keeps to X and Z. */
static Vec3 nudge_axis(Vec3 direction, bool8_t level) {
  uint32_t best = 0u;
  for (uint32_t axis = 1u; axis < 3u; ++axis) {
    if ((!level || axis != 1u) &&
        fabsf(direction.elements[axis]) > fabsf(direction.elements[best])) {
      best = axis;
    }
  }
  Vec3 out = vec3_zero();
  out.elements[best] = direction.elements[best] < 0.0f ? -1.0f : 1.0f;
  return out;
}

/* Whether a parent of `entity` is in `selection`, which then carries it. */
static bool8_t nudge_carried(const VkrScene *scene, VkrEntityId entity,
                             const VkrEntityId *selection, uint32_t count) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  for (uint32_t depth = 0; transform && transform->parent.u64 && depth < 64u;
       ++depth) {
    for (uint32_t i = 0; i < count; ++i) {
      if (selection[i].u64 == transform->parent.u64) {
        return true_v;
      }
    }
    transform = vkr_entity_get_component(scene->world, transform->parent,
                                         scene->comp_transform);
  }
  return false_v;
}

/* The arrow keys nudge the selection one grid step along the world axis
   nearest the screen's: Left and Right along its right, Up and Down along
   its up in an orthographic view and away from the eye in perspective,
   Page Up and Page Down along Y. One batch moves every selected object as
   one undo step. */
static void viewport_nudge(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                           VkrEntityId selected, const VkrScene *scene) {
  static const Keys keys[] = {KEY_LEFT, KEY_RIGHT, KEY_UP,
                              KEY_DOWN, KEY_PRIOR, KEY_NEXT};
  uint32_t pressed = UINT32_MAX;
  for (uint32_t i = 0; i < ArrayCount(keys) && pressed == UINT32_MAX; ++i) {
    if (input_key_just_pressed(frame->input, keys[i]) &&
        input_key_press_modifiers(frame->input, keys[i]) == 0u) {
      pressed = i;
    }
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 center = {image.x + image.z * 0.5f, image.y + image.w * 0.5f};
  Vec3 at = {0};
  Vec3 right = {0};
  Vec3 up = {0};
  Vec3 eye = {0};
  Vec3 forward = {0};
  if (pressed == UINT32_MAX || !viewport_unproject(frame, center, 0.5f, &at) ||
      !viewport_unproject(frame, (Vec2){center.x + 1.0f, center.y}, 0.5f,
                          &right) ||
      !viewport_unproject(frame, (Vec2){center.x, center.y - 1.0f}, 0.5f,
                          &up) ||
      !viewport_ray(frame, center, &eye, &forward)) {
    return;
  }
  const bool8_t perspective =
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
  Vec3 axis = vec3_new(0.0f, 1.0f, 0.0f);
  if (pressed < 2u) {
    axis = nudge_axis(vec3_sub(right, at), perspective);
  } else if (pressed < 4u) {
    axis = perspective ? nudge_axis(forward, true_v)
                       : nudge_axis(vec3_sub(up, at), false_v);
  }
  const float32_t step = frame->view_state.grid_spacing > 0.0f
                             ? frame->view_state.grid_spacing
                             : 0.0625f;
  const bool8_t negative = (pressed & 1u) == ((pressed < 2u) ? 0u : 1u);
  const Vec3 offset = vec3_scale(axis, negative ? -step : step);

  /* The primary, then the rest of the selection in its scene; faces move
     with their brushes and children with a selected parent. */
  VkrEntityId selection[1u + VKR_EDITOR_SELECTION_MAX] = {selected};
  uint32_t count = 1u;
  for (uint32_t i = 0; i < editor->selection_extra_count; ++i) {
    const VkrEntityId extra = editor->selection_extra[i];
    if (extra.u64 != selected.u64 &&
        extra.parts.world == selected.parts.world &&
        vkr_scene_entity_alive(scene, extra)) {
      selection[count++] = extra;
    }
  }
  char line[8192];
  int32_t length = snprintf(line, sizeof(line),
                            "{\"v\":1,\"id\":\"nudge\",\"op\":\"batch\","
                            "\"args\":{\"label\":\"Nudge\",\"review\":false,"
                            "\"ops\":[");
  uint32_t moved = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (vkr_scene_get_typed(scene, selection[i], &vkr_scene_brush_face_type) ||
        nudge_carried(scene, selection[i], selection, count)) {
      continue;
    }
    length +=
        snprintf(line + length, sizeof(line) - (size_t)length,
                 "%s{\"op\":\"entity.move\",\"args\":{\"entity\":\"%u:%u:%u\","
                 "\"offset\":[%g,%g,%g]}}",
                 moved ? "," : "", (unsigned)selection[i].parts.world,
                 (unsigned)selection[i].parts.index,
                 (unsigned)selection[i].parts.generation, (double)offset.x,
                 (double)offset.y, (double)offset.z);
    moved++;
  }
  snprintf(line + length, sizeof(line) - (size_t)length, "]}}");
  if (moved) {
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  frame->ui->capture.keyboard = true_v;
}

/* Face editing (ADR-084): a brush selected with the Select tool shows a
   handle on every face and a drag pushes or pulls that face; Alt+click
   selects the brush face under the pointer instead of its object, which
   then shows its handle alone, and Alt+Up or Alt+Down moves it 0.25 m out
   or in along its normal, 1 m with Shift, through brush.move_face. */
static void viewport_face_tools(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  editor->face_handle_count = 0u;
  if (frame->scene_rendering_stopped || frame->mouse_captured ||
      editor->cmd_active || editor->menu != VKR_EDITOR_MENU_NONE ||
      (editor->brush_draw &&
       (editor->brush_dragging || editor->brush_raising)) ||
      editor->path_tool != VKR_EDITOR_SCENE_TOOL_NONE) {
    editor->face_dragging = false_v;
    editor->face_handle_hot = -1;
    vkr_editor_brush_grid_update(editor, frame, NULL, VKR_ENTITY_ID_INVALID,
                                 VKR_EDITOR_BRUSH_GRID_EDIT, vec3_zero(),
                                 vec3_zero(), false_v, false_v, true_v);
    return;
  }
  const bool8_t alt = input_is_key_down(frame->input, KEY_LMENU) ||
                      input_is_key_down(frame->input, KEY_RMENU);
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  const bool8_t inside = editor->scene_pointer_free;
  Vec3 origin = {0};
  Vec3 direction = {0};
  const bool8_t has_ray = viewport_ray(frame, mouse, &origin, &direction);
  if (editor->clip_tool) {
    /* The Clip tool cuts on the selected brush's grid, with any transform
       tool. */
    editor->face_dragging = false_v;
    editor->face_handle_hot = -1;
    const VkrEntityId brush = viewport_selected_brush(frame);
    vkr_editor_brush_grid_update(editor, frame,
                                 vkr_editor_entity_scene(frame, brush), brush,
                                 VKR_EDITOR_BRUSH_GRID_CLIP, origin, direction,
                                 has_ray, inside, false_v);
    return;
  }
  if (alt && inside && !editor->face_dragging) {
    /* The Scene image stops picking objects while Alt is held. */
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
    VkrEntityId face = VKR_ENTITY_ID_INVALID;
    if (ui->mouse_pressed && has_ray &&
        vkr_editor_brush_pick(frame, origin, direction, 100000.0f, &face)) {
      *frame->scene_edit = (VkrSceneEditRequest){
          .action = VKR_SCENE_EDIT_SELECT, .entity = face};
    }
  }
  const bool8_t scene_focus =
      frame->scene_keyboard_focus && *frame->scene_keyboard_focus;
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene = vkr_editor_entity_scene(frame, selected);
  const bool8_t alive = scene && vkr_scene_entity_alive(scene, selected);
  const bool8_t face_selected =
      alive && vkr_scene_get_typed(scene, selected, &vkr_scene_brush_face_type);
  /* A selected blockout shape shows its handles with the Select tool. */
  vkr_editor_blockout_update(
      editor, frame,
      frame->view_state.gizmo_tool == VKR_GIZMO_MODE_NONE && alive
          ? selected
          : VKR_ENTITY_ID_INVALID,
      origin, direction, has_ray, inside && !alt);
  /* A brush, or a corridor's wall, selected with the Select tool shows its
     grid; the transform tools keep their gizmo instead. */
  const bool8_t brush_selected =
      alive && frame->view_state.gizmo_tool == VKR_GIZMO_MODE_NONE &&
      (vkr_scene_get_typed(scene, selected, &vkr_scene_brush_type) ||
       vkr_scene_get_typed(scene, selected, &vkr_scene_blockout_type));
  if (face_selected) {
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, selected, scene->comp_transform);
    viewport_face_handle_targets(
        editor, frame, scene,
        transform ? transform->parent : VKR_ENTITY_ID_INVALID, selected);
  }
  if (!alt || editor->face_dragging) {
    viewport_face_handles(editor, frame, origin, direction, has_ray, inside);
  }
  /* While boxes are drawn, cells start boxes rather than patches. */
  vkr_editor_brush_grid_update(
      editor, frame, scene, brush_selected ? selected : VKR_ENTITY_ID_INVALID,
      editor->brush_draw ? VKR_EDITOR_BRUSH_GRID_DRAW
                         : VKR_EDITOR_BRUSH_GRID_EDIT,
      origin, direction, has_ray, inside,
      alt || editor->face_handle_hot >= 0 || editor->face_dragging ||
          vkr_editor_blockout_busy(editor));
  if (!scene_focus && ui->focused_id != VKR_UI_ID_NONE) {
    return;
  }
  if (!face_selected) {
    if (alive && !frame->simulation_running) {
      viewport_nudge(editor, frame, selected, scene);
    }
    return;
  }
  static const Keys keys[] = {KEY_UP, KEY_DOWN};
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    const uint8_t modifiers = input_key_press_modifiers(frame->input, keys[i]);
    if (!input_key_just_pressed(frame->input, keys[i]) ||
        (modifiers & (VKR_INPUT_MOD_ALT | VKR_INPUT_MOD_CONTROL |
                      VKR_INPUT_MOD_SUPER)) != VKR_INPUT_MOD_ALT) {
      continue;
    }
    const float32_t step =
        (modifiers & VKR_INPUT_MOD_SHIFT) != 0 ? 1.0f : 0.25f;
    char line[256];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"face\",\"op\":\"brush.move_face\","
             "\"args\":{\"face\":\"%u:%u:%u\",\"distance\":%g,"
             "\"review\":false}}",
             (unsigned)selected.parts.world, (unsigned)selected.parts.index,
             (unsigned)selected.parts.generation, i == 0u ? step : -step);
    (void)vkr_editor_agent_submit(editor->agent, line);
    ui->capture.keyboard = true_v;
  }
}

/* A click of a two-click tool that the grid plane cannot take says why
   instead of doing nothing: the eye is level with the plane, as the default
   camera at ground height is, or the view looks above it. */
static void viewport_plane_missed(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, Vec2 mouse,
                                  bool8_t inside) {
  if (!frame->ui->mouse_pressed || !inside ||
      editor->menu != VKR_EDITOR_MENU_NONE) {
    return;
  }
  Vec3 origin = {0};
  Vec3 direction = {0};
  const bool8_t level = viewport_ray(frame, mouse, &origin, &direction) &&
                        fabsf(origin.y - frame->view_state.grid_height) < 0.05f;
  vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                   level ? "The camera is level with the ground grid; raise "
                           "it (hold the right button and press E) to click "
                           "on the grid"
                         : "Click on the ground grid; look down at it first");
}

/* Brush clipping (ADR-084): the selected brush shows its face grid, and
   the grid takes the clicks that cut it (editor_brush_grid.h). Without a
   brush the Scene keeps picking, so a click selects one. Escape drops the
   cut's points, or turns the tool off. */
static void viewport_clip_tool(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (!editor->clip_tool || frame->scene_rendering_stopped ||
      frame->mouse_captured) {
    return;
  }
  if (input_key_just_pressed(frame->input, KEY_ESCAPE) &&
      !vkr_editor_brush_grid_clip_pending(editor)) {
    editor->clip_tool = false_v;
  }
}

/* The stairs the stairs tool places from a drag `from` toward `to` in the
   palette's kind and turn: straight, L and U stairs run as far as the drag,
   a spiral reaches it with its rim, and a short drag or a click leaves the
   defaults. Its yaw (degrees about +Y) faces the drag, or away from the
   camera. */
static SceneBlockout viewport_stairs_shape(const VkrEditorUi *editor, Vec3 from,
                                           Vec3 to, Vec3 facing,
                                           float32_t *out_yaw) {
  SceneBlockout shape;
  vkr_type_defaults(&vkr_scene_blockout_type, &shape);
  shape.shape = SCENE_BLOCKOUT_STAIRS;
  shape.stairs = (SceneStairsKind)editor->stairs_kind;
  shape.left = editor->stairs_left;
  if (shape.stairs == SCENE_STAIRS_CURVED) {
    shape.radius = 2.0f;
    shape.turn = 90.0f;
  } else if (shape.stairs == SCENE_STAIRS_SPIRAL) {
    shape.radius = 0.3f;
    shape.turn = 360.0f;
    shape.width = 1.2f;
  }
  Vec3 run = vec3_new(to.x - from.x, 0.0f, to.z - from.z);
  const float32_t reach = vec3_length(run);
  if (reach < 0.5f) {
    run = vec3_new(facing.x, 0.0f, facing.z);
  } else if (shape.stairs == SCENE_STAIRS_SPIRAL) {
    shape.width = Max(reach - shape.radius, 0.5f);
  } else if (shape.stairs != SCENE_STAIRS_CURVED) {
    shape.length =
        Max(reach,
            shape.stairs == SCENE_STAIRS_STRAIGHT ? 0.5f : shape.width + 1.0f);
  }
  *out_yaw =
      vec3_length(run) > 1.0e-4f ? atan2f(run.x, run.z) * 57.2957795f : 0.0f;
  return shape;
}

bool8_t vkr_editor_path_stairs(const VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               SceneBlockout *out, Vec3 *out_from,
                               float32_t *out_yaw) {
  (void)frame;
  if (editor->path_tool != VKR_EDITOR_SCENE_TOOL_STAIRS ||
      !editor->path_count) {
    return false_v;
  }
  *out_from = editor->path_points[0];
  *out =
      viewport_stairs_shape(editor, editor->path_points[0],
                            editor->path_current, vec3_new(0, 0, 1), out_yaw);
  return true_v;
}

/* Stairs and corridors (ADR-084). Stairs: a drag on the grid plane, or on
   the surface under the pointer with Surface snapping, places editable
   stairs in the palette's kind from where it starts toward where it ends
   (a click faces them away from the camera); their handles and Details
   shape them afterwards. Corridors: each click adds a floor point; a click
   on the last point again, or Enter, builds an editable corridor through
   them, its corners rounded when the palette says Curved, and Backspace
   drops the last point. Escape steps back, then turns the tool off. */
static void viewport_path_tool(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (editor->path_tool == VKR_EDITOR_SCENE_TOOL_NONE ||
      frame->scene_rendering_stopped || frame->mouse_captured) {
    editor->path_count = 0u;
    return;
  }
  const bool8_t stairs = editor->path_tool == VKR_EDITOR_SCENE_TOOL_STAIRS;
  const Vec4 image = frame->mapping.image_rect_px;
  (void)vkr_ui_input_layer_register(
      ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
      (VkrUiRect){image.x, image.y, image.z, image.w});
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    if (editor->path_count) {
      editor->path_count = 0u;
    } else {
      vkr_editor_scene_tool_set(editor, VKR_EDITOR_SCENE_TOOL_NONE);
    }
    return;
  }
  if (!stairs && editor->path_count &&
      input_key_just_pressed(frame->input, KEY_BACKSPACE)) {
    editor->path_count--;
    return;
  }
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  const bool8_t inside = editor->scene_pointer_free;
  const bool8_t pressed =
      ui->mouse_pressed && inside && editor->menu == VKR_EDITOR_MENU_NONE;
  const bool8_t grid = editor->placement.target != VKR_EDITOR_SNAP_FREE;
  Vec3 origin = {0};
  Vec3 direction = {0};
  (void)viewport_ray(frame, mouse, &origin, &direction);

  if (stairs && editor->path_count) {
    /* The drag runs until the release places the stairs. */
    Vec3 point = {0};
    if (brush_plane_point(frame, mouse, editor->path_points[0].y, grid,
                          &point)) {
      editor->path_current = point;
    }
    if (input_is_button_down(frame->input, BUTTON_LEFT) &&
        !ui->mouse_released) {
      return;
    }
    editor->path_count = 0u;
    char target[16];
    if (!viewport_target(frame, target, sizeof(target)) || !editor->agent) {
      return;
    }
    float32_t yaw = 0.0f;
    SceneBlockout shape = viewport_stairs_shape(
        editor, editor->path_points[0], editor->path_current, direction, &yaw);
    char style[160];
    vkr_editor_brush_style(editor, style, sizeof(style));
    if (editor->brush_material > 0u &&
        editor->brush_material < VKR_EDITOR_BRUSH_MATERIAL_COUNT &&
        editor->brush_role <= 1u) {
      snprintf(shape.material, sizeof(shape.material), "%s",
               vkr_editor_brush_materials[editor->brush_material].path);
    }
    char values[4096];
    if (!vkr_editor_ops_component_text(&vkr_scene_blockout_type, &shape, values,
                                       sizeof(values))) {
      return;
    }
    const Vec3 at = editor->path_points[0];
    char line[4608];
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"path\",\"op\":\"blockout.create\",\"args\":{"
             "\"shape\":\"stairs\",\"position\":[%g,%g,%g],\"yaw\":%g,"
             "\"values\":%s,\"container\":%s,%s,\"grid\":0,"
             "\"review\":false,\"select\":true}}",
             at.x, at.y, at.z, yaw, values, target, style);
    (void)vkr_editor_agent_submit(editor->agent, line);
    return;
  }

  /* Points lie on the plane of the first one; the first starts on the
     surface under the pointer with Surface snapping. */
  Vec3 point = {0};
  const float32_t height = editor->path_count
                               ? editor->path_points[0].y
                               : brush_draw_base(editor, frame, mouse, true_v);
  if (!brush_plane_point(frame, mouse, height, grid, &point)) {
    viewport_plane_missed(editor, frame, mouse, inside);
    return;
  }
  editor->path_current = point;
  const bool8_t finish = !stairs && editor->path_count >= 2u &&
                         input_key_just_pressed(frame->input, KEY_ENTER);
  if (!pressed && !finish) {
    return;
  }
  if (stairs) {
    editor->path_points[0] = point;
    editor->path_current = point;
    editor->path_count = 1u;
    return;
  }

  /* Corridors: a click on the last point again finishes them. */
  const bool8_t again =
      editor->path_count &&
      vec3_length(vec3_sub(
          point, editor->path_points[editor->path_count - 1u])) < 0.25f;
  if (!finish && !again) {
    if (editor->path_count < ArrayCount(editor->path_points)) {
      editor->path_points[editor->path_count++] = point;
    }
    return;
  }
  if (editor->path_count < 2u) {
    return;
  }
  char target[16];
  if (!viewport_target(frame, target, sizeof(target))) {
    return;
  }
  char line[2048];
  int used = snprintf(line, sizeof(line),
                      "{\"v\":1,\"id\":\"path\",\"op\":"
                      "\"blockout.corridor\",\"args\":{\"points\":[");
  for (uint32_t i = 0;
       i < editor->path_count && used > 0 && (size_t)used < sizeof(line); ++i) {
    const Vec3 at = editor->path_points[i];
    used += snprintf(line + used, sizeof(line) - (size_t)used, "%s[%g,%g,%g]",
                     i ? "," : "", at.x, at.y, at.z);
  }
  if (used > 0 && (size_t)used < sizeof(line)) {
    snprintf(line + used, sizeof(line) - (size_t)used,
             "],\"curved\":%s,\"container\":%s,\"grid\":0,"
             "\"review\":false,\"select\":true}}",
             editor->corridor_curved ? "true" : "false", target);
    (void)vkr_editor_agent_submit(editor->agent, line);
  }
  editor->path_count = 0u;
}

/* One line under the Scene's centre that says what the running tool does
   with the next click. */
static void viewport_tool_hint(const VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  const char *hint = NULL;
  switch (vkr_editor_scene_tool(editor)) {
  case VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW:
    hint = editor->brush_raising
               ? "Move up to set the box's height, then click. Esc cancels."
           : frame->view_state.camera_view != VKR_SAMPLE_CAMERA_PERSPECTIVE
               ? "Drag to draw a box brush, as deep as the selection or 1 m. "
                 "Esc stops."
               : "Drag to draw a box brush's base. Esc stops.";
    break;
  case VKR_EDITOR_SCENE_TOOL_CLIP:
    hint = vkr_editor_brush_grid_hint(editor);
    if (!hint) {
      hint = "Click a brush to cut it on its grid. Esc stops.";
    }
    break;
  case VKR_EDITOR_SCENE_TOOL_TERRAIN:
    hint = "Hold the left button over a terrain to sculpt or paint. Esc "
           "stops.";
    break;
  case VKR_EDITOR_SCENE_TOOL_STAIRS:
    hint = editor->path_count
               ? (editor->stairs_kind == SCENE_STAIRS_SPIRAL
                      ? "Release at the spiral's rim. Esc cancels."
                  : editor->stairs_kind == SCENE_STAIRS_CURVED
                      ? "Release toward where the stairs head. Esc cancels."
                      : "Release where the stairs end. Esc cancels.")
               : "Drag from where the stairs start toward where they go, or "
                 "click to place them facing away. Esc stops.";
    break;
  case VKR_EDITOR_SCENE_TOOL_CORRIDOR:
    hint = editor->path_count >= 2u
               ? "Click the next point; click the last one again or press "
                 "Enter to build, then drag its points to steer it. "
                 "Backspace drops a point, Esc all."
           : editor->path_count
               ? "Click the corridor's next point. Esc drops the start."
               : "Click where the corridor starts. Esc stops.";
    break;
  default:
    hint = vkr_editor_blockout_hint(editor);
    if (!hint) {
      hint = vkr_editor_brush_grid_hint(editor);
    }
    if (!hint) {
      return;
    }
    break;
  }
  /* A grid target under the pointer speaks for itself while drawing. */
  if (vkr_editor_scene_tool(editor) == VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW &&
      !editor->brush_raising && vkr_editor_brush_grid_busy(editor)) {
    hint = vkr_editor_brush_grid_hint(editor);
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  if (viewport.z / scale < 320.0f || viewport.w / scale < 160.0f) {
    return;
  }
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = chip.placement.row = 0u;
  chip.placement.justify = VKR_UI_ALIGN_CENTER;
  chip.placement.align = VKR_UI_ALIGN_END;
  chip.placement.margin_pt =
      (VkrUiEdges){0.0f,
                   Max(0.0f, (float32_t)ui->target_width / scale -
                                 (viewport.x + viewport.z) / scale),
                   Max(0.0f, (float32_t)ui->target_height / scale -
                                 (viewport.y + viewport.w) / scale + 44.0f),
                   viewport.x / scale};
  chip.style = vkr_editor_overlay_style();
  chip.style.padding_pt = (VkrUiEdges){5.0f, 12.0f, 5.0f, 10.0f};
  chip.style.font_size_pt = theme->font_body;
  chip.style.text_color = theme->text;
  chip.style.hover_background_color = chip.style.background_color;
  chip.icon = VKR_UI_ICON_INFO_FILL;
  chip.icon_size_pt = 13.0f;
  chip.icon_color = theme->accent_hover;
  vkr_ui_label(ui, string8_lit("viewport.tool_hint"),
               string8_create_from_cstr((const uint8_t *)hint, strlen(hint)),
               &chip);
}

void vkr_editor_viewport_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!frame->mapping_valid) {
    editor->view_toolbar_rect_pt = (Vec4){0};
    editor->view_popup = VIEW_POPUP_NONE;
    editor->scene_pointer_free = false_v;
    return;
  }
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

  /* Floating windows, menus and popups registered their layers before this
     update and the toolbars just did; the tools below register their own
     claim on the image, so they read this instead of the layer. */
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t mouse_x = (float32_t)ui->mouse_x;
  const float32_t mouse_y = (float32_t)ui->mouse_y;
  editor->scene_pointer_free =
      !frame->mouse_captured && ui->mouse_input_layer == 0u &&
      mouse_x >= image.x && mouse_y >= image.y && mouse_x < image.x + image.z &&
      mouse_y < image.y + image.w;

  viewport_clip_tool(editor, frame);
  viewport_path_tool(editor, frame);
  /* Face handles find the one under the pointer before drawing reads it, so
     a press on a handle drags the face instead of starting a box. */
  viewport_face_tools(editor, frame);
  viewport_brush_draw(editor, frame);
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

/* Dropdown chip: one button holding the icon, the value and a trailing
   caret, so the caret always sits inside the chip's padding. */
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
  chip.style.padding_pt =
      (VkrUiEdges){3, VIEW_CHIP_PAD_PT, 3, VIEW_CHIP_PAD_PT};
  chip.style.font_size_pt = theme->font_body;
  chip.style.text_color = theme->text;
  chip.icon = icon;
  chip.icon_size_pt = VIEW_CHIP_ICON_PT;
  chip.icon_color = active ? theme->accent_hover : theme->text_secondary;
  chip.trailing_icon = VKR_UI_ICON_CHEVRON_DOWN;
  chip.trailing_icon_size_pt = VIEW_CHIP_CARET_PT;
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

/* Apply one Show menu row. */
static void view_show_activate(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame, uint32_t index,
                               VkrSampleViewState *next) {
  const uint32_t value = view_show_rows[index].value;
  switch (view_show_rows[index].target) {
  case VIEW_SHOW_KIND:
    next->hidden_kinds ^= value;
    break;
  case VIEW_SHOW_ICONS:
    editor->labels_enabled = !editor->labels_enabled;
    break;
  case VIEW_SHOW_ICON_KIND: {
    bool8_t *flag = view_icon_flag(editor, value);
    *flag = !*flag;
    break;
  }
  case VIEW_SHOW_GRID:
    next->grid_enabled = !next->grid_enabled;
    break;
  case VIEW_SHOW_GRID_THROUGH:
    next->grid_through_geometry = !next->grid_through_geometry;
    break;
  case VIEW_SHOW_COLLISION:
    next->collision_display = next->collision_display == value ? 0u : value;
    break;
  case VIEW_SHOW_PHYSICS:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_TOGGLE_PHYSICS;
    break;
  case VIEW_SHOW_HEADER:
    break;
  }
}

static void view_quality_apply(const VkrSampleUiFrame *frame,
                               VkrGraphicsSettings settings) {
  *frame->graphics_request =
      (VkrGraphicsSettingsRequest){.settings = settings, .apply = true_v};
}

/* Saves `extent` as the texture limit of the open scene, or of the World
   when none is open, as an undoable scene edit. */
static void view_scene_textures_apply(const VkrSampleUiFrame *frame,
                                      uint32_t extent) {
  const VkrScene *scene = view_texture_scene(frame);
  if (!scene || !frame->scene_edit ||
      scene->settings.texture_max_extent == extent) {
    return;
  }
  VkrSceneEditRequest request = {
      .action = VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS,
      .container = frame->scene ? 0u : (uint16_t)VKR_SCENE_WORLD_ROOT_ID,
  };
  request.scene_settings = scene->settings;
  request.scene_settings.texture_max_extent = extent;
  *frame->scene_edit = request;
}

/* Apply a clicked option row; returns whether the popup stays open. */
static bool8_t view_popup_activate(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t popup, uint32_t index,
                                   VkrSampleViewState *next) {
  switch (popup) {
  case VIEW_POPUP_CAMERA:
    next->camera_view = (VkrSampleCameraView)index;
    return false_v;
  case VIEW_POPUP_RENDER:
    next->render_mode = view_render_modes[index].mode;
    return false_v;
  case VIEW_POPUP_GRID:
    if (index == 0) {
      next->grid_enabled = !next->grid_enabled;
    } else if (index == 1) {
      next->grid_through_geometry = !next->grid_through_geometry;
    } else if (index == 2) {
      next->grid_labels = !next->grid_labels;
    } else if (index == 4 || index == 5) {
      next->grid_spacing = vkr_clamp_f32(
          next->grid_spacing * (index == 4 ? 0.5f : 2.0f), 0.001f, 10000.0f);
      next->grid_enabled = true_v;
    } else if (index == 7) {
      vkr_editor_view_fit_grid(frame);
      next->grid_enabled = true_v;
    } else if (index == 8) {
      next->grid_height = 0.0f;
    }
    return true_v;
  case VIEW_POPUP_SHOW:
    view_show_activate(editor, frame, index, next);
    return true_v;
  case VIEW_POPUP_SNAP:
    /* Rows: header, the targets, cell centers, normal alignment, the
       magnet, then a header, grid moves and rotation steps. */
    if (index >= 1 && index <= VKR_EDITOR_SNAP_COUNT) {
      editor->placement.target = (VkrEditorSnapTarget)(index - 1u);
    } else if (index == VKR_EDITOR_SNAP_COUNT + 1u) {
      editor->placement.cell_centers = !editor->placement.cell_centers;
    } else if (index == VKR_EDITOR_SNAP_COUNT + 2u) {
      editor->placement.align_to_normal = !editor->placement.align_to_normal;
    } else if (index == VKR_EDITOR_SNAP_COUNT + 3u) {
      editor->placement.magnet = !editor->placement.magnet;
    } else if (index == VKR_EDITOR_SNAP_COUNT + 5u) {
      editor->placement.move_grid = !editor->placement.move_grid;
    } else if (index == VKR_EDITOR_SNAP_COUNT + 6u) {
      editor->placement.turn_steps = !editor->placement.turn_steps;
    }
    return true_v;
  case VIEW_POPUP_SPEED:
    /* Rows: speed header and slider, sensitivity header and slider, then
       Invert mouse Y, a machine-local Graphics setting. */
    if (index == 4u && frame->graphics) {
      VkrGraphicsSettings settings = frame->graphics->settings;
      settings.invert_mouse_y = !settings.invert_mouse_y;
      view_quality_apply(frame, settings);
    }
    return true_v;
  case VIEW_POPUP_QUALITY: {
    VkrGraphicsSettings settings = view_graphics(editor, frame);
    /* Rows: header, the presets, Custom, header, slider, dynamic, then the
       scene textures header and its limits. */
    const uint32_t texture_row = VKR_GRAPHICS_PRESET_CUSTOM + 6u;
    if (index >= texture_row &&
        index < texture_row + ArrayCount(view_texture_limits)) {
      view_scene_textures_apply(
          frame, view_texture_limits[index - texture_row].extent);
    } else if (index >= 1 && index <= VKR_GRAPHICS_PRESET_CUSTOM) {
      vkr_graphics_settings_apply_preset(&settings,
                                         (VkrGraphicsPreset)(index - 1u));
      view_quality_apply(frame, settings);
    } else if (index == VKR_GRAPHICS_PRESET_CUSTOM + 1u) {
      vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_GRAPHICS, true_v);
      return false_v;
    } else if (index == VKR_GRAPHICS_PRESET_CUSTOM + 4u) {
      settings.dynamic_resolution = !settings.dynamic_resolution;
      view_quality_apply(frame, settings);
    }
    return true_v;
  }
  default:
    return true_v;
  }
}

/* A slider row moved to `value`; `done` ends the gesture. Render scale
   applies once, when the drag ends, since each change resizes targets. */
static void view_popup_slide(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             uint32_t popup, uint32_t index, float32_t value,
                             bool8_t done, VkrSampleViewState *next) {
  if (popup == VIEW_POPUP_SNAP) {
    /* Rotation in 15 degree steps, offset in centimetres. */
    if (index == VKR_EDITOR_SNAP_COUNT + 8u)
      editor->placement.yaw_degrees = roundf(value / 15.0f) * 15.0f;
    else
      editor->placement.offset = roundf(value * 100.0f) / 100.0f;
    return;
  }
  if (popup == VIEW_POPUP_SPEED) {
    /* Rows: speed header and slider, then sensitivity header and slider. */
    if (index == 3u)
      next->camera_sensitivity = roundf(value * 10.0f) / 10.0f;
    else
      next->camera_speed = view_speed_value(value);
    return;
  }
  if (popup != VIEW_POPUP_QUALITY || !frame->graphics) {
    return;
  }
  /* Whole percents read cleanly beside the slider. */
  editor->view_scale_draft = roundf(value * 100.0f) / 100.0f;
  if (done) {
    VkrGraphicsSettings settings = frame->graphics->settings;
    settings.render_scale =
        vkr_clamp_f32(editor->view_scale_draft, 1.0f / 3.0f, 1.0f);
    view_quality_apply(frame, settings);
    editor->view_scale_draft = 0.0f;
  }
}

static void view_popup_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             bool8_t disabled) {
  const uint32_t popup = editor->view_popup;
  ViewRow rows[VIEW_POPUP_ROW_MAX];
  const uint32_t count = view_popup_rows(editor, frame, popup, rows);
  if (!count)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  view_popup_layout(editor, frame);
  view_register_rect(ui, editor->view_popup_rect_pt);
  VkrUiTrack tracks[VIEW_POPUP_ROW_MAX];
  for (uint32_t i = 0; i < count; ++i)
    tracks[i] = (VkrUiTrack){.value = view_row_height(&rows[i]),
                             .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig panel = view_panel(editor->view_popup_rect_pt);
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){5, 5, 5, 5};
  panel.style.gap_pt = 0;
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){editor->view_popup_rect_pt.z, editor->view_popup_rect_pt.w};
  panel.rows = tracks;
  panel.row_count = count;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.viewport.options"), &panel))
    return;
  VkrSampleViewState next = frame->view_state;
  bool8_t changed = false_v;
  for (uint32_t i = 0; i < count; ++i) {
    const ViewRow *row = &rows[i];
    VkrUiWidgetConfig item = vkr_ui_widget_config_default();
    item.placement.column = 0;
    item.placement.row = i;
    item.fill = true_v;
    item.disabled = disabled || row->disabled;
    (void)vkr_ui_push_id_u64(ui, (uint64_t)popup * 64 + i);
    if (row->kind == VIEW_ROW_HEADER) {
      item.placement.align = VKR_UI_ALIGN_END;
      item.style.padding_pt = (VkrUiEdges){6, 8, 3, 8};
      item.style.font_size_pt = theme->font_caption;
      item.style.text_color = theme->text_secondary;
      item.text.font = editor->heading_font;
      item.disabled = false_v;
      vkr_ui_label(ui, string8_lit("header"), view_string(row->text), &item);
    } else if (row->kind == VIEW_ROW_SLIDER) {
      item.placement.margin_pt = (VkrUiEdges){4, 10, 4, 10};
      item.style.min_size_pt = (Vec2){10.0f, 16.0f};
      item.tooltip = popup == VIEW_POPUP_SPEED && i == 3u
                         ? string8_lit("How far the free camera turns per "
                                       "mouse movement")
                     : popup == VIEW_POPUP_SPEED
                         ? string8_lit("Free-camera flight speed")
                     : popup == VIEW_POPUP_SNAP
                         ? string8_lit("How a placed object turns and sits "
                                       "relative to its snap point")
                         : string8_lit("Share of the output resolution the "
                                       "Scene renders before upscaling");
      float32_t value = row->value;
      const VkrUiId slider_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("slider"));
      const bool8_t moved = vkr_ui_slider_f32(
          ui, string8_lit("slider"), &value, row->minimum, row->maximum, &item);
      const bool8_t held = ui->active_id == slider_id;
      if (moved || (!held && editor->view_scale_draft > 0.0f &&
                    popup == VIEW_POPUP_QUALITY)) {
        view_popup_slide(editor, frame, popup, i, value, !held, &next);
        changed |= popup == VIEW_POPUP_SPEED;
      }
    } else {
      vkr_editor_ghost_style(&item);
      item.leading = true_v;
      item.style.hover_background_color = theme->accent;
      item.style.padding_pt = (VkrUiEdges){3, 8, 3, 8};
      item.style.text_color = theme->text;
      item.style.font_size_pt = theme->font_body;
      item.icon = row->checked ? VKR_UI_ICON_CHECK : row->icon;
      item.icon_size_pt = 14.0f;
      item.icon_color =
          row->checked ? theme->accent_hover : theme->text_secondary;
      if (item.icon == VKR_UI_ICON_NONE)
        item.style.padding_pt.left += 20.0f;
      if (vkr_ui_button(ui, string8_lit("option"), view_string(row->text),
                        &item)) {
        if (!view_popup_activate(editor, frame, popup, i, &next))
          editor->view_popup = VIEW_POPUP_NONE;
        changed = true_v;
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (changed)
    view_request(frame, next);
  (void)vkr_ui_panel_end(ui);
}

/* Camera speed: the icon and current speed; its dropdown holds a
   logarithmic slider. The wheel over the chip steps the speed. */
static void view_speed_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             uint32_t column, bool8_t disabled) {
  VkrUiSystem *ui = frame->ui;
  char value_text[24];
  view_speed_text(frame->view_state.camera_speed, value_text);
  (void)vkr_ui_push_id_label(ui, string8_lit("speed"));
  const VkrUiId chip_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("chip"));
  (void)vkr_ui_pop_id(ui);
  (void)view_chip(
      editor, ui, "speed", column, VKR_UI_ICON_CAMERA, value_text,
      editor->view_popup == VIEW_POPUP_SPEED, false_v, disabled, false_v,
      "Camera speed in units per second (scroll to adjust)", VIEW_POPUP_SPEED);
  if (!disabled && ui->mouse_wheel && ui->hot_id == chip_id) {
    VkrSampleViewState next = frame->view_state;
    next.camera_speed =
        view_speed_value(view_speed_fraction(next.camera_speed) +
                         (float32_t)ui->mouse_wheel / 24.0f);
    view_request(frame, next);
  }
}

void vkr_editor_viewport_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid)
    return;
  viewport_tool_hint(editor, frame);
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const ViewHeaderLayout layout = view_header_layout(editor, frame);
  if (layout.left.z <= 0 || layout.left.w <= 0)
    return;
  const bool8_t disabled =
      !frame->view_request || frame->scene_rendering_stopped;
  char text[VIEW_LEFT_CHIPS][48];
  view_chip_text(editor, frame, text);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER);

  VkrUiTrack left_columns[VIEW_LEFT_CHIPS];
  for (uint32_t i = 0; i < VIEW_LEFT_CHIPS; ++i) {
    left_columns[i] =
        (VkrUiTrack){.value = layout.chips[i], .unit = VKR_UI_TRACK_PX};
  }
  const VkrUiTrack chip_row = {.value = VIEW_CHIP_HEIGHT_PT,
                               .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig left = view_panel(layout.left);
  left.columns = left_columns;
  left.column_count = ArrayCount(left_columns);
  left.rows = &chip_row;
  left.row_count = 1u;
  if (vkr_ui_panel_begin(ui, string8_lit("editor.viewport.toolbar"), &left)) {
    const VkrSampleViewState *state = &frame->view_state;
    const bool8_t perspective =
        state->camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE;
    (void)view_chip(editor, ui, "camera", 0, VKR_UI_ICON_PERSPECTIVE, text[0],
                    editor->view_popup == VIEW_POPUP_CAMERA, !perspective,
                    disabled, layout.compact,
                    "Camera projection and orthographic direction",
                    VIEW_POPUP_CAMERA);
    (void)view_chip(editor, ui, "render", 1, VKR_UI_ICON_VIEW_MODE, text[1],
                    editor->view_popup == VIEW_POPUP_RENDER,
                    state->render_mode != VKR_RENDER_MODE_DEFAULT, disabled,
                    layout.compact, "Viewport rendering mode",
                    VIEW_POPUP_RENDER);
    (void)view_chip(editor, ui, "grid", 2, VKR_UI_ICON_GRID, text[2],
                    editor->view_popup == VIEW_POPUP_GRID, state->grid_enabled,
                    disabled, layout.compact,
                    "World grid: visibility, depth and cell size",
                    VIEW_POPUP_GRID);
    const bool8_t filtered = state->hidden_kinds || !editor->labels_enabled;
    (void)view_chip(editor, ui, "show", 3, VKR_UI_ICON_EYE, text[3],
                    editor->view_popup == VIEW_POPUP_SHOW, filtered, disabled,
                    layout.compact,
                    "Show or hide object types, icons and overlays",
                    VIEW_POPUP_SHOW);
    (void)view_chip(editor, ui, "quality", 4, VKR_UI_ICON_GRAPHICS, text[4],
                    editor->view_popup == VIEW_POPUP_QUALITY, false_v,
                    disabled || !frame->graphics || !frame->graphics_request,
                    layout.compact,
                    "Scalability preset and screen percentage, applied live",
                    VIEW_POPUP_QUALITY);
    (void)vkr_ui_panel_end(ui);
  }

  if (layout.show_right) {
    const VkrUiTrack right_columns[] = {
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = VIEW_CHIP_HEIGHT_PT, .unit = VKR_UI_TRACK_PX},
        {.value = 10.0f, .unit = VKR_UI_TRACK_PX},
        {.value = layout.snap, .unit = VKR_UI_TRACK_PX},
        {.value = layout.speed, .unit = VKR_UI_TRACK_PX},
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
      /* Move and rotate follow the world axes or the object's own; scale
         always uses the object's. */
      const bool8_t local =
          frame->view_state.gizmo_space == VKR_GIZMO_SPACE_LOCAL;
      VkrUiWidgetConfig space = vkr_editor_icon_button_config(
          ArrayCount(view_tools), 0,
          local ? VKR_UI_ICON_LOCAL : VKR_UI_ICON_WORLD,
          local ? string8_lit("Local axes: move and rotate along the "
                              "object's axes (click for world)")
                : string8_lit("World axes: move and rotate along the world's "
                              "axes (click for local)"));
      space.style.min_size_pt = space.style.max_size_pt =
          (Vec2){VIEW_CHIP_HEIGHT_PT, VIEW_CHIP_HEIGHT_PT};
      space.icon_color = local ? theme->accent_hover : theme->text_secondary;
      vkr_editor_toggle_style(&space, local);
      space.disabled = disabled;
      if (vkr_ui_button(ui, string8_lit("space"), (String8){0}, &space)) {
        VkrSampleViewState next = frame->view_state;
        next.gizmo_space =
            local ? VKR_GIZMO_SPACE_WORLD : VKR_GIZMO_SPACE_LOCAL;
        view_request(frame, next);
      }
      VkrUiPanelConfig divider = vkr_ui_panel_config_default();
      divider.placement.column = ArrayCount(view_tools) + 1u;
      divider.placement.row = 0;
      divider.placement.justify = VKR_UI_ALIGN_CENTER;
      divider.placement.margin_pt = (VkrUiEdges){5, 0, 5, 0};
      divider.style.min_size_pt = divider.style.max_size_pt =
          (Vec2){1.0f, 16.0f};
      divider.style.background_color = theme->border_strong;
      if (vkr_ui_panel_begin(ui, string8_lit("divider"), &divider))
        (void)vkr_ui_panel_end(ui);
      (void)view_chip(editor, ui, "snap", ArrayCount(view_tools) + 2u,
                      VKR_UI_ICON_SNAP,
                      view_snap_targets[editor->placement.target],
                      editor->view_popup == VIEW_POPUP_SNAP,
                      editor->placement.target != VKR_EDITOR_SNAP_FREE,
                      disabled, layout.compact,
                      "Where new objects land: surfaces, the grid or the "
                      "ground plane",
                      VIEW_POPUP_SNAP);
      view_speed_build(editor, frame, ArrayCount(view_tools) + 3u, disabled);
      (void)vkr_ui_panel_end(ui);
    }
  }
  view_popup_build(editor, frame, disabled);
  (void)vkr_ui_input_layer_set(ui, 0);
}

/* The renderer's grid plane (VkrEditorGridPlane) of a view: XZ, ZY from the
 * left and right, XY from the front and back. */
static uint32_t grid_view_plane(VkrSampleCameraView view) {
  if (view == VKR_SAMPLE_CAMERA_LEFT || view == VKR_SAMPLE_CAMERA_RIGHT) {
    return VKR_EDITOR_GRID_PLANE_ZY;
  }
  if (view == VKR_SAMPLE_CAMERA_FRONT || view == VKR_SAMPLE_CAMERA_BACK) {
    return VKR_EDITOR_GRID_PLANE_XY;
  }
  return VKR_EDITOR_GRID_PLANE_XZ;
}

/* Solve the chosen plane's projective mapping directly. A ray parallel to the
 * plane, including the perspective horizon, has no unique intersection. */
static bool8_t grid_plane_point(const VkrSampleUiFrame *frame, Vec2 ndc,
                                uint32_t plane, Vec2 *point) {
  /* The plane's in-plane axes, as grid_world maps them back. */
  static const Vec4 us[VKR_EDITOR_GRID_PLANE_COUNT] = {
      {1, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}};
  static const Vec4 vs[VKR_EDITOR_GRID_PLANE_COUNT] = {
      {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}};
  const Mat4 matrix = frame->view_projection;
  const Vec4 origin = mat4_mul_vec4(matrix, (Vec4){0, 0, 0, 1});
  const Vec4 u = mat4_mul_vec4(matrix, us[plane]);
  const Vec4 v = mat4_mul_vec4(matrix, vs[plane]);
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

static Vec3 grid_world(Vec2 point, uint32_t plane) {
  if (plane == VKR_EDITOR_GRID_PLANE_ZY) {
    return (Vec3){0, point.y, point.x};
  }
  if (plane == VKR_EDITOR_GRID_PLANE_XY) {
    return (Vec3){point.x, point.y, 0};
  }
  return (Vec3){point.x, 0, point.y};
}

bool8_t vkr_editor_viewport_pixel(const VkrSampleUiFrame *frame, Vec3 world,
                                  Vec2 *out) {
  const Vec4 clip =
      mat4_mul_vec4(frame->view_projection, vec3_to_vec4(world, 1.0f));
  if (!isfinite(clip.w) || clip.w <= 1.0e-6f) {
    return false_v;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  *out = (Vec2){image.x + (clip.x / clip.w * 0.5f + 0.5f) * image.z,
                image.y + (clip.y / clip.w * 0.5f + 0.5f) * image.w};
  return isfinite(out->x) && isfinite(out->y);
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

/* The ray under a viewport pixel: perspective rays leave the eye;
   orthographic rays run through two depths of the pixel. */
bool8_t vkr_editor_viewport_ray(const VkrSampleUiFrame *frame, Vec2 pixel,
                                Vec3 *origin, Vec3 *direction) {
  return viewport_ray(frame, pixel, origin, direction);
}

static bool8_t viewport_ray(const VkrSampleUiFrame *frame, Vec2 pixel,
                            Vec3 *origin, Vec3 *direction) {
  Vec3 target = {0};
  if (!frame->mapping_valid ||
      !viewport_unproject(frame, pixel, 0.5f, &target)) {
    return false_v;
  }
  if (!grid_eye(frame, origin) &&
      !viewport_unproject(frame, pixel, 0.25f, origin)) {
    return false_v;
  }
  const Vec3 ray = vec3_sub(target, *origin);
  const float32_t length = vec3_length(ray);
  if (!isfinite(length) || length < 1e-6f) {
    return false_v;
  }
  *direction = vec3_scale(ray, 1.0f / length);
  return true_v;
}

/* Shortest rotation turning unit `from` onto unit `to`. */
static VkrQuat place_tilt(Vec3 from, Vec3 to) {
  const float32_t cosine = vkr_clamp_f32(vec3_dot(from, to), -1.0f, 1.0f);
  if (cosine >= 0.9999f) {
    return vkr_quat_identity();
  }
  Vec3 axis = vec3_cross(from, to);
  if (cosine <= -0.9999f) {
    /* Opposite vectors turn about any axis perpendicular to them. */
    axis = vec3_cross(from, fabsf(from.x) < 0.9f ? vec3_new(1.0f, 0.0f, 0.0f)
                                                 : vec3_new(0.0f, 0.0f, 1.0f));
  }
  return vkr_quat_from_axis_angle(vec3_normalize(axis), acosf(cosine));
}

/* Rotation turning +Y onto `normal`, then `yaw` radians about it. */
static VkrQuat place_orientation(Vec3 normal, float32_t yaw) {
  const Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  return vkr_quat_normalize(
      vkr_quat_mul(place_tilt(up, normal), vkr_quat_from_axis_angle(up, yaw)));
}

/* True when `entity` is `root` or one of its descendants. */
static bool8_t place_in_subtree(const VkrScene *scene, VkrEntityId entity,
                                VkrEntityId root) {
  while (entity.u64) {
    if (entity.u64 == root.u64) {
      return true_v;
    }
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, entity, scene->comp_transform);
    entity = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  }
  return false_v;
}

/* How far along unit `direction` from `origin` a ray leaves collider
   `entity`: through its brush planes, else its world box; -1 when it cannot
   tell. */
static float32_t place_exit(const VkrScene *scene, VkrEntityId entity,
                            Vec3 origin, Vec3 direction) {
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  const uint32_t count =
      vkr_editor_brush_world_planes(scene, entity, planes, ArrayCount(planes));
  float32_t exit = INFINITY;
  for (uint32_t i = 0; i < count; ++i) {
    const float32_t toward = vec3_dot(planes[i].normal, direction);
    if (toward > 1.0e-6f) {
      exit =
          Min(exit, (planes[i].distance - vec3_dot(planes[i].normal, origin)) /
                        toward);
    }
  }
  if (count) {
    return isfinite(exit) ? exit : -1.0f;
  }
  VkrBrushGeometry *scratch = malloc(sizeof(*scratch));
  Vec3 lo = {0};
  Vec3 hi = {0};
  const bool8_t boxed =
      scratch && vkr_editor_entity_world_box(scene, entity, scratch, &lo, &hi);
  free(scratch);
  if (!boxed) {
    return -1.0f;
  }
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    const float32_t d = direction.elements[axis];
    if (fabsf(d) > 1.0e-6f) {
      const float32_t side = d > 0.0f ? hi.elements[axis] : lo.elements[axis];
      exit = Min(exit, (side - origin.elements[axis]) / d);
    }
  }
  return isfinite(exit) ? exit : -1.0f;
}

/* The first solid collision surface along a segment, past `skip` and its
   descendants and past what the editor hides (Hide and Isolate), which is
   no surface to place on. A collider of either is passed through where the
   ray leaves it, since a brush shares its cell's body with its neighbours;
   another body `skip` owns is ignored whole. Physics queries take a mutable
   scene but change none of its state. */
static bool8_t place_raycast(const VkrScene *scene, Vec3 origin,
                             Vec3 displacement, VkrEntityId skip,
                             VkrPhysicsRayHit *hit) {
  uint64_t ignored[VKR_PHYSICS_MAX_QUERY_IGNORES];
  VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX,
                                  .ignored_entities = ignored};
  const float32_t length = vec3_length(displacement);
  const Vec3 direction =
      length > 0.0f ? vec3_scale(displacement, 1.0f / length) : vec3_zero();
  float32_t travelled = 0.0f;
  uint32_t passes = 0u;
  while (vkr_scene_physics_raycast_query((VkrScene *)scene, origin,
                                         displacement, &filter, hit)) {
    const VkrEntityId collider = {.u64 = hit->collider_entity_id
                                             ? hit->collider_entity_id
                                             : hit->entity_id};
    if (!vkr_scene_editor_shown(scene, collider) ||
        (skip.u64 && place_in_subtree(scene, collider, skip))) {
      /* Start again where the ray leaves it; the fraction counts the whole
         segment. */
      const Vec3 at =
          vec3_new(hit->position[0], hit->position[1], hit->position[2]);
      const float32_t exit = place_exit(scene, collider, at, direction);
      const float32_t step =
          vec3_length(vec3_sub(at, origin)) + Max(exit, 0.0f) + 0.01f;
      if (exit < 0.0f || ++passes > 16u || travelled + step >= length) {
        return false_v;
      }
      travelled += step;
      origin = vec3_add(origin, vec3_scale(direction, step));
      displacement = vec3_scale(direction, length - travelled);
      continue;
    }
    if (travelled > 0.0f) {
      hit->fraction =
          (travelled + hit->fraction * (length - travelled)) / length;
    }
    const VkrEntityId owner = {.u64 = hit->entity_id};
    if (!skip.u64 || !place_in_subtree(scene, owner, skip)) {
      return true_v;
    }
    if (filter.ignored_count == ArrayCount(ignored)) {
      return false_v;
    }
    ignored[filter.ignored_count++] = hit->entity_id;
  }
  return false_v;
}

/* The nearest solid surface along a segment in the open scene, the World or
   an added scene; each container has its own physics. */
static bool8_t place_raycast_frame(const VkrSampleUiFrame *frame, Vec3 origin,
                                   Vec3 displacement, VkrPhysicsRayHit *hit) {
  const VkrScene *scenes[VKR_SCENE_ADDITIVE_MAX + 2u];
  uint32_t count = 0u;
  if (frame->scene) {
    scenes[count++] = frame->scene;
  }
  if (frame->world) {
    scenes[count++] = frame->world;
  }
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i]) {
      scenes[count++] = frame->additive[i];
    }
  }
  bool8_t found = false_v;
  for (uint32_t i = 0; i < count; ++i) {
    VkrPhysicsRayHit candidate = {0};
    if (place_raycast(scenes[i], origin, displacement, VKR_ENTITY_ID_INVALID,
                      &candidate) &&
        (!found || candidate.fraction < hit->fraction)) {
      *hit = candidate;
      found = true_v;
    }
  }
  return found;
}

/* `position` moved to the nearest grid crossing, or cell centre, in XZ. */
static Vec3 place_grid_point(const VkrEditorPlacement *placement,
                             const VkrSampleUiFrame *frame, Vec3 position) {
  const float32_t cell = frame->view_state.grid_spacing > 0.0f
                             ? frame->view_state.grid_spacing
                             : 1.0f;
  const float32_t shift = placement->cell_centers ? 0.5f : 0.0f;
  position.x = (floorf(position.x / cell - shift + 0.5f) + shift) * cell;
  position.z = (floorf(position.z / cell - shift + 0.5f) + shift) * cell;
  return position;
}

bool8_t vkr_editor_viewport_place(const VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, Vec2 pixel,
                                  float32_t base, VkrEditorDropPose *out) {
  const VkrEditorPlacement *placement = &editor->placement;
  Vec3 origin = {0};
  Vec3 direction = {0};
  if (!viewport_ray(frame, pixel, &origin, &direction)) {
    return false_v;
  }
  out->eye = origin;
  const float32_t yaw = placement->yaw_degrees * (VKR_PI / 180.0f);
  const Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 normal = up;
  VkrPhysicsRayHit hit = {0};
  if (placement->target == VKR_EDITOR_SNAP_SURFACE &&
      place_raycast_frame(frame, origin, vec3_scale(direction, 1000.0f),
                          &hit)) {
    Vec3 surface_normal = vec3_new(hit.normal[0], hit.normal[1], hit.normal[2]);
    surface_normal = vec3_length(surface_normal) > 0.5f
                         ? vec3_normalize(surface_normal)
                         : up;
    if (placement->align_to_normal) {
      normal = surface_normal;
    }
    /* The offset follows the surface; the base follows the object's up. */
    out->position = vec3_add(
        vec3_add(vec3_new(hit.position[0], hit.position[1], hit.position[2]),
                 vec3_scale(surface_normal, placement->offset)),
        vec3_scale(normal, base));
    out->rotation = place_orientation(normal, yaw);
    return true_v;
  }
  /* The ground plane at the grid's height, which Fit to surface lifts onto a
     floor modelled above the origin when the scene has no collision. */
  const float32_t ground_y = frame->view_state.grid_height;
  const float32_t t =
      fabsf(direction.y) > 1e-4f ? (ground_y - origin.y) / direction.y : -1.0f;
  const bool8_t ground = t > 0.0f && t < 500.0f;
  Vec3 position = vec3_add(origin, vec3_scale(direction, ground ? t : 8.0f));
  if (ground) {
    position.y = ground_y;
    if (placement->target == VKR_EDITOR_SNAP_GRID) {
      position = place_grid_point(placement, frame, position);
    }
    position.y += placement->offset + base;
  }
  out->position = position;
  out->rotation = place_orientation(normal, yaw);
  return true_v;
}

bool8_t vkr_editor_viewport_snap(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, char *message,
                                 uint64_t message_size) {
  const VkrEditorPlacement *placement = &editor->placement;
  VkrScene *scene = (VkrScene *)vkr_editor_entity_scene(frame, entity);
  VkrSceneEditValues values;
  if (!scene || !vkr_scene_entity_alive(scene, entity) ||
      !vkr_scene_edit_read(scene, entity, &values)) {
    snprintf(message, message_size, "Nothing to snap");
    return false_v;
  }
  const String8 name = vkr_scene_get_name(scene, entity);
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  /* A collider moves with its body; the gizmo edits it through physics. */
  if (!transform || !(values.fields & VKR_SCENE_EDIT_TRANSFORM) ||
      vkr_entity_get_component(scene->world, entity,
                               scene->comp_physics_collider)) {
    snprintf(message, message_size, "'%.*s' has no transform to snap",
             (int)name.length, name.str);
    return false_v;
  }
  if (frame->scene_edit->action != VKR_SCENE_EDIT_NONE) {
    snprintf(message, message_size, "Another scene edit is pending");
    return false_v;
  }

  /* The object's box relative to its origin in world space; one without
     loaded geometry snaps its origin. */
  Vec3 lower = vec3_zero();
  Vec3 upper = vec3_zero();
  (void)vkr_scene_entity_local_bounds(scene, entity, &lower, &upper);
  const Vec3 pivot = mat4_position(transform->world);
  Vec3 corners[8];
  Vec3 centre = vec3_zero();
  float32_t top = -INFINITY;
  for (uint32_t i = 0; i < ArrayCount(corners); ++i) {
    const Vec3 corner =
        vec3_new(i & 1 ? upper.x : lower.x, i & 2 ? upper.y : lower.y,
                 i & 4 ? upper.z : lower.z);
    corners[i] = vec3_sub(mat4_mul_vec3(transform->world, corner), pivot);
    centre = vec3_add(centre, vec3_scale(corners[i], 1.0f / 8.0f));
    top = Max(top, pivot.y + corners[i].y);
  }
  const Vec3 above = vec3_add(pivot, centre);

  /* The point the box comes to rest on, straight below its centre. */
  const Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 normal = up;
  Vec3 target = vec3_new(above.x, frame->view_state.grid_height, above.z);
  const char *onto = "the ground plane";
  bool8_t align = false_v;
  VkrPhysicsRayHit hit = {0};
  if (placement->target == VKR_EDITOR_SNAP_SURFACE &&
      place_raycast(scene, vec3_new(above.x, top + 0.01f, above.z),
                    vec3_new(0.0f, -1000.0f, 0.0f), entity, &hit)) {
    target = vec3_new(hit.position[0], hit.position[1], hit.position[2]);
    const Vec3 surface_normal =
        vec3_new(hit.normal[0], hit.normal[1], hit.normal[2]);
    align = placement->align_to_normal && vec3_length(surface_normal) > 0.5f;
    if (align) {
      normal = vec3_normalize(surface_normal);
    }
    onto = "the surface";
  } else if (placement->target == VKR_EDITOR_SNAP_GRID) {
    target = place_grid_point(placement, frame, target);
    onto = "the grid";
  }

  /* Aligning tilts the object's up onto the normal and keeps its heading. */
  const Vec3 object_up = vec3_normalize(vec3_new(transform->world.elements[4],
                                                 transform->world.elements[5],
                                                 transform->world.elements[6]));
  const VkrQuat tilt =
      align ? place_tilt(object_up, normal) : vkr_quat_identity();
  const Vec3 tilted_centre = vkr_quat_rotate_vec3(tilt, centre);
  float32_t bottom = INFINITY;
  for (uint32_t i = 0; i < ArrayCount(corners); ++i) {
    bottom =
        Min(bottom, vec3_dot(vkr_quat_rotate_vec3(tilt, corners[i]), normal));
  }
  /* The box centre sits over the target and its lowest point `offset` above
     it along the normal. */
  const Vec3 position =
      vec3_add(vec3_sub(target, tilted_centre),
               vec3_scale(normal, vec3_dot(tilted_centre, normal) - bottom +
                                      placement->offset));

  /* Back to the parent's frame. */
  Vec3 local = position;
  VkrQuat parent_rotation = vkr_quat_identity();
  for (VkrEntityId ancestor = transform->parent; ancestor.u64;) {
    const SceneTransform *parent = vkr_scene_get_transform(scene, ancestor);
    if (!parent) {
      break;
    }
    if (ancestor.u64 == transform->parent.u64) {
      local = mat4_mul_vec3(mat4_inverse_affine(parent->world), position);
    }
    parent_rotation = vkr_quat_mul(parent->rotation, parent_rotation);
    ancestor = parent->parent;
  }
  values.fields = VKR_SCENE_EDIT_TRANSFORM;
  values.position = local;
  values.rotation = vkr_quat_normalize(
      vkr_quat_mul(vkr_quat_mul(vkr_quat_conjugate(parent_rotation),
                                vkr_quat_mul(tilt, parent_rotation)),
                   values.rotation));
  if (!vkr_scene_edit_validate(&values)) {
    snprintf(message, message_size, "The scene rejected the snapped pose");
    return false_v;
  }
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
  snprintf(message, message_size, "Snapped %.*s to %s", (int)name.length,
           name.str, onto);
  return true_v;
}

/* Returns false when an image corner misses the grid plane or the visible
 * extent is empty. */
static bool8_t grid_fit_orthographic(const VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t plane, float32_t *spacing,
                                     Vec2 *minimum, Vec2 *maximum) {
  for (uint32_t i = 0; i < 4; ++i) {
    Vec2 point;
    if (!grid_plane_point(frame, (Vec2){i & 1 ? 1 : -1, i & 2 ? 1 : -1}, plane,
                          &point)) {
      return false_v;
    }
    minimum->x = Min(minimum->x, point.x);
    minimum->y = Min(minimum->y, point.y);
    maximum->x = Max(maximum->x, point.x);
    maximum->y = Max(maximum->y, point.y);
  }
  /* Labels mark the lines the renderer's grid draws at full strength: the
     level after the finest, from the same per-pixel footprint
     (editor_grid_kernel.slangh). */
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 extent = {maximum->x - minimum->x, maximum->y - minimum->y};
  if (!(extent.x > 0 && extent.y > 0) || image.z <= 0 || image.w <= 0) {
    return false_v;
  }
  const float32_t pixel_world = Max(extent.x / image.z, extent.y / image.w);
  const float32_t level =
      Max(0.0f, log10f(pixel_world * VIEW_GRID_MIN_SPACING_PX / *spacing));
  *spacing *= powf(10.0f, floorf(level) + 1.0f);
  (void)editor;
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
}

/* Cell labels only; the renderer draws the lines (Editor.Grid). */
static void grid_build_label_widgets(VkrEditorUi *editor, VkrUiSystem *ui) {
  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    VkrEditorGridLine *line = &editor->grid_lines[i];
    line->label = VKR_UI_ID_NONE;
    if (!line->ordinal) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    char text[24];
    grid_label_text(text, line->ordinal, line->top_label);
    VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
    widget.placement.column = 0;
    widget.placement.row = 0;
    widget.placement.justify = VKR_UI_ALIGN_START;
    widget.placement.align = VKR_UI_ALIGN_START;
    widget.placement.margin_pt = (VkrUiEdges){100000, 0, 0, 100000};
    widget.style.min_size_pt = widget.style.max_size_pt = line->label_size_pt;
    widget.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
    widget.style.font_size_pt = 10;
    widget.style.text_color = vkr_ui_theme()->text;
    widget.style.background_color = vkr_ui_theme()->overlay;
    widget.style.corner_radius_pt = (Vec4){2, 2, 2, 2};
    widget.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("cell"), view_string(text), &widget);
    line->label =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("cell"));
    (void)vkr_ui_pop_id(ui);
  }
}

/* Orthographic lines fill the visible plane at the fitted spacing. */
static void grid_orthographic_lines(VkrEditorUi *editor, uint32_t plane,
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
          .from = grid_world(from, plane),
          .to = grid_world(to, plane),
          .label_offset = grid_world(axis ? (Vec2){0, spacing * 0.5f}
                                          : (Vec2){spacing * 0.5f, 0},
                                     plane),
          .top_label = axis == 0 ? first_axis_top : !first_axis_top,
      };
    }
  }
}

/* The renderer draws the grid (Editor.Grid). Orthographic views add cell
   labels that read 1..N across the top and A.. down the right, on the lines
   the renderer draws at full strength. */
void vkr_editor_grid_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  editor->grid_line_count = 0;
  editor->grid_spacing = 0;
  editor->grid_frame = frame->ui->frame_index;
  editor->grid_camera_view = frame->view_state.camera_view;
  editor->grid_reserved_pt = (Vec2){0};
  /* A project's World renders alone before any scene opens. */
  if (!frame->mapping_valid || !frame->view_state.grid_enabled ||
      !frame->view_state.grid_labels ||
      frame->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE ||
      (!frame->scene && !frame->world) || frame->scene_rendering_stopped ||
      (frame->scene_backdrop_blur && *frame->scene_backdrop_blur)) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const uint32_t plane = grid_view_plane(frame->view_state.camera_view);
  float32_t spacing = frame->view_state.grid_spacing;
  if (!isfinite(spacing) || spacing <= 0) {
    return;
  }
  Vec2 minimum = {INFINITY, INFINITY};
  Vec2 maximum = {-INFINITY, -INFINITY};
  Vec2 center_plane;
  if (!grid_plane_point(frame, (Vec2){0, 0}, plane, &center_plane) ||
      !grid_fit_orthographic(editor, frame, plane, &spacing, &minimum,
                             &maximum) ||
      !isfinite(spacing) || spacing <= 0 ||
      fabsf(minimum.x / spacing) > 1000000000 ||
      fabsf(minimum.y / spacing) > 1000000000 ||
      fabsf(maximum.x / spacing) > 1000000000 ||
      fabsf(maximum.y / spacing) > 1000000000) {
    return;
  }
  const Vec3 center = grid_world(center_plane, plane);
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
          ? ui->frame_node_capacity - ui->frame_node_count - 320
          : 0;
  const uint32_t capacity = Min(VKR_EDITOR_GRID_LINE_CAPACITY, available);
  const bool8_t first_axis_top =
      grid_first_axis_top(frame, false_v, center, spacing);
  editor->grid_spacing = spacing;
  grid_orthographic_lines(editor, plane, spacing, minimum, maximum,
                          first_axis_top, capacity);
  /* Labels are sized for the largest possible ordinal. */
  uint32_t axis_lines[2] = {0, 0};
  for (uint32_t i = 0; i < editor->grid_line_count; ++i) {
    ++axis_lines[editor->grid_lines[i].top_label == first_axis_top ? 0 : 1];
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
  grid_build_label_widgets(editor, ui);
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
  float32_t lengths[3] = {0};
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
    lengths[i] = length;
    out_axes[i].direction = length > 1e-6f
                                ? (Vec2){delta.x / length, delta.y / length}
                                : (Vec2){0.0f, 0.0f};
    out_axes[i].depth = clip.w - base.w;
  }
  /* Keep foreshortening: axes pointing at the camera draw shorter. An
     orthographic view's w stays 1, so its full length comes from the three
     projected unit axes, whose squares sum to twice it squared. */
  const bool8_t orthographic =
      frame->view_state.camera_view != VKR_SAMPLE_CAMERA_PERSPECTIVE;
  const float32_t full =
      orthographic
          ? sqrtf(0.5f * (lengths[0] * lengths[0] + lengths[1] * lengths[1] +
                          lengths[2] * lengths[2]))
          : step / Max(fabsf(base.w), 1e-6f);
  for (uint32_t i = 0; i < 3; ++i) {
    const float32_t scale = Min(1.0f, lengths[i] / Max(full, 1e-6f));
    out_axes[i].direction.x *= scale;
    out_axes[i].direction.y *= scale;
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
  /* Each cap looks from its end of the axis toward the origin. */
  static const VkrSampleCameraView positive_views[3] = {
      VKR_SAMPLE_CAMERA_RIGHT, VKR_SAMPLE_CAMERA_TOP, VKR_SAMPLE_CAMERA_FRONT};
  static const VkrSampleCameraView negative_views[3] = {
      VKR_SAMPLE_CAMERA_LEFT, VKR_SAMPLE_CAMERA_BOTTOM, VKR_SAMPLE_CAMERA_BACK};
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
      /* The cap of the current view goes back to perspective. */
      VkrSampleViewState next = frame->view_state;
      next.camera_view = positive ? positive_views[axis] : negative_views[axis];
      if (next.camera_view == frame->view_state.camera_view) {
        next.camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE;
      }
      view_request(frame, next);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0);
}

void vkr_editor_scene_label(const VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame, char *out,
                            uint64_t size) {
  if (frame->world && editor->projects) {
    const String8 name = vkr_editor_projects_scene_name(editor->projects);
    snprintf(out, size, "%.*s", (int)(name.length ? name.length : 5u),
             name.length ? (const char *)name.str : "World");
    return;
  }
  /* The file name without its directory and .scene.json. */
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
  snprintf(out, size, "%.*s", (int)(length ? length : 5u),
           length ? (const char *)path.str + start : "Scene");
}
