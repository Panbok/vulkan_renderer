#include "visibility_test.h"

#include "math/vkr_frustum.h"
#include "math/vkr_math.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_candidate_residency.h"
#include "renderer/vkr_visibility.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_packet_pre_recording_rejection(void) {
  VkrValidationError validation = {0};
  VkrWorldPassPayload world = {
      .gpu_candidate_count = VKR_GPU_DRAW_CANDIDATE_CAPACITY + 1u,
  };
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .world = &world,
  };
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.gpu_candidate_count") ==
         0);

  world = (VkrWorldPassPayload){.gpu_candidate_count = 1u};
  validation = (VkrValidationError){0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.gpu_candidates") == 0);

  VkrWorldDrawCandidate candidate = {0};
  world.gpu_candidates = &candidate;
  world.static_generation = 1u;
  world.dynamic_generation = 1u;
  world.publication_generation = 1u;
  validation = (VkrValidationError){0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
  assert(validation.field_path == NULL && validation.message == NULL);
  world.static_generation = 0u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.static_generation") == 0);
  world.static_generation = 1u;
  world.dynamic_generation = 0u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.dynamic_generation") == 0);
  world.dynamic_generation = 1u;
  world.publication_generation = 0u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.publication_generation") ==
         0);
  world.publication_generation = 1u;
  world.static_candidate_count = 2u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.static_candidate_count") ==
         0);
  world.static_candidate_count = 0u;

  packet.world = NULL;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
}
static void test_candidate_residency_generation_contract(void) {
  VkrCandidateResidencyState committed = {0};
  assert(vkr_candidate_residency_needs_static_repack(&committed, 1u, 1u, 1u));
  const VkrCandidateResidencyState staged =
      vkr_candidate_residency_stage(3u, 5u, 7u, 11u, 2u);
  assert(
      staged.valid && staged.static_generation == 3u &&
      staged.publication_generation == 5u && staged.resource_generation == 7u &&
      staged.packed_static_count == 11u && staged.omitted_static_count == 2u);
  /* Recording stages a replacement without mutating committed slot authority.
     Only the backend's successful-submit path performs this assignment. */
  assert(!committed.valid);
  committed = staged;
  assert(!vkr_candidate_residency_needs_static_repack(&committed, 3u, 5u, 7u));
  assert(vkr_candidate_residency_needs_static_repack(&committed, 3u, 6u, 7u));
  committed = vkr_candidate_residency_stage(3u, 6u, 7u, 13u, 0u);
  assert(committed.packed_static_count == 13u &&
         committed.omitted_static_count == 0u);
  assert(!vkr_candidate_residency_needs_static_repack(&staged, 3u, 5u, 7u));
  assert(vkr_candidate_residency_needs_static_repack(&staged, 4u, 5u, 7u));
  assert(vkr_candidate_residency_needs_static_repack(&staged, 3u, 6u, 7u));
  assert(vkr_candidate_residency_needs_static_repack(&staged, 3u, 5u, 8u));
  assert(!vkr_candidate_residency_stage(0u, 1u, 1u, 0u, 0u).valid);
}

static void test_packet_independent_transmission_stream(void) {
  const VkrWorldDrawCandidate candidate = {0};
  const VkrWorldPassPayload world = {
      .transmission_gpu_candidates = &candidate,
      .transmission_gpu_candidate_count = 1u,
  };
  const VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .world = &world,
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
}

static void test_packet_borrowed_array_validation(void) {
  VkrValidationError validation = {0};
  VkrWorldPassPayload world = {.instance_count = 1u};
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .world = &world,
  };
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.instances") == 0);

  VkrInstanceDataGPU instance = {0};
  VkrDrawItem draw = {.instance_count = 1u, .first_instance = 1u};
  world.instances = &instance;
  world.transparent_draws = &draw;
  world.transparent_draw_count = 1u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.transparent_draws") == 0);

  draw.first_instance = 0u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  VkrFrameLighting lighting = {.point_light_count = 1u};
  packet.world = NULL;
  packet.lighting = &lighting;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.lighting.point_lights") == 0);

  VkrPointLight light = {0};
  VkrPointLightGrid grid = {0};
  lighting.point_lights = &light;
  lighting.point_light_grid = &grid;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);
}

static void test_packet_text_geometry_validation(void) {
  VkrValidationError validation = {0};
  VkrPreparedTextDraw text = {.vertex_count = 1u, .index_count = 1u};
  VkrWorldPassPayload world = {.text_draws = &text, .text_draw_count = 1u};
  const VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .world = &world,
  };
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.world.text_draws") == 0);

  VkrTextVertex vertex = {0};
  uint32_t index = 0u;
  text.vertices = &vertex;
  text.indices = &index;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  text.max_index = 1u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
}

static void test_packet_ui_stream_validation(void) {
  VkrUiVertex vertices[4] = {
      {.position = {0.0f, 0.0f}},
      {.position = {10.0f, 0.0f}},
      {.position = {10.0f, 10.0f}},
      {.position = {0.0f, 10.0f}},
  };
  uint32_t indices[6] = {0u, 1u, 2u, 2u, 3u, 0u};
  VkrUiDrawBatch batch = {
      .index_count = ArrayCount(indices),
      .scissor_rect_px = {0.0f, 0.0f, 100.0f, 50.0f},
      .mode = VKR_UI_DRAW_MODE_QUAD,
  };
  VkrUiPassPayload ui = {
      .draw_list =
          {
              .vertices = vertices,
              .vertex_count = ArrayCount(vertices),
              .indices = indices,
              .index_count = ArrayCount(indices),
              .batches = &batch,
              .batch_count = 1u,
          },
  };
  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .frame = {.window_width = 100u, .window_height = 50u},
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .ui = &ui,
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  indices[5] = 4u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.ui.draw_list.indices") == 0);
  indices[5] = 0u;

  batch.scissor_rect_px.width = 101.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.ui.draw_list.batches.scissor_rect_px") == 0);
  batch.scissor_rect_px.width = 100.0f;

  batch.mode = VKR_UI_DRAW_MODE_MTSDF_TEXT;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.ui.draw_list.batches.texture") ==
         0);
}

static VkrRendererError test_packet_cancel_success(void *state) {
  RendererFrontend *renderer = state;
  renderer->frame_active = false_v;
  return VKR_RENDERER_ERROR_NONE;
}

static VkrRendererError test_packet_cancel_failure(void *state) {
  (void)state;
  return VKR_RENDERER_ERROR_DEVICE_ERROR;
}

static void test_packet_rejection_preserves_cancel_failure(void) {
  static RendererFrontend renderer;
  VkrRendererImplOps ops = {.cancel_frame = test_packet_cancel_success};
  renderer = (RendererFrontend){
      .impl = {.ops = &ops, .state = &renderer},
      .frame_active = true_v,
  };
  const VkrRenderPacket invalid_packet = {0};
  VkrValidationError validation = {0};
  assert(vkr_renderer_submit_packet(&renderer, &invalid_packet, NULL,
                                    &validation) ==
         VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE);
  assert(!renderer.frame_active);

  ops.cancel_frame = test_packet_cancel_failure;
  renderer.frame_active = true_v;
  assert(vkr_renderer_submit_packet(&renderer, &invalid_packet, NULL,
                                    &validation) ==
         VKR_RENDERER_ERROR_DEVICE_ERROR);
  assert(strcmp(validation.field_path, "frame") == 0);
}

static void test_alpha_routing(void) {
  const VkrDrawAlphaRouting opaque =
      vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_OPAQUE);
  const VkrDrawAlphaRouting cutout =
      vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_CUTOUT);
  const VkrDrawAlphaRouting blend =
      vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_BLEND);
  assert(!opaque.world_transparent && !opaque.shadow_alpha_tested);
  assert(!cutout.world_transparent && cutout.shadow_alpha_tested);
  assert(blend.world_transparent && !blend.shadow_alpha_tested);
}

static void test_gpu_state_buckets(void) {
  assert(vkr_world_draw_state_bucket(VKR_MATERIAL_ALPHA_OPAQUE, false_v) ==
         VKR_WORLD_DRAW_STATE_OPAQUE_BACK);
  assert(vkr_world_draw_state_bucket(VKR_MATERIAL_ALPHA_OPAQUE, true_v) ==
         VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED);
  assert(vkr_world_draw_state_bucket(VKR_MATERIAL_ALPHA_CUTOUT, false_v) ==
         VKR_WORLD_DRAW_STATE_CUTOUT_BACK);
  assert(vkr_world_draw_state_bucket(VKR_MATERIAL_ALPHA_CUTOUT, true_v) ==
         VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED);
  assert((VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID &
          VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE) == 0u);
  assert((VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE &
          VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER) == 0u);
}

static bool8_t clip_contains_point(Mat4 view_projection, Vec3 point) {
  const Vec4 clip =
      mat4_mul_vec4(view_projection, vec4_new(point.x, point.y, point.z, 1.0f));
  return clip.w > 0.0f && clip.z >= 0.0f && clip.z <= clip.w &&
                 clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w &&
                 clip.y <= clip.w
             ? true_v
             : false_v;
}

static void test_frustum_never_rejects_visible_geometry(void) {
  const Mat4 view =
      mat4_look_at(vec3_zero(), vec3_new(0.0f, 0.0f, -1.0f), vec3_up());
  const Mat4 projection =
      mat4_perspective(vkr_to_radians(60.0f), 1.0f, 0.1f, 100.0f);
  const Mat4 view_projection = mat4_mul(projection, view);
  const VkrFrustum frustum = vkr_frustum_from_view_projection(view, projection);
  uint32_t visible_count = 0u;
  for (int32_t x = -60; x <= 60; x += 5) {
    for (int32_t y = -60; y <= 60; y += 5) {
      for (int32_t z = -60; z <= 60; z += 5) {
        const Vec3 point = vec3_new((float32_t)x, (float32_t)y, (float32_t)z);
        if (!clip_contains_point(view_projection, point))
          continue;
        visible_count++;
        assert(vkr_frustum_test_sphere(&frustum, point, 0.0f));
      }
    }
  }
  assert(visible_count > 0u);
}

static void test_orthographic_frustum_uses_vulkan_depth(void) {
  const Mat4 view = mat4_look_at(vec3_zero(), vec3_forward(), vec3_up());
  const Mat4 projection =
      mat4_ortho_zo_yinv(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);
  const VkrFrustum frustum = vkr_frustum_from_view_projection(view, projection);
  assert(vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -0.11f), 0.0f));
  assert(vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -99.9f), 0.0f));
  assert(!vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, 1.0f), 0.0f));
  assert(
      !vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -101.0f), 0.0f));
}

static void test_transparent_sort_and_emit(void) {
  VkrTransparentDrawCandidate candidates[3] = {
      {.mesh = {1u, 1u},
       .geometry = {11u, 1u},
       .material = {21u, 1u},
       .instance = {.object_id = 31u, .temporal_index = 7u,
                    .temporal_generation = 4u},
       .sort_key = 10u},
      {.mesh = {2u, 1u},
       .geometry = {12u, 1u},
       .material = {22u, 1u},
       .instance = {.object_id = 32u, .temporal_index = 8u,
                    .temporal_generation = 5u},
       .sort_key = 30u},
      {.mesh = {3u, 1u},
       .geometry = {13u, 1u},
       .material = {23u, 1u},
       .instance = {.object_id = 33u, .temporal_index = 9u,
                    .temporal_generation = 6u},
       .sort_key = 20u},
  };
  qsort(candidates, 3u, sizeof(candidates[0]),
        vkr_transparent_draw_depth_compare);
  VkrDrawItem draws[3] = {0};
  VkrInstanceDataGPU instances[3] = {0};
  assert(vkr_transparent_draw_emit(candidates, 3u, draws, instances) == 3u);
  assert(draws[0].sort_key == 30u && draws[1].sort_key == 20u &&
         draws[2].sort_key == 10u);
  for (uint32_t i = 0; i < 3u; ++i) {
    assert(draws[i].instance_count == 1u);
    assert(draws[i].first_instance == i);
    assert(draws[i].material.id == candidates[i].material.id);
    assert(instances[i].object_id == candidates[i].instance.object_id);
  }
  /* Source identities must follow their draws through back-to-front sorting. */
  assert(instances[0].temporal_index == 8u &&
         instances[0].temporal_generation == 5u);
  assert(instances[1].temporal_index == 9u &&
         instances[1].temporal_generation == 6u);
  assert(instances[2].temporal_index == 7u &&
         instances[2].temporal_generation == 4u);
}

static void test_submesh_sphere_is_conservative_under_scale(void) {
  Mat4 model = mat4_identity();
  model.m00 = 2.0f;
  model.m11 = 3.0f;
  model.m22 = 4.0f;
  model.m30 = 7.0f;
  model.m31 = -2.0f;
  model.m32 = 5.0f;
  Vec3 center = {0};
  float32_t radius = 0.0f;
  vkr_visibility_submesh_sphere(model, vec3_new(1.0f, 2.0f, 3.0f),
                                vec3_new(-1.0f, -2.0f, -3.0f),
                                vec3_new(1.0f, 2.0f, 3.0f), &center, &radius);
  const Vec3 expected = mat4_mul_vec3(model, vec3_new(1.0f, 2.0f, 3.0f));
  assert(vkr_abs_f32(center.x - expected.x) < 0.0001f);
  assert(vkr_abs_f32(center.y - expected.y) < 0.0001f);
  assert(vkr_abs_f32(center.z - expected.z) < 0.0001f);
  assert(vkr_abs_f32(radius - vec3_length(vec3_new(1.0f, 2.0f, 3.0f)) * 4.0f) <
         0.0001f);
}

bool32_t run_visibility_tests(void) {
  printf("--- Starting Visibility Tests ---\n");
  test_packet_pre_recording_rejection();
  test_candidate_residency_generation_contract();
  test_packet_independent_transmission_stream();
  test_packet_borrowed_array_validation();
  test_packet_text_geometry_validation();
  test_packet_ui_stream_validation();
  test_packet_rejection_preserves_cancel_failure();
  test_alpha_routing();
  test_gpu_state_buckets();
  test_frustum_never_rejects_visible_geometry();
  test_orthographic_frustum_uses_vulkan_depth();
  test_transparent_sort_and_emit();
  test_submesh_sphere_is_conservative_under_scale();
  printf("--- Visibility Tests Completed ---\n");
  return true_v;
}
