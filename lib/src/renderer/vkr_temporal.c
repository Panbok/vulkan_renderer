#include "renderer/vkr_temporal.h"
#include "renderer/vkr_prepared_frame.h"

#include <math.h>

#define VKR_TEMPORAL_CAMERA_CUT_DISTANCE 10.0f
#define VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT 0.5f

vkr_internal void temporal_scene_lane(VkrTemporalSceneSignature *signature,
                                      uint64_t lane) {
  // Two independent 64-bit recurrences over semantic lanes, not struct bytes.
  signature->hash[0] ^= lane;
  signature->hash[0] =
      (signature->hash[0] << 27u) | (signature->hash[0] >> 37u);
  signature->hash[0] *= UINT64_C(0x9e3779b185ebca87);
  signature->hash[1] += lane ^ UINT64_C(0xc2b2ae3d27d4eb4f);
  signature->hash[1] =
      (signature->hash[1] << 31u) | (signature->hash[1] >> 33u);
  signature->hash[1] *= UINT64_C(0x165667b19e3779f9);
}

vkr_internal void temporal_scene_pair(VkrTemporalSceneSignature *signature,
                                      uint32_t a, uint32_t b) {
  temporal_scene_lane(signature, (uint64_t)a | ((uint64_t)b << 32u));
}

vkr_internal void temporal_scene_floats(VkrTemporalSceneSignature *signature,
                                        float32_t a, float32_t b) {
  uint32_t a_bits, b_bits;
  MemCopy(&a_bits, &a, sizeof(a_bits));
  MemCopy(&b_bits, &b, sizeof(b_bits));
  temporal_scene_pair(signature, a_bits, b_bits);
}

vkr_internal void temporal_scene_vec3(VkrTemporalSceneSignature *signature,
                                      Vec3 v) {
  temporal_scene_floats(signature, v.x, v.y);
  temporal_scene_floats(signature, v.z, 0.0f);
}

vkr_internal void temporal_scene_vec4(VkrTemporalSceneSignature *signature,
                                      Vec4 v) {
  temporal_scene_floats(signature, v.x, v.y);
  temporal_scene_floats(signature, v.z, v.w);
}

vkr_internal void temporal_scene_matrix(VkrTemporalSceneSignature *signature,
                                        Mat4 matrix) {
  for (uint32_t i = 0u; i < 16u; i += 2u)
    temporal_scene_floats(signature, matrix.elements[i],
                          matrix.elements[i + 1u]);
}

vkr_internal void temporal_scene_instance(VkrTemporalSceneSignature *signature,
                                          const VkrInstanceDataGPU *instance) {
  temporal_scene_matrix(signature, instance->model);
  temporal_scene_pair(signature, instance->object_id, instance->temporal_index);
  temporal_scene_pair(signature, instance->temporal_generation,
                      instance->temporal_flags);
}

vkr_internal void
temporal_scene_candidates(VkrTemporalSceneSignature *signature,
                          const VkrWorldDrawCandidate *rows, uint32_t count) {
  temporal_scene_lane(signature, count);
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrWorldDrawCandidate *row = &rows[i];
    temporal_scene_pair(signature, row->mesh.id, row->mesh.generation);
    temporal_scene_pair(signature, row->geometry.id, row->geometry.generation);
    temporal_scene_pair(signature, row->material.id, row->material.generation);
    temporal_scene_pair(signature, row->submesh_index, row->state_bucket);
    temporal_scene_lane(signature, row->flags);
    temporal_scene_instance(signature, &row->instance);
    temporal_scene_vec4(signature, row->local_bounding_sphere);
  }
}

VkrTemporalSceneSignature
vkr_temporal_scene_signature(const VkrPreparedFrame *packet) {
  VkrTemporalSceneSignature signature = {
      .hash = {UINT64_C(0x243f6a8885a308d3), UINT64_C(0x13198a2e03707344)},
      .eligible = true_v,
  };
  const VkrWorldPassPayload *world = packet->input.world;
  if (world && (world->publication_pending || world->text_draw_count)) {
    signature.eligible = false_v;
    return signature;
  }
  temporal_scene_matrix(&signature, packet->input.globals.view);
  temporal_scene_matrix(&signature, packet->input.globals.projection);
  temporal_scene_vec3(&signature, packet->input.globals.view_position);
  temporal_scene_vec4(&signature, packet->input.globals.ambient_color);
  temporal_scene_pair(
      &signature, packet->input.globals.render_mode,
      packet->input.debug ? packet->input.debug->shadow_debug_mode : 0u);
  temporal_scene_lane(&signature, packet->gtao.enabled);
  temporal_scene_floats(&signature, packet->gtao.radius, packet->gtao.power);
  temporal_scene_lane(&signature, world != NULL);
  if (world) {
    temporal_scene_candidates(&signature, world->gpu_candidates,
                              world->gpu_candidate_count);
    temporal_scene_candidates(&signature, world->transmission_gpu_candidates,
                              world->transmission_gpu_candidate_count);
    temporal_scene_pair(&signature, world->gpu_camera_opaque_candidate_count,
                        world->gpu_shadow_candidate_count);
    temporal_scene_lane(&signature, world->instance_count);
    for (uint32_t i = 0u; i < world->instance_count; ++i)
      temporal_scene_instance(&signature, &world->instances[i]);
    temporal_scene_lane(&signature, world->transparent_draw_count);
    for (uint32_t i = 0u; i < world->transparent_draw_count; ++i) {
      const VkrDrawItem *draw = &world->transparent_draws[i];
      temporal_scene_pair(&signature, draw->mesh.id, draw->mesh.generation);
      temporal_scene_pair(&signature, draw->geometry.id,
                          draw->geometry.generation);
      temporal_scene_pair(&signature, draw->material.id,
                          draw->material.generation);
      temporal_scene_pair(&signature, draw->submesh_index,
                          draw->instance_count);
      temporal_scene_lane(&signature, draw->first_instance);
      temporal_scene_lane(&signature, draw->sort_key);
    }
  }
  const VkrFrameLighting *lighting = packet->input.lighting;
  temporal_scene_lane(&signature, lighting != NULL);
  if (lighting) {
    temporal_scene_pair(&signature, lighting->directional_enabled,
                        lighting->ibl_enabled);
    temporal_scene_vec3(&signature, lighting->directional_direction);
    temporal_scene_vec3(&signature, lighting->directional_color);
    temporal_scene_floats(&signature, lighting->directional_intensity,
                          lighting->ibl_intensity);
    temporal_scene_pair(&signature, lighting->ibl_source.id,
                        lighting->ibl_source.generation);
    temporal_scene_floats(&signature, lighting->ibl_diffuse_intensity,
                          lighting->ibl_specular_intensity);
    temporal_scene_lane(&signature, lighting->point_light_count);
    for (uint32_t i = 0u; i < lighting->point_light_count; ++i) {
      const VkrPointLight *light = &lighting->point_lights[i];
      temporal_scene_vec3(&signature, light->position);
      temporal_scene_vec3(&signature, light->color);
      temporal_scene_floats(&signature, light->intensity, light->constant);
      temporal_scene_floats(&signature, light->linear, light->quadratic);
      temporal_scene_floats(&signature, light->range, light->inner_cone_angle);
      temporal_scene_vec3(&signature, light->direction);
      temporal_scene_floats(&signature, light->outer_cone_angle, 0.0f);
      temporal_scene_pair(&signature, light->kind, light->render_id);
    }
    if (lighting->point_light_count) {
      const VkrPointLightGrid *grid = lighting->point_light_grid;
      temporal_scene_vec3(&signature, grid->origin);
      temporal_scene_floats(&signature, grid->cell_size, 0.0f);
      temporal_scene_pair(&signature, grid->dimensions[0], grid->dimensions[1]);
      temporal_scene_pair(&signature, grid->dimensions[2], grid->cell_count);
      for (uint32_t i = 0u; i < VKR_POINT_LIGHT_GRID_MASK_WORDS; i += 2u)
        temporal_scene_pair(&signature, grid->global_mask.words[i],
                            grid->global_mask.words[i + 1u]);
      for (uint32_t cell = 0u; cell < grid->cell_count; ++cell)
        for (uint32_t i = 0u; i < VKR_POINT_LIGHT_GRID_MASK_WORDS; i += 2u)
          temporal_scene_pair(&signature, grid->masks[cell].words[i],
                              grid->masks[cell].words[i + 1u]);
    }
    temporal_scene_lane(&signature, lighting->ibl_probe_count);
    for (uint32_t i = 0u; i < lighting->ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
      temporal_scene_pair(&signature, probe->sh_slot,
                          probe->box_projection_enabled);
      temporal_scene_pair(&signature, probe->prefilter.id,
                          probe->prefilter.generation);
      temporal_scene_vec3(&signature, probe->center);
      temporal_scene_vec3(&signature, probe->extents);
      temporal_scene_floats(&signature, probe->blend_distance, probe->weight);
      temporal_scene_floats(&signature, probe->intensity,
                            probe->diffuse_intensity);
      temporal_scene_floats(&signature, probe->specular_intensity, 0.0f);
    }
  }
  const VkrShadowPassPayload *shadow = packet->input.shadow;
  temporal_scene_lane(&signature, shadow != NULL);
  if (shadow) {
    temporal_scene_pair(&signature, shadow->cascade_count,
                        shadow->sdsm_enabled);
    for (uint32_t i = 0u; i < shadow->cascade_count; ++i) {
      temporal_scene_matrix(&signature,
                            shadow->cascades[i].light_view_projection);
      temporal_scene_vec4(&signature,
                          shadow->cascades[i].split_near_far_texel_depth);
      const Vec4 origin = shadow->cascades[i].origin_inv_size_pad;
      temporal_scene_vec3(&signature, (Vec3){origin.x, origin.y, origin.z});
    }
    const VkrShadowReceiverPacketData *receiver = &shadow->receiver;
    temporal_scene_floats(&signature, receiver->receiver_bias_texels,
                          receiver->slope_bias_texels);
    temporal_scene_floats(&signature, receiver->normal_offset_texels,
                          receiver->pcf_radius_texels);
    temporal_scene_pair(&signature, receiver->pcf_sample_count,
                        receiver->pcf_uniform_early_out);
    temporal_scene_floats(&signature, receiver->cascade_blend_fraction,
                          receiver->fade_start);
    temporal_scene_floats(&signature, receiver->fade_end, 0.0f);
    const VkrShadowConfigOverride bias = shadow->config_override
                                             ? *shadow->config_override
                                             : (VkrShadowConfigOverride){0};
    temporal_scene_floats(&signature, bias.depth_bias_constant,
                          bias.depth_bias_slope);
    temporal_scene_floats(&signature, bias.depth_bias_clamp, 0.0f);
  }
  temporal_scene_lane(&signature, packet->input.skybox != NULL);
  if (packet->input.skybox) {
    temporal_scene_pair(&signature, packet->input.skybox->cubemap.id,
                        packet->input.skybox->cubemap.generation);
    temporal_scene_pair(&signature, packet->input.skybox->material.id,
                        packet->input.skybox->material.generation);
  }
  return signature;
}

Mat4 vkr_temporal_sky_reprojection(Mat4 current_view_projection,
                                   Mat4 previous_view_projection,
                                   Vec3 current_view_position) {
  /* Convert a reconstructed homogeneous world point into a direction from
     the current eye. A zero w removes the previous camera's translation. */
  Mat4 world_direction = mat4_identity();
  world_direction.m03 = -current_view_position.x;
  world_direction.m13 = -current_view_position.y;
  world_direction.m23 = -current_view_position.z;
  world_direction.m33 = 0.0f;
  Mat4 reprojection = mat4_mul(
      previous_view_projection,
      mat4_mul(world_direction, mat4_inverse(current_view_projection)));
  if (current_view_projection.m30 == 0.0f &&
      current_view_projection.m31 == 0.0f &&
      current_view_projection.m32 == 0.0f) {
    /* Orthographic projection leaves a direction's clip w at zero. Intersect
       the previous eye ray with its far plane, matching far_world-eye in the
       sky shader. History reuse proves equal projections, so the eye has the
       same clip coordinates in both frames. For projected direction d and
       eye e, q = d + e*(d.z-d.w)/(e.w-e.z) lies on q.z == q.w. */
    const Vec4 eye_clip =
        mat4_mul_vec4(current_view_projection,
                      vec4_new(current_view_position.x, current_view_position.y,
                               current_view_position.z, 1.0f));
    const Vec4 far_eye = vec4_scale(eye_clip, 1.0f / (eye_clip.w - eye_clip.z));
    Mat4 far_plane = mat4_identity();
    far_plane.cols[2] = vec4_add(far_plane.cols[2], far_eye);
    far_plane.cols[3] = vec4_sub(far_plane.cols[3], far_eye);
    reprojection = mat4_mul(far_plane, reprojection);
  }
  return reprojection;
}

vkr_internal float32_t vkr_temporal_halton(uint32_t index, uint32_t base) {
  float32_t result = 0.0f;
  float32_t fraction = 1.0f;
  while (index > 0u) {
    fraction /= (float32_t)base;
    result += fraction * (float32_t)(index % base);
    index /= base;
  }
  return result;
}

Vec2 vkr_temporal_jitter_for_frame(uint32_t frame_index) {
  const uint32_t sequence_index =
      frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH + 1u;
  return (Vec2){vkr_temporal_halton(sequence_index, 2u) - 0.5f,
                vkr_temporal_halton(sequence_index, 3u) - 0.5f};
}

vkr_internal Vec3 vkr_temporal_view_forward(Mat4 view) {
  const Mat4 camera = mat4_inverse(view);
  return vec3_normalize((Vec3){-camera.m02, -camera.m12, -camera.m22});
}

vkr_internal bool8_t vkr_temporal_matrix_equal(Mat4 a, Mat4 b) {
  return MemCompare(&a, &b, sizeof(a)) == 0 ? true_v : false_v;
}

vkr_internal bool8_t vkr_temporal_camera_cut(
    const VkrTemporalState *state, const VkrTemporalFrameInput *input) {
  const Vec3 delta = vec3_sub(input->view_position, state->view_position);
  if (vec3_length_squared(delta) >
      VKR_TEMPORAL_CAMERA_CUT_DISTANCE * VKR_TEMPORAL_CAMERA_CUT_DISTANCE)
    return true_v;
  return vec3_dot(vkr_temporal_view_forward(state->view),
                  vkr_temporal_view_forward(input->view)) <
                 VKR_TEMPORAL_CAMERA_CUT_FORWARD_DOT
             ? true_v
             : false_v;
}

vkr_internal Mat4 vkr_temporal_jitter_projection(Mat4 projection,
                                                 Vec2 jitter_pixels,
                                                 uint32_t width,
                                                 uint32_t height) {
  // Translate clip xy by clip w so the raster shift is independent of depth,
  // including orthographic projections where clip w is constant.
  const float32_t x = 2.0f * jitter_pixels.x / (float32_t)width;
  const float32_t y = 2.0f * jitter_pixels.y / (float32_t)height;
  projection.m00 += x * projection.m30;
  projection.m01 += x * projection.m31;
  projection.m02 += x * projection.m32;
  projection.m03 += x * projection.m33;
  projection.m10 += y * projection.m30;
  projection.m11 += y * projection.m31;
  projection.m12 += y * projection.m32;
  projection.m13 += y * projection.m33;
  return projection;
}

VkrTemporalFrame vkr_temporal_prepare(const VkrTemporalState *state,
                                      const VkrTemporalFrameInput *input) {
  VkrTemporalFrame frame = {
      .jittered_projection = input->projection,
      .current_view_projection = mat4_mul(input->projection, input->view),
      .reset_reasons = input->explicit_reset_reasons,
      .enabled = input->enabled,
  };
  if (!state->valid) {
    frame.reset_reasons |= VKR_TEMPORAL_RESET_FIRST_FRAME;
  } else {
    if (state->enabled != input->enabled)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_MODE_CHANGE;
    if (state->frame_index + 1u != input->frame_index)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_FRAME_GAP;
    if (state->width != input->width || state->height != input->height)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_EXTENT_CHANGE;
    if (state->scene_generation != input->scene_generation)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_SCENE_CHANGE;
    if (state->render_mode != input->render_mode)
      frame.reset_reasons |= VKR_TEMPORAL_RESET_MODE_CHANGE;
    if (!vkr_temporal_matrix_equal(state->projection, input->projection))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_PROJECTION_CHANGE;
    if (vkr_temporal_camera_cut(state, input))
      frame.reset_reasons |= VKR_TEMPORAL_RESET_CAMERA_CUT;
  }
  if (input->enabled && input->width > 0u && input->height > 0u) {
    frame.jitter_pixels = vkr_temporal_jitter_for_frame(input->frame_index);
    frame.jittered_projection = vkr_temporal_jitter_projection(
        input->projection, frame.jitter_pixels, input->width, input->height);
  }
  frame.history_valid = input->enabled && frame.reset_reasons == 0u;
  return frame;
}

void vkr_temporal_commit(VkrTemporalState *state,
                         const VkrTemporalFrameInput *input) {
  *state = (VkrTemporalState){
      .view = input->view,
      .projection = input->projection,
      .view_position = input->view_position,
      .scene_generation = input->scene_generation,
      .frame_index = input->frame_index,
      .width = input->width,
      .height = input->height,
      .render_mode = input->render_mode,
      .enabled = input->enabled,
      .valid = true_v,
  };
}
