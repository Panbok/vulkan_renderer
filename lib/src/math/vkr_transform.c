#include "vkr_transform.h"
#include "core/logger.h"
#include "math/mat.h"

VkrTransform vkr_transform_new(Vec3 position, VkrQuat rotation, Vec3 scale) {
  VkrTransform transform;
  transform.position = position;
  transform.rotation = vkr_quat_normalize(rotation);
  transform.scale = scale;
  transform.local = mat4_identity();
  transform.parent = NULL;
  transform.is_dirty = false_v;
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
  assert_log(transform != NULL, "Transform is NULL");

  if (!transform)
    return;
  transform->position = vec3_add(transform->position, translation);
  transform->is_dirty = true;
}

void vkr_transform_rotate(VkrTransform *transform, VkrQuat rotation) {
  assert_log(transform != NULL, "Transform is NULL");
  VkrQuat delta = vkr_quat_normalize(rotation);
  transform->rotation =
      vkr_quat_normalize(vkr_quat_mul(transform->rotation, delta));
  transform->is_dirty = true;
}

void vkr_transform_scale(VkrTransform *transform, Vec3 scale) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->scale = vec3_mul(transform->scale, scale);
  transform->is_dirty = true;
}

void vkr_transform_translate_rotate(VkrTransform *transform, Vec3 translation,
                                    VkrQuat rotation) {
  assert_log(transform != NULL, "Transform is NULL");
  vkr_transform_translate(transform, translation);
  vkr_transform_rotate(transform, rotation);
}

void vkr_transform_set_position(VkrTransform *transform, Vec3 position) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->position = position;
  transform->is_dirty = true;
}

void vkr_transform_set_rotation(VkrTransform *transform, VkrQuat rotation) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->rotation = vkr_quat_normalize(rotation);
  transform->is_dirty = true;
}

void vkr_transform_set_scale(VkrTransform *transform, Vec3 scale) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->scale = scale;
  transform->is_dirty = true;
}

void vkr_transform_set_position_rotation(VkrTransform *transform, Vec3 position,
                                         VkrQuat rotation) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->position = position;
  transform->rotation = vkr_quat_normalize(rotation);
  transform->is_dirty = true;
}

void vkr_transform_set_transform(VkrTransform *transform, Vec3 position,
                                 VkrQuat rotation, Vec3 scale) {
  assert_log(transform != NULL, "Transform is NULL");
  transform->position = position;
  transform->rotation = vkr_quat_normalize(rotation);
  transform->scale = scale;
  transform->is_dirty = true;
}

void vkr_transform_set_parent(VkrTransform *transform, VkrTransform *parent) {
  assert_log(transform != NULL, "Transform is NULL");
  assert_log(parent != NULL, "Parent is NULL");

  if (!transform)
    return;

  if (transform->parent == parent)
    return;

  transform->parent = parent;
  transform->is_dirty = true_v;
}

vkr_internal Mat4 vkr_transform_get_world_internal(VkrTransform *transform,
                                                   int depth) {
  assert_log(transform != NULL, "Transform is NULL");

  if (!transform) {
    return mat4_identity();
  }

  // Prevent infinite recursion due to cycles in parent chain
  // Bail out after reasonable depth limit and return identity as fallback
  if (depth > VKR_TRANSFORM_MAX_DEPTH) {
    return mat4_identity();
  }

  Mat4 local = vkr_transform_get_local(transform);
  if (transform->parent) {
    Mat4 parent_world =
        vkr_transform_get_world_internal(transform->parent, depth + 1);
    return mat4_mul(parent_world, local);
  }

  return local;
}

Mat4 vkr_transform_get_world(VkrTransform *transform) {
  assert_log(transform != NULL, "Transform is NULL");
  return vkr_transform_get_world_internal(transform, 0);
}

Mat4 vkr_transform_get_local(VkrTransform *transform) {
  assert_log(transform != NULL, "Transform is NULL");
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
