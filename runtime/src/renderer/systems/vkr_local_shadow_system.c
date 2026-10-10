#include "renderer/systems/vkr_local_shadow_system.h"

#include "math/vkr_frustum.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "vkr_temporal.h"

#include <float.h>
#include <math.h>

/* A light that has the full filter or transmission layers keeps them until a
 * competitor outscores it by this factor, so they do not flip while scores
 * cross. */
#define VKR_LOCAL_SHADOW_INCUMBENT_BONUS 1.15f
/* Measured contribution moves continuously with the view, so lights of similar
 * contribution trade places often; incumbents keep their place until a
 * competitor contributes this much more. */
#define VKR_LOCAL_SHADOW_FEEDBACK_INCUMBENT_BONUS 1.5f
/* A light keeps its transmission layers until a competitor outscores it by
 * this factor: taking them redraws the light, and while the camera moves,
 * lights of similar importance would otherwise trade them several times a
 * second. */
#define VKR_LOCAL_SHADOW_TRANSMISSION_INCUMBENT_BONUS 4.0f
/* Seconds for a shadow to fade in once every face of its light is valid. */
#define VKR_LOCAL_SHADOW_FADE_SECONDS 0.25f
/* Camera distances below this fraction of a light's range score alike. */
#define VKR_LOCAL_SHADOW_NEAR_RANGE_FRACTION 0.1f
/* Frames after which measured light contribution no longer describes the
 * view, so priority falls back to distance. Readback lags two to three. */
#define VKR_LOCAL_SHADOW_FEEDBACK_MAX_AGE 8u
/* Lights past this many, by importance, take a single filtered tap and no
 * contact shadows, unless every shown light takes the full filter. */
#define VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT 2u
/* Atlas layer side in cells of the smallest face size. */
#define VKR_LOCAL_SHADOW_ATLAS_CELLS                                           \
  (VKR_LOCAL_SHADOW_ATLAS_SIZE / VKR_LOCAL_SHADOW_FACE_SIZE_MIN)
#define VKR_LOCAL_SHADOW_LAYER_CELLS                                           \
  (VKR_LOCAL_SHADOW_ATLAS_CELLS * VKR_LOCAL_SHADOW_ATLAS_CELLS)
_Static_assert(VKR_LOCAL_SHADOW_ATLAS_CELLS == 32u,
               "atlas occupancy rows are 32-bit masks");

/* Draw order of faces that need drawing: lights that show no shadow first,
 * then lights waiting for transmission layers, then stale content. */
typedef enum VkrLocalShadowRenderClass {
  VKR_LOCAL_SHADOW_RENDER_NONE = 0,
  VKR_LOCAL_SHADOW_RENDER_INVALID,
  VKR_LOCAL_SHADOW_RENDER_TRANSMISSION,
  VKR_LOCAL_SHADOW_RENDER_STALE,
} VkrLocalShadowRenderClass;

/* Occupancy of the atlas layers in smallest-face cells, one bit per column. */
typedef struct VkrLocalShadowAtlas {
  uint32_t rows[VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX]
               [VKR_LOCAL_SHADOW_ATLAS_CELLS];
  uint32_t layer_count;
} VkrLocalShadowAtlas;

typedef struct VkrLocalShadowCandidate {
  uint32_t light_index;
  uint32_t render_id;
  uint32_t light_kind;
  uint32_t face_count;
  float32_t half_fov;
  /** Viewer importance without incumbent bonuses. */
  float32_t score;
  /** Camera-distance fade of the light's shadow in [0, 1]. */
  float32_t distance_fade;
  /** Index + 1 of the matching previous cache light; zero for a newcomer. */
  uint32_t previous;
  uint32_t face_size;
  uint32_t face_cells[6];
  uint32_t transmission_layers[6];
  VkrLocalShadowView views[6];
  VkrLocalShadowRenderClass render_class;
  bool8_t opaque_valid;
  /* Opaque content valid and not stale, so drawing transmission alone
     completes the light. */
  bool8_t opaque_current;
  bool8_t transmission_valid;
  bool8_t wants_transmission;
  bool8_t render;
  /** The light stopped casting; its resident shadow fades out and never
   * draws. */
  bool8_t leaving;
  /** Faces, a bit each, whose content predates static changes that do not
   * reach the light; their history moves to the current generation. */
  uint32_t static_advance;
  /** The light's faces draw every caster: the atlas has no dynamic band, the
   * contents are unstable, or a dynamic refractive caster reaches it. */
  bool8_t all_casters;
  /** Faces, a bit each, that a dynamic caster may reach and the camera may
   * see, while the light's static squares hold the static casters only. */
  uint32_t dynamic_wanted;
  /** Faces that show a dynamic square this frame, its cell and the index of
   * the companion view of the static square. */
  uint32_t dynamic_mask;
  uint32_t dynamic_cells[6];
  uint32_t companion_views[6];
  /** A baked lamp: its static squares take the baked layer, it draws first
   * and takes no transmission or filter place, and it publishes as a baked
   * lamp instead of a light of the table. */
  bool8_t baked;
  /** First receiver view the light publishes. */
  uint32_t first_view;
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

/* Basis of face `face`'s view: right, up and forward, as its view matrix
 * builds them. */
vkr_internal void vkr_local_shadow_face_basis(const VkrPointLight *light,
                                              uint32_t face, Vec3 *out_right,
                                              Vec3 *out_up, Vec3 *out_forward) {
  const Vec3 forward = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                           ? vec3_normalize(light->direction)
                           : s_face_direction[face];
  const Vec3 up = fabsf(forward.y) > 0.99f ? (Vec3){0, 0, 1} : (Vec3){0, 1, 0};
  const Vec3 right = vec3_normalize(vec3_cross(forward, up));
  *out_right = right;
  *out_up = vec3_cross(right, forward);
  *out_forward = forward;
}

uint32_t vkr_local_shadow_sphere_faces(const VkrPointLight *light, Vec3 center,
                                       float32_t radius) {
  uint32_t face_count;
  float32_t half_fov;
  if (!vkr_local_shadow_light_valid(light, &face_count, &half_fov))
    return 0u;
  const uint32_t all_faces = (UINT32_C(1) << face_count) - 1u;
  if (!isfinite(center.x) || !isfinite(center.y) || !isfinite(center.z) ||
      !isfinite(radius) || radius < 0.0f)
    return all_faces;

  /* A side plane of a face's square pyramid keeps the points whose
     component along the face axis, times the sine of the half angle, is at
     least their side component times its cosine; the sphere may meet the
     pyramid while its center lies within `radius` of every side plane. */
  const Vec3 delta = vec3_sub(center, light->position);
  const float32_t sine = sinf(half_fov);
  const float32_t cosine = cosf(half_fov);
  uint32_t faces = 0u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    vkr_local_shadow_face_basis(light, face, &right, &up, &forward);
    const float32_t reach = vec3_dot(delta, forward) * sine + radius;
    if (reach >= fabsf(vec3_dot(delta, right)) * cosine &&
        reach >= fabsf(vec3_dot(delta, up)) * cosine)
      faces |= UINT32_C(1) << face;
  }
  return faces;
}

/* Faces, a bit each, whose pyramid out to the light's range may meet the
 * camera frustum: those not wholly outside one of its side planes. Faces
 * receivers cannot show take no dynamic square. */
vkr_internal uint32_t vkr_local_shadow_visible_faces(const VkrPointLight *light,
                                                     uint32_t face_count,
                                                     float32_t half_fov,
                                                     const VkrFrustum *camera) {
  const float32_t extent = light->range * tanf(half_fov);
  uint32_t faces = 0u;
  for (uint32_t face = 0u; face < face_count; ++face) {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    vkr_local_shadow_face_basis(light, face, &right, &up, &forward);
    const Vec3 far_center =
        vec3_add(light->position, vec3_scale(forward, light->range));
    Vec3 corners[5] = {light->position};
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
      const float32_t x = (corner & 1u) ? extent : -extent;
      const float32_t y = (corner & 2u) ? extent : -extent;
      corners[corner + 1u] = vec3_add(
          far_center, vec3_add(vec3_scale(right, x), vec3_scale(up, y)));
    }
    bool8_t visible = true_v;
    for (uint32_t plane = VKR_FRUSTUM_PLANE_LEFT;
         visible && plane <= VKR_FRUSTUM_PLANE_TOP; ++plane) {
      const VkrPlane *side = &camera->planes[plane];
      bool8_t outside = true_v;
      for (uint32_t corner = 0u; outside && corner < 5u; ++corner)
        outside = vec3_dot(side->normal, corners[corner]) + side->d < 0.0f;
      visible = !outside;
    }
    if (visible)
      faces |= UINT32_C(1) << face;
  }
  return faces;
}

uint32_t vkr_local_shadow_camera_faces(const VkrPointLight *light, Mat4 view,
                                       Mat4 projection) {
  uint32_t face_count;
  float32_t half_fov;
  if (!vkr_local_shadow_light_valid(light, &face_count, &half_fov))
    return 0u;
  if (MemCompare(&projection, &(Mat4){0}, sizeof(Mat4)) == 0)
    return (UINT32_C(1) << face_count) - 1u;
  const VkrFrustum camera = vkr_frustum_from_view_projection(view, projection);
  return vkr_local_shadow_visible_faces(light, face_count, half_fov, &camera);
}

vkr_internal bool8_t vkr_local_shadow_map_size_valid(uint32_t map_size) {
  return map_size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
         map_size <= VKR_LOCAL_SHADOW_MAP_SIZE_MAX &&
         (map_size & (map_size - 1u)) == 0u;
}

uint32_t vkr_local_shadow_face_size_for_range(float32_t range,
                                              uint32_t map_size) {
  const float32_t texels = range * VKR_LOCAL_SHADOW_TEXELS_PER_RANGE_METRE;
  uint32_t size = VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
  /* Rounds to the nearest power of two in log space. */
  while (size < map_size && (float32_t)size * 1.41421356f < texels)
    size *= 2u;
  return size;
}

vkr_internal uint32_t vkr_local_shadow_cell(uint32_t cell_x, uint32_t cell_y,
                                            uint32_t layer) {
  return cell_x | (cell_y << 8u) | (layer << 16u);
}

vkr_internal uint32_t vkr_local_shadow_row_bits(uint32_t cell_x,
                                                uint32_t cells) {
  return (uint32_t)(((UINT64_C(1) << cells) - 1u) << cell_x);
}

vkr_internal bool8_t vkr_local_shadow_atlas_free(
    const VkrLocalShadowAtlas *atlas, uint32_t cell, uint32_t cells) {
  const uint32_t cell_x = cell & 0xFFu;
  const uint32_t cell_y = (cell >> 8u) & 0xFFu;
  const uint32_t layer = cell >> 16u;
  if (layer >= atlas->layer_count || cell_x % cells != 0u ||
      cell_y % cells != 0u || cell_x + cells > VKR_LOCAL_SHADOW_ATLAS_CELLS ||
      cell_y + cells > VKR_LOCAL_SHADOW_ATLAS_CELLS)
    return false_v;
  const uint32_t bits = vkr_local_shadow_row_bits(cell_x, cells);
  for (uint32_t row = cell_y; row < cell_y + cells; ++row) {
    if ((atlas->rows[layer][row] & bits) != 0u)
      return false_v;
  }
  return true_v;
}

vkr_internal void vkr_local_shadow_atlas_mark(VkrLocalShadowAtlas *atlas,
                                              uint32_t cell, uint32_t cells) {
  const uint32_t cell_x = cell & 0xFFu;
  const uint32_t cell_y = (cell >> 8u) & 0xFFu;
  const uint32_t bits = vkr_local_shadow_row_bits(cell_x, cells);
  for (uint32_t row = cell_y; row < cell_y + cells; ++row)
    atlas->rows[cell >> 16u][row] |= bits;
}

/* First free aligned square, by layer then row-major. When squares are placed
 * in descending size, every earlier square is aligned to the current size, so
 * a free square exists whenever enough area remains. */
vkr_internal bool8_t vkr_local_shadow_atlas_allocate(VkrLocalShadowAtlas *atlas,
                                                     uint32_t face_size,
                                                     uint32_t *out_cell) {
  const uint32_t cells = face_size / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
  for (uint32_t layer = 0u; layer < atlas->layer_count; ++layer) {
    for (uint32_t cell_y = 0u; cell_y < VKR_LOCAL_SHADOW_ATLAS_CELLS;
         cell_y += cells) {
      for (uint32_t cell_x = 0u; cell_x < VKR_LOCAL_SHADOW_ATLAS_CELLS;
           cell_x += cells) {
        const uint32_t cell = vkr_local_shadow_cell(cell_x, cell_y, layer);
        if (vkr_local_shadow_atlas_free(atlas, cell, cells)) {
          vkr_local_shadow_atlas_mark(atlas, cell, cells);
          *out_cell = cell;
          return true_v;
        }
      }
    }
  }
  return false_v;
}

vkr_internal uint64_t vkr_local_shadow_candidate_cells(
    const VkrLocalShadowCandidate *candidates, uint32_t candidate_count) {
  uint64_t cells = 0u;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    if (candidates[i].baked)
      continue;
    const uint64_t side =
        candidates[i].face_size / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
    cells += (uint64_t)candidates[i].face_count * side * side;
  }
  return cells;
}

/* Shrinks the lowest-scoring lights one size at a time until every face fits
 * the largest atlas. Power-of-two squares whose area fits always pack. */
vkr_internal void
vkr_local_shadow_fit_atlas(VkrLocalShadowCandidate *candidates,
                           uint32_t candidate_count) {
  const uint64_t capacity = (uint64_t)VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX *
                            VKR_LOCAL_SHADOW_LAYER_CELLS;
  while (vkr_local_shadow_candidate_cells(candidates, candidate_count) >
         capacity) {
    uint32_t weakest = UINT32_MAX;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      if (!candidates[i].baked &&
          candidates[i].face_size > VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
          (weakest == UINT32_MAX ||
           candidates[i].score <= candidates[weakest].score))
        weakest = i;
    }
    if (weakest == UINT32_MAX)
      return;
    candidates[weakest].face_size /= 2u;
  }
}

/* Places every face of the casting lights, `candidates` below `shown_count`;
 * the leaving lights after them only keep their previous squares. With an
 * unchanged layer count, a light whose size is unchanged keeps its previous
 * squares when they are still free; the casting others take the first free
 * squares in descending size. If that leaves no room, the leaving lights give
 * up their squares, and if there is still none, every casting light is packed
 * afresh. Removes the leaving lights left without squares and returns the
 * remaining candidate count. Static squares take the first
 * `static_layer_count` layers; the dynamic band follows them. */
vkr_internal uint32_t vkr_local_shadow_pack_atlas(
    const VkrLocalShadowCache *cache, VkrLocalShadowCandidate *candidates,
    uint32_t shown_count, uint32_t candidate_count, uint32_t layer_count,
    uint32_t static_layer_count) {
  const bool8_t keep = cache->valid && cache->atlas_layer_count == layer_count;
  bool8_t placed[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
    VkrLocalShadowAtlas atlas = {.layer_count = static_layer_count};
    MemZero(placed, sizeof(placed));
    const uint32_t kept_count = !keep           ? 0u
                                : attempt == 0u ? candidate_count
                                : attempt == 1u ? shown_count
                                                : 0u;
    for (uint32_t i = 0u; i < kept_count; ++i) {
      VkrLocalShadowCandidate *candidate = &candidates[i];
      if (candidate->previous == 0u || candidate->baked)
        continue;
      const VkrLocalShadowCacheLight *previous =
          &cache->lights[candidate->previous - 1u];
      if (previous->face_size != candidate->face_size)
        continue;
      const uint32_t cells =
          candidate->face_size / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
      VkrLocalShadowAtlas trial = atlas;
      bool8_t fits = true_v;
      for (uint32_t face = 0u; fits && face < candidate->face_count; ++face) {
        fits = vkr_local_shadow_atlas_free(&trial, previous->face_cells[face],
                                           cells);
        if (fits)
          vkr_local_shadow_atlas_mark(&trial, previous->face_cells[face],
                                      cells);
      }
      if (!fits)
        continue;
      atlas = trial;
      placed[i] = true_v;
      MemCopy(candidate->face_cells, previous->face_cells,
              sizeof(candidate->face_cells));
    }

    bool8_t complete = true_v;
    for (uint32_t size = VKR_LOCAL_SHADOW_MAP_SIZE_MAX;
         complete && size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN; size /= 2u) {
      for (uint32_t i = 0u; complete && i < shown_count; ++i) {
        VkrLocalShadowCandidate *candidate = &candidates[i];
        if (placed[i] || candidate->baked || candidate->face_size != size)
          continue;
        for (uint32_t face = 0u; complete && face < candidate->face_count;
             ++face)
          complete = vkr_local_shadow_atlas_allocate(
              &atlas, size, &candidate->face_cells[face]);
      }
    }
    if (complete) {
      break;
    }
  }

  uint32_t count = shown_count;
  for (uint32_t i = shown_count; i < candidate_count; ++i) {
    if (placed[i]) {
      candidates[count++] = candidates[i];
    }
  }
  return count;
}

/* Atlas rect of the square of side `face_size` at `cell`: its origin and side
 * in atlas UV and its layer. */
vkr_internal Vec4 vkr_local_shadow_cell_rect(uint32_t cell,
                                             uint32_t face_size) {
  const float32_t cell_uv = (float32_t)VKR_LOCAL_SHADOW_FACE_SIZE_MIN /
                            (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
  return (Vec4){(float32_t)(cell & 0xFFu) * cell_uv,
                (float32_t)((cell >> 8u) & 0xFFu) * cell_uv,
                (float32_t)face_size / (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE,
                (float32_t)(cell >> 16u)};
}

/* Places the baked lamps' static squares in the layer at `layer`: a lamp
 * keeps its previous squares there when they are free, the others take the
 * first free ones, largest first. Returns false when one does not fit. */
vkr_internal bool8_t vkr_local_shadow_place_baked(
    const VkrLocalShadowCache *cache, VkrLocalShadowCandidate *candidates,
    uint32_t candidate_count, uint32_t layer) {
  VkrLocalShadowAtlas atlas = {.layer_count = 1u};
  bool8_t placed[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX] = {0};
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    if (!candidate->baked || candidate->previous == 0u)
      continue;
    const VkrLocalShadowCacheLight *previous =
        &cache->lights[candidate->previous - 1u];
    if (previous->face_size != candidate->face_size)
      continue;
    const uint32_t cells =
        candidate->face_size / VKR_LOCAL_SHADOW_FACE_SIZE_MIN;
    VkrLocalShadowAtlas trial = atlas;
    bool8_t fits = true_v;
    for (uint32_t face = 0u; fits && face < candidate->face_count; ++face) {
      const uint32_t cell = previous->face_cells[face];
      fits = (cell >> 16u) == layer &&
             vkr_local_shadow_atlas_free(&trial, cell & 0xFFFFu, cells);
      if (fits)
        vkr_local_shadow_atlas_mark(&trial, cell & 0xFFFFu, cells);
    }
    if (!fits)
      continue;
    atlas = trial;
    placed[i] = true_v;
    MemCopy(candidate->face_cells, previous->face_cells,
            sizeof(candidate->face_cells));
  }
  for (uint32_t size = VKR_LOCAL_SHADOW_MAP_SIZE_MAX;
       size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN; size /= 2u) {
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      VkrLocalShadowCandidate *candidate = &candidates[i];
      if (!candidate->baked || placed[i] || candidate->face_size != size)
        continue;
      for (uint32_t face = 0u; face < candidate->face_count; ++face) {
        uint32_t cell = 0u;
        if (!vkr_local_shadow_atlas_allocate(&atlas, size, &cell))
          return false_v;
        candidate->face_cells[face] = cell | (layer << 16u);
      }
    }
  }
  return true_v;
}

/* `source_radius` is the emitter radius receivers use for contact-hardening,
 * zero for the fixed filter. Receivers scale this per-face UV footprint by
 * blocker and receiver distances along the face axis. */
vkr_internal void vkr_local_shadow_write_views(
    const VkrPointLight *light, const VkrLocalShadowCandidate *candidate,
    float32_t strength, bool8_t reduced, float32_t source_radius,
    VkrLocalShadowView *out_views) {
  const float32_t near_clip = Min(0.05f, light->range * 0.01f);
  const float32_t tan_half_fov = tanf(candidate->half_fov);
  const float32_t source_radius_uv =
      reduced ? 0.0f : source_radius / (2.0f * tan_half_fov);
  const Mat4 projection = mat4_perspective(2.0f * candidate->half_fov, 1.0f,
                                           near_clip, light->range);
  for (uint32_t face = 0u; face < candidate->face_count; ++face) {
    const Vec3 direction = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                               ? vec3_normalize(light->direction)
                               : s_face_direction[face];
    const Vec3 up =
        fabsf(direction.y) > 0.99f ? (Vec3){0, 0, 1} : (Vec3){0, 1, 0};
    const Mat4 view =
        mat4_look_at(light->position, vec3_add(light->position, direction), up);
    const uint32_t cell = candidate->face_cells[face];
    out_views[face] = (VkrLocalShadowView){
        .light_view_projection = mat4_mul(projection, view),
        .light_position_near = {light->position.x, light->position.y,
                                light->position.z, near_clip},
        .light_direction_far = {direction.x, direction.y, direction.z,
                                light->range},
        .projection_params = {tan_half_fov,
                              1.0f / (float32_t)candidate->face_size, 1.0f,
                              2.0f},
        .shadow_params = {strength,
                          (float32_t)candidate->transmission_layers[face],
                          reduced ? 1.0f : 0.0f, source_radius_uv},
        .atlas_rect = vkr_local_shadow_cell_rect(cell, candidate->face_size),
    };
  }
}

/* Depth contents depend on the face projection and atlas square only; shadow
 * strength, filter and transmission layer are receiver-side. */
vkr_internal bool8_t vkr_local_shadow_view_projection_equal(
    const VkrLocalShadowView *a, const VkrLocalShadowView *b) {
  return MemCompare(&a->light_view_projection, &b->light_view_projection,
                    sizeof(a->light_view_projection)) == 0 &&
         MemCompare(&a->light_position_near, &b->light_position_near,
                    sizeof(a->light_position_near)) == 0 &&
         MemCompare(&a->light_direction_far, &b->light_direction_far,
                    sizeof(a->light_direction_far)) == 0 &&
         MemCompare(&a->projection_params, &b->projection_params,
                    sizeof(a->projection_params)) == 0 &&
         MemCompare(&a->atlas_rect, &b->atlas_rect, sizeof(a->atlas_rect)) == 0;
}

/* Measured contribution of the light with `render_id`; the table is usually
 * unchanged between the sample's frame and this one. */
vkr_internal float32_t
vkr_local_shadow_feedback_score(const VkrLocalLightContributionSample *feedback,
                                uint32_t light_index, uint32_t render_id) {
  if (light_index < feedback->light_count &&
      feedback->render_ids[light_index] == render_id)
    return (float32_t)feedback->contribution[light_index];
  for (uint32_t i = 0u; i < feedback->light_count; ++i) {
    if (feedback->render_ids[i] == render_id)
      return (float32_t)feedback->contribution[i];
  }
  return 0.0f;
}

/* Index + 1 of the cache light that `candidate` continues; identity needs a
 * nonzero render id that no other resident light shares. */
vkr_internal uint32_t
vkr_local_shadow_previous_light(const VkrLocalShadowCache *cache,
                                const VkrLocalShadowCandidate *candidate) {
  if (!cache->valid || candidate->render_id == 0u)
    return 0u;
  for (uint32_t i = 0u; i < cache->light_count; ++i) {
    const VkrLocalShadowCacheLight *light = &cache->lights[i];
    if (light->render_id == candidate->render_id &&
        light->light_kind == candidate->light_kind &&
        light->face_count == candidate->face_count)
      return i + 1u;
  }
  return 0u;
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

/* Candidate indices by descending `keys`, ties in candidate order. */
vkr_internal void vkr_local_shadow_order_by_key(const float32_t *keys,
                                                uint32_t count,
                                                uint32_t *out_order) {
  for (uint32_t i = 0u; i < count; ++i) {
    uint32_t insert = i;
    while (insert > 0u && keys[out_order[insert - 1u]] < keys[i]) {
      out_order[insert] = out_order[insert - 1u];
      --insert;
    }
    out_order[insert] = i;
  }
}

/* A rebuilt pool has a new generation; content drawn into it by the frame
 * that created it recorded generation zero and is adopted here, while its
 * layers' retained validity still decides whether it survived. */
vkr_internal void
vkr_local_shadow_adopt_new_pool(VkrLocalShadowCache *cache,
                                const VkrRetainedLocalShadowToken *token) {
  static const uint64_t
      zero_generations[VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT] = {0};
  for (uint32_t i = 0u; i < cache->light_count; ++i) {
    VkrLocalShadowCacheLight *light = &cache->lights[i];
    for (uint32_t face = 0u; face < light->face_count; ++face) {
      VkrLocalShadowFaceHistory *history = &light->faces[face];
      if (history->last_submit_value == 0u)
        continue;
      if (history->resource_generation == 0u)
        history->resource_generation = token->resource_generation;
      if (history->transmission_layer != 0u &&
          MemCompare(history->transmission_resource_generations,
                     zero_generations, sizeof(zero_generations)) == 0)
        MemCopy(history->transmission_resource_generations,
                token->transmission_resource_generations,
                sizeof(history->transmission_resource_generations));
    }
  }
}

/* Shadows fade out with camera distance, which bounds the shadowed lights a
 * pixel filters; a faded light stays resident and fades back in. */
vkr_internal float32_t vkr_local_shadow_distance_fade(
    const VkrLocalShadowCacheInput *input, const VkrPointLight *light) {
  const float32_t distance = sqrtf(
      vec3_length_squared(vec3_sub(light->position, input->camera->position)));
  const float32_t distance_fade =
      (input->fade_distance - distance) / VKR_LOCAL_SHADOW_FADE_BAND_METRES;
  return isfinite(distance_fade) ? Clamp(distance_fade, 0.0f, 1.0f) : 0.0f;
}

/* Scores every valid light and sizes its faces by range. With `feedback`,
 * measured contribution scores the light; otherwise distance does. */
vkr_internal uint32_t vkr_local_shadow_gather_candidates(
    const VkrLocalShadowCacheInput *input,
    const VkrLocalLightContributionSample *feedback,
    VkrLocalShadowCandidate *candidates) {
  const VkrLocalShadowCamera *camera = input->camera;
  uint32_t candidate_count = 0u;
  for (uint32_t i = 0u;
       i < Min(input->light_count, VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX); ++i) {
    const VkrPointLight *light = &input->lights[i];
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(light, &face_count, &half_fov))
      continue;
    const Vec3 delta = vec3_sub(light->position, camera->position);
    const float32_t distance_squared = vec3_length_squared(delta);
    const float32_t distance_fade =
        vkr_local_shadow_distance_fade(input, light);
    const float32_t near_distance =
        light->range * VKR_LOCAL_SHADOW_NEAR_RANGE_FRACTION;
    const float32_t luminance = light->color.x * 0.2126f +
                                light->color.y * 0.7152f +
                                light->color.z * 0.0722f;
    /* A baked lamp the frame shadows draws first: the selection only
       picks lamps whose moving casters show. */
    const bool8_t baked = i + input->baked_light_count >= input->light_count;
    float32_t score =
        baked ? FLT_MAX
        : feedback
            ? vkr_local_shadow_feedback_score(feedback, i, light->render_id)
            : Max(luminance, 0.0f) * Max(light->intensity, 0.0f) *
                  light->range * light->range /
                  Max(distance_squared, near_distance * near_distance);
    if (!isfinite(score) || score < 0.0f)
      score = 0.0f;
    candidates[candidate_count++] = (VkrLocalShadowCandidate){
        .light_index = i,
        .render_id = light->render_id,
        .light_kind = (uint32_t)light->kind,
        .face_count = face_count,
        .half_fov = half_fov,
        .score = score,
        .distance_fade = distance_fade,
        .face_size =
            vkr_local_shadow_face_size_for_range(light->range, input->map_size),
        .baked = baked,
    };
  }
  return candidate_count;
}

/* Appends the lights that stopped casting while their shadow still showed,
 * such as one the tiled pipeline's dynamic-light limit no longer shadows:
 * each keeps its resident faces while its shadow fades out. A light needs an
 * atlas of the previous layer count and the previous cache light no other
 * candidate continues, at the same face size; the packing then keeps its
 * squares unless the casting lights need them. It never draws, scores zero
 * and takes no transmission layers. Returns the new candidate count. */
vkr_internal uint32_t vkr_local_shadow_gather_leaving(
    const VkrLocalShadowCache *cache, const VkrLocalShadowCacheInput *input,
    uint32_t layer_count, VkrLocalShadowCandidate *candidates,
    uint32_t candidate_count) {
  if (!cache->valid || cache->atlas_layer_count != layer_count) {
    return candidate_count;
  }

  bool8_t claimed[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX] = {0};
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    if (candidates[i].previous != 0u) {
      claimed[candidates[i].previous - 1u] = true_v;
    }
  }

  for (uint32_t i = 0u;
       i < Min(input->light_count, VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX); ++i) {
    const VkrPointLight *light = &input->lights[i];
    if (light->casts_shadow || light->render_id == 0u) {
      continue;
    }
    VkrPointLight casting = *light;
    casting.casts_shadow = true_v;
    uint32_t face_count;
    float32_t half_fov;
    if (!vkr_local_shadow_light_valid(&casting, &face_count, &half_fov)) {
      continue;
    }

    VkrLocalShadowCandidate candidate = {
        .light_index = i,
        .render_id = light->render_id,
        .light_kind = (uint32_t)light->kind,
        .face_count = face_count,
        .half_fov = half_fov,
        .distance_fade = vkr_local_shadow_distance_fade(input, light),
        .face_size =
            vkr_local_shadow_face_size_for_range(light->range, input->map_size),
        .leaving = true_v,
    };
    candidate.previous = vkr_local_shadow_previous_light(cache, &candidate);
    if (candidate.previous == 0u || claimed[candidate.previous - 1u]) {
      continue;
    }
    const VkrLocalShadowCacheLight *previous =
        &cache->lights[candidate.previous - 1u];
    if (previous->strength * previous->distance_fade <= 0.0f ||
        previous->face_size != candidate.face_size) {
      continue;
    }

    claimed[candidate.previous - 1u] = true_v;
    candidates[candidate_count++] = candidate;
  }
  return candidate_count;
}

/* Whether static changes after generation `since` may reach `light`'s
 * range: the list does not cover them, one is unbounded, or one's box meets
 * the light's sphere. */
vkr_internal bool8_t
vkr_local_shadow_static_reached(const VkrLocalShadowCacheInput *input,
                                uint64_t since, const VkrPointLight *light) {
  const VkrWorldPassPayload changes = {
      .static_generation = input->static_generation,
      .static_changes = input->static_changes,
      .static_change_count = input->static_change_count,
      .static_change_floor = input->static_change_floor,
  };
  return vkr_world_static_changes_reach(&changes, since, light->position,
                                        light->range);
}

/* Faces of the light at `light_index` that a dynamic caster may reach. */
vkr_internal uint32_t vkr_local_shadow_reached_faces(
    const VkrLocalShadowCacheInput *input, uint32_t light_index) {
  return input->dynamic_faces ? input->dynamic_faces[light_index] : 0u;
}

/* Content is invalid once its projection, square or pool changed, and
 * stale while a dynamic caster, a publication or a static-world change may
 * have altered it. Stale content still shows while it waits to redraw. */
vkr_internal void vkr_local_shadow_classify_content(
    const VkrLocalShadowCache *cache, const VkrLocalShadowCacheInput *input,
    VkrLocalShadowCandidate *candidates, uint32_t candidate_count,
    uint32_t valid_layers, uint32_t transmission_layer_count) {
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    const VkrPointLight *light = &input->lights[candidate->light_index];
    vkr_local_shadow_write_views(light, candidate, 0.0f, false_v, 0.0f,
                                 candidate->views);
    const VkrLocalShadowCacheLight *previous =
        candidate->previous ? &cache->lights[candidate->previous - 1u] : NULL;
    bool8_t opaque_valid = previous != NULL;
    bool8_t transmission_valid = candidate->transmission_layers[0] != 0u;
    /* A static square holds the static casters only, so a dynamic caster
       stales only the faces of a light that draws every caster. */
    bool8_t stale =
        input->contents_unstable ||
        (candidate->all_casters &&
         vkr_local_shadow_reached_faces(input, candidate->light_index) != 0u);
    for (uint32_t face = 0u; previous && face < candidate->face_count; ++face) {
      const VkrLocalShadowFaceHistory *history = &previous->faces[face];
      const uint32_t layer = candidate->face_cells[face] >> 16u;
      opaque_valid =
          opaque_valid && history->last_submit_value != 0u &&
          history->resource_generation == input->token.resource_generation &&
          (valid_layers & (UINT32_C(1) << layer)) != 0u &&
          vkr_local_shadow_view_projection_equal(&history->view,
                                                 &candidate->views[face]);
      const uint32_t transmission = candidate->transmission_layers[face];
      transmission_valid =
          transmission_valid && history->transmission_layer == transmission &&
          transmission - 1u < transmission_layer_count &&
          MemCompare(history->transmission_resource_generations,
                     input->token.transmission_resource_generations,
                     sizeof(history->transmission_resource_generations)) == 0;
      const bool8_t static_reached = vkr_local_shadow_static_reached(
          input, history->static_generation, light);
      if (!static_reached &&
          history->static_generation != input->static_generation) {
        candidate->static_advance |= UINT32_C(1) << face;
      }
      stale = stale || !history->static_only_contents || static_reached ||
              history->publication_generation != input->publication_generation;
    }
    candidate->opaque_valid = opaque_valid;
    candidate->opaque_current = opaque_valid && !stale;
    candidate->transmission_valid = transmission_valid && opaque_valid;
    candidate->render_class =
        !opaque_valid ? VKR_LOCAL_SHADOW_RENDER_INVALID
        : candidate->wants_transmission && !candidate->transmission_valid
            ? VKR_LOCAL_SHADOW_RENDER_TRANSMISSION
        : stale ? VKR_LOCAL_SHADOW_RENDER_STALE
                : VKR_LOCAL_SHADOW_RENDER_NONE;
  }
}

/* Places the dynamic squares in the band layer at `band_layer`, largest
 * first. A face keeps its dynamic square only while its light publishes
 * views and its static square holds valid content or draws this frame. */
vkr_internal void vkr_local_shadow_place_dynamic(
    VkrLocalShadowCandidate *candidates, uint32_t candidate_count,
    const float32_t *strengths, uint32_t band_layer) {
  VkrLocalShadowAtlas band = {.layer_count = 1u};
  for (uint32_t size = VKR_LOCAL_SHADOW_MAP_SIZE_MAX;
       size >= VKR_LOCAL_SHADOW_FACE_SIZE_MIN; size /= 2u) {
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      VkrLocalShadowCandidate *candidate = &candidates[i];
      if (candidate->dynamic_mask == 0u || candidate->face_size != size)
        continue;
      const bool8_t published =
          strengths[i] * candidate->distance_fade > 0.0f || candidate->render;
      if (!published || !(candidate->opaque_valid || candidate->render)) {
        candidate->dynamic_mask = 0u;
        continue;
      }
      for (uint32_t face = 0u; face < candidate->face_count; ++face) {
        if ((candidate->dynamic_mask & (UINT32_C(1) << face)) == 0u)
          continue;
        uint32_t cell = 0u;
        /* The band holds the capped count at the largest size. */
        if (!vkr_local_shadow_atlas_allocate(&band, size, &cell)) {
          candidate->dynamic_mask &= ~(UINT32_C(1) << face);
          continue;
        }
        candidate->dynamic_cells[face] = cell | (band_layer << 16u);
      }
    }
  }
}

/* Publishes the shown lights' views; render slots list drawn faces with
 * transmission first and dynamic squares last. */
vkr_internal void vkr_local_shadow_publish(
    const VkrLocalShadowCache *cache, VkrLocalShadowCache *next,
    const VkrLocalShadowCacheInput *input, VkrLocalShadowCandidate *candidates,
    uint32_t candidate_count, const float32_t *strengths,
    const bool8_t *reduced, uint64_t feedback_after_frame, uint32_t layer_count,
    bool8_t identity_valid, VkrLocalShadowPendingHistory *pending,
    VkrLocalShadowPassPayload *out) {
  const VkrLocalShadowCamera *camera = input->camera;
  MemZero(next, sizeof(*next));
  next->light_count = candidate_count;
  next->atlas_layer_count = layer_count;
  next->face_budget = out->face_budget;
  next->camera_view = camera->view;
  next->camera_position = camera->position;
  next->feedback_after_frame = feedback_after_frame;
  next->valid = identity_valid;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    const VkrPointLight *light = &input->lights[candidate->light_index];
    const VkrLocalShadowCacheLight *previous =
        candidate->previous ? &cache->lights[candidate->previous - 1u] : NULL;
    VkrLocalShadowCacheLight *entry = &next->lights[i];
    *entry = (VkrLocalShadowCacheLight){
        .render_id = candidate->render_id,
        .light_kind = candidate->light_kind,
        .face_count = candidate->face_count,
        .face_size = candidate->face_size,
        .strength = strengths[i],
        .distance_fade = candidate->distance_fade,
        .reduced = reduced[i],
    };
    MemCopy(entry->face_cells, candidate->face_cells,
            sizeof(entry->face_cells));
    MemCopy(entry->transmission_layers, candidate->transmission_layers,
            sizeof(entry->transmission_layers));
    if (previous) {
      MemCopy(entry->faces, previous->faces, sizeof(entry->faces));
      for (uint32_t face = 0u; face < candidate->face_count; ++face) {
        if (candidate->static_advance & (UINT32_C(1) << face)) {
          entry->faces[face].static_generation = input->static_generation;
        }
      }
    }
    /* A drawn light is a view whatever its strength, since render slots name
     * views; receivers skip a light at zero strength. */
    const float32_t strength = strengths[i] * candidate->distance_fade;
    if (strength <= 0.0f && !candidate->render)
      continue;

    /* Receivers sample transmission only once its layers hold this light. */
    if (!candidate->render && !candidate->transmission_valid)
      MemZero(candidate->transmission_layers,
              sizeof(candidate->transmission_layers));
    const uint32_t first_view = out->view_count;
    const float32_t source_radius =
        input->soft_shadows && isfinite(light->source_radius) &&
                light->source_radius > 0.0f
            ? Min(light->source_radius, VKR_POINT_LIGHT_SOURCE_RADIUS_MAX)
            : 0.0f;
    vkr_local_shadow_write_views(light, candidate, strength, reduced[i],
                                 source_radius, &out->views[first_view]);
    candidate->first_view = first_view;
    out->view_count += candidate->face_count;
    if (!candidate->baked) {
      out->light_first_view[candidate->light_index] = first_view + 1u;
      continue;
    }
    /* A baked lamp's static block repeats its views, whose squares are all
       static until the dynamic squares replace the composite ones. */
    MemCopy(&out->views[out->view_count], &out->views[first_view],
            candidate->face_count * sizeof(*out->views));
    out->baked_lamps[out->baked_lamp_count++] = (VkrLocalShadowBakedLamp){
        .light = *light,
        .composite_first_view = first_view,
        .static_first_view = out->view_count,
    };
    out->view_count += candidate->face_count;
  }

  /* Receivers of a face with a dynamic square sample it; a companion view
     after the receiver views keeps the face's static square, which its
     render slot draws and the copy reads. */
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    for (uint32_t face = 0u; candidate->dynamic_mask && face < 6u; ++face) {
      if ((candidate->dynamic_mask & (UINT32_C(1) << face)) == 0u)
        continue;
      const uint32_t view = candidate->first_view + face;
      const uint32_t companion = out->view_count + out->companion_view_count++;
      out->views[companion] = out->views[view];
      out->views[view].atlas_rect = vkr_local_shadow_cell_rect(
          candidate->dynamic_cells[face], candidate->face_size);
      candidate->companion_views[face] = companion;
    }
  }

  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    const bool8_t transmission_pass = pass == 0u;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      const VkrLocalShadowCandidate *candidate = &candidates[i];
      if (!candidate->render ||
          (candidate->transmission_layers[0] != 0u) != transmission_pass)
        continue;
      const uint32_t first_view = candidate->first_view;
      /* A light drawn only for its transmission layers keeps its opaque
         faces and their history. */
      const bool8_t retained =
          transmission_pass && candidate->previous && candidate->opaque_current;
      const bool8_t static_only_contents =
          !input->contents_unstable &&
          (!candidate->all_casters ||
           vkr_local_shadow_reached_faces(input, candidate->light_index) == 0u);
      for (uint32_t face = 0u; face < candidate->face_count; ++face) {
        /* The slot draws the face's static square. */
        const uint32_t view =
            (candidate->dynamic_mask & (UINT32_C(1) << face)) != 0u
                ? candidate->companion_views[face]
                : first_view + face;
        if (!candidate->all_casters)
          out->static_render_mask |= UINT64_C(1) << out->render_count;
        if (retained) {
          out->retained_opaque_mask |= UINT64_C(1) << out->render_count;
          VkrLocalShadowFaceHistory history =
              cache->lights[candidate->previous - 1u].faces[face];
          history.transmission_layer = candidate->transmission_layers[face];
          MemCopy(history.transmission_resource_generations,
                  input->token.transmission_resource_generations,
                  sizeof(history.transmission_resource_generations));
          out->render_views[out->render_count++] = view;
          pending->faces[pending->face_count++] = (VkrLocalShadowPendingFace){
              .light = i, .face = face, .history = history};
          continue;
        }
        out->render_views[out->render_count++] = view;
        pending->faces[pending->face_count++] = (VkrLocalShadowPendingFace){
            .light = i,
            .face = face,
            .history =
                {
                    .view = out->views[view],
                    .static_generation = input->static_generation,
                    .publication_generation = input->publication_generation,
                    .resource_generation = input->token.resource_generation,
                    .transmission_layer = candidate->transmission_layers[face],
                    .static_only_contents = static_only_contents,
                },
        };
        MemCopy(pending->faces[pending->face_count - 1u]
                    .history.transmission_resource_generations,
                input->token.transmission_resource_generations,
                sizeof(input->token.transmission_resource_generations));
      }
      if (transmission_pass)
        out->transmission_render_count = out->render_count;
    }
  }

  /* Dynamic squares draw last, after the static squares they copy; their
     content lasts one frame, so they commit no history. */
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[i];
    for (uint32_t face = 0u; candidate->dynamic_mask && face < 6u; ++face) {
      if ((candidate->dynamic_mask & (UINT32_C(1) << face)) == 0u)
        continue;
      out->render_views[out->render_count] = candidate->first_view + face;
      out->dynamic_source_views[out->render_count] =
          candidate->companion_views[face];
      out->render_count++;
      out->dynamic_render_count++;
    }
  }
}

void vkr_local_shadow_cache_resolve(VkrLocalShadowCache *cache,
                                    VkrLocalShadowCache *scratch,
                                    const VkrLocalShadowCacheInput *input,
                                    VkrLocalShadowPendingHistory *pending,
                                    VkrLocalShadowPassPayload *out) {
  MemZero(out, sizeof(*out));
  *pending = (VkrLocalShadowPendingHistory){0};
  const VkrLocalShadowCamera *camera = input->camera;
  const uint32_t face_budget =
      Min(input->face_budget, VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX);
  out->face_budget = face_budget;
  out->map_size = input->map_size;
  if (!input->lights || !camera || !face_budget ||
      !vkr_local_shadow_map_size_valid(input->map_size) ||
      !isfinite(camera->position.x) || !isfinite(camera->position.y) ||
      !isfinite(camera->position.z)) {
    MemZero(cache, sizeof(*cache));
    return;
  }
  vkr_local_shadow_adopt_new_pool(cache, &input->token);

  /* A first resolve, a budget change or a camera cut has no image history to
   * keep, so lights whose faces are valid show at full strength at once;
   * otherwise they fade in. */
  const bool8_t snap =
      !cache->valid || cache->face_budget != face_budget ||
      !isfinite(camera->delta_seconds) || camera->delta_seconds < 0.0f ||
      vkr_temporal_is_camera_cut(cache->camera_position, cache->camera_view,
                                 camera->position, camera->view);
  const float32_t step =
      snap ? 1.0f
           : Min(camera->delta_seconds / VKR_LOCAL_SHADOW_FADE_SECONDS, 1.0f);

  /* Measured contribution ranks lights by what they light on screen, through
   * walls included. It lags the view by a few frames; until a sample from
   * after the last snap arrives, and wherever it is unavailable, distance
   * ranks instead: the light's range sphere's solid angle outside its range
   * and, inside it, inverse-square proximity. */
  const uint64_t feedback_after_frame =
      snap ? camera->frame_index : cache->feedback_after_frame;
  const VkrLocalLightContributionSample *feedback = input->feedback;
  const bool8_t use_feedback =
      feedback && feedback->valid &&
      feedback->source_frame_index >= feedback_after_frame &&
      feedback->source_frame_index <= camera->frame_index &&
      camera->frame_index - feedback->source_frame_index <=
          VKR_LOCAL_SHADOW_FEEDBACK_MAX_AGE;
  const float32_t incumbent_bonus =
      use_feedback ? VKR_LOCAL_SHADOW_FEEDBACK_INCUMBENT_BONUS
                   : VKR_LOCAL_SHADOW_INCUMBENT_BONUS;

  VkrLocalShadowCandidate candidates[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  uint32_t candidate_count = vkr_local_shadow_gather_candidates(
      input, use_feedback ? feedback : NULL, candidates);
  vkr_local_shadow_sort_candidates_by_render_id(candidates, candidate_count);
  bool8_t identity_valid = true_v;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    /* Identity drives reuse, so a zero or duplicated render id disables it. */
    if (candidate->render_id == 0u ||
        (i > 0u && candidates[i - 1u].render_id == candidate->render_id))
      identity_valid = false_v;
    candidate->previous = vkr_local_shadow_previous_light(cache, candidate);
  }
  if (!identity_valid) {
    for (uint32_t i = 0u; i < candidate_count; ++i)
      candidates[i].previous = 0u;
  }

  /* Face sizes follow the light's range, so camera motion never resizes a
   * face; the layer count follows the resident faces. */
  vkr_local_shadow_fit_atlas(candidates, candidate_count);
  const uint64_t static_cells =
      vkr_local_shadow_candidate_cells(candidates, candidate_count);
  /* While the scene has baked lamps the frame may shadow, their static
   * squares take a layer of their own after the static layers, so selecting
   * a lamp never repacks the others. */
  const bool8_t baked_layer =
      input->baked_lamps_possible && input->dynamic_casters;
  const uint32_t static_layer_count =
      Max((uint32_t)((static_cells + VKR_LOCAL_SHADOW_LAYER_CELLS - 1u) /
                     VKR_LOCAL_SHADOW_LAYER_CELLS),
          baked_layer ? 0u : 1u);
  /* While the world has dynamic casters, a band of trailing layers holds
   * the frame's dynamic squares. The band follows the casters' presence, not
   * their motion, so the atlas is not recreated as they move. Without room
   * for it every face draws every caster. */
  const uint32_t dynamic_layer_count =
      input->dynamic_casters && static_layer_count + (baked_layer ? 1u : 0u) +
                                        VKR_LOCAL_SHADOW_DYNAMIC_LAYER_COUNT <=
                                    VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX
          ? VKR_LOCAL_SHADOW_DYNAMIC_LAYER_COUNT
          : 0u;
  const uint32_t baked_layer_count =
      baked_layer && dynamic_layer_count != 0u ? 1u : 0u;
  const uint32_t layer_count =
      static_layer_count + baked_layer_count + dynamic_layer_count;
  /* Static squares hold the static casters only while the dynamic casters
     are known; a light a dynamic refractive caster reaches draws them all,
     since its transmission layers follow that caster. */
  const bool8_t split = dynamic_layer_count != 0u && !input->contents_unstable;

  /* A baked lamp shows its moving casters' shadow as the difference of its
     static and composite squares, which needs the split and its layer;
     without them it leaves the frame. */
  const bool8_t baked_shown =
      split && baked_layer_count != 0u &&
      vkr_local_shadow_place_baked(cache, candidates, candidate_count,
                                   static_layer_count);
  if (!baked_shown) {
    uint32_t kept = 0u;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      if (!candidates[i].baked)
        candidates[kept++] = candidates[i];
    }
    candidate_count = kept;
  }
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    candidate->all_casters =
        !candidate->baked &&
        (!split || (input->dynamic_refractive &&
                    input->dynamic_refractive[candidate->light_index]));
  }

  /* Lights that stopped casting fade their shadows out in the squares they
   * hold, which the casting lights take only when they need the room. */
  const uint32_t shown_count = candidate_count;
  if (identity_valid) {
    candidate_count = vkr_local_shadow_gather_leaving(
        cache, input, layer_count, candidates, candidate_count);
  }
  candidate_count = vkr_local_shadow_pack_atlas(cache, candidates, shown_count,
                                                candidate_count, layer_count,
                                                static_layer_count);
  out->atlas_layer_count = layer_count;
  out->dynamic_layer_count = dynamic_layer_count;
  out->baked_lamp_candidates = baked_shown ? input->baked_lamp_candidates : 0u;

  /* An atlas of another layer count is replaced this frame, so none of its
   * layers holds content. Layers without retained content clear whole. */
  const uint32_t layer_bits =
      (uint32_t)vkr_local_shadow_view_bits(0u, layer_count);
  const bool8_t pool_matches = input->token.resource_generation != 0u &&
                               input->token.atlas_layer_count == layer_count;
  const uint32_t valid_layers =
      pool_matches ? (uint32_t)input->token.valid_layer_mask & layer_bits : 0u;
  out->atlas_clear_mask = layer_bits & ~valid_layers;
  pending->cleared_layer_mask = out->atlas_clear_mask;

  /* Transmission layers below the first never-drawn one are readable. */
  uint32_t transmission_layer_count = 0u;
  while (transmission_layer_count < face_budget &&
         (input->token.transmission_valid_layer_mask &
          vkr_local_shadow_view_bits(transmission_layer_count, 1u)) != 0u)
    ++transmission_layer_count;
  if (!input->refractive_casters)
    transmission_layer_count = 0u;

  /* The most important lights, incumbents weighted, hold transmission layers
   * for every face; glass does not shadow the others. */
  float32_t keys[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  uint32_t order[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  uint64_t used_transmission = 0u;
  if (input->refractive_casters) {
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      const VkrLocalShadowCandidate *candidate = &candidates[i];
      const bool8_t incumbent =
          candidate->previous != 0u &&
          cache->lights[candidate->previous - 1u].transmission_layers[0] != 0u;
      keys[i] = incumbent
                    ? candidate->score *
                          Max(incumbent_bonus,
                              VKR_LOCAL_SHADOW_TRANSMISSION_INCUMBENT_BONUS)
                    : candidate->score;
    }
    vkr_local_shadow_order_by_key(keys, candidate_count, order);
    uint32_t layers = 0u;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      VkrLocalShadowCandidate *candidate = &candidates[order[i]];
      if (candidate->leaving || candidate->baked ||
          layers + candidate->face_count > face_budget)
        continue;
      candidate->wants_transmission = true_v;
      layers += candidate->face_count;
      if (candidate->previous == 0u)
        continue;
      const VkrLocalShadowCacheLight *previous =
          &cache->lights[candidate->previous - 1u];
      bool8_t readable = previous->transmission_layers[0] != 0u;
      for (uint32_t face = 0u; readable && face < candidate->face_count; ++face)
        readable =
            previous->transmission_layers[face] - 1u < transmission_layer_count;
      if (!readable)
        continue;
      MemCopy(candidate->transmission_layers, previous->transmission_layers,
              sizeof(candidate->transmission_layers));
      for (uint32_t face = 0u; face < candidate->face_count; ++face)
        used_transmission |= vkr_local_shadow_view_bits(
            candidate->transmission_layers[face] - 1u, 1u);
    }
  }

  vkr_local_shadow_classify_content(cache, input, candidates, candidate_count,
                                    valid_layers, transmission_layer_count);

  /* Faces a dynamic caster may reach and the camera may see take dynamic
   * squares, by light importance, up to the cap; the rest show their static
   * squares. Their render slots come first out of the slot capacity. */
  for (uint32_t i = 0u; i < candidate_count; ++i)
    keys[i] = candidates[i].score;
  vkr_local_shadow_order_by_key(keys, candidate_count, order);
  uint32_t dynamic_faces = 0u;
  if (split) {
    const bool8_t camera_projection =
        MemCompare(&camera->projection, &(Mat4){0}, sizeof(Mat4)) != 0;
    const VkrFrustum camera_frustum =
        camera_projection
            ? vkr_frustum_from_view_projection(camera->view, camera->projection)
            : (VkrFrustum){0};
    for (uint32_t i = 0u; i < candidate_count; ++i) {
      VkrLocalShadowCandidate *candidate = &candidates[order[i]];
      const uint32_t reached =
          vkr_local_shadow_reached_faces(input, candidate->light_index);
      if (candidate->leaving || candidate->all_casters || reached == 0u ||
          candidate->distance_fade <= 0.0f)
        continue;
      candidate->dynamic_wanted =
          camera_projection
              ? reached & vkr_local_shadow_visible_faces(
                              &input->lights[candidate->light_index],
                              candidate->face_count, candidate->half_fov,
                              &camera_frustum)
              : reached;
      for (uint32_t face = 0u; face < candidate->face_count; ++face) {
        if ((candidate->dynamic_wanted & (UINT32_C(1) << face)) == 0u)
          continue;
        out->dynamic_faces_wanted++;
        if (dynamic_faces == VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX)
          continue;
        candidate->dynamic_mask |= UINT32_C(1) << face;
        dynamic_faces++;
      }
    }
  }
  const uint32_t render_budget =
      Min(face_budget, VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX - dynamic_faces);

  /* Faces draw in complete lights within the face budget: invalid lights,
   * then lights waiting for transmission, then stale content, each by
   * importance, lights whose shadow has faded out by distance last. */
  uint32_t render_faces = 0u;
  for (uint32_t faded = 0u; faded < 2u; ++faded) {
    for (uint32_t render_class = VKR_LOCAL_SHADOW_RENDER_INVALID;
         render_class <= VKR_LOCAL_SHADOW_RENDER_STALE; ++render_class) {
      for (uint32_t i = 0u; i < candidate_count; ++i) {
        VkrLocalShadowCandidate *candidate = &candidates[order[i]];
        if (candidate->leaving || candidate->render_class != render_class ||
            (candidate->distance_fade <= 0.0f) != (faded != 0u) ||
            render_faces + candidate->face_count > render_budget)
          continue;
        candidate->render = true_v;
        render_faces += candidate->face_count;
      }
    }
  }

  /* A light takes transmission layers only in a frame that draws them, the
   * lowest free first, so every layer below the readable count has been
   * drawn. A light that lost its place releases them. */
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    if (!candidate->wants_transmission) {
      MemZero(candidate->transmission_layers,
              sizeof(candidate->transmission_layers));
      candidate->transmission_valid = false_v;
    }
  }
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    VkrLocalShadowCandidate *candidate = &candidates[i];
    if (!candidate->wants_transmission || !candidate->render ||
        candidate->transmission_layers[0] != 0u)
      continue;
    for (uint32_t face = 0u; face < candidate->face_count; ++face) {
      uint32_t layer = 0u;
      while ((used_transmission & vkr_local_shadow_view_bits(layer, 1u)) != 0u)
        ++layer;
      used_transmission |= vkr_local_shadow_view_bits(layer, 1u);
      candidate->transmission_layers[face] = layer + 1u;
      transmission_layer_count = Max(transmission_layer_count, layer + 1u);
    }
  }
  out->refractive_casters = input->refractive_casters;
  out->contact_shadows = input->contact_shadows;
  out->transmission_layer_count = transmission_layer_count;
  pending->transmission_layer_count = transmission_layer_count;

  /* Shadow strength rises once every face of a light is valid, and a
   * leaving light's falls while its faces stay valid. The most important
   * shown lights keep the full filter, a light that had it with the
   * incumbent bonus; a leaving light keeps its filter and takes no place. */
  float32_t strengths[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX] = {0};
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    const VkrLocalShadowCandidate *candidate = &candidates[i];
    const VkrLocalShadowCacheLight *previous =
        candidate->previous ? &cache->lights[candidate->previous - 1u] : NULL;
    if (candidate->leaving) {
      strengths[i] =
          candidate->opaque_valid ? Max(previous->strength - step, 0.0f) : 0.0f;
      keys[i] = -FLT_MAX;
      continue;
    }
    const bool8_t shown = candidate->opaque_valid || candidate->render;
    strengths[i] =
        shown ? Min((previous ? previous->strength : 0.0f) + step, 1.0f) : 0.0f;
    const bool8_t incumbent_full =
        previous && previous->strength * previous->distance_fade > 0.0f &&
        !previous->reduced;
    keys[i] = !shown || candidate->distance_fade <= 0.0f || candidate->baked
                  ? -FLT_MAX
              : incumbent_full
                  ? candidate->score * VKR_LOCAL_SHADOW_INCUMBENT_BONUS
                  : candidate->score;
  }
  vkr_local_shadow_order_by_key(keys, candidate_count, order);
  bool8_t reduced[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  const uint32_t full_filter_count =
      input->full_filter_all ? VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX
                             : VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT;
  for (uint32_t i = 0u; i < candidate_count; ++i)
    reduced[order[i]] = i >= full_filter_count;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    if (candidates[i].leaving) {
      reduced[i] = cache->lights[candidates[i].previous - 1u].reduced;
    }
    if (candidates[i].baked) {
      reduced[i] = false_v;
    }
  }

  vkr_local_shadow_place_dynamic(candidates, candidate_count, strengths,
                                 static_layer_count + baked_layer_count);
  vkr_local_shadow_publish(cache, scratch, input, candidates, candidate_count,
                           strengths, reduced, feedback_after_frame,
                           layer_count, identity_valid, pending, out);
  pending->active = true_v;
  MemCopy(cache, scratch, sizeof(*cache));
}

void vkr_local_shadow_cache_commit(VkrLocalShadowCache *cache,
                                   const VkrLocalShadowPendingHistory *pending,
                                   uint64_t submit_value) {
  if (!pending->active)
    return;
  /* A cleared layer loses the content of every face on it. */
  for (uint32_t i = 0u; pending->cleared_layer_mask && i < cache->light_count;
       ++i) {
    VkrLocalShadowCacheLight *light = &cache->lights[i];
    for (uint32_t face = 0u; face < light->face_count; ++face) {
      if ((pending->cleared_layer_mask &
           (UINT32_C(1) << (light->face_cells[face] >> 16u))) != 0u)
        light->faces[face] = (VkrLocalShadowFaceHistory){0};
    }
  }
  for (uint32_t i = 0u; i < pending->face_count; ++i) {
    const VkrLocalShadowPendingFace *face = &pending->faces[i];
    if (face->light >= cache->light_count)
      continue;
    VkrLocalShadowFaceHistory *history =
        &cache->lights[face->light].faces[face->face];
    *history = face->history;
    history->last_submit_value = submit_value;
  }
  cache->transmission_layer_count = pending->transmission_layer_count;
}
