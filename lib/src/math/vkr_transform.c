#include "vkr_transform.h"
#include "math/mat.h"
#include <stdbool.h>

VkrTransform vkr_transform_new(Vec3 position, VkrQuat rotation, Vec3 scale) {
  VkrTransform transform;
  transform.position = position;
  transform.rotation = vkr_quat_normalize(rotation);
  transform.scale = scale;
  transform.local = mat4_identity();
  transform.parent = NULL;
  return transform;
}

VkrTransform vkr_transform_from_position(Vec3 position) {
  return vkr_transform_new(position, vkr_quat_identity(), vec3_one());
}

VkrTransform vkr_transform_from_rotation(VkrQuat rotation) {
  return vkr_transform_new(vec3_zero(), rotation, vec3_one());
}

VkrTransform vkr_transform_from_scale(Vec3 scale) {
  return vkr_transform_new(vec3_zero(), vkr_quat_identity(), scale);
}

VkrTransform vkr_transform_from_position_rotation(Vec3 position,
                                                  VkrQuat rotation) {
  return vkr_transform_new(position, rotation, vec3_one());
}

VkrTransform vkr_transform_from_position_scale_rotation(Vec3 position,
                                                        Vec3 scale,
                                                        VkrQuat rotation) {
  return vkr_transform_new(position, rotation, scale);
}

VkrTransform vkr_transform_identity() {
  return vkr_transform_new(vec3_zero(), vkr_quat_identity(), vec3_one());
}

void vkr_transform_translate(VkrTransform *transform, Vec3 translation) {
  transform->position = vec3_add(transform->position, translation);
  transform->is_dirty = true;
}

void vkr_transform_rotate(VkrTransform *transform, VkrQuat rotation) {
  VkrQuat delta = vkr_quat_normalize(rotation);
  transform->rotation =
      vkr_quat_normalize(vkr_quat_mul(transform->rotation, delta));
  transform->is_dirty = true;
}

void vkr_transform_scale(VkrTransform *transform, Vec3 scale) {
  transform->scale = vec3_mul(transform->scale, scale);
  transform->is_dirty = true;
}

void vkr_transform_translate_rotate(VkrTransform *transform, Vec3 translation,
                                    VkrQuat rotation) {
  vkr_transform_translate(transform, translation);
  vkr_transform_rotate(transform, rotation);
}

void vkr_transform_set_position(VkrTransform *transform, Vec3 position) {
  transform->position = position;
  transform->is_dirty = true;
}

void vkr_transform_set_rotation(VkrTransform *transform, VkrQuat rotation) {
  transform->rotation = rotation;
  transform->is_dirty = true;
}

void vkr_transform_set_scale(VkrTransform *transform, Vec3 scale) {
  transform->scale = scale;
  transform->is_dirty = true;
}

void vkr_transform_set_position_rotation(VkrTransform *transform, Vec3 position,
                                         VkrQuat rotation) {
  transform->position = position;
  transform->rotation = rotation;
  transform->is_dirty = true;
}

void vkr_transform_set_transform(VkrTransform *transform, Vec3 position,
                                 VkrQuat rotation, Vec3 scale) {
  transform->position = position;
  transform->rotation = rotation;
  transform->scale = scale;
  transform->is_dirty = true;
}

void vkr_transform_set_parent(VkrTransform *transform, VkrTransform *parent) {
  transform->parent = parent;
}

Mat4 vkr_transform_get_world(VkrTransform *transform) {
  if (!transform) {
    return mat4_identity();
  }

  Mat4 local = vkr_transform_get_local(transform);
  if (transform->parent) {
    Mat4 parent_world = vkr_transform_get_world(transform->parent);
    return mat4_mul(parent_world, local);
  }

  return local;
}

Mat4 vkr_transform_get_local(VkrTransform *transform) {
  if (!transform) {
    return mat4_identity();
  }

  if (transform->is_dirty) {
    Mat4 local = mat4_mul(mat4_translate(transform->position),
                          vkr_quat_to_mat4(transform->rotation));
    local = mat4_mul(local, mat4_scale(transform->scale));
    transform->local = local;
    transform->is_dirty = false;
  }

  return transform->local;
}
