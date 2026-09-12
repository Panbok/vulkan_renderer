#include "vkr_froxel_fog.h"
#include "vkr_frame_input.h"

#include <math.h>

VkrFroxelFogSettings vkr_froxel_fog_settings_defaults(void) {
  return (VkrFroxelFogSettings){.color = {1.0f, 1.0f, 1.0f},
                               .density = 0.01f,
                               .height_falloff = 0.1f,
                               .max_distance = 200.0f};
}

static bool8_t finite_vec3(Vec3 v) {
  return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

bool8_t vkr_froxel_fog_settings_valid(const VkrFroxelFogSettings *settings) {
  if (settings->enabled > true_v ||
      settings->box_count > VKR_FROXEL_FOG_BOX_COUNT_MAX)
    return false_v;
  if (!settings->enabled)
    return true_v;
  if (!finite_vec3(settings->color) || settings->color.x < 0.0f ||
      settings->color.y < 0.0f || settings->color.z < 0.0f ||
      settings->color.x > 1.0f || settings->color.y > 1.0f ||
      settings->color.z > 1.0f || !isfinite(settings->density) ||
      settings->density < 0.0f || !isfinite(settings->base_height) ||
      !isfinite(settings->height_falloff) || settings->height_falloff < 0.0f ||
      !isfinite(settings->max_distance) || settings->max_distance <= 0.0f)
    return false_v;
  for (uint32_t i = 0; i < settings->box_count; ++i) {
    const VkrFroxelDensityBox *box = &settings->boxes[i];
    if (!finite_vec3(box->minimum) || !finite_vec3(box->maximum) ||
        box->minimum.x >= box->maximum.x ||
        box->minimum.y >= box->maximum.y ||
        box->minimum.z >= box->maximum.z ||
        !isfinite(box->density_multiplier) || box->density_multiplier < 0.0f)
      return false_v;
  }
  return true_v;
}

bool8_t vkr_froxel_fog_projection_valid(const VkrFroxelFogSettings *settings,
                                       Mat4 projection) {
  if (!settings->enabled)
    return true_v;
  /* The volume extrapolates beyond the raster far plane; only its near
     boundary constrains the authored fog range. */
  const float32_t near_distance = projection.m23 / projection.m22;
  const float32_t ratio = settings->max_distance / near_distance;
  return isfinite(near_distance) && near_distance > 0.0f &&
         isfinite(ratio) && ratio > 1.0f;
}

static void select_local_lights(const VkrFrameInput *input,
                                VkrFroxelFogGpuParams *params) {
  params->selected_local_indices_count[0] = UINT32_MAX;
  params->selected_local_indices_count[1] = UINT32_MAX;
  if (!input->lighting || !input->local_shadow)
    return;

  /* Bound the entire fog frustum once, then rank upper-bound illumination at
     the closest point in that box. View motion changes ranking, not ownership. */
  Vec3 minimum = {INFINITY, INFINITY, INFINITY};
  Vec3 maximum = {-INFINITY, -INFINITY, -INFINITY};
  for (uint32_t corner = 0; corner < 4u; ++corner) {
    const Vec4 ndc = {(corner & 1u) ? 1.0f : -1.0f,
                      (corner & 2u) ? 1.0f : -1.0f, 0.0f, 1.0f};
    Vec4 a = mat4_mul_vec4(params->inverse_view_projection, ndc);
    Vec4 b = mat4_mul_vec4(params->inverse_view_projection,
                           (Vec4){ndc.x, ndc.y, 0.5f, 1.0f});
    a = vec4_scale(a, 1.0f / a.w);
    b = vec4_scale(b, 1.0f / b.w);
    const Vec4 av = mat4_mul_vec4(input->globals.view, a);
    const Vec4 bv = mat4_mul_vec4(input->globals.view, b);
    const float32_t t = (-params->height_distance_phase.z - av.z) /
                        (bv.z - av.z);
    const Vec3 points[2] = {{a.x, a.y, a.z},
                            {a.x + t * (b.x - a.x),
                             a.y + t * (b.y - a.y),
                             a.z + t * (b.z - a.z)}};
    for (uint32_t j = 0; j < 2u; ++j) {
      minimum.x = Min(minimum.x, points[j].x);
      minimum.y = Min(minimum.y, points[j].y);
      minimum.z = Min(minimum.z, points[j].z);
      maximum.x = Max(maximum.x, points[j].x);
      maximum.y = Max(maximum.y, points[j].y);
      maximum.z = Max(maximum.z, points[j].z);
    }
  }
  float64_t scores[2] = {-1.0, -1.0};
  uint32_t identities[2] = {UINT32_MAX, UINT32_MAX};
  for (uint32_t i = 0; i < input->lighting->point_light_count; ++i) {
    const VkrPointLight *light = &input->lighting->point_lights[i];
    const uint32_t first = input->local_shadow->light_first_view[i];
    const uint32_t faces = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT ? 1u : 6u;
    if (!first || first - 1u + faces > input->local_shadow->view_count)
      continue;
    const float64_t dx = Max(Max(minimum.x - light->position.x,
                                 light->position.x - maximum.x), 0.0f);
    const float64_t dy = Max(Max(minimum.y - light->position.y,
                                 light->position.y - maximum.y), 0.0f);
    const float64_t dz = Max(Max(minimum.z - light->position.z,
                                 light->position.z - maximum.z), 0.0f);
    const float64_t distance_squared = dx * dx + dy * dy + dz * dz;
    if (light->kind != VKR_POINT_LIGHT_KIND_POLYNOMIAL && light->range > 0.0f &&
        distance_squared >= (float64_t)light->range * light->range)
      continue;
    float64_t denominator = Max(distance_squared, 0.01);
    if (light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL)
      denominator = Max((float64_t)Max(light->constant, 1.0f) +
                            light->linear * sqrt(distance_squared) +
                            light->quadratic * distance_squared, 0.0001);
    const float64_t score = light->intensity *
        (0.2126 * light->color.x + 0.7152 * light->color.y +
         0.0722 * light->color.z) / denominator;
    if (score <= 0.0)
      continue;
    for (uint32_t slot = 0; slot < 2u; ++slot) {
      if (score > scores[slot] ||
          (score == scores[slot] && light->render_id < identities[slot])) {
        if (slot == 0u) {
          scores[1] = scores[0];
          identities[1] = identities[0];
          params->selected_local_indices_count[1] =
              params->selected_local_indices_count[0];
        }
        scores[slot] = score;
        identities[slot] = light->render_id;
        params->selected_local_indices_count[slot] = i;
        break;
      }
    }
  }
  params->selected_local_indices_count[2] =
      (scores[0] >= 0.0) + (scores[1] >= 0.0);
}

VkrFroxelFogGpuParams vkr_froxel_fog_prepare(const VkrFrameInput *input,
                                            uint32_t width, uint32_t height) {
  const VkrFroxelFogSettings *s = &input->globals.froxel_fog;
  if (!s->enabled || input->globals.render_mode != VKR_RENDER_MODE_DEFAULT)
    return (VkrFroxelFogGpuParams){0};
  const float32_t near_distance = input->globals.projection.m23 /
                                 input->globals.projection.m22;
  const float32_t log_range = logf(s->max_distance / near_distance);
  const Mat4 view_projection = mat4_mul(input->globals.projection,
                                       input->globals.view);
  VkrFroxelFogGpuParams params = {
      .inverse_view_projection = mat4_inverse(view_projection),
      .previous_view_projection = view_projection,
      .previous_view = input->globals.view,
      .current_view_projection = view_projection,
      .inverse_raster_view_projection = mat4_inverse(view_projection),
      .color_density = {s->color.x, s->color.y, s->color.z, s->density},
      .height_distance_phase = {s->base_height, s->height_falloff,
                                s->max_distance, 0.0f},
      .depth_mapping = {near_distance, log_range, 1.0f / log_range, 0.0f},
      .temporal_clamp = {0.9f, 0.25f, 4.0f, 1e-6f},
      .grid_dimensions_cell_pixels = {Max(width / VKR_FROXEL_FOG_CELL_PIXELS, 1u),
          Max(height / VKR_FROXEL_FOG_CELL_PIXELS, 1u), VKR_FROXEL_FOG_DEPTH,
          VKR_FROXEL_FOG_CELL_PIXELS},
  };
  params.selected_local_indices_count[3] = s->box_count;
  for (uint32_t i = 0; i < s->box_count; ++i) {
    const VkrFroxelDensityBox *b = &s->boxes[i];
    params.boxes[i] = (VkrFroxelDensityBoxGpu){
        .min_density = {b->minimum.x, b->minimum.y, b->minimum.z,
                         b->density_multiplier},
        .max_reserved = {b->maximum.x, b->maximum.y, b->maximum.z, 0.0f}};
  }
  select_local_lights(input, &params);
  return params;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, uint32_t count) {
  const uint8_t *bytes = data;
  for (uint32_t i = 0; i < count; ++i)
    hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
  return hash;
}

uint64_t vkr_froxel_fog_content_signature(const VkrFrameInput *input,
                                          const VkrFroxelFogGpuParams *params) {
  if (!params->grid_dimensions_cell_pixels[0])
    return 0u;
  uint64_t hash = UINT64_C(1469598103934665603);
#define HASH(value) hash = hash_bytes(hash, &(value), sizeof(value))
  HASH(input->frame.scene_generation);
  HASH(input->globals.projection);
  /* The packed tail contains no padding; exclude all camera/view matrices. */
  hash = hash_bytes(hash, &params->color_density,
                    offsetof(VkrFroxelFogGpuParams, current_view_projection) -
                        offsetof(VkrFroxelFogGpuParams, color_density));
  if (input->world) {
    HASH(input->world->static_generation);
    HASH(input->world->dynamic_generation);
    HASH(input->world->publication_generation);
    HASH(input->world->caster_bounds_generation);
  }
  if (input->lighting) {
    HASH(input->lighting->directional_enabled);
    HASH(input->lighting->directional_direction);
    HASH(input->lighting->directional_color);
    HASH(input->lighting->directional_intensity);
    for (uint32_t i = 0; i < params->selected_local_indices_count[2]; ++i) {
      const uint32_t index = params->selected_local_indices_count[i];
      const VkrPointLight *light = &input->lighting->point_lights[index];
      VkrGpuPointLightRow row;
      vkr_point_light_pack(light, &row);
      HASH(row);
      HASH(light->render_id);
      const uint32_t first = input->local_shadow->light_first_view[index];
      HASH(first);
      const uint32_t faces = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT ? 1u : 6u;
      hash = hash_bytes(hash, &input->local_shadow->views[first - 1u],
                        faces * sizeof(VkrLocalShadowView));
    }
  }
  if (input->shadow) {
    HASH(input->shadow->cascade_count);
    hash = hash_bytes(hash, input->shadow->cascades,
                      input->shadow->cascade_count * sizeof(VkrShadowCascadePacketData));
    HASH(input->shadow->receiver.receiver_bias_texels);
  }
#undef HASH
  return hash;
}
