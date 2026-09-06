#include "vkr_scene_edit.h"

#include "core/logger.h"
#include "core/vkr_json_writer.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDIT_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

static bool8_t finite_vector(const float32_t *v, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    if (!isfinite(v[i]))
      return false_v;
  return true_v;
}

bool8_t vkr_scene_edit_read(const VkrScene *scene, VkrEntityId entity,
                            VkrSceneEditValues *out) {
  MemZero(out, sizeof(*out));
  if (!scene || !vkr_scene_entity_alive(scene, entity))
    return false_v;
  String8 name = vkr_scene_get_name(scene, entity);
  if (vkr_entity_get_component(scene->world, entity, scene->comp_name) &&
      name.length < sizeof(out->name)) {
    MemCopy(out->name, name.str, name.length);
    out->fields |= VKR_SCENE_EDIT_NAME;
  }
  const SceneTransform *tr =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (tr && tr->trs_editable) {
    out->fields |= VKR_SCENE_EDIT_TRANSFORM;
    out->position = tr->position;
    out->rotation = tr->rotation;
    out->scale = tr->scale;
  }
  const SceneVisibility *vis =
      vkr_entity_get_component(scene->world, entity, scene->comp_visibility);
  if (vis) {
    out->fields |= VKR_SCENE_EDIT_VISIBILITY;
    out->visibility = *vis;
  }
  const ScenePointLight *point =
      vkr_entity_get_component(scene->world, entity, scene->comp_point_light);
  if (point) {
    out->fields |= VKR_SCENE_EDIT_POINT_LIGHT;
    out->point_light = *point;
  }
  const SceneDirectionalLight *directional = vkr_entity_get_component(
      scene->world, entity, scene->comp_directional_light);
  if (directional) {
    out->fields |= VKR_SCENE_EDIT_DIRECTIONAL_LIGHT;
    out->directional_light = *directional;
  }
  return true_v;
}

bool8_t vkr_scene_edit_validate(const VkrSceneEditValues *v) {
  if (!v->fields || (v->fields & ~31u))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_NAME) && !memchr(v->name, 0, sizeof(v->name)))
    return false_v;
  if (v->fields & VKR_SCENE_EDIT_TRANSFORM) {
    if (!finite_vector(&v->position.x, 3) ||
        !finite_vector(&v->rotation.x, 4) || !finite_vector(&v->scale.x, 3) ||
        fabsf(v->scale.x) < 0.000001f || fabsf(v->scale.y) < 0.000001f ||
        fabsf(v->scale.z) < 0.000001f ||
        !isfinite(vec4_dot(v->rotation, v->rotation)) ||
        vec4_dot(v->rotation, v->rotation) < 0.000001f)
      return false_v;
  }
  if ((v->fields & VKR_SCENE_EDIT_VISIBILITY) &&
      (v->visibility.visible > 1 || v->visibility.inherit_parent > 1))
    return false_v;
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT) {
    const ScenePointLight *p = &v->point_light;
    if (!finite_vector(&p->color.x, 3) || p->color.x < 0 || p->color.y < 0 ||
        p->color.z < 0 || !isfinite(p->intensity) || p->intensity < 0 ||
        !isfinite(p->range) || p->range < 0 || !isfinite(p->constant) ||
        !isfinite(p->linear) || !isfinite(p->quadratic) || p->constant < 0 ||
        p->linear < 0 || p->quadratic < 0 ||
        !finite_vector(&p->direction_local.x, 3) ||
        !isfinite(p->inner_cone_angle) || !isfinite(p->outer_cone_angle) ||
        p->inner_cone_angle < 0 || p->outer_cone_angle < p->inner_cone_angle ||
        p->outer_cone_angle > 1.5707964f || p->enabled > 1 ||
        (uint32_t)p->kind > VKR_POINT_LIGHT_KIND_GLTF_SPOT ||
        (p->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT &&
         (!isfinite(vec3_dot(p->direction_local, p->direction_local)) ||
          vec3_dot(p->direction_local, p->direction_local) < 0.000001f)))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    const SceneDirectionalLight *p = &v->directional_light;
    if (!finite_vector(&p->color.x, 3) || p->color.x < 0 || p->color.y < 0 ||
        p->color.z < 0 || !isfinite(p->intensity) || p->intensity < 0 ||
        !finite_vector(&p->direction_local.x, 3) ||
        !isfinite(vec3_dot(p->direction_local, p->direction_local)) ||
        p->enabled > 1 ||
        vec3_dot(p->direction_local, p->direction_local) < 0.000001f)
      return false_v;
  }
  return true_v;
}

void vkr_scene_edit_reset(VkrSceneEditState *s, VkrAllocator *allocator,
                          uint64_t generation) {
  if (s->undo)
    vkr_allocator_free(s->allocator, s->undo,
                       sizeof(*s->undo) * VKR_SCENE_EDIT_UNDO_CAPACITY,
                       EDIT_TAG);
  if (s->touched)
    vkr_allocator_free(s->allocator, s->touched,
                       sizeof(*s->touched) * s->touched_capacity, EDIT_TAG);
  *s = (VkrSceneEditState){.allocator = allocator, .generation = generation};
}

static bool8_t edit_touch(VkrSceneEditState *s, VkrEntityId entity) {
  for (uint32_t i = 0; i < s->touched_count; ++i)
    if (s->touched[i].u64 == entity.u64)
      return true_v;
  if (s->touched_count == s->touched_capacity) {
    uint32_t capacity = Max(32u, s->touched_capacity * 2u);
    VkrEntityId *next = vkr_allocator_realloc(
        s->allocator, s->touched, s->touched_capacity * sizeof(*next),
        capacity * sizeof(*next), EDIT_TAG);
    if (!next)
      return false_v;
    s->touched = next;
    s->touched_capacity = capacity;
  }
  s->touched[s->touched_count++] = entity;
  return true_v;
}

/* Preparation owns a replacement in the scene allocator. Components and all
   values are proven before commit; the scalar commit cannot allocate or fail.
 */
typedef struct EditPrepared {
  VkrEntityId entity;
  VkrSceneEditValues values;
  String8 replacement_name;
  bool8_t add_touched;
} EditPrepared;

static void edit_discard(VkrScene *scene, EditPrepared *p) {
  if (p->replacement_name.str)
    vkr_allocator_free(scene->alloc, p->replacement_name.str,
                       p->replacement_name.length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  p->replacement_name = (String8){0};
}

static bool8_t edit_prepare(VkrScene *scene, VkrEntityId entity,
                            const VkrSceneEditValues *v, EditPrepared *p) {
  *p = (EditPrepared){.entity = entity, .values = *v};
  VkrSceneEditValues current;
  if (!vkr_scene_edit_read(scene, entity, &current) ||
      (v->fields & ~current.fields) || !vkr_scene_edit_validate(v))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_NAME) && strcmp(current.name, v->name)) {
    uint64_t length = strlen(v->name);
    uint8_t *name = vkr_allocator_alloc(scene->alloc, length + 1u,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!name)
      return false_v;
    MemCopy(name, v->name, length + 1u);
    p->replacement_name = string8_create(name, length);
  }
  return true_v;
}

static void edit_commit(VkrScene *scene, EditPrepared *p) {
  const VkrSceneEditValues *v = &p->values;
  VkrEntityId entity = p->entity;
  if (p->replacement_name.str) {
    SceneName *name =
        vkr_entity_get_component_mut(scene->world, entity, scene->comp_name);
    String8 old = name->name;
    name->name = p->replacement_name;
    p->replacement_name = (String8){0};
    if (old.str)
      vkr_allocator_free(scene->alloc, old.str, old.length + 1u,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    scene->structure_revision++;
  }
  if (v->fields & VKR_SCENE_EDIT_TRANSFORM) {
    vkr_scene_set_position(scene, entity, v->position);
    vkr_scene_set_rotation(scene, entity, vkr_quat_normalize(v->rotation));
    vkr_scene_set_scale(scene, entity, v->scale);
  }
  if (v->fields & VKR_SCENE_EDIT_VISIBILITY)
    vkr_scene_set_visibility(scene, entity, v->visibility.visible,
                             v->visibility.inherit_parent);
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    *vkr_scene_get_point_light(scene, entity) = v->point_light;
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    *vkr_scene_get_directional_light(scene, entity) = v->directional_light;
}

static bool8_t edit_write(VkrScene *scene, VkrEntityId entity,
                          const VkrSceneEditValues *v) {
  EditPrepared prepared;
  if (!edit_prepare(scene, entity, v, &prepared))
    return false_v;
  edit_commit(scene, &prepared);
  return true_v;
}

static bool8_t edit_journal_prepare(VkrSceneEditState *s, VkrEntityId entity) {
  if (!s->undo) {
    s->undo = vkr_allocator_alloc(
        s->allocator, sizeof(*s->undo) * VKR_SCENE_EDIT_UNDO_CAPACITY,
        EDIT_TAG);
    if (!s->undo)
      return false_v;
  }
  return edit_touch(s, entity);
}

static void edit_journal_append(VkrSceneEditState *s, VkrEntityId entity,
                                VkrSceneEditValues before,
                                const VkrSceneEditValues *after) {
  before.fields = after->fields;
  s->undo_count = s->undo_cursor;
  if (s->undo_count == VKR_SCENE_EDIT_UNDO_CAPACITY) {
    memmove(s->undo, s->undo + 1,
            (VKR_SCENE_EDIT_UNDO_CAPACITY - 1u) * sizeof(*s->undo));
    s->undo_count--;
  }
  s->undo[s->undo_count++] = (VkrSceneEditEntry){entity, before, *after};
  s->undo_cursor = s->undo_count;
  s->revision++;
  snprintf(s->status, sizeof(s->status),
           "Edited. Save writes scene overrides.");
}

bool8_t vkr_scene_edit_apply(VkrSceneEditState *s, VkrScene *scene,
                             VkrEntityId entity, const VkrSceneEditValues *v) {
  VkrSceneEditValues before;
  EditPrepared prepared;
  if (!vkr_scene_edit_read(scene, entity, &before) ||
      !edit_prepare(scene, entity, v, &prepared)) {
    snprintf(s->status, sizeof(s->status),
             "Invalid values or stale selection.");
    return false_v;
  }
  if (!edit_journal_prepare(s, entity)) {
    edit_discard(scene, &prepared);
    return false_v;
  }
  edit_commit(scene, &prepared);
  edit_journal_append(s, entity, before, v);
  return true_v;
}

bool8_t vkr_scene_edit_record_external(VkrSceneEditState *s, VkrScene *scene,
                                       VkrEntityId entity,
                                       const VkrSceneEditValues *before,
                                       const VkrSceneEditValues *after) {
  VkrSceneEditValues current;
  if (before->fields != after->fields || !vkr_scene_edit_validate(before) ||
      !vkr_scene_edit_validate(after) ||
      !vkr_scene_edit_read(scene, entity, &current) ||
      (after->fields & ~current.fields) || !edit_journal_prepare(s, entity))
    return false_v;
  edit_journal_append(s, entity, *before, after);
  return true_v;
}

bool8_t vkr_scene_edit_undo(VkrSceneEditState *s, VkrScene *scene,
                            bool8_t redo) {
  if (redo ? s->undo_cursor == s->undo_count : s->undo_cursor == 0u)
    return false_v;
  VkrSceneEditEntry *entry =
      &s->undo[redo ? s->undo_cursor : s->undo_cursor - 1u];
  if (!edit_write(scene, entry->entity, redo ? &entry->after : &entry->before))
    return false_v;
  if (redo)
    s->undo_cursor++;
  else
    s->undo_cursor--;
  s->revision++;
  snprintf(s->status, sizeof(s->status), "%s", redo ? "Redone." : "Undone.");
  return true_v;
}

static bool8_t json_floats(VkrJsonWriter *w, const char *name,
                           const float32_t *values, uint32_t count) {
  if (!vkr_json_writer_name(w, string8_create((uint8_t *)name, strlen(name))) ||
      !vkr_json_writer_begin_array(w))
    return false_v;
  for (uint32_t i = 0; i < count; ++i)
    if (!vkr_json_writer_f64(w, values[i]))
      return false_v;
  return vkr_json_writer_end_array(w);
}

static bool8_t write_values(VkrJsonWriter *w, const VkrSceneEditValues *v) {
#define WRITE_INT(key, value)                                                  \
  (vkr_json_writer_name(w, string8_lit(key)) && vkr_json_writer_i64(w, (value)))
#define WRITE_BOOL(key, value)                                                 \
  (vkr_json_writer_name(w, string8_lit(key)) &&                                \
   vkr_json_writer_bool(w, (value)))
  if (!WRITE_INT("fields", v->fields))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_NAME) &&
      !(vkr_json_writer_name(w, string8_lit("name")) &&
        vkr_json_writer_string(
            w, string8_create((uint8_t *)v->name, strlen(v->name)))))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_TRANSFORM) &&
      !(json_floats(w, "position", &v->position.x, 3) &&
        json_floats(w, "rotation", &v->rotation.x, 4) &&
        json_floats(w, "scale", &v->scale.x, 3)))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_VISIBILITY) &&
      !(WRITE_BOOL("visible", v->visibility.visible) &&
        WRITE_BOOL("inherit", v->visibility.inherit_parent)))
    return false_v;
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT) {
    const ScenePointLight *p = &v->point_light;
    float32_t params[] = {p->intensity,       p->constant, p->linear,
                          p->quadratic,       p->range,    p->inner_cone_angle,
                          p->outer_cone_angle};
    if (!json_floats(w, "point_color", &p->color.x, 3) ||
        !json_floats(w, "point_direction", &p->direction_local.x, 3) ||
        !json_floats(w, "point_params", params, 7) ||
        !WRITE_INT("point_kind", p->kind) ||
        !WRITE_BOOL("point_enabled", p->enabled))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    const SceneDirectionalLight *p = &v->directional_light;
    if (!json_floats(w, "directional_color", &p->color.x, 3) ||
        !json_floats(w, "directional_direction", &p->direction_local.x, 3) ||
        !json_floats(w, "directional_intensity", &p->intensity, 1) ||
        !WRITE_BOOL("directional_enabled", p->enabled))
      return false_v;
  }
  return true_v;
}

static int32_t hex_digit(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool8_t vkr_scene_edit_save(VkrSceneEditState *s, const VkrScene *scene,
                            String8 path) {
  if (s->sidecar_conflict) {
    snprintf(
        s->status, sizeof(s->status),
        "Save blocked: resolve or move the conflicting sidecar, then reload.");
    return false_v;
  }
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, path))
    goto failed;
  VkrJsonWriter *w = &file.writer;
  if (!vkr_json_writer_begin_object(w) || !WRITE_INT("version", 1) ||
      !vkr_json_writer_name(w, string8_lit("overrides")) ||
      !vkr_json_writer_begin_array(w))
    goto failed;
  for (uint32_t i = 0; i < s->touched_count; i++) {
    VkrEntityId entity = s->touched[i];
    VkrSceneEditValues values;
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, entity, scene->comp_source_identity);
    if (!source || !vkr_scene_edit_read(scene, entity, &values))
      goto failed;
    char hash[17];
    snprintf(hash, sizeof(hash), "%016llx",
             (unsigned long long)source->source_fingerprint);
    if (!vkr_json_writer_begin_object(w) ||
        !WRITE_INT("scene_entity", source->scene_entity_index) ||
        !WRITE_INT("gltf_node", (int32_t)source->gltf_node_index) ||
        !vkr_json_writer_name(w, string8_lit("source_fingerprint")) ||
        !vkr_json_writer_string(w, string8_create((uint8_t *)hash, 16)) ||
        !write_values(w, &values) || !vkr_json_writer_end_object(w))
      goto failed;
  }
  if (!vkr_json_writer_end_array(w) || !vkr_json_writer_end_object(w) ||
      !vkr_json_file_writer_commit(&file))
    goto failed;
  s->saved_revision = s->revision;
  snprintf(s->status, sizeof(s->status), "Saved %u node overrides.",
           s->touched_count);
  log_info("Saved editor overrides to %.*s", (int)path.length, path.str);
  return true_v;
failed:
  vkr_json_file_writer_abort(&file);
  snprintf(s->status, sizeof(s->status),
           "Save failed: file or source identity unavailable.");
  log_error("Editor override save failed: %.*s", (int)path.length, path.str);
  return false_v;
}

/* This schema intentionally accepts only flat override records. The generic
   field-search reader is unsuitable here: nesting, duplicates and EOF must
   never turn a partial sidecar into a valid mutation batch. */
typedef struct EditJson {
  const uint8_t *at;
  const uint8_t *end;
} EditJson;

static void edit_json_space(EditJson *j) {
  while (j->at < j->end &&
         (*j->at == ' ' || *j->at == '\t' || *j->at == '\r' || *j->at == '\n'))
    j->at++;
}

static bool8_t edit_json_take(EditJson *j, uint8_t c) {
  edit_json_space(j);
  if (j->at == j->end || *j->at != c)
    return false_v;
  j->at++;
  return true_v;
}

static bool8_t edit_json_hex4(EditJson *j, uint32_t *out) {
  if (j->end - j->at < 4)
    return false_v;
  *out = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t digit = hex_digit(*j->at++);
    if (digit < 0)
      return false_v;
    *out = (*out << 4u) | (uint32_t)digit;
  }
  return true_v;
}

static bool8_t edit_json_string(EditJson *j, char *out, uint32_t capacity) {
  if (!edit_json_take(j, '"'))
    return false_v;
  uint32_t count = 0;
  while (j->at < j->end) {
    uint32_t c = *j->at++;
    if (c == '"') {
      out[count] = 0;
      return true_v;
    }
    if (c < 32)
      return false_v;
    if (c == '\\') {
      if (j->at == j->end)
        return false_v;
      c = *j->at++;
      switch (c) {
      case '"':
      case '\\':
      case '/':
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'u': {
        if (!edit_json_hex4(j, &c))
          return false_v;
        if (c >= 0xd800 && c <= 0xdbff) {
          uint32_t low;
          if (j->end - j->at < 2 || *j->at++ != '\\' || *j->at++ != 'u' ||
              !edit_json_hex4(j, &low) || low < 0xdc00 || low > 0xdfff)
            return false_v;
          c = 0x10000u + ((c - 0xd800u) << 10u) + low - 0xdc00u;
        } else if (c >= 0xdc00 && c <= 0xdfff)
          return false_v;
        break;
      }
      default:
        return false_v;
      }
    } else if (c >= 128) {
      uint32_t remaining, minimum;
      if (c >= 0xc2 && c <= 0xdf) {
        remaining = 1;
        minimum = 0x80;
        c &= 31;
      } else if (c >= 0xe0 && c <= 0xef) {
        remaining = 2;
        minimum = 0x800;
        c &= 15;
      } else if (c >= 0xf0 && c <= 0xf4) {
        remaining = 3;
        minimum = 0x10000;
        c &= 7;
      } else
        return false_v;
      if (j->end - j->at < remaining)
        return false_v;
      for (uint32_t i = 0; i < remaining; ++i) {
        uint32_t next = *j->at++;
        if ((next & 0xc0) != 0x80)
          return false_v;
        c = (c << 6u) | (next & 63u);
      }
      if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return false_v;
    }
    uint32_t size = c < 0x80 ? 1u : c < 0x800 ? 2u : c < 0x10000 ? 3u : 4u;
    if (c == 0 || count + size >= capacity)
      return false_v;
    if (size == 1)
      out[count++] = (char)c;
    else {
      out[count++] = (char)((size == 2   ? 0xc0
                             : size == 3 ? 0xe0
                                         : 0xf0) |
                            (c >> (6u * (size - 1u))));
      for (uint32_t n = size - 1u; n > 0; --n)
        out[count++] = (char)(0x80u | ((c >> (6u * (n - 1u))) & 63u));
    }
  }
  return false_v;
}

static bool8_t edit_json_number(EditJson *j, float64_t *out, bool8_t integer) {
  edit_json_space(j);
  const uint8_t *start = j->at;
  if (j->at < j->end && *j->at == '-')
    j->at++;
  if (j->at == j->end)
    return false_v;
  if (*j->at == '0')
    j->at++;
  else {
    if (*j->at < '1' || *j->at > '9')
      return false_v;
    while (j->at < j->end && *j->at >= '0' && *j->at <= '9')
      j->at++;
  }
  if (!integer && j->at < j->end && *j->at == '.') {
    j->at++;
    const uint8_t *digits = j->at;
    while (j->at < j->end && *j->at >= '0' && *j->at <= '9')
      j->at++;
    if (j->at == digits)
      return false_v;
  }
  if (!integer && j->at < j->end && (*j->at == 'e' || *j->at == 'E')) {
    j->at++;
    if (j->at < j->end && (*j->at == '+' || *j->at == '-'))
      j->at++;
    const uint8_t *digits = j->at;
    while (j->at < j->end && *j->at >= '0' && *j->at <= '9')
      j->at++;
    if (j->at == digits)
      return false_v;
  }
  char number[96];
  size_t length = (size_t)(j->at - start);
  if (length >= sizeof(number))
    return false_v;
  MemCopy(number, start, length);
  number[length] = 0;
  char *end;
  *out = strtod(number, &end);
  return end == number + length && isfinite(*out);
}

static bool8_t edit_json_int(EditJson *j, int64_t min, int64_t max,
                             int64_t *out) {
  float64_t value;
  if (!edit_json_number(j, &value, true_v) || value < (float64_t)min ||
      value > (float64_t)max)
    return false_v;
  *out = (int64_t)value;
  return true_v;
}

static bool8_t edit_json_bool(EditJson *j, bool8_t *out) {
  edit_json_space(j);
  if (j->end - j->at >= 4 && MemCompare(j->at, "true", 4) == 0) {
    j->at += 4;
    *out = true_v;
    return true_v;
  }
  if (j->end - j->at >= 5 && MemCompare(j->at, "false", 5) == 0) {
    j->at += 5;
    *out = false_v;
    return true_v;
  }
  return false_v;
}

static bool8_t edit_json_floats(EditJson *j, float32_t *out, uint32_t count) {
  if (!edit_json_take(j, '['))
    return false_v;
  for (uint32_t i = 0; i < count; ++i) {
    float64_t value;
    if ((i && !edit_json_take(j, ',')) || !edit_json_number(j, &value, false_v))
      return false_v;
    out[i] = (float32_t)value;
    if (!isfinite(out[i]))
      return false_v;
  }
  return edit_json_take(j, ']');
}

static bool8_t edit_json_record(EditJson *j, VkrSceneEditValues *v,
                                uint32_t *wrapper, uint32_t *node,
                                uint64_t *fingerprint) {
  static const char *keys[] = {"scene_entity",
                               "gltf_node",
                               "source_fingerprint",
                               "fields",
                               "name",
                               "position",
                               "rotation",
                               "scale",
                               "visible",
                               "inherit",
                               "point_color",
                               "point_direction",
                               "point_params",
                               "point_kind",
                               "point_enabled",
                               "directional_color",
                               "directional_direction",
                               "directional_intensity",
                               "directional_enabled"};
  MemZero(v, sizeof(*v));
  uint32_t seen = 0;
  if (!edit_json_take(j, '{'))
    return false_v;
  for (;;) {
    char key[32];
    int64_t integer = 0;
    bool8_t ok = false_v;
    if (!edit_json_string(j, key, sizeof(key)) || !edit_json_take(j, ':'))
      return false_v;
    uint32_t k = 0;
    while (k < sizeof(keys) / sizeof(*keys) && strcmp(keys[k], key))
      k++;
    if (k == sizeof(keys) / sizeof(*keys) || (seen & (1u << k)))
      return false_v;
    seen |= 1u << k;
    switch (k) {
    case 0:
      ok = edit_json_int(j, 0, UINT32_MAX, &integer);
      *wrapper = (uint32_t)integer;
      break;
    case 1:
      ok = edit_json_int(j, -1, INT32_MAX, &integer);
      *node = (uint32_t)integer;
      break;
    case 2: {
      char hash[17];
      if (!edit_json_string(j, hash, sizeof(hash)) || strlen(hash) != 16)
        return false_v;
      *fingerprint = 0;
      for (uint32_t i = 0; i < 16; ++i) {
        int32_t digit = hex_digit((uint8_t)hash[i]);
        if (digit < 0)
          return false_v;
        *fingerprint = (*fingerprint << 4u) | (uint32_t)digit;
      }
      ok = true_v;
      break;
    }
    case 3:
      ok = edit_json_int(j, 1, 31, &integer);
      v->fields = (uint32_t)integer;
      break;
    case 4:
      ok = edit_json_string(j, v->name, sizeof(v->name));
      break;
    case 5:
      ok = edit_json_floats(j, &v->position.x, 3);
      break;
    case 6:
      ok = edit_json_floats(j, &v->rotation.x, 4);
      break;
    case 7:
      ok = edit_json_floats(j, &v->scale.x, 3);
      break;
    case 8:
      ok = edit_json_bool(j, &v->visibility.visible);
      break;
    case 9:
      ok = edit_json_bool(j, &v->visibility.inherit_parent);
      break;
    case 10:
      ok = edit_json_floats(j, &v->point_light.color.x, 3);
      break;
    case 11:
      ok = edit_json_floats(j, &v->point_light.direction_local.x, 3);
      break;
    case 12: {
      float32_t params[7];
      if (!edit_json_floats(j, params, 7))
        return false_v;
      ScenePointLight *p = &v->point_light;
      p->intensity = params[0];
      p->constant = params[1];
      p->linear = params[2];
      p->quadratic = params[3];
      p->range = params[4];
      p->inner_cone_angle = params[5];
      p->outer_cone_angle = params[6];
      ok = true_v;
      break;
    }
    case 13:
      ok = edit_json_int(j, 0, 2, &integer);
      v->point_light.kind = (VkrPointLightKind)integer;
      break;
    case 14:
      ok = edit_json_bool(j, &v->point_light.enabled);
      break;
    case 15:
      ok = edit_json_floats(j, &v->directional_light.color.x, 3);
      break;
    case 16:
      ok = edit_json_floats(j, &v->directional_light.direction_local.x, 3);
      break;
    case 17:
      ok = edit_json_floats(j, &v->directional_light.intensity, 1);
      break;
    case 18:
      ok = edit_json_bool(j, &v->directional_light.enabled);
      break;
    }
    if (!ok)
      return false_v;
    if (edit_json_take(j, '}'))
      break;
    if (!edit_json_take(j, ','))
      return false_v;
  }
  uint32_t required = 15u;
  if (v->fields & VKR_SCENE_EDIT_NAME)
    required |= 1u << 4u;
  if (v->fields & VKR_SCENE_EDIT_TRANSFORM)
    required |= 7u << 5u;
  if (v->fields & VKR_SCENE_EDIT_VISIBILITY)
    required |= 3u << 8u;
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    required |= 31u << 10u;
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    required |= 15u << 15u;
  return seen == required && vkr_scene_edit_validate(v);
}

typedef struct EditSourceIndex {
  uint32_t wrapper;
  uint32_t node;
  VkrEntityId entity;
  uint64_t fingerprint;
  bool8_t ambiguous;
  bool8_t seen;
  bool8_t touched;
} EditSourceIndex;

static int edit_source_compare(const void *left, const void *right) {
  const EditSourceIndex *a = left, *b = right;
  if (a->wrapper != b->wrapper)
    return a->wrapper < b->wrapper ? -1 : 1;
  if (a->node != b->node)
    return a->node < b->node ? -1 : 1;
  return 0;
}

static EditSourceIndex *edit_source_find(EditSourceIndex *index, uint32_t count,
                                         uint32_t wrapper, uint32_t node) {
  uint32_t begin = 0, end = count;
  while (begin < end) {
    uint32_t middle = begin + (end - begin) / 2u;
    EditSourceIndex *entry = &index[middle];
    if (entry->wrapper < wrapper ||
        (entry->wrapper == wrapper && entry->node < node))
      begin = middle + 1u;
    else
      end = middle;
  }
  if (begin == count || index[begin].wrapper != wrapper ||
      index[begin].node != node)
    return NULL;
  return &index[begin];
}

/* All prepared strings belong to the scene; pending records/file bytes belong
   to the editor allocator and die at this input boundary. No scene mutation
   occurs until closing delimiters, EOF, identities and every allocation pass.
 */
bool8_t vkr_scene_edit_load(VkrSceneEditState *s, VkrScene *scene,
                            String8 path) {
  char cpath[1024];
  FILE *file = NULL;
  uint8_t *bytes = NULL;
  EditPrepared *pending = NULL;
  uint32_t count = 0, capacity = 0;
  EditSourceIndex *index = NULL;
  uint32_t index_count = 0, additional_touched = 0;
  uint32_t index_capacity = scene->world->dir.capacity;
  uint32_t touched_before = s->touched_count;
  bool8_t success = false_v;
  long length = 0;
  if (path.length >= sizeof(cpath))
    goto cleanup;
  MemCopy(cpath, path.str, path.length);
  cpath[path.length] = 0;
  file = fopen(cpath, "rb");
  if (!file && errno == ENOENT) {
    s->sidecar_conflict = false_v;
    return true_v;
  }
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
    goto cleanup;
  bytes = vkr_allocator_alloc(s->allocator, (uint64_t)length, EDIT_TAG);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length)
    goto cleanup;
  /* One directory walk and sort serve all records. Seen flags reject repeated
     overrides; touched flags merge an existing journal without quadratic scans.
   */
  if (index_capacity) {
    index = vkr_allocator_alloc(s->allocator, index_capacity * sizeof(*index),
                                EDIT_TAG);
    if (!index)
      goto cleanup;
  }
  for (uint32_t i = 0; i < index_capacity; ++i) {
    VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, entity, scene->comp_source_identity);
    if (source)
      index[index_count++] =
          (EditSourceIndex){.wrapper = source->scene_entity_index,
                            .node = source->gltf_node_index,
                            .entity = entity,
                            .fingerprint = source->source_fingerprint};
  }
  if (index_count > 1u) {
    qsort(index, index_count, sizeof(*index), edit_source_compare);
    for (uint32_t i = 1; i < index_count; ++i)
      if (edit_source_compare(&index[i - 1u], &index[i]) == 0)
        index[i - 1u].ambiguous = index[i].ambiguous = true_v;
  }
  for (uint32_t i = 0; i < touched_before; ++i) {
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, s->touched[i], scene->comp_source_identity);
    if (source) {
      EditSourceIndex *entry =
          edit_source_find(index, index_count, source->scene_entity_index,
                           source->gltf_node_index);
      if (entry && entry->entity.u64 == s->touched[i].u64)
        entry->touched = true_v;
    }
  }
  EditJson json = {.at = bytes, .end = bytes + length};
  uint32_t root_seen = 0;
  if (!edit_json_take(&json, '{'))
    goto cleanup;
  for (;;) {
    char key[32];
    if (!edit_json_string(&json, key, sizeof(key)) ||
        !edit_json_take(&json, ':'))
      goto cleanup;
    if (strcmp(key, "version") == 0) {
      int64_t version;
      if ((root_seen & 1u) || !edit_json_int(&json, 1, 1, &version))
        goto cleanup;
      root_seen |= 1u;
    } else if (strcmp(key, "overrides") == 0) {
      if ((root_seen & 2u) || !edit_json_take(&json, '['))
        goto cleanup;
      root_seen |= 2u;
      if (!edit_json_take(&json, ']'))
        for (;;) {
          VkrSceneEditValues values;
          uint32_t wrapper, node;
          uint64_t fingerprint;
          if (!edit_json_record(&json, &values, &wrapper, &node, &fingerprint))
            goto cleanup;
          EditSourceIndex *source =
              edit_source_find(index, index_count, wrapper, node);
          if (!source || source->ambiguous || source->seen ||
              source->fingerprint != fingerprint)
            goto cleanup;
          source->seen = true_v;
          VkrEntityId entity = source->entity;
          if (count == capacity) {
            uint32_t next = Max(16u, capacity * 2u);
            EditPrepared *entries = vkr_allocator_realloc(
                s->allocator, pending, capacity * sizeof(*entries),
                next * sizeof(*entries), EDIT_TAG);
            if (!entries)
              goto cleanup;
            pending = entries;
            capacity = next;
          }
          if (!edit_prepare(scene, entity, &values, &pending[count]))
            goto cleanup;
          pending[count].add_touched = !source->touched;
          additional_touched += !source->touched;
          count++;
          if (edit_json_take(&json, ']'))
            break;
          if (!edit_json_take(&json, ','))
            goto cleanup;
        }
    } else
      goto cleanup;
    if (edit_json_take(&json, '}'))
      break;
    if (!edit_json_take(&json, ','))
      goto cleanup;
  }
  edit_json_space(&json);
  if (root_seen != 3u || json.at != json.end)
    goto cleanup;
  if (additional_touched > UINT32_MAX - touched_before)
    goto cleanup;
  const uint32_t touched_needed = touched_before + additional_touched;
  if (touched_needed > s->touched_capacity) {
    const uint32_t next_capacity = Max(32u, touched_needed);
    VkrEntityId *next = vkr_allocator_realloc(
        s->allocator, s->touched, s->touched_capacity * sizeof(*next),
        next_capacity * sizeof(*next), EDIT_TAG);
    if (!next)
      goto cleanup;
    s->touched = next;
    s->touched_capacity = next_capacity;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (pending[i].add_touched)
      s->touched[s->touched_count++] = pending[i].entity;
    edit_commit(scene, &pending[i]);
  }
  snprintf(s->status, sizeof(s->status), "Loaded %u node overrides.", count);
  success = true_v;
  s->sidecar_conflict = false_v;
cleanup:
  if (index)
    vkr_allocator_free(s->allocator, index, index_capacity * sizeof(*index),
                       EDIT_TAG);
  if (file)
    fclose(file);
  if (bytes)
    vkr_allocator_free(s->allocator, bytes, (uint64_t)length, EDIT_TAG);
  for (uint32_t i = 0; i < count; ++i)
    edit_discard(scene, &pending[i]);
  if (pending)
    vkr_allocator_free(s->allocator, pending, capacity * sizeof(*pending),
                       EDIT_TAG);
  if (!success) {
    s->touched_count = touched_before;
    s->sidecar_conflict = true_v;
    snprintf(s->status, sizeof(s->status),
             "Override conflict or invalid file. Source scene preserved.");
    log_error("Editor overrides rejected: %.*s", (int)path.length, path.str);
  }
  return success;
}
