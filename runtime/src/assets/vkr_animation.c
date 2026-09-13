#include "vkr_animation.h"

#include <float.h>
#include <math.h>

static bool8_t animation_fail(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static bool8_t animation_name_valid(String8 name) {
  if (name.length > UINT32_MAX || (name.length && !name.str)) {
    return false_v;
  }
  return !name.length || !memchr(name.str, 0, (size_t)name.length);
}

static bool8_t animation_vec3_finite(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static bool8_t animation_vec4_finite(Vec4 value) {
  return animation_vec3_finite(vec4_to_vec3(value)) && isfinite(value.w);
}

static bool8_t animation_rotation_valid(Vec4 value) {
  const float64_t squared =
      (float64_t)value.x * value.x + (float64_t)value.y * value.y +
      (float64_t)value.z * value.z + (float64_t)value.w * value.w;
  return isfinite(squared) && fabs(squared - 1.0) <= 0.0001;
}

static bool8_t animation_matrix_finite(Mat4 value) {
  for (uint32_t i = 0; i < ArrayCount(value.elements); ++i) {
    if (!isfinite(value.elements[i])) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t animation_matrix_affine(Mat4 value) {
  return animation_matrix_finite(value) && value.m30 == 0.0f &&
         value.m31 == 0.0f && value.m32 == 0.0f && value.m33 == 1.0f;
}

static bool8_t animation_inverse_bind_valid(Mat4 value) {
  if (!animation_matrix_affine(value)) {
    return false_v;
  }
  /* Double precision avoids rejecting small valid scales through a float
   * determinant underflow or an arbitrary singularity epsilon. */
  const float64_t determinant =
      (float64_t)value.m00 * ((float64_t)value.m11 * value.m22 -
                              (float64_t)value.m12 * value.m21) -
      (float64_t)value.m01 * ((float64_t)value.m10 * value.m22 -
                              (float64_t)value.m12 * value.m20) +
      (float64_t)value.m02 *
          ((float64_t)value.m10 * value.m21 - (float64_t)value.m11 * value.m20);
  return determinant != 0.0;
}

static bool8_t animation_validate_contents(const VkrAnimationAsset *asset,
                                           uint32_t *marks,
                                           const char **error) {
  uint32_t *roots = marks + asset->node_count;
  MemZero(marks, sizeof(*marks) * asset->node_count);
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    const uint32_t index = asset->node_order[i];
    if (index >= asset->node_count || marks[index]) {
      return animation_fail(error, "Node order is not a permutation");
    }
    const VkrAnimationNode *node = &asset->nodes[index];
    if (node->parent != VKR_ANIMATION_NO_NODE &&
        (node->parent >= asset->node_count || !marks[node->parent])) {
      return animation_fail(error, "Node parent must precede its child");
    }
    if (!animation_name_valid(node->name) || node->matrix_authored > true_v ||
        !animation_matrix_affine(node->local) ||
        !animation_vec3_finite(node->rest.translation) ||
        !animation_rotation_valid(node->rest.rotation) ||
        !animation_vec3_finite(node->rest.scale)) {
      return animation_fail(error, "Invalid node name or rest transform");
    }
    marks[index] = 1u;
    roots[index] =
        node->parent == VKR_ANIMATION_NO_NODE ? index : roots[node->parent];
  }

  for (uint32_t i = 0; i < asset->skin_count; ++i) {
    const VkrAnimationSkin *skin = &asset->skins[i];
    if (!animation_name_valid(skin->name) || !skin->joint_count ||
        skin->joint_count > asset->node_count || !skin->joints ||
        !skin->inverse_bind ||
        (skin->skeleton_node != VKR_ANIMATION_NO_NODE &&
         skin->skeleton_node >= asset->node_count)) {
      return animation_fail(error, "Invalid skin metadata");
    }
    MemZero(marks, sizeof(*marks) * asset->node_count);
    uint32_t root = VKR_ANIMATION_NO_NODE;
    for (uint32_t j = 0; j < skin->joint_count; ++j) {
      const uint32_t node = skin->joints[j];
      if (node >= asset->node_count || marks[node] ||
          !animation_inverse_bind_valid(skin->inverse_bind[j])) {
        return animation_fail(error,
                              "Invalid skin joint or inverse bind matrix");
      }
      if (j && roots[node] != root) {
        return animation_fail(error, "Skin joints must share a tree root");
      }
      root = roots[node];
      marks[node] = 1u;
    }
    if (skin->skeleton_node != VKR_ANIMATION_NO_NODE) {
      /* Mark descendants in topological order, including the skeleton root. */
      for (uint32_t j = 0; j < asset->node_count; ++j) {
        const uint32_t node = asset->node_order[j];
        const uint32_t parent = asset->nodes[node].parent;
        if (node == skin->skeleton_node ||
            (parent != VKR_ANIMATION_NO_NODE && (marks[parent] & 2u))) {
          marks[node] |= 2u;
        }
        if ((marks[node] & 1u) && !(marks[node] & 2u)) {
          return animation_fail(error, "Skin skeleton is not a joint ancestor");
        }
      }
    }
  }

  uint64_t total_keys = 0;
  for (uint32_t i = 0; i < asset->clip_count; ++i) {
    const VkrAnimationClip *clip = &asset->clips[i];
    if (!animation_name_valid(clip->name) || !isfinite(clip->duration) ||
        clip->duration < 0.0f || !clip->channel_count ||
        clip->channel_count > (uint64_t)asset->node_count * 3u ||
        !clip->channels) {
      return animation_fail(error, "Invalid clip metadata");
    }
    MemZero(marks, sizeof(*marks) * asset->node_count);
    float32_t duration = 0.0f;
    for (uint32_t j = 0; j < clip->channel_count; ++j) {
      const VkrAnimationChannel *channel = &clip->channels[j];
      if (channel->node >= asset->node_count ||
          (uint32_t)channel->path > VKR_ANIMATION_SCALE ||
          (uint32_t)channel->interpolation > VKR_ANIMATION_CUBIC_SPLINE ||
          !channel->key_count || channel->key_count > VKR_ANIMATION_MAX_KEYS ||
          (channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE &&
           channel->key_count < 2u) ||
          !channel->times || !channel->values) {
        return animation_fail(error, "Invalid animation channel metadata");
      }
      const uint32_t path_bit = 1u << channel->path;
      if (asset->nodes[channel->node].matrix_authored ||
          (marks[channel->node] & path_bit)) {
        return animation_fail(error,
                              "Duplicate channel or matrix-authored target");
      }
      marks[channel->node] |= path_bit;
      total_keys += channel->key_count;
      if (total_keys > VKR_ANIMATION_MAX_KEYS) {
        return animation_fail(error, "Animation bank exceeds key limit");
      }
      const uint32_t stride =
          channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE ? 3u : 1u;
      for (uint32_t k = 0; k < channel->key_count; ++k) {
        const float32_t time = channel->times[k];
        if (!isfinite(time) || time < 0.0f ||
            (k && time <= channel->times[k - 1u])) {
          return animation_fail(error,
                                "Animation times must strictly increase");
        }
        for (uint32_t v = 0; v < stride; ++v) {
          const Vec4 value = channel->values[k * stride + v];
          if (!animation_vec4_finite(value) ||
              (channel->path != VKR_ANIMATION_ROTATION && value.w != 0.0f)) {
            return animation_fail(error, "Invalid animation value or tangent");
          }
        }
        const uint32_t value_index = k * stride + (stride == 3u ? 1u : 0u);
        if (channel->path == VKR_ANIMATION_ROTATION &&
            !animation_rotation_valid(channel->values[value_index])) {
          return animation_fail(error,
                                "Rotation key must be a unit quaternion");
        }
      }
      duration = Max(duration, channel->times[channel->key_count - 1u]);
    }
    if (duration != clip->duration) {
      return animation_fail(error,
                            "Clip duration does not match channel endpoints");
    }
  }
  return true_v;
}

bool8_t vkr_animation_validate(const VkrAnimationAsset *asset,
                               VkrAllocator *scratch, const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!asset || !scratch || !asset->node_count ||
      asset->node_count > VKR_ANIMATION_MAX_NODES ||
      asset->skin_count > VKR_ANIMATION_MAX_NODES ||
      asset->clip_count > VKR_ANIMATION_MAX_CLIPS || !asset->nodes ||
      !asset->node_order || (asset->skin_count && !asset->skins) ||
      (asset->clip_count && !asset->clips)) {
    return animation_fail(error, "Invalid animation bank metadata or scratch");
  }
  VkrAllocatorScope scope = {0};
  const bool8_t scoped = vkr_allocator_supports_scopes(scratch);
  if (scoped) {
    scope = vkr_allocator_begin_scope(scratch);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      return animation_fail(error, "Cannot begin animation validation scratch");
    }
  } else if (!scratch->free) {
    return animation_fail(error,
                          "Animation scratch must support scopes or free");
  }
  const uint64_t bytes = sizeof(uint32_t) * (uint64_t)asset->node_count * 2u;
  uint32_t *marks =
      vkr_allocator_alloc(scratch, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  const bool8_t valid =
      marks ? animation_validate_contents(asset, marks, error)
            : animation_fail(error, "Cannot allocate validation scratch");
  if (scoped) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else if (marks) {
    vkr_allocator_free(scratch, marks, bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  return valid;
}

static bool8_t animation_sample_channel(const VkrAnimationChannel *channel,
                                        float64_t seconds, Vec4 *sample) {
  const uint32_t stride =
      channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE ? 3u : 1u;
  const uint32_t value_offset = stride == 3u ? 1u : 0u;
  uint32_t left = 0;
  uint32_t right = channel->key_count - 1u;
  if (seconds <= channel->times[left]) {
    *sample = channel->values[value_offset];
    return true_v;
  }
  if (seconds >= channel->times[right]) {
    *sample = channel->values[right * stride + value_offset];
    return true_v;
  }
  while (right - left > 1u) {
    const uint32_t middle = left + (right - left) / 2u;
    if (seconds < channel->times[middle]) {
      right = middle;
    } else {
      left = middle;
    }
  }
  const Vec4 a = channel->values[left * stride + value_offset];
  const Vec4 b = channel->values[right * stride + value_offset];
  if (channel->interpolation == VKR_ANIMATION_STEP) {
    *sample = a;
    return true_v;
  }
  const float64_t duration =
      (float64_t)channel->times[right] - channel->times[left];
  const float64_t t = (seconds - channel->times[left]) / duration;
  if (channel->interpolation == VKR_ANIMATION_LINEAR &&
      channel->path == VKR_ANIMATION_ROTATION) {
    *sample = vkr_quat_slerp(a, b, (float32_t)t);
    return animation_vec4_finite(*sample);
  }
  /* Double intermediates retain finite results when float subtraction or
   * tangent scaling would overflow before cancellation. */
  float64_t a_weight = 1.0 - t;
  float64_t b_weight = t;
  float64_t out_weight = 0.0;
  float64_t in_weight = 0.0;
  Vec4 out_tangent = {0};
  Vec4 in_tangent = {0};
  if (channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE) {
    const float64_t squared = t * t;
    const float64_t cubed = squared * t;
    a_weight = 2.0 * cubed - 3.0 * squared + 1.0;
    b_weight = -2.0 * cubed + 3.0 * squared;
    out_weight = (cubed - 2.0 * squared + t) * duration;
    in_weight = (cubed - squared) * duration;
    out_tangent = channel->values[left * stride + 2u];
    in_tangent = channel->values[right * stride];
  }
  float64_t components[4];
  float64_t magnitude = 0.0;
  for (uint32_t i = 0; i < ArrayCount(components); ++i) {
    components[i] = a_weight * a.elements[i] + b_weight * b.elements[i] +
                    out_weight * out_tangent.elements[i] +
                    in_weight * in_tangent.elements[i];
    if (!isfinite(components[i])) {
      return false_v;
    }
    const float64_t absolute = fabs(components[i]);
    magnitude = Max(magnitude, absolute);
  }
  if (channel->path == VKR_ANIMATION_ROTATION) {
    /* Cubic values and tangents retain authored signs. Scale in double before
     * converting to float so a finite large tangent can still normalize. */
    if (magnitude == 0.0) {
      return false_v;
    }
    for (uint32_t i = 0; i < ArrayCount(components); ++i) {
      sample->elements[i] = (float32_t)(components[i] / magnitude);
    }
    *sample = vkr_quat_normalize(*sample);
  } else {
    if (magnitude > FLT_MAX) {
      return false_v;
    }
    for (uint32_t i = 0; i < ArrayCount(components); ++i) {
      sample->elements[i] = (float32_t)components[i];
    }
  }
  return true_v;
}

bool8_t vkr_animation_sample(const VkrAnimationAsset *asset, uint32_t clip,
                             float64_t seconds, VkrAnimationTrs *local_pose) {
  if (!asset || clip >= asset->clip_count || !local_pose ||
      !isfinite(seconds)) {
    return false_v;
  }
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    local_pose[i] = asset->nodes[i].rest;
  }
  const VkrAnimationClip *animation = &asset->clips[clip];
  for (uint32_t i = 0; i < animation->channel_count; ++i) {
    const VkrAnimationChannel *channel = &animation->channels[i];
    Vec4 sample;
    if (!animation_sample_channel(channel, seconds, &sample)) {
      return false_v;
    }
    VkrAnimationTrs *pose = &local_pose[channel->node];
    switch (channel->path) {
    case VKR_ANIMATION_TRANSLATION:
      pose->translation = vec4_to_vec3(sample);
      break;
    case VKR_ANIMATION_ROTATION:
      pose->rotation = sample;
      break;
    case VKR_ANIMATION_SCALE:
      pose->scale = vec4_to_vec3(sample);
      break;
    }
  }
  return true_v;
}

bool8_t vkr_animation_global_pose(const VkrAnimationAsset *asset,
                                  const VkrAnimationTrs *local_pose,
                                  Mat4 *global_pose) {
  if (!asset || !local_pose || !global_pose) {
    return false_v;
  }
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    const uint32_t index = asset->node_order[i];
    const VkrAnimationNode *node = &asset->nodes[index];
    const VkrAnimationTrs *pose = &local_pose[index];
    const Mat4 local = node->matrix_authored
                           ? node->local
                           : mat4_mul(mat4_from_vkr_quat_pos(pose->rotation,
                                                             pose->translation),
                                      mat4_scale(pose->scale));
    const Mat4 global = node->parent == VKR_ANIMATION_NO_NODE
                            ? local
                            : mat4_mul(global_pose[node->parent], local);
    if (!animation_matrix_finite(global)) {
      return false_v;
    }
    global_pose[index] = global;
  }
  return true_v;
}

bool8_t vkr_animation_skin_palette(const VkrAnimationAsset *asset,
                                   uint32_t skin, const Mat4 *global_pose,
                                   Mat4 *palette) {
  if (!asset || skin >= asset->skin_count || !global_pose || !palette) {
    return false_v;
  }
  const VkrAnimationSkin *binding = &asset->skins[skin];
  for (uint32_t i = 0; i < binding->joint_count; ++i) {
    const Mat4 matrix =
        mat4_mul(global_pose[binding->joints[i]], binding->inverse_bind[i]);
    if (!animation_matrix_finite(matrix)) {
      return false_v;
    }
    palette[i] = matrix;
  }
  return true_v;
}
