#include "renderer/systems/vkr_local_shadow_system.h"

#include "renderer/systems/vkr_shadow_system.h"
#include "vkr_temporal.h"

#include <float.h>
#include <math.h>

/* A selected light keeps its layers until a competitor outscores it by this
 * factor, which limits selection churn and cached-face redraws. */
#define VKR_LOCAL_SHADOW_INCUMBENT_BONUS 1.15f
/* Seconds for a shadow to fade fully in or out when the selection changes. */
#define VKR_LOCAL_SHADOW_FADE_SECONDS 0.25f
/* Camera distances below this fraction of a light's range score alike. */
#define VKR_LOCAL_SHADOW_NEAR_RANGE_FRACTION 0.1f
/* Lights past this many, by importance, take a single filtered tap and no
 * contact shadows, so a larger budget shadows more lights at a small cost. */
#define VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT 3u
/* Atlas side in cells of the smallest face size. */
#define VKR_LOCAL_SHADOW_ATLAS_CELLS                                           \
  (VKR_LOCAL_SHADOW_ATLAS_SIZE / VKR_LOCAL_SHADOW_FACE_SIZE_MIN)
_Static_assert(VKR_LOCAL_SHADOW_ATLAS_CELLS == 32u,
               "atlas occupancy rows are 32-bit masks");

/* Occupancy of the atlas in smallest-face cells, one bit per column. */
typedef struct VkrLocalShadowAtlas {
  uint32_t rows[VKR_LOCAL_SHADOW_ATLAS_CELLS];
} VkrLocalShadowAtlas;

typedef struct VkrLocalShadowCandidate {
  uint32_t light_index;
  uint32_t render_id;
  uint32_t light_kind;
  uint32_t face_count;
  /** Previous first layer + 1 while the light was selected; zero otherwise. */
  uint32_t incumbent_first_view;
  float32_t half_fov;
  /** Viewer importance without the incumbent bonus. */
  float32_t score;
  /** Previous shadow strength while the light was selected. */
  float32_t strength;
  /** Previous face size and atlas cells while the light was selected. */
  uint32_t incumbent_face_size;
  uint32_t incumbent_face_cells[6];
  /** Whether the light took the reduced filter while selected. */
  bool8_t incumbent_reduced;
  /** Whether the bonus-weighted selection wants this light shadowed. */
  bool8_t desired;
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

vkr_internal bool8_t vkr_local_shadow_map_size_valid(uint32_t map_size) {
  return map_size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
         map_size <= VKR_LOCAL_SHADOW_MAP_SIZE_MAX &&
         (map_size & (map_size - 1u)) == 0u;
}

vkr_internal uint32_t vkr_local_shadow_row_bits(uint32_t cell_x,
                                                uint32_t cells) {
  return ((UINT32_C(1) << cells) - 1u) << cell_x;
}

vkr_internal bool8_t
vkr_local_shadow_atlas_free(const VkrLocalShadowAtlas *atlas, uint32_t cell_x,
                            uint32_t cell_y, uint32_t cells) {
  if (cell_x % cells != 0u || cell_y % cells != 0u ||
      cell_x + cells > VKR_LOCAL_SHADOW_ATLAS_CELLS ||
      cell_y + cells > VKR_LOCAL_SHADOW_ATLAS_CELLS)
    return false_v;
  const uint32_t bits = vkr_local_shadow_row_bits(cell_x, cells);
  for (uint32_t row = cell_y; row < cell_y + cells; ++row) {
    if ((atlas->rows[row] & bits) != 0u)
      return false_v;
  }
  return true_v;
}

vkr_internal void vkr_local_shadow_atlas_mark(VkrLocalShadowAtlas *atlas,
                                              uint32_t cell_x, uint32_t cell_y,
                                              uint32_t cells) {
  const uint32_t bits = vkr_local_shadow_row_bits(cell_x, cells);
  for (uint32_t row = cell_y; row < cell_y + cells; ++row)
    atlas->rows[row] |= bits;
}

/* First free aligned square in row-major order. When squares are placed in
 * descending size, every earlier square is aligned to the current size, so a
 * free square exists whenever enough area remains. */
vkr_internal bool8_t vkr_local_shadow_atlas_allocate(VkrLocalShadowAtlas *atlas,
                                                     uint32_t face_size,
                                                     uint32_t *out_cell) {
  const uint32_t cells = face_size / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
  for (uint32_t cell_y = 0u; cell_y < VKR_LOCAL_SHADOW_ATLAS_CELLS;
       cell_y += cells) {
    for (uint32_t cell_x = 0u; cell_x < VKR_LOCAL_SHADOW_ATLAS_CELLS;
         cell_x += cells) {
      if (vkr_local_shadow_atlas_free(atlas, cell_x, cell_y, cells)) {
        vkr_local_shadow_atlas_mark(atlas, cell_x, cell_y, cells);
        *out_cell = cell_x | (cell_y << 8u);
        return true_v;
      }
    }
  }
  return false_v;
}

/* Shrinks the lowest-scoring groups one size at a time until every face fits
 * the atlas. Power-of-two squares whose area fits always pack. */
vkr_internal void vkr_local_shadow_fit_atlas(const float32_t *scores,
                                             const uint32_t *face_counts,
                                             uint32_t group_count,
                                             uint32_t *face_sizes) {
  for (;;) {
    uint64_t cells = 0u;
    for (uint32_t i = 0u; i < group_count; ++i) {
      const uint64_t side = face_sizes[i] / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
      cells += (uint64_t)face_counts[i] * side * side;
    }
    if (cells <=
        (uint64_t)VKR_LOCAL_SHADOW_ATLAS_CELLS * VKR_LOCAL_SHADOW_ATLAS_CELLS)
      return;
    uint32_t weakest = UINT32_MAX;
    for (uint32_t i = 0u; i < group_count; ++i) {
      if (face_sizes[i] > VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
          (weakest == UINT32_MAX || scores[i] <= scores[weakest]))
        weakest = i;
    }
    if (weakest == UINT32_MAX)
      return;
    face_sizes[weakest] /= 2u;
  }
}

/* Places every face of every group. A group whose size is unchanged keeps its
 * previous squares when they are still free; the others take the first free
 * squares in descending size. If fragmentation leaves no room, all groups are
 * packed afresh. `previous_cells` is NULL when nothing was placed before. */
vkr_internal void vkr_local_shadow_pack_atlas(
    const uint32_t *face_counts, const uint32_t *face_sizes,
    const uint32_t *previous_sizes, const uint32_t (*previous_cells)[6],
    uint32_t group_count, uint32_t (*out_cells)[6]) {
  VkrLocalShadowAtlas atlas = {0};
  bool8_t placed[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  for (uint32_t i = 0u; previous_cells && i < group_count; ++i) {
    if (previous_sizes[i] != face_sizes[i])
      continue;
    const uint32_t cells = face_sizes[i] / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
    VkrLocalShadowAtlas trial = atlas;
    bool8_t fits = true_v;
    for (uint32_t face = 0u; fits && face < face_counts[i]; ++face) {
      const uint32_t cell_x = previous_cells[i][face] & 0xFFu;
      const uint32_t cell_y = previous_cells[i][face] >> 8u;
      fits = vkr_local_shadow_atlas_free(&trial, cell_x, cell_y, cells);
      if (fits)
        vkr_local_shadow_atlas_mark(&trial, cell_x, cell_y, cells);
    }
    if (!fits)
      continue;
    atlas = trial;
    placed[i] = true_v;
    MemCopy(out_cells[i], previous_cells[i], sizeof(out_cells[i]));
  }

  for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
    bool8_t complete = true_v;
    for (uint32_t size = VKR_LOCAL_SHADOW_MAP_SIZE_MAX;
         complete && size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN; size /= 2u) {
      for (uint32_t i = 0u; complete && i < group_count; ++i) {
        if (placed[i] || face_sizes[i] != size)
          continue;
        for (uint32_t face = 0u; complete && face < face_counts[i]; ++face)
          complete = vkr_local_shadow_atlas_allocate(&atlas, size,
                                                     &out_cells[i][face]);
      }
    }
    if (complete)
      return;
    atlas = (VkrLocalShadowAtlas){0};
    MemZero(placed, sizeof(placed));
  }
}

/* The smallest power-of-two face whose side reaches the light's range sphere
 * radius on screen, clamped to the configured largest face. Inside the range
 * the sphere fills the view and the face takes the largest size. */
vkr_internal uint32_t vkr_local_shadow_ideal_face_size(
    const VkrPointLight *light, const VkrLocalShadowCamera *camera,
    uint32_t map_size) {
  if (!(camera->focal_pixels > 0.0f) || !isfinite(camera->focal_pixels))
    return map_size;
  const float32_t distance =
      vec3_length(vec3_sub(light->position, camera->position));
  const float32_t radius_pixels =
      camera->focal_pixels * light->range / Max(distance, light->range);
  uint32_t size = VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
  while (size < map_size && (float32_t)size < radius_pixels)
    size *= 2u;
  return size;
}

/* A face grows as soon as the light needs more texels but shrinks only once
 * it has twice the texels it needs, so sizes do not flicker at a boundary. */
vkr_internal uint32_t vkr_local_shadow_hysteresis_face_size(
    uint32_t ideal, uint32_t incumbent_size, uint32_t map_size) {
  if (incumbent_size == 0u || ideal >= incumbent_size)
    return ideal;
  if (ideal * 4u <= incumbent_size)
    return ideal * 2u;
  return Min(incumbent_size, map_size);
}

vkr_internal void vkr_local_shadow_write(
    const VkrPointLight *light, uint32_t light_index, uint32_t first_view,
    uint32_t face_count, float32_t half_fov, uint32_t face_size,
    const uint32_t *face_cells, float32_t strength, uint32_t mask_layer,
    bool8_t reduced, VkrLocalShadowPassPayload *out) {
  const float32_t near_clip = Min(0.05f, light->range * 0.01f);
  const Mat4 projection =
      mat4_perspective(2.0f * half_fov, 1.0f, near_clip, light->range);
  out->light_first_view[light_index] = first_view + 1u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    const Vec3 direction = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                               ? vec3_normalize(light->direction)
                               : s_face_direction[face];
    const Vec3 up =
        fabsf(direction.y) > 0.99f ? (Vec3){0, 0, 1} : (Vec3){0, 1, 0};
    const Mat4 view =
        mat4_look_at(light->position, vec3_add(light->position, direction), up);
    const float32_t cell_uv = (float32_t)VKR_LOCAL_SHADOW_FACE_SIZE_MIN /
                              (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
    out->views[first_view + face] = (VkrLocalShadowView){
        .light_view_projection = mat4_mul(projection, view),
        .light_position_near = {light->position.x, light->position.y,
                                light->position.z, near_clip},
        .light_direction_far = {direction.x, direction.y, direction.z,
                                light->range},
        .projection_params = {tanf(half_fov), 1.0f / (float32_t)face_size, 1.0f,
                              2.0f},
        .shadow_params = {strength, (float32_t)mask_layer,
                          reduced ? 1.0f : 0.0f, 0.0f},
        .atlas_rect = {(float32_t)(face_cells[face] & 0xFFu) * cell_uv,
                       (float32_t)(face_cells[face] >> 8u) * cell_uv,
                       (float32_t)face_size /
                           (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE,
                       0.0f},
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
  if (!lights || !out->face_budget ||
      !vkr_local_shadow_map_size_valid(map_size))
    return;

  uint32_t light_indices[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT] = {0};
  uint32_t face_counts[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT] = {0};
  float32_t half_fovs[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT] = {0};
  uint32_t shadowed_count = 0u;
  uint32_t view_count = 0u;
  for (uint32_t i = 0u; i < Min(light_count, VKR_MAX_SCENE_POINT_LIGHTS) &&
                        shadowed_count < VKR_LOCAL_SHADOW_MASK_LAYER_COUNT;
       ++i) {
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(&lights[i], &face_count, &half_fov) ||
        view_count + face_count > out->face_budget) {
      continue;
    }
    light_indices[shadowed_count] = i;
    face_counts[shadowed_count] = face_count;
    half_fovs[shadowed_count++] = half_fov;
    view_count += face_count;
  }

  /* Without a camera every light scores alike, so later lights shrink first. */
  float32_t scores[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT] = {0};
  uint32_t face_sizes[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT] = {0};
  uint32_t face_cells[VKR_LOCAL_SHADOW_MASK_LAYER_COUNT][6] = {{0}};
  for (uint32_t i = 0u; i < shadowed_count; ++i) {
    scores[i] = (float32_t)(shadowed_count - i);
    face_sizes[i] = map_size;
  }
  vkr_local_shadow_fit_atlas(scores, face_counts, shadowed_count, face_sizes);
  vkr_local_shadow_pack_atlas(face_counts, face_sizes, NULL, NULL,
                              shadowed_count, face_cells);
  for (uint32_t i = 0u; i < shadowed_count; ++i) {
    vkr_local_shadow_write(&lights[light_indices[i]], light_indices[i],
                           out->view_count, face_counts[i], half_fovs[i],
                           face_sizes[i], face_cells[i], 1.0f, i,
                           i >= VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT, out);
    out->view_count += face_counts[i];
  }
}

vkr_internal const VkrLocalShadowSelectionGroup *
vkr_local_shadow_incumbent(const VkrLocalShadowSelection *selection,
                           const VkrLocalShadowCandidate *candidate) {
  if (!selection || !selection->valid || candidate->render_id == 0u)
    return NULL;
  for (uint32_t i = 0u; i < selection->group_count; ++i) {
    const VkrLocalShadowSelectionGroup *group = &selection->groups[i];
    if (group->render_id == candidate->render_id &&
        group->light_kind == candidate->light_kind &&
        group->face_count == candidate->face_count) {
      return group;
    }
  }
  return NULL;
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

/* Selects complete light groups with the highest total score, incumbents
 * weighted by the bonus, that fit the face budget. Candidates arrive in
 * render-id order, so ties resolve the same way every frame. Returns the
 * selected candidate indices in that order. */
vkr_internal uint32_t vkr_local_shadow_knapsack(
    const VkrLocalShadowCandidate *candidates, uint32_t candidate_count,
    uint32_t face_budget, uint32_t *out_selected) {
  float32_t score[VKR_MAX_SCENE_POINT_LIGHTS + 1u]
                 [VKR_LOCAL_SHADOW_FACE_COUNT_MAX + 1u];
  uint8_t take[VKR_MAX_SCENE_POINT_LIGHTS + 1u]
              [VKR_LOCAL_SHADOW_FACE_COUNT_MAX + 1u] = {{0}};
  for (uint32_t budget = 0u; budget <= face_budget; ++budget)
    score[0][budget] = budget == 0u ? 0.0f : -FLT_MAX;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[i];
    const float32_t candidate_score =
        candidate->incumbent_first_view != 0u
            ? candidate->score * VKR_LOCAL_SHADOW_INCUMBENT_BONUS
            : candidate->score;
    for (uint32_t budget = 0u; budget <= face_budget; ++budget) {
      score[i + 1u][budget] = score[i][budget];
      take[i + 1u][budget] = 0u;
      const uint32_t cost = candidate->face_count;
      if (budget < cost || score[i][budget - cost] == -FLT_MAX)
        continue;
      const float32_t selected_score =
          score[i][budget - cost] + candidate_score;
      if (selected_score > score[i + 1u][budget]) {
        score[i + 1u][budget] = selected_score;
        take[i + 1u][budget] = 1u;
      }
    }
  }

  uint32_t selected_budget = 0u;
  for (uint32_t budget = 1u; budget <= face_budget; ++budget) {
    if (score[candidate_count][budget] >
        score[candidate_count][selected_budget])
      selected_budget = budget;
  }
  uint32_t selected_count = 0u;
  for (uint32_t i = candidate_count; i > 0u; --i) {
    if (!take[i][selected_budget])
      continue;
    out_selected[selected_count++] = i - 1u;
    selected_budget -= candidates[i - 1u].face_count;
  }

  /* Backtracking visits candidates in reverse; restore render-id order. */
  for (uint32_t i = 0u; i < selected_count / 2u; ++i) {
    const uint32_t swap = out_selected[i];
    out_selected[i] = out_selected[selected_count - 1u - i];
    out_selected[selected_count - 1u - i] = swap;
  }
  return selected_count;
}

/* Keeps the highest-scoring desired lights when more than the mask has layers
 * for, preserving render-id order. Only budgets of many one-face spots reach
 * the limit. */
vkr_internal uint32_t
vkr_local_shadow_limit_to_mask(const VkrLocalShadowCandidate *candidates,
                               uint32_t *desired, uint32_t desired_count) {
  while (desired_count > VKR_LOCAL_SHADOW_MASK_LAYER_COUNT) {
    uint32_t weakest = 0u;
    for (uint32_t i = 1u; i < desired_count; ++i) {
      if (candidates[desired[i]].score < candidates[desired[weakest]].score)
        weakest = i;
    }
    for (uint32_t i = weakest + 1u; i < desired_count; ++i)
      desired[i - 1u] = desired[i];
    --desired_count;
  }
  return desired_count;
}

vkr_internal uint64_t vkr_local_shadow_layer_bits(uint32_t first_view,
                                                  uint32_t face_count) {
  return ((UINT64_C(1) << face_count) - 1u) << first_view;
}

/* An incumbent keeps its layers so its cached faces stay valid; a newcomer
 * takes the lowest free range. When a newcomer does not fit or layers stay
 * unowned, groups are compacted in their current layer order, which moves only
 * the groups above the first gap. Returns the total face count. */
vkr_internal uint32_t vkr_local_shadow_place(
    const VkrLocalShadowCandidate *candidates, const uint32_t *selected,
    uint32_t selected_count, uint32_t face_budget, uint32_t *out_first_view) {
  uint64_t used = 0u;
  uint32_t face_count = 0u;
  for (uint32_t i = 0u; i < selected_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[selected[i]];
    out_first_view[i] = UINT32_MAX;
    face_count += candidate->face_count;
    if (candidate->incumbent_first_view == 0u)
      continue;
    const uint32_t first_view = candidate->incumbent_first_view - 1u;
    if (first_view + candidate->face_count > face_budget)
      continue;
    const uint64_t bits =
        vkr_local_shadow_layer_bits(first_view, candidate->face_count);
    if ((used & bits) != 0u)
      continue;
    out_first_view[i] = first_view;
    used |= bits;
  }

  bool8_t placed = true_v;
  for (uint32_t i = 0u; i < selected_count; ++i) {
    if (out_first_view[i] != UINT32_MAX)
      continue;
    const uint32_t count = candidates[selected[i]].face_count;
    for (uint32_t first_view = 0u; first_view + count <= face_budget;
         ++first_view) {
      const uint64_t bits = vkr_local_shadow_layer_bits(first_view, count);
      if ((used & bits) == 0u) {
        out_first_view[i] = first_view;
        used |= bits;
        break;
      }
    }
    placed = placed && out_first_view[i] != UINT32_MAX;
  }
  if (placed && used == vkr_local_shadow_layer_bits(0u, face_count))
    return face_count;

  uint32_t order[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  for (uint32_t i = 0u; i < selected_count; ++i) {
    uint32_t insert = i;
    while (insert > 0u &&
           out_first_view[order[insert - 1u]] > out_first_view[i]) {
      order[insert] = order[insert - 1u];
      --insert;
    }
    order[insert] = i;
  }
  uint32_t first_view = 0u;
  for (uint32_t i = 0u; i < selected_count; ++i) {
    out_first_view[order[i]] = first_view;
    first_view += candidates[selected[order[i]]].face_count;
  }
  return face_count;
}

void vkr_local_shadow_prepare_selection(VkrLocalShadowSelection *selection,
                                        const VkrPointLight *lights,
                                        uint32_t light_count,
                                        const VkrLocalShadowCamera *camera,
                                        uint32_t face_budget, uint32_t map_size,
                                        VkrLocalShadowPassPayload *out) {
  MemZero(out, sizeof(*out));
  out->face_budget = Min(face_budget, VKR_LOCAL_SHADOW_FACE_COUNT_MAX);
  out->map_size = map_size;
  if (!selection || !lights || !camera || !out->face_budget ||
      !vkr_local_shadow_map_size_valid(map_size) ||
      !isfinite(camera->position.x) || !isfinite(camera->position.y) ||
      !isfinite(camera->position.z)) {
    if (selection)
      *selection = (VkrLocalShadowSelection){0};
    return;
  }

  /* Importance follows the light's apparent influence: its range sphere's
   * solid angle outside the range and, inside it, inverse-square proximity, so
   * the nearest of several overlapping lights wins. */
  VkrLocalShadowCandidate candidates[VKR_MAX_SCENE_POINT_LIGHTS] = {0};
  uint32_t candidate_count = 0u;
  for (uint32_t i = 0u; i < Min(light_count, VKR_MAX_SCENE_POINT_LIGHTS); ++i) {
    const VkrPointLight *light = &lights[i];
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(light, &face_count, &half_fov) ||
        face_count > out->face_budget)
      continue;

    const Vec3 delta = vec3_sub(light->position, camera->position);
    const float32_t distance_squared = vec3_length_squared(delta);
    const float32_t range_squared = light->range * light->range;
    const float32_t near_distance =
        light->range * VKR_LOCAL_SHADOW_NEAR_RANGE_FRACTION;
    const float32_t luminance = light->color.x * 0.2126f +
                                light->color.y * 0.7152f +
                                light->color.z * 0.0722f;
    const float32_t score =
        Max(luminance, 0.0f) * Max(light->intensity, 0.0f) * range_squared /
        Max(distance_squared, near_distance * near_distance);
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
    const VkrLocalShadowSelectionGroup *incumbent =
        vkr_local_shadow_incumbent(selection, candidate);
    if (incumbent) {
      candidate->incumbent_first_view = incumbent->first_view + 1u;
      candidate->strength = incumbent->strength;
      candidate->incumbent_face_size = incumbent->face_size;
      candidate->incumbent_reduced = incumbent->reduced;
      MemCopy(candidate->incumbent_face_cells, incumbent->face_cells,
              sizeof(candidate->incumbent_face_cells));
    }
  }
  vkr_local_shadow_sort_candidates_by_render_id(candidates, candidate_count);

  uint32_t desired[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  const uint32_t desired_count = vkr_local_shadow_limit_to_mask(
      candidates, desired,
      vkr_local_shadow_knapsack(candidates, candidate_count, out->face_budget,
                                desired));
  for (uint32_t i = 0u; i < desired_count; ++i)
    candidates[desired[i]].desired = true_v;

  /* A first selection, a budget change or a camera cut has no continuity to
   * keep, so the desired lights take their layers at full strength. Otherwise
   * a light losing its place keeps its layers while its shadow fades out, and
   * a newcomer enters at zero strength once its faces fit the budget. */
  const bool8_t snap =
      !selection->valid || selection->face_budget != out->face_budget ||
      !isfinite(camera->delta_seconds) || camera->delta_seconds < 0.0f ||
      vkr_temporal_is_camera_cut(selection->camera_position,
                                 selection->camera_view, camera->position,
                                 camera->view);
  const float32_t step =
      snap ? 1.0f
           : Min(camera->delta_seconds / VKR_LOCAL_SHADOW_FADE_SECONDS, 1.0f);

  uint32_t selected[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  float32_t strengths[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t selected_count = 0u;
  if (snap) {
    for (uint32_t i = 0u; i < desired_count; ++i) {
      selected[selected_count] = desired[i];
      strengths[selected_count++] = 1.0f;
    }
  } else {
    uint32_t used_faces = 0u;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      const VkrLocalShadowCandidate *candidate = &candidates[i];
      if (candidate->incumbent_first_view == 0u)
        continue;
      const float32_t strength = candidate->desired
                                     ? Min(candidate->strength + step, 1.0f)
                                     : candidate->strength - step;
      if (strength <= 0.0f)
        continue;
      selected[selected_count] = i;
      strengths[selected_count++] = strength;
      used_faces += candidate->face_count;
    }

    /* The most important newcomers take the faces that remain. */
    uint32_t newcomers[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
    uint32_t newcomer_count = 0u;
    for (uint32_t i = 0u; i < desired_count; ++i) {
      const uint32_t index = desired[i];
      if (candidates[index].incumbent_first_view != 0u)
        continue;
      uint32_t insert = newcomer_count++;
      while (insert > 0u && candidates[newcomers[insert - 1u]].score <
                                candidates[index].score) {
        newcomers[insert] = newcomers[insert - 1u];
        --insert;
      }
      newcomers[insert] = index;
    }
    for (uint32_t i = 0u; i < newcomer_count &&
                          selected_count < VKR_LOCAL_SHADOW_MASK_LAYER_COUNT;
         ++i) {
      const VkrLocalShadowCandidate *candidate = &candidates[newcomers[i]];
      if (used_faces + candidate->face_count > out->face_budget)
        continue;
      selected[selected_count] = newcomers[i];
      strengths[selected_count++] = 0.0f;
      used_faces += candidate->face_count;
    }
  }

  uint32_t first_views[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  out->view_count = vkr_local_shadow_place(candidates, selected, selected_count,
                                           out->face_budget, first_views);

  /* Face sizes follow screen coverage with hysteresis, then shrink by
   * importance until the atlas holds them; unchanged sizes keep their squares
   * so cached faces stay valid. */
  float32_t scores[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t face_counts[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t face_sizes[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t previous_sizes[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  uint32_t previous_cells[VKR_LOCAL_SHADOW_FACE_COUNT_MAX][6] = {{0}};
  uint32_t face_cells[VKR_LOCAL_SHADOW_FACE_COUNT_MAX][6] = {{0}};
  for (uint32_t i = 0u; i < selected_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[selected[i]];
    scores[i] = candidate->score;
    face_counts[i] = candidate->face_count;
    face_sizes[i] = vkr_local_shadow_hysteresis_face_size(
        vkr_local_shadow_ideal_face_size(&lights[candidate->light_index],
                                         camera, map_size),
        candidate->incumbent_face_size, map_size);
    previous_sizes[i] = candidate->incumbent_face_size;
    MemCopy(previous_cells[i], candidate->incumbent_face_cells,
            sizeof(previous_cells[i]));
  }
  vkr_local_shadow_fit_atlas(scores, face_counts, selected_count, face_sizes);
  vkr_local_shadow_pack_atlas(face_counts, face_sizes, previous_sizes,
                              previous_cells, selected_count, face_cells);

  /* The most important lights keep the full filter. A light that had it keeps
   * the incumbent bonus, so its filter does not flip while scores cross. */
  bool8_t reduced[VKR_LOCAL_SHADOW_FACE_COUNT_MAX] = {0};
  for (uint32_t i = 0u; i < selected_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[selected[i]];
    const float32_t own =
        candidate->incumbent_first_view != 0u && !candidate->incumbent_reduced
            ? candidate->score * VKR_LOCAL_SHADOW_INCUMBENT_BONUS
            : candidate->score;
    uint32_t outranked_by = 0u;
    for (uint32_t j = 0u; j < selected_count; ++j) {
      const VkrLocalShadowCandidate *other = &candidates[selected[j]];
      const float32_t theirs =
          other->incumbent_first_view != 0u && !other->incumbent_reduced
              ? other->score * VKR_LOCAL_SHADOW_INCUMBENT_BONUS
              : other->score;
      if (theirs > own || (theirs == own && j < i))
        ++outranked_by;
    }
    reduced[i] = outranked_by >= VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT;
  }

  VkrLocalShadowSelection next = {
      .camera_view = camera->view,
      .camera_position = camera->position,
      .face_budget = out->face_budget,
      .face_count = out->view_count,
      .valid = true_v,
  };
  for (uint32_t i = 0u; i < selected_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[selected[i]];
    vkr_local_shadow_write(
        &lights[candidate->light_index], candidate->light_index, first_views[i],
        candidate->face_count, candidate->half_fov, face_sizes[i],
        face_cells[i], strengths[i], i, reduced[i], out);

    /* Identity drives reuse, layer retention and the crossfade, so a zero or
     * duplicated render id disables them for the next frame. */
    if (candidate->render_id == 0u)
      next.valid = false_v;
    for (uint32_t j = 0u; j < next.group_count; ++j) {
      if (next.groups[j].render_id == candidate->render_id)
        next.valid = false_v;
    }
    next.groups[next.group_count++] = (VkrLocalShadowSelectionGroup){
        .render_id = candidate->render_id,
        .light_kind = candidate->light_kind,
        .face_count = candidate->face_count,
        .first_view = first_views[i],
        .strength = strengths[i],
        .face_size = face_sizes[i],
        .reduced = reduced[i],
    };
    MemCopy(next.groups[next.group_count - 1u].face_cells, face_cells[i],
            sizeof(face_cells[i]));
  }
  *selection = next;
}
