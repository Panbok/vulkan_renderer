#include "vkr_scene_edit.h"
#include "filesystem/filesystem.h"

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
  if (!v->fields || (v->fields & ~127u))
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
        p->casts_shadow > 1 ||
        (p->casts_shadow &&
         (p->range <= 0.0f || (p->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT &&
                               (p->outer_cone_angle <= 0.0f ||
                                p->outer_cone_angle >= 1.57079632679f)))) ||
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
        !isfinite(p->sun_angular_diameter_degrees) ||
        p->sun_angular_diameter_degrees < 0.0f ||
        p->sun_angular_diameter_degrees >= 180.0f ||
        !(p->temperature_kelvin == 0.0f ||
          (p->temperature_kelvin >= VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K &&
           p->temperature_kelvin <= VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K)) ||
        p->enabled > 1 || p->atmosphere_sun > 1 ||
        vec3_dot(p->direction_local, p->direction_local) < 0.000001f)
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    const SceneRectangleLight *p = &v->rectangle_light;
    if (!finite_vector(&p->color.x, 3) || p->color.x < 0.0f ||
        p->color.y < 0.0f || p->color.z < 0.0f || !isfinite(p->radiance) ||
        p->radiance < 0.0f || !isfinite(p->size.x) || !isfinite(p->size.y) ||
        p->size.x <= 0.0f || p->size.y <= 0.0f || p->enabled > 1u)
      return false_v;
  }
  return true_v;
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
  VkrScenePhysicsPrepared *physics;
  bool8_t add_touched;
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
  if (!vkr_scene_edit_read(scene, entity, &current) ||
      (v->fields & ~current.fields) || !vkr_scene_edit_validate(v))
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

static void edit_journal_append(VkrSceneEditState *s, VkrSceneEditEntry entry) {
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
      !vkr_scene_edit_read(scene, entity, &current) ||
      (after->fields & ~current.fields)) {
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
    const float32_t params[] = {p->mass,           p->friction,
                                p->restitution,    p->gravity_factor,
                                p->linear_damping, p->angular_damping};
    if (!vkr_json_writer_name(w, string8_lit("physics")) ||
        !vkr_json_writer_begin_object(w) || !WRITE_INT("version", 2) ||
        !WRITE_BOOL("present", p->present) || !WRITE_INT("motion", p->motion) ||
        !WRITE_INT("layer", p->collision_layer) ||
        !WRITE_INT("mask", p->collision_mask) ||
        !json_floats(w, "parameters", params, ArrayCount(params)) ||
        !WRITE_BOOL("enabled", p->enabled) ||
        !WRITE_BOOL("sleep", p->allow_sleep) ||
        !WRITE_BOOL("continuous", p->continuous) ||
        !WRITE_BOOL("sensor", p->sensor) ||
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
  if (!vkr_json_writer_begin_object(w) || !WRITE_INT("version", 3) ||
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
  VkrSceneCollisionLayers settings;
  vkr_scene_collision_layers_read(scene, &settings);
  if (!vkr_json_writer_end_array(w) ||
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
} EditJson;

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
  source->scene_entity_index = (uint32_t)value;
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
  p->motion = (VkrPhysicsMotion)integer;
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
      !edit_json_key(j, "enabled", true_v) || !edit_json_bool(j, &p->enabled) ||
      !edit_json_key(j, "sleep", true_v) ||
      !edit_json_bool(j, &p->allow_sleep) ||
      !edit_json_key(j, "continuous", true_v) ||
      !edit_json_bool(j, &p->continuous) ||
      !edit_json_key(j, "sensor", true_v) || !edit_json_bool(j, &p->sensor) ||
      (version >= 2 && !edit_json_attachment_joints(j, p)) ||
      !edit_json_key(j, "colliders", true_v) || !edit_json_take(j, '[')) {
    return false_v;
  }
  p->mass = params[0];
  p->friction = params[1];
  p->restitution = params[2];
  p->gravity_factor = params[3];
  p->linear_damping = params[4];
  p->angular_damping = params[5];
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
                               "directional_enabled",
                               "point_casts_shadow",
                               "directional_sun_angular_diameter_degrees",
                               "rectangle_color",
                               "rectangle_radiance",
                               "rectangle_size",
                               "rectangle_enabled",
                               "physics",
                               "directional_temperature_kelvin",
                               "directional_atmosphere_sun"};
  MemZero(v, sizeof(*v));
  v->directional_light.sun_angular_diameter_degrees =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;
  v->directional_light.atmosphere_sun = true_v;
  uint32_t seen = 0;
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
bool8_t vkr_scene_edit_load(VkrSceneEditState *s, VkrScene *scene,
                            String8 path) {
  char cpath[1024];
  FILE *file = NULL;
  uint8_t *bytes = NULL;
  EditPrepared *pending = NULL;
  VkrSceneCollisionLayers settings = {0};
  VkrSceneCollisionLayersPrepared *pending_settings = NULL;
  VkrScenePhysicsPrepared *matrix_bodies[VKR_SCENE_PHYSICS_MAX_BODIES] = {0};
  uint32_t matrix_body_count = 0;
  uint32_t count = 0, capacity = 0;
  EditSourceIndex *index = NULL;
  uint32_t index_count = 0, additional_touched = 0;
  uint32_t index_capacity = scene->world->dir.capacity;
  uint32_t touched_before = s->touched_count;
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
  if (fread(bytes, 1, (size_t)length, file) != (size_t)length)
    goto cleanup;
  /* One directory walk and sort serve all records. Seen flags reject repeated
     overrides; touched flags merge an existing journal without quadratic scans.
   */
  if (index_capacity) {
    index = vkr_allocator_alloc(s->allocator, index_capacity * sizeof(*index),
                                EDIT_TAG);
    if (!index) {
      diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
      goto cleanup;
    }
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
      if ((root_seen & 1u) || !edit_json_int(&json, 1, 3, &version)) {
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
          uint32_t wrapper, node;
          uint64_t fingerprint;
          if (!edit_json_record(&json, &values, &wrapper, &node,
                                &fingerprint)) {
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
          if (!source) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SOURCE_MISSING;
            goto cleanup;
          }
          if (source->ambiguous) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SOURCE_AMBIGUOUS;
            goto cleanup;
          }
          if (source->seen) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SOURCE_DUPLICATE;
            goto cleanup;
          }
          if (source->fingerprint != fingerprint) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_FINGERPRINT;
            goto cleanup;
          }
          source->seen = true_v;
          VkrEntityId entity = source->entity;
          if (count == capacity) {
            uint32_t next = Max(16u, capacity * 2u);
            EditPrepared *entries = vkr_allocator_realloc(
                s->allocator, pending, capacity * sizeof(*entries),
                next * sizeof(*entries), EDIT_TAG);
            if (!entries) {
              diagnostic.failure = EDIT_SIDECAR_FAILURE_ALLOC;
              goto cleanup;
            }
            pending = entries;
            capacity = next;
          }
          if (!edit_prepare(scene, entity, &values, &pending[count])) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_FIELDS;
            goto cleanup;
          }
          pending[count].add_touched = !source->touched;
          additional_touched += !source->touched;
          count++;
          if (edit_json_take(&json, ']'))
            break;
          if (!edit_json_take(&json, ',')) {
            diagnostic.failure = EDIT_SIDECAR_FAILURE_SCHEMA;
            goto cleanup;
          }
        }
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
  if (root_seen != (version >= 3 ? 7u : 3u) || json.at != json.end) {
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
  if (root_seen & 4u) {
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
  if (rebuild_matrix) {
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
                                     &matrix_bodies[matrix_body_count], NULL)) {
        diagnostic.failure = EDIT_SIDECAR_FAILURE_FIELDS;
        goto cleanup;
      }
      matrix_body_count++;
    }
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
    edit_commit(scene, &pending[i]);
  }
  vkr_scene_collision_layers_commit(pending_settings);
  pending_settings = NULL;
  snprintf(s->status, sizeof(s->status), "Loaded %u node overrides.", count);
  success = true_v;
  s->sidecar_conflict = false_v;
cleanup:
  for (uint32_t i = 0; i < matrix_body_count; ++i) {
    vkr_scene_physics_discard(matrix_bodies[i]);
  }
  vkr_scene_collision_layers_discard(pending_settings);
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
    edit_sidecar_reject_status(s, &diagnostic);
    log_error("Editor overrides rejected: %.*s: %s", (int)path.length, path.str,
              s->status);
  }
  return success;
}
