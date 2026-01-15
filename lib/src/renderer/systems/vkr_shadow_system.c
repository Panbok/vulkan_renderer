#include "renderer/systems/vkr_shadow_system.h"

#include "core/logger.h"
#include "math/vkr_math.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"

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
    VkrWindowPixelSize window_size = vkr_window_get_pixel_size(camera->window);
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

vkr_internal void vkr_shadow_compute_cascade_matrix(
    const Vec3 *light_direction, const Vec3 frustum_corners[8],
    uint32_t shadow_map_size, bool8_t stabilize, float32_t guard_band_texels,
    const VkrShadowSceneBounds *scene_bounds, Mat4 *out_view_projection,
    float32_t *out_world_units_per_texel) {
  Vec3 dir = *light_direction;
  if (vec3_length(dir) < 0.001f) {
    dir = vec3_new(0.0f, -1.0f, 0.0f);
  }
  dir = vec3_normalize(dir);

  // Compute the frustum center and radius for XY bounds
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

  Vec3 up = (vkr_abs_f32(dir.y) > 0.99f) ? vec3_new(0.0f, 0.0f, 1.0f)
                                         : vec3_new(0.0f, 1.0f, 0.0f);

  // Build light view matrix - position it far enough to see the scene
  float32_t light_distance = radius * 2.0f;
  if (scene_bounds && scene_bounds->use_scene_bounds) {
    // Include scene bounds in distance calculation
    Vec3 scene_center = vec3_scale(
        vec3_add(scene_bounds->min, scene_bounds->max), 0.5f);
    Vec3 scene_half_extent = vec3_scale(
        vec3_sub(scene_bounds->max, scene_bounds->min), 0.5f);
    float32_t scene_radius = vec3_length(scene_half_extent);
    light_distance = vkr_max_f32(light_distance, scene_radius * 2.0f);
  }

  Mat4 light_view = mat4_identity();
  float32_t min_x = 0.0f;
  float32_t max_x = 0.0f;
  float32_t min_y = 0.0f;
  float32_t max_y = 0.0f;
  float32_t min_z = 0.0f;
  float32_t max_z = 0.0f;

  for (uint32_t attempt = 0; attempt < 2; ++attempt) {
    Vec3 light_pos = vec3_sub(center, vec3_scale(dir, light_distance));
    light_view = mat4_look_at(light_pos, center, up);

    min_x = VKR_FLOAT_MAX;
    max_x = -VKR_FLOAT_MAX;
    min_y = VKR_FLOAT_MAX;
    max_y = -VKR_FLOAT_MAX;
    min_z = VKR_FLOAT_MAX;
    max_z = -VKR_FLOAT_MAX;

    // Compute XY bounds from frustum corners only (for resolution optimization)
    for (int i = 0; i < 8; ++i) {
      Vec4 corner_ls =
          mat4_mul_vec4(light_view, vec3_to_vec4(frustum_corners[i], 1.0f));
      min_x = vkr_min_f32(min_x, corner_ls.x);
      max_x = vkr_max_f32(max_x, corner_ls.x);
      min_y = vkr_min_f32(min_y, corner_ls.y);
      max_y = vkr_max_f32(max_y, corner_ls.y);
      min_z = vkr_min_f32(min_z, corner_ls.z);
      max_z = vkr_max_f32(max_z, corner_ls.z);
    }

    if (max_z <= 0.0f) {
      break;
    }

    // Move the light further back along its direction.
    light_distance += max_z + 0.5f;
  }

  // Extend Z bounds using scene AABB (like reference implementation)
  // This ensures all shadow casters in the scene are included in the depth
  // range, regardless of camera position - eliminating shadow pop-in.
  if (scene_bounds && scene_bounds->use_scene_bounds) {
    // Transform all 8 corners of the scene AABB to light space and extend Z
    for (int i = 0; i < 8; ++i) {
      Vec3 corner = vec3_new(
          (i & 1) ? scene_bounds->max.x : scene_bounds->min.x,
          (i & 2) ? scene_bounds->max.y : scene_bounds->min.y,
          (i & 4) ? scene_bounds->max.z : scene_bounds->min.z);
      Vec4 corner_ls = mat4_mul_vec4(light_view, vec3_to_vec4(corner, 1.0f));
      // Only extend Z, not XY (XY is fitted to frustum for resolution)
      min_z = vkr_min_f32(min_z, corner_ls.z);
      max_z = vkr_max_f32(max_z, corner_ls.z);
    }
  }

  float32_t z_range = max_z - min_z;
  // Small padding for depth precision
  float32_t z_pad = vkr_max_f32(0.5f, z_range * 0.05f);
  min_z -= z_pad;
  max_z += z_pad;

  float32_t extent_x = max_x - min_x;
  float32_t extent_y = max_y - min_y;
  float32_t center_x = (min_x + max_x) * 0.5f;
  float32_t center_y = (min_y + max_y) * 0.5f;
  float32_t extent = vkr_max_f32(extent_x, extent_y);
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
  texel_size = (shadow_map_size > 0) ? (extent / (float32_t)shadow_map_size)
                                     : texel_size;
  if (texel_size < 0.000001f) {
    texel_size = 0.000001f;
  }

  float32_t half = extent * 0.5f;

  if (stabilize && shadow_map_size > 0) {
    // Stabilize by snapping the cascade projection center to the texel grid
    // (light space) to reduce shimmering as the camera moves.
    center_x = floorf(center_x / texel_size + 0.5f) * texel_size;
    center_y = floorf(center_y / texel_size + 0.5f) * texel_size;
  }

  float32_t left = center_x - half;
  float32_t right = center_x + half;
  float32_t bottom = center_y - half;
  float32_t top = center_y + half;
  *out_world_units_per_texel = texel_size;

  float32_t near_clip = -max_z;
  float32_t far_clip = -min_z;
  if (near_clip < 0.0f) {
    near_clip = 0.0f;
  }
  if (far_clip <= near_clip + 0.001f) {
    far_clip = near_clip + 0.001f;
  }

  Mat4 light_projection =
      mat4_ortho_vulkan(left, right, bottom, top, near_clip, far_clip);
  *out_view_projection = mat4_mul(light_projection, light_view);
}

// ============================================================================
// Resource Creation
// ============================================================================

vkr_internal bool8_t vkr_shadow_create_renderpass(VkrShadowSystem *system,
                                                  RendererFrontend *rf) {
  VkrRenderPassHandle pass =
      vkr_renderer_renderpass_get(rf, string8_lit("Renderpass.CSM.Shadow"));
  if (!pass) {
    const float32_t shadow_map_size = system->config.shadow_map_size;
    VkrRenderPassConfig cfg = {
        .name = string8_lit("Renderpass.CSM.Shadow"),
        .clear_flags = VKR_RENDERPASS_CLEAR_DEPTH,
        .domain = VKR_PIPELINE_DOMAIN_SHADOW,
        .render_area = vec4_new(0.0f, 0.0f, shadow_map_size, shadow_map_size),
    };
    pass = vkr_renderer_renderpass_create(rf, &cfg);
    if (!pass) {
      log_error("Failed to create shadow render pass");
      return false_v;
    }
    system->owns_renderpass = true_v;
  }

  system->shadow_renderpass = pass;
  return true_v;
}

vkr_internal bool8_t vkr_shadow_create_shadow_maps(VkrShadowSystem *system,
                                                   RendererFrontend *rf) {
  uint32_t size = system->config.shadow_map_size;
  uint32_t cascades = system->config.cascade_count;
  uint32_t frames = vkr_renderer_window_attachment_count(rf);
  if (frames == 0 || cascades == 0) {
    return false_v;
  }

  system->frame_resource_count = frames;
  system->frames = (VkrShadowFrameResources *)vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrShadowFrameResources) * (uint64_t)frames,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!system->frames) {
    return false_v;
  }
  MemZero(system->frames, sizeof(VkrShadowFrameResources) * (uint64_t)frames);

  for (uint32_t f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < cascades; ++c) {
      VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
      system->frames[f].shadow_maps[c] =
          vkr_renderer_create_sampled_depth_attachment(rf, size, size,
                                                       &tex_err);
      if (!system->frames[f].shadow_maps[c]) {
        String8 err = vkr_renderer_get_error_string(tex_err);
        log_error("Failed to create shadow depth texture: %s",
                  string8_cstr(&err));
        return false_v;
      }

      VkrTextureOpaqueHandle attachments[1] = {
          system->frames[f].shadow_maps[c]};
      VkrRenderTargetDesc rt_desc = {
          .sync_to_window_size = false_v,
          .attachment_count = 1,
          .attachments = attachments,
          .width = size,
          .height = size,
      };

      system->frames[f].shadow_targets[c] = vkr_renderer_render_target_create(
          rf, &rt_desc, system->shadow_renderpass);
      if (!system->frames[f].shadow_targets[c]) {
        log_error(
            "Failed to create shadow render target (frame %u, cascade %u)", f,
            c);
        return false_v;
      }
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_shadow_create_pipeline(VkrShadowSystem *system,
                                                RendererFrontend *rf) {
  VkrResourceHandleInfo cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/shadow.shadercfg"),
          &rf->scratch_allocator, &cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Shadow shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  if (!cfg_info.as.custom) {
    log_error("Shadow shadercfg returned null custom data");
    return false_v;
  }

  system->shader_config = *(VkrShaderConfig *)cfg_info.as.custom;

  if (!vkr_shader_system_create(&rf->shader_system, &system->shader_config)) {
    log_error("Failed to create shadow shader from config");
    return false_v;
  }

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->shader_config,
          VKR_PIPELINE_DOMAIN_SHADOW, string8_lit("shadow"),
          &system->shadow_pipeline, &pipeline_error)) {
    String8 err = vkr_renderer_get_error_string(pipeline_error);
    log_error("Shadow pipeline creation failed: %s", string8_cstr(&err));
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    return false_v;
  }

  if (system->shader_config.name.str && system->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, system->shadow_pipeline,
        system->shader_config.name, &alias_err);
  }

  return true_v;
}

// ============================================================================
// Public API
// ============================================================================

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

  if (!vkr_shadow_create_renderpass(system, rf)) {
    goto cleanup;
  }

  if (!vkr_shadow_create_shadow_maps(system, rf)) {
    goto cleanup;
  }

  if (!vkr_shadow_create_pipeline(system, rf)) {
    goto cleanup;
  }

  system->initialized = true_v;
  return true_v;

cleanup:
  vkr_shadow_system_shutdown(system, rf);
  return false_v;
}

void vkr_shadow_system_shutdown(VkrShadowSystem *system, RendererFrontend *rf) {
  if (!system || !rf) {
    return;
  }

  if (system->shadow_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->shadow_pipeline);
    system->shadow_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (system->shader_config.name.str) {
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
  }

  if (system->frames) {
    for (uint32_t f = 0; f < system->frame_resource_count; ++f) {
      for (uint32_t c = 0; c < system->config.cascade_count; ++c) {
        if (system->frames[f].shadow_targets[c]) {
          vkr_renderer_render_target_destroy(
              rf, system->frames[f].shadow_targets[c], true_v);
          system->frames[f].shadow_targets[c] = NULL;
        }
        if (system->frames[f].shadow_maps[c]) {
          vkr_renderer_destroy_texture(rf, system->frames[f].shadow_maps[c]);
          system->frames[f].shadow_maps[c] = NULL;
        }
      }
    }

    vkr_allocator_free(&rf->allocator, system->frames,
                       sizeof(VkrShadowFrameResources) *
                           (uint64_t)system->frame_resource_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    system->frames = NULL;
    system->frame_resource_count = 0;
  }

  if (system->shadow_renderpass && system->owns_renderpass) {
    vkr_renderer_renderpass_destroy(rf, system->shadow_renderpass);
  }
  system->shadow_renderpass = NULL;
  system->owns_renderpass = false_v;

  system->initialized = false_v;
}

void vkr_shadow_system_update(VkrShadowSystem *system, const VkrCamera *camera,
                              bool8_t light_enabled, Vec3 light_direction) {
  if (!system || !system->initialized || !camera) {
    return;
  }

  system->light_enabled = light_enabled;
  system->light_direction = light_direction;

  if (!light_enabled) {
    for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
      system->cascades[i].view_projection = mat4_identity();
      system->cascades[i].split_far = 0.0f;
      system->cascades[i].world_units_per_texel = 0.0f;
      system->cascades[i].bounds_center = vec3_zero();
      system->cascades[i].bounds_radius = 0.0f;
    }
    return;
  }

  vkr_shadow_compute_cascade_splits(system, camera->near_clip, camera->far_clip,
                                    system->config.cascade_split_lambda);

  for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
    float32_t split_near = system->cascade_splits[i];
    float32_t split_far = system->cascade_splits[i + 1];

    Vec3 corners[8];
    vkr_shadow_compute_frustum_corners(camera, split_near, split_far, corners);

    vkr_shadow_compute_cascade_matrix(
        &light_direction, corners, system->config.shadow_map_size,
        system->config.stabilize_cascades, system->config.cascade_guard_band_texels,
        &system->config.scene_bounds, &system->cascades[i].view_projection,
        &system->cascades[i].world_units_per_texel);

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
}

VkrRenderTargetHandle
vkr_shadow_system_get_render_target(const VkrShadowSystem *system,
                                    uint32_t frame_index,
                                    uint32_t cascade_index) {
  if (!system || !system->frames) {
    return NULL;
  }
  if (frame_index >= system->frame_resource_count) {
    return NULL;
  }
  if (cascade_index >= system->config.cascade_count) {
    return NULL;
  }
  return system->frames[frame_index].shadow_targets[cascade_index];
}

void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data) {
  if (!out_data) {
    return;
  }

  MemZero(out_data, sizeof(*out_data));

  if (!system || !system->initialized) {
    return;
  }
  if (frame_index >= system->frame_resource_count) {
    return;
  }

  out_data->enabled = system->light_enabled;
  out_data->cascade_count = system->config.cascade_count;
  out_data->shadow_map_inv_size =
      (system->config.shadow_map_size > 0)
          ? (1.0f / (float32_t)system->config.shadow_map_size)
          : 0.0f;
  out_data->pcf_radius = system->config.pcf_radius;
  out_data->shadow_bias = system->config.shadow_bias;
  out_data->normal_bias = system->config.normal_bias;
  out_data->debug_show_cascades = system->config.debug_show_cascades;

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    if (i < system->config.cascade_count) {
      out_data->split_far[i] = system->cascades[i].split_far;
      out_data->view_projection[i] = system->cascades[i].view_projection;
      out_data->shadow_maps[i] = system->frames[frame_index].shadow_maps[i];
    } else {
      out_data->split_far[i] = 0.0f;
      out_data->view_projection[i] = mat4_identity();
      out_data->shadow_maps[i] = NULL;
    }
  }
}
