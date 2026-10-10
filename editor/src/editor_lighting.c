#include "editor_lighting.h"

#include "editor_agent.h"
#include "editor_details.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_projects.h"
#include "editor_scene_panels.h"

#include "core/vkr_json_writer.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_types.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
    LIGHTING_SIZE(VkrEditorDiffuseSettings, spacing, "spacing", "Probe spacing",
                  "Finest probe spacing; a large level needs a wider one to "
                  "fit the volume's brick budget. 0 uses Bakery's default",
                  "m", 0.05f),
    LIGHTING_COUNT(VkrEditorDiffuseSettings, face_size, "face_size",
                   "Face size",
                   "Cube-face pixels per edge each probe gathers; 0 uses "
                   "Bakery's default",
                   32.0f),
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
    {.name = "bounds_min",
     .label = "Box min",
     .tooltip = "The volume's lowest world corner; both corners zero cover "
                "the whole scene",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrEditorDiffuseSettings, bounds_min),
     .kind = VKR_PROPERTY_VEC3,
     .min = -100000.0f,
     .max = 100000.0f,
     .step = 0.5f},
    {.name = "bounds_max",
     .label = "Box max",
     .tooltip = "The volume's highest world corner",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrEditorDiffuseSettings, bounds_max),
     .kind = VKR_PROPERTY_VEC3,
     .min = -100000.0f,
     .max = 100000.0f,
     .step = 0.5f},
};

bool8_t
vkr_editor_diffuse_bounds_set(const VkrEditorDiffuseSettings *settings) {
  return settings->bounds_max.x > settings->bounds_min.x &&
         settings->bounds_max.y > settings->bounds_min.y &&
         settings->bounds_max.z > settings->bounds_min.z;
}

/* A box is empty (the whole scene) or has max above min on every axis. */
static bool8_t diffuse_settings_validate(const void *value, char *error,
                                         uint32_t capacity) {
  const VkrEditorDiffuseSettings *settings = value;
  const Vec3 zero = {0};
  const bool8_t empty =
      MemCompare(&settings->bounds_min, &zero, sizeof(zero)) == 0 &&
      MemCompare(&settings->bounds_max, &zero, sizeof(zero)) == 0;
  if (!empty && !vkr_editor_diffuse_bounds_set(settings)) {
    snprintf(error, capacity,
             "The box needs max above min on every axis; both zero cover the "
             "whole scene.");
    return false_v;
  }
  return true_v;
}

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
    .validate = diffuse_settings_validate,
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
    if (property->kind == VKR_PROPERTY_VEC3) {
      /* The diffuse box goes as one `bounds` list below. */
      continue;
    }
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
  const VkrEditorDiffuseSettings *diffuse =
      type == &vkr_editor_diffuse_settings_type ? value : NULL;
  if (ok && diffuse && vkr_editor_diffuse_bounds_set(diffuse)) {
    /* `bounds`: min x, y, z then max x, y, z, as Bakery's --bounds. */
    if (!written) {
      ok = vkr_json_writer_name(
               writer,
               string8_create_from_cstr((const uint8_t *)name, strlen(name))) &&
           vkr_json_writer_begin_object(writer);
    }
    written++;
    ok = ok && vkr_json_writer_name(writer, string8_lit("bounds")) &&
         vkr_json_writer_begin_array(writer);
    const Vec3 corners[2] = {diffuse->bounds_min, diffuse->bounds_max};
    for (uint32_t c = 0; ok && c < 2u; ++c) {
      for (uint32_t a = 0; ok && a < 3u; ++a) {
        ok = vkr_json_writer_f64(writer, corners[c].elements[a]);
      }
    }
    ok = ok && vkr_json_writer_end_array(writer);
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

  /* The diffuse volume's box: around the selection, or the whole scene.
     The Scene draws it with face handles while this window is open. */
  const float32_t half = (width - VKR_EDITOR_DETAILS_PAD_PT * 3.0f) * 0.5f;
  VkrUiWidgetConfig fit = vkr_editor_details_widget(VKR_EDITOR_DETAILS_PAD_PT,
                                                    y + 4.0f, half, 24.0f);
  vkr_editor_ghost_style(&fit);
  fit.icon = VKR_UI_ICON_BOUNDING_BOX;
  fit.icon_size_pt = 12.0f;
  fit.tooltip = string8_lit("Fit the diffuse volume's box around the selected "
                            "object, 1 m wider on every side");
  const VkrScene *selected_scene =
      vkr_editor_entity_scene(frame, frame->selected_entity);
  if (vkr_ui_button(ui, string8_lit("bake.box_fit"),
                    string8_lit("Box around selection"), &fit) &&
      selected_scene) {
    VkrBrushGeometry *scratch = vkr_allocator_alloc(
        ui->frame_allocator, sizeof(*scratch), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    Vec3 lo = {0};
    Vec3 hi = {0};
    if (scratch &&
        vkr_editor_entity_world_box(selected_scene, frame->selected_entity,
                                    scratch, &lo, &hi)) {
      settings->diffuse.bounds_min = vec3_sub(lo, vec3_one());
      settings->diffuse.bounds_max = vec3_add(hi, vec3_one());
    } else {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning,
                       "Select an object with geometry to fit the box around");
    }
  }
  VkrUiWidgetConfig whole = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT * 2.0f + half, y + 4.0f, half, 24.0f);
  vkr_editor_ghost_style(&whole);
  whole.icon = VKR_UI_ICON_RESET;
  whole.icon_size_pt = 12.0f;
  whole.tooltip = string8_lit("Cover the whole scene, Bakery's default");
  if (vkr_ui_button(ui, string8_lit("bake.box_clear"),
                    string8_lit("Whole scene"), &whole)) {
    settings->diffuse.bounds_min = vec3_zero();
    settings->diffuse.bounds_max = vec3_zero();
  }
  y += 36.0f;
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

const SceneTimeOfDay *vkr_editor_lighting_clock(const VkrSampleUiFrame *frame,
                                                VkrEntityId *out_entity) {
  /* The rendered scene resolves the World's clock, a World-only type,
     into its own state. */
  const VkrEntityId clock = frame->scene
                                ? frame->scene->world_state.time_of_day_entity
                                : VKR_ENTITY_ID_INVALID;
  const VkrScene *owner =
      clock.u64 ? vkr_editor_entity_scene(frame, clock) : NULL;
  const SceneTimeOfDay *authored =
      owner ? vkr_scene_get_typed(owner, clock, &vkr_scene_time_of_day_type)
            : NULL;
  if (!authored || !authored->enabled) {
    return NULL;
  }
  if (out_entity) {
    *out_entity = clock;
  }
  return authored;
}

void vkr_editor_lighting_hour_text(float64_t hour, char out[8]) {
  const uint32_t minutes =
      (uint32_t)(fmod(fmod(hour, 24.0) + 24.0, 24.0) * 60.0 + 0.5) %
      (24u * 60u);
  snprintf(out, 8, "%02u:%02u", minutes / 60u, minutes % 60u);
}

void vkr_editor_lighting_scrub(const VkrSampleUiFrame *frame, float64_t hour) {
  if (frame->time_of_day_request && isfinite(hour)) {
    frame->time_of_day_request->set_hour = true_v;
    frame->time_of_day_request->hour = fmod(fmod(hour, 24.0) + 24.0, 24.0);
  }
}

void vkr_editor_lighting_keep_hour(const VkrSampleUiFrame *frame) {
  VkrEntityId clock = VKR_ENTITY_ID_INVALID;
  const SceneTimeOfDay *authored = vkr_editor_lighting_clock(frame, &clock);
  if (!authored) {
    return;
  }
  SceneTimeOfDay next = *authored;
  next.hour = (float32_t)fmod(frame->scene->clock.hour, 24.0);
  vkr_editor_request_component(frame, clock, &vkr_scene_time_of_day_type,
                               &next);
}

void vkr_editor_lighting_time_rows(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   float32_t width, float32_t *y) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const SceneTimeOfDay *authored = vkr_editor_lighting_clock(frame, NULL);
  VkrUiWidgetConfig label = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, *y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      20.0f);
  label.style.font_size_pt = theme->font_caption;
  label.style.text_color = theme->text_secondary;
  if (!authored) {
    vkr_ui_label(ui, string8_lit("time.none"),
                 string8_lit("Add an enabled time of day to the World to "
                             "scrub it"),
                 &label);
    *y += 24.0f;
    return;
  }
  const float64_t hour = frame->scene->clock.hour;
  char text[8];
  vkr_editor_lighting_hour_text(hour, text);
  vkr_ui_label(ui, string8_lit("time.hour"),
               string8_create_formatted(ui->frame_allocator,
                                        "%s  (starts %.2f h)", text,
                                        (float64_t)authored->hour),
               &label);
  *y += 22.0f;
  float32_t value = (float32_t)hour;
  VkrUiWidgetConfig slider = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, *y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      22.0f);
  if (vkr_ui_slider_f32(ui, string8_lit("time.scrub"), &value, 0.0f, 24.0f,
                        &slider)) {
    vkr_editor_lighting_scrub(frame, (float64_t)value);
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
    vkr_editor_lighting_keep_hour(frame);
  }
  *y += 30.0f;
}

// =============================================================================
// Outline handles
// =============================================================================

/* How near the pointer a handle takes it, in points on screen. */
#define HANDLES_PICK_PT 10.0f
/* Handles one object shows: a box's six faces at most. */
#define HANDLES_MAX 8u
#define HANDLES_LINE_MAX 64u
/* The smallest range, size or box side a drag leaves, in metres. */
#define HANDLES_MIN_M 0.05f

typedef enum HandleKind {
  HANDLE_RANGE = 0,
  HANDLE_CONE,
  HANDLE_RECT_WIDTH,
  HANDLE_RECT_HEIGHT,
  /* A face of a transform box (look volume, decal): `face` is axis * 2 +
     side. */
  HANDLE_BOX_FACE,
  /* A face of a reflection probe's world box. */
  HANDLE_PROBE_FACE,
  /* A face of the diffuse volume's box in the bake settings. */
  HANDLE_BOUNDS_FACE,
} HandleKind;

/* A handle at world `at`, dragged along unit world `axis`; a box face
   also keeps its distance from the box's center. */
typedef struct LightingHandle {
  HandleKind kind;
  uint32_t face;
  Vec3 at;
  Vec3 axis;
  float32_t half;
} LightingHandle;

typedef struct VkrEditorLightingHandles {
  /* What the handles edit: an entity's light or volume, else, with
     `bounds`, the diffuse volume's box in the bake settings. */
  VkrEntityId entity;
  bool8_t bounds;
  uint32_t count;
  LightingHandle handles[HANDLES_MAX];
  int32_t hot;
  /* The drag: its handle, the values it started from and where the
     pointer pressed along the handle's axis. */
  bool8_t dragging;
  LightingHandle drag;
  const VkrTypeDesc *type;
  _Alignas(16) uint8_t start[VKR_TYPE_VALUE_MAX];
  VkrSceneEditValues start_values;
  float32_t press_param;
  /* Whether the drag has applied an edit, which Escape then undoes. */
  bool8_t applied;
  uint64_t gesture;
  uint64_t gesture_counter;
  uint32_t line_count;
  VkrEditorBrushGridLine lines[HANDLES_LINE_MAX];
} VkrEditorLightingHandles;

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
  /* The diffuse volume's box while the Bake settings window shows it. */
  VkrEditorBakeSettings *settings =
      vkr_editor_projects_bake_settings(editor->projects);
  if (settings && editor->windows[VKR_EDITOR_WINDOW_BAKE_SETTINGS].visible &&
      vkr_editor_diffuse_bounds_set(&settings->diffuse)) {
    const Vec3 lo = settings->diffuse.bounds_min;
    const Vec3 hi = settings->diffuse.bounds_max;
    const Vec3 half[3] = {vec3_new((hi.x - lo.x) * 0.5f, 0, 0),
                          vec3_new(0, (hi.y - lo.y) * 0.5f, 0),
                          vec3_new(0, 0, (hi.z - lo.z) * 0.5f)};
    lighting_box(&lines, vec3_scale(vec3_add(lo, hi), 0.5f), half,
                 (Vec4){1.0f, 0.6f, 0.2f, 1.0f});
  }
  /* The drag handles of the last update. */
  const VkrEditorLightingHandles *handles = editor->lighting_handles;
  for (uint32_t i = 0; handles && (handles->entity.u64 || handles->bounds) &&
                       i < handles->line_count;
       ++i) {
    lighting_line(&lines, handles->lines[i].from, handles->lines[i].to,
                  handles->lines[i].color);
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

/* The parameter along the line through `anchor` along unit `axis` closest
   to the ray. */
static bool8_t handles_axis_param(Vec3 anchor, Vec3 axis, Vec3 origin,
                                  Vec3 direction, float32_t *out) {
  const Vec3 w = vec3_sub(anchor, origin);
  const float32_t b = vec3_dot(axis, direction);
  const float32_t denominator = 1.0f - b * b;
  if (denominator < 1.0e-6f) {
    return false_v;
  }
  *out = (b * vec3_dot(direction, w) - vec3_dot(axis, w)) / denominator;
  return isfinite(*out);
}

static void handles_add(VkrEditorLightingHandles *state, HandleKind kind,
                        uint32_t face, Vec3 at, Vec3 axis) {
  if (state->count < HANDLES_MAX && vec3_length(axis) > 1.0e-4f) {
    state->handles[state->count++] =
        (LightingHandle){.kind = kind,
                         .face = face,
                         .at = at,
                         .axis = vec3_normalize(axis),
                         .half = vec3_length(axis)};
  }
}

/* The component a handle edits on `entity`: its light or volume type. */
static const VkrTypeDesc *handles_type(const VkrScene *scene,
                                       VkrEntityId entity) {
  if (vkr_entity_get_component_if_alive_const(scene->world, entity,
                                              scene->comp_point_light)) {
    return &vkr_scene_point_light_type;
  }
  if (vkr_entity_get_component_if_alive_const(scene->world, entity,
                                              scene->comp_rectangle_light)) {
    return &vkr_scene_rectangle_light_type;
  }
  static const VkrTypeDesc *const boxes[] = {&vkr_scene_look_volume_type,
                                             &vkr_scene_decal_type,
                                             &vkr_scene_reflection_probe_type};
  for (uint32_t i = 0; i < ArrayCount(boxes); ++i) {
    if (vkr_scene_get_typed(scene, entity, boxes[i])) {
      return boxes[i];
    }
  }
  return NULL;
}

/* The value of `type` on `entity`: a light's from its scene component, a
   volume's from typed storage. */
static const void *handles_value(const VkrScene *scene, VkrEntityId entity,
                                 const VkrTypeDesc *type) {
  if (type == &vkr_scene_point_light_type) {
    return vkr_entity_get_component_if_alive_const(scene->world, entity,
                                                   scene->comp_point_light);
  }
  if (type == &vkr_scene_rectangle_light_type) {
    return vkr_entity_get_component_if_alive_const(scene->world, entity,
                                                   scene->comp_rectangle_light);
  }
  return vkr_scene_get_typed(scene, entity, type);
}

/* The handles of `entity` as its values are now. */
static void handles_build(VkrEditorLightingHandles *state,
                          const VkrScene *scene, VkrEntityId entity,
                          const VkrTypeDesc *type) {
  state->count = 0u;
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  if (type == &vkr_scene_reflection_probe_type) {
    const SceneReflectionProbeSettings *probe =
        vkr_scene_get_typed(scene, entity, type);
    for (uint32_t face = 0; face < 6u; ++face) {
      Vec3 axis = {0};
      axis.elements[face / 2u] = face & 1u ? -1.0f : 1.0f;
      handles_add(
          state, HANDLE_PROBE_FACE, face,
          vec3_add(probe->center,
                   vec3_scale(axis, probe->extents.elements[face / 2u])),
          axis);
    }
    return;
  }
  if (!transform) {
    return;
  }
  const Vec3 position = mat4_position(transform->world);
  if (type == &vkr_scene_point_light_type) {
    const ScenePointLight *point = vkr_entity_get_component_if_alive_const(
        scene->world, entity, scene->comp_point_light);
    if (!(point->range > 0.0f)) {
      return;
    }
    if (point->kind != VKR_POINT_LIGHT_KIND_GLTF_SPOT) {
      handles_add(state, HANDLE_RANGE, 0u,
                  vec3_add(position, vec3_new(point->range, 0, 0)),
                  vec3_new(1, 0, 0));
      return;
    }
    const Vec3 axis = vec3_normalize(
        vkr_quat_rotate_vec3(transform->rotation, point->direction_local));
    Vec3 u;
    Vec3 v;
    lighting_basis(axis, &u, &v);
    const float32_t angle = Min(point->outer_cone_angle, 1.55f);
    handles_add(state, HANDLE_RANGE, 0u,
                vec3_add(position, vec3_scale(axis, point->range)), axis);
    handles_add(state, HANDLE_CONE, 0u,
                vec3_add(position,
                         vec3_add(vec3_scale(axis, point->range * cosf(angle)),
                                  vec3_scale(u, point->range * sinf(angle)))),
                u);
    return;
  }
  if (type == &vkr_scene_rectangle_light_type) {
    const SceneRectangleLight *rect = vkr_entity_get_component_if_alive_const(
        scene->world, entity, scene->comp_rectangle_light);
    VkrQuat rotation;
    if (!lighting_world_rotation(scene, transform, &rotation)) {
      return;
    }
    const Vec3 right = vkr_quat_rotate_vec3(rotation, vec3_right());
    const Vec3 up = vkr_quat_rotate_vec3(rotation, vec3_up());
    handles_add(state, HANDLE_RECT_WIDTH, 0u,
                vec3_add(position, vec3_scale(right, rect->size.x * 0.5f)),
                right);
    handles_add(state, HANDLE_RECT_HEIGHT, 0u,
                vec3_add(position, vec3_scale(up, rect->size.y * 0.5f)), up);
    return;
  }
  /* A transform box: the [-0.5, 0.5] cube through the world matrix. */
  for (uint32_t face = 0; face < 6u; ++face) {
    Vec4 unit = {0};
    unit.elements[face / 2u] = 1.0f;
    const Vec4 column = mat4_mul_vec4(transform->world, unit);
    const Vec3 half = vec3_scale((Vec3){column.x, column.y, column.z},
                                 face & 1u ? -0.5f : 0.5f);
    handles_add(state, HANDLE_BOX_FACE, face, vec3_add(position, half), half);
  }
}

/* A world box's six faces, as `kind` handles. */
static void handles_box_faces(VkrEditorLightingHandles *state, HandleKind kind,
                              Vec3 lo, Vec3 hi) {
  const Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
  for (uint32_t face = 0; face < 6u; ++face) {
    const uint32_t axis = face / 2u;
    Vec3 normal = {0};
    normal.elements[axis] = face & 1u ? -1.0f : 1.0f;
    Vec3 at = center;
    at.elements[axis] = face & 1u ? lo.elements[axis] : hi.elements[axis];
    handles_add(state, kind, face, at, normal);
  }
}

/* The diffuse box a drag of `delta` metres makes from the start: the face
   moves and the opposite face stays. */
static void handles_drag_bounds(const VkrEditorLightingHandles *state,
                                float32_t delta,
                                VkrEditorDiffuseSettings *settings) {
  Vec3 lo;
  Vec3 hi;
  MemCopy(&lo, state->start, sizeof(lo));
  MemCopy(&hi, state->start + sizeof(lo), sizeof(hi));
  const uint32_t axis = state->drag.face / 2u;
  if (state->drag.face & 1u) {
    lo.elements[axis] =
        Min(lo.elements[axis] - delta, hi.elements[axis] - HANDLES_MIN_M);
  } else {
    hi.elements[axis] =
        Max(hi.elements[axis] + delta, lo.elements[axis] + HANDLES_MIN_M);
  }
  settings->bounds_min = lo;
  settings->bounds_max = hi;
}

/* The values a drag of `delta` metres along the handle's axis makes from
   the start, as one scene edit. */
static bool8_t handles_drag_values(const VkrEditorLightingHandles *state,
                                   const VkrScene *scene, float32_t delta,
                                   VkrSceneEditValues *out) {
  const LightingHandle *handle = &state->drag;
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  MemCopy(value, state->start, state->type->size);
  *out = state->start_values;
  switch (handle->kind) {
  case HANDLE_RANGE: {
    ScenePointLight *point = (ScenePointLight *)value;
    point->range = Max(HANDLES_MIN_M, point->range + delta);
    break;
  }
  case HANDLE_CONE: {
    /* The rim's distance from the axis over its distance along it. */
    ScenePointLight *point = (ScenePointLight *)value;
    const float32_t angle = Min(point->outer_cone_angle, 1.55f);
    const float32_t along = point->range * cosf(angle);
    const float32_t across = Max(0.0f, point->range * sinf(angle) + delta);
    point->outer_cone_angle = Clamp(atan2f(across, Max(along, 1.0e-3f)),
                                    point->inner_cone_angle + 0.01f, 1.5707f);
    break;
  }
  case HANDLE_RECT_WIDTH:
  case HANDLE_RECT_HEIGHT: {
    /* Both edges move, so the light stays centred. */
    SceneRectangleLight *rect = (SceneRectangleLight *)value;
    float32_t *size =
        handle->kind == HANDLE_RECT_WIDTH ? &rect->size.x : &rect->size.y;
    *size = Max(HANDLES_MIN_M, *size + 2.0f * delta);
    break;
  }
  case HANDLE_BOUNDS_FACE:
    return false_v;
  case HANDLE_PROBE_FACE: {
    /* The face moves; the opposite face stays. */
    SceneReflectionProbeSettings *probe = (SceneReflectionProbeSettings *)value;
    const uint32_t axis = handle->face / 2u;
    const float32_t sign = handle->face & 1u ? -1.0f : 1.0f;
    const float32_t extent =
        Max(HANDLES_MIN_M * 0.5f, probe->extents.elements[axis] + delta * 0.5f);
    probe->center.elements[axis] +=
        sign * (extent - probe->extents.elements[axis]);
    probe->extents.elements[axis] = extent;
    break;
  }
  case HANDLE_BOX_FACE: {
    /* The face moves along its axis and the opposite face stays: the
       entity's scale grows by the share of its side the drag adds, and
       its center moves half the drag, in its parent's space. */
    const uint32_t axis = handle->face / 2u;
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        scene->world, state->entity, scene->comp_transform);
    if (!transform) {
      return false_v;
    }
    const float32_t length = state->start_values.scale.elements[axis];
    const float32_t world_length = 2.0f * handle->half;
    const float32_t grown = Max(HANDLES_MIN_M, world_length + delta);
    out->scale.elements[axis] = length * grown / Max(world_length, 1.0e-4f);
    const Vec3 shift = vec3_scale(handle->axis, (grown - world_length) * 0.5f);
    Mat4 to_parent = mat4_identity();
    const SceneTransform *parent =
        transform->parent.u64
            ? vkr_entity_get_component_if_alive_const(
                  scene->world, transform->parent, scene->comp_transform)
            : NULL;
    if (parent) {
      to_parent = mat4_inverse(parent->world);
    }
    const Vec4 local = mat4_mul_vec4(to_parent, vec3_to_vec4(shift, 0.0f));
    out->position = vec3_add(state->start_values.position,
                             (Vec3){local.x, local.y, local.z});
    out->fields = VKR_SCENE_EDIT_TRANSFORM;
    return true_v;
  }
  }
  const uint32_t field = vkr_scene_edit_component_field(state->type);
  if (field) {
    if (!vkr_scene_edit_component_set(out, state->type, value)) {
      return false_v;
    }
    out->fields = field;
  } else {
    out->fields = VKR_SCENE_EDIT_COMPONENT;
    out->component_type = state->type;
    MemCopy(out->component, value, state->type->size);
  }
  return true_v;
}

static void handles_line(VkrEditorLightingHandles *state, Vec3 from, Vec3 to,
                         Vec4 color) {
  if (state->line_count < HANDLES_LINE_MAX) {
    state->lines[state->line_count++] =
        (VkrEditorBrushGridLine){.from = from, .to = to, .color = color};
  }
}

/* Each handle as a small cross sized to its distance from the eye, and a
   short arrow along its axis; the hot or dragged one brighter. */
static void handles_lines(VkrEditorLightingHandles *state, Vec3 eye) {
  static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (uint32_t i = 0; i < state->count; ++i) {
    const LightingHandle *handle = &state->handles[i];
    const bool8_t hot = (int32_t)i == state->hot ||
                        (state->dragging && handle->kind == state->drag.kind &&
                         handle->face == state->drag.face);
    const Vec4 color = hot ? (Vec4){1.0f, 1.0f, 0.55f, 1.0f}
                           : (Vec4){0.45f, 0.75f, 1.0f, 1.0f};
    const float32_t size =
        0.012f * vec3_length(vec3_sub(eye, handle->at)) * (hot ? 1.6f : 1.0f);
    for (uint32_t a = 0; a < 3u; ++a) {
      const Vec3 half = vec3_scale(axes[a], size);
      handles_line(state, vec3_sub(handle->at, half),
                   vec3_add(handle->at, half), color);
    }
    handles_line(state, handle->at,
                 vec3_add(handle->at, vec3_scale(handle->axis, size * 4.0f)),
                 color);
  }
}

/* The handle under the pointer, within HANDLES_PICK_PT, or -1. */
static int32_t handles_pick(const VkrEditorLightingHandles *state,
                            const VkrSampleUiFrame *frame) {
  int32_t best = -1;
  float32_t best_gap = HANDLES_PICK_PT * frame->ui->content_scale;
  for (uint32_t i = 0; i < state->count; ++i) {
    Vec2 pixel = {0};
    if (!vkr_editor_viewport_pixel(frame, state->handles[i].at, &pixel)) {
      continue;
    }
    const float32_t dx = pixel.x - (float32_t)frame->ui->mouse_x;
    const float32_t dy = pixel.y - (float32_t)frame->ui->mouse_y;
    const float32_t gap = sqrtf(dx * dx + dy * dy);
    if (gap < best_gap) {
      best_gap = gap;
      best = (int32_t)i;
    }
  }
  return best;
}

void vkr_editor_lighting_handles_update(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        VkrEntityId entity, Vec3 origin,
                                        Vec3 direction, bool8_t has_ray,
                                        bool8_t inside) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  const VkrTypeDesc *type =
      scene && vkr_scene_entity_alive(scene, entity) && !frame->scripts_running
          ? handles_type(scene, entity)
          : NULL;
  /* Without a light or volume selected, the Bake settings window shows the
     diffuse volume's box. */
  VkrEditorBakeSettings *settings =
      vkr_editor_projects_bake_settings(editor->projects);
  const bool8_t bounds =
      !type && has_ray && settings &&
      editor->windows[VKR_EDITOR_WINDOW_BAKE_SETTINGS].visible &&
      vkr_editor_diffuse_bounds_set(&settings->diffuse);
  VkrEditorLightingHandles *state = editor->lighting_handles;
  if (!state && (type || bounds)) {
    state = calloc(1u, sizeof(*state));
    editor->lighting_handles = state;
  }
  if (!state) {
    return;
  }
  state->line_count = 0u;
  state->hot = -1;
  if ((!type && !bounds) || entity.u64 != state->entity.u64 ||
      bounds != state->bounds) {
    state->dragging = false_v;
  }
  state->entity = type ? entity : VKR_ENTITY_ID_INVALID;
  state->bounds = bounds;
  state->count = 0u;
  if (!type && !bounds) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  if (state->dragging) {
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
        (VkrUiRect){image.x, image.y, image.z, image.w});
    const bool8_t cancel = input_key_just_pressed(frame->input, KEY_ESCAPE);
    float32_t param = 0.0f;
    float32_t delta = 0.0f;
    const bool8_t moved =
        cancel ||
        (has_ray && handles_axis_param(state->drag.at, state->drag.axis, origin,
                                       direction, &param));
    if (moved) {
      /* Escape puts the start back and ends the drag. */
      delta = cancel ? 0.0f : param - state->press_param;
      VkrSceneEditValues values;
      if (bounds) {
        handles_drag_bounds(state, delta, &settings->diffuse);
      } else if (cancel && !state->applied) {
        /* Nothing moved yet: Escape leaves no undo step. */
      } else if (frame->scene_edit &&
                 handles_drag_values(state, scene, delta, &values)) {
        state->applied = true_v;
        *frame->scene_edit =
            (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                  .entity = entity,
                                  .values = values,
                                  .gesture = state->gesture};
      }
    }
    if (cancel || !input_is_button_down(frame->input, BUTTON_LEFT) ||
        ui->mouse_released) {
      state->dragging = false_v;
    }
  }
  if (bounds) {
    handles_box_faces(state, HANDLE_BOUNDS_FACE, settings->diffuse.bounds_min,
                      settings->diffuse.bounds_max);
  } else {
    handles_build(state, scene, entity, type);
  }
  if (!state->dragging && inside) {
    state->hot = handles_pick(state, frame);
    if (state->hot >= 0) {
      /* The Scene stops picking objects under a handle. */
      (void)vkr_ui_input_layer_register(
          ui, VKR_EDITOR_VIEW_TOOLBAR_LAYER,
          (VkrUiRect){image.x, image.y, image.z, image.w});
      const LightingHandle handle = state->handles[state->hot];
      const void *value = bounds ? NULL : handles_value(scene, entity, type);
      if (ui->mouse_pressed && (bounds || value) &&
          (bounds ||
           vkr_scene_edit_read(scene, entity, &state->start_values)) &&
          handles_axis_param(handle.at, handle.axis, origin, direction,
                             &state->press_param)) {
        state->dragging = true_v;
        state->applied = false_v;
        state->drag = handle;
        state->type = type;
        if (bounds) {
          MemCopy(state->start, &settings->diffuse.bounds_min, sizeof(Vec3));
          MemCopy(state->start + sizeof(Vec3), &settings->diffuse.bounds_max,
                  sizeof(Vec3));
        } else {
          MemCopy(state->start, value, type->size);
        }
        /* Live edits of one drag share a gesture, one undo step. */
        state->gesture = (1ull << 62) | ++state->gesture_counter;
      }
    }
  }
  handles_lines(state, origin);
}

bool8_t vkr_editor_lighting_handles_busy(const VkrEditorUi *editor) {
  const VkrEditorLightingHandles *state = editor->lighting_handles;
  return state && (state->entity.u64 || state->bounds) &&
         (state->hot >= 0 || state->dragging);
}

const char *vkr_editor_lighting_handles_hint(const VkrEditorUi *editor) {
  const VkrEditorLightingHandles *state = editor->lighting_handles;
  if (!state || (!state->entity.u64 && !state->bounds) ||
      (state->hot < 0 && !state->dragging)) {
    return NULL;
  }
  const HandleKind kind =
      state->dragging ? state->drag.kind : state->handles[state->hot].kind;
  switch (kind) {
  case HANDLE_RANGE:
    return "Drag to set the light's range. Esc puts it back.";
  case HANDLE_CONE:
    return "Drag out or in to widen or narrow the spot's cone. Esc puts it "
           "back.";
  case HANDLE_RECT_WIDTH:
  case HANDLE_RECT_HEIGHT:
    return "Drag to resize the light about its center. Esc puts it back.";
  case HANDLE_BOX_FACE:
  case HANDLE_PROBE_FACE:
    return "Drag the face; the opposite face stays. Esc puts it back.";
  case HANDLE_BOUNDS_FACE:
    return "Drag the diffuse volume's face; the next Bake lighting covers "
           "the box. Esc puts it back.";
  }
  return NULL;
}

void vkr_editor_lighting_handles_destroy(VkrEditorUi *editor) {
  free(editor->lighting_handles);
  editor->lighting_handles = NULL;
}

// =============================================================================
// Probes
// =============================================================================

void vkr_editor_lighting_create_probe(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  if (!editor->agent) {
    return;
  }
  if (!vkr_editor_projects_managed_scene(editor->projects, frame)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "Reflection probes are baked into a project scene: open "
                     "one to add a probe");
    return;
  }
  /* A room-sized box standing where the Scene's center meets a surface. */
  const Vec4 image = frame->mapping.image_rect_px;
  VkrEditorDropPose pose;
  if (!vkr_editor_viewport_place(
          editor, frame,
          (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f}, 2.5f,
          &pose)) {
    return;
  }
  char line[320];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"probe\",\"op\":\"probe.create\",\"args\":{"
           "\"center\":[%g,%g,%g],\"review\":false,\"select\":true}}",
           pose.position.x, pose.position.y, pose.position.z);
  (void)vkr_editor_agent_submit(editor->agent, line);
}
