#include "editor_terrain.h"

#include "editor_details.h"
#include "editor_internal.h"

#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TERRAIN_PAD_PT 10.0f
#define TERRAIN_ROW_PT 28.0f
/* Terrains one ray tests per scene. */
#define TERRAIN_RAY_MAX VKR_SCENE_TERRAIN_MAX

static const char *const s_terrain_mode_labels[VKR_EDITOR_TERRAIN_MODE_COUNT] =
    {"Raise", "Lower", "Smooth", "Flatten", "Paint"};

// =============================================================================
// Rays
// =============================================================================

/* Marches a local ray over one field: steps of half a sample, then halving
   to the crossing. */
static bool8_t terrain_march(const VkrHeightfield *field, Vec3 origin,
                             Vec3 direction, float32_t max_distance,
                             float32_t *out_t) {
  const float32_t half = vkr_heightfield_half_size(field);
  /* Clip to the terrain's square. */
  float32_t t0 = 0.0f;
  float32_t t1 = max_distance;
  const float32_t o[2] = {origin.x, origin.z};
  const float32_t d[2] = {direction.x, direction.z};
  for (uint32_t axis = 0; axis < 2u; ++axis) {
    if (fabsf(d[axis]) < 1.0e-8f) {
      if (o[axis] < -half || o[axis] > half) {
        return false_v;
      }
      continue;
    }
    float32_t a = (-half - o[axis]) / d[axis];
    float32_t b = (half - o[axis]) / d[axis];
    if (a > b) {
      const float32_t swap = a;
      a = b;
      b = swap;
    }
    t0 = Max(t0, a);
    t1 = Min(t1, b);
  }
  if (t0 > t1) {
    return false_v;
  }
  const float32_t step = 0.5f * field->spacing;
  float32_t previous = t0;
  float32_t ground = 0.0f;
  Vec3 at = vec3_add(origin, vec3_scale(direction, t0));
  if (vkr_heightfield_sample(field, at.x, at.z, &ground) && at.y <= ground) {
    *out_t = t0;
    return true_v;
  }
  for (float32_t t = t0 + step; t <= t1 + step; t += step) {
    const float32_t clamped = Min(t, t1);
    at = vec3_add(origin, vec3_scale(direction, clamped));
    if (vkr_heightfield_sample(field, at.x, at.z, &ground) && at.y <= ground) {
      float32_t lo = previous;
      float32_t hi = clamped;
      for (uint32_t i = 0; i < 16u; ++i) {
        const float32_t mid = 0.5f * (lo + hi);
        const Vec3 p = vec3_add(origin, vec3_scale(direction, mid));
        if (vkr_heightfield_sample(field, p.x, p.z, &ground) && p.y <= ground) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      *out_t = hi;
      return true_v;
    }
    previous = clamped;
  }
  return false_v;
}

bool8_t vkr_editor_terrain_ray(const VkrSampleUiFrame *frame, Vec3 origin,
                               Vec3 direction, VkrEntityId *out_entity,
                               Vec3 *out_point) {
  const VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {frame->scene,
                                                         frame->world};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = frame->additive[i];
  }
  float32_t nearest = 4096.0f;
  bool8_t found = false_v;
  for (uint32_t s = 0; s < ArrayCount(scenes); ++s) {
    const VkrScene *scene = scenes[s];
    if (!scene || (s == 1u && scene == frame->scene)) {
      continue;
    }
    VkrEntityId terrains[TERRAIN_RAY_MAX];
    const uint32_t count = Min(
        TERRAIN_RAY_MAX, vkr_scene_find_typed(scene, &vkr_scene_terrain_type,
                                              terrains, TERRAIN_RAY_MAX));
    for (uint32_t i = 0; i < count; ++i) {
      const VkrHeightfield *field = vkr_scene_terrain_field(scene, terrains[i]);
      Vec3 local = {0};
      float32_t t = 0.0f;
      if (field &&
          vkr_scene_terrain_to_local(scene, terrains[i], origin, &local) &&
          terrain_march(field, local, direction, nearest, &t) && t < nearest) {
        nearest = t;
        *out_entity = terrains[i];
        *out_point = vec3_add(origin, vec3_scale(direction, t));
        found = true_v;
      }
    }
  }
  return found;
}

// =============================================================================
// Scene tool
// =============================================================================

void vkr_editor_terrain_update(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  editor->terrain_hit_valid = false_v;
  if (!(editor->terrain_radius > 0.0f)) {
    editor->terrain_radius = 8.0f;
    editor->terrain_strength = 1.0f;
  }
  const float64_t now = vkr_platform_get_absolute_time();
  const float32_t dt =
      (float32_t)Min(0.1, Max(0.0, now - editor->terrain_last_time));
  editor->terrain_last_time = now;
  if (!editor->terrain_tool || frame->scene_rendering_stopped ||
      frame->mouse_captured || frame->simulation_running ||
      editor->menu != VKR_EDITOR_MENU_NONE) {
    editor->terrain_stroke = 0u;
    return;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  const bool8_t inside = mouse.x >= image.x && mouse.y >= image.y &&
                         mouse.x < image.x + image.z &&
                         mouse.y < image.y + image.w;
  if (input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->terrain_tool = false_v;
    editor->terrain_stroke = 0u;
    return;
  }
  /* The Scene image stops picking objects while the tool is on. */
  (void)vkr_ui_input_layer_register(
      ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
      (VkrUiRect){image.x, image.y, image.z, image.w});
  Vec3 origin = {0};
  Vec3 direction = {0};
  VkrEntityId terrain = VKR_ENTITY_ID_INVALID;
  Vec3 point = {0};
  if (!inside || !vkr_editor_viewport_ray(frame, mouse, &origin, &direction) ||
      !vkr_editor_terrain_ray(frame, origin, direction, &terrain, &point)) {
    if (!input_is_button_down(frame->input, BUTTON_LEFT)) {
      editor->terrain_stroke = 0u;
    }
    return;
  }
  editor->terrain_hit_valid = true_v;
  editor->terrain_hit = point;
  editor->terrain_hit_entity = terrain;
  if (ui->mouse_pressed) {
    /* A stroke is one undo step; flatten holds the height it started at. */
    editor->terrain_stroke = ++editor->terrain_stroke_counter;
    editor->terrain_flatten_height = point.y;
  }
  if (!editor->terrain_stroke ||
      !input_is_button_down(frame->input, BUTTON_LEFT)) {
    editor->terrain_stroke = 0u;
    return;
  }
  /* Raise and lower move `strength` metres a second; the rest blend by
     their fraction a second. */
  static const VkrHeightfieldBrush brushes[VKR_EDITOR_TERRAIN_MODE_COUNT] = {
      VKR_HEIGHTFIELD_RAISE, VKR_HEIGHTFIELD_LOWER, VKR_HEIGHTFIELD_SMOOTH,
      VKR_HEIGHTFIELD_FLATTEN, VKR_HEIGHTFIELD_PAINT};
  const VkrHeightfieldOp op = {
      .kind = VKR_HEIGHTFIELD_OP_BRUSH,
      .brush = brushes[editor->terrain_mode],
      .layer = editor->terrain_layer,
      .a = point,
      .radius = editor->terrain_radius,
      .strength =
          editor->terrain_strength * Max(dt, 1.0f / 120.0f) *
          (editor->terrain_mode <= VKR_EDITOR_TERRAIN_LOWER ? 4.0f : 2.0f),
      .height = editor->terrain_flatten_height,
  };
  *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_TERRAIN,
                                             .entity = terrain,
                                             .gesture = editor->terrain_stroke,
                                             .terrain = op};
}

uint32_t vkr_editor_terrain_brush_outline(const VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          Vec3 *out, uint32_t capacity) {
  if (!editor->terrain_tool || !editor->terrain_hit_valid || capacity < 3u) {
    return 0u;
  }
  const VkrScene *scene =
      vkr_editor_entity_scene(frame, editor->terrain_hit_entity);
  const VkrHeightfield *field =
      scene ? vkr_scene_terrain_field(scene, editor->terrain_hit_entity) : NULL;
  Vec3 offset = {0};
  if (!field || !vkr_scene_terrain_to_local(scene, editor->terrain_hit_entity,
                                            vec3_zero(), &offset)) {
    return 0u;
  }
  /* The circle follows the ground a little above it. */
  for (uint32_t i = 0; i < capacity; ++i) {
    const float32_t angle = 6.28318531f * (float32_t)i / (float32_t)capacity;
    Vec3 p = vec3_add(editor->terrain_hit,
                      vec3_new(cosf(angle) * editor->terrain_radius, 0.0f,
                               sinf(angle) * editor->terrain_radius));
    float32_t ground = 0.0f;
    if (vkr_heightfield_sample(field, p.x + offset.x, p.z + offset.z,
                               &ground)) {
      p.y = ground - offset.y + 0.05f;
    }
    out[i] = p;
  }
  return capacity;
}

// =============================================================================
// Window
// =============================================================================

static void terrain_label(VkrUiSystem *ui, String8 id, float32_t y,
                          float32_t width, String8 text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      TERRAIN_PAD_PT, y, width - TERRAIN_PAD_PT * 2.0f, TERRAIN_ROW_PT - 4.0f);
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, id, text, &label);
}

/* A labelled slider row; returns the value. */
static float32_t terrain_slider(VkrUiSystem *ui, String8 id, String8 label,
                                float32_t y, float32_t width, float32_t value,
                                float32_t minimum, float32_t maximum,
                                const char *format) {
  const float32_t label_w = 96.0f;
  terrain_label(ui, label, y, label_w + TERRAIN_PAD_PT * 2.0f, label);
  (void)id;
  VkrUiWidgetConfig slider = vkr_editor_details_widget(
      TERRAIN_PAD_PT + label_w, y + 2.0f,
      Max(40.0f, width - label_w - TERRAIN_PAD_PT * 2.0f - 56.0f),
      TERRAIN_ROW_PT - 6.0f);
  (void)vkr_ui_slider_f32(ui, id, &value, minimum, maximum, &slider);
  char text[32];
  snprintf(text, sizeof(text), format, value);
  VkrUiWidgetConfig shown = vkr_editor_details_widget(
      width - TERRAIN_PAD_PT - 52.0f, y, 52.0f, TERRAIN_ROW_PT - 4.0f);
  shown.style.font_size_pt = vkr_ui_theme()->font_body;
  vkr_ui_label(ui,
               string8_create_formatted(ui->frame_allocator, "%.*s.value",
                                        (int)id.length, id.str),
               string8_create_from_cstr((const uint8_t *)text, strlen(text)),
               &shown);
  return value;
}

void vkr_editor_terrain_window_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / ui->content_scale;
  float32_t y = 8.0f;
  /* The tool toggle. */
  VkrUiWidgetConfig tool = vkr_editor_details_widget(
      TERRAIN_PAD_PT, y, width - TERRAIN_PAD_PT * 2.0f, TERRAIN_ROW_PT);
  vkr_editor_toggle_style(&tool, editor->terrain_tool);
  tool.icon = VKR_UI_ICON_PENCIL_LINE;
  tool.icon_size_pt = 13.0f;
  tool.tooltip = string8_lit("Sculpt and paint in the Scene with the left "
                             "button; Escape stops");
  if (vkr_ui_button(ui, string8_lit("terrain.tool"),
                    editor->terrain_tool ? string8_lit("Sculpting in the Scene")
                                         : string8_lit("Sculpt in the Scene"),
                    &tool)) {
    editor->terrain_tool = !editor->terrain_tool;
    editor->brush_draw = false_v;
    editor->clip_tool = false_v;
  }
  y += TERRAIN_ROW_PT + 8.0f;
  /* Modes, one button each. */
  const float32_t mode_w = (width - TERRAIN_PAD_PT * 2.0f) /
                           (float32_t)VKR_EDITOR_TERRAIN_MODE_COUNT;
  for (uint32_t i = 0; i < VKR_EDITOR_TERRAIN_MODE_COUNT; ++i) {
    VkrUiWidgetConfig mode =
        vkr_editor_details_widget(TERRAIN_PAD_PT + mode_w * (float32_t)i, y,
                                  mode_w - 4.0f, TERRAIN_ROW_PT);
    vkr_editor_toggle_style(&mode, editor->terrain_mode == i);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(
            ui, string8_lit("terrain.mode"),
            string8_create_from_cstr((const uint8_t *)s_terrain_mode_labels[i],
                                     strlen(s_terrain_mode_labels[i])),
            &mode)) {
      editor->terrain_mode = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  y += TERRAIN_ROW_PT + 8.0f;
  editor->terrain_radius =
      terrain_slider(ui, string8_lit("terrain.radius"), string8_lit("Radius"),
                     y, width, editor->terrain_radius, 0.5f, 64.0f, "%.1f m");
  y += TERRAIN_ROW_PT;
  editor->terrain_strength = terrain_slider(
      ui, string8_lit("terrain.strength"), string8_lit("Strength"), y, width,
      editor->terrain_strength, 0.05f, 4.0f, "%.2f");
  y += TERRAIN_ROW_PT;
  if (editor->terrain_mode == VKR_EDITOR_TERRAIN_PAINT) {
    const float32_t layer = terrain_slider(
        ui, string8_lit("terrain.layer"), string8_lit("Layer"), y, width,
        (float32_t)editor->terrain_layer + 1.0f, 1.0f, 4.0f, "%.0f");
    editor->terrain_layer =
        (uint32_t)Min(3.0f, Max(0.0f, roundf(layer) - 1.0f));
    y += TERRAIN_ROW_PT;
  }
  (void)theme;
  terrain_label(ui, string8_lit("terrain.hint"), y + 4.0f, width,
                editor->terrain_hit_valid
                    ? string8_lit("Hold the left button over the terrain")
                    : string8_lit("Point at a terrain; Terrain in the Create "
                                  "menu adds one"));
}
