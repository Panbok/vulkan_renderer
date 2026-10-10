#include "vkr_net_scene_edit.h"

#include "core/vkr_byte_io.h"
#include "renderer/systems/vkr_scene_types.h"
#include "vkr_net_type.h"

#include <stdio.h>
#include <string.h>

#define NET_EDIT_ACTION_BITS 5u
#define NET_EDIT_TYPE_NAME_MAX 64u
/* Raw arrays start on this boundary from the start of the encoded bytes,
   so a reader of bytes aligned as well borrows them in place. */
#define NET_EDIT_ARRAY_ALIGN 16u
/* Largest stamp image side and road point count that travel. */
#define NET_EDIT_IMAGE_SIDE_MAX 4096u
#define NET_EDIT_PATH_MAX 65536u

/* How an entity travels: none, an earlier edit of the batch, or a world id
   and document id. */
enum {
  NET_EDIT_ENTITY_NONE = 0,
  NET_EDIT_ENTITY_BATCH = 1,
  NET_EDIT_ENTITY_REF = 2,
};

static const uint32_t net_edit_value_fields =
    VKR_SCENE_EDIT_TRANSFORM | VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_VISIBILITY |
    VKR_SCENE_EDIT_POINT_LIGHT | VKR_SCENE_EDIT_DIRECTIONAL_LIGHT |
    VKR_SCENE_EDIT_RECTANGLE_LIGHT | VKR_SCENE_EDIT_COMPONENT;

bool8_t vkr_net_scene_edit_supported(const VkrSceneEditRequest *request) {
  switch (request->action) {
  case VKR_SCENE_EDIT_APPLY:
  case VKR_SCENE_EDIT_CREATE:
    return (request->values.fields & ~net_edit_value_fields) == 0u;
  case VKR_SCENE_EDIT_TERRAIN:
    /* Results travel apart from the request (vkr_net_scene_edit_samples). */
    return request->terrain.kind < VKR_HEIGHTFIELD_OP_SAMPLES;
  case VKR_SCENE_EDIT_DELETE:
  case VKR_SCENE_EDIT_REPARENT:
  case VKR_SCENE_EDIT_DUPLICATE:
  case VKR_SCENE_EDIT_ADD_COMPONENT:
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
  case VKR_SCENE_EDIT_REPLACE_COMPONENT:
    return true_v;
  default:
    return false_v;
  }
}

static const VkrScene *net_edit_container(const VkrNetSceneEditScenes *scenes,
                                          uint32_t world) {
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    return scenes->world;
  }
  return world <= VKR_SCENE_ADDITIVE_MAX ? scenes->containers[world] : NULL;
}

/* A component type by the name both peers register. */
static const VkrTypeDesc *net_edit_type_named(const char *name) {
  const String8 key =
      string8_create_from_cstr((const uint8_t *)name, strlen(name));
  const VkrTypeDesc *type = vkr_scene_world_type_named(key);
  if (type) {
    return type;
  }
  for (uint32_t i = 0u; (type = vkr_scene_edit_component_type(i)); ++i) {
    if (strcmp(type->name, name) == 0) {
      return type;
    }
  }
  return NULL;
}

static bool8_t net_edit_write_type_name(VkrBitWriter *writer,
                                        const VkrTypeDesc *type) {
  const uint32_t length = type ? (uint32_t)strlen(type->name) : 0u;
  if (!type || length == 0u || length >= NET_EDIT_TYPE_NAME_MAX) {
    return false_v;
  }
  vkr_bit_write_varuint(writer, length);
  vkr_bit_write_bytes(writer, type->name, length);
  return true_v;
}

static const VkrTypeDesc *
net_edit_read_type_name(VkrBitReader *reader, char *error, uint32_t capacity) {
  const uint64_t length = vkr_bit_read_varuint(reader);
  if (length == 0u || length >= NET_EDIT_TYPE_NAME_MAX) {
    snprintf(error, capacity, "malformed component type name");
    return NULL;
  }
  const uint8_t *bytes = vkr_bit_read_bytes(reader, (uint32_t)length);
  if (!bytes) {
    snprintf(error, capacity, "edit ends early");
    return NULL;
  }
  char name[NET_EDIT_TYPE_NAME_MAX];
  memcpy(name, bytes, (size_t)length);
  name[length] = '\0';
  const VkrTypeDesc *type = net_edit_type_named(name);
  if (!type) {
    snprintf(error, capacity, "unknown component type %s", name);
  }
  return type;
}

static bool8_t net_edit_write_entity(VkrBitWriter *writer,
                                     const VkrNetSceneEditScenes *scenes,
                                     VkrEntityId entity, int32_t batch_ref,
                                     char *error, uint32_t capacity) {
  if (batch_ref >= 0) {
    vkr_bit_write(writer, NET_EDIT_ENTITY_BATCH, 2u);
    vkr_bit_write_varuint(writer, (uint64_t)batch_ref);
    return true_v;
  }
  if (!entity.u64) {
    vkr_bit_write(writer, NET_EDIT_ENTITY_NONE, 2u);
    return true_v;
  }
  const VkrScene *scene = net_edit_container(scenes, entity.parts.world);
  VkrEntityRef ref;
  if (!scene || !vkr_scene_entity_ref(scene, entity, &ref) ||
      vkr_entity_ref_empty(&ref)) {
    snprintf(error, capacity, "entity %u:%u has no document id",
             entity.parts.world, entity.parts.index);
    return false_v;
  }
  vkr_bit_write(writer, NET_EDIT_ENTITY_REF, 2u);
  vkr_bit_write(writer, entity.parts.world, 16u);
  vkr_bit_write(writer, vkr_load_le_u64(ref.bytes), 64u);
  vkr_bit_write(writer, vkr_load_le_u64(ref.bytes + 8), 64u);
  return true_v;
}

static bool8_t net_edit_read_entity(VkrBitReader *reader,
                                    const VkrNetSceneEditScenes *scenes,
                                    VkrEntityId *out_entity,
                                    int32_t *out_batch_ref, char *error,
                                    uint32_t capacity) {
  *out_entity = VKR_ENTITY_ID_INVALID;
  *out_batch_ref = -1;
  const uint32_t form = (uint32_t)vkr_bit_read(reader, 2u);
  if (form == NET_EDIT_ENTITY_NONE) {
    return true_v;
  }
  if (form == NET_EDIT_ENTITY_BATCH) {
    const uint64_t index = vkr_bit_read_varuint(reader);
    if (index >= INT32_MAX) {
      snprintf(error, capacity, "batch reference out of range");
      return false_v;
    }
    *out_batch_ref = (int32_t)index;
    return true_v;
  }
  if (form != NET_EDIT_ENTITY_REF) {
    snprintf(error, capacity, "malformed entity reference");
    return false_v;
  }
  const uint32_t world = (uint32_t)vkr_bit_read(reader, 16u);
  VkrEntityRef ref;
  vkr_store_le_u64(ref.bytes, vkr_bit_read(reader, 64u));
  vkr_store_le_u64(ref.bytes + 8, vkr_bit_read(reader, 64u));
  const VkrScene *scene = net_edit_container(scenes, world);
  const VkrEntityId entity =
      scene ? vkr_scene_find_entity_ref(scene, &ref) : VKR_ENTITY_ID_INVALID;
  if (!entity.u64) {
    char text[37];
    vkr_entity_ref_format(&ref, text);
    snprintf(error, capacity, "no entity %s in container %u", text, world);
    return false_v;
  }
  *out_entity = entity;
  return true_v;
}

static bool8_t net_edit_write_values(VkrBitWriter *writer,
                                     const VkrSceneEditValues *values,
                                     bool8_t create, char *error,
                                     uint32_t capacity) {
  const uint32_t fields = values->fields;
  vkr_bit_write(writer, fields, 8u);
  if (fields & VKR_SCENE_EDIT_NAME) {
    const uint8_t *end = memchr(values->name, 0, sizeof(values->name));
    if (!end) {
      snprintf(error, capacity, "name has no terminator");
      return false_v;
    }
    const uint32_t length = (uint32_t)(end - (const uint8_t *)values->name);
    vkr_bit_write_varuint(writer, length);
    vkr_bit_write_bytes(writer, values->name, length);
  }
  if (fields & VKR_SCENE_EDIT_TRANSFORM) {
    const float32_t floats[10] = {values->position.x, values->position.y,
                                  values->position.z, values->rotation.x,
                                  values->rotation.y, values->rotation.z,
                                  values->rotation.w, values->scale.x,
                                  values->scale.y,    values->scale.z};
    for (uint32_t i = 0u; i < 10u; ++i) {
      vkr_bit_write(writer, vkr_f32_bits(floats[i]), 32u);
    }
  }
  bool8_t ok = true_v;
  if (fields & VKR_SCENE_EDIT_VISIBILITY) {
    ok = ok && vkr_net_type_write(writer, &vkr_scene_visibility_type,
                                  &values->visibility);
  }
  if (fields & VKR_SCENE_EDIT_POINT_LIGHT) {
    ok = ok && vkr_net_type_write(writer, &vkr_scene_point_light_type,
                                  &values->point_light);
  }
  if (fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    ok = ok && vkr_net_type_write(writer, &vkr_scene_directional_light_type,
                                  &values->directional_light);
  }
  if (fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    ok = ok && vkr_net_type_write(writer, &vkr_scene_rectangle_light_type,
                                  &values->rectangle_light);
  }
  if (fields & VKR_SCENE_EDIT_COMPONENT) {
    ok = ok && net_edit_write_type_name(writer, values->component_type) &&
         vkr_net_type_write(writer, values->component_type, values->component);
  }
  if (!ok) {
    snprintf(error, capacity, "a value cannot travel");
    return false_v;
  }
  if (create) {
    if (vkr_entity_ref_empty(&values->ref)) {
      snprintf(error, capacity, "a creation needs its document id");
      return false_v;
    }
    vkr_bit_write(writer, vkr_load_le_u64(values->ref.bytes), 64u);
    vkr_bit_write(writer, vkr_load_le_u64(values->ref.bytes + 8), 64u);
  }
  return true_v;
}

static bool8_t net_edit_read_values(VkrBitReader *reader,
                                    VkrSceneEditValues *values, bool8_t create,
                                    char *error, uint32_t capacity) {
  values->fields = (uint32_t)vkr_bit_read(reader, 8u);
  if (values->fields & ~net_edit_value_fields) {
    snprintf(error, capacity, "unsupported edit fields");
    return false_v;
  }
  if (values->fields & VKR_SCENE_EDIT_NAME) {
    const uint64_t length = vkr_bit_read_varuint(reader);
    const uint8_t *bytes = length < sizeof(values->name)
                               ? vkr_bit_read_bytes(reader, (uint32_t)length)
                               : NULL;
    if (!bytes || memchr(bytes, 0, (size_t)length)) {
      snprintf(error, capacity, "malformed name");
      return false_v;
    }
    memcpy(values->name, bytes, (size_t)length);
    values->name[length] = '\0';
  }
  if (values->fields & VKR_SCENE_EDIT_TRANSFORM) {
    float32_t floats[10];
    for (uint32_t i = 0u; i < 10u; ++i) {
      floats[i] = vkr_f32_from_bits((uint32_t)vkr_bit_read(reader, 32u));
    }
    values->position = vec3_new(floats[0], floats[1], floats[2]);
    values->rotation = vkr_quat_new(floats[3], floats[4], floats[5], floats[6]);
    values->scale = vec3_new(floats[7], floats[8], floats[9]);
  }
  if ((values->fields & VKR_SCENE_EDIT_VISIBILITY) &&
      !vkr_net_type_read(reader, &vkr_scene_visibility_type,
                         &values->visibility, error, capacity)) {
    return false_v;
  }
  if ((values->fields & VKR_SCENE_EDIT_POINT_LIGHT) &&
      !vkr_net_type_read(reader, &vkr_scene_point_light_type,
                         &values->point_light, error, capacity)) {
    return false_v;
  }
  if ((values->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) &&
      !vkr_net_type_read(reader, &vkr_scene_directional_light_type,
                         &values->directional_light, error, capacity)) {
    return false_v;
  }
  if ((values->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) &&
      !vkr_net_type_read(reader, &vkr_scene_rectangle_light_type,
                         &values->rectangle_light, error, capacity)) {
    return false_v;
  }
  if (values->fields & VKR_SCENE_EDIT_COMPONENT) {
    values->component_type = net_edit_read_type_name(reader, error, capacity);
    if (!values->component_type ||
        values->component_type->size > sizeof(values->component) ||
        !vkr_net_type_read(reader, values->component_type, values->component,
                           error, capacity)) {
      return false_v;
    }
  }
  if (create) {
    vkr_store_le_u64(values->ref.bytes, vkr_bit_read(reader, 64u));
    vkr_store_le_u64(values->ref.bytes + 8, vkr_bit_read(reader, 64u));
    if (vkr_entity_ref_empty(&values->ref)) {
      snprintf(error, capacity, "a creation needs its document id");
      return false_v;
    }
  }
  if (reader->overflow) {
    snprintf(error, capacity, "edit ends early");
    return false_v;
  }
  return vkr_scene_edit_validate(values);
}

// =============================================================================
// Raw arrays and terrain
// =============================================================================

static void net_edit_write_float(VkrBitWriter *writer, float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_bit_write(writer, bits, 32u);
}

static float32_t net_edit_read_float(VkrBitReader *reader) {
  const uint32_t bits = (uint32_t)vkr_bit_read(reader, 32u);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

void vkr_net_write_array(VkrBitWriter *writer, const void *bytes,
                         uint32_t size) {
  static const uint8_t zeros[NET_EDIT_ARRAY_ALIGN] = {0};
  vkr_bit_write_align(writer);
  const uint32_t pad =
      (NET_EDIT_ARRAY_ALIGN - writer->bytes % NET_EDIT_ARRAY_ALIGN) %
      NET_EDIT_ARRAY_ALIGN;
  vkr_bit_write_bytes(writer, zeros, pad);
  vkr_bit_write_bytes(writer, bytes, size);
}

const void *vkr_net_read_array(VkrBitReader *reader, uint32_t size) {
  vkr_bit_read_align(reader);
  const uint32_t pad =
      (NET_EDIT_ARRAY_ALIGN - reader->bytes % NET_EDIT_ARRAY_ALIGN) %
      NET_EDIT_ARRAY_ALIGN;
  const uint8_t *skipped = vkr_bit_read_bytes(reader, pad);
  const uint8_t *bytes = vkr_bit_read_bytes(reader, size);
  if (!skipped || !bytes ||
      ((uintptr_t)bytes & (NET_EDIT_ARRAY_ALIGN - 1u)) != 0u) {
    reader->overflow = true_v;
    return NULL;
  }
  return bytes;
}

static void net_edit_write_vec3(VkrBitWriter *writer, Vec3 value) {
  net_edit_write_float(writer, value.x);
  net_edit_write_float(writer, value.y);
  net_edit_write_float(writer, value.z);
}

static Vec3 net_edit_read_vec3(VkrBitReader *reader) {
  const float32_t x = net_edit_read_float(reader);
  const float32_t y = net_edit_read_float(reader);
  const float32_t z = net_edit_read_float(reader);
  return vec3_new(x, y, z);
}

/* A terrain op as its parameters, with a stamp's image and a road's points
   as raw arrays. */
static void net_edit_write_terrain(VkrBitWriter *writer,
                                   const VkrHeightfieldOp *op) {
  vkr_bit_write(writer, op->kind, 4u);
  vkr_bit_write(writer, op->brush, 4u);
  vkr_bit_write_varuint(writer, op->layer);
  net_edit_write_vec3(writer, op->a);
  net_edit_write_vec3(writer, op->b);
  net_edit_write_float(writer, op->min.x);
  net_edit_write_float(writer, op->min.y);
  net_edit_write_float(writer, op->max.x);
  net_edit_write_float(writer, op->max.y);
  net_edit_write_float(writer, op->radius);
  net_edit_write_float(writer, op->strength);
  net_edit_write_float(writer, op->height);
  net_edit_write_float(writer, op->width);
  net_edit_write_float(writer, op->falloff);
  vkr_bit_write(writer, op->add, 1u);
  const bool8_t image = op->image && op->image_width && op->image_height;
  vkr_bit_write_varuint(writer, image ? op->image_width : 0u);
  vkr_bit_write_varuint(writer, image ? op->image_height : 0u);
  if (image) {
    vkr_net_write_array(writer, op->image,
                        op->image_width * op->image_height *
                            (uint32_t)sizeof(float32_t));
  }
  const uint32_t path = op->path ? op->path_count : 0u;
  vkr_bit_write_varuint(writer, path);
  if (path) {
    vkr_net_write_array(writer, op->path, path * (uint32_t)sizeof(Vec3));
  }
}

static bool8_t net_edit_read_terrain(VkrBitReader *reader,
                                     VkrHeightfieldOp *out, char *error,
                                     uint32_t capacity) {
  MemZero(out, sizeof(*out));
  out->kind = (VkrHeightfieldOpKind)vkr_bit_read(reader, 4u);
  out->brush = (VkrHeightfieldBrush)vkr_bit_read(reader, 4u);
  out->layer = (uint32_t)vkr_bit_read_varuint(reader);
  out->a = net_edit_read_vec3(reader);
  out->b = net_edit_read_vec3(reader);
  out->min.x = net_edit_read_float(reader);
  out->min.y = net_edit_read_float(reader);
  out->max.x = net_edit_read_float(reader);
  out->max.y = net_edit_read_float(reader);
  out->radius = net_edit_read_float(reader);
  out->strength = net_edit_read_float(reader);
  out->height = net_edit_read_float(reader);
  out->width = net_edit_read_float(reader);
  out->falloff = net_edit_read_float(reader);
  out->add = (bool8_t)vkr_bit_read(reader, 1u);
  const uint64_t image_width = vkr_bit_read_varuint(reader);
  const uint64_t image_height = vkr_bit_read_varuint(reader);
  if (out->kind >= VKR_HEIGHTFIELD_OP_SAMPLES ||
      out->brush >= VKR_HEIGHTFIELD_BRUSH_COUNT ||
      image_width > NET_EDIT_IMAGE_SIDE_MAX ||
      image_height > NET_EDIT_IMAGE_SIDE_MAX ||
      (image_width == 0u) != (image_height == 0u)) {
    snprintf(error, capacity, "a malformed terrain edit");
    return false_v;
  }
  if (image_width) {
    out->image_width = (uint32_t)image_width;
    out->image_height = (uint32_t)image_height;
    out->image =
        vkr_net_read_array(reader, out->image_width * out->image_height *
                                       (uint32_t)sizeof(float32_t));
  }
  const uint64_t path = vkr_bit_read_varuint(reader);
  if (path > NET_EDIT_PATH_MAX) {
    snprintf(error, capacity, "a terrain road with too many points");
    return false_v;
  }
  if (path) {
    out->path_count = (uint32_t)path;
    out->path =
        vkr_net_read_array(reader, out->path_count * (uint32_t)sizeof(Vec3));
  }
  if (reader->overflow || (image_width && !out->image) ||
      (path && !out->path)) {
    snprintf(error, capacity, "a truncated terrain edit");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_net_scene_edit_write(VkrBitWriter *writer,
                                 const VkrNetSceneEditScenes *scenes,
                                 const VkrSceneEditRequest *request,
                                 int32_t entity_ref, int32_t parent_ref,
                                 char *error, uint32_t capacity) {
  if (!vkr_net_scene_edit_supported(request)) {
    snprintf(error, capacity, "this edit cannot travel in a session yet");
    return false_v;
  }
  const VkrSceneEditAction action = request->action;
  vkr_bit_write(writer, (uint32_t)action, NET_EDIT_ACTION_BITS);
  vkr_bit_write_varuint(writer, request->gesture);
  const bool8_t has_entity = action != VKR_SCENE_EDIT_CREATE;
  const bool8_t has_parent =
      action == VKR_SCENE_EDIT_CREATE || action == VKR_SCENE_EDIT_REPARENT;
  if (has_entity && !net_edit_write_entity(writer, scenes, request->entity,
                                           entity_ref, error, capacity)) {
    return false_v;
  }
  if (has_parent && !net_edit_write_entity(writer, scenes, request->parent,
                                           parent_ref, error, capacity)) {
    return false_v;
  }
  if (action == VKR_SCENE_EDIT_CREATE) {
    vkr_bit_write(writer, request->container, 16u);
  }
  switch (action) {
  case VKR_SCENE_EDIT_APPLY:
  case VKR_SCENE_EDIT_CREATE:
    return net_edit_write_values(writer, &request->values,
                                 action == VKR_SCENE_EDIT_CREATE, error,
                                 capacity);
  case VKR_SCENE_EDIT_ADD_COMPONENT:
    return (net_edit_write_type_name(writer, request->values.component_type) &&
            vkr_net_type_write(writer, request->values.component_type,
                               request->values.component)) ||
           (snprintf(error, capacity, "a component cannot travel"), false_v);
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
    return net_edit_write_type_name(writer, request->values.component_type) ||
           (snprintf(error, capacity, "a component type cannot travel"),
            false_v);
  case VKR_SCENE_EDIT_TERRAIN:
    net_edit_write_terrain(writer, &request->terrain);
    return true_v;
  case VKR_SCENE_EDIT_DUPLICATE:
    /* Every editor derives the copies' ids from this seed. */
    if (vkr_entity_ref_empty(&request->values.ref)) {
      snprintf(error, capacity, "a duplicate travels with the seed of its ids");
      return false_v;
    }
    vkr_bit_write(writer, vkr_load_le_u64(request->values.ref.bytes), 64u);
    vkr_bit_write(writer, vkr_load_le_u64(request->values.ref.bytes + 8), 64u);
    return true_v;
  case VKR_SCENE_EDIT_REPLACE_COMPONENT:
    return (net_edit_write_type_name(writer, request->replaced_type) &&
            net_edit_write_type_name(writer, request->values.component_type) &&
            vkr_net_type_write(writer, request->values.component_type,
                               request->values.component)) ||
           (snprintf(error, capacity, "a component cannot travel"), false_v);
  default:
    return true_v;
  }
}

bool8_t vkr_net_scene_edit_read(VkrBitReader *reader,
                                const VkrNetSceneEditScenes *scenes,
                                VkrSceneEditRequest *out_request,
                                int32_t *out_entity_ref,
                                int32_t *out_parent_ref, char *error,
                                uint32_t capacity) {
  VkrSceneEditRequest *request = out_request;
  MemZero(request, sizeof(*request));
  *out_entity_ref = -1;
  *out_parent_ref = -1;
  const uint32_t action = (uint32_t)vkr_bit_read(reader, NET_EDIT_ACTION_BITS);
  request->action = (VkrSceneEditAction)action;
  request->gesture = vkr_bit_read_varuint(reader);
  /* Fields are not read yet, so APPLY and CREATE pass here; their values
     are checked when read. */
  if (!vkr_net_scene_edit_supported(request)) {
    snprintf(error, capacity, "unsupported edit action %u", action);
    return false_v;
  }
  const bool8_t has_entity = action != VKR_SCENE_EDIT_CREATE;
  const bool8_t has_parent =
      action == VKR_SCENE_EDIT_CREATE || action == VKR_SCENE_EDIT_REPARENT;
  if (has_entity && !net_edit_read_entity(reader, scenes, &request->entity,
                                          out_entity_ref, error, capacity)) {
    return false_v;
  }
  if (has_entity && !request->entity.u64 && *out_entity_ref < 0) {
    snprintf(error, capacity, "the edit names no entity");
    return false_v;
  }
  if (has_parent && !net_edit_read_entity(reader, scenes, &request->parent,
                                          out_parent_ref, error, capacity)) {
    return false_v;
  }
  if (action == VKR_SCENE_EDIT_CREATE) {
    request->container = (uint16_t)vkr_bit_read(reader, 16u);
  }
  bool8_t ok = true_v;
  switch (action) {
  case VKR_SCENE_EDIT_APPLY:
  case VKR_SCENE_EDIT_CREATE:
    ok = net_edit_read_values(reader, &request->values,
                              action == VKR_SCENE_EDIT_CREATE, error, capacity);
    break;
  case VKR_SCENE_EDIT_ADD_COMPONENT:
  case VKR_SCENE_EDIT_REPLACE_COMPONENT: {
    if (action == VKR_SCENE_EDIT_REPLACE_COMPONENT) {
      request->replaced_type = net_edit_read_type_name(reader, error, capacity);
      if (!request->replaced_type) {
        return false_v;
      }
    }
    const VkrTypeDesc *type = net_edit_read_type_name(reader, error, capacity);
    ok = type && type->size <= sizeof(request->values.component) &&
         vkr_net_type_read(reader, type, request->values.component, error,
                           capacity);
    request->values.component_type = type;
    request->values.fields = VKR_SCENE_EDIT_COMPONENT;
    break;
  }
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
    request->values.component_type =
        net_edit_read_type_name(reader, error, capacity);
    request->values.fields = VKR_SCENE_EDIT_COMPONENT;
    ok = request->values.component_type != NULL;
    break;
  case VKR_SCENE_EDIT_TERRAIN:
    ok = net_edit_read_terrain(reader, &request->terrain, error, capacity);
    break;
  case VKR_SCENE_EDIT_DUPLICATE:
    vkr_store_le_u64(request->values.ref.bytes, vkr_bit_read(reader, 64u));
    vkr_store_le_u64(request->values.ref.bytes + 8, vkr_bit_read(reader, 64u));
    ok = !vkr_entity_ref_empty(&request->values.ref);
    if (!ok) {
      snprintf(error, capacity, "a duplicate without the seed of its ids");
    }
    break;
  default:
    break;
  }
  if (ok && reader->overflow) {
    snprintf(error, capacity, "edit ends early");
    ok = false_v;
  }
  return ok;
}
