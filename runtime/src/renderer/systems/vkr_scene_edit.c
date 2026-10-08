#include "vkr_scene_edit.h"
#include "filesystem/filesystem.h"
#include "renderer/systems/vkr_scene_partition.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"

#include "core/logger.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>
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

static bool8_t edit_journal_evict(VkrSceneEditState *s);

/* Grows an array the journal owns from `capacity` to `next` items. While the
   allocator is full, old undo steps leave first, as for a payload
   (edit_journal_prepare); only paths that add history call it, never undo or
   redo, which hold an entry meanwhile. */
static void *edit_grow(VkrSceneEditState *s, void *items, uint32_t capacity,
                       uint32_t next, uint64_t item_size) {
  void *grown = vkr_allocator_realloc(s->allocator, items, capacity * item_size,
                                      next * item_size, EDIT_TAG);
  while (!grown && edit_journal_evict(s)) {
    grown = vkr_allocator_realloc(s->allocator, items, capacity * item_size,
                                  next * item_size, EDIT_TAG);
  }
  return grown;
}

static bool8_t edit_touch(VkrSceneEditState *s, VkrEntityId entity) {
  for (uint32_t i = 0; i < s->touched_count; ++i)
    if (s->touched[i].u64 == entity.u64)
      return true_v;
  if (s->touched_count == s->touched_capacity) {
    uint32_t capacity = Max(32u, s->touched_capacity * 2u);
    VkrEntityId *next =
        edit_grow(s, s->touched, s->touched_capacity, capacity, sizeof(*next));
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
  /* A brush face is a plane in its brush's space; it moves with the brush
     (ADR-084). */
  if ((v->fields & VKR_SCENE_EDIT_TRANSFORM) &&
      vkr_scene_get_typed(scene, entity, &vkr_scene_brush_face_type)) {
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
   references and scene-global settings do not inflate every history slot.
   Undo history is a cache the next edit may reclaim: past
   VKR_SCENE_EDIT_HISTORY_BYTES, or when the allocator is full, the oldest
   steps leave first. Callers keep no pointer into `s->undo` across this call,
   since leaving steps move the entries after them. */
static void *edit_journal_prepare(VkrSceneEditState *s, VkrEntityId entity,
                                  uint64_t payload_size) {
  if (s->group_open && s->group_entries >= VKR_SCENE_EDIT_GROUP_MAX) {
    snprintf(s->status, sizeof(s->status),
             "The edit group exceeds %u undo entries.",
             VKR_SCENE_EDIT_GROUP_MAX);
    return NULL;
  }
  if (!s->undo) {
    s->undo = vkr_allocator_alloc(
        s->allocator, sizeof(*s->undo) * VKR_SCENE_EDIT_UNDO_CAPACITY,
        EDIT_TAG);
    if (!s->undo) {
      return NULL;
    }
  }
  while (s->payload_bytes + payload_size > VKR_SCENE_EDIT_HISTORY_BYTES &&
         edit_journal_evict(s)) {
  }
  void *payload = vkr_allocator_alloc(s->allocator, payload_size, EDIT_TAG);
  while (!payload && edit_journal_evict(s)) {
    payload = vkr_allocator_alloc(s->allocator, payload_size, EDIT_TAG);
  }
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
   journal, and another names their undo groups. */
static uint64_t s_edit_sequence;
static uint64_t s_edit_group;

uint64_t vkr_scene_edit_take_sequence(void) { return ++s_edit_sequence; }

uint64_t vkr_scene_edit_take_group(void) { return ++s_edit_group; }

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

/* Frees entries [first, first + count) and closes the gap; the cursor
   follows entries that move. */
static void edit_journal_remove(VkrSceneEditState *s, uint32_t first,
                                uint32_t count) {
  if (!count) {
    return;
  }
  for (uint32_t i = first; i < first + count; ++i) {
    vkr_allocator_free(s->allocator, s->undo[i].payload,
                       s->undo[i].payload_size, EDIT_TAG);
    s->payload_bytes -= s->undo[i].payload_size;
  }
  const uint32_t tail = s->undo_count - first - count;
  MemCopy(s->undo + first, s->undo + first + count, tail * sizeof(*s->undo));
  s->undo_count -= count;
  if (s->undo_cursor >= first + count) {
    s->undo_cursor -= count;
  } else if (s->undo_cursor > first) {
    s->undo_cursor = first;
  }
}

/* Entries of the step that starts at `index`: its whole group, else one. */
static uint32_t edit_journal_run(const VkrSceneEditState *s, uint32_t index) {
  const uint64_t group = s->undo[index].group;
  uint32_t end = index + 1u;
  while (group && end < s->undo_count && s->undo[end].group == group) {
    end++;
  }
  return end - index;
}

/* Drops the redo steps, which the next entry replaces anyway, else the
   oldest step unless it is the open group. False when nothing can leave. */
static bool8_t edit_journal_evict(VkrSceneEditState *s) {
  if (s->undo_cursor < s->undo_count) {
    edit_journal_remove(s, s->undo_cursor, s->undo_count - s->undo_cursor);
    return true_v;
  }
  if (!s->undo_count || (s->group_open && s->undo[0].group == s->group_open)) {
    return false_v;
  }
  edit_journal_remove(s, 0u, edit_journal_run(s, 0u));
  return true_v;
}

static void edit_journal_append(VkrSceneEditState *s, VkrSceneEditEntry entry) {
  entry.sequence = ++s_edit_sequence;
  entry.group = s->group_open;
  edit_journal_remove(s, s->undo_cursor, s->undo_count - s->undo_cursor);
  /* Eviction drops the oldest whole step, so no group is left partial. The
     open group holds at most half of the capacity, so it is never evicted. */
  if (s->undo_count == VKR_SCENE_EDIT_UNDO_CAPACITY) {
    edit_journal_remove(s, 0u, edit_journal_run(s, 0u));
  }
  s->undo[s->undo_count++] = entry;
  s->payload_bytes += entry.payload_size;
  s->undo_cursor = s->undo_count;
  if (s->group_open) {
    s->group_entries++;
  }
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
              s->undo[s->undo_count - 1u].entity.u64 == entity.u64 &&
              s->undo[s->undo_count - 1u].group == s->group_open
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
  if (settings->inherit_world > 1u ||
      !vkr_scene_texture_extent_valid(settings->texture_max_extent)) {
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

/* Samples a terrain stroke's entry may cover before the stroke continues in
   another entry: 3 MiB of payload. */
#define EDIT_TERRAIN_FOLD_SAMPLES (512u * 512u)

/* A terrain entry's payload: the rectangle, its weights before and after,
   then its heights before and after, so every array stays aligned. */
typedef struct EditTerrainPayload {
  VkrHeightfieldRect rect;
} EditTerrainPayload;

static uint64_t edit_terrain_size(VkrHeightfieldRect rect) {
  return sizeof(EditTerrainPayload) +
         2u * (uint64_t)vkr_heightfield_rect_count(rect) *
             (sizeof(uint16_t) + sizeof(uint32_t));
}

/* The sample arrays of a payload: heights and weights, before (0) or after
   (1). */
static void edit_terrain_arrays(EditTerrainPayload *payload, uint32_t which,
                                uint16_t **heights, uint32_t **weights) {
  const uint64_t count = vkr_heightfield_rect_count(payload->rect);
  uint8_t *at = (uint8_t *)(payload + 1);
  *weights = (uint32_t *)(at + which * count * sizeof(uint32_t));
  *heights = (uint16_t *)(at + 2u * count * sizeof(uint32_t) +
                          which * count * sizeof(uint16_t));
}

bool8_t vkr_scene_edit_terrain(VkrSceneEditState *s, VkrScene *scene,
                               VkrEntityId entity, const VkrHeightfieldOp *op,
                               uint64_t gesture) {
  const VkrHeightfield *field = vkr_scene_terrain_field(scene, entity);
  Vec3 origin = {0};
  if (!field ||
      !vkr_scene_terrain_to_local(scene, entity, vec3_zero(), &origin)) {
    snprintf(s->status, sizeof(s->status),
             "That object has no loaded terrain.");
    return false_v;
  }
  /* World to the terrain's local space. */
  VkrHeightfieldOp local = *op;
  local.a = vec3_add(op->a, origin);
  local.b = vec3_add(op->b, origin);
  local.min = vec2_new(op->min.x + origin.x, op->min.y + origin.z);
  local.max = vec2_new(op->max.x + origin.x, op->max.y + origin.z);
  local.height = op->height + origin.y;
  VkrHeightfieldRect rect;
  if (!vkr_heightfield_op_rect(field, &local, &rect)) {
    snprintf(s->status, sizeof(s->status), "The edit misses the terrain.");
    return false_v;
  }
  /* A stroke's next step folds into its entry: the entry grows to cover both
     rectangles and keeps the samples from before the stroke. Once that
     rectangle would pass EDIT_TERRAIN_FOLD_SAMPLES, the step starts another
     entry of the stroke's group instead, so a long stroke does not copy an
     ever larger rectangle at each step and still undoes as one. */
  VkrSceneEditEntry *last = s->undo_count && s->undo_cursor == s->undo_count
                                ? &s->undo[s->undo_count - 1u]
                                : NULL;
  const bool8_t stroke = gesture && s->gesture == gesture && last &&
                         last->kind == VKR_SCENE_EDIT_ENTRY_TERRAIN &&
                         last->entity.u64 == entity.u64 &&
                         (last->group == s->group_open ||
                          (last->group && last->group == s->gesture_group));
  VkrHeightfieldRect whole = rect;
  bool8_t fold = stroke;
  if (stroke) {
    const EditTerrainPayload *stroked = last->payload;
    (void)vkr_heightfield_rect_union(field, stroked->rect, rect, &whole);
    /* The union contains the entry's rectangle; equal counts mean equal. */
    fold = vkr_heightfield_rect_count(whole) <= EDIT_TERRAIN_FOLD_SAMPLES ||
           vkr_heightfield_rect_count(whole) ==
               vkr_heightfield_rect_count(stroked->rect);
    whole = fold ? whole : rect;
  }
  if (stroke && !fold && !s->group_open && !last->group) {
    s->gesture_group = ++s_edit_group;
    last->group = s->gesture_group;
  }
  EditTerrainPayload *previous = fold ? last->payload : NULL;
  /* A streamed terrain loads the samples the entry keeps. */
  if (!vkr_scene_terrain_require(scene, entity, whole)) {
    snprintf(s->status, sizeof(s->status),
             "The terrain samples could not be read.");
    return false_v;
  }
  const uint64_t size = edit_terrain_size(whole);
  /* A new entry's storage may evict old steps and move `last`. */
  const uint64_t stroke_group = stroke && !s->group_open ? last->group : 0u;
  EditTerrainPayload *payload =
      previous ? vkr_allocator_alloc(s->allocator, size, EDIT_TAG)
               : edit_journal_prepare(s, entity, size);
  if (!payload) {
    snprintf(s->status, sizeof(s->status), "Out of edit memory.");
    return false_v;
  }
  payload->rect = whole;
  uint16_t *before_heights = NULL;
  uint32_t *before_weights = NULL;
  uint16_t *after_heights = NULL;
  uint32_t *after_weights = NULL;
  edit_terrain_arrays(payload, 0u, &before_heights, &before_weights);
  edit_terrain_arrays(payload, 1u, &after_heights, &after_weights);
  vkr_heightfield_read_rect(field, whole, before_heights, before_weights);
  if (previous) {
    /* What the stroke changed already goes back to its first state. */
    uint16_t *old_heights = NULL;
    uint32_t *old_weights = NULL;
    edit_terrain_arrays(previous, 0u, &old_heights, &old_weights);
    const uint32_t old_width = previous->rect.x1 - previous->rect.x0 + 1u;
    const uint32_t width = whole.x1 - whole.x0 + 1u;
    for (uint32_t z = previous->rect.z0; z <= previous->rect.z1; ++z) {
      const size_t from = (size_t)(z - previous->rect.z0) * old_width;
      const size_t to =
          (size_t)(z - whole.z0) * width + (previous->rect.x0 - whole.x0);
      MemCopy(before_heights + to, old_heights + from,
              old_width * sizeof(uint16_t));
      MemCopy(before_weights + to, old_weights + from,
              old_width * sizeof(uint32_t));
    }
  }
  VkrHeightfieldRect touched;
  if (!vkr_scene_terrain_apply(scene, entity, &local, s->allocator, &touched)) {
    vkr_allocator_free(s->allocator, payload, size, EDIT_TAG);
    snprintf(s->status, sizeof(s->status), "The terrain edit failed.");
    return false_v;
  }
  vkr_heightfield_read_rect(vkr_scene_terrain_field(scene, entity), whole,
                            after_heights, after_weights);
  if (previous) {
    vkr_allocator_free(s->allocator, last->payload, last->payload_size,
                       EDIT_TAG);
    s->payload_bytes = s->payload_bytes - last->payload_size + size;
    last->payload = payload;
    last->payload_size = size;
    s->revision++;
    return true_v;
  }
  edit_journal_append(s,
                      (VkrSceneEditEntry){.kind = VKR_SCENE_EDIT_ENTRY_TERRAIN,
                                          .entity = entity,
                                          .payload = payload,
                                          .payload_size = size});
  if (stroke_group) {
    s->undo[s->undo_count - 1u].group = stroke_group;
  } else {
    s->gesture_group = 0u;
  }
  s->gesture = gesture;
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
  /* The document-stable id the editor gave a created entity. */
  EDIT_OBJECT_REF = 1u << 7,
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
  VkrEntityRef ref;
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

/* Converts one held value of `type` in place. */
static void edit_migrate_bytes(const VkrTypeDesc *held, uint8_t *bytes,
                               const VkrTypeDesc *type,
                               const VkrTypeDesc *previous) {
  if (held != type) {
    return;
  }
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  vkr_type_migrate(previous, bytes, type, value);
  MemCopy(bytes, value, type->size);
}

static void edit_migrate_values(VkrSceneEditValues *values,
                                const VkrTypeDesc *type,
                                const VkrTypeDesc *previous) {
  if (values->fields & VKR_SCENE_EDIT_COMPONENT) {
    edit_migrate_bytes(values->component_type, values->component, type,
                       previous);
  }
}

void vkr_scene_edit_migrate_type(VkrSceneEditState *s,
                                 VkrSceneEditValues *pending,
                                 const VkrTypeDesc *type,
                                 const VkrTypeDesc *previous) {
  if (pending) {
    edit_migrate_values(pending, type, previous);
  }
  for (uint32_t i = 0; s && i < s->undo_count; ++i) {
    VkrSceneEditEntry *entry = &s->undo[i];
    if (entry->kind == VKR_SCENE_EDIT_ENTRY_ENTITY) {
      VkrSceneEditValues *values = entry->payload;
      edit_migrate_values(&values[0], type, previous);
      edit_migrate_values(&values[1], type, previous);
    } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_STRUCTURE) {
      EditStructure *structure = entry->payload;
      edit_migrate_bytes(structure->type, structure->component, type, previous);
      edit_migrate_bytes(structure->replaced, structure->replaced_component,
                         type, previous);
      EditObject *object = &structure->object;
      for (uint32_t c = 0; c < object->component_count; ++c) {
        edit_migrate_bytes(object->types[c], object->components[c], type,
                           previous);
      }
    }
  }
}

/* Types the editor may add or remove on a loaded scene. */
static bool8_t edit_world_type(const VkrTypeDesc *type) {
  return vkr_scene_world_type_live(type);
}

/* Whether `entity` has a child; a brush's faces do not count when
   `brush_faces` is false, because deleting the brush deletes them. */
/* Whether `entity` has children; without `parts`, only children that are
   objects of their own (vkr_scene_entity_is_part) count. */
static bool8_t edit_has_children_except(const VkrScene *scene,
                                        VkrEntityId entity, bool8_t parts) {
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId other = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, other, scene->comp_transform);
    if (transform && transform->parent.u64 == entity.u64 &&
        vkr_scene_entity_alive(scene, other) &&
        (parts || !vkr_scene_entity_is_part(scene, other))) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t edit_has_children(const VkrScene *scene, VkrEntityId entity) {
  return edit_has_children_except(scene, entity, true_v);
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
        id == scene->comp_rectangle_light || id == scene->comp_entity_ref;
    /* Animation settings need their binding, which undo cannot restore. */
    for (uint32_t t = 0; !known && t < scene->type_count; ++t) {
      known = id == scene->types[t].id &&
              scene->types[t].type != &vkr_scene_animation_type;
    }
    /* Generated shape, brush, blockout and text state is rebuilt from its
       typed component. */
    known |= (id == scene->comp_shape &&
              (vkr_scene_get_typed(scene, entity, &vkr_scene_shape_type) ||
               vkr_scene_get_typed(scene, entity, &vkr_scene_brush_type) ||
               vkr_scene_get_typed(scene, entity, &vkr_scene_blockout_type))) ||
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
  if (edit_has_children_except(scene, entity, false_v)) {
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
  const VkrEntityRef *ref =
      vkr_entity_get_component(world, entity, scene->comp_entity_ref);
  if (ref) {
    o->ref = *ref;
    o->parts |= EDIT_OBJECT_REF;
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
  if (ok && (o->parts & EDIT_OBJECT_REF)) {
    ok = vkr_scene_set_entity_ref(scene, entity, &o->ref);
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
    VkrSceneEditCreated *next =
        edit_grow(s, s->created, s->created_capacity, capacity, sizeof(*next));
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
    /* Undo and redo grow this array while they hold an entry, so no step
       may leave here. */
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

/* Restores `structure`'s object under `parent` as a new editor-created
   entity and journals the creation. The object carries its id. */
static VkrEntityId edit_create_object(VkrSceneEditState *s, VkrScene *scene,
                                      VkrEntityId parent,
                                      EditStructure *structure) {
  structure->op = EDIT_STRUCTURE_CREATE;
  structure->object.created_id = Max(1u, s->next_created_id);
  structure->object.transform.parent = parent;
  structure->object.parts |= EDIT_OBJECT_REF;
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
  return entity;
}

VkrEntityId vkr_scene_edit_create(VkrSceneEditState *s, VkrScene *scene,
                                  VkrEntityId parent,
                                  const VkrSceneEditValues *values) {
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  if ((parent.u64 && !vkr_scene_get_transform(scene, parent)) ||
      !edit_object_from_values(values, &structure->object)) {
    snprintf(s->status, sizeof(s->status), "Invalid object values.");
    return VKR_ENTITY_ID_INVALID;
  }
  /* Every created entity gets its own id, even a copy of another, unless
     its creator chose one ahead. */
  if (vkr_entity_ref_empty(&values->ref) ||
      vkr_scene_find_entity_ref(scene, &values->ref).u64) {
    vkr_scene_entity_ref_generate(&structure->object.ref);
  } else {
    structure->object.ref = values->ref;
  }
  const VkrEntityId entity = edit_create_object(s, scene, parent, structure);
  if (entity.u64) {
    snprintf(s->status, sizeof(s->status), "Created %s.",
             structure->object.name[0] ? structure->object.name : "an object");
  }
  return entity;
}

/* Hierarchies deeper than this are refused rather than recursed into. */
#define EDIT_DUPLICATE_DEPTH_MAX 32u

/* Whether every entity of `entity`'s subtree is made of parts a snapshot
   restores. */
static bool8_t edit_duplicable(const VkrScene *scene, VkrEntityId entity,
                               uint32_t depth, const char **reason) {
  if (depth >= EDIT_DUPLICATE_DEPTH_MAX) {
    *reason = "The hierarchy is too deep to duplicate.";
    return false_v;
  }
  if (!edit_deletable_parts(scene, entity, NULL)) {
    *reason = "Only lights, shapes, brushes, text and world objects can be "
              "duplicated; meshes and physics bodies cannot yet.";
    return false_v;
  }
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, child, scene->comp_transform);
    if (transform && transform->parent.u64 == entity.u64 &&
        vkr_scene_entity_alive(scene, child) &&
        !edit_duplicable(scene, child, depth + 1u, reason)) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_scene_edit_can_duplicate(const VkrScene *scene, VkrEntityId entity,
                                     const char **reason) {
  const char *unused = NULL;
  reason = reason ? reason : &unused;
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    *reason = "The object no longer exists.";
    return false_v;
  }
  if (!vkr_entity_get_component(scene->world, entity, scene->comp_transform)) {
    *reason = "Only placed objects can be duplicated.";
    return false_v;
  }
  return edit_duplicable(scene, entity, 0u, reason);
}

/* Whether an entity of `scene` is named `name`. */
static bool8_t edit_name_taken(const VkrScene *scene, const char *name) {
  const uint64_t length = strlen(name);
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId other = vkr_entity_id_from_index(scene->world, i);
    const String8 taken = vkr_scene_get_name(scene, other);
    if (taken.length == length && MemCompare(taken.str, name, length) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* `name` with the first free " (n)" number, replacing one it ends with, as
   Unity numbers duplicates. */
static void edit_duplicate_name(const VkrScene *scene, const char *name,
                                char *out, uint32_t capacity) {
  uint64_t stem = strlen(name);
  if (stem >= 4u && name[stem - 1u] == ')') {
    uint64_t open = stem - 1u;
    while (open > 0u && name[open - 1u] >= '0' && name[open - 1u] <= '9') {
      --open;
    }
    if (open >= 3u && open < stem - 1u && name[open - 1u] == '(' &&
        name[open - 2u] == ' ') {
      stem = open - 2u;
    }
  }
  for (uint32_t n = 1u; n < 10000u; ++n) {
    snprintf(out, capacity, "%.*s (%u)", (int)stem, name, n);
    if (!edit_name_taken(scene, out)) {
      return;
    }
  }
}

/* Copies `entity` under `parent`, then its children under the copy. */
static VkrEntityId edit_duplicate_subtree(VkrSceneEditState *s, VkrScene *scene,
                                          VkrEntityId entity,
                                          VkrEntityId parent,
                                          const char *name) {
  EditStructure *structure = &s_edit_structure;
  MemZero(structure, sizeof(*structure));
  edit_object_capture(s, scene, entity, &structure->object);
  /* The copy belongs to no document entity and has its own id. */
  structure->object.parts &= ~(uint32_t)EDIT_OBJECT_SOURCE;
  MemZero(&structure->object.source, sizeof(structure->object.source));
  vkr_scene_entity_ref_generate(&structure->object.ref);
  if (name) {
    snprintf(structure->object.name, sizeof(structure->object.name), "%s",
             name);
    structure->object.parts |= EDIT_OBJECT_NAME;
  }
  const VkrEntityId copy = edit_create_object(s, scene, parent, structure);
  if (!copy.u64) {
    return VKR_ENTITY_ID_INVALID;
  }
  /* Copies append to the entity directory; they never match `entity` as a
     parent, so the scan stays bounded by the copies it makes. */
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId child = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, child, scene->comp_transform);
    if (transform && transform->parent.u64 == entity.u64 &&
        vkr_scene_entity_alive(scene, child) &&
        !edit_duplicate_subtree(s, scene, child, copy, NULL).u64) {
      return VKR_ENTITY_ID_INVALID;
    }
  }
  return copy;
}

VkrEntityId vkr_scene_edit_duplicate(VkrSceneEditState *s, VkrScene *scene,
                                     VkrEntityId entity) {
  const char *reason = NULL;
  if (!vkr_scene_edit_can_duplicate(scene, entity, &reason)) {
    snprintf(s->status, sizeof(s->status), "%s", reason);
    return VKR_ENTITY_ID_INVALID;
  }
  char source_name[VKR_SCENE_EDIT_NAME_CAPACITY] = {0};
  const String8 current = vkr_scene_get_name(scene, entity);
  snprintf(source_name, sizeof(source_name), "%.*s", (int)current.length,
           current.str);
  char name[VKR_SCENE_EDIT_NAME_CAPACITY] = {0};
  edit_duplicate_name(scene, source_name[0] ? source_name : "Object", name,
                      sizeof(name));
  const VkrEntityId parent = vkr_scene_get_transform(scene, entity)->parent;
  const bool8_t own_group = !s->group_open;
  if (own_group) {
    (void)vkr_scene_edit_group_begin(s);
  }
  const VkrEntityId copy =
      edit_duplicate_subtree(s, scene, entity, parent, name);
  if (own_group) {
    if (copy.u64) {
      vkr_scene_edit_group_end(s);
    } else {
      char status[sizeof(s->status)];
      snprintf(status, sizeof(status), "%s", s->status);
      (void)vkr_scene_edit_group_rollback(s, scene);
      snprintf(s->status, sizeof(s->status), "%s", status);
    }
  }
  if (copy.u64) {
    snprintf(s->status, sizeof(s->status), "Duplicated %s as %s.",
             source_name[0] ? source_name : "the object", name);
  }
  return copy;
}

static bool8_t edit_delete_one(VkrSceneEditState *s, VkrScene *scene,
                               VkrEntityId entity);

bool8_t vkr_scene_edit_delete(VkrSceneEditState *s, VkrScene *scene,
                              VkrEntityId entity) {
  const char *reason = NULL;
  if (!vkr_scene_edit_can_delete(scene, entity, &reason)) {
    snprintf(s->status, sizeof(s->status), "%s", reason);
    return false_v;
  }
  if (!edit_has_children(scene, entity)) {
    return edit_delete_one(s, scene, entity);
  }
  /* An object leaves with its parts (brush faces, connections) as one
     step: parts first, so undo restores the object before them. */
  const bool8_t own_group = !s->group_open;
  if (own_group) {
    (void)vkr_scene_edit_group_begin(s);
  }
  bool8_t ok = true_v;
  for (uint32_t i = 0; ok && i < scene->world->dir.living; ++i) {
    const VkrEntityId face = vkr_entity_id_from_index(scene->world, i);
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, face, scene->comp_transform);
    if (transform && transform->parent.u64 == entity.u64 &&
        vkr_scene_entity_alive(scene, face)) {
      ok = edit_delete_one(s, scene, face);
    }
  }
  ok = ok && edit_delete_one(s, scene, entity);
  if (own_group) {
    if (ok) {
      vkr_scene_edit_group_end(s);
    } else {
      char status[sizeof(s->status)];
      snprintf(status, sizeof(status), "%s", s->status);
      (void)vkr_scene_edit_group_rollback(s, scene);
      snprintf(s->status, sizeof(s->status), "%s", status);
    }
  }
  return ok;
}

static bool8_t edit_delete_one(VkrSceneEditState *s, VkrScene *scene,
                               VkrEntityId entity) {
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

/* The world matrix of `entity` from the local values along its parents. An
   earlier edit of the same batch, such as a snap before a move or a parent
   created and placed just before, leaves the cached world matrices behind
   until the scene updates. A transform without editable values gives its
   cached world. */
static Mat4 edit_world_now(VkrScene *scene, VkrEntityId entity) {
  Mat4 world = mat4_identity();
  for (uint32_t depth = 0; entity.u64 && depth < 256u; ++depth) {
    const SceneTransform *link = vkr_scene_get_transform(scene, entity);
    if (!link) {
      break;
    }
    if (!link->trs_editable) {
      return mat4_mul(link->world, world);
    }
    const Mat4 local = mat4_mul(mat4_mul(mat4_translate(link->position),
                                         vkr_quat_to_mat4(link->rotation)),
                                mat4_scale(link->scale));
    world = mat4_mul(local, world);
    entity = link->parent;
  }
  return world;
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
  /* The world poses from the local values now (edit_world_now). */
  const Mat4 world_now = edit_world_now(scene, entity);
  const Mat4 local =
      parent.u64
          ? mat4_mul(mat4_inverse(edit_world_now(scene, parent)), world_now)
          : world_now;
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

/* Applies one entry's before (undo) or after (redo) state. */
static bool8_t edit_entry_write(VkrSceneEditState *s, VkrScene *scene,
                                VkrSceneEditEntry *entry, bool8_t redo) {
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
  } else if (entry->kind == VKR_SCENE_EDIT_ENTRY_TERRAIN) {
    EditTerrainPayload *payload = entry->payload;
    uint16_t *heights = NULL;
    uint32_t *weights = NULL;
    edit_terrain_arrays(payload, redo ? 1u : 0u, &heights, &weights);
    if (!vkr_scene_terrain_write(scene, entry->entity, payload->rect, heights,
                                 weights)) {
      snprintf(s->status, sizeof(s->status), "The terrain is gone.");
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
  return true_v;
}

/* Moves the cursor over one entry. */
static bool8_t edit_step(VkrSceneEditState *s, VkrScene *scene, bool8_t redo) {
  VkrSceneEditEntry *entry =
      &s->undo[redo ? s->undo_cursor : s->undo_cursor - 1u];
  if (!edit_entry_write(s, scene, entry, redo)) {
    return false_v;
  }
  if (redo)
    s->undo_cursor++;
  else
    s->undo_cursor--;
  return true_v;
}

bool8_t vkr_scene_edit_undo(VkrSceneEditState *s, VkrScene *scene,
                            bool8_t redo) {
  if (s->group_open ||
      (redo ? s->undo_cursor == s->undo_count : s->undo_cursor == 0u))
    return false_v;
  s->gesture = 0u;
  /* A group's entries move together: undo walks it from its newest entry,
     redo from its oldest. A failure stops inside the group, as it would
     between two single entries. */
  const uint64_t group =
      s->undo[redo ? s->undo_cursor : s->undo_cursor - 1u].group;
  do {
    if (!edit_step(s, scene, redo)) {
      s->revision++;
      return false_v;
    }
  } while (group && (redo ? s->undo_cursor < s->undo_count &&
                                s->undo[s->undo_cursor].group == group
                          : s->undo_cursor > 0u &&
                                s->undo[s->undo_cursor - 1u].group == group));
  s->revision++;
  snprintf(s->status, sizeof(s->status), "%s", redo ? "Redone." : "Undone.");
  return true_v;
}

uint64_t vkr_scene_edit_group_begin(VkrSceneEditState *s) {
  if (s->group_open) {
    return 0u;
  }
  s->group_open = ++s_edit_group;
  s->group_entries = 0u;
  s->gesture = 0u;
  return s->group_open;
}

void vkr_scene_edit_group_end(VkrSceneEditState *s) {
  s->group_open = 0u;
  s->group_entries = 0u;
}

bool8_t vkr_scene_edit_group_rollback(VkrSceneEditState *s, VkrScene *scene) {
  const uint64_t group = s->group_open;
  bool8_t ok = true_v;
  /* The open group's entries are the newest ones and the cursor sits above
     them, because appending dropped every redo entry. */
  while (group && s->undo_cursor > 0u &&
         s->undo[s->undo_cursor - 1u].group == group) {
    if (!edit_step(s, scene, false_v)) {
      ok = false_v;
      break;
    }
  }
  if (ok) {
    edit_journal_remove(s, s->undo_cursor, s->undo_count - s->undo_cursor);
  }
  vkr_scene_edit_group_end(s);
  s->revision++;
  return ok;
}

bool8_t vkr_scene_edit_group_present(const VkrSceneEditState *s,
                                     uint64_t group) {
  for (uint32_t i = 0; group && i < s->undo_count; ++i) {
    if (s->undo[i].group == group) {
      return true_v;
    }
  }
  return false_v;
}

/* Whether `entity` is one of `set` or, while alive, lies below one of them. */
static bool8_t edit_entity_within(const VkrScene *scene, VkrEntityId entity,
                                  const VkrEntityId *set, uint32_t count) {
  VkrEntityId at = entity;
  for (uint32_t depth = 0; at.u64 && depth < 256u; ++depth) {
    for (uint32_t i = 0; i < count; ++i) {
      if (set[i].u64 == at.u64) {
        return true_v;
      }
    }
    const SceneTransform *transform =
        vkr_scene_entity_alive(scene, at)
            ? vkr_scene_get_transform((VkrScene *)scene, at)
            : NULL;
    at = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
  }
  return false_v;
}

/* Every entity an entry names: its own, a reparent's parents and, with
   `object_parent`, the parent of a created or deleted object. Reverting a
   creation or deletion does not change that parent, so a group does not
   claim it; a later entry that names it as a parent still depends on it. */
static uint32_t edit_entry_entities(const VkrSceneEditEntry *entry,
                                    bool8_t object_parent, VkrEntityId out[4]) {
  uint32_t count = 0u;
  if (entry->entity.u64) {
    out[count++] = entry->entity;
  }
  if (entry->kind == VKR_SCENE_EDIT_ENTRY_STRUCTURE) {
    const EditStructure *structure = entry->payload;
    const VkrEntityId parents[3] = {structure->parent[0], structure->parent[1],
                                    object_parent
                                        ? structure->object.transform.parent
                                        : VKR_ENTITY_ID_INVALID};
    for (uint32_t i = 0; i < 3u; ++i) {
      if (parents[i].u64) {
        out[count++] = parents[i];
      }
    }
  }
  return count;
}

bool8_t vkr_scene_edit_group_revert(VkrSceneEditState *s, VkrScene *scene,
                                    uint64_t group, VkrEntityId *out_conflict) {
  if (out_conflict) {
    *out_conflict = VKR_ENTITY_ID_INVALID;
  }
  if (!group || s->group_open) {
    return false_v;
  }
  uint32_t first = UINT32_MAX;
  for (uint32_t i = 0; i < s->undo_count; ++i) {
    if (s->undo[i].group == group) {
      first = i;
      break;
    }
  }
  if (first == UINT32_MAX) {
    return false_v;
  }
  const uint32_t count = edit_journal_run(s, first);
  const uint32_t end = first + count;
  /* An undone group only leaves the redo entries. */
  if (s->undo_cursor <= first) {
    edit_journal_remove(s, s->undo_cursor, s->undo_count - s->undo_cursor);
    s->revision++;
    return true_v;
  }
  /* Later applied entries must not depend on what the group touched. */
  VkrEntityId touched[4u * VKR_SCENE_EDIT_GROUP_MAX];
  uint32_t touched_count = 0u;
  for (uint32_t i = first; i < end; ++i) {
    touched_count +=
        edit_entry_entities(&s->undo[i], false_v, touched + touched_count);
  }
  for (uint32_t i = first; i < end; ++i) {
    if (s->undo[i].kind == VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH) {
      /* Physics batches change bodies across the scene; a group holding one
         reverts only as the newest step. */
      if (end != s->undo_cursor) {
        snprintf(s->status, sizeof(s->status),
                 "Physics edits revert only as the newest step.");
        return false_v;
      }
    }
  }
  for (uint32_t i = end; i < s->undo_cursor; ++i) {
    VkrEntityId named[4];
    const uint32_t named_count =
        edit_entry_entities(&s->undo[i], true_v, named);
    if (s->undo[i].kind == VKR_SCENE_EDIT_ENTRY_PHYSICS_BATCH ||
        s->undo[i].kind == VKR_SCENE_EDIT_ENTRY_COLLISION_LAYERS) {
      snprintf(s->status, sizeof(s->status),
               "A later physics edit may depend on this group.");
      return false_v;
    }
    for (uint32_t n = 0; n < named_count; ++n) {
      if (edit_entity_within(scene, named[n], touched, touched_count)) {
        if (out_conflict) {
          *out_conflict = named[n];
        }
        snprintf(s->status, sizeof(s->status),
                 "A later edit changed what this group touched.");
        return false_v;
      }
    }
  }
  edit_journal_remove(s, s->undo_cursor, s->undo_count - s->undo_cursor);
  for (uint32_t i = end; i > first; --i) {
    if (!edit_entry_write(s, scene, &s->undo[i - 1u], false_v)) {
      /* Entries above the failure stay reverted; drop them so the journal
         matches the scene. */
      edit_journal_remove(s, i, end - i);
      s->revision++;
      return false_v;
    }
  }
  edit_journal_remove(s, first, count);
  s->revision++;
  snprintf(s->status, sizeof(s->status), "Reverted.");
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

/* `texture_max_extent` is written only when set, so files without the limit
   keep their earlier form. */
static bool8_t write_scene_settings(VkrJsonWriter *w,
                                    const VkrSceneSettings *settings) {
  if (!vkr_json_writer_name(w, string8_lit("scene_settings")) ||
      !vkr_json_writer_begin_object(w) ||
      !vkr_json_writer_name(w, string8_lit("inherit_world")) ||
      !vkr_json_writer_bool(w, settings->inherit_world)) {
    return false_v;
  }
  if (settings->texture_max_extent != 0u &&
      (!vkr_json_writer_name(w, string8_lit("texture_max_extent")) ||
       !vkr_json_writer_i64(w, settings->texture_max_extent))) {
    return false_v;
  }
  return vkr_json_writer_end_object(w);
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
        !WRITE_BOOL("point_casts_shadow", p->casts_shadow) ||
        !json_floats(w, "point_source_radius", &p->source_radius, 1) ||
        !WRITE_INT("point_mobility", p->mobility) ||
        !vkr_json_writer_name(w, string8_lit("point_group")) ||
        !vkr_json_writer_string(w, string8_create((uint8_t *)p->light_group,
                                                  strlen(p->light_group))))
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
        !WRITE_BOOL("directional_atmosphere_sun", p->atmosphere_sun) ||
        !WRITE_BOOL("directional_atmosphere_moon", p->atmosphere_moon))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    const SceneRectangleLight *p = &v->rectangle_light;
    if (!json_floats(w, "rectangle_color", &p->color.x, 3) ||
        !json_floats(w, "rectangle_radiance", &p->radiance, 1) ||
        !json_floats(w, "rectangle_size", &p->size.x, 2) ||
        !WRITE_BOOL("rectangle_enabled", p->enabled) ||
        !WRITE_INT("rectangle_mobility", p->mobility) ||
        !vkr_json_writer_name(w, string8_lit("rectangle_group")) ||
        !vkr_json_writer_string(w, string8_create((uint8_t *)p->light_group,
                                                  strlen(p->light_group))))
      return false_v;
  }
  if (v->fields & VKR_SCENE_EDIT_PHYSICS) {
    const VkrScenePhysicsSnapshot *p = &v->physics;
    const float32_t params[] = {
        p->body.mass,           p->body.friction,
        p->body.restitution,    p->body.gravity_factor,
        p->body.linear_damping, p->body.angular_damping};
    if (!vkr_json_writer_name(w, string8_lit("physics")) ||
        !vkr_json_writer_begin_object(w) || !WRITE_INT("version", 3) ||
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
          !WRITE_INT("fit", c->fit) ||
          !json_floats(w, "fit_offset", &c->fit_offset.x, 3) ||
          !json_floats(w, "padding", &c->padding.x, 3) ||
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
    vkr_entity_ref_format(&scene->document_ids[i], text);
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

/* Created entity `index` of the overlay as a record. */
static bool8_t write_created(VkrJsonWriter *w, const VkrSceneEditState *s,
                             const VkrScene *scene, uint32_t index) {
  const VkrEntityId entity = s->created[index].entity;
  VkrSceneEditValues values;
  if (!vkr_scene_edit_read(scene, entity, &values)) {
    return false_v;
  }
  /* A created entity's body is saved with it; an absent one is noise. */
  if (!values.physics.present) {
    values.fields &= ~(uint32_t)VKR_SCENE_EDIT_PHYSICS;
  }
  VkrEntityRef ref = {0};
  char ref_text[37] = {0};
  if (vkr_scene_entity_ref(scene, entity, &ref)) {
    vkr_entity_ref_format(&ref, ref_text);
  }
  return vkr_json_writer_begin_object(w) &&
         WRITE_INT("id", s->created[index].id) &&
         (!ref_text[0] || (vkr_json_writer_name(w, string8_lit("uuid")) &&
                           vkr_json_writer_string(
                               w, string8_create((uint8_t *)ref_text, 36u)))) &&
         write_parent(w, s, scene, entity) && write_values(w, &values) &&
         write_components(w, scene, entity) && vkr_json_writer_end_object(w);
}

/* Whether the overlay itself writes created entity `index`: dead ones
   (undone creations) are skipped, and in a partitioned scene those
   streaming with a cell go to its document. */
static bool8_t edit_overlay_holds(const VkrSceneEditState *s,
                                  const VkrScene *scene, bool8_t partitioned,
                                  const SceneWorldPartition *settings,
                                  uint32_t index) {
  const VkrEntityId entity = s->created[index].entity;
  VkrScenePartitionCell cell;
  return vkr_scene_entity_alive(scene, entity) &&
         !(partitioned &&
           vkr_scene_partition_entity_cell(scene, settings, entity, &cell));
}

/* Created entities the overlay itself would write. */
static uint32_t edit_overlay_created_count(const VkrSceneEditState *s,
                                           const VkrScene *scene) {
  SceneWorldPartition settings;
  const bool8_t partitioned =
      s->cells_root[0] && vkr_scene_partition_settings(scene, &settings);
  uint32_t count = 0u;
  for (uint32_t i = 0; i < s->created_count; ++i) {
    count += edit_overlay_holds(s, scene, partitioned, &settings, i);
  }
  return count;
}

/* Deleted document entities, then the editor-created entities the overlay
   holds, with their overlay ids. */
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
  SceneWorldPartition settings;
  const bool8_t partitioned =
      s->cells_root[0] && vkr_scene_partition_settings(scene, &settings);
  for (uint32_t i = 0; i < s->created_count; ++i) {
    if (!edit_overlay_holds(s, scene, partitioned, &settings, i)) {
      continue;
    }
    if (!write_created(w, s, scene, i)) {
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

/* What a save does to one loaded or stale cell: its flags afterwards, and
   whether a new document waits beside the old or the old one goes. */
typedef struct EditCellChange {
  uint32_t record;
  uint32_t flags;
  bool8_t staged;
  bool8_t remove;
} EditCellChange;

/* A save's cell documents and index, written beside the files they replace
   before any replaces them, so a failure leaves every document as it was. */
typedef struct EditCellsSave {
  bool8_t active;
  EditCellChange *changes;
  uint32_t change_count;
  uint32_t change_capacity;
  bool8_t index_staged;
  char index_path[1100];
} EditCellsSave;

static bool8_t edit_cells_stage(VkrSceneEditState *s, VkrScene *scene,
                                EditCellsSave *save);
static void edit_cells_discard(VkrSceneEditState *s, VkrScene *scene,
                               EditCellsSave *save);
static bool8_t edit_cells_commit(VkrSceneEditState *s, VkrScene *scene,
                                 EditCellsSave *save);

bool8_t vkr_scene_edit_save(VkrSceneEditState *s, VkrScene *scene,
                            String8 path) {
  if (s->sidecar_conflict) {
    snprintf(
        s->status, sizeof(s->status),
        "Save blocked: resolve or move the conflicting sidecar, then reload.");
    return false_v;
  }
  /* A rebased world (ADR-086) holds positions the documents do not. */
  if (scene->origin_offset.x != 0.0f || scene->origin_offset.y != 0.0f ||
      scene->origin_offset.z != 0.0f) {
    snprintf(s->status, sizeof(s->status),
             "Save blocked: reset the simulation; the origin is rebased.");
    return false_v;
  }
  /* Objects in a cell whose document has not loaded join it first, so the
     cell's document keeps what it held. */
  if (!vkr_scene_edit_cells_track(s, scene)) {
    snprintf(s->status, sizeof(s->status),
             "Save blocked: a cell document could not be read.");
    return false_v;
  }
  const uint32_t created = edit_overlay_created_count(s, scene);
  if (created > VKR_SCENE_EDIT_CREATED_MAX) {
    snprintf(s->status, sizeof(s->status),
             "Save blocked: %u created objects; a scene file loads at most %u "
             "(world partition cells hold more).",
             created, VKR_SCENE_EDIT_CREATED_MAX);
    return false_v;
  }
  /* Terrain samples live in their own files, written with the edits. */
  if (!vkr_scene_terrain_save(scene, s->status, sizeof(s->status))) {
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
       created entities below; runtime-only ones, such as what scripts
       spawned in Play, are never saved. */
    if (!vkr_scene_entity_alive(scene, entity) || edit_created_id(s, entity) ||
        vkr_scene_entity_transient(scene, entity))
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
      !write_scene_settings(w, &scene->settings) ||
      !vkr_json_writer_name(w, string8_lit("collision_settings")) ||
      !write_collision_layers(w, &settings) || !vkr_json_writer_end_object(w))
    goto failed;
  /* Cell documents are written beside their files first: an object moving
     between the overlay and a cell is never missing from both. */
  EditCellsSave cells;
  if (!edit_cells_stage(s, scene, &cells)) {
    vkr_json_file_writer_abort(&file);
    return false_v;
  }
  if (!vkr_json_file_writer_commit(&file)) {
    edit_cells_discard(s, scene, &cells);
    goto failed;
  }
  if (!edit_cells_commit(s, scene, &cells)) {
    return false_v;
  }
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
  EDIT_SIDECAR_FAILURE_CREATED_LIMIT,
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

/* `scene_settings`: `inherit_world`, then an optional `texture_max_extent`
   that earlier files omit. */
static bool8_t edit_json_scene_settings(EditJson *j, VkrSceneSettings *out) {
  if (!edit_json_take(j, '{') || !edit_json_key(j, "inherit_world", false_v) ||
      !edit_json_bool(j, &out->inherit_world)) {
    return false_v;
  }
  out->texture_max_extent = 0u;
  if (edit_json_take(j, ',')) {
    int64_t extent = 0;
    if (!edit_json_key(j, "texture_max_extent", false_v) ||
        !edit_json_int(j, 0, VKR_SCENE_TEXTURE_EXTENT_MAX, &extent) ||
        !vkr_scene_texture_extent_valid((uint32_t)extent)) {
      return false_v;
    }
    out->texture_max_extent = (uint32_t)extent;
  }
  return edit_json_take(j, '}');
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
      !edit_json_int(j, 1, 3, &version) ||
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
            !edit_json_string(j, c->asset_path, sizeof(c->asset_path))))) {
        return false_v;
      }
      /* Colliders saved before fits existed keep their authored sizes. */
      c->fit = VKR_SCENE_COLLIDER_FIT_MANUAL;
      if (version >= 3) {
        if (!edit_json_key(j, "fit", true_v) ||
            !edit_json_int(j, VKR_SCENE_COLLIDER_FIT_MANUAL,
                           VKR_SCENE_COLLIDER_FIT_AUTO, &integer) ||
            !edit_json_key(j, "fit_offset", true_v) ||
            !edit_json_floats(j, &c->fit_offset.x, 3) ||
            !edit_json_key(j, "padding", true_v) ||
            !edit_json_floats(j, &c->padding.x, 3)) {
          return false_v;
        }
        c->fit = (VkrSceneColliderFit)integer;
      }
      if (!edit_json_take(j, '}')) {
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
  /* Created records: the overlay id and the entity's document-stable id,
     which overlays before entity references lack. */
  uint32_t id;
  bool8_t has_ref;
  VkrEntityRef ref;
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
                               "id",
                               "directional_atmosphere_moon",
                               "uuid",
                               "point_source_radius",
                               "point_mobility",
                               "point_group",
                               "rectangle_mobility",
                               "rectangle_group"};
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
    case 32:
      ok = edit_json_bool(j, &v->directional_light.atmosphere_moon);
      break;
    case 33: {
      char text[40];
      ok = created && edit_json_string(j, text, sizeof(text)) &&
           vkr_entity_ref_parse(text, strlen(text), &extra->ref);
      extra->has_ref = ok;
      break;
    }
    case 34:
      ok = edit_json_floats(j, &v->point_light.source_radius, 1);
      break;
    case 35:
      ok = edit_json_int(j, 0, 1, &integer);
      v->point_light.mobility = (VkrLightMobility)integer;
      break;
    case 36:
      ok = edit_json_string(j, v->point_light.light_group,
                            sizeof(v->point_light.light_group)) &&
           vkr_light_group_name_valid(v->point_light.light_group,
                                      strlen(v->point_light.light_group));
      break;
    case 37:
      ok = edit_json_int(j, 0, 1, &integer);
      v->rectangle_light.mobility = (VkrLightMobility)integer;
      break;
    case 38:
      ok = edit_json_string(j, v->rectangle_light.light_group,
                            sizeof(v->rectangle_light.light_group)) &&
           vkr_light_group_name_valid(v->rectangle_light.light_group,
                                      strlen(v->rectangle_light.light_group));
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
  if (v->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    required |= seen & (1ull << 32u); /* Old journals have no moon. */
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    required |= seen & (1u << 19u); /* Old journals default shadows off. */
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    required |= seen & (1ull << 34u); /* Old journals have no source radius. */
  /* Old journals bake every light into the default group. */
  if (v->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    required |= seen & (3ull << 35u);
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT)
    required |= seen & (3ull << 37u);
  if (v->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT)
    required |= 15u << 21u;
  if (v->fields & VKR_SCENE_EDIT_PHYSICS) {
    required |= 1u << 25u;
  }
  required |= seen & (1ull << 28u); /* Components are optional. */
  required |= seen & (1ull << 30u); /* So is the parent. */
  required |= seen & (1ull << 33u); /* And a created entity's id. */
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
  case EDIT_SIDECAR_FAILURE_CREATED_LIMIT:
    snprintf(s->status, sizeof(s->status),
             "Override file creates more than %u objects.",
             VKR_SCENE_EDIT_CREATED_MAX);
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

/* Whether the entity still carries the name `values` saved. */
static bool8_t edit_load_same_name(const VkrScene *scene, VkrEntityId entity,
                                   const VkrSceneEditValues *values) {
  if (!(values->fields & VKR_SCENE_EDIT_NAME)) {
    return false_v;
  }
  const String8 name = vkr_scene_get_name(scene, entity);
  const uint64_t length = strlen(values->name);
  return name.length == length &&
         MemCompare(name.str, values->name, length) == 0;
}

/* Claim the entity an override record names: it must exist, be unique and
   not already be claimed. A different source fingerprint, as a local
   download that differs between machines gives, still binds when the node
   keeps the name the record saved; `rebound` counts those. */
static bool8_t
edit_load_bind_source(const VkrScene *scene, EditSourceIndex *source,
                      uint64_t fingerprint, const VkrSceneEditValues *values,
                      uint32_t *rebound, EditSidecarFailure *failure) {
  if (!source) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_MISSING;
  } else if (source->ambiguous) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_AMBIGUOUS;
  } else if (source->seen) {
    *failure = EDIT_SIDECAR_FAILURE_SOURCE_DUPLICATE;
  } else if (source->fingerprint != fingerprint &&
             !edit_load_same_name(scene, source->entity, values)) {
    *failure = EDIT_SIDECAR_FAILURE_FINGERPRINT;
  } else {
    *rebound += source->fingerprint != fingerprint;
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

typedef struct EditParentLink {
  /* Override records name the child entity; created records their id. */
  VkrEntityId child;
  uint32_t created_child;
  EditParentRef parent;
} EditParentLink;

/* Version 4 structure staged while the file is read and applied after every
   override commits: created entities first, then parent links. */
/* A created object's physics body, built once the object and its parent
   exist. */
typedef struct EditCreatedBody {
  uint32_t created_id;
  VkrScenePhysicsSnapshot physics;
} EditCreatedBody;

/* A staged created object, packed at `offset` in the load's byte store as
   EditObject's fields before `components`, then each component's own bytes:
   a level stages tens of thousands of objects, and EditObject's eight 1 KiB
   component slots take 8 KiB of each. */
typedef struct EditStagedObject {
  uint64_t offset;
  uint32_t created_id;
} EditStagedObject;

#define EDIT_OBJECT_HEAD_SIZE offsetof(EditObject, components)

typedef struct EditStructureLoad {
  EditStagedObject *objects;
  uint32_t object_count;
  uint32_t object_capacity;
  uint8_t *packed;
  uint64_t packed_size;
  uint64_t packed_capacity;
  /* The object being staged, or the last one unpacked. */
  EditObject *scratch;
  EditParentLink *links;
  uint32_t link_count;
  uint32_t link_capacity;
  EditCreatedBody *bodies;
  uint32_t body_count;
  uint32_t body_capacity;
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
  if (load->packed) {
    vkr_allocator_free(s->allocator, load->packed, load->packed_capacity,
                       EDIT_TAG);
  }
  if (load->scratch) {
    vkr_allocator_free(s->allocator, load->scratch, sizeof(*load->scratch),
                       EDIT_TAG);
  }
  if (load->links) {
    vkr_allocator_free(s->allocator, load->links,
                       load->link_capacity * sizeof(*load->links), EDIT_TAG);
  }
  if (load->bodies) {
    vkr_allocator_free(s->allocator, load->bodies,
                       load->body_capacity * sizeof(*load->bodies), EDIT_TAG);
  }
  MemZero(load, sizeof(*load));
}

/* Packs `object` into the load's byte store as its next staged object. */
static bool8_t edit_staged_add(VkrSceneEditState *s, EditStructureLoad *load,
                               const EditObject *object) {
  uint64_t size = EDIT_OBJECT_HEAD_SIZE;
  for (uint32_t c = 0; c < object->component_count; ++c) {
    size += object->types[c]->size;
  }
  if (load->object_count == load->object_capacity) {
    const uint32_t capacity = Max(64u, load->object_capacity * 2u);
    EditStagedObject *objects = vkr_allocator_realloc(
        s->allocator, load->objects, load->object_capacity * sizeof(*objects),
        capacity * sizeof(*objects), EDIT_TAG);
    if (!objects) {
      return false_v;
    }
    load->objects = objects;
    load->object_capacity = capacity;
  }
  if (load->packed_size + size > load->packed_capacity) {
    uint64_t capacity = Max(KB(64), load->packed_capacity * 2u);
    while (load->packed_size + size > capacity) {
      capacity *= 2u;
    }
    uint8_t *packed = vkr_allocator_realloc(
        s->allocator, load->packed, load->packed_capacity, capacity, EDIT_TAG);
    if (!packed) {
      return false_v;
    }
    load->packed = packed;
    load->packed_capacity = capacity;
  }

  uint8_t *at = load->packed + load->packed_size;
  MemCopy(at, object, EDIT_OBJECT_HEAD_SIZE);
  at += EDIT_OBJECT_HEAD_SIZE;
  for (uint32_t c = 0; c < object->component_count; ++c) {
    MemCopy(at, object->components[c], object->types[c]->size);
    at += object->types[c]->size;
  }
  load->objects[load->object_count++] = (EditStagedObject){
      .offset = load->packed_size, .created_id = object->created_id};
  load->packed_size += size;
  return true_v;
}

/* Staged object `index` unpacked into the load's scratch object, valid until
   the next unpack. */
static const EditObject *edit_staged_object(const EditStructureLoad *load,
                                            uint32_t index) {
  const EditStagedObject *staged = &load->objects[index];
  EditObject *object = load->scratch;
  const uint8_t *at = load->packed + staged->offset;
  MemCopy(object, at, EDIT_OBJECT_HEAD_SIZE);
  at += EDIT_OBJECT_HEAD_SIZE;
  for (uint32_t c = 0; c < object->component_count; ++c) {
    MemCopy(object->components[c], at, object->types[c]->size);
    at += object->types[c]->size;
  }
  object->created_id = staged->created_id;
  return object;
}

static bool8_t edit_structure_body(VkrSceneEditState *s,
                                   EditStructureLoad *load, uint32_t created_id,
                                   const VkrScenePhysicsSnapshot *physics) {
  if (load->body_count == load->body_capacity) {
    const uint32_t capacity = Max(4u, load->body_capacity * 2u);
    EditCreatedBody *bodies = vkr_allocator_realloc(
        s->allocator, load->bodies, load->body_capacity * sizeof(*bodies),
        capacity * sizeof(*bodies), EDIT_TAG);
    if (!bodies) {
      return false_v;
    }
    load->bodies = bodies;
    load->body_capacity = capacity;
  }
  load->bodies[load->body_count++] =
      (EditCreatedBody){.created_id = created_id, .physics = *physics};
  return true_v;
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
  VkrEntityRef id;
  uint32_t index;
} EditDocumentKey;

static int edit_document_key_compare(const void *left, const void *right) {
  return MemCompare(left, right, sizeof(VkrEntityRef));
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
      VkrEntityRef id;
      if (!edit_json_string(j, text, sizeof(text)) ||
          !vkr_entity_ref_parse(text, strlen(text), &id)) {
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

static int edit_u32_compare(const void *a, const void *b) {
  const uint32_t x = *(const uint32_t *)a;
  const uint32_t y = *(const uint32_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* A document repeating a created id is malformed. Sorting a copy of the ids
   keeps the check linearithmic, where comparing objects pairwise walked
   kilobyte-strided snapshots quadratically. */
static bool8_t edit_created_ids_unique(VkrSceneEditState *s,
                                       const EditStructureLoad *load,
                                       EditSidecarFailure *failure) {
  if (load->object_count < 2u) {
    return true_v;
  }
  const uint64_t size = load->object_count * sizeof(uint32_t);
  uint32_t *ids = vkr_allocator_alloc(s->allocator, size, EDIT_TAG);
  if (!ids) {
    *failure = EDIT_SIDECAR_FAILURE_ALLOC;
    return false_v;
  }
  for (uint32_t i = 0; i < load->object_count; ++i) {
    ids[i] = load->objects[i].created_id;
  }
  qsort(ids, load->object_count, sizeof(*ids), edit_u32_compare);
  bool8_t unique = true_v;
  for (uint32_t i = 1; i < load->object_count && unique; ++i) {
    unique = ids[i] != ids[i - 1u];
  }
  vkr_allocator_free(s->allocator, ids, size, EDIT_TAG);
  return unique;
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
    if (load->object_count == VKR_SCENE_EDIT_CREATED_MAX) {
      *failure = EDIT_SIDECAR_FAILURE_CREATED_LIMIT;
      return false_v;
    }
    if (!edit_json_record(j, &values, &wrapper, &node, &fingerprint,
                          s->allocator, components, &extra, true_v))
      return false_v;
    if (!load->scratch) {
      load->scratch =
          vkr_allocator_alloc(s->allocator, sizeof(*load->scratch), EDIT_TAG);
      if (!load->scratch) {
        *failure = EDIT_SIDECAR_FAILURE_ALLOC;
        return false_v;
      }
    }
    EditObject *object = load->scratch;
    /* edit_structure_load_commit builds the body once the object exists. */
    if (values.fields & VKR_SCENE_EDIT_PHYSICS) {
      if (values.physics.present &&
          !edit_structure_body(s, load, extra.id, &values.physics)) {
        *failure = EDIT_SIDECAR_FAILURE_ALLOC;
        return false_v;
      }
      values.fields &= ~(uint32_t)VKR_SCENE_EDIT_PHYSICS;
    }
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
    if (extra.has_ref) {
      object->ref = extra.ref;
    } else {
      vkr_scene_entity_ref_generate(&object->ref);
    }
    object->parts |= EDIT_OBJECT_REF;
    if (!edit_staged_add(s, load, object)) {
      *failure = EDIT_SIDECAR_FAILURE_ALLOC;
      return false_v;
    }
    if (extra.parent.kind != EDIT_PARENT_NONE &&
        !edit_structure_link(s, load,
                             (EditParentLink){.created_child = extra.id,
                                              .parent = extra.parent})) {
      *failure = EDIT_SIDECAR_FAILURE_ALLOC;
      return false_v;
    }
    if (edit_json_take(j, ']'))
      return edit_created_ids_unique(s, load, failure);
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

static int edit_created_compare(const void *a, const void *b) {
  const uint32_t x = ((const VkrSceneEditCreated *)a)->id;
  const uint32_t y = ((const VkrSceneEditCreated *)b)->id;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* The entity of created id `id`, searched in `sorted`, the state's created
   records ordered by id, else scanned for when it is NULL. */
static VkrEntityId edit_created_lookup(const VkrSceneEditState *s,
                                       const VkrSceneEditCreated *sorted,
                                       uint32_t id) {
  if (!sorted) {
    return edit_created_entity(s, id);
  }
  const VkrSceneEditCreated key = {.id = id};
  const VkrSceneEditCreated *found = bsearch(
      &key, sorted, s->created_count, sizeof(*sorted), edit_created_compare);
  return found ? found->entity : VKR_ENTITY_ID_INVALID;
}

/* Create the staged entities, then link parents. Runs after every override
   committed; a failure here leaves that entity out and is logged. */
static void edit_structure_load_commit(VkrSceneEditState *s, VkrScene *scene,
                                       const EditStructureLoad *load,
                                       EditSourceIndex *index,
                                       uint32_t index_count) {
  for (uint32_t i = 0; i < load->object_count; ++i) {
    const VkrEntityId entity = edit_object_restore(
        scene, edit_staged_object(load, i), VKR_ENTITY_ID_INVALID);
    const uint32_t id = load->objects[i].created_id;
    if (!entity.u64 || !edit_created_add(s, entity, id)) {
      log_warn("Overlay object %u could not be created", id);
    }
  }
  /* Links and bodies find created ids in a sorted copy: a level links tens
     of thousands of objects, which a scan per link visits quadratically. A
     failed copy falls back to the scan. */
  const uint64_t sorted_size =
      (uint64_t)s->created_count * sizeof(VkrSceneEditCreated);
  VkrSceneEditCreated *sorted =
      sorted_size ? vkr_allocator_alloc(s->allocator, sorted_size, EDIT_TAG)
                  : NULL;
  if (sorted) {
    MemCopy(sorted, s->created, sorted_size);
    qsort(sorted, s->created_count, sizeof(*sorted), edit_created_compare);
  }

  for (uint32_t i = 0; i < load->link_count; ++i) {
    const EditParentLink *link = &load->links[i];
    const VkrEntityId child =
        link->created_child
            ? edit_created_lookup(s, sorted, link->created_child)
            : link->child;
    VkrEntityId parent = VKR_ENTITY_ID_INVALID;
    if (link->parent.kind == EDIT_PARENT_CREATED) {
      parent = edit_created_lookup(s, sorted, link->parent.created);
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
  /* Bodies follow parenting so they start at the object's final pose. A body
     that cannot be built leaves its object loaded without it. */
  for (uint32_t i = 0; i < load->body_count; ++i) {
    const EditCreatedBody *body = &load->bodies[i];
    const VkrEntityId entity = edit_created_lookup(s, sorted, body->created_id);
    VkrSceneEditValues values;
    MemZero(&values, sizeof(values));
    values.fields = VKR_SCENE_EDIT_PHYSICS;
    values.physics = body->physics;
    if (!vkr_scene_entity_alive(scene, entity) ||
        !edit_write(scene, entity, &values)) {
      log_warn("Overlay object %u loaded without its physics body",
               body->created_id);
    }
  }
  if (sorted) {
    vkr_allocator_free(s->allocator, sorted, sorted_size, EDIT_TAG);
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
  uint32_t rebound = 0u;
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
          if (!edit_load_bind_source(scene, source, fingerprint, &values,
                                     &rebound, &diagnostic.failure))
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
      if ((root_seen & 16u) ||
          !edit_json_scene_settings(&json, &scene_settings)) {
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
  if (rebound) {
    log_warn("%u overrides of '%s' bound by node name: their source changed "
             "since they were saved",
             rebound, cpath);
  }
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

bool8_t vkr_scene_edit_peek_settings(VkrAllocator *allocator, String8 path,
                                     VkrSceneSettings *out_settings) {
  *out_settings = (VkrSceneSettings){.inherit_world = true_v};
  char cpath[1024];
  if (path.length >= sizeof(cpath)) {
    return false_v;
  }
  MemCopy(cpath, path.str, path.length);
  cpath[path.length] = 0;
  FILE *file = file_fopen(cpath, "rb");
  if (!file) {
    return errno == ENOENT;
  }

  bool8_t success = false_v;
  uint8_t *bytes = NULL;
  long length = 0;
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
    goto cleanup;
  }
  bytes = vkr_allocator_alloc(allocator, (uint64_t)length, EDIT_TAG);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
    goto cleanup;
  }

  /* vkr_scene_edit_load validates the whole file; this reads one object. */
  VkrJsonReader root = vkr_json_reader_create(bytes, (uint64_t)length);
  VkrJsonReader object = {0};
  success = true_v;
  if (vkr_json_find_root_field(&root, "scene_settings") &&
      vkr_json_enter_object(&root, &object)) {
    VkrJsonReader field = object;
    if (vkr_json_find_root_field(&field, "inherit_world")) {
      (void)vkr_json_parse_bool(&field, &out_settings->inherit_world);
    }
    field = object;
    int32_t extent = 0;
    if (vkr_json_find_root_field(&field, "texture_max_extent") &&
        vkr_json_parse_int(&field, &extent) && extent >= 0 &&
        vkr_scene_texture_extent_valid((uint32_t)extent)) {
      out_settings->texture_max_extent = (uint32_t)extent;
    }
  }

cleanup:
  if (bytes) {
    vkr_allocator_free(allocator, bytes, (uint64_t)length, EDIT_TAG);
  }
  fclose(file);
  return success;
}

// =============================================================================
// World partition cells (ADR-086)
// =============================================================================

/* A cell document in memory while it is written. */
typedef struct EditBuffer {
  VkrAllocator *allocator;
  uint8_t *bytes;
  uint64_t length;
  uint64_t capacity;
} EditBuffer;

static bool8_t edit_buffer_sink(void *context, const uint8_t *data,
                                uint64_t size) {
  EditBuffer *buffer = context;
  if (buffer->length + size > buffer->capacity) {
    const uint64_t capacity =
        Max(buffer->capacity * 2u, Max(buffer->length + size, 4096u));
    uint8_t *bytes = vkr_allocator_realloc(
        buffer->allocator, buffer->bytes, buffer->capacity, capacity, EDIT_TAG);
    if (!bytes) {
      return false_v;
    }
    buffer->bytes = bytes;
    buffer->capacity = capacity;
  }
  MemCopy(buffer->bytes + buffer->length, data, size);
  buffer->length += size;
  return true_v;
}

static void edit_buffer_free(EditBuffer *buffer) {
  if (buffer->bytes) {
    vkr_allocator_free(buffer->allocator, buffer->bytes, buffer->capacity,
                       EDIT_TAG);
  }
  MemZero(buffer, sizeof(*buffer));
}

/* The whole file at `path` into `out`; false when it cannot be read. */
static bool8_t edit_read_file(VkrSceneEditState *s, const char *path,
                              EditBuffer *out) {
  *out = (EditBuffer){.allocator = s->allocator};
  FILE *file = file_fopen(path, "rb");
  long length = 0;
  if (!file) {
    return false_v;
  }
  bool8_t ok = fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
               length <= 64 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0;
  if (ok) {
    out->bytes = vkr_allocator_alloc(s->allocator, (uint64_t)length, EDIT_TAG);
    out->capacity = out->bytes ? (uint64_t)length : 0u;
    ok = out->bytes &&
         fread(out->bytes, 1u, (size_t)length, file) == (size_t)length;
    out->length = ok ? (uint64_t)length : 0u;
  }
  fclose(file);
  if (!ok) {
    edit_buffer_free(out);
  }
  return ok;
}

/* Whether the file at `path` holds exactly `buffer`'s bytes. */
static bool8_t edit_file_holds(VkrSceneEditState *s, const char *path,
                               const EditBuffer *buffer) {
  EditBuffer current;
  if (!edit_read_file(s, path, &current)) {
    return false_v;
  }
  const bool8_t same =
      current.length == buffer->length &&
      MemCompare(current.bytes, buffer->bytes, buffer->length) == 0;
  edit_buffer_free(&current);
  return same;
}

static FilePath edit_file_path(const char *path) {
  return (FilePath){
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
}

static bool8_t edit_temp_path(const char *path, char *out, uint32_t capacity) {
  const int written = snprintf(out, capacity, "%s.tmp", path);
  return written > 0 && (uint32_t)written < capacity;
}

/* Writes `buffer` to `<path>.tmp` and stores it on the disk, unless `path`
   already holds it; `*out_staged` says whether a file waits to replace
   `path`. */
static bool8_t edit_stage_file(VkrSceneEditState *s, const char *path,
                               const EditBuffer *buffer, bool8_t *out_staged) {
  *out_staged = false_v;
  if (edit_file_holds(s, path, buffer)) {
    return true_v;
  }
  char temp[1100];
  if (!edit_temp_path(path, temp, sizeof(temp))) {
    return false_v;
  }
  FILE *file = file_fopen(temp, "wb");
  if (!file) {
    return false_v;
  }
  bool8_t ok = fwrite(buffer->bytes, 1u, (size_t)buffer->length, file) ==
               (size_t)buffer->length;
  ok = file_flush_durable(file) && ok;
  ok = fclose(file) == 0 && ok;
  if (!ok) {
    const FilePath staged = edit_file_path(temp);
    (void)file_remove(&staged);
    return false_v;
  }
  *out_staged = true_v;
  return true_v;
}

/* Replaces `path` with the file edit_stage_file staged for it. */
static bool8_t edit_promote_file(const char *path) {
  char temp[1100];
  if (!edit_temp_path(path, temp, sizeof(temp))) {
    return false_v;
  }
  const FilePath from = edit_file_path(temp);
  const FilePath to = edit_file_path(path);
  if (file_rename(&from, &to, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&from);
    return false_v;
  }
  return true_v;
}

/* Removes the file edit_stage_file staged for `path`. */
static void edit_discard_file(const char *path) {
  char temp[1100];
  if (edit_temp_path(path, temp, sizeof(temp))) {
    const FilePath staged = edit_file_path(temp);
    (void)file_remove(&staged);
  }
}

static bool8_t edit_cell_path(const VkrSceneEditState *s,
                              VkrScenePartitionCell cell, char *out,
                              uint32_t capacity) {
  const int written =
      snprintf(out, capacity, "%s/%d_%d.json", s->cells_root, cell.x, cell.z);
  return written > 0 && (uint32_t)written < capacity;
}

static bool8_t edit_cell_same(VkrScenePartitionCell a,
                              VkrScenePartitionCell b) {
  return a.x == b.x && a.z == b.z;
}

void vkr_scene_edit_set_cells_root(VkrSceneEditState *s, String8 root) {
  const uint64_t length = Min(root.length, sizeof(s->cells_root) - 1u);
  MemCopy(s->cells_root, root.str, length);
  s->cells_root[length] = '\0';
}

bool8_t vkr_scene_edit_cells_open(VkrSceneEditState *s, VkrScene *scene) {
  SceneWorldPartition settings;
  vkr_scene_partition_reset(scene);
  if (!s->cells_root[0] || !vkr_scene_partition_settings(scene, &settings)) {
    return true_v;
  }
  s->cell_size = settings.cell_size;
  char path[1100];
  snprintf(path, sizeof(path), "%s/index.json", s->cells_root);
  EditBuffer bytes;
  if (!edit_read_file(s, path, &bytes)) {
    return true_v;
  }
  /* {"version":1,"cell_size":N,"next_id":N,"cells":[[x,z],...]}: the
     documents, their cell size and the overlay ids they may use. */
  EditJson j = {.at = bytes.bytes, .end = bytes.bytes + bytes.length};
  int64_t version = 0;
  int64_t next_id = 0;
  float64_t size = 0.0;
  bool8_t ok = edit_json_take(&j, '{') &&
               edit_json_key(&j, "version", false_v) &&
               edit_json_int(&j, 1, 1, &version) &&
               edit_json_key(&j, "cell_size", true_v) &&
               edit_json_number(&j, &size, false_v) && size >= 1.0 &&
               edit_json_key(&j, "next_id", true_v) &&
               edit_json_int(&j, 0, INT32_MAX, &next_id) &&
               edit_json_key(&j, "cells", true_v) && edit_json_take(&j, '[');
  if (ok && !edit_json_take(&j, ']')) {
    for (;;) {
      int64_t x = 0;
      int64_t z = 0;
      ok = edit_json_take(&j, '[') &&
           edit_json_int(&j, INT32_MIN, INT32_MAX, &x) &&
           edit_json_take(&j, ',') &&
           edit_json_int(&j, INT32_MIN, INT32_MAX, &z) &&
           edit_json_take(&j, ']');
      VkrScenePartitionCellRecord *record =
          ok ? vkr_scene_partition_cell(
                   scene, (VkrScenePartitionCell){(int32_t)x, (int32_t)z},
                   true_v)
             : NULL;
      ok = record != NULL;
      if (!ok) {
        break;
      }
      record->flags |= VKR_SCENE_PARTITION_CELL_ON_DISK;
      if (edit_json_take(&j, ']')) {
        break;
      }
      if (!edit_json_take(&j, ',')) {
        ok = false_v;
        break;
      }
    }
  }
  ok = ok && edit_json_take(&j, '}');
  edit_buffer_free(&bytes);
  if (!ok) {
    vkr_scene_partition_reset(scene);
    snprintf(s->status, sizeof(s->status), "The cell index is unreadable.");
    log_error("World partition: %s is not a cell index", path);
    return false_v;
  }
  /* Cells of another size load under their old names and save anew. */
  s->cell_size = (float32_t)size;
  s->next_created_id = Max(s->next_created_id, (uint32_t)next_id);
  return true_v;
}

/* Keeps each staged object's overlay id unless a loaded object holds it,
   then gives it a fresh one, followed in the links and bodies. The index's
   high-water id keeps new objects off the ids of unloaded cells, so a
   remap is rare and only rewrites the cell it happened in. */
static void edit_cell_remap_ids(VkrSceneEditState *s, EditStructureLoad *load) {
  /* Loaded ids are searched in a sorted copy: a cell of tens of thousands of
     objects beside as many loaded ones would compare every pair. A failed
     copy falls back to the scan. */
  const uint64_t loaded_size = (uint64_t)s->created_count * sizeof(uint32_t);
  uint32_t *loaded =
      loaded_size ? vkr_allocator_alloc(s->allocator, loaded_size, EDIT_TAG)
                  : NULL;
  if (loaded) {
    for (uint32_t c = 0; c < s->created_count; ++c) {
      loaded[c] = s->created[c].id;
    }
    qsort(loaded, s->created_count, sizeof(*loaded), edit_u32_compare);
  }

  for (uint32_t i = 0; i < load->object_count; ++i) {
    const uint32_t from = load->objects[i].created_id;
    bool8_t taken = false_v;
    if (loaded) {
      taken = bsearch(&from, loaded, s->created_count, sizeof(*loaded),
                      edit_u32_compare) != NULL;
    }
    for (uint32_t c = 0; !loaded && c < s->created_count && !taken; ++c) {
      taken = s->created[c].id == from;
    }
    if (!taken) {
      continue;
    }
    const uint32_t to = s->next_created_id++;
    load->objects[i].created_id = to;
    for (uint32_t l = 0; l < load->link_count; ++l) {
      EditParentLink *link = &load->links[l];
      if (link->created_child == from) {
        link->created_child = to | 0x80000000u;
      }
      if (link->parent.kind == EDIT_PARENT_CREATED &&
          link->parent.created == from) {
        link->parent.created = to | 0x80000000u;
      }
    }
    for (uint32_t b = 0; b < load->body_count; ++b) {
      if (load->bodies[b].created_id == from) {
        load->bodies[b].created_id = to | 0x80000000u;
      }
    }
  }
  /* The high bit kept remapped ids apart from file ids until all moved. */
  for (uint32_t l = 0; l < load->link_count; ++l) {
    load->links[l].created_child &= 0x7FFFFFFFu;
    load->links[l].parent.created &= 0x7FFFFFFFu;
  }
  for (uint32_t b = 0; b < load->body_count; ++b) {
    load->bodies[b].created_id &= 0x7FFFFFFFu;
  }
  if (loaded) {
    vkr_allocator_free(s->allocator, loaded, loaded_size, EDIT_TAG);
  }
}

/* Whether staged object `id` has a parent. */
static bool8_t edit_staged_child(const EditStructureLoad *load, uint32_t id) {
  for (uint32_t l = 0; l < load->link_count; ++l) {
    if (load->links[l].created_child == id &&
        (load->links[l].parent.kind == EDIT_PARENT_CREATED ||
         load->links[l].parent.kind == EDIT_PARENT_DOCUMENT)) {
      return true_v;
    }
  }
  return false_v;
}

/* Moves the staged root objects by -`offset`: a world whose origin is
   rebased (ADR-086) holds its roots that far from their documents. */
static void edit_cell_rebase(VkrSceneEditState *s, EditStructureLoad *load,
                             Vec3 offset) {
  if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) {
    return;
  }
  /* The ids of objects with a parent, sorted, so each object is one search
     rather than a scan of every link. A failed copy falls back to the
     scan. */
  const uint64_t children_size = (uint64_t)load->link_count * sizeof(uint32_t);
  uint32_t *children =
      children_size ? vkr_allocator_alloc(s->allocator, children_size, EDIT_TAG)
                    : NULL;
  uint32_t child_count = 0u;
  for (uint32_t l = 0; children && l < load->link_count; ++l) {
    if (load->links[l].created_child &&
        (load->links[l].parent.kind == EDIT_PARENT_CREATED ||
         load->links[l].parent.kind == EDIT_PARENT_DOCUMENT)) {
      children[child_count++] = load->links[l].created_child;
    }
  }
  if (children) {
    qsort(children, child_count, sizeof(*children), edit_u32_compare);
  }

  for (uint32_t i = 0; i < load->object_count; ++i) {
    const uint32_t id = load->objects[i].created_id;
    const bool8_t child =
        children ? bsearch(&id, children, child_count, sizeof(*children),
                           edit_u32_compare) != NULL
                 : edit_staged_child(load, id);
    if (!child) {
      /* The packed fields keep EditObject's layout. */
      uint8_t *at = load->packed + load->objects[i].offset +
                    offsetof(EditObject, transform);
      SceneTransform transform;
      MemCopy(&transform, at, sizeof(transform));
      transform.position = vec3_sub(transform.position, offset);
      MemCopy(at, &transform, sizeof(transform));
    }
  }
  if (children) {
    vkr_allocator_free(s->allocator, children, children_size, EDIT_TAG);
  }
}

bool8_t vkr_scene_edit_cell_load(VkrSceneEditState *s, VkrScene *scene,
                                 VkrScenePartitionCell cell) {
  VkrScenePartitionCellRecord *record =
      vkr_scene_partition_cell(scene, cell, true_v);
  char path[1100];
  if (!record || !edit_cell_path(s, cell, path, sizeof(path))) {
    return false_v;
  }
  if (record->flags & VKR_SCENE_PARTITION_CELL_LOADED) {
    return true_v;
  }
  const FilePath document = edit_file_path(path);
  if ((record->flags & VKR_SCENE_PARTITION_CELL_ON_DISK) &&
      !file_exists(&document)) {
    /* The index outlived the document, as after a save cut short between
       removing an emptied cell and rewriting the index. */
    log_warn("World partition: %s is listed but missing; the cell is empty",
             path);
    record->flags &= ~(uint32_t)(VKR_SCENE_PARTITION_CELL_ON_DISK |
                                 VKR_SCENE_PARTITION_CELL_UNREADABLE);
  }
  EditBuffer bytes = {0};
  if (!(record->flags & VKR_SCENE_PARTITION_CELL_ON_DISK)) {
    /* No document: the cell starts empty. */
    record->flags |= VKR_SCENE_PARTITION_CELL_LOADED;
    return true_v;
  }
  if (!edit_read_file(s, path, &bytes)) {
    record->flags |= VKR_SCENE_PARTITION_CELL_UNREADABLE;
    snprintf(s->status, sizeof(s->status), "Cell %d,%d is unreadable.", cell.x,
             cell.z);
    log_error("World partition: %s could not be read", path);
    return false_v;
  }
  EditRecordComponents *components =
      vkr_allocator_alloc(s->allocator, sizeof(*components), EDIT_TAG);
  EditStructureLoad load = {0};
  EditSidecarFailure failure = EDIT_SIDECAR_FAILURE_SCHEMA;
  /* {"version":1,"cell":[x,z],"created":[...]} */
  EditJson j = {.at = bytes.bytes, .end = bytes.bytes + bytes.length};
  int64_t version = 0;
  int64_t x = 0;
  int64_t z = 0;
  bool8_t ok =
      components && edit_json_take(&j, '{') &&
      edit_json_key(&j, "version", false_v) &&
      edit_json_int(&j, 1, 1, &version) && edit_json_key(&j, "cell", true_v) &&
      edit_json_take(&j, '[') && edit_json_int(&j, INT32_MIN, INT32_MAX, &x) &&
      edit_json_take(&j, ',') && edit_json_int(&j, INT32_MIN, INT32_MAX, &z) &&
      edit_json_take(&j, ']') && x == cell.x && z == cell.z &&
      edit_json_key(&j, "created", true_v) &&
      edit_json_created(&j, s, &load, components, &failure) &&
      edit_json_take(&j, '}');
  edit_json_space(&j);
  ok = ok && j.at == j.end;
  if (ok) {
    edit_cell_remap_ids(s, &load);
    edit_cell_rebase(s, &load, scene->origin_offset);
    edit_structure_load_commit(s, scene, &load, NULL, 0u);
    record->flags =
        (record->flags & ~(uint32_t)VKR_SCENE_PARTITION_CELL_UNREADABLE) |
        VKR_SCENE_PARTITION_CELL_LOADED;
  } else {
    record->flags |= VKR_SCENE_PARTITION_CELL_UNREADABLE;
    snprintf(s->status, sizeof(s->status), "Cell %d,%d is unreadable.", cell.x,
             cell.z);
    log_error("World partition: %s is not a cell document", path);
  }
  edit_structure_load_free(s, &load);
  if (components) {
    vkr_allocator_free(s->allocator, components, sizeof(*components), EDIT_TAG);
  }
  edit_buffer_free(&bytes);
  return ok;
}

/* Whether created entity `entity` streams with `cell`. */
static bool8_t edit_cell_member(const VkrScene *scene,
                                const SceneWorldPartition *settings,
                                VkrEntityId entity,
                                VkrScenePartitionCell cell) {
  VkrScenePartitionCell at;
  return vkr_scene_entity_alive(scene, entity) &&
         vkr_scene_partition_entity_cell(scene, settings, entity, &at) &&
         edit_cell_same(at, cell);
}

static bool8_t
edit_cell_matches_document(VkrSceneEditState *s, const VkrScene *scene,
                           const SceneWorldPartition *settings,
                           const VkrScenePartitionCellRecord *record);

bool8_t vkr_scene_edit_cell_unloadable(VkrSceneEditState *s, VkrScene *scene,
                                       VkrScenePartitionCell cell) {
  SceneWorldPartition settings;
  if (!vkr_scene_partition_settings(scene, &settings)) {
    return false_v;
  }
  /* Undo and redo must find every entity they name. */
  for (uint32_t i = 0; i < s->undo_count; ++i) {
    const VkrSceneEditEntry *entry = &s->undo[i];
    if (edit_cell_member(scene, &settings, entry->entity, cell)) {
      return false_v;
    }
    if (entry->kind == VKR_SCENE_EDIT_ENTRY_STRUCTURE) {
      const EditStructure *structure = entry->payload;
      if (edit_cell_member(scene, &settings, structure->parent[0], cell) ||
          edit_cell_member(scene, &settings, structure->parent[1], cell)) {
        return false_v;
      }
    }
  }
  if (s->revision == s->saved_revision) {
    return true_v;
  }
  /* With unsaved edits elsewhere, a cell still goes when it holds what its
     document does; the answer stands until the next edit. */
  VkrScenePartitionCellRecord *record =
      vkr_scene_partition_cell(scene, cell, false_v);
  if (!record || (record->flags & VKR_SCENE_PARTITION_CELL_STALE)) {
    return false_v;
  }
  if (record->checked_revision != s->revision + 1u) {
    record->checked_clean =
        edit_cell_matches_document(s, scene, &settings, record);
    record->checked_revision = s->revision + 1u;
  }
  return record->checked_clean;
}

typedef struct EditCellDoomed {
  VkrEntityId entity;
  uint32_t depth;
} EditCellDoomed;

static int edit_cell_doomed_compare(const void *a, const void *b) {
  const uint32_t da = ((const EditCellDoomed *)a)->depth;
  const uint32_t db = ((const EditCellDoomed *)b)->depth;
  return da > db ? -1 : da < db ? 1 : 0;
}

static uint32_t edit_entity_depth(const VkrScene *scene, VkrEntityId entity) {
  uint32_t depth = 0u;
  for (; depth < 256u; ++depth) {
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, entity, scene->comp_transform);
    if (!transform || !transform->parent.u64 ||
        !vkr_scene_entity_alive(scene, transform->parent)) {
      break;
    }
    entity = transform->parent;
  }
  return depth;
}

void vkr_scene_edit_cell_unload(VkrSceneEditState *s, VkrScene *scene,
                                VkrScenePartitionCell cell) {
  VkrScenePartitionCellRecord *record =
      vkr_scene_partition_cell(scene, cell, false_v);
  SceneWorldPartition settings;
  if (!record || !vkr_scene_partition_settings(scene, &settings)) {
    return;
  }
  /* Membership follows roots, so it is settled before anything goes;
     children go before their parents. */
  EditCellDoomed *doomed =
      s->created_count
          ? vkr_allocator_alloc(s->allocator,
                                s->created_count * sizeof(*doomed), EDIT_TAG)
          : NULL;
  uint32_t count = 0u;
  uint32_t kept = 0u;
  for (uint32_t i = 0; i < s->created_count; ++i) {
    const VkrEntityId entity = s->created[i].entity;
    if (doomed && edit_cell_member(scene, &settings, entity, cell)) {
      doomed[count++] =
          (EditCellDoomed){entity, edit_entity_depth(scene, entity)};
      continue;
    }
    s->created[kept++] = s->created[i];
  }
  if (!doomed && s->created_count) {
    return;
  }
  s->created_count = kept;
  qsort(doomed, count, sizeof(*doomed), edit_cell_doomed_compare);
  for (uint32_t i = 0; i < count; ++i) {
    vkr_scene_destroy_entity(scene, doomed[i].entity);
  }
  if (doomed) {
    vkr_allocator_free(s->allocator, doomed,
                       (uint64_t)(kept + count) * sizeof(*doomed), EDIT_TAG);
  }
  record->flags &= ~(uint32_t)VKR_SCENE_PARTITION_CELL_LOADED;
}

bool8_t vkr_scene_edit_cells_track(VkrSceneEditState *s, VkrScene *scene) {
  SceneWorldPartition settings;
  if (!s->cells_root[0] || !vkr_scene_partition_settings(scene, &settings)) {
    return true_v;
  }
  bool8_t ok = true_v;
  if (settings.cell_size != s->cell_size) {
    /* A new cell size moves every object: all cells load, and the old
       documents give way to new ones at the next save. */
    uint32_t count = 0u;
    vkr_scene_partition_cells(scene, &count);
    for (uint32_t i = 0; i < count; ++i) {
      const VkrScenePartitionCellRecord *records =
          vkr_scene_partition_cells(scene, &count);
      if ((records[i].flags & VKR_SCENE_PARTITION_CELL_ON_DISK) &&
          !(records[i].flags & VKR_SCENE_PARTITION_CELL_LOADED)) {
        ok = vkr_scene_edit_cell_load(s, scene, records[i].cell) && ok;
      }
    }
    for (uint32_t i = 0; i < count; ++i) {
      VkrScenePartitionCellRecord *record = vkr_scene_partition_cell(
          scene, vkr_scene_partition_cells(scene, &count)[i].cell, false_v);
      record->flags = (record->flags & VKR_SCENE_PARTITION_CELL_ON_DISK)
                          ? VKR_SCENE_PARTITION_CELL_STALE
                          : 0u;
    }
    s->cell_size = settings.cell_size;
  }
  /* A cell holding objects is loaded; one with a document merges it
     first. */
  for (uint32_t i = 0; i < s->created_count; ++i) {
    VkrScenePartitionCell cell;
    if (!vkr_scene_entity_alive(scene, s->created[i].entity) ||
        !vkr_scene_partition_entity_cell(scene, &settings, s->created[i].entity,
                                         &cell)) {
      continue;
    }
    VkrScenePartitionCellRecord *record =
        vkr_scene_partition_cell(scene, cell, true_v);
    if (!record || (record->flags & VKR_SCENE_PARTITION_CELL_LOADED)) {
      continue;
    }
    if (record->flags & VKR_SCENE_PARTITION_CELL_STALE) {
      record->flags |= VKR_SCENE_PARTITION_CELL_LOADED;
      continue;
    }
    ok = vkr_scene_edit_cell_load(s, scene, cell) && ok;
  }
  return ok;
}

typedef struct EditCellMember {
  VkrScenePartitionCell cell;
  uint32_t created;
} EditCellMember;

static int edit_cell_member_compare(const void *a, const void *b) {
  const EditCellMember *ma = a;
  const EditCellMember *mb = b;
  if (ma->cell.x != mb->cell.x) {
    return ma->cell.x < mb->cell.x ? -1 : 1;
  }
  if (ma->cell.z != mb->cell.z) {
    return ma->cell.z < mb->cell.z ? -1 : 1;
  }
  return ma->created < mb->created ? -1 : ma->created > mb->created ? 1 : 0;
}

static int edit_cell_record_compare(const void *a, const void *b) {
  const VkrScenePartitionCellRecord *ra = a;
  const VkrScenePartitionCellRecord *rb = b;
  if (ra->cell.x != rb->cell.x) {
    return ra->cell.x < rb->cell.x ? -1 : 1;
  }
  return ra->cell.z < rb->cell.z ? -1 : ra->cell.z > rb->cell.z ? 1 : 0;
}

/* One cell's document, its members `members` in overlay order. */
static bool8_t edit_cell_document(VkrSceneEditState *s, const VkrScene *scene,
                                  VkrScenePartitionCell cell,
                                  const EditCellMember *members, uint32_t count,
                                  EditBuffer *out) {
  VkrJsonWriter writer;
  VkrJsonWriter *w = &writer;
  vkr_json_writer_init(w, edit_buffer_sink, out);
  if (!vkr_json_writer_begin_object(w) || !WRITE_INT("version", 1) ||
      !vkr_json_writer_name(w, string8_lit("cell")) ||
      !vkr_json_writer_begin_array(w) || !vkr_json_writer_i64(w, cell.x) ||
      !vkr_json_writer_i64(w, cell.z) || !vkr_json_writer_end_array(w) ||
      !vkr_json_writer_name(w, string8_lit("created")) ||
      !vkr_json_writer_begin_array(w)) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!write_created(w, s, scene, members[i].created)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(w) && vkr_json_writer_end_object(w) &&
         vkr_json_writer_complete(w);
}

static bool8_t
edit_cell_matches_document(VkrSceneEditState *s, const VkrScene *scene,
                           const SceneWorldPartition *settings,
                           const VkrScenePartitionCellRecord *record) {
  EditCellMember *members =
      s->created_count
          ? vkr_allocator_alloc(s->allocator,
                                s->created_count * sizeof(*members), EDIT_TAG)
          : NULL;
  if (s->created_count && !members) {
    return false_v;
  }
  /* Members in overlay order, as a save writes them. */
  uint32_t count = 0u;
  for (uint32_t i = 0; i < s->created_count; ++i) {
    if (edit_cell_member(scene, settings, s->created[i].entity, record->cell)) {
      members[count++] = (EditCellMember){record->cell, i};
    }
  }
  const bool8_t on_disk = record->flags & VKR_SCENE_PARTITION_CELL_ON_DISK;
  bool8_t same = !count && !on_disk;
  char path[1100];
  if (count && on_disk && edit_cell_path(s, record->cell, path, sizeof(path))) {
    EditBuffer buffer = {.allocator = s->allocator};
    same =
        edit_cell_document(s, scene, record->cell, members, count, &buffer) &&
        edit_file_holds(s, path, &buffer);
    edit_buffer_free(&buffer);
  }
  if (members) {
    vkr_allocator_free(s->allocator, members,
                       s->created_count * sizeof(*members), EDIT_TAG);
  }
  return same;
}

static void edit_cells_save_free(VkrSceneEditState *s, EditCellsSave *save) {
  if (save->changes) {
    vkr_allocator_free(s->allocator, save->changes,
                       save->change_capacity * sizeof(*save->changes),
                       EDIT_TAG);
  }
  MemZero(save, sizeof(*save));
}

/* Removes every staged file and forgets the save. */
static void edit_cells_discard(VkrSceneEditState *s, VkrScene *scene,
                               EditCellsSave *save) {
  uint32_t record_count = 0u;
  const VkrScenePartitionCellRecord *records =
      vkr_scene_partition_cells(scene, &record_count);
  for (uint32_t i = 0; i < save->change_count; ++i) {
    char path[1100];
    if (save->changes[i].staged &&
        edit_cell_path(s, records[save->changes[i].record].cell, path,
                       sizeof(path))) {
      edit_discard_file(path);
    }
  }
  if (save->index_staged) {
    edit_discard_file(save->index_path);
  }
  edit_cells_save_free(s, save);
}

/* Stages the document of every loaded cell whose bytes changed and the
   index listing the documents the save leaves. */
static bool8_t edit_cells_stage(VkrSceneEditState *s, VkrScene *scene,
                                EditCellsSave *save) {
  MemZero(save, sizeof(*save));
  SceneWorldPartition settings;
  if (!s->cells_root[0] || !vkr_scene_partition_settings(scene, &settings)) {
    return true_v;
  }
  save->active = true_v;
  const String8 root = string8_create_from_cstr((const uint8_t *)s->cells_root,
                                                strlen(s->cells_root));
  VkrAllocator *allocator = s->allocator;
  uint32_t record_count = 0u;
  const VkrScenePartitionCellRecord *records =
      vkr_scene_partition_cells(scene, &record_count);
  EditCellMember *members =
      s->created_count
          ? vkr_allocator_alloc(allocator, s->created_count * sizeof(*members),
                                EDIT_TAG)
          : NULL;
  save->changes =
      record_count
          ? vkr_allocator_alloc(allocator,
                                record_count * sizeof(*save->changes), EDIT_TAG)
          : NULL;
  save->change_capacity = save->changes ? record_count : 0u;
  bool8_t ok =
      (!s->created_count || members) && (!record_count || save->changes);
  uint32_t count = 0u;
  for (uint32_t i = 0; ok && i < s->created_count; ++i) {
    VkrScenePartitionCell cell;
    if (vkr_scene_entity_alive(scene, s->created[i].entity) &&
        vkr_scene_partition_entity_cell(scene, &settings, s->created[i].entity,
                                        &cell)) {
      members[count++] = (EditCellMember){cell, i};
    }
  }
  qsort(members, count, sizeof(*members), edit_cell_member_compare);
  /* An object in a cell that did not load would be dropped by the save. */
  bool8_t blocked = false_v;
  for (uint32_t i = 0; ok && i < count; ++i) {
    const VkrScenePartitionCellRecord *record =
        vkr_scene_partition_cell(scene, members[i].cell, false_v);
    if (!record || !(record->flags & (VKR_SCENE_PARTITION_CELL_LOADED |
                                      VKR_SCENE_PARTITION_CELL_STALE))) {
      snprintf(s->status, sizeof(s->status),
               "Save blocked: cell %d,%d holds an object but its document "
               "did not load.",
               members[i].cell.x, members[i].cell.z);
      blocked = true_v;
      ok = false_v;
    }
  }
  /* A cell document loads at most VKR_SCENE_EDIT_CREATED_MAX objects. */
  for (uint32_t i = 0; ok && i < count;) {
    uint32_t end = i + 1u;
    while (end < count && edit_cell_same(members[end].cell, members[i].cell)) {
      end++;
    }
    if (end - i > VKR_SCENE_EDIT_CREATED_MAX) {
      snprintf(s->status, sizeof(s->status),
               "Save blocked: cell %d,%d holds %u created objects; a cell "
               "loads at most %u.",
               members[i].cell.x, members[i].cell.z, end - i,
               VKR_SCENE_EDIT_CREATED_MAX);
      blocked = true_v;
      ok = false_v;
    }
    i = end;
  }
  /* The documents sit in one directory beside the scene's. */
  const FilePath directory = {.path = root, .type = FILE_PATH_TYPE_ABSOLUTE};
  ok = ok && file_create_directory(&directory);
  /* Every loaded cell is written: with its members, or removed when it has
     none. Unloaded cells keep their documents. */
  uint32_t first = 0u;
  for (uint32_t r = 0; ok && r < record_count; ++r) {
    const VkrScenePartitionCellRecord *record = &records[r];
    if (!(record->flags &
          (VKR_SCENE_PARTITION_CELL_LOADED | VKR_SCENE_PARTITION_CELL_STALE))) {
      continue;
    }
    first = 0u;
    while (first < count &&
           edit_cell_member_compare(&members[first],
                                    &(EditCellMember){record->cell, 0u}) < 0) {
      first++;
    }
    uint32_t last = first;
    while (last < count && edit_cell_same(members[last].cell, record->cell)) {
      last++;
    }
    EditCellChange *change = &save->changes[save->change_count++];
    *change = (EditCellChange){
        .record = r,
        .flags =
            record->flags & ~(uint32_t)(VKR_SCENE_PARTITION_CELL_STALE |
                                        VKR_SCENE_PARTITION_CELL_UNREADABLE)};
    if (last == first) {
      change->remove = (record->flags & (VKR_SCENE_PARTITION_CELL_ON_DISK |
                                         VKR_SCENE_PARTITION_CELL_STALE)) != 0u;
      change->flags &= ~(uint32_t)VKR_SCENE_PARTITION_CELL_ON_DISK;
      continue;
    }
    char path[1100];
    EditBuffer buffer = {.allocator = allocator};
    ok = edit_cell_path(s, record->cell, path, sizeof(path)) &&
         edit_cell_document(s, scene, record->cell, members + first,
                            last - first, &buffer) &&
         edit_stage_file(s, path, &buffer, &change->staged);
    edit_buffer_free(&buffer);
    change->flags |= VKR_SCENE_PARTITION_CELL_ON_DISK;
  }
  if (members) {
    vkr_allocator_free(allocator, members, s->created_count * sizeof(*members),
                       EDIT_TAG);
  }
  /* The index lists the documents the save leaves, in cell order. */
  VkrScenePartitionCellRecord *sorted =
      ok && record_count
          ? vkr_allocator_alloc(allocator, record_count * sizeof(*sorted),
                                EDIT_TAG)
          : NULL;
  ok = ok && (!record_count || sorted);
  uint32_t listed = 0u;
  for (uint32_t r = 0, c = 0; ok && r < record_count; ++r) {
    uint32_t flags = records[r].flags;
    if (c < save->change_count && save->changes[c].record == r) {
      flags = save->changes[c++].flags;
    }
    if (flags & VKR_SCENE_PARTITION_CELL_ON_DISK) {
      sorted[listed++] = records[r];
    }
  }
  qsort(sorted, listed, sizeof(*sorted), edit_cell_record_compare);
  EditBuffer index = {.allocator = allocator};
  VkrJsonWriter writer;
  VkrJsonWriter *w = &writer;
  vkr_json_writer_init(w, edit_buffer_sink, &index);
  ok = ok && vkr_json_writer_begin_object(w) && WRITE_INT("version", 1) &&
       vkr_json_writer_name(w, string8_lit("cell_size")) &&
       vkr_json_writer_f64(w, settings.cell_size) &&
       WRITE_INT("next_id", s->next_created_id) &&
       vkr_json_writer_name(w, string8_lit("cells")) &&
       vkr_json_writer_begin_array(w);
  for (uint32_t i = 0; ok && i < listed; ++i) {
    ok = vkr_json_writer_begin_array(w) &&
         vkr_json_writer_i64(w, sorted[i].cell.x) &&
         vkr_json_writer_i64(w, sorted[i].cell.z) &&
         vkr_json_writer_end_array(w);
  }
  ok = ok && vkr_json_writer_end_array(w) && vkr_json_writer_end_object(w) &&
       vkr_json_writer_complete(w) &&
       snprintf(save->index_path, sizeof(save->index_path), "%s/index.json",
                s->cells_root) < (int)sizeof(save->index_path) &&
       edit_stage_file(s, save->index_path, &index, &save->index_staged);
  edit_buffer_free(&index);
  if (sorted) {
    vkr_allocator_free(allocator, sorted, record_count * sizeof(*sorted),
                       EDIT_TAG);
  }
  if (!ok) {
    if (!blocked) {
      snprintf(s->status, sizeof(s->status),
               "Save failed: a cell document could not be written.");
    }
    edit_cells_discard(s, scene, save);
  }
  return ok;
}

/* Replaces the documents the save staged, removes those of emptied cells,
   then replaces the index. */
static bool8_t edit_cells_commit(VkrSceneEditState *s, VkrScene *scene,
                                 EditCellsSave *save) {
  if (!save->active) {
    return true_v;
  }
  uint32_t record_count = 0u;
  vkr_scene_partition_cells(scene, &record_count);
  bool8_t ok = true_v;
  for (uint32_t i = 0; i < save->change_count; ++i) {
    const EditCellChange *change = &save->changes[i];
    VkrScenePartitionCellRecord *record = vkr_scene_partition_cell(
        scene,
        vkr_scene_partition_cells(scene, &record_count)[change->record].cell,
        false_v);
    char path[1100];
    if (!edit_cell_path(s, record->cell, path, sizeof(path))) {
      ok = false_v;
      continue;
    }
    if (change->staged && !edit_promote_file(path)) {
      ok = false_v;
      continue;
    }
    if (change->remove) {
      const FilePath file = edit_file_path(path);
      (void)file_remove(&file);
    }
    record->flags = change->flags;
  }
  ok = (!save->index_staged || edit_promote_file(save->index_path)) && ok;
  edit_cells_save_free(s, save);
  if (!ok) {
    snprintf(s->status, sizeof(s->status),
             "Save failed: a cell document could not be replaced.");
  }
  return ok;
}
