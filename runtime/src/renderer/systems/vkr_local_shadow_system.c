#include "renderer/systems/vkr_local_shadow_system.h"

#include "renderer/systems/vkr_shadow_system.h"

#include <float.h>
#include <math.h>

typedef struct VkrLocalShadowCandidate {
  uint32_t light_index;
  uint32_t render_id;
  uint32_t light_kind;
  uint32_t face_count;
  float32_t half_fov;
  float32_t score;
} VkrLocalShadowCandidate;

static const Vec3 s_face_direction[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

vkr_internal bool8_t vkr_local_shadow_light_valid(const VkrPointLight *light,
                                                  uint32_t *out_face_count,
                                                  float32_t *out_half_fov) {
  const bool8_t spot = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  const uint32_t face_count = spot ? 1u : 6u;
  if (!light->casts_shadow || !isfinite(light->range) || light->range <= 0.0f ||
      !isfinite(light->position.x) || !isfinite(light->position.y) ||
      !isfinite(light->position.z)) {
    return false_v;
  }

  const float32_t half_fov = spot ? light->outer_cone_angle : 0.78539816339f;
  if (!isfinite(half_fov) || half_fov <= 0.0f || half_fov >= 1.57079632679f) {
    return false_v;
  }
  if (spot) {
    const float32_t direction_length = vec3_length(light->direction);
    if (!isfinite(direction_length) || direction_length < 0.000001f)
      return false_v;
  }

  *out_face_count = face_count;
  *out_half_fov = half_fov;
  return true_v;
}

vkr_internal void vkr_local_shadow_append(const VkrPointLight *light,
                                          uint32_t light_index,
                                          uint32_t face_count,
                                          float32_t half_fov, uint32_t map_size,
                                          VkrLocalShadowPassPayload *out) {
  const float32_t near_clip = Min(0.05f, light->range * 0.01f);
  const Mat4 projection =
      mat4_perspective(2.0f * half_fov, 1.0f, near_clip, light->range);
  out->light_first_view[light_index] = out->view_count + 1u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    const Vec3 direction = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                               ? vec3_normalize(light->direction)
                               : s_face_direction[face];
    const Vec3 up =
        fabsf(direction.y) > 0.99f ? (Vec3){0, 0, 1} : (Vec3){0, 1, 0};
    const Mat4 view =
        mat4_look_at(light->position, vec3_add(light->position, direction), up);
    out->views[out->view_count++] = (VkrLocalShadowView){
        .light_view_projection = mat4_mul(projection, view),
        .light_position_near = {light->position.x, light->position.y,
                                light->position.z, near_clip},
        .light_direction_far = {direction.x, direction.y, direction.z,
                                light->range},
        .projection_params = {tanf(half_fov), 1.0f / (float32_t)map_size, 1.0f,
                              2.0f},
    };
  }
}

/* Preparation owns light selection and all projection validation. The payload
 * stays in application frame storage; native uploads borrow it until render
 * returns, then GPU slot completion owns the uploaded bytes. */
void vkr_local_shadow_prepare(const VkrPointLight *lights, uint32_t light_count,
                              uint32_t face_budget, uint32_t map_size,
                              VkrLocalShadowPassPayload *out) {
  MemZero(out, sizeof(*out));
  out->face_budget = Min(face_budget, VKR_LOCAL_SHADOW_FACE_COUNT_MAX);
  out->map_size = map_size;
  if (!lights || !out->face_budget || !map_size || map_size > 1024u)
    return;

  for (uint32_t i = 0u; i < Min(light_count, VKR_MAX_SCENE_POINT_LIGHTS); ++i) {
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(&lights[i], &face_count, &half_fov) ||
        out->view_count + face_count > out->face_budget) {
      continue;
    }
    vkr_local_shadow_append(&lights[i], i, face_count, half_fov, map_size, out);
  }
}

vkr_internal bool8_t
vkr_local_shadow_selection_contains(const VkrLocalShadowSelection *selection,
                                    const VkrLocalShadowCandidate *candidate) {
  if (!selection || !selection->valid || candidate->render_id == 0u)
    return false_v;
  for (uint32_t i = 0u; i < selection->group_count; ++i) {
    const VkrLocalShadowSelectionGroup *group = &selection->groups[i];
    if (group->render_id == candidate->render_id &&
        group->light_kind == candidate->light_kind &&
        group->face_count == candidate->face_count) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_local_shadow_sort_candidates_by_render_id(
    VkrLocalShadowCandidate *candidates, uint32_t candidate_count) {
  for (uint32_t i = 1u; i < candidate_count; ++i) {
    const VkrLocalShadowCandidate value = candidates[i];
    uint32_t insert = i;
    while (insert > 0u) {
      const VkrLocalShadowCandidate *left = &candidates[insert - 1u];
      if (left->render_id < value.render_id ||
          (left->render_id == value.render_id &&
           (left->light_kind < value.light_kind ||
            (left->light_kind == value.light_kind &&
             left->light_index <= value.light_index)))) {
        break;
      }
      candidates[insert] = candidates[insert - 1u];
      --insert;
    }
    candidates[insert] = value;
  }
}

vkr_internal bool8_t vkr_local_shadow_selection_matches(
    const VkrLocalShadowSelection *selection,
    const VkrLocalShadowCandidate *candidates, uint32_t candidate_count,
    const uint32_t *selected, uint32_t selected_count, uint32_t face_budget) {
  if (!selection || !selection->valid ||
      selection->face_budget != face_budget ||
      selection->group_count != selected_count)
    return false_v;

  for (uint32_t group_index = 0u; group_index < selection->group_count;
       ++group_index) {
    const VkrLocalShadowSelectionGroup *group = &selection->groups[group_index];
    bool8_t found = false_v;
    for (uint32_t selected_index = 0u; selected_index < selected_count;
         ++selected_index) {
      const uint32_t candidate_index = selected[selected_index];
      if (candidate_index >= candidate_count)
        return false_v;
      const VkrLocalShadowCandidate *candidate = &candidates[candidate_index];
      if (group->render_id == candidate->render_id &&
          group->light_kind == candidate->light_kind &&
          group->face_count == candidate->face_count) {
        found = true_v;
        break;
      }
    }
    if (!found)
      return false_v;
  }
  return true_v;
}

vkr_internal uint64_t
vkr_local_shadow_next_layout_generation(uint64_t generation) {
  return generation == UINT64_MAX ? 1u : generation + 1u;
}

vkr_internal void vkr_local_shadow_sort_selected_by_render_id(
    const VkrLocalShadowCandidate *candidates, uint32_t *selected,
    uint32_t selected_count) {
  for (uint32_t i = 1u; i < selected_count; ++i) {
    const uint32_t value = selected[i];
    uint32_t insert = i;
    while (insert > 0u) {
      const VkrLocalShadowCandidate *left = &candidates[selected[insert - 1u]];
      const VkrLocalShadowCandidate *right = &candidates[value];
      if (left->render_id < right->render_id ||
          (left->render_id == right->render_id &&
           (left->light_kind < right->light_kind ||
            (left->light_kind == right->light_kind &&
             left->light_index <= right->light_index)))) {
        break;
      }
      selected[insert] = selected[insert - 1u];
      --insert;
    }
    selected[insert] = value;
  }
}

vkr_internal void
vkr_local_shadow_order_like_selection(const VkrLocalShadowSelection *selection,
                                      const VkrLocalShadowCandidate *candidates,
                                      uint32_t *selected,
                                      uint32_t selected_count) {
  uint32_t ordered[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  for (uint32_t group_index = 0u; group_index < selected_count; ++group_index) {
    const VkrLocalShadowSelectionGroup *group = &selection->groups[group_index];
    for (uint32_t selected_index = 0u; selected_index < selected_count;
         ++selected_index) {
      const VkrLocalShadowCandidate *candidate =
          &candidates[selected[selected_index]];
      if (group->render_id == candidate->render_id &&
          group->light_kind == candidate->light_kind &&
          group->face_count == candidate->face_count) {
        ordered[group_index] = selected[selected_index];
        break;
      }
    }
  }
  MemCopy(selected, ordered, sizeof(uint32_t) * selected_count);
}

void vkr_local_shadow_prepare_selection(VkrLocalShadowSelection *selection,
                                        const VkrPointLight *lights,
                                        uint32_t light_count,
                                        Vec3 camera_position,
                                        uint32_t face_budget, uint32_t map_size,
                                        VkrLocalShadowPassPayload *out) {
  MemZero(out, sizeof(*out));
  out->face_budget = Min(face_budget, VKR_LOCAL_SHADOW_FACE_COUNT_MAX);
  out->map_size = map_size;
  if (!selection || !lights || !out->face_budget || !map_size ||
      map_size > 1024u || !isfinite(camera_position.x) ||
      !isfinite(camera_position.y) || !isfinite(camera_position.z)) {
    if (selection) {
      const uint64_t layout_generation =
          vkr_local_shadow_next_layout_generation(selection->layout_generation);
      *selection = (VkrLocalShadowSelection){
          .layout_generation = layout_generation,
      };
    }
    return;
  }

  VkrLocalShadowCandidate candidates[VKR_MAX_SCENE_POINT_LIGHTS] = {0};
  uint32_t candidate_count = 0u;
  for (uint32_t i = 0u; i < Min(light_count, VKR_MAX_SCENE_POINT_LIGHTS); ++i) {
    const VkrPointLight *light = &lights[i];
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(light, &face_count, &half_fov) ||
        face_count > out->face_budget)
      continue;

    const Vec3 delta = vec3_sub(light->position, camera_position);
    const float32_t distance_squared = vec3_length_squared(delta);
    const float32_t range_squared = light->range * light->range;
    const float32_t luminance = light->color.x * 0.2126f +
                                light->color.y * 0.7152f +
                                light->color.z * 0.0722f;
    float32_t score = Max(luminance, 0.0f) * Max(light->intensity, 0.0f) *
                      range_squared / Max(distance_squared, range_squared);
    if (!isfinite(distance_squared) || !isfinite(range_squared) ||
        !isfinite(score) || score <= 0.0f) {
      continue;
    }

    VkrLocalShadowCandidate *candidate = &candidates[candidate_count++];
    *candidate = (VkrLocalShadowCandidate){
        .light_index = i,
        .render_id = light->render_id,
        .light_kind = (uint32_t)light->kind,
        .face_count = face_count,
        .half_fov = half_fov,
        .score = score,
    };
    if (vkr_local_shadow_selection_contains(selection, candidate))
      candidate->score *= 1.15f;
  }
  vkr_local_shadow_sort_candidates_by_render_id(candidates, candidate_count);

  float32_t score[VKR_MAX_SCENE_POINT_LIGHTS + 1u]
                 [VKR_LOCAL_SHADOW_FACE_COUNT_MAX + 1u];
  uint8_t take[VKR_MAX_SCENE_POINT_LIGHTS + 1u]
              [VKR_LOCAL_SHADOW_FACE_COUNT_MAX + 1u] = {{0}};
  for (uint32_t budget = 0u; budget <= out->face_budget; ++budget)
    score[0][budget] = budget == 0u ? 0.0f : -FLT_MAX;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    for (uint32_t budget = 0u; budget <= out->face_budget; ++budget) {
      score[i + 1u][budget] = score[i][budget];
      take[i + 1u][budget] = 0u;
      const uint32_t cost = candidates[i].face_count;
      if (budget < cost || score[i][budget - cost] == -FLT_MAX)
        continue;
      const float32_t selected_score =
          score[i][budget - cost] + candidates[i].score;
      if (selected_score > score[i + 1u][budget]) {
        score[i + 1u][budget] = selected_score;
        take[i + 1u][budget] = 1u;
      }
    }
  }

  uint32_t selected[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t selected_count = 0u;
  uint32_t selected_budget = 0u;
  for (uint32_t budget = 1u; budget <= out->face_budget; ++budget) {
    if (score[candidate_count][budget] >
        score[candidate_count][selected_budget])
      selected_budget = budget;
  }
  for (uint32_t i = candidate_count; i > 0u; --i) {
    if (!take[i][selected_budget])
      continue;
    const uint32_t candidate_index = i - 1u;
    selected[selected_count++] = candidate_index;
    selected_budget -= candidates[candidate_index].face_count;
  }

  const bool8_t same_layout = vkr_local_shadow_selection_matches(
      selection, candidates, candidate_count, selected, selected_count,
      out->face_budget);
  if (same_layout)
    vkr_local_shadow_order_like_selection(selection, candidates, selected,
                                          selected_count);
  else
    vkr_local_shadow_sort_selected_by_render_id(candidates, selected,
                                                selected_count);

  VkrLocalShadowSelection next = {
      .face_budget = out->face_budget,
      .layout_generation = same_layout
                               ? selection->layout_generation
                               : vkr_local_shadow_next_layout_generation(
                                     selection->layout_generation),
  };
  for (uint32_t i = 0u; i < selected_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[selected[i]];
    const VkrPointLight *light = &lights[candidate->light_index];
    vkr_local_shadow_append(light, candidate->light_index,
                            candidate->face_count, candidate->half_fov,
                            map_size, out);
    next.groups[next.group_count++] = (VkrLocalShadowSelectionGroup){
        .render_id = candidate->render_id,
        .light_kind = candidate->light_kind,
        .face_count = candidate->face_count,
    };
    next.face_count += candidate->face_count;
  }
  next.valid = next.group_count == selected_count;
  if (next.valid) {
    for (uint32_t i = 0u; i < next.group_count; ++i) {
      if (next.groups[i].render_id == 0u) {
        next.valid = false_v;
        break;
      }
      for (uint32_t j = 0u; j < i; ++j) {
        if (next.groups[j].render_id == next.groups[i].render_id) {
          next.valid = false_v;
          break;
        }
      }
      if (!next.valid)
        break;
    }
  }
  *selection = next;
}
