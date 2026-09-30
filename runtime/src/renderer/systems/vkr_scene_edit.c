#include "vkr_scene_edit.h"
#include "filesystem/filesystem.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_types.h"

#include "core/logger.h"
#include "core/vkr_json_writer.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDIT_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* World components one object or overlay record carries. */
#define EDIT_COMPONENT_MAX 8u

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
  const SceneRectangleLight *rectangle = vkr_entity_get_component(
      scene->world, entity, scene->comp_rectangle_light);
  if (rectangle) {
    out->fields |= VKR_SCENE_EDIT_RECTANGLE_LIGHT;
    out->rectangle_light = *rectangle;
  }
  if (vkr_scene_physics_read(scene, entity, &out->physics)) {
    out->fields |= VKR_SCENE_EDIT_PHYSICS;
  }
  return true_v;
}

bool8_t vkr_scene_edit_validate(const VkrSceneEditValues *v) {
  if (!v->fields || (v->fields & ~255u))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_COMPONENT) &&
      (!v->component_type ||
       vkr_scene_world_type_named(string8_create_from_cstr(
           (const uint8_t *)v->component_type->name,
           strlen(v->component_type->name))) != v->component_type ||
       !vkr_type_validate(v->component_type, v->component, NULL, 0u)))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_PHYSICS) &&
      !vkr_scene_physics_snapshot_validate(&v->physics, NULL)) {
    return false_v;
  }
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
  if ((v->fields & VKR_SCENE_EDIT_POINT_LIGHT) &&
      !vkr_type_validate(&vkr_scene_point_light_type, &v->point_light, NULL,
                         0u))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) &&
      !vkr_type_validate(&vkr_scene_directional_light_type,
                         &v->directional_light, NULL, 0u))
    return false_v;
  if ((v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) &&
      !vkr_type_validate(&vkr_scene_rectangle_light_type, &v->rectangle_light,
                         NULL, 0u))
    return false_v;
  return true_v;
}

bool8_t vkr_scene_edit_read_component(const VkrScene *scene, VkrEntityId entity,
                                      const VkrTypeDesc *type,
                                      VkrSceneEditValues *values) {
  const void *component = vkr_scene_get_typed(scene, entity, type);
  if (!component) {
    return false_v;
  }
  values->component_type = type;
  MemCopy(values->component, component, type->size);
  values->fields |= VKR_SCENE_EDIT_COMPONENT;
  return true_v;
}

typedef struct EditComponent {
  const VkrTypeDesc *type;
  uint32_t field;
} EditComponent;

static const EditComponent s_edit_components[] = {
    {&vkr_scene_transform_type, VKR_SCENE_EDIT_TRANSFORM},
    {&vkr_scene_visibility_type, VKR_SCENE_EDIT_VISIBILITY},
    {&vkr_scene_point_light_type, VKR_SCENE_EDIT_POINT_LIGHT},
    {&vkr_scene_directional_light_type, VKR_SCENE_EDIT_DIRECTIONAL_LIGHT},
    {&vkr_scene_rectangle_light_type, VKR_SCENE_EDIT_RECTANGLE_LIGHT},
    {&vkr_scene_physics_body_type, VKR_SCENE_EDIT_PHYSICS},
};

const VkrTypeDesc *vkr_scene_edit_component_type(uint32_t index) {
  return index < ArrayCount(s_edit_components) ? s_edit_components[index].type
                                               : NULL;
}

uint32_t vkr_scene_edit_component_field(const VkrTypeDesc *type) {
  for (uint32_t i = 0; i < ArrayCount(s_edit_components); ++i) {
    if (s_edit_components[i].type == type) {
      return s_edit_components[i].field;
    }
  }
  return 0u;
}

bool8_t vkr_scene_edit_component_get(const VkrSceneEditValues *values,
                                     const VkrTypeDesc *type, void *out) {
  const uint32_t field = vkr_scene_edit_component_field(type);
  if (!field || !(values->fields & field)) {
    return false_v;
  }
  switch (field) {
  case VKR_SCENE_EDIT_TRANSFORM: {
    SceneTransform transform = {0};
    transform.position = values->position;
    transform.rotation = values->rotation;
    transform.scale = values->scale;
    MemCopy(out, &transform, sizeof(transform));
    return true_v;
  }
  case VKR_SCENE_EDIT_VISIBILITY:
    MemCopy(out, &values->visibility, sizeof(values->visibility));
    return true_v;
  case VKR_SCENE_EDIT_POINT_LIGHT:
    MemCopy(out, &values->point_light, sizeof(values->point_light));
    return true_v;
  case VKR_SCENE_EDIT_DIRECTIONAL_LIGHT:
    MemCopy(out, &values->directional_light, sizeof(values->directional_light));
    return true_v;
  case VKR_SCENE_EDIT_RECTANGLE_LIGHT:
    MemCopy(out, &values->rectangle_light, sizeof(values->rectangle_light));
    return true_v;
  case VKR_SCENE_EDIT_PHYSICS:
    /* Physics reads succeed without a body; only a present body counts. */
    if (!values->physics.present) {
      return false_v;
    }
    MemCopy(out, &values->physics.body, sizeof(values->physics.body));
    return true_v;
  default:
    return false_v;
  }
}

bool8_t vkr_scene_edit_component_set(VkrSceneEditValues *values,
                                     const VkrTypeDesc *type, const void *in) {
  switch (vkr_scene_edit_component_field(type)) {
  case VKR_SCENE_EDIT_TRANSFORM: {
    const SceneTransform *transform = in;
    values->position = transform->position;
    values->rotation = transform->rotation;
    values->scale = transform->scale;
    return true_v;
  }
  case VKR_SCENE_EDIT_VISIBILITY:
    MemCopy(&values->visibility, in, sizeof(values->visibility));
    return true_v;
  case VKR_SCENE_EDIT_POINT_LIGHT:
    MemCopy(&values->point_light, in, sizeof(values->point_light));
    return true_v;
  case VKR_SCENE_EDIT_DIRECTIONAL_LIGHT:
    MemCopy(&values->directional_light, in, sizeof(values->directional_light));
    return true_v;
  case VKR_SCENE_EDIT_RECTANGLE_LIGHT:
    MemCopy(&values->rectangle_light, in, sizeof(values->rectangle_light));
    return true_v;
  case VKR_SCENE_EDIT_PHYSICS:
    MemCopy(&values->physics.body, in, sizeof(values->physics.body));
    return true_v;
  default:
    return false_v;
  }
}

void vkr_scene_edit_reset(VkrSceneEditState *s, VkrAllocator *allocator,
                          uint64_t generation) {
  for (uint32_t i = 0; i < s->undo_count; ++i) {
    vkr_allocator_free(s->allocator, s->undo[i].payload,
                       s->undo[i].payload_size, EDIT_TAG);
  }
  if (s->undo)
    vkr_allocator_free(s->allocator, s->undo,
                       sizeof(*s->undo) * VKR_SCENE_EDIT_UNDO_CAPACITY,
                       EDIT_TAG);
  if (s->touched)
    vkr_allocator_free(s->allocator, s->touched,
                       sizeof(*s->touched) * s->touched_capacity, EDIT_TAG);
  if (s->created)
    vkr_allocator_free(s->allocator, s->created,
                       sizeof(*s->created) * s->created_capacity, EDIT_TAG);
  if (s->deleted)
    vkr_allocator_free(s->allocator, s->deleted,
                       sizeof(*s->deleted) * s->deleted_capacity, EDIT_TAG);
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
typedef enum EditPreparedOp {
  EDIT_PREPARED_VALUES,
  /* Overlay structure: values.component_type names the component. */
  EDIT_PREPARED_ADD_COMPONENT,
  EDIT_PREPARED_REMOVE_COMPONENT,
  EDIT_PREPARED_DESTROY,
} EditPreparedOp;

typedef struct EditPrepared {
  VkrEntityId entity;
  VkrSceneEditValues values;
  String8 replacement_name;
  VkrScenePhysicsPrepared *physics;
  bool8_t add_touched;
  EditPreparedOp op;
} EditPrepared;

static void edit_discard(VkrScene *scene, EditPrepared *p) {
  vkr_scene_physics_discard(p->physics);
  p->physics = NULL;
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
  if (!vkr_scene_edit_read(scene, entity, &current))
    return false_v;
  if (v->fields & VKR_SCENE_EDIT_COMPONENT)
    (void)vkr_scene_edit_read_component(scene, entity, v->component_type,
                                        &current);
  if ((v->fields & ~current.fields) || !vkr_scene_edit_validate(v))
    return false_v;
  const VkrEntityId physics_owner = vkr_scene_physics_owner(scene, entity);
  if (physics_owner.u64 && physics_owner.u64 != entity.u64) {
    return false_v;
  }
  if ((v->fields & VKR_SCENE_EDIT_TRANSFORM) && current.physics.present &&
      !vkr_scene_physics_is_paused(scene)) {
    return false_v;
  }
  if ((v->fields & VKR_SCENE_EDIT_NAME) && strcmp(current.name, v->name)) {
    uint64_t length = strlen(v->name);
    uint8_t *name = vkr_allocator_alloc(scene->alloc, length + 1u,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!name)
      return false_v;
    MemCopy(name, v->name, length + 1u);
    p->replacement_name = string8_create(name, length);
  }
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if ((v->fields & VKR_SCENE_EDIT_TRANSFORM) &&
      ((v->fields & VKR_SCENE_EDIT_PHYSICS) ? v->physics.present
                                            : current.physics.present) &&
      (!transform ||
       !vkr_scene_physics_transform_validate(
           scene, entity, v->position, vkr_quat_normalize(v->rotation),
           v->scale, transform->parent, NULL))) {
    edit_discard(scene, p);
    return false_v;
  }
  return true_v;
}

static bool8_t edit_prepare_physics(VkrScene *scene, EditPrepared *prepared) {
  const VkrSceneEditValues *values = &prepared->values;
  if (!(values->fields & VKR_SCENE_EDIT_PHYSICS)) {
    return true_v;
  }
  VkrScenePhysicsSnapshot current;
  if (!vkr_scene_physics_read(scene, prepared->entity, &current)) {
    return false_v;
  }
  return !(values->physics.present || current.present) ||
         vkr_scene_physics_prepare(scene, prepared->entity, &values->physics,
                                   &prepared->physics, NULL);
}

static void edit_commit(VkrScene *scene, EditPrepared *p) {
  const VkrSceneEditValues *v = &p->values;
  VkrEntityId entity = p->entity;
  switch (p->op) {
  case EDIT_PREPARED_VALUES:
    break;
  case EDIT_PREPARED_ADD_COMPONENT:
    if (!vkr_scene_set_typed(scene, entity, v->component_type, v->component))
      log_warn("Overlay component %s could not be added",
               v->component_type->name);
    return;
  case EDIT_PREPARED_REMOVE_COMPONENT:
    (void)vkr_scene_remove_typed(scene, entity, v->component_type);
    return;
  case EDIT_PREPARED_DESTROY:
    vkr_scene_destroy_entity(scene, entity);
    return;
  }
  if (p->physics) {
    vkr_scene_physics_commit(p->physics);
    p->physics = NULL;
  }
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
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT)
    *vkr_scene_get_rectangle_light(scene, entity) = v->rectangle_light;
  /* Preparation proved the component exists, so this overwrites in place. */
  if (v->fields & VKR_SCENE_EDIT_COMPONENT)
    (void)vkr_scene_set_typed(scene, entity, v->component_type, v->component);
}

static bool8_t edit_write(VkrScene *scene, VkrEntityId entity,
                          const VkrSceneEditValues *v) {
  EditPrepared prepared;
  if (!edit_prepare(scene, entity, v, &prepared)) {
    return false_v;
  }
  if (!edit_prepare_physics(scene, &prepared) ||
      !vkr_scene_physics_prepare_complete(scene, NULL)) {
    edit_discard(scene, &prepared);
    return false_v;
  }
  edit_commit(scene, &prepared);
  return true_v;
}

/* Payloads are allocated only for live journal entries. Expanded collider
   references and scene-global settings do not inflate every history slot. */
static void *edit_journal_prepare(VkrSceneEditState *s, VkrEntityId entity,
                                  uint64_t payload_size) {
  if (!s->undo) {
    s->undo = vkr_allocator_alloc(
        s->allocator, sizeof(*s->undo) * VKR_SCENE_EDIT_UNDO_CAPACITY,
        EDIT_TAG);
    if (!s->undo) {
      return NULL;
    }
  }
  void *payload = vkr_allocator_alloc(s->allocator, payload_size, EDIT_TAG);
  if (!payload) {
    return NULL;
  }
  if (entity.u64 && !edit_touch(s, entity)) {
    vkr_allocator_free(s->allocator, payload, payload_size, EDIT_TAG);
    return NULL;
  }
  return payload;
}

/* Edits happen on the UI thread; one counter orders every container's
   journal. */
static uint64_t s_edit_sequence;

uint64_t vkr_scene_edit_next_sequence(const VkrSceneEditState *s,
                                      bool8_t redo) {
  if (!s || !s->undo) {
    return 0u;
  }
  if (redo) {
    return s->undo_cursor < s->undo_count ? s->undo[s->undo_cursor].sequence
                                          : 0u;
  }
  return s->undo_cursor ? s->undo[s->undo_cursor - 1u].sequence : 0u;
}

static void edit_journal_append(VkrSceneEditState *s, VkrSceneEditEntry entry) {
  entry.sequence = ++s_edit_sequence;
  for (uint32_t i = s->undo_cursor; i < s->undo_count; ++i) {
    vkr_allocator_free(s->allocator, s->undo[i].payload,
                       s->undo[i].payload_size, EDIT_TAG);
  }
  s->undo_count = s->undo_cursor;
  if (s->undo_count == VKR_SCENE_EDIT_UNDO_CAPACITY) {
    vkr_allocator_free(s->allocator, s->undo[0].payload,
                       s->undo[0].payload_size, EDIT_TAG);
    MemCopy(s->undo, s->undo + 1,
            (VKR_SCENE_EDIT_UNDO_CAPACITY - 1u) * sizeof(*s->undo));
    s->undo_count--;
  }
  s->undo[s->undo_count++] = entry;
  s->undo_cursor = s->undo_count;
  s->revision++;
  s->gesture = 0u;
  snprintf(s->status, sizeof(s->status),
           "Edited. Save writes scene overrides.");
}

bool8_t vkr_scene_edit_apply(VkrSceneEditState *s, VkrScene *scene,
                             VkrEntityId entity, const VkrSceneEditValues *v) {
  VkrSceneEditValues before;
  EditPrepared prepared;
  if ((v->fields & VKR_SCENE_EDIT_PHYSICS) &&
      (v->physics.present || vkr_scene_physics_owner(scene, entity).u64)) {
    const char *error = NULL;
    if (!vkr_scene_physics_is_paused(scene)) {
      snprintf(s->status, sizeof(s->status),
               "Pause simulation before editing physics.");
      return false_v;
    }
    if (!vkr_scene_physics_validate(scene, entity, &v->physics, &error)) {
      snprintf(s->status, sizeof(s->status), "%s",
               error ? error : "Invalid physics settings.");
      return false_v;
    }
  }
  if (!vkr_scene_edit_read(scene, entity, &before) ||
      ((v->fields & VKR_SCENE_EDIT_COMPONENT) &&
       !vkr_scene_edit_read_component(scene, entity, v->component_type,
                                      &before)) ||
      !edit_prepare(scene, entity, v, &prepared)) {
    snprintf(s->status, sizeof(s->status),
             "Invalid values or stale selection.");
    return false_v;
  }
  if (!edit_prepare_physics(scene, &prepared) ||
      !vkr_scene_physics_prepare_complete(scene, NULL)) {
    edit_discard(scene, &prepared);
    snprintf(s->status, sizeof(s->status), "%s",
             vkr_scene_physics_error(scene));
    return false_v;
  }
  VkrSceneEditValues *payload =
      edit_journal_prepare(s, entity, 2u * sizeof(*payload));
  if (!payload) {
    edit_discard(scene, &prepared);
    return false_v;
  }
  before.fields = v->fields;
  payload[0] = before;
  payload[1] = *v;
  edit_commit(scene, &prepared);
  edit_journal_append(
      s, (VkrSceneEditEntry){.entity = entity,
                             .kind = VKR_SCENE_EDIT_ENTRY_ENTITY,
                             .payload = payload,
                             .payload_size = 2u * sizeof(*payload)});
  return true_v;
}

bool8_t vkr_scene_edit_apply_gesture(VkrSceneEditState *s, VkrScene *scene,
                                     VkrEntityId entity,
                                     const VkrSceneEditValues *v,
                                     uint64_t gesture) {
  VkrSceneEditValues *last =
      s->undo_count && s->undo_cursor == s->undo_count &&
              s->undo[s->undo_count - 1u].kind == VKR_SCENE_EDIT_ENTRY_ENTITY &&
              s->undo[s->undo_count - 1u].entity.u64 == entity.u64
          ? (VkrSceneEditValues *)s->undo[s->undo_count - 1u].payload
          : NULL;
  const bool8_t coalesce = gesture && s->gesture == gesture && last &&
                           last[1].fields == v->fields &&
                           last[1].component_type == v->component_type &&
                           !(v->fields & VKR_SCENE_EDIT_PHYSICS);
  if (!coalesce) {
    const bool8_t applied = vkr_scene_edit_apply(s, scene, entity, v);
    s->gesture = applied ? gesture : 0u;
    return applied;
  }
  EditPrepared prepared;
  if (!edit_prepare(scene, entity, v, &prepared)) {
    snprintf(s->status, sizeof(s->status),
             "Invalid values or stale selection.");
    return false_v;
  }
  if (!edit_prepare_physics(scene, &prepared) ||
      !vkr_scene_physics_prepare_complete(scene, NULL)) {
    edit_discard(scene, &prepared);
    snprintf(s->status, sizeof(s->status), "%s",
             vkr_scene_physics_error(scene));
    return false_v;
  }
  edit_commit(scene, &prepared);
  last[1] = *v;
  s->revision++;
  return true_v;
}

bool8_t vkr_scene_edit_record_external(VkrSceneEditState *s, VkrScene *scene,
                                       VkrEntityId entity,
                                       const VkrSceneEditValues *before,
                                       const VkrSceneEditValues *after) {
  VkrSceneEditValues current;
  if (before->fields != after->fields || !vkr_scene_edit_validate(before) ||
      !vkr_scene_edit_validate(after) ||
      !vkr_scene_edit_read(scene, entity, &current)) {
    return false_v;
  }
  if (after->fields & VKR_SCENE_EDIT_COMPONENT)
    (void)vkr_scene_edit_read_component(scene, entity, after->component_type,
                                        &current);
  if (after->fields & ~current.fields) {
    return false_v;
  }
  VkrSceneEditValues *payload =
      edit_journal_prepare(s, entity, 2u * sizeof(*payload));
  if (!payload) {
    return false_v;
  }
  payload[0] = *before;
  payload[1] = *after;
  edit_journal_append(
      s, (VkrSceneEditEntry){.entity = entity,
                             .kind = VKR_SCENE_EDIT_ENTRY_ENTITY,
                             .payload = payload,
                             .payload_size = 2u * sizeof(*payload)});
  return true_v;
}

bool8_t
vkr_scene_edit_apply_collision_layers(VkrSceneEditState *s, VkrScene *scene,
                                      const VkrSceneCollisionLayers *settings) {
  const char *error = NULL;
  if (!vkr_scene_collision_layers_validate(settings, &error)) {
    snprintf(s->status, sizeof(s->status), "%s",
             error ? error : "Invalid collision settings.");
    return false_v;
  }
  VkrSceneCollisionLayers *payload =
      edit_journal_prepare(s, VKR_ENTITY_ID_INVALID, 2u * sizeof(*payload));
  if (!payload) {
    return false_v;
  }
  vkr_scene_collision_layers_read(scene, &payload[0]);
  payload[1] = *settings;
  if (!vkr_scene_collision_layers_apply(scene, settings, &error)) {
    vkr_allocator_free(s->allocator, payload, 2u * sizeof(*payload), EDIT_TAG);
    snprintf(s->status, sizeof(s->status), "%s",
             error ? error : "Collision settings failed.");
    return false_v;
  }
  edit_journal_append(
      s, (VkrSceneEditEntry){.kind = VKR_SCENE_EDIT_ENTRY_COLLISION_LAYERS,
                             .payload = payload,
                             .payload_size = 2u * sizeof(*payload)});
  return true_v;
}

bool8_t vkr_scene_edit_apply_scene_settings(VkrSceneEditState *s,
                                            VkrScene *scene,
                                            const VkrSceneSettings *settings) {
  if (settings->inherit_world > 1u) {
    return false_v;
  }
  VkrSceneSettings *payload =
      edit_journal_prepare(s, VKR_ENTITY_ID_INVALID, 2u * sizeof(*payload));
  if (!payload) {
    return false_v;
  }
  payload[0] = scene->settings;
  payload[1] = *settings;
  scene->settings = *settings;
  scene->world_revision++;
  edit_journal_append(
      s, (VkrSceneEditEntry){.kind = VKR_SCENE_EDIT_ENTRY_SCENE_SETTINGS,
                             .payload = payload,
                             .payload_size = 2u * sizeof(*payload)});
  return true_v;
}

typedef struct EditPhysicsBatchRecord {
  VkrEntityId entity;
  VkrScenePhysicsSnapshot before;
  VkrScenePhysicsSnapshot after;
} EditPhysicsBatchRecord;

static bool8_t edit_physics_batch_write(VkrSceneEditState *s, VkrScene *scene,
                                        const EditPhysicsBatchRecord *records,
                                        uint32_t count, bool8_t after) {
  VkrScenePhysicsPrepared *pending[VKR_SCENE_PHYSICS_MAX_BODIES] = {0};
  uint32_t prepared_count = 0;
  const char *error = NULL;
  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_scene_physics_prepare(scene, records[i].entity,
                                   after ? &records[i].after
                                         : &records[i].before,
                                   &pending[prepared_count], &error)) {
      goto failed;
    }
    prepared_count++;
  }
  if (!vkr_scene_physics_prepare_complete(scene, &error)) {
    goto failed;
  }
  for (uint32_t i = 0; i < prepared_count; ++i) {
    vkr_scene_physics_commit(pending[i]);
  }
  return true_v;
failed:
  for (uint32_t i = 0; i < prepared_count; ++i) {
    vkr_scene_physics_discard(pending[i]);
  }
  snprintf(s->status, sizeof(s->status), "%s",
           error ? error : "Physics batch could not be staged.");
  return false_v;
}

bool8_t vkr_scene_edit_apply_physics_batch(VkrSceneEditState *s,
                                           VkrScene *scene,
                                           const VkrScenePhysicsChange *changes,
                                           uint32_t count) {
  if (!changes || !count || count > VKR_SCENE_PHYSICS_MAX_BODIES) {
    return false_v;
  }
  const uint64_t size = count * sizeof(EditPhysicsBatchRecord);
  EditPhysicsBatchRecord *records =
      edit_journal_prepare(s, VKR_ENTITY_ID_INVALID, size);
  if (!records) {
    return false_v;
  }
  const uint32_t touched_before = s->touched_count;
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t j = 0; j < i; ++j) {
      if (changes[j].entity.u64 == changes[i].entity.u64) {
        goto failed;
      }
    }
    records[i].entity = changes[i].entity;
    records[i].after = changes[i].snapshot;
    if (!vkr_scene_physics_read(scene, records[i].entity, &records[i].before) ||
        !vkr_scene_physics_snapshot_validate(&records[i].after, NULL) ||
        !edit_touch(s, records[i].entity)) {
      goto failed;
    }
  }
  if (!edit_physics_batch_write(s, scene, records, count, true_v)) {
    goto failed;
  }
  edit_journal_append(
      s, (VkrSceneEditEntry){.kind = VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH,
                             .payload = records,
                             .payload_size = size});
  return true_v;
failed:
  s->touched_count = touched_before;
  vkr_allocator_free(s->allocator, records, size, EDIT_TAG);
  return false_v;
}

// =============================================================================
// Structure: components, creation, deletion and parents (ADR-076)
// =============================================================================

typedef enum EditObjectPart {
  EDIT_OBJECT_NAME = 1u << 0,
  EDIT_OBJECT_TRANSFORM = 1u << 1,
  EDIT_OBJECT_VISIBILITY = 1u << 2,
  EDIT_OBJECT_SOURCE = 1u << 3,
  EDIT_OBJECT_POINT_LIGHT = 1u << 4,
  EDIT_OBJECT_DIRECTIONAL_LIGHT = 1u << 5,
  EDIT_OBJECT_RECTANGLE_LIGHT = 1u << 6,
} EditObjectPart;

/* Everything delete removes and undo restores. Only entities made of these
   parts can be deleted, so the snapshot is exact. */
typedef struct EditObject {
  uint32_t parts;
  char name[VKR_SCENE_EDIT_NAME_CAPACITY];
  /* Raw authored transform; `parent` is the entity's parent. */
  SceneTransform transform;
  SceneVisibility visibility;
  SceneSourceIdentity source;
  ScenePointLight point_light;
  SceneDirectionalLight directional_light;
  SceneRectangleLight rectangle_light;
  /* Overlay id when the editor created the entity, else zero. */
  uint32_t created_id;
  uint32_t component_count;
  const VkrTypeDesc *types[EDIT_COMPONENT_MAX];
  _Alignas(16) uint8_t components[EDIT_COMPONENT_MAX][VKR_TYPE_VALUE_MAX];
} EditObject;

typedef enum EditStructureOp {
  EDIT_STRUCTURE_ADD_COMPONENT,
  EDIT_STRUCTURE_REMOVE_COMPONENT,
  /* `replaced` and its bytes before; `type` and `component` after. */
  EDIT_STRUCTURE_REPLACE_COMPONENT,
  EDIT_STRUCTURE_CREATE,
  EDIT_STRUCTURE_DELETE,
  EDIT_STRUCTURE_REPARENT,
} EditStructureOp;

typedef struct EditStructure {
  EditStructureOp op;
  /* REPARENT: parent and local transform before [0] and after [1]. */
  VkrEntityId parent[2];
  Vec3 position[2];
  VkrQuat rotation[2];
  Vec3 scale[2];
  /* ADD_COMPONENT, REMOVE_COMPONENT and REPLACE_COMPONENT. */
  const VkrTypeDesc *type;
  _Alignas(16) uint8_t component[VKR_TYPE_VALUE_MAX];
  const VkrTypeDesc *replaced;
  _Alignas(16) uint8_t replaced_component[VKR_TYPE_VALUE_MAX];
  /* CREATE and DELETE. */
  EditObject object;
} EditStructure;

/* Types the editor may add or remove on a loaded scene. */
static bool8_t edit_world_type(const VkrTypeDesc *type) {
  return vkr_scene_world_type_live(type);
}

static bool8_t edit_has_children(const VkrScene *scene, VkrEntityId entity) {
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId other = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, other, scene->comp_transform);
    if (transform && transform->parent.u64 == entity.u64) {
      return true_v;
    }
  }
  return false_v;
}

/* The entity is made only of parts an EditObject snapshot restores. */
static bool8_t edit_deletable_parts(const VkrScene *scene, VkrEntityId entity,
                                    const char **reason) {
  const char *unused = NULL;
  reason = reason ? reason : &unused;
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    *reason = "The object no longer exists.";
    return false_v;
  }
  const VkrEntityRecord *record =
      &scene->world->dir.records[entity.parts.index];
  const VkrArchetype *archetype = vkr_entity_chunk_archetype(record->chunk);
  const uint32_t count = vkr_entity_archetype_component_count(archetype);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrComponentTypeId id =
        vkr_entity_archetype_component_at(archetype, i);
    /* Light setters assign a fresh picking id when a light is restored. */
    bool8_t known =
        id == scene->comp_source_identity || id == scene->comp_name ||
        id == scene->comp_transform || id == scene->comp_visibility ||
        id == scene->comp_render_id || id == scene->comp_point_light ||
        id == scene->comp_directional_light ||
        id == scene->comp_rectangle_light;
    /* Animation settings need their binding, which undo cannot restore. */
    for (uint32_t t = 0; !known && t < scene->type_count; ++t) {
      known = id == scene->types[t].id &&
              scene->types[t].type != &vkr_scene_animation_type;
    }
    /* Generated shape and text state is rebuilt from its typed component. */
    known |= (id == scene->comp_shape &&
              vkr_scene_get_typed(scene, entity, &vkr_scene_shape_type)) ||
             (id == scene->comp_text3d &&
              vkr_scene_get_typed(scene, entity, &vkr_scene_text_type));
    if (!known) {
      *reason = "Only lights, shapes, text and world objects can be deleted; "
                "meshes and physics bodies cannot yet.";
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_scene_edit_can_delete(const VkrScene *scene, VkrEntityId entity,
                                  const char **reason) {
  const char *unused = NULL;
  reason = reason ? reason : &unused;
  if (!edit_deletable_parts(scene, entity, reason)) {
    return false_v;
  }
  if (edit_has_children(scene, entity)) {
    *reason = "Delete or move its children first.";
    return false_v;
  }
  return true_v;
}

static uint32_t edit_created_id(const VkrSceneEditState *s,
                                VkrEntityId entity) {
  for (uint32_t i = 0; i < s->created_count; ++i) {
    if (s->created[i].entity.u64 == entity.u64) {
      return s->created[i].id;
    }
  }
  return 0u;
}

static void edit_object_capture(const VkrSceneEditState *s,
                                const VkrScene *scene, VkrEntityId entity,
                                EditObject *o) {
  MemZero(o, sizeof(*o));
  const VkrWorld *world = scene->world;
  String8 name = vkr_scene_get_name(scene, entity);
  if (vkr_entity_get_component(world, entity, scene->comp_name) &&
      name.length < sizeof(o->name)) {
    MemCopy(o->name, name.str, name.length);
    o->parts |= EDIT_OBJECT_NAME;
  }
  const SceneTransform *transform =
      vkr_entity_get_component(world, entity, scene->comp_transform);
  if (transform) {
    o->transform = *transform;
    o->parts |= EDIT_OBJECT_TRANSFORM;
  }
  const SceneVisibility *visibility =
      vkr_entity_get_component(world, entity, scene->comp_visibility);
  if (visibility) {
    o->visibility = *visibility;
    o->parts |= EDIT_OBJECT_VISIBILITY;
  }
  const SceneSourceIdentity *source =
      vkr_entity_get_component(world, entity, scene->comp_source_identity);
  if (source) {
    o->source = *source;
    o->parts |= EDIT_OBJECT_SOURCE;
  }
  const ScenePointLight *point =
      vkr_entity_get_component(world, entity, scene->comp_point_light);
  if (point) {
    o->point_light = *point;
    o->parts |= EDIT_OBJECT_POINT_LIGHT;
  }
  const SceneDirectionalLight *directional =
      vkr_entity_get_component(world, entity, scene->comp_directional_light);
  if (directional) {
    o->directional_light = *directional;
    o->parts |= EDIT_OBJECT_DIRECTIONAL_LIGHT;
  }
  const SceneRectangleLight *rectangle =
      vkr_entity_get_component(world, entity, scene->comp_rectangle_light);
  if (rectangle) {
    o->rectangle_light = *rectangle;
    o->parts |= EDIT_OBJECT_RECTANGLE_LIGHT;
  }
  for (uint32_t i = 0;
       i < scene->type_count && o->component_count < EDIT_COMPONENT_MAX; ++i) {
    const void *value =
        vkr_entity_get_component(world, entity, scene->types[i].id);
    if (value) {
      o->types[o->component_count] = scene->types[i].type;
      MemCopy(o->components[o->component_count], value,
              scene->types[i].type->size);
      o->component_count++;
    }
  }
  o->created_id = edit_created_id(s, entity);
}

/* A new entity carrying the snapshot; invalid, with nothing left behind, when
   any part cannot be stored. The parent must be alive or invalid. */
static VkrEntityId edit_object_restore(VkrScene *scene, const EditObject *o,
                                       VkrEntityId parent) {
  const VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  if (!entity.u64) {
    return VKR_ENTITY_ID_INVALID;
  }
  bool8_t ok = true_v;
  if (o->parts & EDIT_OBJECT_NAME) {
    ok = vkr_scene_set_name(
        scene, entity,
        string8_create_from_cstr((const uint8_t *)o->name, strlen(o->name)));
  }
  if (ok && (o->parts & EDIT_OBJECT_TRANSFORM)) {
    ok = vkr_scene_set_transform(scene, entity, o->transform.position,
                                 o->transform.rotation, o->transform.scale);
    SceneTransform *transform =
        ok ? vkr_scene_get_transform(scene, entity) : NULL;
    if (transform) {
      *transform = o->transform;
      transform->parent = VKR_ENTITY_ID_INVALID;
      transform->flags |=
          SCENE_TRANSFORM_DIRTY_HIERARCHY | SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }
  if (ok && (o->parts & EDIT_OBJECT_VISIBILITY)) {
    vkr_scene_set_visibility(scene, entity, o->visibility.visible,
                             o->visibility.inherit_parent);
  }
  if (ok && (o->parts & EDIT_OBJECT_SOURCE)) {
    ok = vkr_scene_set_source_identity(scene, entity, &o->source);
  }
  if (ok && (o->parts & EDIT_OBJECT_POINT_LIGHT)) {
    ok = vkr_scene_set_point_light(scene, entity, &o->point_light);
  }
  if (ok && (o->parts & EDIT_OBJECT_DIRECTIONAL_LIGHT)) {
    ok = vkr_scene_set_directional_light(scene, entity, &o->directional_light);
  }
  if (ok && (o->parts & EDIT_OBJECT_RECTANGLE_LIGHT)) {
    ok = vkr_scene_set_rectangle_light(scene, entity, &o->rectangle_light);
  }
  for (uint32_t i = 0; ok && i < o->component_count; ++i) {
    ok = vkr_scene_set_typed(scene, entity, o->types[i], o->components[i]);
  }
  if (!ok) {
    vkr_scene_destroy_entity(scene, entity);
    return VKR_ENTITY_ID_INVALID;
  }
  if (parent.u64 && (o->parts & EDIT_OBJECT_TRANSFORM)) {
    vkr_scene_set_parent(scene, entity, parent);
  }
  scene->structure_revision++;
  return entity;
}

static void edit_remap_entity(VkrEntityId *id, VkrEntityId from,
                              VkrEntityId to) {
  if (id->u64 == from.u64) {
    *id = to;
  }
}

/* Recreation gives an entity a new ECS id; every journal reference follows. */
static void edit_remap(VkrSceneEditState *s, VkrEntityId from, VkrEntityId to) {
  for (uint32_t i = 0; i < s->undo_count; ++i) {
    VkrSceneEditEntry *entry = &s->undo[i];
    edit_remap_entity(&entry->entity, from, to);
    if (entry->kind == VKR_SCENE_EDIT_ENTRY_STRUCTURE) {
      EditStructure *structure = entry->payload;
      edit_remap_entity(&structure->parent[0], from, to);
      edit_remap_entity(&structure->parent[1], from, to);
      edit_remap_entity(&structure->object.transform.parent, from, to);
    } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH) {
      EditPhysicsBatchRecord *records = entry->payload;
      const uint32_t count = (uint32_t)(entry->payload_size / sizeof(*records));
      for (uint32_t r = 0; r < count; ++r) {
        edit_remap_entity(&records[r].entity, from, to);
      }
    }
  }
  for (uint32_t i = 0; i < s->touched_count; ++i) {
    edit_remap_entity(&s->touched[i], from, to);
  }
  for (uint32_t i = 0; i < s->created_count; ++i) {
    edit_remap_entity(&s->created[i].entity, from, to);
  }
}

static bool8_t edit_created_add(VkrSceneEditState *s, VkrEntityId entity,
                                uint32_t id) {
  if (s->created_count == s->created_capacity) {
    const uint32_t capacity = Max(16u, s->created_capacity * 2u);
    VkrSceneEditCreated *next = vkr_allocator_realloc(
        s->allocator, s->created, s->created_capacity * sizeof(*next),
        capacity * sizeof(*next), EDIT_TAG);
    if (!next) {
      return false_v;
    }
    s->created = next;
    s->created_capacity = capacity;
  }
  s->created[s->created_count++] = (VkrSceneEditCreated){entity, id};
  s->next_created_id = Max(s->next_created_id, id + 1u);
  return true_v;
}

static bool8_t edit_deleted_add(VkrSceneEditState *s,
                                const SceneSourceIdentity *source) {
  if (s->deleted_count == s->deleted_capacity) {
    const uint32_t capacity = Max(16u, s->deleted_capacity * 2u);
    SceneSourceIdentity *next = vkr_allocator_realloc(
        s->allocator, s->deleted, s->deleted_capacity * sizeof(*next),
        capacity * sizeof(*next), EDIT_TAG);
    if (!next) {
      return false_v;
    }
    s->deleted = next;
    s->deleted_capacity = capacity;
  }
  s->deleted[s->deleted_count++] = *source;
  return true_v;
}

static void edit_deleted_remove(VkrSceneEditState *s,
                                const SceneSourceIdentity *source) {
  for (uint32_t i = 0; i < s->deleted_count; ++i) {
    if (s->deleted[i].scene_entity_index == source->scene_entity_index &&
        s->deleted[i].gltf_node_index == source->gltf_node_index) {
      s->deleted[i] = s->deleted[--s->deleted_count];
      return;
    }
  }
}

static void edit_decompose(Mat4 m, Vec3 *position, VkrQuat *rotation,
                           Vec3 *scale) {
  *position = mat4_position(m);
  Vec3 axes[3] = {vec3_new(m.m00, m.m10, m.m20), vec3_new(m.m01, m.m11, m.m21),
                  vec3_new(m.m02, m.m12, m.m22)};
  float32_t lengths[3];
  for (uint32_t i = 0; i < 3; ++i) {
    lengths[i] = vec3_length(axes[i]);
    if (lengths[i] < 0.000001f) {
      lengths[i] = 1.0f;
    }
  }
  /* A mirrored basis keeps a proper rotation by negating one scale. */
  if (vec3_dot(vec3_cross(axes[0], axes[1]), axes[2]) < 0.0f) {
    lengths[0] = -lengths[0];
  }
  Mat4 basis = mat4_identity();
  basis.m00 = m.m00 / lengths[0];
  basis.m10 = m.m10 / lengths[0];
  basis.m20 = m.m20 / lengths[0];
  basis.m01 = m.m01 / lengths[1];
  basis.m11 = m.m11 / lengths[1];
  basis.m21 = m.m21 / lengths[1];
  basis.m02 = m.m02 / lengths[2];
  basis.m12 = m.m12 / lengths[2];
  basis.m22 = m.m22 / lengths[2];
  *rotation = vkr_quat_normalize(mat4_to_quat(basis));
  *scale = vec3_new(lengths[0], lengths[1], lengths[2]);
}

/* Parent and local transform, applied together so the entity stays put. */
static bool8_t edit_set_parent(VkrScene *scene, VkrEntityId entity,
                               VkrEntityId parent, Vec3 position,
                               VkrQuat rotation, Vec3 scale) {
  if (!vkr_scene_set_transform(scene, entity, position, rotation, scale)) {
    return false_v;
  }
  vkr_scene_set_parent(scene, entity, parent);
  const SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  return transform && transform->parent.u64 == parent.u64;
}

static bool8_t edit_structure_append(VkrSceneEditState *s, VkrEntityId entity,
                                     const EditStructure *structure) {
  EditStructure *payload = edit_journal_prepare(s, entity, sizeof(*payload));
  if (!payload) {
    snprintf(s->status, sizeof(s->status), "Out of memory for undo history.");
    return false_v;
  }
  *payload = *structure;
  edit_journal_append(
      s, (VkrSceneEditEntry){.entity = entity,
                             .kind = VKR_SCENE_EDIT_ENTRY_STRUCTURE,
                             .payload = payload,
                             .payload_size = sizeof(*payload)});
  return true_v;
}

/* The structure payloads are large; one reused scratch value keeps them off
   the UI thread's stack. Edits never nest. */
static EditStructure s_edit_structure;

bool8_t vkr_scene_edit_add_component(VkrSceneEditState *s, VkrScene *scene,
                                     VkrEntityId entity,
                                     const VkrTypeDesc *type,
                                     const void *value) {
  if (!edit_world_type(type) || !vkr_scene_entity_alive(scene, entity)) {
    snprintf(s->status, sizeof(s->status), "Unknown component or object.");
    return false_v;
  }
  if (vkr_scene_get_typed(scene, entity, type)) {
    snprintf(s->status, sizeof(s->status), "The object already has %s.",
             type->label);
    return false_v;
  }
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_ADD_COMPONENT;
  structure->type = type;
  if (value) {
    MemCopy(structure->component, value, type->size);
  } else {
    vkr_type_defaults(type, structure->component);
  }
  char error[160] = {0};
  if (!vkr_type_validate(type, structure->component, error, sizeof(error)) ||
      !vkr_scene_set_typed(scene, entity, type, structure->component)) {
    snprintf(s->status, sizeof(s->status), "%s",
             error[0] ? error : "The component could not be added.");
    return false_v;
  }
  if (!edit_structure_append(s, entity, structure)) {
    (void)vkr_scene_remove_typed(scene, entity, type);
    return false_v;
  }
  snprintf(s->status, sizeof(s->status), "Added %s.", type->label);
  return true_v;
}

bool8_t vkr_scene_edit_remove_component(VkrSceneEditState *s, VkrScene *scene,
                                        VkrEntityId entity,
                                        const VkrTypeDesc *type) {
  const void *value =
      edit_world_type(type) ? vkr_scene_get_typed(scene, entity, type) : NULL;
  if (!value) {
    snprintf(s->status, sizeof(s->status), "No such component.");
    return false_v;
  }
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_REMOVE_COMPONENT;
  structure->type = type;
  MemCopy(structure->component, value, type->size);
  /* Record first: removal cannot fail once the component exists. */
  if (!edit_structure_append(s, entity, structure)) {
    return false_v;
  }
  (void)vkr_scene_remove_typed(scene, entity, type);
  snprintf(s->status, sizeof(s->status), "Removed %s.", type->label);
  return true_v;
}

bool8_t vkr_scene_edit_replace_component(VkrSceneEditState *s, VkrScene *scene,
                                         VkrEntityId entity,
                                         const VkrTypeDesc *replaced,
                                         const VkrTypeDesc *type,
                                         const void *value) {
  const void *current = edit_world_type(replaced)
                            ? vkr_scene_get_typed(scene, entity, replaced)
                            : NULL;
  if (!current || !edit_world_type(type) || type == replaced) {
    snprintf(s->status, sizeof(s->status), "No such component.");
    return false_v;
  }
  if (vkr_scene_get_typed(scene, entity, type)) {
    snprintf(s->status, sizeof(s->status), "The object already has %s.",
             type->label);
    return false_v;
  }
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_REPLACE_COMPONENT;
  structure->replaced = replaced;
  MemCopy(structure->replaced_component, current, replaced->size);
  structure->type = type;
  if (value) {
    MemCopy(structure->component, value, type->size);
  } else {
    vkr_type_defaults(type, structure->component);
  }
  char error[160] = {0};
  if (!vkr_type_validate(type, structure->component, error, sizeof(error)) ||
      !vkr_scene_set_typed(scene, entity, type, structure->component)) {
    snprintf(s->status, sizeof(s->status), "%s",
             error[0] ? error : "The component could not be added.");
    return false_v;
  }
  if (!edit_structure_append(s, entity, structure)) {
    (void)vkr_scene_remove_typed(scene, entity, type);
    return false_v;
  }
  /* Removal cannot fail once the component exists. */
  (void)vkr_scene_remove_typed(scene, entity, replaced);
  snprintf(s->status, sizeof(s->status), "Replaced %s with %s.",
           replaced->label, type->label);
  return true_v;
}

/* Snapshot of a new entity described by edit values. */
static bool8_t edit_object_from_values(const VkrSceneEditValues *v,
                                       EditObject *o) {
  MemZero(o, sizeof(*o));
  if (!v->fields || !vkr_scene_edit_validate(v) ||
      (v->fields & VKR_SCENE_EDIT_PHYSICS)) {
    return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_NAME) {
    MemCopy(o->name, v->name, sizeof(o->name));
    o->parts |= EDIT_OBJECT_NAME;
  }
  o->transform.position = v->position;
  o->transform.rotation = vkr_quat_identity();
  o->transform.scale = vec3_one();
  if (v->fields & VKR_SCENE_EDIT_TRANSFORM) {
    o->transform.rotation = vkr_quat_normalize(v->rotation);
    o->transform.scale = v->scale;
  } else {
    o->transform.position = vec3_zero();
  }
  o->transform.local = mat4_identity();
  o->transform.world = mat4_identity();
  o->transform.trs_editable = true_v;
  o->transform.flags =
      SCENE_TRANSFORM_DIRTY_LOCAL | SCENE_TRANSFORM_DIRTY_WORLD;
  o->parts |= EDIT_OBJECT_TRANSFORM | EDIT_OBJECT_VISIBILITY;
  o->visibility =
      (v->fields & VKR_SCENE_EDIT_VISIBILITY)
          ? v->visibility
          : (SceneVisibility){.visible = true_v, .inherit_parent = true_v};
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT) {
    o->point_light = v->point_light;
    o->parts |= EDIT_OBJECT_POINT_LIGHT;
  }
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    o->directional_light = v->directional_light;
    o->parts |= EDIT_OBJECT_DIRECTIONAL_LIGHT;
  }
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    o->rectangle_light = v->rectangle_light;
    o->parts |= EDIT_OBJECT_RECTANGLE_LIGHT;
  }
  if (v->fields & VKR_SCENE_EDIT_COMPONENT) {
    if (!vkr_scene_world_type_live(v->component_type)) {
      return false_v;
    }
    o->types[0] = v->component_type;
    MemCopy(o->components[0], v->component, v->component_type->size);
    o->component_count = 1u;
  }
  return true_v;
}

VkrEntityId vkr_scene_edit_create(VkrSceneEditState *s, VkrScene *scene,
                                  VkrEntityId parent,
                                  const VkrSceneEditValues *values) {
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_CREATE;
  if ((parent.u64 && !vkr_scene_get_transform(scene, parent)) ||
      !edit_object_from_values(values, &structure->object)) {
    snprintf(s->status, sizeof(s->status), "Invalid object values.");
    return VKR_ENTITY_ID_INVALID;
  }
  structure->object.created_id = Max(1u, s->next_created_id);
  structure->object.transform.parent = parent;
  const VkrEntityId entity =
      edit_object_restore(scene, &structure->object, parent);
  if (!entity.u64) {
    snprintf(s->status, sizeof(s->status), "The object could not be created.");
    return VKR_ENTITY_ID_INVALID;
  }
  if (!edit_created_add(s, entity, structure->object.created_id) ||
      !edit_structure_append(s, entity, structure)) {
    if (s->created_count &&
        s->created[s->created_count - 1u].entity.u64 == entity.u64) {
      s->created_count--;
    }
    vkr_scene_destroy_entity(scene, entity);
    return VKR_ENTITY_ID_INVALID;
  }
  snprintf(s->status, sizeof(s->status), "Created %s.",
           structure->object.name[0] ? structure->object.name : "an object");
  return entity;
}

bool8_t vkr_scene_edit_delete(VkrSceneEditState *s, VkrScene *scene,
                              VkrEntityId entity) {
  const char *reason = NULL;
  if (!vkr_scene_edit_can_delete(scene, entity, &reason)) {
    snprintf(s->status, sizeof(s->status), "%s", reason);
    return false_v;
  }
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_DELETE;
  edit_object_capture(s, scene, entity, &structure->object);
  const bool8_t document = (structure->object.parts & EDIT_OBJECT_SOURCE) &&
                           !structure->object.created_id;
  if ((document && !edit_deleted_add(s, &structure->object.source)) ||
      !edit_structure_append(s, entity, structure)) {
    if (document) {
      edit_deleted_remove(s, &structure->object.source);
    }
    return false_v;
  }
  vkr_scene_destroy_entity(scene, entity);
  snprintf(s->status, sizeof(s->status), "Deleted %s.",
           structure->object.name[0] ? structure->object.name : "the object");
  return true_v;
}

bool8_t vkr_scene_edit_reparent(VkrSceneEditState *s, VkrScene *scene,
                                VkrEntityId entity, VkrEntityId parent) {
  SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  if (!transform || !transform->trs_editable ||
      (parent.u64 && (!vkr_scene_get_transform(scene, parent) ||
                      parent.parts.world != entity.parts.world))) {
    snprintf(s->status, sizeof(s->status),
             "Only placed objects in the same scene can be parented.");
    return false_v;
  }
  for (VkrEntityId at = parent; at.u64;) {
    const SceneTransform *link = vkr_scene_get_transform(scene, at);
    if (at.u64 == entity.u64) {
      snprintf(s->status, sizeof(s->status),
               "An object cannot be parented under itself.");
      return false_v;
    }
    at = link ? link->parent : VKR_ENTITY_ID_INVALID;
  }
  if (transform->parent.u64 == parent.u64) {
    return true_v;
  }
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  structure->op = EDIT_STRUCTURE_REPARENT;
  structure->parent[0] = transform->parent;
  structure->parent[1] = parent;
  structure->position[0] = transform->position;
  structure->rotation[0] = transform->rotation;
  structure->scale[0] = transform->scale;
  const SceneTransform *parent_transform =
      parent.u64 ? vkr_scene_get_transform(scene, parent) : NULL;
  const Mat4 local =
      parent_transform
          ? mat4_mul(mat4_inverse(parent_transform->world), transform->world)
          : transform->world;
  edit_decompose(local, &structure->position[1], &structure->rotation[1],
                 &structure->scale[1]);
  if (!edit_set_parent(scene, entity, parent, structure->position[1],
                       structure->rotation[1], structure->scale[1])) {
    (void)edit_set_parent(scene, entity, structure->parent[0],
                          structure->position[0], structure->rotation[0],
                          structure->scale[0]);
    snprintf(s->status, sizeof(s->status), "The object cannot move there.");
    return false_v;
  }
  if (!edit_structure_append(s, entity, structure)) {
    (void)edit_set_parent(scene, entity, structure->parent[0],
                          structure->position[0], structure->rotation[0],
                          structure->scale[0]);
    return false_v;
  }
  snprintf(s->status, sizeof(s->status), "Moved.");
  return true_v;
}

/* Undo (after=false) or redo one structure entry. Recreation remaps ids. */
static bool8_t edit_structure_write(VkrSceneEditState *s, VkrScene *scene,
                                    VkrSceneEditEntry *entry, bool8_t after) {
  EditStructure *structure = entry->payload;
  const VkrEntityId entity = entry->entity;
  switch (structure->op) {
  case EDIT_STRUCTURE_ADD_COMPONENT:
  case EDIT_STRUCTURE_REMOVE_COMPONENT: {
    const bool8_t present =
        (structure->op == EDIT_STRUCTURE_ADD_COMPONENT) == after;
    return present ? vkr_scene_set_typed(scene, entity, structure->type,
                                         structure->component)
                   : vkr_scene_remove_typed(scene, entity, structure->type);
  }
  case EDIT_STRUCTURE_REPLACE_COMPONENT: {
    const VkrTypeDesc *add = after ? structure->type : structure->replaced;
    const VkrTypeDesc *remove = after ? structure->replaced : structure->type;
    if (!vkr_scene_set_typed(scene, entity, add,
                             after ? structure->component
                                   : structure->replaced_component)) {
      return false_v;
    }
    (void)vkr_scene_remove_typed(scene, entity, remove);
    return true_v;
  }
  case EDIT_STRUCTURE_CREATE:
  case EDIT_STRUCTURE_DELETE: {
    const bool8_t present = (structure->op == EDIT_STRUCTURE_CREATE) == after;
    const EditObject *object = &structure->object;
    const bool8_t document =
        (object->parts & EDIT_OBJECT_SOURCE) && !object->created_id;
    if (!present) {
      if (!vkr_scene_entity_alive(scene, entity) ||
          !vkr_scene_edit_can_delete(scene, entity, NULL)) {
        snprintf(s->status, sizeof(s->status),
                 "The object changed; it cannot be removed again.");
        return false_v;
      }
      if (document && !edit_deleted_add(s, &object->source)) {
        return false_v;
      }
      vkr_scene_destroy_entity(scene, entity);
      return true_v;
    }
    const VkrEntityId parent = object->transform.parent;
    const VkrEntityId restored = edit_object_restore(
        scene, object,
        vkr_scene_entity_alive(scene, parent) ? parent : VKR_ENTITY_ID_INVALID);
    if (!restored.u64) {
      snprintf(s->status, sizeof(s->status), "The object could not return.");
      return false_v;
    }
    if (document) {
      edit_deleted_remove(s, &object->source);
    }
    edit_remap(s, entity, restored);
    return true_v;
  }
  case EDIT_STRUCTURE_REPARENT: {
    const uint32_t i = after ? 1u : 0u;
    return edit_set_parent(scene, entity, structure->parent[i],
                           structure->position[i], structure->rotation[i],
                           structure->scale[i]);
  }
  }
  return false_v;
}

bool8_t vkr_scene_edit_undo(VkrSceneEditState *s, VkrScene *scene,
                            bool8_t redo) {
  if (redo ? s->undo_cursor == s->undo_count : s->undo_cursor == 0u)
    return false_v;
  s->gesture = 0u;
  VkrSceneEditEntry *entry =
      &s->undo[redo ? s->undo_cursor : s->undo_cursor - 1u];
  if (entry->kind == VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH) {
    if (!edit_physics_batch_write(
            s, scene, entry->payload,
            (uint32_t)(entry->payload_size / sizeof(EditPhysicsBatchRecord)),
            redo)) {
      return false_v;
    }
  } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_SCENE_SETTINGS) {
    const VkrSceneSettings *payload = entry->payload;
    scene->settings = payload[redo ? 1 : 0];
    scene->world_revision++;
  } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_STRUCTURE) {
    if (!edit_structure_write(s, scene, entry, redo)) {
      return false_v;
    }
  } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_COLLISION_LAYERS) {
    const VkrSceneCollisionLayers *payload = entry->payload;
    const char *error = NULL;
    if (!vkr_scene_collision_layers_apply(scene, &payload[redo ? 1 : 0],
                                          &error)) {
      snprintf(s->status, sizeof(s->status), "%s",
               error ? error : "Collision settings undo failed.");
      return false_v;
    }
  } else {
    const VkrSceneEditValues *payload = entry->payload;
    if (!edit_write(scene, entry->entity, &payload[redo ? 1 : 0])) {
      return false_v;
    }
  }
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

static bool8_t write_source(VkrJsonWriter *w,
                            const SceneSourceIdentity *source) {
  char fingerprint[17];
  snprintf(fingerprint, sizeof(fingerprint), "%016llx",
           (unsigned long long)source->source_fingerprint);
  return vkr_json_writer_begin_object(w) &&
         vkr_json_writer_name(w, string8_lit("scene_entity")) &&
         vkr_json_writer_i64(w, source->scene_entity_index) &&
         vkr_json_writer_name(w, string8_lit("gltf_node")) &&
         vkr_json_writer_i64(w, (int32_t)source->gltf_node_index) &&
         vkr_json_writer_name(w, string8_lit("fingerprint")) &&
         vkr_json_writer_string(w,
                                string8_create((uint8_t *)fingerprint, 16)) &&
         vkr_json_writer_end_object(w);
}

static bool8_t
write_attachment_and_joints(VkrJsonWriter *w,
                            const VkrScenePhysicsSnapshot *body) {
  const VkrScenePhysicsAttachment *attachment = &body->attachment;
  if (!vkr_json_writer_name(w, string8_lit("attachment")) ||
      !vkr_json_writer_begin_object(w) ||
      !vkr_json_writer_name(w, string8_lit("enabled")) ||
      !vkr_json_writer_bool(w, attachment->enabled) ||
      !vkr_json_writer_name(w, string8_lit("source")) ||
      !write_source(w, &attachment->animation_source) ||
      !vkr_json_writer_name(w, string8_lit("node")) ||
      !vkr_json_writer_i64(w, attachment->source_node) ||
      !vkr_json_writer_name(w, string8_lit("drive_bone")) ||
      !vkr_json_writer_bool(w, attachment->drive_bone) ||
      !json_floats(w, "position", &attachment->position.x, 3) ||
      !json_floats(w, "rotation", &attachment->rotation.x, 4) ||
      !vkr_json_writer_end_object(w) ||
      !vkr_json_writer_name(w, string8_lit("joints")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < body->joint_count; ++i) {
    const VkrSceneJointConfig *joint = &body->joints[i];
    char id[17];
    snprintf(id, sizeof(id), "%016llx", (unsigned long long)joint->authored_id);
    const float32_t limits[] = {joint->min_limit, joint->max_limit,
                                joint->swing_normal_limit,
                                joint->swing_plane_limit};
    if (!vkr_json_writer_begin_object(w) ||
        !vkr_json_writer_name(w, string8_lit("id")) ||
        !vkr_json_writer_string(w, string8_create((uint8_t *)id, 16)) ||
        !vkr_json_writer_name(w, string8_lit("type")) ||
        !vkr_json_writer_i64(w, joint->type) ||
        !vkr_json_writer_name(w, string8_lit("enabled")) ||
        !vkr_json_writer_bool(w, joint->enabled) ||
        !vkr_json_writer_name(w, string8_lit("target")) ||
        !write_source(w, &joint->target_source) ||
        !json_floats(w, "anchor_a", &joint->anchor_a.x, 3) ||
        !json_floats(w, "anchor_b", &joint->anchor_b.x, 3) ||
        !json_floats(w, "axis_a", &joint->axis_a.x, 3) ||
        !json_floats(w, "axis_b", &joint->axis_b.x, 3) ||
        !json_floats(w, "normal_a", &joint->normal_a.x, 3) ||
        !json_floats(w, "normal_b", &joint->normal_b.x, 3) ||
        !json_floats(w, "limits", limits, ArrayCount(limits)) ||
        !vkr_json_writer_end_object(w)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(w);
}

static bool8_t write_collision_layers(VkrJsonWriter *w,
                                      const VkrSceneCollisionLayers *settings) {
  if (!vkr_json_writer_begin_object(w) ||
      !vkr_json_writer_name(w, string8_lit("version")) ||
      !vkr_json_writer_i64(w, 1) ||
      !vkr_json_writer_name(w, string8_lit("names")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if (!vkr_json_writer_string(w,
                                string8_create((uint8_t *)settings->names[i],
                                               strlen(settings->names[i])))) {
      return false_v;
    }
  }
  if (!vkr_json_writer_end_array(w) ||
      !vkr_json_writer_name(w, string8_lit("matrix")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if (!vkr_json_writer_i64(w, settings->matrix[i])) {
      return false_v;
    }
  }
  if (!vkr_json_writer_end_array(w) ||
      !vkr_json_writer_name(w, string8_lit("presets")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < settings->preset_count; ++i) {
    const VkrSceneCollisionPreset *preset = &settings->presets[i];
    if (!vkr_json_writer_begin_object(w) ||
        !vkr_json_writer_name(w, string8_lit("name")) ||
        !vkr_json_writer_string(
            w, string8_create((uint8_t *)preset->name, strlen(preset->name))) ||
        !vkr_json_writer_name(w, string8_lit("membership")) ||
        !vkr_json_writer_i64(w, preset->membership) ||
        !vkr_json_writer_name(w, string8_lit("mask")) ||
        !vkr_json_writer_i64(w, preset->mask) ||
        !vkr_json_writer_name(w, string8_lit("sensor")) ||
        !vkr_json_writer_bool(w, preset->sensor) ||
        !vkr_json_writer_end_object(w)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(w) && vkr_json_writer_end_object(w);
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
        !WRITE_BOOL("point_enabled", p->enabled) ||
        !WRITE_BOOL("point_casts_shadow", p->casts_shadow))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    const SceneDirectionalLight *p = &v->directional_light;
    if (!json_floats(w, "directional_color", &p->color.x, 3) ||
        !json_floats(w, "directional_direction", &p->direction_local.x, 3) ||
        !json_floats(w, "directional_intensity", &p->intensity, 1) ||
        !json_floats(w, "directional_sun_angular_diameter_degrees",
                     &p->sun_angular_diameter_degrees, 1) ||
        !json_floats(w, "directional_temperature_kelvin",
                     &p->temperature_kelvin, 1) ||
        !WRITE_BOOL("directional_enabled", p->enabled) ||
        !WRITE_BOOL("directional_atmosphere_sun", p->atmosphere_sun))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    const SceneRectangleLight *p = &v->rectangle_light;
    if (!json_floats(w, "rectangle_color", &p->color.x, 3) ||
        !json_floats(w, "rectangle_radiance", &p->radiance, 1) ||
        !json_floats(w, "rectangle_size", &p->size.x, 2) ||
        !WRITE_BOOL("rectangle_enabled", p->enabled))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_PHYSICS) {
    const VkrScenePhysicsSnapshot *p = &v->physics;
    const float32_t params[] = {
        p->body.mass,           p->body.friction,
        p->body.restitution,    p->body.gravity_factor,
        p->body.linear_damping, p->body.angular_damping};
    if (!vkr_json_writer_name(w, string8_lit("physics")) ||
        !vkr_json_writer_begin_object(w) || !WRITE_INT("version", 2) ||
        !WRITE_BOOL("present", p->present) ||
        !WRITE_INT("motion", p->body.motion) ||
        !WRITE_INT("layer", p->collision_layer) ||
        !WRITE_INT("mask", p->collision_mask) ||
        !json_floats(w, "parameters", params, ArrayCount(params)) ||
        !WRITE_BOOL("enabled", p->body.enabled) ||
        !WRITE_BOOL("sleep", p->body.allow_sleep) ||
        !WRITE_BOOL("continuous", p->body.continuous) ||
        !WRITE_BOOL("sensor", p->body.sensor) ||
        !write_attachment_and_joints(w, p) ||
        !vkr_json_writer_name(w, string8_lit("colliders")) ||
        !vkr_json_writer_begin_array(w)) {
      return false_v;
    }
    for (uint32_t i = 0; i < p->collider_count; ++i) {
      const VkrSceneColliderConfig *c = &p->colliders[i];
      char id[17];
      snprintf(id, sizeof(id), "%016llx", (unsigned long long)c->authored_id);
      const float32_t dimensions[] = {c->half_extent.x, c->half_extent.y,
                                      c->half_extent.z, c->radius,
                                      c->half_height};
      if (!vkr_json_writer_begin_object(w) ||
          !vkr_json_writer_name(w, string8_lit("id")) ||
          !vkr_json_writer_string(w, string8_create((uint8_t *)id, 16)) ||
          !WRITE_INT("shape", c->shape) || !WRITE_BOOL("enabled", c->enabled) ||
          !json_floats(w, "position", &c->position.x, 3) ||
          !json_floats(w, "rotation", &c->rotation.x, 4) ||
          !json_floats(w, "dimensions", dimensions, ArrayCount(dimensions)) ||
          !json_floats(w, "scale", &c->scale.x, 3) ||
          !vkr_json_writer_name(w, string8_lit("asset")) ||
          !vkr_json_writer_string(w,
                                  (String8){.str = (uint8_t *)c->asset_path,
                                            .length = strlen(c->asset_path)}) ||
          !vkr_json_writer_end_object(w)) {
        return false_v;
      }
    }
    if (!vkr_json_writer_end_array(w) || !vkr_json_writer_end_object(w)) {
      return false_v;
    }
  }
  return true_v;
}

/* The document ids in document order (version 5), so a load after the
   document is reordered maps each saved index to the entity's current one.
   Omitted when the document has none. */
static bool8_t write_document_ids(VkrJsonWriter *w, const VkrScene *scene) {
  if (!scene->document_id_count) {
    return true_v;
  }
  if (!vkr_json_writer_name(w, string8_lit("document_ids")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < scene->document_id_count; ++i) {
    char text[37];
    vkr_scene_document_id_format(&scene->document_ids[i], text);
    if (!vkr_json_writer_string(w, string8_create((uint8_t *)text, 36))) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(w);
}

/* Every world component the entity carries, keyed by type name; the overlay
   records final values, so unchanged components round-trip unchanged. The
   map is always written, so a removed component stays removed (version 4). */
static bool8_t write_components(VkrJsonWriter *w, const VkrScene *scene,
                                VkrEntityId entity) {
  if (!vkr_json_writer_name(w, string8_lit("components")) ||
      !vkr_json_writer_begin_object(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < scene->type_count; ++i) {
    const VkrTypeDesc *type = scene->types[i].type;
    const void *value =
        vkr_entity_get_component(scene->world, entity, scene->types[i].id);
    if (!value) {
      continue;
    }
    if (!vkr_json_writer_name(
            w, string8_create_from_cstr((const uint8_t *)type->name,
                                        strlen(type->name))) ||
        !vkr_type_write_json(w, type, value)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_object(w);
}

/* "parent": null for a root, {"created": id} for an editor-created parent or
   the parent's source identity; omitted when the parent has neither. */
static bool8_t write_parent(VkrJsonWriter *w, const VkrSceneEditState *s,
                            const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (!transform) {
    return true_v;
  }
  const VkrEntityId parent = transform->parent;
  if (!parent.u64 || !vkr_scene_entity_alive(scene, parent)) {
    return vkr_json_writer_name(w, string8_lit("parent")) &&
           vkr_json_writer_null(w);
  }
  const uint32_t created = edit_created_id(s, parent);
  if (created) {
    return vkr_json_writer_name(w, string8_lit("parent")) &&
           vkr_json_writer_begin_object(w) && WRITE_INT("created", created) &&
           vkr_json_writer_end_object(w);
  }
  const SceneSourceIdentity *source = vkr_entity_get_component(
      scene->world, parent, scene->comp_source_identity);
  if (!source) {
    return true_v;
  }
  return vkr_json_writer_name(w, string8_lit("parent")) &&
         vkr_json_writer_begin_object(w) &&
         WRITE_INT("scene_entity", source->scene_entity_index) &&
         WRITE_INT("gltf_node", (int32_t)source->gltf_node_index) &&
         vkr_json_writer_end_object(w);
}

static bool8_t write_identity(VkrJsonWriter *w,
                              const SceneSourceIdentity *source) {
  char hash[17];
  snprintf(hash, sizeof(hash), "%016llx",
           (unsigned long long)source->source_fingerprint);
  return WRITE_INT("scene_entity", source->scene_entity_index) &&
         WRITE_INT("gltf_node", (int32_t)source->gltf_node_index) &&
         vkr_json_writer_name(w, string8_lit("source_fingerprint")) &&
         vkr_json_writer_string(w, string8_create((uint8_t *)hash, 16));
}

/* Deleted document entities, then editor-created entities with their
   overlay ids. Dead created entities (undone creations) are skipped. */
static bool8_t write_structure(VkrJsonWriter *w, const VkrSceneEditState *s,
                               const VkrScene *scene) {
  for (uint32_t i = 0; i < s->deleted_count; ++i) {
    if (!vkr_json_writer_begin_object(w) ||
        !write_identity(w, &s->deleted[i]) ||
        !vkr_json_writer_name(w, string8_lit("deleted")) ||
        !vkr_json_writer_bool(w, true_v) || !vkr_json_writer_end_object(w)) {
      return false_v;
    }
  }
  if (!vkr_json_writer_end_array(w) ||
      !vkr_json_writer_name(w, string8_lit("created")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < s->created_count; ++i) {
    const VkrEntityId entity = s->created[i].entity;
    VkrSceneEditValues values;
    if (!vkr_scene_entity_alive(scene, entity)) {
      continue;
    }
    if (!vkr_scene_edit_read(scene, entity, &values)) {
      return false_v;
    }
    /* Created entities carry no physics body; the empty snapshot is noise. */
    values.fields &= ~(uint32_t)VKR_SCENE_EDIT_PHYSICS;
    if (!vkr_json_writer_begin_object(w) ||
        !WRITE_INT("id", s->created[i].id) ||
        !write_parent(w, s, scene, entity) || !write_values(w, &values) ||
        !write_components(w, scene, entity) || !vkr_json_writer_end_object(w)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(w);
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
  if (!vkr_json_writer_begin_object(w) || !WRITE_INT("version", 5) ||
      !write_document_ids(w, scene) ||
      !vkr_json_writer_name(w, string8_lit("overrides")) ||
      !vkr_json_writer_begin_array(w))
    goto failed;
  for (uint32_t i = 0; i < s->touched_count; i++) {
    VkrEntityId entity = s->touched[i];
    VkrSceneEditValues values;
    /* Deleted entities are written by identity and created ones with the
       created entities below. */
    if (!vkr_scene_entity_alive(scene, entity) || edit_created_id(s, entity))
      continue;
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, entity, scene->comp_source_identity);
    if (!source || !vkr_scene_edit_read(scene, entity, &values))
      goto failed;
    if (!vkr_json_writer_begin_object(w) || !write_identity(w, source) ||
        !write_parent(w, s, scene, entity) || !write_values(w, &values) ||
        !write_components(w, scene, entity) || !vkr_json_writer_end_object(w))
      goto failed;
  }
  VkrSceneCollisionLayers settings;
  vkr_scene_collision_layers_read(scene, &settings);
  if (!write_structure(w, s, scene) ||
      !vkr_json_writer_name(w, string8_lit("scene_settings")) ||
      !vkr_json_writer_begin_object(w) ||
      !WRITE_BOOL("inherit_world", scene->settings.inherit_world) ||
      !vkr_json_writer_end_object(w) ||
      !vkr_json_writer_name(w, string8_lit("collision_settings")) ||
      !write_collision_layers(w, &settings) || !vkr_json_writer_end_object(w) ||
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
  /* Saved document index to current index, from a version 5 overlay's
     document ids; NULL binds saved indices directly. */
  const uint32_t *remap;
  uint32_t remap_count;
} EditJson;

/* The current index of a saved document entity index; UINT32_MAX when the
   saved entity is no longer in the document. */
static uint32_t edit_json_document_index(const EditJson *j, int64_t saved) {
  if (!j->remap) {
    return (uint32_t)saved;
  }
  return saved >= 0 && (uint64_t)saved < j->remap_count ? j->remap[saved]
                                                        : UINT32_MAX;
}

typedef enum EditSidecarFailure {
  EDIT_SIDECAR_FAILURE_READ,
  EDIT_SIDECAR_FAILURE_SCHEMA,
  EDIT_SIDECAR_FAILURE_ALLOC,
  EDIT_SIDECAR_FAILURE_SOURCE_MISSING,
  EDIT_SIDECAR_FAILURE_SOURCE_AMBIGUOUS,
  EDIT_SIDECAR_FAILURE_SOURCE_DUPLICATE,
  EDIT_SIDECAR_FAILURE_FINGERPRINT,
  EDIT_SIDECAR_FAILURE_FIELDS,
} EditSidecarFailure;

typedef struct EditSidecarDiagnostic {
  EditSidecarFailure failure;
  uint32_t record;
  uint32_t wrapper;
  uint32_t node;
  uint64_t saved_fingerprint;
  uint64_t current_fingerprint;
} EditSidecarDiagnostic;

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

static bool8_t edit_json_key(EditJson *j, const char *expected, bool8_t comma) {
  char key[32];
  return (!comma || edit_json_take(j, ',')) &&
         edit_json_string(j, key, sizeof(key)) && !strcmp(key, expected) &&
         edit_json_take(j, ':');
}

static bool8_t edit_json_u64_hex(EditJson *j, uint64_t *value) {
  char text[17];
  if (!edit_json_string(j, text, sizeof(text)) || strlen(text) != 16) {
    return false_v;
  }
  *value = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    const int32_t digit = hex_digit((uint8_t)text[i]);
    if (digit < 0) {
      return false_v;
    }
    *value = (*value << 4u) | (uint32_t)digit;
  }
  return true_v;
}

static bool8_t edit_json_source(EditJson *j, SceneSourceIdentity *source) {
  int64_t value;
  if (!edit_json_take(j, '{') || !edit_json_key(j, "scene_entity", false_v) ||
      !edit_json_int(j, 0, UINT32_MAX, &value)) {
    return false_v;
  }
  source->scene_entity_index = edit_json_document_index(j, value);
  if (!edit_json_key(j, "gltf_node", true_v) ||
      !edit_json_int(j, -1, INT32_MAX, &value)) {
    return false_v;
  }
  source->gltf_node_index = (uint32_t)value;
  return edit_json_key(j, "fingerprint", true_v) &&
         edit_json_u64_hex(j, &source->source_fingerprint) &&
         edit_json_take(j, '}');
}

static bool8_t edit_json_attachment_joints(EditJson *j,
                                           VkrScenePhysicsSnapshot *body) {
  VkrScenePhysicsAttachment *attachment = &body->attachment;
  int64_t integer;
  if (!edit_json_key(j, "attachment", true_v) || !edit_json_take(j, '{') ||
      !edit_json_key(j, "enabled", false_v) ||
      !edit_json_bool(j, &attachment->enabled) ||
      !edit_json_key(j, "source", true_v) ||
      !edit_json_source(j, &attachment->animation_source) ||
      !edit_json_key(j, "node", true_v) ||
      !edit_json_int(j, 0, UINT32_MAX, &integer)) {
    return false_v;
  }
  attachment->source_node = (uint32_t)integer;
  if (!edit_json_key(j, "drive_bone", true_v) ||
      !edit_json_bool(j, &attachment->drive_bone) ||
      !edit_json_key(j, "position", true_v) ||
      !edit_json_floats(j, &attachment->position.x, 3) ||
      !edit_json_key(j, "rotation", true_v) ||
      !edit_json_floats(j, &attachment->rotation.x, 4) ||
      !edit_json_take(j, '}') || !edit_json_key(j, "joints", true_v) ||
      !edit_json_take(j, '[')) {
    return false_v;
  }
  if (!edit_json_take(j, ']')) {
    do {
      if (body->joint_count == VKR_SCENE_PHYSICS_MAX_JOINTS) {
        return false_v;
      }
      VkrSceneJointConfig *joint = &body->joints[body->joint_count++];
      float32_t limits[4];
      if (!edit_json_take(j, '{') || !edit_json_key(j, "id", false_v) ||
          !edit_json_u64_hex(j, &joint->authored_id) ||
          !edit_json_key(j, "type", true_v) ||
          !edit_json_int(j, VKR_PHYSICS_JOINT_FIXED,
                         VKR_PHYSICS_JOINT_SWING_TWIST, &integer)) {
        return false_v;
      }
      joint->type = (VkrPhysicsJointType)integer;
      if (!edit_json_key(j, "enabled", true_v) ||
          !edit_json_bool(j, &joint->enabled) ||
          !edit_json_key(j, "target", true_v) ||
          !edit_json_source(j, &joint->target_source) ||
          !edit_json_key(j, "anchor_a", true_v) ||
          !edit_json_floats(j, &joint->anchor_a.x, 3) ||
          !edit_json_key(j, "anchor_b", true_v) ||
          !edit_json_floats(j, &joint->anchor_b.x, 3) ||
          !edit_json_key(j, "axis_a", true_v) ||
          !edit_json_floats(j, &joint->axis_a.x, 3) ||
          !edit_json_key(j, "axis_b", true_v) ||
          !edit_json_floats(j, &joint->axis_b.x, 3) ||
          !edit_json_key(j, "normal_a", true_v) ||
          !edit_json_floats(j, &joint->normal_a.x, 3) ||
          !edit_json_key(j, "normal_b", true_v) ||
          !edit_json_floats(j, &joint->normal_b.x, 3) ||
          !edit_json_key(j, "limits", true_v) ||
          !edit_json_floats(j, limits, 4) || !edit_json_take(j, '}')) {
        return false_v;
      }
      joint->min_limit = limits[0];
      joint->max_limit = limits[1];
      joint->swing_normal_limit = limits[2];
      joint->swing_plane_limit = limits[3];
      if (edit_json_take(j, ']')) {
        break;
      }
      if (!edit_json_take(j, ',')) {
        return false_v;
      }
    } while (true_v);
  }
  return true_v;
}

static bool8_t edit_json_collision_layers(EditJson *j,
                                          VkrSceneCollisionLayers *settings) {
  int64_t integer;
  MemZero(settings, sizeof(*settings));
  if (!edit_json_take(j, '{') || !edit_json_key(j, "version", false_v) ||
      !edit_json_int(j, 1, 1, &integer) || !edit_json_key(j, "names", true_v) ||
      !edit_json_take(j, '[')) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if ((i && !edit_json_take(j, ',')) ||
        !edit_json_string(j, settings->names[i], sizeof(settings->names[i]))) {
      return false_v;
    }
  }
  if (!edit_json_take(j, ']') || !edit_json_key(j, "matrix", true_v) ||
      !edit_json_take(j, '[')) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    if ((i && !edit_json_take(j, ',')) ||
        !edit_json_int(j, 0, UINT16_MAX, &integer)) {
      return false_v;
    }
    settings->matrix[i] = (uint16_t)integer;
  }
  if (!edit_json_take(j, ']') || !edit_json_key(j, "presets", true_v) ||
      !edit_json_take(j, '[')) {
    return false_v;
  }
  if (!edit_json_take(j, ']')) {
    do {
      if (settings->preset_count == VKR_COLLISION_PRESET_CAPACITY) {
        return false_v;
      }
      VkrSceneCollisionPreset *preset =
          &settings->presets[settings->preset_count++];
      if (!edit_json_take(j, '{') || !edit_json_key(j, "name", false_v) ||
          !edit_json_string(j, preset->name, sizeof(preset->name)) ||
          !edit_json_key(j, "membership", true_v) ||
          !edit_json_int(j, 0, UINT16_MAX, &integer)) {
        return false_v;
      }
      preset->membership = (uint16_t)integer;
      if (!edit_json_key(j, "mask", true_v) ||
          !edit_json_int(j, 0, UINT16_MAX, &integer)) {
        return false_v;
      }
      preset->mask = (uint16_t)integer;
      if (!edit_json_key(j, "sensor", true_v) ||
          !edit_json_bool(j, &preset->sensor) || !edit_json_take(j, '}')) {
        return false_v;
      }
      if (edit_json_take(j, ']')) {
        break;
      }
      if (!edit_json_take(j, ',')) {
        return false_v;
      }
    } while (true_v);
  }
  return edit_json_take(j, '}') &&
         vkr_scene_collision_layers_validate(settings, NULL);
}

/* The versioned physics object has canonical field order. Every delimiter and
   field is consumed; unknown/duplicate properties cannot become partial edits.
 */
static bool8_t edit_json_physics(EditJson *j, VkrScenePhysicsSnapshot *p) {
  int64_t integer;
  int64_t version;
  float32_t params[6];
  p->attachment.rotation = vkr_quat_identity();
  if (!edit_json_take(j, '{') || !edit_json_key(j, "version", false_v) ||
      !edit_json_int(j, 1, 2, &version) ||
      !edit_json_key(j, "present", true_v) || !edit_json_bool(j, &p->present) ||
      !edit_json_key(j, "motion", true_v) ||
      !edit_json_int(j, VKR_PHYSICS_STATIC, VKR_PHYSICS_DYNAMIC, &integer)) {
    return false_v;
  }
  p->body.motion = (VkrPhysicsMotion)integer;
  if (!edit_json_key(j, "layer", true_v) ||
      !edit_json_int(j, 0, UINT16_MAX, &integer)) {
    return false_v;
  }
  p->collision_layer = (uint16_t)integer;
  if (!edit_json_key(j, "mask", true_v) ||
      !edit_json_int(j, 0, UINT16_MAX, &integer)) {
    return false_v;
  }
  p->collision_mask = (uint16_t)integer;
  if (!edit_json_key(j, "parameters", true_v) ||
      !edit_json_floats(j, params, ArrayCount(params)) ||
      !edit_json_key(j, "enabled", true_v) ||
      !edit_json_bool(j, &p->body.enabled) ||
      !edit_json_key(j, "sleep", true_v) ||
      !edit_json_bool(j, &p->body.allow_sleep) ||
      !edit_json_key(j, "continuous", true_v) ||
      !edit_json_bool(j, &p->body.continuous) ||
      !edit_json_key(j, "sensor", true_v) ||
      !edit_json_bool(j, &p->body.sensor) ||
      (version >= 2 && !edit_json_attachment_joints(j, p)) ||
      !edit_json_key(j, "colliders", true_v) || !edit_json_take(j, '[')) {
    return false_v;
  }
  p->body.mass = params[0];
  p->body.friction = params[1];
  p->body.restitution = params[2];
  p->body.gravity_factor = params[3];
  p->body.linear_damping = params[4];
  p->body.angular_damping = params[5];
  if (!edit_json_take(j, ']')) {
    do {
      if (p->collider_count == VKR_SCENE_PHYSICS_MAX_COLLIDERS) {
        return false_v;
      }
      VkrSceneColliderConfig *c = &p->colliders[p->collider_count++];
      c->scale = vec3_one();
      char id[17];
      float32_t dimensions[5];
      if (!edit_json_take(j, '{') || !edit_json_key(j, "id", false_v) ||
          !edit_json_string(j, id, sizeof(id)) || strlen(id) != 16) {
        return false_v;
      }
      for (uint32_t i = 0; i < 16; ++i) {
        const int32_t digit = hex_digit((uint8_t)id[i]);
        if (digit < 0) {
          return false_v;
        }
        c->authored_id = (c->authored_id << 4u) | (uint32_t)digit;
      }
      if (!edit_json_key(j, "shape", true_v) ||
          !edit_json_int(j, VKR_PHYSICS_BOX,
                         version >= 2 ? VKR_PHYSICS_TRIANGLE_MESH
                                      : VKR_PHYSICS_CAPSULE,
                         &integer)) {
        return false_v;
      }
      c->shape = (VkrPhysicsShape)integer;
      if (!edit_json_key(j, "enabled", true_v) ||
          !edit_json_bool(j, &c->enabled) ||
          !edit_json_key(j, "position", true_v) ||
          !edit_json_floats(j, &c->position.x, 3) ||
          !edit_json_key(j, "rotation", true_v) ||
          !edit_json_floats(j, &c->rotation.x, 4) ||
          !edit_json_key(j, "dimensions", true_v) ||
          !edit_json_floats(j, dimensions, 5) ||
          (version >= 2 &&
           (!edit_json_key(j, "scale", true_v) ||
            !edit_json_floats(j, &c->scale.x, 3) ||
            !edit_json_key(j, "asset", true_v) ||
            !edit_json_string(j, c->asset_path, sizeof(c->asset_path)))) ||
          !edit_json_take(j, '}')) {
        return false_v;
      }
      c->half_extent = vec3_new(dimensions[0], dimensions[1], dimensions[2]);
      c->radius = dimensions[3];
      c->half_height = dimensions[4];
      if (edit_json_take(j, ']')) {
        break;
      }
      if (!edit_json_take(j, ',')) {
        return false_v;
      }
    } while (true_v);
  }
  return edit_json_take(j, '}') && vkr_scene_physics_snapshot_validate(p, NULL);
}

/* World components of one overlay record, applied after its base values. */
typedef struct EditRecordComponents {
  uint32_t count;
  const VkrTypeDesc *types[EDIT_COMPONENT_MAX];
  _Alignas(16) uint8_t values[EDIT_COMPONENT_MAX][VKR_TYPE_VALUE_MAX];
} EditRecordComponents;

/* `{"<type>": {descriptor object}, ...}` read through the type descriptors;
   unknown types, repeats and invalid values reject the record. */
static bool8_t edit_json_components(EditJson *j, VkrAllocator *allocator,
                                    EditRecordComponents *out) {
  VkrJsonReader reader =
      vkr_json_reader_create(j->at, (uint64_t)(j->end - j->at));
  out->count = 0u;
  if (!edit_json_take(j, '{'))
    return false_v;
  if (edit_json_take(j, '}'))
    return true_v;
  for (;;) {
    char key[64];
    if (!edit_json_string(j, key, sizeof(key)) || !edit_json_take(j, ':') ||
        out->count == EDIT_COMPONENT_MAX)
      return false_v;
    const VkrTypeDesc *type = vkr_scene_world_type_named(
        string8_create_from_cstr((const uint8_t *)key, strlen(key)));
    if (!type)
      return false_v;
    for (uint32_t i = 0; i < out->count; ++i)
      if (out->types[i] == type)
        return false_v;
    uint8_t *value = out->values[out->count];
    vkr_type_defaults(type, value);
    edit_json_space(j);
    reader.data = j->at;
    reader.length = (uint64_t)(j->end - j->at);
    reader.pos = 0u;
    if (!vkr_type_read_json(&reader, type, value, allocator, NULL, 0u))
      return false_v;
    j->at += reader.pos;
    out->types[out->count++] = type;
    if (edit_json_take(j, '}'))
      return true_v;
    if (!edit_json_take(j, ','))
      return false_v;
  }
}

typedef enum EditParentKind {
  EDIT_PARENT_NONE, /* The record does not say. */
  EDIT_PARENT_ROOT,
  EDIT_PARENT_DOCUMENT,
  EDIT_PARENT_CREATED,
} EditParentKind;

typedef struct EditParentRef {
  EditParentKind kind;
  uint32_t wrapper;
  uint32_t node;
  uint32_t created;
} EditParentRef;

/* Version 4 record members beyond values and components. */
typedef struct EditRecordExtra {
  bool8_t deleted;
  /* The record listed its components; version 4 treats the list as whole. */
  bool8_t components;
  EditParentRef parent;
  /* Created records: the overlay id. */
  uint32_t id;
} EditRecordExtra;

/* null, {"created": id} or {"scene_entity": n, "gltf_node": n}. */
static bool8_t edit_json_parent(EditJson *j, EditParentRef *out) {
  int64_t value = 0;
  char key[32];
  edit_json_space(j);
  if (j->end - j->at >= 4 && MemCompare(j->at, "null", 4) == 0) {
    j->at += 4;
    out->kind = EDIT_PARENT_ROOT;
    return true_v;
  }
  if (!edit_json_take(j, '{') || !edit_json_string(j, key, sizeof(key)) ||
      !edit_json_take(j, ':')) {
    return false_v;
  }
  if (!strcmp(key, "created")) {
    if (!edit_json_int(j, 1, UINT32_MAX, &value)) {
      return false_v;
    }
    out->kind = EDIT_PARENT_CREATED;
    out->created = (uint32_t)value;
    return edit_json_take(j, '}');
  }
  if (strcmp(key, "scene_entity") || !edit_json_int(j, 0, UINT32_MAX, &value)) {
    return false_v;
  }
  out->wrapper = edit_json_document_index(j, value);
  if (!edit_json_key(j, "gltf_node", true_v) ||
      !edit_json_int(j, -1, INT32_MAX, &value)) {
    return false_v;
  }
  out->node = (uint32_t)value;
  out->kind = EDIT_PARENT_DOCUMENT;
  return edit_json_take(j, '}');
}

/* One override record, or with `created` one created-entity record keyed by
   "id" instead of a source identity. */
static bool8_t edit_json_record(EditJson *j, VkrSceneEditValues *v,
                                uint32_t *wrapper, uint32_t *node,
                                uint64_t *fingerprint, VkrAllocator *allocator,
                                EditRecordComponents *components,
                                EditRecordExtra *extra, bool8_t created) {
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
                               "directional_enabled",
                               "point_casts_shadow",
                               "directional_sun_angular_diameter_degrees",
                               "rectangle_color",
                               "rectangle_radiance",
                               "rectangle_size",
                               "rectangle_enabled",
                               "physics",
                               "directional_temperature_kelvin",
                               "directional_atmosphere_sun",
                               "components",
                               "deleted",
                               "parent",
                               "id"};
  MemZero(v, sizeof(*v));
  MemZero(extra, sizeof(*extra));
  components->count = 0u;
  v->directional_light.sun_angular_diameter_degrees =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;
  v->directional_light.atmosphere_sun = true_v;
  uint64_t seen = 0;
  if (!edit_json_take(j, '{'))
    return false_v;
  for (;;) {
    char key[64];
    int64_t integer = 0;
    bool8_t ok = false_v;
    if (!edit_json_string(j, key, sizeof(key)) || !edit_json_take(j, ':'))
      return false_v;
    uint32_t k = 0;
    while (k < sizeof(keys) / sizeof(*keys) && strcmp(keys[k], key))
      k++;
    if (k == sizeof(keys) / sizeof(*keys) || (seen & (1ull << k)))
      return false_v;
    seen |= 1ull << k;
    switch (k) {
    case 0:
      ok = edit_json_int(j, 0, UINT32_MAX, &integer);
      *wrapper = edit_json_document_index(j, integer);
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
      ok = edit_json_int(j, 1, 127, &integer);
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
    case 20:
      ok = edit_json_floats(
          j, &v->directional_light.sun_angular_diameter_degrees, 1);
      /* Older overlays authored up to 180 degrees; see the scene loader. */
      if (ok && v->directional_light.sun_angular_diameter_degrees >
                    VKR_DIRECTIONAL_LIGHT_MAX_SUN_ANGULAR_DIAMETER_DEGREES) {
        v->directional_light.sun_angular_diameter_degrees =
            VKR_DIRECTIONAL_LIGHT_MAX_SUN_ANGULAR_DIAMETER_DEGREES;
      }
      break;
    case 19:
      ok = edit_json_bool(j, &v->point_light.casts_shadow);
      break;
    case 18:
      ok = edit_json_bool(j, &v->directional_light.enabled);
      break;
    case 21:
      ok = edit_json_floats(j, &v->rectangle_light.color.x, 3);
      break;
    case 22:
      ok = edit_json_floats(j, &v->rectangle_light.radiance, 1);
      break;
    case 23:
      ok = edit_json_floats(j, &v->rectangle_light.size.x, 2);
      break;
    case 25:
      ok = edit_json_physics(j, &v->physics);
      break;
    case 24:
      ok = edit_json_bool(j, &v->rectangle_light.enabled);
      break;
    case 26:
      ok = edit_json_floats(j, &v->directional_light.temperature_kelvin, 1);
      break;
    case 27:
      ok = edit_json_bool(j, &v->directional_light.atmosphere_sun);
      break;
    case 28:
      ok = edit_json_components(j, allocator, components);
      extra->components = true_v;
      break;
    case 29:
      ok = edit_json_bool(j, &extra->deleted) && extra->deleted;
      break;
    case 30:
      ok = edit_json_parent(j, &extra->parent);
      break;
    case 31:
      ok = edit_json_int(j, 1, UINT32_MAX, &integer);
      extra->id = (uint32_t)integer;
      break;
    }
    if (!ok)
      return false_v;
    if (edit_json_take(j, '}'))
      break;
    if (!edit_json_take(j, ','))
      return false_v;
  }
  if (extra->deleted) {
    /* A deletion names the entity and nothing else. */
    return !created && seen == (7ull | (1ull << 29u));
  }
  uint64_t required = created ? (1ull << 3u) | (1ull << 31u) : 15u;
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
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    required |= seen & (1u << 20u); /* Old journals use the default angle. */
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    required |= seen & (1u << 26u); /* Old journals use the light's colour. */
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    required |= seen & (1u << 27u); /* Old journals keep the scene default. */
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    required |= seen & (1u << 19u); /* Old journals default shadows off. */
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT)
    required |= 15u << 21u;
  if (v->fields & VKR_SCENE_EDIT_PHYSICS) {
    required |= 1u << 25u;
  }
  required |= seen & (1ull << 28u); /* Components are optional. */
  required |= seen & (1ull << 30u); /* So is the parent. */
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

static void
edit_sidecar_reject_status(VkrSceneEditState *s,
                           const EditSidecarDiagnostic *diagnostic) {
  switch (diagnostic->failure) {
  case EDIT_SIDECAR_FAILURE_SCHEMA:
    snprintf(s->status, sizeof(s->status),
             "Override file is malformed or uses an unsupported schema.");
    break;
  case EDIT_SIDECAR_FAILURE_ALLOC:
    snprintf(s->status, sizeof(s->status),
             "Override file could not stage all changes.");
    break;
  case EDIT_SIDECAR_FAILURE_SOURCE_MISSING:
    snprintf(s->status, sizeof(s->status),
             "Override %u targets missing source %u/%d.", diagnostic->record,
             diagnostic->wrapper, (int32_t)diagnostic->node);
    break;
  case EDIT_SIDECAR_FAILURE_SOURCE_AMBIGUOUS:
    snprintf(s->status, sizeof(s->status),
             "Override %u targets ambiguous source %u/%d.", diagnostic->record,
             diagnostic->wrapper, (int32_t)diagnostic->node);
    break;
  case EDIT_SIDECAR_FAILURE_SOURCE_DUPLICATE:
    snprintf(s->status, sizeof(s->status), "Override %u repeats source %u/%d.",
             diagnostic->record, diagnostic->wrapper,
             (int32_t)diagnostic->node);
    break;
  case EDIT_SIDECAR_FAILURE_FINGERPRINT:
    snprintf(s->status, sizeof(s->status),
             "Override %u source %u/%d fingerprint differs: saved %016llx, "
             "current %016llx.",
             diagnostic->record, diagnostic->wrapper, (int32_t)diagnostic->node,
             (unsigned long long)diagnostic->saved_fingerprint,
             (unsigned long long)diagnostic->current_fingerprint);
    break;
  case EDIT_SIDECAR_FAILURE_FIELDS:
    snprintf(s->status, sizeof(s->status),
             "Override %u fields no longer match source %u/%d.",
             diagnostic->record, diagnostic->wrapper,
             (int32_t)diagnostic->node);
    break;
  case EDIT_SIDECAR_FAILURE_READ:
    snprintf(s->status, sizeof(s->status), "Override file could not be read.");
    break;
  }
}

/* All prepared strings belong to the scene; pending records/file bytes belong
   to the editor allocator and die at this input boundary. No scene mutation
   occurs until closing delimiters, EOF, identities and every allocation pass.
 */
/* Prepare one resolved overlay record: its base values, then each world
   component as its own entry for the same entity. A version 4 component list
   is whole: listed components the entity lacks are added and unlisted live
   world components are removed. A deletion prepares only the destroy. The whole
   file still commits or fails together. */
static bool8_t edit_load_prepare_record(
    VkrSceneEditState *s, VkrScene *scene, const EditSourceIndex *source,
    VkrSceneEditValues *values, const EditRecordComponents *components,
    const EditRecordExtra *extra, bool8_t whole, EditPrepared **pending,
    uint32_t *count, uint32_t *capacity, uint32_t *touched,
    EditSidecarFailure *failure) {
  const uint32_t needed = *count + 1u + components->count + scene->type_count;
  if (needed > *capacity) {
    uint32_t next = Max(16u, *capacity * 2u);
    while (next < needed)
      next *= 2u;
    EditPrepared *entries = vkr_allocator_realloc(
        s->allocator, *pending, *capacity * sizeof(*entries),
        next * sizeof(*entries), EDIT_TAG);
    if (!entries) {
      *failure = EDIT_SIDECAR_FAILURE_ALLOC;
      return false_v;
    }
    *pending = entries;
    *capacity = next;
  }
  if (extra->deleted) {
    /* Children were deleted first, so only the parts are checked here. */
    if (!edit_deletable_parts(scene, source->entity, NULL)) {
      *failure = EDIT_SIDECAR_FAILURE_FIELDS;
      return false_v;
    }
    (*pending)[(*count)++] =
        (EditPrepared){.entity = source->entity, .op = EDIT_PREPARED_DESTROY};
    return true_v;
  }
  if (!edit_prepare(scene, source->entity, values, &(*pending)[*count])) {
    *failure = EDIT_SIDECAR_FAILURE_FIELDS;
    return false_v;
  }
  (*pending)[*count].add_touched = !source->touched;
  *touched += (*pending)[*count].add_touched;
  (*count)++;
  for (uint32_t c = 0; c < components->count; ++c) {
    const VkrTypeDesc *type = components->types[c];
    EditPrepared *prepared = &(*pending)[*count];
    values->fields = VKR_SCENE_EDIT_COMPONENT;
    values->component_type = type;
    MemCopy(values->component, components->values[c], type->size);
    if (vkr_scene_get_typed(scene, source->entity, type)) {
      if (!edit_prepare(scene, source->entity, values, prepared)) {
        *failure = EDIT_SIDECAR_FAILURE_FIELDS;
        return false_v;
      }
    } else {
      if (!whole || !vkr_scene_world_type_live(type) ||
          !vkr_scene_edit_validate(values)) {
        *failure = EDIT_SIDECAR_FAILURE_FIELDS;
        return false_v;
      }
      *prepared = (EditPrepared){.entity = source->entity,
                                 .values = *values,
                                 .op = EDIT_PREPARED_ADD_COMPONENT};
    }
    prepared->add_touched = false_v;
    (*count)++;
  }
  for (uint32_t t = 0; whole && extra->components && t < scene->type_count;
       ++t) {
    const VkrTypeDesc *type = scene->types[t].type;
    bool8_t listed = false_v;
    for (uint32_t c = 0; c < components->count; ++c) {
      listed |= components->types[c] == type;
    }
    /* Only a type the editor may remove can be removed by omission. */
    if (!listed && vkr_scene_world_type_live(type) &&
        vkr_scene_get_typed(scene, source->entity, type)) {
      EditPrepared *prepared = &(*pending)[(*count)++];
      *prepared = (EditPrepared){.entity = source->entity,
                                 .op = EDIT_PREPARED_REMOVE_COMPONENT};
      prepared->values.component_type = type;
    }
  }
  return true_v;
}

/* Claim the entity an override record names: it must exist, be unique,
   not already be claimed and match the saved fingerprint. */
static bool8_t edit_load_bind_source(EditSourceIndex *source,
                                     uint64_t fingerprint,
                                     EditSidecarFailure *failure) {
  if (!source) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_MISSING;
  } else if (source->ambiguous) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_AMBIGUOUS;
  } else if (source->seen) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_DUPLICATE;
  } else if (source->fingerprint != fingerprint) {
    *failure = EDIT_SIDECAR_FAILURE_FINGERPRINT;
  } else {
    source->seen = true_v;
    return true_v;
  }
  return false_v;
}

/* One directory walk and sort serve all records. Seen flags reject repeated
   overrides; touched flags merge an existing journal without quadratic scans.
   The index holds `capacity` entries; the caller frees it. */
static bool8_t edit_source_index_build(const VkrSceneEditState *s,
                                       const VkrScene *scene, uint32_t capacity,
                                       EditSourceIndex **out_index,
                                       uint32_t *out_count) {
  EditSourceIndex *index = NULL;
  uint32_t count = 0u;
  if (capacity) {
    index =
        vkr_allocator_alloc(s->allocator, capacity * sizeof(*index), EDIT_TAG);
    if (!index) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < capacity; ++i) {
    VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, entity, scene->comp_source_identity);
    if (source)
      index[count++] =
          (EditSourceIndex){.wrapper = source->scene_entity_index,
                            .node = source->gltf_node_index,
                            .entity = entity,
                            .fingerprint = source->source_fingerprint};
  }
  if (count > 1u) {
    qsort(index, count, sizeof(*index), edit_source_compare);
    for (uint32_t i = 1; i < count; ++i)
      if (edit_source_compare(&index[i - 1u], &index[i]) == 0)
        index[i - 1u].ambiguous = index[i].ambiguous = true_v;
  }
  for (uint32_t i = 0; i < s->touched_count; ++i) {
    const SceneSourceIdentity *source = vkr_entity_get_component(
        scene->world, s->touched[i], scene->comp_source_identity);
    if (source) {
      EditSourceIndex *entry = edit_source_find(
          index, count, source->scene_entity_index, source->gltf_node_index);
      if (entry && entry->entity.u64 == s->touched[i].u64)
        entry->touched = true_v;
    }
  }
  *out_index = index;
  *out_count = count;
  return true_v;
}

/* A changed collision matrix rebuilds every body the overlay does not
   replace itself, so each one's effective mask follows the new matrix. */
static bool8_t edit_load_prepare_matrix(VkrScene *scene,
                                        const EditPrepared *pending,
                                        uint32_t count,
                                        VkrScenePhysicsPrepared **matrix_bodies,
                                        uint32_t *matrix_body_count) {
  const uint32_t bodies = vkr_scene_physics_body_count(scene);
  for (uint32_t i = 0; i < bodies; ++i) {
    const VkrEntityId entity = vkr_scene_physics_body_at(scene, i);
    bool8_t overridden = false_v;
    for (uint32_t j = 0; j < count; ++j) {
      overridden |=
          pending[j].entity.u64 == entity.u64 && pending[j].physics != NULL;
    }
    if (overridden) {
      continue;
    }
    VkrScenePhysicsSnapshot snapshot;
    if (!vkr_scene_physics_read(scene, entity, &snapshot) ||
        !vkr_scene_physics_prepare(scene, entity, &snapshot,
                                   &matrix_bodies[*matrix_body_count], NULL)) {
      return false_v;
    }
    (*matrix_body_count)++;
  }
  return true_v;
}

/* Created entities read from a version 4 overlay, bounded like its records. */
#define EDIT_CREATED_MAX 1024u

typedef struct EditParentLink {
  /* Override records name the child entity; created records their id. */
  VkrEntityId child;
  uint32_t created_child;
  EditParentRef parent;
} EditParentLink;

/* Version 4 structure staged while the file is read and applied after every
   override commits: created entities first, then parent links. */
typedef struct EditStructureLoad {
  EditObject *objects;
  uint32_t object_count;
  uint32_t object_capacity;
  EditParentLink *links;
  uint32_t link_count;
  uint32_t link_capacity;
  /* The file used members only version 4 defines. */
  bool8_t version4;
} EditStructureLoad;

static void edit_structure_load_free(VkrSceneEditState *s,
                                     EditStructureLoad *load) {
  if (load->objects) {
    vkr_allocator_free(s->allocator, load->objects,
                       load->object_capacity * sizeof(*load->objects),
                       EDIT_TAG);
  }
  if (load->links) {
    vkr_allocator_free(s->allocator, load->links,
                       load->link_capacity * sizeof(*load->links), EDIT_TAG);
  }
  MemZero(load, sizeof(*load));
}

static bool8_t edit_structure_link(VkrSceneEditState *s,
                                   EditStructureLoad *load,
                                   EditParentLink link) {
  if (load->link_count == load->link_capacity) {
    const uint32_t capacity = Max(16u, load->link_capacity * 2u);
    EditParentLink *next = vkr_allocator_realloc(
        s->allocator, load->links, load->link_capacity * sizeof(*next),
        capacity * sizeof(*next), EDIT_TAG);
    if (!next) {
      return false_v;
    }
    load->links = next;
    load->link_capacity = capacity;
  }
  load->links[load->link_count++] = link;
  load->version4 = true_v;
  return true_v;
}

/* `[record, ...]` of created entities; ids are unique and positive. */
typedef struct EditDocumentKey {
  VkrSceneDocumentId id;
  uint32_t index;
} EditDocumentKey;

static int edit_document_key_compare(const void *left, const void *right) {
  return MemCompare(left, right, sizeof(VkrSceneDocumentId));
}

/* A version 5 overlay's document ids: the saved index of each becomes the
   index of the entity with that id in the loaded document, or UINT32_MAX
   when the document no longer has it. A document without ids binds saved
   indices directly. Installs the map in `j`; the caller frees `out` with
   `out_capacity` entries. */
static bool8_t edit_json_document_remap(EditJson *j, VkrSceneEditState *s,
                                        const VkrScene *scene, uint32_t **out,
                                        uint32_t *out_capacity,
                                        EditSidecarFailure *failure) {
  const uint32_t current = scene->document_id_count;
  EditDocumentKey *keys = NULL;
  uint32_t *remap = NULL;
  uint32_t count = 0u, capacity = 0u;
  bool8_t ok = false_v;
  if (current) {
    keys = vkr_allocator_alloc(s->allocator, current * sizeof(*keys), EDIT_TAG);
    if (!keys) {
      *failure = EDIT_SIDECAR_FAILURE_ALLOC;
      return false_v;
    }
    for (uint32_t i = 0; i < current; ++i) {
      keys[i] = (EditDocumentKey){.id = scene->document_ids[i], .index = i};
    }
    qsort(keys, current, sizeof(*keys), edit_document_key_compare);
  }
  if (!edit_json_take(j, '[')) {
    goto done;
  }
  if (!edit_json_take(j, ']')) {
    for (;;) {
      char text[40];
      VkrSceneDocumentId id;
      if (!edit_json_string(j, text, sizeof(text)) ||
          !vkr_scene_document_id_parse(
              string8_create((uint8_t *)text, strlen(text)), &id)) {
        goto done;
      }
      if (current) {
        if (count == capacity) {
          const uint32_t next = Max(64u, capacity * 2u);
          uint32_t *grown = vkr_allocator_realloc(
              s->allocator, remap, capacity * sizeof(*remap),
              next * sizeof(*remap), EDIT_TAG);
          if (!grown) {
            *failure = EDIT_SIDECAR_FAILURE_ALLOC;
            goto done;
          }
          remap = grown;
          capacity = next;
        }
        const EditDocumentKey *match = bsearch(
            &id, keys, current, sizeof(*keys), edit_document_key_compare);
        remap[count] = match ? match->index : UINT32_MAX;
      }
      count++;
      if (edit_json_take(j, ']')) {
        break;
      }
      if (!edit_json_take(j, ',')) {
        goto done;
      }
    }
  }
  ok = true_v;

done:
  if (keys) {
    vkr_allocator_free(s->allocator, keys, current * sizeof(*keys), EDIT_TAG);
  }
  if (!ok || !current) {
    if (remap) {
      vkr_allocator_free(s->allocator, remap, capacity * sizeof(*remap),
                         EDIT_TAG);
    }
    *out = NULL;
    *out_capacity = 0u;
    return ok;
  }
  *out = remap;
  *out_capacity = capacity;
  j->remap = remap;
  j->remap_count = count;
  return true_v;
}

static bool8_t edit_json_created(EditJson *j, VkrSceneEditState *s,
                                 EditStructureLoad *load,
                                 EditRecordComponents *components,
                                 EditSidecarFailure *failure) {
  *failure = EDIT_SIDECAR_FAILURE_SCHEMA;
  load->version4 = true_v;
  if (!edit_json_take(j, '['))
    return false_v;
  if (edit_json_take(j, ']'))
    return true_v;
  for (;;) {
    VkrSceneEditValues values;
    EditRecordExtra extra;
    uint32_t wrapper = 0;
    uint32_t node = 0;
    uint64_t fingerprint = 0;
    if (load->object_count == EDIT_CREATED_MAX ||
        !edit_json_record(j, &values, &wrapper, &node, &fingerprint,
                          s->allocator, components, &extra, true_v))
      return false_v;
    for (uint32_t i = 0; i < load->object_count; ++i)
      if (load->objects[i].created_id == extra.id)
        return false_v;
    if (load->object_count == load->object_capacity) {
      const uint32_t capacity = Max(8u, load->object_capacity * 2u);
      EditObject *objects = vkr_allocator_realloc(
          s->allocator, load->objects, load->object_capacity * sizeof(*objects),
          capacity * sizeof(*objects), EDIT_TAG);
      if (!objects) {
        *failure = EDIT_SIDECAR_FAILURE_ALLOC;
        return false_v;
      }
      load->objects = objects;
      load->object_capacity = capacity;
    }
    EditObject *object = &load->objects[load->object_count];
    values.fields &= ~(uint32_t)VKR_SCENE_EDIT_COMPONENT;
    if (!edit_object_from_values(&values, object))
      return false_v;
    for (uint32_t c = 0; c < components->count; ++c) {
      if (!vkr_scene_world_type_live(components->types[c]))
        return false_v;
      object->types[c] = components->types[c];
      MemCopy(object->components[c], components->values[c],
              components->types[c]->size);
    }
    object->component_count = components->count;
    object->created_id = extra.id;
    load->object_count++;
    if (extra.parent.kind != EDIT_PARENT_NONE &&
        !edit_structure_link(s, load,
                             (EditParentLink){.created_child = extra.id,
                                              .parent = extra.parent})) {
      *failure = EDIT_SIDECAR_FAILURE_ALLOC;
      return false_v;
    }
    if (edit_json_take(j, ']'))
      return true_v;
    if (!edit_json_take(j, ','))
      return false_v;
  }
}

static VkrEntityId edit_created_entity(const VkrSceneEditState *s,
                                       uint32_t id) {
  for (uint32_t i = 0; i < s->created_count; ++i) {
    if (s->created[i].id == id) {
      return s->created[i].entity;
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

/* Create the staged entities, then link parents. Runs after every override
   committed; a failure here leaves that entity out and is logged. */
static void edit_structure_load_commit(VkrSceneEditState *s, VkrScene *scene,
                                       const EditStructureLoad *load,
                                       EditSourceIndex *index,
                                       uint32_t index_count) {
  for (uint32_t i = 0; i < load->object_count; ++i) {
    const VkrEntityId entity =
        edit_object_restore(scene, &load->objects[i], VKR_ENTITY_ID_INVALID);
    const uint32_t id = load->objects[i].created_id;
    if (!entity.u64 || !edit_created_add(s, entity, id)) {
      log_warn("Overlay object %u could not be created", id);
    }
  }
  for (uint32_t i = 0; i < load->link_count; ++i) {
    const EditParentLink *link = &load->links[i];
    const VkrEntityId child = link->created_child
                                  ? edit_created_entity(s, link->created_child)
                                  : link->child;
    VkrEntityId parent = VKR_ENTITY_ID_INVALID;
    if (link->parent.kind == EDIT_PARENT_CREATED) {
      parent = edit_created_entity(s, link->parent.created);
    } else if (link->parent.kind == EDIT_PARENT_DOCUMENT) {
      const EditSourceIndex *source = edit_source_find(
          index, index_count, link->parent.wrapper, link->parent.node);
      parent = source ? source->entity : VKR_ENTITY_ID_INVALID;
    }
    if (!vkr_scene_entity_alive(scene, child)) {
      continue;
    }
    if (link->parent.kind != EDIT_PARENT_ROOT &&
        !vkr_scene_entity_alive(scene, parent)) {
      log_warn("Overlay parent of an object is missing; it stays at the root");
      parent = VKR_ENTITY_ID_INVALID;
    }
    vkr_scene_set_parent(scene, child, parent);
  }
}

bool8_t vkr_scene_edit_load(VkrSceneEditState *s, VkrScene *scene,
                            String8 path) {
  char cpath[1024];
  EditRecordComponents *components = NULL;
  FILE *file = NULL;
  uint8_t *bytes = NULL;
  EditPrepared *pending = NULL;
  VkrSceneCollisionLayers settings = {0};
  VkrSceneCollisionLayersPrepared *pending_settings = NULL;
  VkrScenePhysicsPrepared *matrix_bodies[VKR_SCENE_PHYSICS_MAX_BODIES] = {0};
  uint32_t matrix_body_count = 0;
  uint32_t count = 0, capacity = 0;
  EditSourceIndex *index = NULL;
  uint32_t *remap = NULL;
  uint32_t remap_capacity = 0u;
  uint32_t index_count = 0, additional_touched = 0;
  uint32_t index_capacity = scene->world->dir.capacity;
  uint32_t touched_before = s->touched_count;
  const uint32_t deleted_before = s->deleted_count;
  EditStructureLoad structure = {0};
  VkrSceneSettings scene_settings = {.inherit_world = true_v};
  bool8_t success = false_v;
  EditSidecarDiagnostic diagnostic = {
      .failure = EDIT_SIDECAR_FAILURE_READ,
  };
  long length = 0;
  if (path.length >= sizeof(cpath))
    goto cleanup;
  MemCopy(cpath, path.str, path.length);
  cpath[path.length] = 0;
  file = file_fopen(cpath, "rb");
  if (!file && errno == ENOENT) {
    s->sidecar_conflict = false_v;
    return true_v;
  }
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
    goto cleanup;
  bytes = vkr_allocator_alloc(s->allocator, (uint64_t)length, EDIT_TAG);
  if (!bytes) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
    goto cleanup;
  }
  components = vkr_allocator_alloc(s->allocator, sizeof(*components), EDIT_TAG);
  if (!components) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
    goto cleanup;
  }
  if (fread(bytes, 1, (size_t)length, file) != (size_t)length)
    goto cleanup;
  if (!edit_source_index_build(s, scene, index_capacity, &index,
                               &index_count)) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
    goto cleanup;
  }
  EditJson json = {.at = bytes, .end = bytes + length};
  uint32_t root_seen = 0;
  int64_t version = 0;
  if (!edit_json_take(&json, '{')) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
    goto cleanup;
  }
  for (;;) {
    char key[32];
    if (!edit_json_string(&json, key, sizeof(key)) ||
        !edit_json_take(&json, ':')) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
      goto cleanup;
    }
    if (strcmp(key, "version") == 0) {
      if ((root_seen & 1u) || !edit_json_int(&json, 1, 5, &version)) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
      root_seen |= 1u;
    } else if (strcmp(key, "overrides") == 0) {
      if ((root_seen & 2u) || !edit_json_take(&json, '[')) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
      root_seen |= 2u;
      if (!edit_json_take(&json, ']'))
        for (;;) {
          VkrSceneEditValues values;
          EditRecordExtra extra;
          uint32_t wrapper, node;
          uint64_t fingerprint;
          if (!edit_json_record(&json, &values, &wrapper, &node, &fingerprint,
                                s->allocator, components, &extra, false_v)) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
            diagnostic.record = count + 1u;
            goto cleanup;
          }
          EditSourceIndex *source =
              edit_source_find(index, index_count, wrapper, node);
          diagnostic = (EditSidecarDiagnostic){
              .record = count + 1u,
              .wrapper = wrapper,
              .node = node,
              .saved_fingerprint = fingerprint,
              .current_fingerprint = source ? source->fingerprint : 0u,
          };
          if (!edit_load_bind_source(source, fingerprint, &diagnostic.failure))
            goto cleanup;
          structure.version4 |= extra.deleted;
          if (!edit_load_prepare_record(s, scene, source, &values, components,
                                        &extra, version >= 4, &pending, &count,
                                        &capacity, &additional_touched,
                                        &diagnostic.failure))
            goto cleanup;
          if (extra.parent.kind != EDIT_PARENT_NONE &&
              !edit_structure_link(s, &structure,
                                   (EditParentLink){.child = source->entity,
                                                    .parent = extra.parent})) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
            goto cleanup;
          }
          if (edit_json_take(&json, ']'))
            break;
          if (!edit_json_take(&json, ',')) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
            goto cleanup;
          }
        }
    } else if (!strcmp(key, "document_ids")) {
      /* Records bind through the map, so it precedes every record. */
      if ((root_seen & (32u | 2u | 8u)) ||
          !edit_json_document_remap(&json, s, scene, &remap, &remap_capacity,
                                    &diagnostic.failure)) {
        if (diagnostic.failure == EDIT_SIDECAR_FAILURE_READ)
          diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
      root_seen |= 32u;
    } else if (!strcmp(key, "scene_settings")) {
      if ((root_seen & 16u) || !edit_json_take(&json, '{') ||
          !edit_json_key(&json, "inherit_world", false_v) ||
          !edit_json_bool(&json, &scene_settings.inherit_world) ||
          !edit_json_take(&json, '}')) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
      root_seen |= 16u;
      structure.version4 = true_v;
    } else if (!strcmp(key, "created")) {
      if ((root_seen & 8u) ||
          !edit_json_created(&json, s, &structure, components,
                             &diagnostic.failure)) {
        diagnostic.record = count + 1u;
        goto cleanup;
      }
      root_seen |= 8u;
    } else if (!strcmp(key, "collision_settings")) {
      if ((root_seen & 4u) || !edit_json_collision_layers(&json, &settings)) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
      root_seen |= 4u;
    } else {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
      goto cleanup;
    }
    if (edit_json_take(&json, '}'))
      break;
    if (!edit_json_take(&json, ',')) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
      goto cleanup;
    }
  }
  edit_json_space(&json);
  if ((root_seen & ~56u) != (version >= 3 ? 7u : 3u) ||
      ((root_seen & 32u) && version < 5) || json.at != json.end ||
      (structure.version4 && version < 4)) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
    goto cleanup;
  }
  if (version == 1) {
    for (uint32_t i = 0; i < count; ++i) {
      if (pending[i].values.fields & VKR_SCENE_EDIT_PHYSICS) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
        goto cleanup;
      }
    }
  }
  if (additional_touched > UINT32_MAX - touched_before) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
    goto cleanup;
  }
  const uint32_t touched_needed = touched_before + additional_touched;
  if (touched_needed > s->touched_capacity) {
    const uint32_t next_capacity = Max(32u, touched_needed);
    VkrEntityId *next = vkr_allocator_realloc(
        s->allocator, s->touched, s->touched_capacity * sizeof(*next),
        next_capacity * sizeof(*next), EDIT_TAG);
    if (!next) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
      goto cleanup;
    }
    s->touched = next;
    s->touched_capacity = next_capacity;
  }
  bool8_t rebuild_matrix = false_v;
  /* In a physics set only the layers owner (the World, else the primary
     scene) applies its saved collision settings; members share them. */
  if ((root_seen & 4u) && vkr_scene_physics_layers_owner(scene)) {
    VkrSceneCollisionLayers before;
    vkr_scene_collision_layers_read(scene, &before);
    rebuild_matrix =
        MemCompare(before.matrix, settings.matrix, sizeof(before.matrix)) != 0;
    if (!vkr_scene_collision_layers_prepare(scene, &settings, &pending_settings,
                                            NULL)) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
      goto cleanup;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!edit_prepare_physics(scene, &pending[i])) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_FIELDS;
      goto cleanup;
    }
  }
  if (rebuild_matrix &&
      !edit_load_prepare_matrix(scene, pending, count, matrix_bodies,
                                &matrix_body_count)) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_FIELDS;
    goto cleanup;
  }
  if (!vkr_scene_physics_prepare_complete(scene, NULL)) {
    diagnostic.failure = EDIT_SIDECAR_FAILURE_FIELDS;
    goto cleanup;
  }
  for (uint32_t i = 0; i < matrix_body_count; ++i) {
    vkr_scene_physics_commit(matrix_bodies[i]);
    matrix_bodies[i] = NULL;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (pending[i].add_touched)
      s->touched[s->touched_count++] = pending[i].entity;
    if (pending[i].op == EDIT_PREPARED_DESTROY) {
      const SceneSourceIdentity *source = vkr_entity_get_component(
          scene->world, pending[i].entity, scene->comp_source_identity);
      if (!source || !edit_deleted_add(s, source))
        log_warn("Overlay deletion could not be recorded for the next save");
    }
    edit_commit(scene, &pending[i]);
  }
  edit_structure_load_commit(s, scene, &structure, index, index_count);
  if (root_seen & 16u) {
    scene->settings = scene_settings;
    scene->world_revision++;
  }
  vkr_scene_collision_layers_commit(pending_settings);
  if (pending_settings) {
    const char *share_error = NULL;
    if (!vkr_scene_collision_layers_share(scene, &share_error))
      log_warn("Collision settings could not reach every loaded scene: %s",
               share_error ? share_error : "unknown error");
  }
  pending_settings = NULL;
  snprintf(s->status, sizeof(s->status), "Loaded %u node overrides.", count);
  success = true_v;
  s->sidecar_conflict = false_v;
cleanup:
  for (uint32_t i = 0; i < matrix_body_count; ++i) {
    vkr_scene_physics_discard(matrix_bodies[i]);
  }
  if (remap)
    vkr_allocator_free(s->allocator, remap, remap_capacity * sizeof(*remap),
                       EDIT_TAG);
  vkr_scene_collision_layers_discard(pending_settings);
  if (index)
    vkr_allocator_free(s->allocator, index, index_capacity * sizeof(*index),
                       EDIT_TAG);
  if (file)
    fclose(file);
  if (bytes)
    vkr_allocator_free(s->allocator, bytes, (uint64_t)length, EDIT_TAG);
  if (components)
    vkr_allocator_free(s->allocator, components, sizeof(*components), EDIT_TAG);
  edit_structure_load_free(s, &structure);
  for (uint32_t i = 0; i < count; ++i)
    edit_discard(scene, &pending[i]);
  if (pending)
    vkr_allocator_free(s->allocator, pending, capacity * sizeof(*pending),
                       EDIT_TAG);
  if (!success) {
    s->touched_count = touched_before;
    s->deleted_count = deleted_before;
    s->sidecar_conflict = true_v;
    edit_sidecar_reject_status(s, &diagnostic);
    log_error("Editor overrides rejected: %.*s: %s", (int)path.length, path.str,
              s->status);
  }
  return success;
}
