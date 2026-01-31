#include "renderer/systems/vkr_shadow_system.h"

#include "core/logger.h"
#include "math/vkr_math.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/vulkan/vulkan_types.h"

vkr_internal VkrTextureFormat
vkr_shadow_get_depth_format(RendererFrontend *rf) {
  if (!rf) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }

  VkrTextureOpaqueHandle depth_tex = vkr_renderer_depth_attachment_get(rf);
  if (!depth_tex) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }

  struct s_TextureHandle *handle = (struct s_TextureHandle *)depth_tex;
  return handle->description.format;
}

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

vkr_internal void vkr_shadow_compute_cascade_matrix(
    const Mat4 *light_view, const Vec3 frustum_corners[8],
    uint32_t shadow_map_size, bool8_t stabilize, float32_t guard_band_texels,
    bool8_t use_constant_cascade_size, const VkrShadowSceneBounds *scene_bounds,
    float32_t z_extension_factor, Mat4 *out_view_projection,
    float32_t *out_world_units_per_texel, Vec2 *out_light_space_origin) {
  Mat4 view = light_view ? *light_view : mat4_identity();

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

  // Extend Z bounds using scene AABB (like reference implementation)
  // This ensures all shadow casters in the scene are included in the depth
  // range, regardless of camera position - eliminating shadow pop-in.
  if (scene_bounds && scene_bounds->use_scene_bounds) {
    // Transform all 8 corners of the scene AABB to light space and extend Z
    for (int i = 0; i < 8; ++i) {
      Vec3 corner =
          vec3_new((i & 1) ? scene_bounds->max.x : scene_bounds->min.x,
                   (i & 2) ? scene_bounds->max.y : scene_bounds->min.y,
                   (i & 4) ? scene_bounds->max.z : scene_bounds->min.z);
      Vec4 corner_ls = mat4_mul_vec4(view, vec3_to_vec4(corner, 1.0f));
      // Only extend Z, not XY (XY is fitted to frustum for resolution)
      min_z = vkr_min_f32(min_z, corner_ls.z);
      max_z = vkr_max_f32(max_z, corner_ls.z);
    }
  } else if (z_extension_factor > 0.0f) {
    float32_t z_ext = radius * z_extension_factor;
    min_z -= z_ext;
    max_z += z_ext;
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

  float32_t left = center_x - half;
  float32_t right = center_x + half;
  float32_t bottom = center_y - half;
  float32_t top = center_y + half;
  *out_world_units_per_texel = texel_size;
  if (out_light_space_origin) {
    out_light_space_origin->x = left - view.columns.col3.x;
    out_light_space_origin->y = bottom - view.columns.col3.y;
  }

  float32_t near_clip = -max_z;
  float32_t far_clip = -min_z;
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

// ============================================================================
// Resource Creation
// ============================================================================

vkr_internal bool8_t vkr_shadow_create_renderpass(VkrShadowSystem *system,
                                                  RendererFrontend *rf) {
  VkrRenderPassHandle pass =
      vkr_renderer_renderpass_get(rf, string8_lit("Renderpass.CSM.Shadow"));
  if (!pass) {
    VkrTextureFormat depth_format = vkr_shadow_get_depth_format(rf);
    VkrClearValue clear_depth = {.depth_stencil = {1.0f, 0}};
    VkrRenderPassAttachmentDesc depth_attachment = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc desc = {
        .name = string8_lit("Renderpass.CSM.Shadow"),
        .domain = VKR_PIPELINE_DOMAIN_SHADOW,
        .color_attachment_count = 0,
        .color_attachments = NULL,
        .depth_stencil_attachment = &depth_attachment,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    pass = vkr_renderer_renderpass_create_desc(rf, &desc, &pass_err);
    if (!pass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("Failed to create shadow render pass");
      log_error("Renderpass error: %s", string8_cstr(&err));
      return false_v;
    }
    system->owns_renderpass = true_v;
  }

  system->shadow_renderpass = pass;
  return true_v;
}

vkr_internal bool8_t vkr_shadow_create_shadow_maps(VkrShadowSystem *system,
                                                   RendererFrontend *rf) {
  uint32_t cascades = system->config.cascade_count;
  uint32_t frames = vkr_renderer_window_attachment_count(rf);
  if (frames == 0 || cascades == 0) {
    return false_v;
  }

  uint32_t map_size = vkr_shadow_config_get_max_map_size(&system->config);
  if (map_size == 0) {
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
    VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
    system->frames[f].shadow_map =
        vkr_renderer_create_sampled_depth_attachment_array(
            rf, map_size, map_size, cascades, &tex_err);
    if (!system->frames[f].shadow_map) {
      String8 err = vkr_renderer_get_error_string(tex_err);
      log_error("Failed to create shadow depth array: %s", string8_cstr(&err));
      return false_v;
    }

    for (uint32_t c = 0; c < cascades; ++c) {
      VkrRenderTargetAttachmentRef attachments[1] = {
          {.texture = system->frames[f].shadow_map,
           .mip_level = 0,
           .base_layer = c,
           .layer_count = 1},
      };
      VkrRenderTargetDesc rt_desc = {
          .sync_to_window_size = false_v,
          .attachment_count = 1,
          .attachments = attachments,
          .width = map_size,
          .height = map_size,
      };

      VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
      system->frames[f].shadow_targets[c] = vkr_renderer_render_target_create(
          rf, &rt_desc, system->shadow_renderpass, &rt_err);
      if (!system->frames[f].shadow_targets[c]) {
        String8 err = vkr_renderer_get_error_string(rt_err);
        log_error(
            "Failed to create shadow render target (frame %u, cascade %u)", f,
            c);
        log_error("Render target error: %s", string8_cstr(&err));
        return false_v;
      }
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_shadow_create_pipeline(VkrShadowSystem *system,
                                                RendererFrontend *rf) {
  VkrResourceHandleInfo alpha_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/shadow.shadercfg"),
          &rf->scratch_allocator, &alpha_cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Shadow shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  if (!alpha_cfg_info.as.custom) {
    log_error("Shadow shadercfg returned null custom data");
    return false_v;
  }

  system->shader_config_alpha = *(VkrShaderConfig *)alpha_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system,
                                &system->shader_config_alpha)) {
    log_error("Failed to create shadow alpha shader from config");
    return false_v;
  }

  VkrResourceHandleInfo opaque_cfg_info = {0};
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/shadow_opaque.shadercfg"),
          &rf->scratch_allocator, &opaque_cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Shadow opaque shadercfg load failed: %s", string8_cstr(&err));
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    return false_v;
  }

  if (!opaque_cfg_info.as.custom) {
    log_error("Shadow opaque shadercfg returned null custom data");
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    return false_v;
  }

  system->shader_config_opaque = *(VkrShaderConfig *)opaque_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system,
                                &system->shader_config_opaque)) {
    log_error("Failed to create shadow opaque shader from config");
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    return false_v;
  }

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->shader_config_alpha,
          VKR_PIPELINE_DOMAIN_SHADOW, string8_lit("shadow_alpha"),
          &system->shadow_pipeline_alpha, &pipeline_error)) {
    String8 err = vkr_renderer_get_error_string(pipeline_error);
    log_error("Shadow alpha pipeline creation failed: %s", string8_cstr(&err));
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow.opaque");
    return false_v;
  }

  pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->shader_config_opaque,
          VKR_PIPELINE_DOMAIN_SHADOW, string8_lit("shadow_opaque"),
          &system->shadow_pipeline_opaque, &pipeline_error)) {
    String8 err = vkr_renderer_get_error_string(pipeline_error);
    log_error("Shadow opaque pipeline creation failed: %s", string8_cstr(&err));
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->shadow_pipeline_alpha);
    system->shadow_pipeline_alpha = VKR_PIPELINE_HANDLE_INVALID;
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow.opaque");
    return false_v;
  }

  if (system->shader_config_alpha.name.str &&
      system->shader_config_alpha.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, system->shadow_pipeline_alpha,
        system->shader_config_alpha.name, &alias_err);
  }

  if (system->shader_config_opaque.name.str &&
      system->shader_config_opaque.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, system->shadow_pipeline_opaque,
        system->shader_config_opaque.name, &alias_err);
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
  if (!(system->config.anchor_snap_texels >= 0.0f)) {
    system->config.anchor_snap_texels = 0.0f;
  }
  // Vulkan rasterization depth bias: keep parameters non-negative and
  // deterministic even if caller passes NaNs.
  if (!(system->config.depth_bias_constant_factor >= 0.0f)) {
    system->config.depth_bias_constant_factor = 0.0f;
  }
  if (!(system->config.depth_bias_slope_factor >= 0.0f)) {
    system->config.depth_bias_slope_factor = 0.0f;
  }
  if (!(system->config.depth_bias_clamp >= 0.0f)) {
    system->config.depth_bias_clamp = 0.0f;
  }
  if (!(system->config.shadow_bias >= 0.0f)) {
    system->config.shadow_bias = 0.0f;
  }
  if (!(system->config.normal_bias >= 0.0f)) {
    system->config.normal_bias = 0.0f;
  }
  if (!(system->config.shadow_slope_bias >= 0.0f)) {
    system->config.shadow_slope_bias = 0.0f;
  }
  if (!(system->config.shadow_bias_texel_scale >= 0.0f)) {
    system->config.shadow_bias_texel_scale = 0.0f;
  }
  if (!(system->config.shadow_slope_bias_texel_scale >= 0.0f)) {
    system->config.shadow_slope_bias_texel_scale = 0.0f;
  }
  if (!(system->config.shadow_distance_fade_range >= 0.0f)) {
    system->config.shadow_distance_fade_range = 0.0f;
  }
  system->config.foliage_alpha_cutoff_bias =
      vkr_clamp_f32(system->config.foliage_alpha_cutoff_bias, 0.0f, 1.0f);

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

  if (system->shadow_pipeline_alpha.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->shadow_pipeline_alpha);
    system->shadow_pipeline_alpha = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (system->shadow_pipeline_opaque.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->shadow_pipeline_opaque);
    system->shadow_pipeline_opaque = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (system->shader_config_alpha.name.str) {
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow");
  }

  if (system->shader_config_opaque.name.str) {
    vkr_shader_system_delete(&rf->shader_system, "shader.shadow.opaque");
  }

  if (system->frames) {
    for (uint32_t f = 0; f < system->frame_resource_count; ++f) {
      for (uint32_t c = 0; c < system->config.cascade_count; ++c) {
        if (system->frames[f].shadow_targets[c]) {
          vkr_renderer_render_target_destroy(
              rf, system->frames[f].shadow_targets[c]);
          system->frames[f].shadow_targets[c] = NULL;
        }
      }
      if (system->frames[f].shadow_map) {
        vkr_renderer_destroy_texture(rf, system->frames[f].shadow_map);
        system->frames[f].shadow_map = NULL;
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
      system->cascades[i].light_space_origin = vec2_zero();
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

  for (uint32_t i = 0; i < system->config.cascade_count; ++i) {
    float32_t split_near = system->cascade_splits[i];
    float32_t split_far = system->cascade_splits[i + 1];

    Vec3 corners[8];
    vkr_shadow_compute_frustum_corners(camera, split_near, split_far, corners);

    vkr_shadow_compute_cascade_matrix(
        &light_view, corners, shadow_map_size,
        system->config.stabilize_cascades,
        system->config.cascade_guard_band_texels,
        system->config.use_constant_cascade_size, &system->config.scene_bounds,
        system->config.z_extension_factor, &system->cascades[i].view_projection,
        &system->cascades[i].world_units_per_texel,
        &system->cascades[i].light_space_origin);

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
  out_data->pcf_radius = system->config.pcf_radius;
  out_data->shadow_bias = system->config.shadow_bias;
  out_data->normal_bias = system->config.normal_bias;
  out_data->shadow_slope_bias = system->config.shadow_slope_bias;
  out_data->shadow_bias_texel_scale = system->config.shadow_bias_texel_scale;
  out_data->shadow_slope_bias_texel_scale =
      system->config.shadow_slope_bias_texel_scale;
  out_data->shadow_distance_fade_range =
      system->config.shadow_distance_fade_range;
  out_data->cascade_blend_range = system->config.cascade_blend_range;
  out_data->debug_show_cascades = system->config.debug_show_cascades;

  uint32_t map_size = vkr_shadow_config_get_max_map_size(&system->config);

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    if (i < system->config.cascade_count) {
      out_data->shadow_map_inv_size[i] =
          (map_size > 0) ? (1.0f / (float32_t)map_size) : 0.0f;
      out_data->split_far[i] = system->cascades[i].split_far;
      out_data->world_units_per_texel[i] =
          system->cascades[i].world_units_per_texel;
      out_data->light_space_origin[i] = system->cascades[i].light_space_origin;
      out_data->view_projection[i] = system->cascades[i].view_projection;
    } else {
      out_data->shadow_map_inv_size[i] = 0.0f;
      out_data->split_far[i] = 0.0f;
      out_data->world_units_per_texel[i] = 0.0f;
      out_data->light_space_origin[i] = vec2_zero();
      out_data->view_projection[i] = mat4_identity();
    }
  }

  out_data->shadow_map = system->frames[frame_index].shadow_map;
}
