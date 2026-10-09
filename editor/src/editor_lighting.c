#include "editor_lighting.h"

#include "editor_details.h"
#include "editor_internal.h"
#include "editor_projects.h"
#include "editor_scene_panels.h"

#include "core/vkr_json_writer.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_types.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Bake settings
// =============================================================================

#define TYPE_OFFSET(type, field) (uint32_t)offsetof(type, field)

#define LIGHTING_COUNT(type, field, name_, label_, tooltip_, max_)             \
  {.name = name_,                                                              \
   .label = label_,                                                            \
   .tooltip = tooltip_,                                                        \
   .offset = TYPE_OFFSET(type, field),                                         \
   .kind = VKR_PROPERTY_U32,                                                   \
   .min = 0.0f,                                                                \
   .max = max_,                                                                \
   .step = 1.0f}

#define LIGHTING_SIZE(type, field, name_, label_, tooltip_, unit_, step_)      \
  {.name = name_,                                                              \
   .label = label_,                                                            \
   .tooltip = tooltip_,                                                        \
   .unit = unit_,                                                              \
   .offset = TYPE_OFFSET(type, field),                                         \
   .kind = VKR_PROPERTY_F32,                                                   \
   .min = 0.0f,                                                                \
   .max = FLT_MAX,                                                             \
   .step = step_}

static const char *const s_denoise_names[] = {"default", "on", "off", NULL};

static const VkrPropertyDesc s_lightmap_properties[] = {
    LIGHTING_COUNT(VkrEditorLightmapSettings, samples, "samples", "Samples",
                   "Paths traced per texel; 0 uses Bakery's default", 4096.0f),
    LIGHTING_COUNT(VkrEditorLightmapSettings, max_depth, "max_depth", "Bounces",
                   "Bounces per path; 0 uses Bakery's default", 64.0f),
    LIGHTING_COUNT(VkrEditorLightmapSettings, seed, "seed", "Seed",
                   "Random seed; 0 uses Bakery's default", 1000000.0f),
    LIGHTING_COUNT(VkrEditorLightmapSettings, page_size, "page_size",
                   "Page size",
                   "Texels along a page's edge, a multiple of 4; "
                   "0 uses Bakery's default",
                   8192.0f),
    LIGHTING_SIZE(VkrEditorLightmapSettings, texels_per_unit, "texels_per_unit",
                  "Texel density",
                  "Lightmap texels per metre; 0 uses Bakery's default", "px/m",
                  0.5f),
    {.name = "denoise",
     .label = "Denoise",
     .tooltip = "Denoise the indirect light; default leaves Bakery's choice",
     .names = s_denoise_names,
     .offset = TYPE_OFFSET(VkrEditorLightmapSettings, denoise),
     .kind = VKR_PROPERTY_ENUM},
    LIGHTING_COUNT(VkrEditorLightmapSettings, denoise_iterations,
                   "denoise_iterations", "Denoise passes",
                   "0 uses Bakery's default", 64.0f),
    LIGHTING_SIZE(VkrEditorLightmapSettings, indirect_clamp, "indirect_clamp",
                  "Indirect clamp",
                  "Largest indirect radiance a sample keeps, against "
                  "fireflies; 0 uses Bakery's default",
                  NULL, 0.1f),
};

static const VkrPropertyDesc s_diffuse_properties[] = {
    LIGHTING_SIZE(VkrEditorDiffuseSettings, voxel_size, "voxel_size",
                  "Voxel size", "0 uses Bakery's default", "m", 0.05f),
    LIGHTING_SIZE(VkrEditorDiffuseSettings, face_size, "face_size", "Face size",
                  "0 uses Bakery's default", "m", 0.05f),
    LIGHTING_COUNT(VkrEditorDiffuseSettings, samples, "samples", "Samples",
                   "0 uses Bakery's default", 1000000.0f),
    LIGHTING_COUNT(VkrEditorDiffuseSettings, max_depth, "max_depth", "Bounces",
                   "0 uses Bakery's default", 64.0f),
    LIGHTING_COUNT(VkrEditorDiffuseSettings, seed, "seed", "Seed",
                   "0 uses Bakery's default", 1000000.0f),
    LIGHTING_COUNT(VkrEditorDiffuseSettings, photons, "photons", "Photons",
                   "0 uses Bakery's default", 100000000.0f),
    LIGHTING_SIZE(VkrEditorDiffuseSettings, photon_radius, "photon_radius",
                  "Photon radius", "0 uses Bakery's default", "m", 0.01f),
};

#undef LIGHTING_COUNT
#undef LIGHTING_SIZE
#undef TYPE_OFFSET

const VkrTypeDesc vkr_editor_lightmap_settings_type = {
    .name = "lightmap_settings",
    .label = "Lightmaps",
    .category = "Bake",
    .properties = s_lightmap_properties,
    .property_count = ArrayCount(s_lightmap_properties),
    .size = sizeof(VkrEditorLightmapSettings),
    .align = AlignOf(VkrEditorLightmapSettings),
};

const VkrTypeDesc vkr_editor_diffuse_settings_type = {
    .name = "diffuse_settings",
    .label = "Diffuse volume",
    .category = "Bake",
    .properties = s_diffuse_properties,
    .property_count = ArrayCount(s_diffuse_properties),
    .size = sizeof(VkrEditorDiffuseSettings),
    .align = AlignOf(VkrEditorDiffuseSettings),
};

/* One group's nonzero values as an object member `name`. */
static bool8_t bake_group_write(VkrJsonWriter *writer, const char *name,
                                const VkrTypeDesc *type, const void *value,
                                uint32_t samples) {
  uint32_t written = 0u;
  bool8_t ok = true_v;
  for (uint32_t i = 0; ok && i < type->property_count; ++i) {
    const VkrPropertyDesc *property = &type->properties[i];
    const uint8_t *field = (const uint8_t *)value + property->offset;
    const bool8_t is_samples =
        samples && strcmp(property->name, "samples") == 0;
    float64_t number = 0.0;
    if (property->kind == VKR_PROPERTY_F32) {
      number = (float64_t) * (const float32_t *)field;
    } else {
      number = (float64_t) * (const uint32_t *)field;
    }
    if (is_samples) {
      number = (float64_t)samples;
    }
    if (number == 0.0) {
      continue;
    }
    if (!written) {
      ok = vkr_json_writer_name(
               writer,
               string8_create_from_cstr((const uint8_t *)name, strlen(name))) &&
           vkr_json_writer_begin_object(writer);
    }
    written++;
    ok = ok && vkr_json_writer_name(writer, string8_create_from_cstr(
                                                (const uint8_t *)property->name,
                                                strlen(property->name)));
    if (property->kind == VKR_PROPERTY_ENUM) {
      /* `denoise` is a JSON boolean. */
      ok = ok && vkr_json_writer_bool(writer, number == 1.0);
    } else if (property->kind == VKR_PROPERTY_F32) {
      ok = ok && vkr_json_writer_f64(writer, number);
    } else {
      ok = ok && vkr_json_writer_u64(writer, (uint64_t)number);
    }
  }
  return ok && (!written || vkr_json_writer_end_object(writer));
}

bool8_t vkr_editor_bake_settings_write(VkrJsonWriter *writer,
                                       const VkrEditorBakeSettings *settings,
                                       uint32_t lightmap_samples) {
  return bake_group_write(writer, "lightmap_settings",
                          &vkr_editor_lightmap_settings_type,
                          &settings->lightmap, lightmap_samples) &&
         bake_group_write(writer, "diffuse_settings",
                          &vkr_editor_diffuse_settings_type, &settings->diffuse,
                          0u);
}

void vkr_editor_bake_settings_window_build(VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / Max(ui->content_scale, 0.001f);
  if (width < 64.0f) {
    return;
  }
  VkrEditorBakeSettings *settings =
      vkr_editor_projects_bake_settings(editor->projects);
  float32_t y = 6.0f;
  VkrUiWidgetConfig note = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      22.0f);
  note.style.font_size_pt = theme->font_caption;
  note.style.text_color = theme->text_secondary;
  if (!settings) {
    /* Bake lighting is a project job. */
    vkr_ui_label(ui, string8_lit("bake.none"),
                 string8_lit("Open a project to bake its scenes' lighting."),
                 &note);
    return;
  }
  vkr_ui_label(ui, string8_lit("bake.note"),
               string8_lit("Bake lighting sends these to Bakery; 0 keeps its "
                           "default."),
               &note);
  y += 26.0f;
  VkrEditorDetails *details = &editor->bake_settings_details;
  vkr_editor_details_begin(details);
  vkr_editor_details_error(details, ui, width, &y);
  const struct {
    const VkrTypeDesc *type;
    void *value;
    VkrUiIcon icon;
  } groups[] = {
      {&vkr_editor_lightmap_settings_type, &settings->lightmap,
       VKR_UI_ICON_SUN_DIM},
      {&vkr_editor_diffuse_settings_type, &settings->diffuse,
       VKR_UI_ICON_LIGHT},
  };
  for (uint32_t i = 0; i < ArrayCount(groups); ++i) {
    (void)vkr_ui_push_id_u64(ui, 0xbac0u + i);
    if (vkr_editor_details_section(
            ui, string8_lit("group"), width, &y, groups[i].icon, theme->accent,
            string8_create_from_cstr((const uint8_t *)groups[i].type->label,
                                     strlen(groups[i].type->label)),
            editor->heading_font, &editor->bake_settings_collapsed[i])) {
      (void)vkr_editor_details_type(details, ui, frame->input, width, &y,
                                    groups[i].type, groups[i].value, NULL,
                                    false_v);
    }
    (void)vkr_ui_pop_id(ui);
  }
  vkr_editor_details_end(details);
  vkr_editor_context_open_choice(editor, details);
  vkr_editor_color_picker_open(editor, details);
  y += 8.0f;
  VkrUiWidgetConfig bake = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, y, Min(200.0f, width - 20.0f), 26.0f);
  vkr_editor_action_style(&bake, editor->heading_font);
  bake.icon = VKR_UI_ICON_BAKERY;
  bake.icon_size_pt = 13.0f;
  bake.tooltip = string8_lit("Bake the open project scene's lighting with "
                             "these settings");
  if (vkr_ui_button(ui, string8_lit("bake.run"), string8_lit("Bake lighting"),
                    &bake)) {
    vkr_editor_command_execute(CMD_SCENE_BAKE, editor, frame);
  }
}

// =============================================================================
// Time of day
// =============================================================================

void vkr_editor_lighting_time_rows(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   float32_t width, float32_t *y) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrScene *world = frame->world;
  const VkrEntityId clock =
      world ? world->world_state.time_of_day_entity : VKR_ENTITY_ID_INVALID;
  const SceneTimeOfDay *authored =
      clock.u64 ? vkr_scene_get_typed(world, clock, &vkr_scene_time_of_day_type)
                : NULL;
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, *y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      20.0f);
  label.style.font_size_pt = theme->font_caption;
  label.style.text_color = theme->text_secondary;
  if (!authored || !authored->enabled || !frame->scene) {
    vkr_ui_label(ui, string8_lit("time.none"),
                 string8_lit("Add an enabled time of day to the World to "
                             "scrub it"),
                 &label);
    *y += 24.0f;
    return;
  }
  const float64_t hour = frame->scene->clock.hour;
  const uint32_t minutes = (uint32_t)(hour * 60.0 + 0.5) % (24u * 60u);
  vkr_ui_label(ui, string8_lit("time.hour"),
               string8_create_formatted(
                   ui->frame_allocator, "%02u:%02u  (starts %.2f h)",
                   minutes / 60u, minutes % 60u, (float64_t)authored->hour),
               &label);
  *y += 22.0f;
  float32_t value = (float32_t)hour;
  VkrUiWidgetConfig slider = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, *y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      22.0f);
  if (vkr_ui_slider_f32(ui, string8_lit("time.scrub"), &value, 0.0f, 24.0f,
                        &slider) &&
      frame->time_of_day_request) {
    frame->time_of_day_request->set_hour = true_v;
    frame->time_of_day_request->hour = (float64_t)value;
  }
  *y += 28.0f;
  VkrUiWidgetConfig keep = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, *y, Min(180.0f, width - 20.0f), 24.0f);
  vkr_editor_action_style(&keep, editor->heading_font);
  keep.icon = VKR_UI_ICON_CHECK;
  keep.icon_size_pt = 12.0f;
  keep.tooltip = string8_lit("Make this hour the World's starting hour "
                             "(undoable)");
  if (vkr_ui_button(ui, string8_lit("time.keep"), string8_lit("Keep hour"),
                    &keep)) {
    SceneTimeOfDay next = *authored;
    next.hour = (float32_t)fmod(hour, 24.0);
    vkr_editor_request_component(frame, clock, &vkr_scene_time_of_day_type,
                                 &next);
  }
  *y += 30.0f;
}

// =============================================================================
// Outlines
// =============================================================================

#define LIGHTING_CIRCLE_SEGMENTS 32u
/* A directional light's arrow, and the reach shown for an unbounded light. */
#define LIGHTING_ARROW_M 2.0f

typedef struct LightingLines {
  VkrEditorBrushGridLine *out;
  uint32_t capacity;
  uint32_t count;
} LightingLines;

static void lighting_line(LightingLines *lines, Vec3 from, Vec3 to,
                          Vec4 color) {
  if (lines->out && lines->count < lines->capacity) {
    lines->out[lines->count] =
        (VkrEditorBrushGridLine){.from = from, .to = to, .color = color};
  }
  ++lines->count;
}

/* A circle about `center` in the plane of the unit axes `u` and `v`. */
static void lighting_circle(LightingLines *lines, Vec3 center, Vec3 u, Vec3 v,
                            float32_t radius, Vec4 color) {
  Vec3 previous = vec3_add(center, vec3_scale(u, radius));
  for (uint32_t i = 1; i <= LIGHTING_CIRCLE_SEGMENTS; ++i) {
    const float32_t angle =
        (float32_t)i *
        (2.0f * 3.14159265f / (float32_t)LIGHTING_CIRCLE_SEGMENTS);
    const Vec3 point =
        vec3_add(center, vec3_add(vec3_scale(u, cosf(angle) * radius),
                                  vec3_scale(v, sinf(angle) * radius)));
    lighting_line(lines, previous, point, color);
    previous = point;
  }
}

/* The twelve edges of the box about `center` whose half extents lie along
   `half[0..2]`. */
static void lighting_box(LightingLines *lines, Vec3 center, const Vec3 half[3],
                         Vec4 color) {
  for (uint32_t corner = 0; corner < 8; ++corner) {
    Vec3 from = center;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      from = vec3_add(
          from, vec3_scale(half[axis], (corner & (1u << axis)) ? 1.0f : -1.0f));
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (corner & (1u << axis)) {
        continue;
      }
      lighting_line(lines, from, vec3_add(from, vec3_scale(half[axis], 2.0f)),
                    color);
    }
  }
}

static void lighting_arrow(LightingLines *lines, Vec3 from, Vec3 direction,
                           float32_t length, Vec4 color) {
  const Vec3 tip = vec3_add(from, vec3_scale(direction, length));
  const Vec3 side = fabsf(direction.y) < 0.9f
                        ? vec3_normalize(vec3_cross(direction, vec3_up()))
                        : vec3_normalize(vec3_cross(direction, vec3_right()));
  const Vec3 back = vec3_scale(direction, -length * 0.15f);
  lighting_line(lines, from, tip, color);
  lighting_line(lines, tip,
                vec3_add(tip, vec3_add(back, vec3_scale(side, length * 0.08f))),
                color);
  lighting_line(lines, tip,
                vec3_add(tip, vec3_sub(back, vec3_scale(side, length * 0.08f))),
                color);
}

/* Two unit axes perpendicular to unit `axis`. */
static void lighting_basis(Vec3 axis, Vec3 *u, Vec3 *v) {
  *u = vec3_normalize(
      vec3_cross(axis, fabsf(axis.y) < 0.9f ? vec3_up() : vec3_right()));
  *v = vec3_cross(axis, *u);
}

/* The world rotation through the hierarchy's quaternions, as the lighting
   system orients rectangle lights; false under a matrix-authored parent. */
static bool8_t lighting_world_rotation(const VkrScene *scene,
                                       const SceneTransform *transform,
                                       VkrQuat *out) {
  VkrQuat rotation = transform->rotation;
  VkrEntityId ancestor = transform->parent;
  for (uint32_t depth = 0u;
       ancestor.u64 != VKR_ENTITY_ID_INVALID.u64 && depth < scene->topo_count;
       ++depth) {
    const SceneTransform *parent = vkr_entity_get_component_if_alive_const(
        scene->world, ancestor, scene->comp_transform);
    if (!parent || !parent->trs_editable) {
      return false_v;
    }
    rotation = vkr_quat_mul(parent->rotation, rotation);
    ancestor = parent->parent;
  }
  if (ancestor.u64 != VKR_ENTITY_ID_INVALID.u64) {
    return false_v;
  }
  *out = vkr_quat_normalize(rotation);
  return true_v;
}

/* A box component's [-0.5, 0.5] cube through the entity's world matrix,
   and the same box grown by `margin` metres on every face. */
static void lighting_unit_box(LightingLines *lines,
                              const SceneTransform *transform, float32_t margin,
                              Vec4 color, Vec4 margin_color) {
  const Vec3 center = mat4_position(transform->world);
  Vec3 half[3];
  Vec3 grown[3];
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const Vec4 column =
        mat4_mul_vec4(transform->world,
                      (Vec4){axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
                             axis == 2 ? 1.0f : 0.0f, 0.0f});
    half[axis] = vec3_scale((Vec3){column.x, column.y, column.z}, 0.5f);
    const float32_t length = vec3_length(half[axis]);
    grown[axis] = length > 1.0e-6f
                      ? vec3_scale(half[axis], (length + margin) / length)
                      : half[axis];
  }
  lighting_box(lines, center, half, color);
  if (margin > 0.0f) {
    lighting_box(lines, center, grown, margin_color);
  }
}

static void lighting_entity_lines(LightingLines *lines, const VkrScene *scene,
                                  VkrEntityId entity) {
  /* A reflection probe places its box in world space; the rest follow the
     entity's transform. */
  const SceneReflectionProbeSettings *probe =
      vkr_scene_get_typed(scene, entity, &vkr_scene_reflection_probe_type);
  if (probe) {
    const Vec4 color = {0.55f, 1.0f, 0.6f, 1.0f};
    const Vec3 half[3] = {vec3_new(probe->extents.x, 0, 0),
                          vec3_new(0, probe->extents.y, 0),
                          vec3_new(0, 0, probe->extents.z)};
    lighting_box(lines, probe->center, half, color);
    if (probe->blend_distance > 0.0f) {
      const float32_t blend = probe->blend_distance;
      const Vec3 grown[3] = {vec3_new(probe->extents.x + blend, 0, 0),
                             vec3_new(0, probe->extents.y + blend, 0),
                             vec3_new(0, 0, probe->extents.z + blend)};
      lighting_box(lines, probe->center, grown,
                   (Vec4){0.55f, 1.0f, 0.6f, 0.35f});
    }
  }
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  if (!transform) {
    return;
  }
  const Vec3 position = mat4_position(transform->world);
  const Vec4 light = {1.0f, 0.85f, 0.35f, 1.0f};
  const Vec4 light_dim = {1.0f, 0.85f, 0.35f, 0.45f};

  const ScenePointLight *point = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_point_light);
  if (point) {
    const float32_t reach = point->range > 0.0f ? point->range : 0.0f;
    if (point->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT) {
      /* The cone as the lighting system aims it: the local direction
         through the entity's own rotation. */
      const Vec3 axis = vec3_normalize(
          vkr_quat_rotate_vec3(transform->rotation, point->direction_local));
      const float32_t length = reach > 0.0f ? reach : LIGHTING_ARROW_M;
      Vec3 u;
      Vec3 v;
      lighting_basis(axis, &u, &v);
      const struct {
        float32_t angle;
        Vec4 color;
      } cones[] = {{point->outer_cone_angle, light},
                   {point->inner_cone_angle, light_dim}};
      for (uint32_t c = 0; c < ArrayCount(cones); ++c) {
        const float32_t angle = Min(cones[c].angle, 1.55f);
        const Vec3 center =
            vec3_add(position, vec3_scale(axis, length * cosf(angle)));
        const float32_t radius = length * sinf(angle);
        lighting_circle(lines, center, u, v, radius, cones[c].color);
        if (c == 0) {
          for (uint32_t side = 0; side < 4; ++side) {
            const Vec3 edge = side & 1u ? v : u;
            const float32_t sign = side & 2u ? -1.0f : 1.0f;
            lighting_line(lines, position,
                          vec3_add(center, vec3_scale(edge, sign * radius)),
                          cones[c].color);
          }
        }
      }
      lighting_line(lines, position,
                    vec3_add(position, vec3_scale(axis, length)), light_dim);
    } else {
      /* Range as three great circles; an unbounded light shows a marker. */
      const float32_t radius = reach > 0.0f ? reach : 0.25f;
      const Vec4 color = reach > 0.0f ? light : light_dim;
      lighting_circle(lines, position, vec3_right(), vec3_up(), radius, color);
      lighting_circle(lines, position, vec3_right(), vec3_forward(), radius,
                      color);
      lighting_circle(lines, position, vec3_up(), vec3_forward(), radius,
                      color);
    }
  }

  const SceneRectangleLight *rect = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_rectangle_light);
  VkrQuat rotation;
  if (rect && lighting_world_rotation(scene, transform, &rotation)) {
    /* Local +X and +Y span it; it emits along local -Z. */
    const Vec3 half[2] = {
        vec3_scale(vkr_quat_rotate_vec3(rotation, vec3_right()),
                   rect->size.x * 0.5f),
        vec3_scale(vkr_quat_rotate_vec3(rotation, vec3_up()),
                   rect->size.y * 0.5f),
    };
    for (uint32_t corner = 0; corner < 4; ++corner) {
      const uint32_t next = corner == 0   ? 1u
                            : corner == 1 ? 3u
                            : corner == 3 ? 2u
                                          : 0u;
      const Vec3 a = vec3_add(
          position, vec3_add(vec3_scale(half[0], corner & 1u ? 1.0f : -1.0f),
                             vec3_scale(half[1], corner & 2u ? 1.0f : -1.0f)));
      const Vec3 b = vec3_add(
          position, vec3_add(vec3_scale(half[0], next & 1u ? 1.0f : -1.0f),
                             vec3_scale(half[1], next & 2u ? 1.0f : -1.0f)));
      lighting_line(lines, a, b, light);
    }
    const float32_t extent = Max(rect->size.x, rect->size.y);
    lighting_arrow(lines, position,
                   vkr_quat_rotate_vec3(rotation, vec3_new(0.0f, 0.0f, -1.0f)),
                   Clamp(extent * 0.5f, 0.25f, LIGHTING_ARROW_M), light);
  }

  const SceneDirectionalLight *sun = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_directional_light);
  if (sun) {
    lighting_arrow(lines, position,
                   vec3_normalize(vkr_quat_rotate_vec3(transform->rotation,
                                                       sun->direction_local)),
                   LIGHTING_ARROW_M, light);
  }

  const SceneLookVolume *look =
      vkr_scene_get_typed(scene, entity, &vkr_scene_look_volume_type);
  if (look) {
    lighting_unit_box(lines, transform, look->blend_distance,
                      (Vec4){0.4f, 0.85f, 1.0f, 1.0f},
                      (Vec4){0.4f, 0.85f, 1.0f, 0.35f});
  }
  const SceneDecal *decal =
      vkr_scene_get_typed(scene, entity, &vkr_scene_decal_type);
  if (decal) {
    const Vec4 color = {1.0f, 0.45f, 0.8f, 1.0f};
    lighting_unit_box(lines, transform, 0.0f, color, color);
    /* It projects along the box's -Y. */
    const Vec4 up = mat4_mul_vec4(transform->world, (Vec4){0, 1, 0, 0});
    const Vec3 axis = (Vec3){up.x, up.y, up.z};
    const float32_t height = vec3_length(axis);
    if (height > 1.0e-6f) {
      lighting_arrow(lines, vec3_add(position, vec3_scale(axis, 0.5f)),
                     vec3_scale(axis, -1.0f / height), height, color);
    }
  }
}

uint32_t vkr_editor_lighting_lines(const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrEditorBrushGridLine *out,
                                   uint32_t capacity) {
  if (frame->scripts_running) {
    return 0u;
  }
  LightingLines lines = {.out = out, .capacity = capacity};
  VkrEntityId selected[VKR_EDITOR_SELECTION_MAX + 1u];
  const uint32_t count =
      vkr_editor_selection_list(editor, frame, selected, ArrayCount(selected));
  for (uint32_t i = 0; i < count; ++i) {
    const VkrScene *scene = vkr_editor_entity_scene(frame, selected[i]);
    if (scene && vkr_scene_entity_alive(scene, selected[i])) {
      lighting_entity_lines(&lines, scene, selected[i]);
    }
  }
  return out ? Min(lines.count, capacity) : lines.count;
}

// =============================================================================
// Lights window
// =============================================================================

#define LIGHTS_ROW_PT 24.0f

const VkrTypeDesc *vkr_editor_light_type(VkrEditorLightKind kind) {
  return kind == VKR_EDITOR_LIGHT_DIRECTIONAL
             ? &vkr_scene_directional_light_type
         : kind == VKR_EDITOR_LIGHT_RECT ? &vkr_scene_rectangle_light_type
                                         : &vkr_scene_point_light_type;
}

static VkrUiIcon lights_icon(VkrEditorLightKind kind) {
  return kind == VKR_EDITOR_LIGHT_DIRECTIONAL ? VKR_UI_ICON_DIRECTIONAL_LIGHT
         : kind == VKR_EDITOR_LIGHT_SPOT      ? VKR_UI_ICON_SPOT_LIGHT
         : kind == VKR_EDITOR_LIGHT_RECT      ? VKR_UI_ICON_RECT_LIGHT
                                              : VKR_UI_ICON_POINT_LIGHT;
}

/* Lights keep their values in the scene's own components, not in typed
   (world type) storage. */
static VkrComponentTypeId lighting_component(const VkrScene *scene,
                                             VkrEditorLightKind kind) {
  return kind == VKR_EDITOR_LIGHT_DIRECTIONAL ? scene->comp_directional_light
         : kind == VKR_EDITOR_LIGHT_RECT      ? scene->comp_rectangle_light
                                              : scene->comp_point_light;
}

const void *vkr_editor_light_value(const VkrEditorLight *light) {
  return vkr_entity_get_component_if_alive_const(
      light->scene->world, light->entity,
      lighting_component(light->scene, light->kind));
}

/* `scene`'s lights in entity order, written while `count` is under
   `capacity`; returns how many it has. */
static uint32_t lighting_collect(const VkrScene *scene, VkrEditorLight *out,
                                 uint32_t capacity, uint32_t count) {
  static const VkrEditorLightKind kinds[] = {VKR_EDITOR_LIGHT_DIRECTIONAL,
                                             VKR_EDITOR_LIGHT_POINT,
                                             VKR_EDITOR_LIGHT_RECT};
  const VkrWorld *world = scene->world;
  for (uint32_t i = 0; i < world->dir.capacity; ++i) {
    if (!world->dir.records[i].chunk) {
      continue;
    }
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    for (uint32_t k = 0; k < ArrayCount(kinds); ++k) {
      VkrEditorLight light = {
          .scene = scene, .entity = entity, .kind = kinds[k]};
      const void *value = vkr_editor_light_value(&light);
      if (!value) {
        continue;
      }
      if (light.kind == VKR_EDITOR_LIGHT_POINT &&
          ((const ScenePointLight *)value)->kind ==
              VKR_POINT_LIGHT_KIND_GLTF_SPOT) {
        light.kind = VKR_EDITOR_LIGHT_SPOT;
      }
      if (out && count < capacity) {
        out[count] = light;
      }
      count++;
    }
  }
  return count;
}

uint32_t vkr_editor_lighting_list(const VkrSampleUiFrame *frame,
                                  VkrAllocator *allocator,
                                  VkrEditorLight **out) {
  const VkrScene *scenes[2] = {
      frame->scene, frame->world != frame->scene ? frame->world : NULL};
  uint32_t total = 0u;
  for (uint32_t s = 0; s < ArrayCount(scenes); ++s) {
    total = scenes[s] ? lighting_collect(scenes[s], NULL, 0u, total) : total;
  }
  *out = NULL;
  if (!total) {
    return 0u;
  }
  VkrEditorLight *entries =
      vkr_allocator_alloc(allocator, sizeof(VkrEditorLight) * total,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!entries) {
    return 0u;
  }
  uint32_t count = 0u;
  for (uint32_t s = 0; s < ArrayCount(scenes); ++s) {
    count =
        scenes[s] ? lighting_collect(scenes[s], entries, total, count) : count;
  }
  *out = entries;
  return Min(count, total);
}

static float32_t lights_caption(VkrUiSystem *ui, String8 id, float32_t y,
                                float32_t width, String8 text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      20.0f);
  label.style.font_size_pt = theme->font_caption;
  label.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, id, text, &label);
  return y + 22.0f;
}

/* Light groups scale their lights live, as scripts and the time of day's
   night groups do; a simulation reset restores one. */
static float32_t lights_groups(const VkrSampleUiFrame *frame, float32_t y,
                               float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrSceneLightGroups *groups = &frame->scene->light_groups;
  y = lights_caption(ui, string8_lit("lights.groups"), y, width,
                     string8_lit("LIGHT GROUPS"));
  if (!groups->count) {
    return lights_caption(ui, string8_lit("lights.groups.none"), y, width,
                          string8_lit("No groups yet: name one in a light's "
                                      "Group."));
  }
  const float32_t name_width = Min(140.0f, width * 0.35f);
  for (uint32_t i = 0; i < groups->count; ++i) {
    (void)vkr_ui_push_id_u64(ui, 0x9700u + i);
    VkrUiWidgetConfig name = vkr_editor_details_widget(
        VKR_EDITOR_DETAILS_PAD_PT, y, name_width, 22.0f);
    name.style.text_color = vkr_ui_theme()->text;
    const char *group = groups->names[i];
    vkr_ui_label(ui, string8_lit("name"),
                 string8_create_formatted(ui->frame_allocator, "%s  %.2f",
                                          group,
                                          (float64_t)groups->intensities[i]),
                 &name);
    float32_t value = groups->intensities[i];
    VkrUiWidgetConfig slider = vkr_editor_details_widget(
        VKR_EDITOR_DETAILS_PAD_PT + name_width + 6.0f, y,
        Max(40.0f,
            width - name_width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f - 6.0f),
        22.0f);
    if (vkr_ui_slider_f32(ui, string8_lit("intensity"), &value, 0.0f, 4.0f,
                          &slider) &&
        frame->time_of_day_request) {
      frame->time_of_day_request->set_group = true_v;
      snprintf(frame->time_of_day_request->group,
               sizeof(frame->time_of_day_request->group), "%s", group);
      frame->time_of_day_request->intensity = value;
    }
    (void)vkr_ui_pop_id(ui);
    y += 26.0f;
  }
  return lights_caption(ui, string8_lit("lights.groups.note"), y, width,
                        string8_lit("Live until the simulation resets; "
                                    "scripts set groups in play."));
}

/* The selected light's rows, edited in place as Details edits them. */
static float32_t lights_selected(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 const VkrEditorLight *entry, float32_t y,
                                 float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrTypeDesc *type = vkr_editor_light_type(entry->kind);
  const void *current = vkr_editor_light_value(entry);
  if (!current) {
    return y;
  }
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  MemCopy(value, current, type->size);
  const String8 name = vkr_scene_get_name(entry->scene, entry->entity);
  y = lights_caption(
      ui, string8_lit("lights.selected"), y, width,
      string8_create_formatted(ui->frame_allocator, "SELECTED: %.*s",
                               (int)name.length, (const char *)name.str));
  VkrEditorDetails *details = &editor->lights_details;
  vkr_editor_details_error(details, ui, width, &y);
  (void)vkr_ui_push_id_u64(ui, entry->entity.u64);
  const VkrEditorDetailsResult result = vkr_editor_details_type(
      details, ui, frame->input, width, &y, type, value, NULL, false_v);
  (void)vkr_ui_pop_id(ui);
  if (result.changed && frame->scene_edit) {
    VkrSceneEditValues values;
    MemZero(&values, sizeof(values));
    if (vkr_scene_edit_read(entry->scene, entry->entity, &values) &&
        vkr_scene_edit_component_set(&values, type, value)) {
      values.fields = vkr_scene_edit_component_field(type);
      *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                                 .entity = entry->entity,
                                                 .values = values,
                                                 .gesture = result.gesture};
    }
  }
  return y + 8.0f;
}

/* One list row: enabled, the name (selects it), mobility, and the group
   and intensity. */
static void lights_row(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                       const VkrEditorLight *entry, float32_t y,
                       float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrTypeDesc *type = vkr_editor_light_type(entry->kind);
  const void *current = vkr_editor_light_value(entry);
  if (!current) {
    return;
  }
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  MemCopy(value, current, type->size);
  bool8_t *enabled = NULL;
  VkrLightMobility *mobility = NULL;
  const char *group = "";
  float32_t intensity = 0.0f;
  if (entry->kind == VKR_EDITOR_LIGHT_DIRECTIONAL) {
    SceneDirectionalLight *light = (SceneDirectionalLight *)value;
    enabled = &light->enabled;
    intensity = light->intensity;
  } else if (entry->kind == VKR_EDITOR_LIGHT_RECT) {
    SceneRectangleLight *light = (SceneRectangleLight *)value;
    enabled = &light->enabled;
    mobility = &light->mobility;
    group = light->light_group;
    intensity = light->radiance;
  } else {
    ScenePointLight *light = (ScenePointLight *)value;
    enabled = &light->enabled;
    mobility = &light->mobility;
    group = light->light_group;
    intensity = light->intensity;
  }
  (void)vkr_ui_push_id_u64(ui, entry->entity.u64);
  const float32_t info_width = Min(110.0f, width * 0.28f);
  const float32_t mobility_width = 68.0f;
  const float32_t name_x = VKR_EDITOR_DETAILS_PAD_PT + 24.0f;
  const float32_t name_width =
      Max(40.0f, width - name_x - info_width - mobility_width -
                     VKR_EDITOR_DETAILS_PAD_PT - 8.0f);

  VkrUiWidgetConfig check = vkr_editor_details_widget(VKR_EDITOR_DETAILS_PAD_PT,
                                                      y + 2.0f, 20.0f, 20.0f);
  check.tooltip = string8_lit("Enabled (undoable)");
  bool8_t on = *enabled;
  if (vkr_ui_checkbox(ui, string8_lit("enabled"), (String8){0}, &on, &check)) {
    *enabled = on;
    vkr_editor_request_component(frame, entry->entity, type, value);
  }

  const bool8_t selected =
      vkr_editor_selection_contains(editor, frame, entry->entity);
  VkrUiWidgetConfig row =
      vkr_editor_details_widget(name_x, y, name_width, LIGHTS_ROW_PT - 2.0f);
  vkr_editor_ghost_style(&row);
  vkr_editor_toggle_style(&row, selected);
  row.icon = lights_icon(entry->kind);
  row.icon_size_pt = 13.0f;
  const String8 name = vkr_scene_get_name(entry->scene, entry->entity);
  row.tooltip = entry->scene == frame->world
                    ? string8_lit("A World light, shared by every scene")
                    : string8_lit("Select it");
  if (vkr_ui_button(ui, string8_lit("name"),
                    name.length ? name : string8_lit("(unnamed)"), &row) &&
      frame->scene_edit) {
    *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT,
                                               .entity = entry->entity};
  }

  VkrUiWidgetConfig move = vkr_editor_details_widget(
      name_x + name_width + 4.0f, y, mobility_width, LIGHTS_ROW_PT - 2.0f);
  if (mobility) {
    vkr_editor_ghost_style(&move);
    vkr_editor_toggle_style(&move, *mobility == VKR_LIGHT_MOBILITY_DYNAMIC);
    move.tooltip = string8_lit("Static lights bake into lightmaps; dynamic "
                               "lights light at runtime. Click to switch "
                               "(undoable)");
    if (vkr_ui_button(ui, string8_lit("mobility"),
                      *mobility == VKR_LIGHT_MOBILITY_DYNAMIC
                          ? string8_lit("Dynamic")
                          : string8_lit("Static"),
                      &move)) {
      *mobility = *mobility == VKR_LIGHT_MOBILITY_DYNAMIC
                      ? VKR_LIGHT_MOBILITY_STATIC
                      : VKR_LIGHT_MOBILITY_DYNAMIC;
      vkr_editor_request_component(frame, entry->entity, type, value);
    }
  }

  VkrUiWidgetConfig info =
      vkr_editor_details_widget(name_x + name_width + mobility_width + 8.0f, y,
                                info_width, LIGHTS_ROW_PT - 2.0f);
  info.style.font_size_pt = theme->font_caption;
  info.style.text_color =
      *enabled ? theme->text_secondary : theme->text_disabled;
  vkr_ui_label(ui, string8_lit("info"),
               string8_create_formatted(ui->frame_allocator, "%s%s%.3g", group,
                                        group[0] ? " \xc2\xb7 " : "",
                                        (float64_t)intensity),
               &info);
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_lights_window_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const float32_t width = bounds.width / Max(ui->content_scale, 0.001f);
  const float32_t height = bounds.height / Max(ui->content_scale, 0.001f);
  if (width < 64.0f || height < 24.0f) {
    return;
  }
  if (!frame->scene) {
    (void)lights_caption(ui, string8_lit("lights.none"), 8.0f, width,
                         string8_lit("Open a scene to list its lights"));
    return;
  }
  VkrEditorLight *entries = NULL;
  const uint32_t count =
      vkr_editor_lighting_list(frame, ui->frame_allocator, &entries);

  if (ui->mouse_input_layer == ui->input_layer && !ui->mouse_captured &&
      ui->mouse_x >= bounds.x && ui->mouse_x < bounds.x + bounds.width &&
      ui->mouse_y >= bounds.y && ui->mouse_y < bounds.y + bounds.height) {
    editor->lights_scroll -= ui->mouse_wheel * 40.0f;
  }
  const float32_t content = Max(height, editor->lights_height);
  editor->lights_scroll =
      Clamp(editor->lights_scroll, 0.0f, Max(0.0f, content - height));
  const VkrUiTrack content_track = {.value = content, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig area = vkr_ui_panel_config_default();
  area.placement.column = 0u;
  area.placement.row = 0u;
  area.rows = &content_track;
  area.row_count = 1u;
  area.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("lights.scroll"), &area)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &editor->lights_scroll);

  VkrEditorDetails *details = &editor->lights_details;
  vkr_editor_details_begin(details);
  float32_t y = 6.0f;
  y = lights_groups(frame, y, width);
  y += 6.0f;
  for (uint32_t i = 0; i < count; ++i) {
    if (entries[i].entity.u64 == frame->selected_entity.u64) {
      y = lights_selected(editor, frame, &entries[i], y, width);
      break;
    }
  }
  y = lights_caption(
      ui, string8_lit("lights.list"), y, width,
      string8_create_formatted(ui->frame_allocator, "LIGHTS (%u)", count));
  /* Only the rows in view are built. */
  const float32_t top = editor->lights_scroll - LIGHTS_ROW_PT;
  const float32_t bottom = editor->lights_scroll + height;
  for (uint32_t i = 0; i < count; ++i) {
    const float32_t row_y = y + (float32_t)i * LIGHTS_ROW_PT;
    if (row_y >= top && row_y <= bottom) {
      lights_row(editor, frame, &entries[i], row_y, width);
    }
  }
  y += (float32_t)count * LIGHTS_ROW_PT;
  vkr_editor_details_end(details);
  vkr_editor_context_open_choice(editor, details);
  vkr_editor_color_picker_open(editor, details);
  editor->lights_height = y + 24.0f;
  (void)vkr_ui_scroll_area_end(ui);
}
