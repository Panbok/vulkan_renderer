#include "shadow_system_test.h"

#include "math/vkr_math.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_render_packet.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fit hysteresis is pure math over a previous and a raw fit, so it is tested
   directly rather than through a camera. Extent and depth may retain a larger
   previous bound but may never retain a smaller one. Center retention relies
   on the stabilization fit's extra guard texel. */

static const float32_t k_texel = 0.5f;

/* vkr_shadow_system_init() only rejects a null frontend; it stores nothing from
   it and vkr_shadow_system_shutdown() ignores it entirely. Passing an opaque
   non-null pointer keeps these tests free of the whole renderer frontend. */
static struct s_RendererFrontend *test_frontend(void) {
  static int placeholder = 0;
  return (struct s_RendererFrontend *)&placeholder;
}

static VkrCamera test_camera(void) {
  VkrCamera camera = {0};
  camera.type = VKR_CAMERA_TYPE_PERSPECTIVE;
  camera.position = vec3_new(0.0f, 2.0f, 0.0f);
  camera.forward = vec3_new(0.0f, 0.0f, -1.0f);
  camera.right = vec3_new(1.0f, 0.0f, 0.0f);
  camera.up = vec3_new(0.0f, 1.0f, 0.0f);
  camera.near_clip = 0.1f;
  camera.far_clip = 500.0f;
  camera.zoom = 70.0f;
  camera.cached_window_width = 1280;
  camera.cached_window_height = 720;
  return camera;
}

static VkrShadowFit make_fit(float32_t center_x, float32_t center_y,
                             float32_t extent, float32_t min_z,
                             float32_t max_z) {
  return (VkrShadowFit){
      .center_x = center_x,
      .center_y = center_y,
      .extent = extent,
      .min_z = min_z,
      .max_z = max_z,
      .world_units_per_texel = k_texel,
  };
}

static void assert_fit_equal(const VkrShadowFit *a, const VkrShadowFit *b) {
  assert(a->center_x == b->center_x);
  assert(a->center_y == b->center_y);
  assert(a->extent == b->extent);
  assert(a->min_z == b->min_z);
  assert(a->max_z == b->max_z);
  assert(a->world_units_per_texel == b->world_units_per_texel);
}

static void
poison_history_inside_deadbands(VkrShadowFitHistory *history,
                                const VkrShadowFitHistory *reference,
                                uint32_t cascade_count) {
  for (uint32_t i = 0u; i < cascade_count; ++i) {
    const VkrShadowFit *raw = &reference->cascades[i];
    VkrShadowFit *stale = &history->cascades[i];
    *stale = *raw;
    stale->center_x += 0.5f * raw->world_units_per_texel;
    stale->extent += raw->world_units_per_texel;
    stale->min_z -= raw->world_units_per_texel;
    stale->max_z += raw->world_units_per_texel;
  }
}

static void test_growth_is_never_deadbanded(void) {
  const VkrShadowFit previous = make_fit(0.0f, 0.0f, 100.0f, -10.0f, 10.0f);
  // Grown by a quarter texel: far inside the two-texel band, but growth is
  // taken regardless, or the volume would stop containing the raw fit.
  const VkrShadowFit raw =
      make_fit(0.0f, 0.0f, 100.0f + 0.25f * k_texel, -10.0f, 10.0f);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.extent == raw.extent);

  // Depth growth on each side independently.
  const VkrShadowFit grown_z = make_fit(0.0f, 0.0f, 100.0f, -10.1f, 10.1f);
  const VkrShadowFit z_fit =
      vkr_shadow_apply_fit_hysteresis(&previous, &grown_z);
  assert(z_fit.min_z == grown_z.min_z);
  assert(z_fit.max_z == grown_z.max_z);
}

static void test_extent_shrink_inside_deadband_is_held(void) {
  const VkrShadowFit previous = make_fit(0.0f, 0.0f, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit raw =
      make_fit(0.0f, 0.0f, 100.0f - 1.5f * k_texel, -10.0f, 10.0f);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.extent == previous.extent);
  assert(fit.world_units_per_texel == previous.world_units_per_texel);
  assert(fit.extent > raw.extent); // Still contains the raw fit.
}

static void test_extent_shrink_beyond_deadband_is_taken(void) {
  const VkrShadowFit previous = make_fit(0.0f, 0.0f, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit raw =
      make_fit(0.0f, 0.0f, 100.0f - 3.0f * k_texel, -10.0f, 10.0f);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.extent == raw.extent);
}

static void test_center_within_one_texel_keeps_previous(void) {
  const VkrShadowFit previous = make_fit(20.0f, -5.0f, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit raw = make_fit(
      20.0f + 0.9f * k_texel, -5.0f - 0.9f * k_texel, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.center_x == previous.center_x);
  assert(fit.center_y == previous.center_y);
}

static void test_center_beyond_one_texel_moves(void) {
  const VkrShadowFit previous = make_fit(20.0f, -5.0f, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit raw =
      make_fit(20.0f + 4.0f * k_texel, -5.0f, 100.0f, -10.0f, 10.0f);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.center_x == raw.center_x);
  // The unmoved axis still holds, so the two axes are genuinely independent.
  assert(fit.center_y == previous.center_y);
}

static void test_depth_shrink_deadbands_each_bound_independently(void) {
  const VkrShadowFit previous = make_fit(0.0f, 0.0f, 100.0f, -10.0f, 10.0f);
  // min_z contracts slightly (held); max_z contracts a lot (taken).
  const VkrShadowFit raw = make_fit(0.0f, 0.0f, 100.0f, -10.0f + 1.0f * k_texel,
                                    10.0f - 5.0f * k_texel);
  const VkrShadowFit fit = vkr_shadow_apply_fit_hysteresis(&previous, &raw);
  assert(fit.min_z == previous.min_z);
  assert(fit.max_z == raw.max_z);
}

static void test_quantize_extent_grows_and_is_bounded(void) {
  const uint32_t map_size = 2048u;
  const float32_t extent = 137.31f;
  const float32_t quantized = vkr_shadow_quantize_extent_up(extent, map_size);
  assert(quantized >= extent); // Never clips the fit it came from.

  // Growth is bounded by one quantum, which is one texel of the power-of-two
  // bracket above the extent. For 137.31 that bracket is 256.
  const float32_t quantum = 256.0f / (float32_t)map_size;
  assert(quantized - extent < quantum);

  // Quantization is idempotent, so a stable extent does not creep upward frame
  // after frame.
  assert(vkr_shadow_quantize_extent_up(quantized, map_size) == quantized);
}

static void test_quantize_extent_is_stable_under_small_change(void) {
  const uint32_t map_size = 2048u;
  // Two extents a hair apart inside the same quantum must land on the same
  // value; that piecewise-constant behaviour is the whole point.
  const float32_t a = vkr_shadow_quantize_extent_up(137.3100f, map_size);
  const float32_t b = vkr_shadow_quantize_extent_up(137.3101f, map_size);
  assert(a == b);
}

/* History validity is the gate that decides whether hysteresis may run at all.
   These cases drive the system through the transitions the design names as
   invalidating. */

static void test_history_invalid_before_first_update(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  assert(!system.fit_history.valid);
  // Generation starts nonzero, or a zeroed history would compare equal to it.
  assert(system.enable_generation != 0u);
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_disable_reenable_bumps_generation(void) {
  VkrShadowSystem system = {0};
  VkrShadowSystem reference = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  assert(vkr_shadow_system_init(&reference, test_frontend(), &config));

  const VkrCamera initial_camera = test_camera();
  VkrCamera moved_camera = initial_camera;
  moved_camera.position = vec3_new(14.0f, 5.0f, -9.0f);

  const Vec3 light = vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f));
  vkr_shadow_system_update(&reference, &moved_camera, true_v, light, NULL);

  vkr_shadow_system_update(&system, &initial_camera, true_v, light, NULL);
  assert(system.fit_history.valid);
  const uint64_t generation_while_on = system.enable_generation;
  assert(system.fit_history.enable_generation == generation_while_on);

  // Disabling is the edge that creates the discontinuity, so the bump lands
  // there; the stored history is now stamped with a stale generation.
  vkr_shadow_system_update(&system, &initial_camera, false_v, light, NULL);
  assert(system.enable_generation == generation_while_on + 1u);
  assert(system.fit_history.enable_generation != system.enable_generation);

  /* If the generation check is removed, these stale values all sit inside a
     deadband and visibly contaminate the next fit. */
  poison_history_inside_deadbands(&system.fit_history, &reference.fit_history,
                                  config.cascade_count);

  // Re-enabling must refuse the pre-gap fit and restamp with the new
  // generation.
  vkr_shadow_system_update(&system, &moved_camera, true_v, light, NULL);
  assert(system.fit_history.valid);
  assert(system.fit_history.enable_generation == system.enable_generation);
  for (uint32_t i = 0u; i < config.cascade_count; ++i) {
    assert_fit_equal(&system.fit_history.cascades[i],
                     &reference.fit_history.cascades[i]);
  }

  vkr_shadow_system_shutdown(&system, test_frontend());
  vkr_shadow_system_shutdown(&reference, test_frontend());
}

static void test_light_direction_change_invalidates_history(void) {
  VkrShadowSystem system = {0};
  VkrShadowSystem reference = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  assert(vkr_shadow_system_init(&reference, test_frontend(), &config));

  const VkrCamera camera = test_camera();

  const Vec3 light_a = vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f));
  const Vec3 light_b = vec3_normalize(vec3_new(0.7f, -1.0f, 0.2f));
  vkr_shadow_system_update(&reference, &camera, true_v, light_b, NULL);

  vkr_shadow_system_update(&system, &camera, true_v, light_a, NULL);
  assert(system.fit_history.valid);

  poison_history_inside_deadbands(&system.fit_history, &reference.fit_history,
                                  config.cascade_count);

  // A stored fit framed by a different light basis is not a previous value of
  // the same quantity; the history must restamp rather than blend.
  vkr_shadow_system_update(&system, &camera, true_v, light_b, NULL);
  assert(system.fit_history.valid);
  assert(system.fit_history.light_direction.x == light_b.x);
  assert(system.fit_history.light_direction.y == light_b.y);
  assert(system.fit_history.light_direction.z == light_b.z);
  for (uint32_t i = 0u; i < config.cascade_count; ++i) {
    assert_fit_equal(&system.fit_history.cascades[i],
                     &reference.fit_history.cascades[i]);
  }

  vkr_shadow_system_shutdown(&system, test_frontend());
  vkr_shadow_system_shutdown(&reference, test_frontend());
}

static void test_explicit_invalidation_clears_history(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));

  const VkrCamera camera = test_camera();

  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f)), NULL);
  assert(system.fit_history.valid);

  // Scene replacement and target recreation leave every stamp identical while
  // making the stored fit meaningless, so they need the explicit call.
  vkr_shadow_system_invalidate_fit_history(&system);
  assert(!system.fit_history.valid);

  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_disabled_stabilization_does_not_publish_history(void) {
  VkrShadowSystem system = {0};
  VkrShadowSystem reference = {0};
  VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  config.stabilize_cascades = false_v;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));

  const VkrShadowConfig stable_config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&reference, test_frontend(), &stable_config));
  const VkrCamera camera = test_camera();
  const Vec3 light = vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f));

  vkr_shadow_system_update(&system, &camera, true_v, light, NULL);
  assert(!system.fit_history.valid);

  system.config.stabilize_cascades = true_v;
  vkr_shadow_system_update(&system, &camera, true_v, light, NULL);
  vkr_shadow_system_update(&reference, &camera, true_v, light, NULL);
  assert(system.fit_history.valid);
  for (uint32_t i = 0u; i < stable_config.cascade_count; ++i) {
    assert_fit_equal(&system.fit_history.cascades[i],
                     &reference.fit_history.cascades[i]);
  }

  vkr_shadow_system_shutdown(&system, test_frontend());
  vkr_shadow_system_shutdown(&reference, test_frontend());
}

static void test_frame_data_carries_only_consumed_fields(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));

  const VkrCamera camera = test_camera();

  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f)), NULL);

  VkrShadowFrameData frame = {0};
  vkr_shadow_system_get_frame_data(&system, 0u, &frame);
  assert(frame.enabled);
  assert(frame.cascade_count == config.cascade_count);
  for (uint32_t i = 0; i < config.cascade_count; ++i) {
    assert(frame.split_far[i] > 0.0f);
  }
  // Inactive cascade fields stay zero rather than retaining stale data.
  for (uint32_t i = config.cascade_count; i < VKR_SHADOW_CASCADE_COUNT_MAX;
       ++i) {
    assert(frame.split_far[i] == 0.0f);
  }

  // The fitted depth span is retained for the phases that need it, and must be
  // a real interval rather than the zero a dropped field would leave.
  for (uint32_t i = 0; i < config.cascade_count; ++i) {
    assert(system.cascades[i].light_space_depth_span > 0.0f);
    assert(system.cascades[i].world_units_per_texel > 0.0f);
  }

  vkr_shadow_system_update(&system, &camera, false_v,
                           vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f)), NULL);
  frame = (VkrShadowFrameData){0};
  vkr_shadow_system_get_frame_data(&system, 0u, &frame);
  assert(!frame.enabled);
  assert(frame.cascade_count == 0u);

  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_shadow_raster_bias_packet_validation(void) {
  VkrShadowConfigOverride bias = {
      .depth_bias_constant = 1.25f,
      .depth_bias_slope = 1.75f,
      .depth_bias_clamp = 0.0f,
  };
  VkrShadowPassPayload shadow = {
      .cascade_count = 1u,
      .config_override = &bias,
  };
  const VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .shadow = &shadow,
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  bias.depth_bias_constant = NAN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.config_override.depth_bias_constant") == 0);

  bias.depth_bias_constant = 1.25f;
  bias.depth_bias_slope = -0.01f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.config_override.depth_bias_slope") == 0);

  bias.depth_bias_slope = 1.75f;
  bias.depth_bias_clamp = INFINITY;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.config_override.depth_bias_clamp") == 0);
}

/* Mobility partition and generations (spec section 6.1-6.2). These exist to
   serve the cascade-reuse rule, not to speed anything up: reuse compares the
   generations a layer was rendered with, and scans only the dynamic span. */

static void test_mobility_defaults_to_dynamic(void) {
  /* The safe classification must be the one a caller gets by saying nothing.
     A zeroed instance is DYNAMIC, so an unclassified caster forces a render
     rather than silently qualifying a cascade for reuse. */
  VkrMeshInstance instance = {0};
  VkrMesh mesh = {0};
  assert(instance.shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);
  assert(mesh.shadow_mobility == VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);
  assert(VKR_SHADOW_CASTER_MOBILITY_DYNAMIC == 0);
}

static void test_generations_never_start_at_zero(void) {
  /* A consumer record is zeroed before its first use. If a live generation
     could also be zero, that record would compare equal and be read as
     "unchanged since I last rendered", authorizing reuse of contents that were
     never produced. */
  VkrMeshManagerGenerations zeroed = {0};
  assert(zeroed.topology == 0u);
  assert(zeroed.static_content == 0u);
  assert(zeroed.dynamic_content == 0u);
  assert(zeroed.caster_bounds == 0u);
}

static VkrWorldPassPayload retained_static_payload(void) {
  return (VkrWorldPassPayload){
      .static_generation = 7u,
      .dynamic_generation = 11u,
      .caster_bounds_generation = 13u,
  };
}

static uint32_t cascade_mask(const VkrShadowSystem *system) {
  return (UINT32_C(1) << system->config.cascade_count) - 1u;
}

static void update_for_reuse(VkrShadowSystem *system, VkrCamera *camera) {
  vkr_shadow_system_update(system, camera, true_v,
                           vec3_normalize(vec3_new(-0.4f, -1.0f, -0.3f)), NULL);
}

static void prime_retained_history(VkrShadowSystem *system, VkrCamera *camera,
                                   uint32_t image_index,
                                   VkrWorldPassPayload *payload) {
  update_for_reuse(system, camera);
  VkrShadowFrameData frame = {0};
  vkr_shadow_system_resolve_frame(
      system, image_index, (VkrRetainedShadowToken){.resource_generation = 3u},
      payload, VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(system));
  vkr_shadow_system_commit_frame(system, 17u);
}

static void
test_retained_history_reuses_per_image_and_commits_only_on_submit(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);

  update_for_reuse(&system, &camera);
  VkrShadowFrameData frame = {0};
  const VkrRetainedShadowToken valid = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == 0u);
  for (uint32_t i = 0u; i < config.cascade_count; ++i)
    assert(frame.reused[i] == 1u && frame.rendered[i] == 0u);

  vkr_shadow_system_resolve_frame(&system, 1u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  assert(system.cascade_history[1][0].last_submit_value == 0u);

  vkr_shadow_system_resolve_frame(&system, 1u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  vkr_shadow_system_commit_frame(&system, 19u);
  assert(system.cascade_history[1][0].last_submit_value == 19u);
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void
test_retained_history_guard_contains_small_motion_not_large_motion(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);
  const VkrRetainedShadowToken valid = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };

  camera.position.x += 0.1f;
  update_for_reuse(&system, &camera);
  VkrShadowFrameData frame = {0};
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask != cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);

  camera.position.x += 2000.0f;
  update_for_reuse(&system, &camera);
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static VkrWorldDrawCandidate
dynamic_candidate_at_history(const VkrShadowCascadeHistory *history,
                             bool8_t bounds_valid) {
  const Vec4 light_center = {
      history->rendered_fit.center_x, history->rendered_fit.center_y,
      (history->rendered_fit.min_z + history->rendered_fit.max_z) * 0.5f, 1.0f};
  const Vec4 world = mat4_mul_vec4(
      mat4_inverse_rigid(history->rendered_light_view), light_center);
  return (VkrWorldDrawCandidate){
      .instance = {.model = mat4_identity()},
      .local_bounding_sphere = {world.x, world.y, world.z, 1.0f},
      .flags = bounds_valid ? VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID : 0u,
  };
}

static void test_dynamic_overlap_and_publication_fail_closed(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);
  const VkrRetainedShadowToken valid = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };
  update_for_reuse(&system, &camera);

  VkrWorldDrawCandidate dynamic =
      dynamic_candidate_at_history(&system.cascade_history[0][0], true_v);
  payload.gpu_candidates = &dynamic;
  payload.gpu_shadow_candidate_count = 1u;
  payload.static_candidate_count = 0u;
  VkrShadowFrameData frame = {0};
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert((frame.cascade_render_mask & 1u) != 0u);
  assert(frame.dynamic_forced[0] == 1u);
  vkr_shadow_system_discard_frame(&system);

  dynamic.local_bounding_sphere =
      vec4_new(100000.0f, 100000.0f, 100000.0f, 1.0f);
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == 0u);
  for (uint32_t i = 0u; i < config.cascade_count; ++i) {
    assert(frame.dynamic_candidates_tested[i] == 1u);
    assert(frame.dynamic_forced[i] == 0u);
  }
  vkr_shadow_system_discard_frame(&system);

  system.config.reuse_dynamic_scan_budget = 0u;
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  for (uint32_t i = 0u; i < config.cascade_count; ++i)
    assert(frame.dynamic_forced[i] == 1u);
  vkr_shadow_system_discard_frame(&system);
  system.config.reuse_dynamic_scan_budget =
      VKR_SHADOW_DYNAMIC_SCAN_BUDGET_DEFAULT;

  dynamic.flags = 0u;
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  for (uint32_t i = 0u; i < config.cascade_count; ++i)
    assert(frame.dynamic_forced[i] == 1u);
  vkr_shadow_system_discard_frame(&system);

  payload.gpu_shadow_candidate_count = 0u;
  payload.publication_pending = true_v;
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void
test_retained_history_signatures_and_invalidation_fail_closed(void) {
  VkrShadowSystem system = {0};
  VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);
  update_for_reuse(&system, &camera);
  VkrShadowFrameData frame = {0};
  VkrRetainedShadowToken token = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };

  token.valid_layer_mask &= ~UINT32_C(1);
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == UINT32_C(1));
  vkr_shadow_system_discard_frame(&system);
  token.valid_layer_mask = cascade_mask(&system);

  payload.static_generation++;
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  payload.static_generation--;

  payload.caster_bounds_generation++;
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  payload.caster_bounds_generation--;

  token.resource_generation++;
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  token.resource_generation--;

  system.config.depth_bias_constant_factor += 1.0f;
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  system.config.depth_bias_constant_factor = config.depth_bias_constant_factor;

  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize(vec3_new(0.7f, -1.0f, 0.2f)), NULL);
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);

  vkr_shadow_system_invalidate_fit_history(&system);
  assert(system.cascade_history[0][0].last_submit_value == 0u);
  update_for_reuse(&system, &camera);
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_stale_dynamic_contents_render_once_after_caster_leaves(void) {
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  update_for_reuse(&system, &camera);
  VkrWorldDrawCandidate dynamic = {
      .instance = {.model = mat4_identity()},
      .local_bounding_sphere = {0.0f, 2.0f, -10.0f, 10000.0f},
      .flags = VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID,
  };
  payload.gpu_candidates = &dynamic;
  payload.gpu_shadow_candidate_count = 1u;
  VkrShadowFrameData frame = {0};
  const VkrRetainedShadowToken token = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  vkr_shadow_system_commit_frame(&system, 1u);
  assert(!system.cascade_history[0][0].static_only_contents);

  payload.gpu_candidates = NULL;
  payload.gpu_shadow_candidate_count = 0u;
  update_for_reuse(&system, &camera);
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_commit_frame(&system, 2u);
  assert(system.cascade_history[0][0].static_only_contents);

  update_for_reuse(&system, &camera);
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == 0u);
  vkr_shadow_system_shutdown(&system, test_frontend());
}

bool32_t run_shadow_system_tests(void) {
  printf("--- Starting Shadow System Tests ---\n");
  printf("  Running test_growth_is_never_deadbanded...\n");
  test_growth_is_never_deadbanded();
  printf("  test_growth_is_never_deadbanded PASSED\n");
  printf("  Running test_extent_shrink_inside_deadband_is_held...\n");
  test_extent_shrink_inside_deadband_is_held();
  printf("  test_extent_shrink_inside_deadband_is_held PASSED\n");
  printf("  Running test_extent_shrink_beyond_deadband_is_taken...\n");
  test_extent_shrink_beyond_deadband_is_taken();
  printf("  test_extent_shrink_beyond_deadband_is_taken PASSED\n");
  printf("  Running test_center_within_one_texel_keeps_previous...\n");
  test_center_within_one_texel_keeps_previous();
  printf("  test_center_within_one_texel_keeps_previous PASSED\n");
  printf("  Running test_center_beyond_one_texel_moves...\n");
  test_center_beyond_one_texel_moves();
  printf("  test_center_beyond_one_texel_moves PASSED\n");
  printf("  Running test_depth_shrink_deadbands_each_bound_independently...\n");
  test_depth_shrink_deadbands_each_bound_independently();
  printf("  test_depth_shrink_deadbands_each_bound_independently PASSED\n");
  printf("  Running test_quantize_extent_grows_and_is_bounded...\n");
  test_quantize_extent_grows_and_is_bounded();
  printf("  test_quantize_extent_grows_and_is_bounded PASSED\n");
  printf("  Running test_quantize_extent_is_stable_under_small_change...\n");
  test_quantize_extent_is_stable_under_small_change();
  printf("  test_quantize_extent_is_stable_under_small_change PASSED\n");
  printf("  Running test_history_invalid_before_first_update...\n");
  test_history_invalid_before_first_update();
  printf("  test_history_invalid_before_first_update PASSED\n");
  printf("  Running test_disable_reenable_bumps_generation...\n");
  test_disable_reenable_bumps_generation();
  printf("  test_disable_reenable_bumps_generation PASSED\n");
  printf("  Running test_light_direction_change_invalidates_history...\n");
  test_light_direction_change_invalidates_history();
  printf("  test_light_direction_change_invalidates_history PASSED\n");
  printf("  Running test_explicit_invalidation_clears_history...\n");
  test_explicit_invalidation_clears_history();
  printf("  test_explicit_invalidation_clears_history PASSED\n");
  printf("  Running test_disabled_stabilization_does_not_publish_history...\n");
  test_disabled_stabilization_does_not_publish_history();
  printf("  test_disabled_stabilization_does_not_publish_history PASSED\n");
  printf("  Running test_mobility_defaults_to_dynamic...\n");
  test_mobility_defaults_to_dynamic();
  printf("  test_mobility_defaults_to_dynamic PASSED\n");
  printf("  Running test_generations_never_start_at_zero...\n");
  test_generations_never_start_at_zero();
  printf("  test_generations_never_start_at_zero PASSED\n");
  printf("  Running retained cascade reuse tests...\n");
  test_retained_history_reuses_per_image_and_commits_only_on_submit();
  test_retained_history_guard_contains_small_motion_not_large_motion();
  test_dynamic_overlap_and_publication_fail_closed();
  test_retained_history_signatures_and_invalidation_fail_closed();
  test_stale_dynamic_contents_render_once_after_caster_leaves();
  printf("  retained cascade reuse tests PASSED\n");
  printf("  Running test_frame_data_carries_only_consumed_fields...\n");
  test_frame_data_carries_only_consumed_fields();
  printf("  test_frame_data_carries_only_consumed_fields PASSED\n");
  printf("  Running test_shadow_raster_bias_packet_validation...\n");
  test_shadow_raster_bias_packet_validation();
  printf("  test_shadow_raster_bias_packet_validation PASSED\n");
  printf("--- Shadow System Tests Completed ---\n");
  return true_v;
}
