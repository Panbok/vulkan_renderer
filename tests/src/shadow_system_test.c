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

/* A payload the receiver can trust: one cascade with a real slice, texel size,
   and depth span, and a supported tap count. Tests mutate one field at a time
   from this baseline so a rejection names the field under test. */
static VkrShadowPassPayload test_shadow_valid_payload(void) {
  VkrShadowPassPayload shadow = {
      .cascade_count = 1u,
      .receiver =
          {
              .receiver_bias_texels = 1.0f,
              .slope_bias_texels = 2.0f,
              .normal_offset_texels = 1.0f,
              .pcf_radius_texels = 1.5f,
              .pcf_sample_count = 16u,
              .pcf_uniform_early_out = true_v,
              .cascade_blend_fraction = 0.08f,
              .fade_start = 180.0f,
              .fade_end = 200.0f,
          },
  };
  shadow.cascades[0].light_view_projection = mat4_identity();
  shadow.cascades[0].split_near_far_texel_depth =
      (Vec4){0.1f, 200.0f, 0.01f, 120.0f};
  shadow.cascades[0].origin_inv_size_pad =
      (Vec4){3.0f, 4.0f, 1.0f / 2048.0f, 0.0f};
  return shadow;
}

static void test_shadow_raster_bias_packet_validation(void) {
  VkrShadowConfigOverride bias = {
      .depth_bias_constant = 1.25f,
      .depth_bias_slope = 1.75f,
      .depth_bias_clamp = 0.0f,
  };
  VkrShadowPassPayload shadow = test_shadow_valid_payload();
  shadow.config_override = &bias;
  const VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
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

/* Receiver quality (spec section 11). The shader hot path has no recovery
   branch, so each of these is the only thing standing between a bad
   configuration and an out-of-range table index or a divide by zero. */

static void test_shadow_receiver_packet_validation(void) {
  VkrShadowPassPayload shadow = test_shadow_valid_payload();
  const VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .shadow = &shadow,
  };
  VkrValidationError validation = {0};
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  /* Every supported tap count passes; the one immediately outside each does
     not. A rejected count would otherwise index the 64-entry table freely. */
  const uint32_t supported[] = {1u, 4u, 9u, 16u, 32u};
  for (uint32_t i = 0; i < ArrayCount(supported); ++i) {
    shadow.receiver.pcf_sample_count = supported[i];
    assert(vkr_renderer_validate_packet(&packet, &validation) ==
           VKR_RENDERER_ERROR_NONE);
  }
  const uint32_t rejected[] = {0u, 2u, 8u, 17u, 64u, 65u};
  for (uint32_t i = 0; i < ArrayCount(rejected); ++i) {
    shadow.receiver.pcf_sample_count = rejected[i];
    assert(vkr_renderer_validate_packet(&packet, &validation) ==
           VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
    assert(strcmp(validation.field_path,
                  "packet.shadow.receiver.pcf_sample_count") == 0);
  }
  shadow.receiver.pcf_sample_count = 16u;

  shadow.receiver.pcf_uniform_early_out = 2u;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.receiver.pcf_uniform_early_out") == 0);
  shadow.receiver.pcf_uniform_early_out = true_v;

  shadow.receiver.pcf_radius_texels = -0.1f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.receiver.pcf_radius_texels") == 0);
  shadow.receiver.pcf_radius_texels = 1.5f;

  shadow.receiver.slope_bias_texels = NAN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.shadow.receiver") == 0);
  shadow.receiver.slope_bias_texels = 2.0f;

  /* Above 0.5 the band would consume more than half a cascade's span and the
     fade would reach back past the previous split. */
  shadow.receiver.cascade_blend_fraction = 0.51f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.receiver.cascade_blend_fraction") == 0);
  shadow.receiver.cascade_blend_fraction = 0.08f;

  shadow.receiver.fade_end = shadow.receiver.fade_start - 1.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.shadow.receiver.fade_end") == 0);
  shadow.receiver.fade_end = 200.0f;

  shadow.receiver.fade_end = 201.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.shadow.receiver.fade_end") == 0);
  shadow.receiver.fade_end = 200.0f;

  /* A zero depth span is the receiver's bias divisor. Rejecting it here is why
     the shader can divide unconditionally. */
  shadow.cascades[0].split_near_far_texel_depth.w = 0.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.cascades.split_near_far_texel_depth") == 0);
  shadow.cascades[0].split_near_far_texel_depth.w = 120.0f;

  shadow.cascades[0].origin_inv_size_pad.x = NAN;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.cascades.origin_inv_size_pad") == 0);
  shadow.cascades[0].origin_inv_size_pad.x = 3.0f;

  shadow.cascades[0].origin_inv_size_pad.z = 0.0f;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.cascades.origin_inv_size_pad") == 0);
  shadow.cascades[0].origin_inv_size_pad.z = 1.0f / 2048.0f;

  shadow.cascades[0].split_near_far_texel_depth.y =
      shadow.cascades[0].split_near_far_texel_depth.x;
  assert(vkr_renderer_validate_packet(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path,
                "packet.shadow.cascades.split_near_far_texel_depth") == 0);
}

static void test_shadow_receiver_config_normalization(void) {
  /* Normalization happens once at init so nothing downstream needs a guard.
     A hostile configuration must come out of init already valid. */
  VkrShadowConfig config = VKR_SHADOW_CONFIG_HIGH;
  config.receiver_bias_texels = NAN;
  config.receiver_slope_bias_texels = -1.0f;
  config.normal_offset_texels = -INFINITY;
  config.pcf_radius_texels = NAN;
  config.pcf_sample_count = 7u;
  config.pcf_uniform_early_out = (bool8_t)7;
  config.cascade_blend_fraction = 12.0f;
  config.shadow_distance_fade_range = NAN;

  VkrShadowSystem system = {0};
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  assert(system.config.receiver_bias_texels == 0.0f);
  assert(system.config.receiver_slope_bias_texels == 0.0f);
  assert(system.config.normal_offset_texels == 0.0f);
  assert(system.config.pcf_radius_texels == 0.0f);
  /* An unsupported tap count degrades to the one-tap kernel; it never reaches
     the shader as an unvalidated index. */
  assert(system.config.pcf_sample_count == 1u);
  assert(system.config.pcf_uniform_early_out == true_v);
  assert(system.config.cascade_blend_fraction == 0.5f);
  assert(system.config.shadow_distance_fade_range == 0.0f);
  vkr_shadow_system_shutdown(&system, test_frontend());

  /* A fade range wider than the shadow distance would put fade_start behind the
     camera and fade every visible shadow. */
  config = VKR_SHADOW_CONFIG_HIGH;
  config.max_shadow_distance = 50.0f;
  config.shadow_distance_fade_range = 400.0f;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  assert(system.config.shadow_distance_fade_range == 50.0f);
  vkr_shadow_system_shutdown(&system, test_frontend());

  /* Every shipped preset must already satisfy packet validation. */
  const VkrShadowConfig presets[] = {VKR_SHADOW_CONFIG_HIGH,
                                     VKR_SHADOW_CONFIG_BALANCED};
  for (uint32_t i = 0; i < ArrayCount(presets); ++i) {
    assert(vkr_shadow_pcf_sample_count_supported(presets[i].pcf_sample_count));
    assert(presets[i].pcf_uniform_early_out == true_v);
    assert(presets[i].cascade_blend_fraction >= 0.0f &&
           presets[i].cascade_blend_fraction <= 0.5f);
    assert(presets[i].shadow_distance_fade_range <=
           presets[i].max_shadow_distance);
  }
}

static VkrWorldPassPayload retained_static_payload(void) {
  return (VkrWorldPassPayload){
      .static_generation = 7u,
      .dynamic_generation = 11u,
      .publication_generation = 17u,
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

static void test_reused_cascade_publishes_its_rendered_receiver_data(void) {
  /* A reused layer publishes the matrix it was rendered with. Its texel size,
     origin, and depth span must come from that same committed fit: pairing the
     rendered matrix with the current raw fit's divisors would misconvert every
     texel-denominated bias for that cascade and read as a bias defect. */
  VkrShadowSystem system = {0};
  const VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);

  /* Move far enough that the raw fit differs from the committed one, but not so
     far that reuse fails. */
  camera.position.x += 0.05f;
  update_for_reuse(&system, &camera);
  VkrShadowFrameData frame = {0};
  const VkrRetainedShadowToken valid = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == 0u);

  for (uint32_t i = 0u; i < config.cascade_count; ++i) {
    const VkrShadowCascadeHistory *history = &system.cascade_history[0][i];
    assert(frame.reused[i] == 1u);
    assert(MemCompare(&frame.view_projection[i],
                      &history->rendered_view_projection, sizeof(Mat4)) == 0);
    assert(frame.world_units_per_texel[i] ==
           history->rendered_fit.world_units_per_texel);
    /* The guarded fit is wider than the raw fit, so the published texel size
       must not be the raw one. Otherwise this assertion could pass by
       coincidence. */
    assert(frame.world_units_per_texel[i] !=
           system.cascades[i].world_units_per_texel);
    assert(frame.light_space_depth_span[i] > 0.0f);
    assert(frame.split_far[i] == system.cascades[i].split_far);
    assert(frame.split_far[i] > frame.split_near[i]);
  }

  /* A rendered cascade publishes its own guard-banded fit instead. */
  camera.position.x += 2000.0f;
  update_for_reuse(&system, &camera);
  vkr_shadow_system_resolve_frame(&system, 0u, valid, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  for (uint32_t i = 0u; i < config.cascade_count; ++i) {
    assert(frame.rendered[i] == 1u);
    assert(
        frame.world_units_per_texel[i] ==
        system.pending_history.cascades[i].rendered_fit.world_units_per_texel);
    assert(frame.light_space_depth_span[i] > 0.0f);
  }
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
  payload.publication_generation++;
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);
  assert(frame.cascade_render_mask == cascade_mask(&system));
  vkr_shadow_system_discard_frame(&system);
  payload.publication_generation--;

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

static void test_proactive_refresh_is_bounded_to_reusable_cascades(void) {
  VkrShadowSystem system = {0};
  VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  config.reuse_proactive_refresh_budget = 1u;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  VkrWorldPassPayload payload = retained_static_payload();
  prime_retained_history(&system, &camera, 0u, &payload);
  update_for_reuse(&system, &camera);

  const VkrRetainedShadowToken token = {
      .resource_generation = 3u,
      .valid_layer_mask = cascade_mask(&system),
  };
  VkrShadowFrameData frame = {0};
  vkr_shadow_system_resolve_frame(&system, 0u, token, &payload,
                                  VKR_TEXTURE_FORMAT_D32_SFLOAT, &frame);

  uint32_t proactive_count = 0u;
  uint32_t reused_count = 0u;
  for (uint32_t cascade = 0u; cascade < config.cascade_count; ++cascade) {
    proactive_count += frame.proactive_refreshed[cascade];
    reused_count += frame.reused[cascade];
    if (frame.proactive_refreshed[cascade]) {
      assert(frame.rendered[cascade] == 1u);
      assert(frame.correctness_forced[cascade] == 0u);
    }
  }
  assert(proactive_count == 1u);
  assert(reused_count == config.cascade_count - 1u);
  assert(frame.cascade_render_mask != 0u);
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_sdsm_uses_completed_occupied_depth_and_keeps_fixed_tail(void) {
  VkrShadowSystem system = {0};
  VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
  config.sdsm_enabled = true_v;
  config.sdsm_temporal_blend = 0.0f;
  assert(vkr_shadow_system_init(&system, test_frontend(), &config));
  VkrCamera camera = test_camera();
  camera.projection =
      mat4_perspective(vkr_to_radians(camera.zoom),
                       (float32_t)camera.cached_window_width /
                           (float32_t)camera.cached_window_height,
                       camera.near_clip, camera.far_clip);
  const float32_t a = camera.projection.elements[10];
  const float32_t b = camera.projection.elements[14];
  VkrShadowDepthRangeSample sample = {
      .min_device_z = b / 10.0f - a,
      .max_device_z = b / 100.0f - a,
      .occupied_count = 4096u,
      .projection_convention = 0u,
      .source_depth_linearize = {a, b, 0.0f, 0.0f},
      .source_near = camera.near_clip,
      .source_far = camera.far_clip,
      .source_frame_index = 8u,
      .source_projection_generation =
          vkr_shadow_projection_generation(&camera.projection),
      .source_scene_generation = 3u,
      .submit_value = 9u,
      .valid = true_v,
  };
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 10u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);

  assert(system.sdsm_status == VKR_SHADOW_SDSM_ACTIVE);
  assert(system.sdsm_source_lag == 2u);
  assert(system.sdsm_occupied_count == 4096u);
  assert(fabsf(system.cascade_splits[0] - 10.0f) < 0.01f);
  assert(fabsf(system.cascade_splits[config.cascade_count] -
               config.max_shadow_distance) < 0.01f);

  vkr_shadow_system_set_depth_range_sample(&system, &sample, 11u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_CACHED);
  assert(fabsf(system.cascade_splits[0] - 10.0f) < 0.01f);

  sample.min_device_z = b / 50.0f - a;
  sample.max_device_z = b / 60.0f - a;
  sample.source_frame_index = 10u;
  sample.submit_value = 10u;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 12u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_ACTIVE);
  assert(fabsf(system.sdsm_linear_near - 19.0f) < 0.02f);
  assert(fabsf(system.sdsm_linear_far - 91.0f) < 0.02f);

  sample.occupied_count = 0u;
  sample.source_frame_index = 12u;
  sample.submit_value = 11u;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 13u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_EMPTY);
  assert(fabsf(system.cascade_splits[0] - camera.near_clip) < 0.001f);

  sample.occupied_count = 4096u;
  sample.source_frame_index = 8u;
  sample.submit_value = 12u;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 20u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_STALE);
  assert(fabsf(system.cascade_splits[0] - camera.near_clip) < 0.001f);

  sample.source_frame_index = 20u;
  sample.source_projection_generation++;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 21u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_STALE);

  sample.source_projection_generation =
      vkr_shadow_projection_generation(&camera.projection);
  sample.source_frame_index = 21u;
  sample.submit_value = 0u;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 22u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_STALE);

  sample.min_device_z = b / config.max_shadow_distance - a;
  sample.max_device_z = sample.min_device_z;
  sample.source_frame_index = 22u;
  sample.submit_value = 13u;
  vkr_shadow_system_set_depth_range_sample(&system, &sample, 23u, 3u);
  vkr_shadow_system_update(&system, &camera, true_v,
                           vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f}), NULL);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_STALE);
  for (uint32_t cascade = 0u; cascade < config.cascade_count; ++cascade)
    assert(system.cascade_splits[cascade] <
           system.cascade_splits[cascade + 1u]);

  system.sdsm_status = VKR_SHADOW_SDSM_ACTIVE;
  system.sdsm_range_valid = true_v;
  vkr_shadow_system_invalidate_fit_history(&system);
  assert(system.sdsm_status == VKR_SHADOW_SDSM_WARMUP);
  assert(!system.sdsm_range_valid);
  assert(system.sdsm_source_lag == 0u);
  assert(system.sdsm_occupied_count == 0u);
  vkr_shadow_system_shutdown(&system, test_frontend());
}

static void test_sdsm_cached_range_rejects_scene_and_projection_changes(void) {
  for (uint32_t change = 0u; change < 2u; ++change) {
    VkrShadowSystem system = {0};
    VkrShadowConfig config = VKR_SHADOW_CONFIG_DEFAULT;
    config.sdsm_enabled = true_v;
    config.sdsm_temporal_blend = 0.0f;
    assert(vkr_shadow_system_init(&system, test_frontend(), &config));
    VkrCamera camera = test_camera();
    camera.projection = mat4_perspective(vkr_to_radians(camera.zoom), 1.0f,
                                         camera.near_clip, camera.far_clip);
    const float32_t a = camera.projection.elements[10];
    const float32_t b = camera.projection.elements[14];
    const VkrShadowDepthRangeSample sample = {
        .min_device_z = b / 10.0f - a,
        .max_device_z = b / 100.0f - a,
        .occupied_count = 4096u,
        .source_depth_linearize = {a, b, 0.0f, 0.0f},
        .source_near = camera.near_clip,
        .source_far = camera.far_clip,
        .source_frame_index = 8u,
        .source_projection_generation =
            vkr_shadow_projection_generation(&camera.projection),
        .source_scene_generation = 3u,
        .submit_value = 9u,
        .valid = true_v,
    };
    const Vec3 light = vec3_normalize((Vec3){-1.0f, -1.0f, -1.0f});
    vkr_shadow_system_set_depth_range_sample(&system, &sample, 10u, 3u);
    vkr_shadow_system_update(&system, &camera, true_v, light, NULL);
    assert(system.sdsm_status == VKR_SHADOW_SDSM_ACTIVE);

    /* A missing readback may reuse only a compatible cached sample. */
    if (change == 1u) {
      camera.projection =
          mat4_perspective(vkr_to_radians(camera.zoom + 10.0f), 1.0f,
                           camera.near_clip, camera.far_clip);
    }
    vkr_shadow_system_set_depth_range_sample(&system, NULL, 11u,
                                             change == 0u ? 4u : 3u);
    vkr_shadow_system_update(&system, &camera, true_v, light, NULL);
    assert(system.sdsm_status == VKR_SHADOW_SDSM_STALE);
    assert(!system.sdsm_range_valid);
    assert(fabsf(system.cascade_splits[0] - camera.near_clip) < 0.001f);
    vkr_shadow_system_shutdown(&system, test_frontend());
  }
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
  printf("  Running retained cascade reuse tests...\n");
  test_retained_history_reuses_per_image_and_commits_only_on_submit();
  test_retained_history_guard_contains_small_motion_not_large_motion();
  test_dynamic_overlap_and_publication_fail_closed();
  test_retained_history_signatures_and_invalidation_fail_closed();
  test_stale_dynamic_contents_render_once_after_caster_leaves();
  test_proactive_refresh_is_bounded_to_reusable_cascades();
  test_sdsm_uses_completed_occupied_depth_and_keeps_fixed_tail();
  test_sdsm_cached_range_rejects_scene_and_projection_changes();
  test_reused_cascade_publishes_its_rendered_receiver_data();
  printf("  retained cascade reuse tests PASSED\n");
  printf("  Running test_frame_data_carries_only_consumed_fields...\n");
  test_frame_data_carries_only_consumed_fields();
  printf("  test_frame_data_carries_only_consumed_fields PASSED\n");
  printf("  Running test_shadow_raster_bias_packet_validation...\n");
  test_shadow_raster_bias_packet_validation();
  printf("  test_shadow_raster_bias_packet_validation PASSED\n");
  printf("  Running test_shadow_receiver_packet_validation...\n");
  test_shadow_receiver_packet_validation();
  printf("  test_shadow_receiver_packet_validation PASSED\n");
  printf("  Running test_shadow_receiver_config_normalization...\n");
  test_shadow_receiver_config_normalization();
  printf("  test_shadow_receiver_config_normalization PASSED\n");
  printf("--- Shadow System Tests Completed ---\n");
  return true_v;
}
