#include "editor_environment.h"

#include "editor_details.h"
#include "editor_internal.h"
#include "editor_material.h"
#include "vkr_bakery_json.h"

#include "core/vkr_json_writer.h"
#include "filesystem/vkr_asset_path.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_types.h"

#include <stdio.h>
#include <string.h>

#define ENVIRONMENT_VERSION 1u
/* Bytes of one preset's text; nine parts of a few hundred bytes each. */
#define ENVIRONMENT_TEXT_CAPACITY (32u * 1024u)

typedef struct EnvironmentPartInfo {
  const char *key;
  const char *label;
  const VkrTypeDesc *type;
} EnvironmentPartInfo;

static EnvironmentPartInfo environment_part(VkrEditorEnvironmentPart part) {
  switch (part) {
  case VKR_EDITOR_ENVIRONMENT_SKY_LIGHT:
    return (EnvironmentPartInfo){"environment", "Sky light",
                                 &vkr_scene_environment_type};
  case VKR_EDITOR_ENVIRONMENT_ATMOSPHERE:
    return (EnvironmentPartInfo){"atmosphere", "Atmosphere",
                                 &vkr_scene_atmosphere_type};
  case VKR_EDITOR_ENVIRONMENT_CLOUDS:
    return (EnvironmentPartInfo){"clouds", "Clouds", &vkr_scene_clouds_type};
  case VKR_EDITOR_ENVIRONMENT_FOG:
    return (EnvironmentPartInfo){"fog", "Height fog", &vkr_scene_fog_type};
  case VKR_EDITOR_ENVIRONMENT_VOLUMETRIC_FOG:
    return (EnvironmentPartInfo){"volumetric_fog", "Volumetric fog",
                                 &vkr_scene_froxel_fog_type};
  case VKR_EDITOR_ENVIRONMENT_POST_PROCESS:
    return (EnvironmentPartInfo){"post_process", "Post process",
                                 &vkr_scene_post_process_type};
  case VKR_EDITOR_ENVIRONMENT_TIME_OF_DAY:
    return (EnvironmentPartInfo){"time_of_day", "Time of day",
                                 &vkr_scene_time_of_day_type};
  case VKR_EDITOR_ENVIRONMENT_SUN:
    return (EnvironmentPartInfo){"sun", "Sun",
                                 &vkr_scene_directional_light_type};
  case VKR_EDITOR_ENVIRONMENT_MOON:
  default:
    return (EnvironmentPartInfo){"moon", "Moon",
                                 &vkr_scene_directional_light_type};
  }
}

const char *vkr_editor_environment_key(VkrEditorEnvironmentPart part) {
  return environment_part(part).key;
}

const char *vkr_editor_environment_label(VkrEditorEnvironmentPart part) {
  return environment_part(part).label;
}

const VkrTypeDesc *vkr_editor_environment_type(VkrEditorEnvironmentPart part) {
  return environment_part(part).type;
}

static bool8_t environment_light_part(VkrEditorEnvironmentPart part) {
  return part == VKR_EDITOR_ENVIRONMENT_SUN ||
         part == VKR_EDITOR_ENVIRONMENT_MOON;
}

static const VkrScene *environment_scene(const VkrSampleUiFrame *frame,
                                         uint16_t container) {
  if (container == VKR_SCENE_WORLD_ROOT_ID) {
    return frame->world;
  }
  if (container == 0u) {
    return frame->scene;
  }
  return container <= VKR_SCENE_ADDITIVE_MAX ? frame->additive[container - 1u]
                                             : NULL;
}

/* The winning entity of a singleton part, as world resolution chose it. */
static VkrEntityId environment_winner(const VkrScene *scene,
                                      VkrEditorEnvironmentPart part) {
  const VkrSceneWorldState *state = &scene->world_state;
  switch (part) {
  case VKR_EDITOR_ENVIRONMENT_SKY_LIGHT:
    return state->environment_entity;
  case VKR_EDITOR_ENVIRONMENT_ATMOSPHERE:
    return state->atmosphere_entity;
  case VKR_EDITOR_ENVIRONMENT_CLOUDS:
    return state->clouds_entity;
  case VKR_EDITOR_ENVIRONMENT_FOG:
    return state->fog_entity;
  case VKR_EDITOR_ENVIRONMENT_VOLUMETRIC_FOG:
    return state->froxel_fog_entity;
  case VKR_EDITOR_ENVIRONMENT_POST_PROCESS:
    return state->post_process_entity;
  case VKR_EDITOR_ENVIRONMENT_TIME_OF_DAY:
    return state->time_of_day_entity;
  default:
    return VKR_ENTITY_ID_INVALID;
  }
}

/* The first enabled directional light of `scene` that drives the
   atmosphere's sun, or with `moon` its moon. */
static VkrEntityId environment_light(const VkrScene *scene, bool8_t moon) {
  const VkrWorld *world = scene->world;
  for (uint32_t i = 0; i < world->dir.capacity; ++i) {
    if (!world->dir.records[i].chunk) {
      continue;
    }
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    const SceneDirectionalLight *light =
        vkr_entity_get_component(world, entity, scene->comp_directional_light);
    if (light && light->enabled &&
        (moon ? light->atmosphere_moon
              : light->atmosphere_sun && !light->atmosphere_moon)) {
      return entity;
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

/* Reads `entity`'s value of `type` in `owner`; false when it has none. */
static bool8_t environment_value(const VkrScene *owner, VkrEntityId entity,
                                 const VkrTypeDesc *type, void *out) {
  if (!owner || !entity.u64 || !vkr_scene_entity_alive(owner, entity)) {
    return false_v;
  }
  if (vkr_scene_edit_component_field(type)) {
    VkrSceneEditValues values;
    return vkr_scene_edit_read(owner, entity, &values) &&
           vkr_scene_edit_component_get(&values, type, out);
  }
  const void *value = vkr_scene_get_typed(owner, entity, type);
  if (!value) {
    return false_v;
  }
  MemCopy(out, value, type->size);
  return true_v;
}

bool8_t vkr_editor_environment_resolve(
    const VkrSampleUiFrame *frame, uint16_t container,
    VkrEditorEnvironmentSource out[VKR_EDITOR_ENVIRONMENT_PART_COUNT]) {
  const VkrScene *scene = environment_scene(frame, container);
  if (!scene) {
    return false_v;
  }
  const bool8_t world_container = container == VKR_SCENE_WORLD_ROOT_ID;
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    const VkrEditorEnvironmentPart part = (VkrEditorEnvironmentPart)i;
    const VkrTypeDesc *type = vkr_editor_environment_type(part);
    VkrEditorEnvironmentSource *source = &out[i];
    MemZero(source, sizeof(*source));
    vkr_type_defaults(type, source->value);
    VkrEntityId entity = VKR_ENTITY_ID_INVALID;
    const VkrScene *owner = scene;
    if (environment_light_part(part)) {
      const bool8_t moon = part == VKR_EDITOR_ENVIRONMENT_MOON;
      entity = environment_light(scene, moon);
      /* The World's lights light every scene beside it. */
      if (!entity.u64 && !world_container && frame->world) {
        owner = frame->world;
        entity = environment_light(frame->world, moon);
      }
    } else {
      entity = environment_winner(scene, part);
      if (entity.u64 && entity.parts.world == VKR_SCENE_WORLD_ROOT_ID) {
        owner = frame->world;
      }
    }
    if (environment_value(owner, entity, type, source->value)) {
      source->entity = entity;
      source->owner = owner;
      source->from_world = !world_container && owner == frame->world;
    } else {
      vkr_type_defaults(type, source->value);
    }
  }
  return true_v;
}

void vkr_editor_environment_capture(
    const VkrEditorEnvironmentSource sources[VKR_EDITOR_ENVIRONMENT_PART_COUNT],
    VkrEditorEnvironmentPreset *out) {
  out->parts = 0u;
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    if (!sources[i].entity.u64) {
      continue;
    }
    out->parts |= 1u << i;
    MemCopy(out->values[i], sources[i].value,
            vkr_editor_environment_type((VkrEditorEnvironmentPart)i)->size);
  }
}

// =============================================================================
// Documents
// =============================================================================

typedef struct EnvironmentText {
  char *data;
  uint64_t length;
  uint64_t capacity;
} EnvironmentText;

static bool8_t environment_sink(void *context, const uint8_t *data,
                                uint64_t length) {
  EnvironmentText *text = context;
  if (text->length + length >= text->capacity) {
    return false_v;
  }
  MemCopy(text->data + text->length, data, length);
  text->length += length;
  return true_v;
}

bool8_t vkr_editor_environment_write(const VkrEditorEnvironmentPreset *preset,
                                     VkrAllocator *allocator, String8 *out) {
  EnvironmentText text = {
      .data = vkr_allocator_alloc(allocator, ENVIRONMENT_TEXT_CAPACITY,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING),
      .capacity = ENVIRONMENT_TEXT_CAPACITY,
  };
  if (!text.data) {
    return false_v;
  }
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, environment_sink, &text);
  bool8_t ok = vkr_json_writer_begin_object(&writer) &&
               vkr_json_writer_name(&writer, string8_lit("version")) &&
               vkr_json_writer_u64(&writer, ENVIRONMENT_VERSION) &&
               vkr_json_writer_name(&writer, string8_lit("parts")) &&
               vkr_json_writer_begin_object(&writer);
  for (uint32_t i = 0; ok && i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    if (!(preset->parts & (1u << i))) {
      continue;
    }
    const VkrEditorEnvironmentPart part = (VkrEditorEnvironmentPart)i;
    const char *key = vkr_editor_environment_key(part);
    ok = vkr_json_writer_name(
             &writer,
             string8_create_from_cstr((const uint8_t *)key, strlen(key))) &&
         vkr_type_write_json(&writer, vkr_editor_environment_type(part),
                             preset->values[i]);
  }
  ok = ok && vkr_json_writer_end_object(&writer) &&
       vkr_json_writer_end_object(&writer) &&
       vkr_json_writer_complete(&writer) &&
       environment_sink(&text, (const uint8_t *)"\n", 1u);
  if (!ok) {
    return false_v;
  }
  *out = (String8){.str = (uint8_t *)text.data, .length = text.length};
  return true_v;
}

bool8_t vkr_editor_environment_read(String8 text, Arena *arena,
                                    VkrEditorEnvironmentPreset *out,
                                    char *error, uint32_t capacity) {
  out->parts = 0u;
  const VkrBakeryJson *root =
      vkr_bakery_json_parse(arena, text.str, text.length, 32u, NULL);
  int64_t version = 0;
  const VkrBakeryJson *parts = root ? vkr_bakery_json_get(root, "parts") : NULL;
  if (!root || !vkr_bakery_json_get_int(root, "version", &version) ||
      version != ENVIRONMENT_VERSION || !parts ||
      parts->type != VKR_BAKERY_JSON_OBJECT) {
    snprintf(error, capacity,
             "An environment preset is {\"version\": %u, \"parts\": {...}}",
             ENVIRONMENT_VERSION);
    return false_v;
  }
  VkrAllocator scratch = {.ctx = arena};
  vkr_allocator_arena(&scratch);
  for (const VkrBakeryJson *member = parts->first; member;
       member = member->next) {
    uint32_t index = VKR_EDITOR_ENVIRONMENT_PART_COUNT;
    for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
      const char *key = vkr_editor_environment_key((VkrEditorEnvironmentPart)i);
      if (member->key.length == strlen(key) &&
          MemCompare(member->key.str, key, member->key.length) == 0) {
        index = i;
      }
    }
    String8 values = {0};
    if (index == VKR_EDITOR_ENVIRONMENT_PART_COUNT ||
        !vkr_bakery_json_write(arena, member, VKR_BAKERY_JSON_COMPACT,
                               &values)) {
      snprintf(error, capacity,
               "'%.*s' is no part; parts are environment, atmosphere, clouds, "
               "fog, volumetric_fog, post_process, time_of_day, sun and moon",
               (int)member->key.length, (const char *)member->key.str);
      return false_v;
    }
    const VkrTypeDesc *type =
        vkr_editor_environment_type((VkrEditorEnvironmentPart)index);
    vkr_type_defaults(type, out->values[index]);
    char message[128] = {0};
    if (!vkr_type_read_json_document(values, type, out->values[index], &scratch,
                                     message, sizeof(message))) {
      snprintf(error, capacity, "%.*s: %s", (int)member->key.length,
               (const char *)member->key.str,
               message[0] ? message : "invalid values");
      return false_v;
    }
    out->parts |= 1u << index;
  }
  return true_v;
}

// =============================================================================
// Applying
// =============================================================================

uint32_t vkr_editor_environment_apply_items(
    const VkrSampleUiFrame *frame, uint16_t container,
    const VkrEditorEnvironmentPreset *preset, VkrSampleEditBatchItem *items,
    uint32_t capacity, uint32_t *skipped) {
  *skipped = 0u;
  VkrEditorEnvironmentSource sources[VKR_EDITOR_ENVIRONMENT_PART_COUNT];
  if (!vkr_editor_environment_resolve(frame, container, sources)) {
    *skipped = preset->parts;
    return 0u;
  }
  const VkrScene *scene = environment_scene(frame, container);
  uint32_t count = 0u;
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    if (!(preset->parts & (1u << i))) {
      continue;
    }
    const VkrEditorEnvironmentPart part = (VkrEditorEnvironmentPart)i;
    const VkrTypeDesc *type = vkr_editor_environment_type(part);
    const VkrEditorEnvironmentSource *source = &sources[i];
    const bool8_t own = source->entity.u64 && !source->from_world;
    /* The time of day is World-only; a sky light loads with its scene, so
       only an existing one takes values. */
    if ((part == VKR_EDITOR_ENVIRONMENT_TIME_OF_DAY &&
         container != VKR_SCENE_WORLD_ROOT_ID) ||
        (part == VKR_EDITOR_ENVIRONMENT_SKY_LIGHT && !own) ||
        count == capacity) {
      *skipped |= 1u << i;
      continue;
    }
    VkrSampleEditBatchItem *item = &items[count++];
    MemZero(item, sizeof(*item));
    item->entity_ref = -1;
    item->parent_ref = -1;
    VkrSceneEditRequest *request = &item->request;
    request->container = container;
    VkrSceneEditValues *values = &request->values;
    const uint32_t field = vkr_scene_edit_component_field(type);
    if (own) {
      request->action = VKR_SCENE_EDIT_APPLY;
      request->entity = source->entity;
      if (field) {
        (void)vkr_scene_edit_read(scene, source->entity, values);
      }
    } else {
      request->action = VKR_SCENE_EDIT_CREATE;
      vkr_scene_entity_ref_generate(&values->ref);
      values->fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
      snprintf(values->name, sizeof(values->name), "%s",
               vkr_editor_environment_label(part));
      values->rotation = vkr_quat_identity();
      values->scale = vec3_one();
    }
    if (field) {
      (void)vkr_scene_edit_component_set(values, type, preset->values[i]);
      values->fields = own ? field : values->fields | field;
    } else {
      values->component_type = type;
      MemCopy(values->component, preset->values[i], type->size);
      values->fields = own ? VKR_SCENE_EDIT_COMPONENT
                           : values->fields | VKR_SCENE_EDIT_COMPONENT;
    }
  }
  return count;
}

// =============================================================================
// Window
// =============================================================================

typedef struct EnvironmentListing {
  VkrEditorUi *editor;
} EnvironmentListing;

static bool8_t environment_ends_with(const char *text, const char *suffix) {
  const size_t length = strlen(text);
  const size_t suffix_length = strlen(suffix);
  return length > suffix_length &&
         strcmp(text + length - suffix_length, suffix) == 0;
}

static void environment_list_entry(void *context, const char *name,
                                   bool8_t directory) {
  VkrEditorUi *editor = ((EnvironmentListing *)context)->editor;
  if (directory ||
      !environment_ends_with(name, VKR_EDITOR_ENVIRONMENT_EXTENSION) ||
      editor->environment_preset_count >=
          ArrayCount(editor->environment_presets)) {
    return;
  }
  char *out = editor->environment_presets[editor->environment_preset_count];
  if (snprintf(out, sizeof(editor->environment_presets[0]), "%s/%s",
               VKR_EDITOR_ENVIRONMENT_DIRECTORY,
               name) < (int)sizeof(editor->environment_presets[0])) {
    editor->environment_preset_count++;
  }
}

/* The preset documents under assets/environments, read again every few
   seconds while the window shows. */
static void environment_list_presets(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (editor->environment_listed_at > 0.0 &&
      now - editor->environment_listed_at < 3.0) {
    return;
  }
  editor->environment_listed_at = now;
  editor->environment_preset_count = 0u;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  const FilePath root = vkr_asset_path_file(
      scratch, string8_lit(VKR_EDITOR_ENVIRONMENT_DIRECTORY));
  if (root.path.length && root.path.length < 1024u) {
    char absolute[1024];
    snprintf(absolute, sizeof(absolute), "%.*s", (int)root.path.length,
             (const char *)root.path.str);
    EnvironmentListing listing = {.editor = editor};
    vkr_editor_directory_list(absolute, environment_list_entry, &listing);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
}

/* Saves the resolved environment as `path`, through the document journal. */
static void environment_save(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             const VkrEditorEnvironmentSource *sources,
                             const char *path) {
  VkrEditorEnvironmentPreset *preset =
      vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*preset),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  String8 text = {0};
  char error[160] = {0};
  if (!preset) {
    return;
  }
  vkr_editor_environment_capture(sources, preset);
  if (!vkr_editor_environment_write(preset, frame->ui->frame_allocator,
                                    &text) ||
      !vkr_editor_material_write(editor->materials, frame, path, text, false_v,
                                 vkr_editor_material_group(), NULL, error,
                                 sizeof(error))) {
    snprintf(editor->environment_message, sizeof(editor->environment_message),
             "Not saved: %s", error[0] ? error : "out of memory");
    return;
  }
  snprintf(editor->environment_message, sizeof(editor->environment_message),
           "Saved %s", path);
  editor->environment_listed_at = 0.0;
}

/* Copies preset `path` into `container` as one batch of edits. */
static void environment_apply(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint16_t container,
                              const char *path) {
  if (!frame->edit_batch) {
    return;
  }
  Arena *arena = arena_create(MB(4), KB(64));
  VkrEditorEnvironmentPreset *preset =
      arena ? arena_alloc(arena, sizeof(*preset), ARENA_MEMORY_TAG_STRUCT)
            : NULL;
  VkrAllocator allocator = {.ctx = arena};
  String8 text = {0};
  char error[192] = {0};
  if (!preset) {
    snprintf(editor->environment_message, sizeof(editor->environment_message),
             "Out of memory");
  } else {
    vkr_allocator_arena(&allocator);
    if (!vkr_editor_material_read(&allocator, path, &text)) {
      snprintf(editor->environment_message, sizeof(editor->environment_message),
               "%s does not open", path);
    } else if (!vkr_editor_environment_read(text, arena, preset, error,
                                            sizeof(error))) {
      snprintf(editor->environment_message, sizeof(editor->environment_message),
               "%s", error);
    } else {
      uint32_t skipped = 0u;
      const uint32_t count = vkr_editor_environment_apply_items(
          frame, container, preset, editor->environment_items,
          ArrayCount(editor->environment_items), &skipped);
      if (count > 0u) {
        *frame->edit_batch = (VkrSampleEditBatchRequest){
            .token = ++editor->environment_batch_token,
            .items = editor->environment_items,
            .count = count,
            .container = container,
        };
      }
      snprintf(editor->environment_message, sizeof(editor->environment_message),
               "Applied %s: %u parts%s", path, count,
               skipped ? "; the sky light or time of day stays" : "");
    }
  }
  if (arena) {
    arena_destroy(arena);
  }
}

/* A new entity of `container` holding `value` of part `part`. */
static void environment_create(const VkrSampleUiFrame *frame,
                               uint16_t container,
                               VkrEditorEnvironmentPart part,
                               const void *value) {
  const VkrTypeDesc *type = vkr_editor_environment_type(part);
  VkrSceneEditValues values;
  MemZero(&values, sizeof(values));
  values.fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
  snprintf(values.name, sizeof(values.name), "%s",
           vkr_editor_environment_label(part));
  values.rotation = vkr_quat_identity();
  values.scale = vec3_one();
  const uint32_t field = vkr_scene_edit_component_field(type);
  if (field) {
    (void)vkr_scene_edit_component_set(&values, type, value);
    values.fields |= field;
  } else {
    values.component_type = type;
    MemCopy(values.component, value, type->size);
    values.fields |= VKR_SCENE_EDIT_COMPONENT;
  }
  *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_CREATE,
                                             .values = values,
                                             .container = container};
}

static float32_t environment_note(VkrUiSystem *ui, String8 id, float32_t y,
                                  float32_t width, String8 text, Vec4 color) {
  VkrUiWidgetConfig note = vkr_editor_details_widget(
      VKR_EDITOR_DETAILS_PAD_PT, y, width - VKR_EDITOR_DETAILS_PAD_PT * 2.0f,
      22.0f);
  note.style.font_size_pt = vkr_ui_theme()->font_caption;
  note.style.text_color = color;
  vkr_ui_label(ui, id, text, &note);
  return y + 22.0f;
}

static bool8_t environment_button(VkrEditorUi *editor, VkrUiSystem *ui,
                                  String8 id, String8 label, VkrUiIcon icon,
                                  float32_t x, float32_t y, float32_t width,
                                  String8 tooltip) {
  VkrUiWidgetConfig button = vkr_editor_details_widget(x, y, width, 24.0f);
  vkr_editor_action_style(&button, editor->heading_font);
  button.icon = icon;
  button.icon_size_pt = 13.0f;
  button.tooltip = tooltip;
  return vkr_ui_button(ui, id, label, &button);
}

void vkr_editor_environment_window_build(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / Max(ui->content_scale, 0.001f);
  const float32_t height = bounds.height / Max(ui->content_scale, 0.001f);
  if (width < 64.0f || height < 24.0f) {
    return;
  }
  /* The primary scene's environment, else the World's. */
  const uint16_t container = frame->scene ? 0u : VKR_SCENE_WORLD_ROOT_ID;
  VkrEditorEnvironmentSource *sources = vkr_allocator_alloc(
      ui->frame_allocator,
      sizeof(VkrEditorEnvironmentSource) * VKR_EDITOR_ENVIRONMENT_PART_COUNT,
      VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!sources || !vkr_editor_environment_resolve(frame, container, sources)) {
    (void)environment_note(ui, string8_lit("environment.none"), 8.0f, width,
                           string8_lit("Open a scene to edit its environment"),
                           theme->text_secondary);
    return;
  }
  environment_list_presets(editor, frame);

  if (ui->mouse_input_layer == ui->input_layer && !ui->mouse_captured &&
      ui->mouse_x >= bounds.x && ui->mouse_x < bounds.x + bounds.width &&
      ui->mouse_y >= bounds.y && ui->mouse_y < bounds.y + bounds.height) {
    editor->environment_scroll -= ui->mouse_wheel * 40.0f;
  }
  const float32_t content = Max(height, editor->environment_height);
  editor->environment_scroll =
      Clamp(editor->environment_scroll, 0.0f, Max(0.0f, content - height));
  const VkrUiTrack content_track = {.value = content, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig area = vkr_ui_panel_config_default();
  area.placement.column = 0u;
  area.placement.row = 0u;
  area.rows = &content_track;
  area.row_count = 1u;
  area.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("environment.scroll"), &area)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &editor->environment_scroll);

  float32_t y = 6.0f;
  y = environment_note(
      ui, string8_lit("environment.scope"), y, width,
      container == 0u
          ? string8_lit("The scene's environment. World values apply until "
                        "the scene sets its own.")
          : string8_lit("The World's environment, shared by every scene "
                        "that inherits it."),
      theme->text_secondary);

  /* Presets: save this environment, or copy one in. */
  const float32_t half = (width - VKR_EDITOR_DETAILS_PAD_PT * 3.0f) * 0.5f;
  if (environment_button(
          editor, ui, string8_lit("environment.save"),
          string8_lit("Save preset"), VKR_UI_ICON_SAVE,
          VKR_EDITOR_DETAILS_PAD_PT, y, half,
          string8_lit("Save every part shown here as an environment "
                      "preset named after the scene"))) {
    const String8 scene_path = frame->scene_path;
    const char *stem = "world";
    char name[96];
    if (container == 0u && scene_path.length) {
      uint64_t start = 0u;
      for (uint64_t i = 0; i < scene_path.length; ++i) {
        if (scene_path.str[i] == '/' || scene_path.str[i] == '\\') {
          start = i + 1u;
        }
      }
      uint64_t end = scene_path.length;
      for (uint64_t i = start; i < scene_path.length; ++i) {
        if (scene_path.str[i] == '.') {
          end = i;
          break;
        }
      }
      snprintf(name, sizeof(name), "%.*s", (int)(end - start),
               (const char *)scene_path.str + start);
      stem = name;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s%s", VKR_EDITOR_ENVIRONMENT_DIRECTORY,
             stem, VKR_EDITOR_ENVIRONMENT_EXTENSION);
    environment_save(editor, frame, sources, path);
  }
  y += 30.0f;
  for (uint32_t i = 0; i < editor->environment_preset_count; ++i) {
    (void)vkr_ui_push_id_u64(ui, 0x5e7e0000u + i);
    const char *path = editor->environment_presets[i];
    VkrUiWidgetConfig label = vkr_editor_details_widget(
        VKR_EDITOR_DETAILS_PAD_PT, y, width - 110.0f, 24.0f);
    label.icon = VKR_UI_ICON_PALETTE;
    label.icon_size_pt = 12.0f;
    vkr_ui_label(ui, string8_lit("preset"),
                 string8_create_from_cstr((const uint8_t *)path, strlen(path)),
                 &label);
    if (environment_button(editor, ui, string8_lit("apply"),
                           string8_lit("Apply"), VKR_UI_ICON_CHECK,
                           width - 96.0f, y, 86.0f,
                           string8_lit("Copy this preset's values into the "
                                       "scene (undoable)"))) {
      environment_apply(editor, frame, container, path);
    }
    (void)vkr_ui_pop_id(ui);
    y += 28.0f;
  }
  if (editor->environment_message[0]) {
    y = environment_note(
        ui, string8_lit("environment.message"), y, width,
        string8_create_from_cstr((const uint8_t *)editor->environment_message,
                                 strlen(editor->environment_message)),
        theme->text_secondary);
  }
  y += 4.0f;

  VkrEditorDetails *details = &editor->environment_details;
  vkr_editor_details_begin(details);
  vkr_editor_details_error(details, ui, width, &y);
  for (uint32_t i = 0; i < VKR_EDITOR_ENVIRONMENT_PART_COUNT; ++i) {
    const VkrEditorEnvironmentPart part = (VkrEditorEnvironmentPart)i;
    const VkrTypeDesc *type = vkr_editor_environment_type(part);
    VkrEditorEnvironmentSource *source = &sources[i];
    (void)vkr_ui_push_id_u64(ui, 0x5e7f0000u + i);
    const char *from = !source->entity.u64                    ? "not set"
                       : source->from_world                   ? "World"
                       : container == VKR_SCENE_WORLD_ROOT_ID ? "World"
                                                              : "Scene";
    const String8 title =
        string8_create_formatted(ui->frame_allocator, "%s \xc2\xb7 %s",
                                 vkr_editor_environment_label(part), from);
    const bool8_t expanded = vkr_editor_details_section(
        ui, string8_lit("part"), width, &y,
        environment_light_part(part) ? VKR_UI_ICON_DIRECTIONAL_LIGHT
                                     : vkr_editor_world_type_icon(type),
        source->entity.u64 ? theme->accent : theme->text_secondary, title,
        editor->heading_font, &editor->environment_collapsed[i]);
    if (expanded) {
      const bool8_t world_only = part == VKR_EDITOR_ENVIRONMENT_TIME_OF_DAY &&
                                 container != VKR_SCENE_WORLD_ROOT_ID;
      if (!source->entity.u64) {
        if (part == VKR_EDITOR_ENVIRONMENT_SKY_LIGHT) {
          y = environment_note(ui, string8_lit("sky"), y, width,
                               string8_lit("A sky light loads with its scene; "
                                           "add it in the scene document"),
                               theme->text_secondary);
        } else if (world_only) {
          y = environment_note(ui, string8_lit("world_only"), y, width,
                               string8_lit("The time of day belongs to the "
                                           "World"),
                               theme->text_secondary);
        } else if (environment_button(editor, ui, string8_lit("add"),
                                      string8_lit("Add"), VKR_UI_ICON_ADD,
                                      VKR_EDITOR_DETAILS_PAD_PT, y, 120.0f,
                                      string8_lit("Add this part with its "
                                                  "defaults"))) {
          environment_create(frame, container, part, source->value);
        }
        y += source->entity.u64 ? 0.0f : 30.0f;
      } else {
        if (source->from_world) {
          y = environment_note(ui, string8_lit("shared"), y, width,
                               string8_lit("From the World: edits here change "
                                           "every scene that inherits it"),
                               theme->warning);
          if (!world_only &&
              environment_button(editor, ui, string8_lit("override"),
                                 string8_lit("Override in scene"),
                                 VKR_UI_ICON_COPY, VKR_EDITOR_DETAILS_PAD_PT, y,
                                 170.0f,
                                 string8_lit("Copy the World's values into "
                                             "this scene"))) {
            environment_create(frame, container, part, source->value);
          }
          y += world_only ? 0.0f : 30.0f;
        }
        const VkrEditorDetailsResult result =
            vkr_editor_details_type(details, ui, frame->input, width, &y, type,
                                    source->value, NULL, false_v);
        if (result.changed && frame->scene_edit) {
          VkrSceneEditValues values;
          MemZero(&values, sizeof(values));
          const uint32_t field = vkr_scene_edit_component_field(type);
          if (field) {
            (void)vkr_scene_edit_read(source->owner, source->entity, &values);
            (void)vkr_scene_edit_component_set(&values, type, source->value);
            values.fields = field;
          } else {
            values.fields = VKR_SCENE_EDIT_COMPONENT;
            values.component_type = type;
            MemCopy(values.component, source->value, type->size);
          }
          *frame->scene_edit =
              (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                    .entity = source->entity,
                                    .values = values,
                                    .gesture = result.gesture};
        }
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  vkr_editor_details_end(details);
  vkr_editor_context_open_choice(editor, details);
  vkr_editor_color_picker_open(editor, details);
  editor->environment_height = y + 24.0f;
  (void)vkr_ui_scroll_area_end(ui);
}
