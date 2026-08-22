#include "renderer/systems/vkr_shadow_system.h"

#include "core/logger.h"
#include "math/vkr_math.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"

// ============================================================================
// Cascade Helpers
// ============================================================================

vkr_internal void vkr_shadow_compute_cascade_splits(VkrShadowSystem *system,
                                                    float32_t near_clip,
                                                    float32_t far_clip,
                                                    float32_t lambda) {
  uint32_t count = system->config.cascade_count;
  if (count == 0) {
    system->cascade_splits[0] = near_clip;
    system->cascade_splits[1] = far_clip;
    return;
  }

  float32_t far_for_shadows = far_clip;
  if (system->config.max_shadow_distance > 0.0f) {
    far_for_shadows =
        vkr_min_f32(far_for_shadows, system->config.max_shadow_distance);
  }
  far_for_shadows = vkr_max_f32(far_for_shadows, near_clip + 0.001f);

  for (uint32_t i = 0; i <= count; ++i) {
    float32_t p = (float32_t)i / (float32_t)count;
    float32_t log_split = near_clip * powf(far_for_shadows / near_clip, p);
    float32_t linear_split = near_clip + (far_for_shadows - near_clip) * p;
    system->cascade_splits[i] =
        lambda * log_split + (1.0f - lambda) * linear_split;
  }
}

vkr_internal bool8_t vkr_shadow_has_any_per_cascade(const float32_t *values,
                                                    uint32_t count) {
  if (!values || count == 0) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (values[i] > 0.0f) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal float32_t vkr_shadow_profile_lerp(float32_t start, float32_t end,
                                               uint32_t cascade,
                                               uint32_t count) {
  if (count <= 1) {
    return start;
  }
  float32_t t = (float32_t)cascade / (float32_t)(count - 1);
  return start + (end - start) * t;
}

vkr_internal float32_t vkr_shadow_default_guard_band(float32_t base,
                                                     uint32_t cascade,
                                                     uint32_t count) {
  return base * vkr_shadow_profile_lerp(0.5f, 1.25f, cascade, count);
}

vkr_internal float32_t vkr_shadow_default_z_extension(float32_t base,
                                                      uint32_t cascade,
                                                      uint32_t count) {
  return base * vkr_shadow_profile_lerp(0.6f, 1.2f, cascade, count);
}

vkr_internal void vkr_shadow_compute_frustum_corners(const VkrCamera *camera,
                                                     float32_t near_split,
                                                     float32_t far_split,
                                                     Vec3 out_corners[8]) {
  if (!camera || !out_corners) {
    return;
  }

  Vec3 forward = camera->forward;
  if (vec3_length(forward) < 0.001f) {
    forward = vec3_new(0.0f, 0.0f, -1.0f);
  }
  forward = vec3_normalize(forward);

  Vec3 right = camera->right;
  if (vec3_length(right) < 0.001f) {
    right = vec3_new(1.0f, 0.0f, 0.0f);
  }
  right = vec3_normalize(right);

  Vec3 up = camera->up;
  if (vec3_length(up) < 0.001f) {
    up = vec3_new(0.0f, 1.0f, 0.0f);
  }
  up = vec3_normalize(up);

  float32_t near_d = vkr_max_f32(near_split, 0.0f);
  float32_t far_d = vkr_max_f32(far_split, near_d);

  float32_t near_half_w = 0.0f;
  float32_t near_half_h = 0.0f;
  float32_t far_half_w = 0.0f;
  float32_t far_half_h = 0.0f;

  if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    VkrWindowPixelSize window_size = {camera->cached_window_width,
                                      camera->cached_window_height};
    if ((window_size.width == 0 || window_size.height == 0) && camera->window) {
      window_size = vkr_window_get_pixel_size(camera->window);
    }
    float32_t aspect = 1.0f;
    if (window_size.width > 0 && window_size.height > 0) {
      aspect = (float32_t)window_size.width / (float32_t)window_size.height;
    }

    float32_t fov = vkr_to_radians(camera->zoom);
    float32_t tan_half_fov = vkr_tan_f32(fov * 0.5f);

    near_half_h = near_d * tan_half_fov;
    near_half_w = near_half_h * aspect;
    far_half_h = far_d * tan_half_fov;
    far_half_w = far_half_h * aspect;
  } else if (camera->type == VKR_CAMERA_TYPE_ORTHOGRAPHIC) {
    near_half_w = 0.5f * (camera->right_clip - camera->left_clip);
    near_half_h = 0.5f * (camera->top_clip - camera->bottom_clip);
    far_half_w = near_half_w;
    far_half_h = near_half_h;
  } else {
    return;
  }

  Vec3 near_center = vec3_add(camera->position, vec3_scale(forward, near_d));
  Vec3 far_center = vec3_add(camera->position, vec3_scale(forward, far_d));

  Vec3 near_right = vec3_scale(right, near_half_w);
  Vec3 near_up = vec3_scale(up, near_half_h);
  Vec3 far_right = vec3_scale(right, far_half_w);
  Vec3 far_up = vec3_scale(up, far_half_h);

  // Order: near TL/TR/BR/BL, far TL/TR/BR/BL.
  out_corners[0] = vec3_add(vec3_sub(near_center, near_right), near_up);
  out_corners[1] = vec3_add(vec3_add(near_center, near_right), near_up);
  out_corners[2] = vec3_sub(vec3_add(near_center, near_right), near_up);
  out_corners[3] = vec3_sub(vec3_sub(near_center, near_right), near_up);

  out_corners[4] = vec3_add(vec3_sub(far_center, far_right), far_up);
  out_corners[5] = vec3_add(vec3_add(far_center, far_right), far_up);
  out_corners[6] = vec3_sub(vec3_add(far_center, far_right), far_up);
  out_corners[7] = vec3_sub(vec3_sub(far_center, far_right), far_up);
}

vkr_internal Mat4 vkr_shadow_compute_light_view(
    const VkrCamera *camera, const VkrShadowSceneBounds *scene_bounds,
    Vec3 light_direction, float32_t max_shadow_distance,
    uint32_t shadow_map_size, float32_t snap_texels) {
  Vec3 dir = light_direction;
  if (vec3_length(dir) < 0.001f) {
    dir = vec3_new(0.0f, -1.0f, 0.0f);
  }
  dir = vec3_normalize(dir);

  Vec3 up_ref = (vkr_abs_f32(dir.y) > 0.99f) ? vec3_new(0.0f, 0.0f, 1.0f)
                                             : vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 right = vec3_normalize(vec3_cross(up_ref, dir));
  Vec3 up = vec3_cross(dir, right);

  Vec3 anchor = vec3_zero();
  float32_t radius = 1.0f;

  if (scene_bounds && scene_bounds->use_scene_bounds) {
    anchor = vec3_scale(vec3_add(scene_bounds->min, scene_bounds->max), 0.5f);
    Vec3 half_extent =
        vec3_scale(vec3_sub(scene_bounds->max, scene_bounds->min), 0.5f);
    radius = vec3_length(half_extent);
  } else if (camera) {
    anchor = camera->position;
    float32_t far_for_shadows = camera->far_clip;
    if (max_shadow_distance > 0.0f) {
      far_for_shadows = vkr_min_f32(far_for_shadows, max_shadow_distance);
    }
    far_for_shadows = vkr_max_f32(far_for_shadows, camera->near_clip + 0.001f);

    Vec3 corners[8];
    vkr_shadow_compute_frustum_corners(camera, camera->near_clip,
                                       far_for_shadows, corners);
    radius = 0.0f;
    for (int i = 0; i < 8; ++i) {
      Vec3 diff = vec3_sub(corners[i], anchor);
      radius = vkr_max_f32(radius, vec3_length(diff));
    }
  }

  if (radius < 0.001f) {
    radius = 0.001f;
  }

  if (shadow_map_size > 0 && snap_texels > 0.0f) {
    // Snap anchor in light space to reduce long-range drift.
    float32_t texel_size = (radius * 2.0f) / (float32_t)shadow_map_size;
    if (texel_size > 0.0f) {
      float32_t snap = vkr_max_f32(texel_size * snap_texels, 0.001f);
      float32_t anchor_x = vec3_dot(anchor, right);
      float32_t anchor_y = vec3_dot(anchor, up);
      float32_t anchor_z = vec3_dot(anchor, dir);
      anchor_x = vkr_floor_f32(anchor_x / snap) * snap;
      anchor_y = vkr_floor_f32(anchor_y / snap) * snap;
      anchor = vec3_add(
          vec3_add(vec3_scale(right, anchor_x), vec3_scale(up, anchor_y)),
          vec3_scale(dir, anchor_z));
    }
  }

  float32_t light_distance = vkr_max_f32(radius * 2.0f, 1.0f);
  Vec3 light_pos = vec3_sub(anchor, vec3_scale(dir, light_distance));
  return mat4_look_at(light_pos, anchor, up);
}

Vec2 vkr_shadow_light_space_origin_from_view(const Mat4 *light_view,
                                             float32_t left, float32_t bottom) {
  const Mat4 view = light_view ? *light_view : mat4_identity();
  return vec2_new(view.columns.col3.x - left, bottom - view.columns.col3.y);
}

vkr_internal void vkr_shadow_include_z(float32_t z, bool8_t *found,
                                       float32_t *min_z, float32_t *max_z) {
  *min_z = *found ? vkr_min_f32(*min_z, z) : z;
  *max_z = *found ? vkr_max_f32(*max_z, z) : z;
  *found = true_v;
}

bool8_t vkr_shadow_fit_relevant_caster_z(
    const Mat4 *light_view, const VkrShadowSceneBounds *scene_bounds,
    float32_t left, float32_t right, float32_t bottom, float32_t top,
    float32_t *out_min_z, float32_t *out_max_z) {
  if (!light_view || !scene_bounds || !out_min_z || !out_max_z ||
      scene_bounds->min.x > scene_bounds->max.x ||
      scene_bounds->min.y > scene_bounds->max.y ||
      scene_bounds->min.z > scene_bounds->max.z || left > right ||
      bottom > top) {
    return false_v;
  }

  Vec3 corners[8];
  bool8_t found = false_v;
  float32_t min_z = 0.0f;
  float32_t max_z = 0.0f;
  for (uint32_t i = 0; i < 8u; ++i) {
    Vec3 corner =
        vec3_new((i & 1u) ? scene_bounds->max.x : scene_bounds->min.x,
                 (i & 2u) ? scene_bounds->max.y : scene_bounds->min.y,
                 (i & 4u) ? scene_bounds->max.z : scene_bounds->min.z);
    Vec4 light_corner = mat4_mul_vec4(*light_view, vec3_to_vec4(corner, 1.0f));
    corners[i] = vec3_new(light_corner.x, light_corner.y, light_corner.z);
    if (light_corner.x >= left && light_corner.x <= right &&
        light_corner.y >= bottom && light_corner.y <= top) {
      vkr_shadow_include_z(light_corner.z, &found, &min_z, &max_z);
    }
  }

  static const uint8_t edges[12][2] = {
      {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };
  const float32_t x_planes[2] = {left, right};
  const float32_t y_planes[2] = {bottom, top};
  for (uint32_t edge_index = 0; edge_index < 12u; ++edge_index) {
    Vec3 a = corners[edges[edge_index][0]];
    Vec3 b = corners[edges[edge_index][1]];
    Vec3 delta = vec3_sub(b, a);
    if (vkr_abs_f32(delta.x) > 1e-6f) {
      for (uint32_t plane_index = 0; plane_index < 2u; ++plane_index) {
        float32_t t = (x_planes[plane_index] - a.x) / delta.x;
        float32_t y = a.y + delta.y * t;
        if (t >= 0.0f && t <= 1.0f && y >= bottom && y <= top) {
          vkr_shadow_include_z(a.z + delta.z * t, &found, &min_z, &max_z);
        }
      }
    }
    if (vkr_abs_f32(delta.y) > 1e-6f) {
      for (uint32_t plane_index = 0; plane_index < 2u; ++plane_index) {
        float32_t t = (y_planes[plane_index] - a.y) / delta.y;
        float32_t x = a.x + delta.x * t;
        if (t >= 0.0f && t <= 1.0f && x >= left && x <= right) {
          vkr_shadow_include_z(a.z + delta.z * t, &found, &min_z, &max_z);
        }
      }
    }
  }

  // The remaining possible extrema lie where two cascade clip planes meet.
  // Intersect the four light-space Z lines through the rectangle corners with
  // the world-space AABB using a slab test.
  Mat4 inverse_view = mat4_inverse_rigid(*light_view);
  for (uint32_t corner_index = 0; corner_index < 4u; ++corner_index) {
    Vec4 origin4 = mat4_mul_vec4(
        inverse_view, vec4_new((corner_index & 1u) ? right : left,
                               (corner_index & 2u) ? top : bottom, 0.0f, 1.0f));
    Vec4 direction4 = mat4_mul_vec4(inverse_view, vec4_new(0, 0, 1, 0));
    float32_t t_min = -VKR_FLOAT_MAX;
    float32_t t_max = VKR_FLOAT_MAX;
    const float32_t origin[3] = {origin4.x, origin4.y, origin4.z};
    const float32_t direction[3] = {direction4.x, direction4.y, direction4.z};
    const float32_t bounds_min[3] = {scene_bounds->min.x, scene_bounds->min.y,
                                     scene_bounds->min.z};
    const float32_t bounds_max[3] = {scene_bounds->max.x, scene_bounds->max.y,
                                     scene_bounds->max.z};
    bool8_t intersects = true_v;
    for (uint32_t axis = 0; axis < 3u; ++axis) {
      if (vkr_abs_f32(direction[axis]) <= 1e-6f) {
        if (origin[axis] < bounds_min[axis] ||
            origin[axis] > bounds_max[axis]) {
          intersects = false_v;
          break;
        }
        continue;
      }
      float32_t t0 = (bounds_min[axis] - origin[axis]) / direction[axis];
      float32_t t1 = (bounds_max[axis] - origin[axis]) / direction[axis];
      if (t0 > t1) {
        float32_t swap = t0;
        t0 = t1;
        t1 = swap;
      }
      t_min = vkr_max_f32(t_min, t0);
      t_max = vkr_min_f32(t_max, t1);
      if (t_min > t_max) {
        intersects = false_v;
        break;
      }
    }
    if (intersects) {
      vkr_shadow_include_z(t_min, &found, &min_z, &max_z);
      vkr_shadow_include_z(t_max, &found, &min_z, &max_z);
    }
  }

  if (!found) {
    return false_v;
  }
  *out_min_z = min_z;
  *out_max_z = max_z;
  return true_v;
}

float32_t vkr_shadow_quantize_extent_up(float32_t extent,
                                        uint32_t shadow_map_size) {
  // The quantum has to be independent of the extent, or quantization is a
  // no-op: a quantum derived from the extent itself always divides it exactly.
  // Bracketing the extent up to the next power of two gives a step that only
  // changes when the extent crosses a binade, so the derived texel size takes
  // discrete values across ordinary camera motion instead of sliding with it.
  int exponent = 0;
  (void)frexpf(extent, &exponent);
  const float32_t bracket = ldexpf(1.0f, exponent);
  const float32_t quantum = bracket / (float32_t)shadow_map_size;
  return vkr_ceil_f32(extent / quantum) * quantum;
}

VkrShadowFit vkr_shadow_apply_fit_hysteresis(const VkrShadowFit *previous,
                                             const VkrShadowFit *raw) {
  VkrShadowFit fit = *raw;

  // Growth is taken immediately; only contraction is deadbanded. Extent and Z
  // therefore never become smaller than the raw fit.
  const float32_t raw_texel = raw->world_units_per_texel;
  if (raw->extent < previous->extent &&
      (previous->extent - raw->extent) < 2.0f * raw_texel) {
    fit.extent = previous->extent;
    fit.world_units_per_texel = previous->world_units_per_texel;
  }

  // Compared against the final texel size because keeping the previous extent
  // also keeps its grid. The fit reserved one guard texel for this shift.
  const float32_t texel = fit.world_units_per_texel;
  if (vkr_abs_f32(raw->center_x - previous->center_x) <= texel) {
    fit.center_x = previous->center_x;
  }
  if (vkr_abs_f32(raw->center_y - previous->center_y) <= texel) {
    fit.center_y = previous->center_y;
  }

  // Each depth bound deadbands independently: one end of the interval can be
  // stable while the other legitimately moves.
  if (raw->min_z > previous->min_z &&
      (raw->min_z - previous->min_z) < 2.0f * texel) {
    fit.min_z = previous->min_z;
  }
  if (raw->max_z < previous->max_z &&
      (previous->max_z - raw->max_z) < 2.0f * texel) {
    fit.max_z = previous->max_z;
  }
  return fit;
}

vkr_internal bool8_t vkr_shadow_fit_history_matches(
    const VkrShadowFitHistory *history, Vec3 light_direction,
    uint32_t cascade_count, uint32_t shadow_map_size,
    uint32_t projection_convention, uint64_t enable_generation) {
  // Any mismatch means the stored fit was framed by different rules, so it is
  // not a previous value of the same quantity and must not be blended with.
  return history->valid && history->cascade_count == cascade_count &&
         history->shadow_map_size == shadow_map_size &&
         history->projection_convention == projection_convention &&
         history->enable_generation == enable_generation &&
         history->light_direction.x == light_direction.x &&
         history->light_direction.y == light_direction.y &&
         history->light_direction.z == light_direction.z;
}

void vkr_shadow_system_invalidate_fit_history(VkrShadowSystem *system) {
  if (system) {
    system->fit_history.valid = false_v;
  }
}

vkr_internal void vkr_shadow_compute_cascade_matrix(
    const Mat4 *light_view, const Vec3 frustum_corners[8],
    uint32_t shadow_map_size, bool8_t stabilize, float32_t guard_band_texels,
    bool8_t use_constant_cascade_size, const VkrShadowSceneBounds *scene_bounds,
    float32_t z_extension_factor, const VkrShadowFit *previous_fit,
    Mat4 *out_view_projection, VkrShadowFit *out_fit,
    Vec2 *out_light_space_origin) {
  const Mat4 view = *light_view;

  // Compute the slice center and its bounding sphere radius.
  Vec3 center = vec3_zero();
  for (int i = 0; i < 8; ++i) {
    center = vec3_add(center, frustum_corners[i]);
  }
  center = vec3_scale(center, 1.0f / 8.0f);

  float32_t radius_sq = 0.0f;
  for (int i = 0; i < 8; ++i) {
    Vec3 diff = vec3_sub(frustum_corners[i], center);
    radius_sq = vkr_max_f32(radius_sq, vec3_length_squared(diff));
  }
  float32_t radius = vkr_sqrt_f32(radius_sq);
  if (radius < 0.001f) {
    radius = 0.001f;
  }

  if (stabilize) {
    radius = vkr_ceil_f32(radius * 16.0f) / 16.0f;
  }

  float32_t min_x = 0.0f;
  float32_t max_x = 0.0f;
  float32_t min_y = 0.0f;
  float32_t max_y = 0.0f;
  float32_t min_z = 0.0f;
  float32_t max_z = 0.0f;

  min_x = VKR_FLOAT_MAX;
  max_x = -VKR_FLOAT_MAX;
  min_y = VKR_FLOAT_MAX;
  max_y = -VKR_FLOAT_MAX;
  min_z = VKR_FLOAT_MAX;
  max_z = -VKR_FLOAT_MAX;

  // Compute bounds from frustum corners in light space.
  for (int i = 0; i < 8; ++i) {
    Vec4 corner_ls =
        mat4_mul_vec4(view, vec3_to_vec4(frustum_corners[i], 1.0f));
    min_x = vkr_min_f32(min_x, corner_ls.x);
    max_x = vkr_max_f32(max_x, corner_ls.x);
    min_y = vkr_min_f32(min_y, corner_ls.y);
    max_y = vkr_max_f32(max_y, corner_ls.y);
    min_z = vkr_min_f32(min_z, corner_ls.z);
    max_z = vkr_max_f32(max_z, corner_ls.z);
  }

  if ((!scene_bounds || !scene_bounds->use_scene_bounds) &&
      z_extension_factor > 0.0f) {
    float32_t z_ext = radius * z_extension_factor;
    min_z -= z_ext;
    max_z += z_ext;
  }

  float32_t extent_x = max_x - min_x;
  float32_t extent_y = max_y - min_y;
  float32_t center_x = (min_x + max_x) * 0.5f;
  float32_t center_y = (min_y + max_y) * 0.5f;
  float32_t extent = vkr_max_f32(extent_x, extent_y);

  if (use_constant_cascade_size) {
    Vec4 center_ls = mat4_mul_vec4(view, vec3_to_vec4(center, 1.0f));
    center_x = center_ls.x;
    center_y = center_ls.y;
    extent = radius * 2.0f;
  }
  if (extent < 0.001f) {
    extent = 0.001f;
  }

  float32_t texel_size =
      (shadow_map_size > 0) ? (extent / (float32_t)shadow_map_size) : 0.0f;
  if (texel_size < 0.000001f) {
    texel_size = 0.000001f;
  }

  float32_t guard_texels = vkr_max_f32(guard_band_texels, 0.0f);
  if (stabilize && shadow_map_size > 0) {
    // Snapping can shift the projection by up to ~0.5 texel; include a small
    // extra margin so the receiver frustum stays covered after snapping.
    guard_texels += 1.0f;
  }

  // Expand the fitted AABB by a guard band (expressed in texels) so that nearby
  // casters just outside the camera frustum can still contribute. This reduces
  // shadow pop-in when rotating the camera, at the cost of some resolution.
  extent += 2.0f * texel_size * guard_texels;
  if (stabilize) {
    extent = vkr_shadow_quantize_extent_up(extent, shadow_map_size);
  }
  texel_size = (shadow_map_size > 0) ? (extent / (float32_t)shadow_map_size)
                                     : texel_size;
  if (texel_size < 0.000001f) {
    texel_size = 0.000001f;
  }

  float32_t half = extent * 0.5f;

  if (stabilize && shadow_map_size > 0) {
    // Stabilize by snapping the ortho bounds to the texel grid in light space.
    float32_t snap_x = center_x - half;
    float32_t snap_y = center_y - half;
    snap_x = vkr_floor_f32(snap_x / texel_size) * texel_size;
    snap_y = vkr_floor_f32(snap_y / texel_size) * texel_size;
    center_x = snap_x + half;
    center_y = snap_y + half;
  }

  // The caster-Z fit needs the XY rectangle, so it runs before hysteresis. The
  // deadbands then apply to the complete raw interval rather than to a partial
  // one that a later widening would invalidate.
  if (scene_bounds && scene_bounds->use_scene_bounds) {
    float32_t caster_min_z = 0.0f;
    float32_t caster_max_z = 0.0f;
    if (vkr_shadow_fit_relevant_caster_z(
            &view, scene_bounds, center_x - half, center_x + half,
            center_y - half, center_y + half, &caster_min_z, &caster_max_z)) {
      min_z = vkr_min_f32(min_z, caster_min_z);
      max_z = vkr_max_f32(max_z, caster_max_z);
    }
  }

  float32_t z_range = max_z - min_z;
  float32_t z_pad = vkr_max_f32(0.5f, z_range * 0.05f);
  min_z -= z_pad;
  max_z += z_pad;

  VkrShadowFit fit = {
      .center_x = center_x,
      .center_y = center_y,
      .extent = extent,
      .min_z = min_z,
      .max_z = max_z,
      .world_units_per_texel = texel_size,
  };
  if (stabilize && previous_fit) {
    fit = vkr_shadow_apply_fit_hysteresis(previous_fit, &fit);
  }

  const float32_t final_half = fit.extent * 0.5f;
  const float32_t left = fit.center_x - final_half;
  const float32_t right = fit.center_x + final_half;
  const float32_t bottom = fit.center_y - final_half;
  const float32_t top = fit.center_y + final_half;

  *out_fit = fit;
  *out_light_space_origin =
      vkr_shadow_light_space_origin_from_view(&view, left, bottom);

  float32_t near_clip = -fit.max_z;
  float32_t far_clip = -fit.min_z;
  if (near_clip < 0.0f) {
    near_clip = 0.0f;
  }
  if (far_clip <= near_clip + 0.001f) {
    far_clip = near_clip + 0.001f;
  }

  Mat4 light_projection =
      mat4_ortho_zo_yinv(left, right, bottom, top, near_clip, far_clip);
  *out_view_projection = mat4_mul(light_projection, view);
}

bool8_t vkr_shadow_system_init(VkrShadowSystem *system, RendererFrontend *rf,
                               const VkrShadowConfig *config) {
  if (!system || !rf) {
    return false_v;
  }

  MemZero(system, sizeof(*system));
  system->config = config ? *config : VKR_SHADOW_CONFIG_DEFAULT;

  if (system->config.cascade_count == 0) {
    system->config.cascade_count = 1;
  }
  if (system->config.cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
    system->config.cascade_count = VKR_SHADOW_CASCADE_COUNT_MAX;
  }
  if (system->config.shadow_map_size == 0) {
    system->config.shadow_map_size = VKR_SHADOW_MAP_SIZE_DEFAULT;
  }
  system->config.cascade_split_lambda =
      vkr_clamp_f32(system->config.cascade_split_lambda, 0.0f, 1.0f);
  if (system->config.cascade_guard_band_texels < 0.0f) {
    system->config.cascade_guard_band_texels = 0.0f;
  }
  if (system->config.z_extension_factor < 0.0f) {
    system->config.z_extension_factor = 0.0f;
  }
  if (!(system->config.anchor_snap_texels >= 0.0f)) {
    system->config.anchor_snap_texels = 0.0f;
  }
  // Raster depth bias reaches both native encoders. Normalize it once here.
  if (!(system->config.depth_bias_constant_factor >= 0.0f)) {
    system->config.depth_bias_constant_factor = 0.0f;
  }
  if (!(system->config.depth_bias_slope_factor >= 0.0f)) {
    system->config.depth_bias_slope_factor = 0.0f;
  }
  if (!(system->config.depth_bias_clamp >= 0.0f)) {
    system->config.depth_bias_clamp = 0.0f;
  }
  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    if (!(system->config.cascade_guard_band_texels_per[i] >= 0.0f)) {
      system->config.cascade_guard_band_texels_per[i] = 0.0f;
    }
    if (!(system->config.cascade_z_extension_factor_per[i] >= 0.0f)) {
      system->config.cascade_z_extension_factor_per[i] = 0.0f;
    }
  }

  // Generation 1, not 0: a zeroed history compares equal to generation 0 and
  // would be treated as valid before any fit exists.
  system->enable_generation = 1u;
  system->fit_history.valid = false_v;
  system->initialized = true_v;
  return true_v;
}

void vkr_shadow_system_shutdown(VkrShadowSystem *system, RendererFrontend *rf) {
  (void)rf;
  if (system) {
    MemZero(system, sizeof(*system));
  }
}

void vkr_shadow_system_update(VkrShadowSystem *system, const VkrCamera *camera,
                              bool8_t light_enabled, Vec3 light_direction) {
  if (!system || !system->initialized || !camera) {
    return;
  }

  const bool8_t was_enabled = system->light_enabled;
  system->light_enabled = light_enabled;
  system->light_direction = light_direction;

  if (!light_enabled) {
    // The camera may travel arbitrarily far while shadows are off, so the fit
    // stored before the gap is not a previous value of the same quantity.
    // Bumping here rather than on re-enable keeps the invalidation on the edge
    // that actually creates the discontinuity.
    if (was_enabled) {
      system->enable_generation++;
    }
    for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
      system->cascades[i].view_projection = mat4_identity();
      system->cascades[i].split_far = 0.0f;
      system->cascades[i].world_units_per_texel = 0.0f;
      system->cascades[i].light_space_origin = vec2_zero();
      system->cascades[i].light_space_depth_span = 0.0f;
      system->cascades[i].bounds_center = vec3_zero();
      system->cascades[i].bounds_radius = 0.0f;
    }
    return;
  }

  vkr_shadow_compute_cascade_splits(system, camera->near_clip, camera->far_clip,
                                    system->config.cascade_split_lambda);

  uint32_t shadow_map_size =
      vkr_shadow_config_get_max_map_size(&system->config);
  Mat4 light_view = vkr_shadow_compute_light_view(
      camera, &system->config.scene_bounds, light_direction,
      system->config.max_shadow_distance, shadow_map_size,
      system->config.anchor_snap_texels);

  uint32_t cascade_count = system->config.cascade_count;
  bool8_t has_guard_per = vkr_shadow_has_any_per_cascade(
      system->config.cascade_guard_band_texels_per, cascade_count);
  bool8_t has_z_per = vkr_shadow_has_any_per_cascade(
      system->config.cascade_z_extension_factor_per, cascade_count);

  // Projection convention is fixed for this build (reverse-Z off, zero-to-one
  // depth with an inverted Y). It is stamped anyway so that changing it later
  // invalidates stored fits instead of silently mixing conventions.
  const uint32_t projection_convention = 0u;
  VkrShadowFitHistory *history = &system->fit_history;
  const bool8_t history_usable =
      system->config.stabilize_cascades &&
      vkr_shadow_fit_history_matches(history, light_direction, cascade_count,
                                     shadow_map_size, projection_convention,
                                     system->enable_generation);

  for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
    float32_t split_near = system->cascade_splits[i];
    float32_t split_far = system->cascade_splits[i + 1];

    Vec3 corners[8];
    vkr_shadow_compute_frustum_corners(camera, split_near, split_far, corners);

    float32_t guard_band = system->config.cascade_guard_band_texels;
    if (has_guard_per) {
      float32_t per = system->config.cascade_guard_band_texels_per[i];
      if (per > 0.0f) {
        guard_band = per;
      }
    } else {
      guard_band = vkr_shadow_default_guard_band(guard_band, i, cascade_count);
    }

    float32_t z_extension = system->config.z_extension_factor;
    if (has_z_per) {
      float32_t per = system->config.cascade_z_extension_factor_per[i];
      if (per > 0.0f) {
        z_extension = per;
      }
    } else {
      z_extension =
          vkr_shadow_default_z_extension(z_extension, i, cascade_count);
    }

    VkrShadowFit fit = {0};
    vkr_shadow_compute_cascade_matrix(
        &light_view, corners, shadow_map_size,
        system->config.stabilize_cascades, guard_band,
        system->config.use_constant_cascade_size, &system->config.scene_bounds,
        z_extension, history_usable ? &history->cascades[i] : NULL,
        &system->cascades[i].view_projection, &fit,
        &system->cascades[i].light_space_origin);
    history->cascades[i] = fit;
    system->cascades[i].world_units_per_texel = fit.world_units_per_texel;
    system->cascades[i].light_space_depth_span = fit.max_z - fit.min_z;

    Vec3 cascade_center = vec3_zero();
    for (int c = 0; c < 8; ++c) {
      cascade_center = vec3_add(cascade_center, corners[c]);
    }
    cascade_center = vec3_scale(cascade_center, 1.0f / 8.0f);

    float32_t max_radius_sq = 0.0f;
    for (int c = 0; c < 8; ++c) {
      Vec3 diff = vec3_sub(corners[c], cascade_center);
      float32_t dist_sq = vec3_length_squared(diff);
      if (dist_sq > max_radius_sq) {
        max_radius_sq = dist_sq;
      }
    }

    system->cascades[i].bounds_center = cascade_center;
    system->cascades[i].bounds_radius = vkr_sqrt_f32(max_radius_sq);
    system->cascades[i].split_far = split_far;
  }

  history->light_direction = light_direction;
  history->cascade_count = cascade_count;
  history->shadow_map_size = shadow_map_size;
  history->projection_convention = projection_convention;
  history->enable_generation = system->enable_generation;
  history->valid = system->config.stabilize_cascades;
}

void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data) {
  /* Per-target-image shadow state does not exist yet: one raw fit set is
     produced per frame and every image reads the same one. The parameter is
     retained because the retained-history phase makes fits per image, and
     removing it now would only have to be added back at that seam. */
  (void)frame_index;
  if (!out_data) {
    return;
  }

  MemZero(out_data, sizeof(*out_data));

  if (!system || !system->initialized) {
    return;
  }

  out_data->enabled = system->light_enabled;
  if (!out_data->enabled) {
    return;
  }
  out_data->cascade_count = system->config.cascade_count;

  for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
    out_data->split_far[i] = system->cascades[i].split_far;
    out_data->view_projection[i] = system->cascades[i].view_projection;
  }
}
