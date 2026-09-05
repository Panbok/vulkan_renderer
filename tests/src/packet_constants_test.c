#include "packet_constants_test.h"

#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_packet_constants.h"

#include <assert.h>
#include <stdio.h>

static void test_packet_frame_constants(void) {
  printf("  Running test_packet_frame_constants...\n");
  VkrPointLightGrid grid = {
      .origin = {1.0f, 2.0f, 3.0f},
      .cell_size = 4.0f,
      .dimensions = {5u, 6u, 7u},
      .cell_count = 8u,
      .global_mask = {.words = {9u, 10u, 11u, 12u}},
  };
  const VkrFrameLighting lighting = {
      .directional_enabled = true_v,
      .directional_direction = {0.25f, 0.5f, 0.75f},
      .directional_color = {0.1f, 0.2f, 0.3f},
      .directional_intensity = 2.0f,
      .ibl_intensity = 1.5f,
      .ibl_diffuse_intensity = 1.25f,
      .ibl_specular_intensity = 0.75f,
      .point_light_count = 3u,
      .point_light_grid = &grid,
  };
  const VkrShadowPassPayload shadow = {
      .cascade_count = 4u,
      .receiver =
          {
              .receiver_bias_texels = 1.0f,
              .slope_bias_texels = 2.0f,
              .normal_offset_texels = 0.5f,
              .pcf_radius_texels = 1.5f,
              .pcf_sample_count = 16u,
              .pcf_uniform_early_out = true_v,
              .cascade_blend_fraction = 0.08f,
              .fade_start = 180.0f,
              .fade_end = 200.0f,
          },
  };
  Mat4 view = mat4_identity();
  view.elements[12] = 13.0f;
  const VkrPreparedFrame packet = {
      .input = {
          .globals =
              {
                  .view = view,
                  .view_position = {14.0f, 15.0f, 16.0f},
                  .ambient_color = {0.4f, 0.5f, 0.6f, 1.0f},
                  .render_mode = 17u,
              },
          .lighting = &lighting,
          .shadow = &shadow,
          .debug = &(const VkrGpuDebugPayload){.shadow_debug_mode = 3u},
      }};

  const VkrPacketFrameConstants constants =
      vkr_packet_derive_frame_constants(&packet, 800u, 400u);
  assert(constants.view_position.x == 14.0f &&
         constants.view_position.y == 15.0f &&
         constants.view_position.z == 16.0f &&
         constants.view_position.w == 1.0f);
  assert(constants.ibl_controls.x == 1.5f &&
         constants.ibl_controls.y == 1.25f &&
         constants.ibl_controls.z == 0.75f &&
         constants.ibl_controls.w == 1.0f / 800.0f);
  assert(constants.directional_direction_enabled.w == 1.0f);
  assert(constants.directional_color_intensity.w == 2.0f);
  assert(constants.ambient_color.x == 0.4f &&
         constants.ambient_color.w == 1.0f / 400.0f);
  assert(constants.point_light_grid_origin_cell_size.x == 1.0f &&
         constants.point_light_grid_origin_cell_size.w == 4.0f);
  assert(constants.point_light_grid_dimensions_count[0] == 5u &&
         constants.point_light_grid_dimensions_count[1] == 6u &&
         constants.point_light_grid_dimensions_count[2] == 7u &&
         constants.point_light_grid_dimensions_count[3] == 8u);
  assert(constants.point_light_global_mask.words[0] == 9u &&
         constants.point_light_global_mask.words[3] == 12u);
  assert(constants.point_light_count == 3u && constants.render_mode == 17u &&
         constants.shadow_debug_mode == 3u &&
         constants.prefilter_mip_count == VKR_IBL_PREFILTER_MIP_COUNT &&
         constants.shadow_cascade_count == 4u);
  /* The receiver block is forwarded verbatim: normalization is the shadow
     system's job at its cold boundary, not this derivation's. */
  assert(MemCompare(&constants.shadow_receiver, &shadow.receiver,
                    sizeof(shadow.receiver)) == 0);
  assert(MemCompare(&constants.view, &view, sizeof(view)) == 0);

  const VkrPreparedFrame unlit = {.input = {0}};
  const VkrPacketFrameConstants defaults =
      vkr_packet_derive_frame_constants(&unlit, 0u, 0u);
  assert(defaults.ibl_controls.x == 1.0f && defaults.ibl_controls.y == 1.0f &&
         defaults.ibl_controls.z == 1.0f && defaults.ibl_controls.w == 1.0f);
  assert(defaults.ambient_color.w == 1.0f && defaults.point_light_count == 0u &&
         defaults.shadow_debug_mode == 0u);
  /* No shadow payload leaves the block zeroed, and a zero tap count is the
     receiver's own "no cascades" signal. */
  assert(defaults.shadow_cascade_count == 0u &&
         defaults.shadow_receiver.pcf_sample_count == 0u);
  printf("  test_packet_frame_constants PASSED\n");
}

static void test_packet_material_constants(void) {
  printf("  Running test_packet_material_constants...\n");
  const VkrPbrProperties pbr = {
      .metallic = 0.1f,
      .roughness = 0.2f,
      .normal_scale = 0.3f,
      .occlusion_strength = 0.4f,
      .emissive_factor = {0.5f, 0.6f, 0.7f},
      .dielectric_specular = {0.8f, 0.9f, 1.0f},
      .transmission_factor = 0.25f,
      .ior = 1.5f,
      .thickness_factor = 0.75f,
      .attenuation_color = {0.11f, 0.22f, 0.33f},
      .attenuation_distance = 4.5f,
  };
  const VkrPacketMaterialConstants constants =
      vkr_packet_derive_material_constants(&pbr, 0.42f,
                                           VKR_MATERIAL_ALPHA_CUTOUT);
  assert(constants.emissive.x == 0.5f && constants.emissive.w == 0.0f);
  assert(constants.dielectric_specular.x == 0.8f &&
         constants.dielectric_specular.w == 0.0f);
  assert(constants.surface.x == 0.1f && constants.surface.y == 0.2f &&
         constants.surface.z == 0.3f && constants.surface.w == 0.4f);
  assert(constants.alpha.x == 0.42f && constants.alpha.y == 0.25f &&
         constants.alpha.z == 1.5f && constants.alpha.w == 0.75f);
  assert(constants.attenuation_color.x == 0.11f &&
         constants.attenuation_color.w == 4.5f);
  assert(constants.alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT);
  printf("  test_packet_material_constants PASSED\n");
}

static void test_packet_frame_flags(void) {
  printf("  Running test_packet_frame_flags...\n");
  VkrFrameLighting lighting = {.ibl_enabled = true_v};
  VkrPreparedFrame packet = {.input = {.lighting = &lighting}};

  assert(vkr_packet_derive_frame_flags(&packet, true_v, true_v) ==
         (VKR_PACKET_FRAME_FLAG_LIGHTING | VKR_PACKET_FRAME_FLAG_IBL));
  assert(vkr_packet_derive_frame_flags(&packet, true_v, false_v) ==
         VKR_PACKET_FRAME_FLAG_LIGHTING);
  assert(vkr_packet_derive_frame_flags(&packet, false_v, true_v) == 0u);
  lighting.ibl_enabled = false_v;
  assert(vkr_packet_derive_frame_flags(&packet, true_v, true_v) ==
         VKR_PACKET_FRAME_FLAG_LIGHTING);
  assert(vkr_packet_derive_frame_flags(NULL, true_v, true_v) ==
         VKR_PACKET_FRAME_FLAG_LIGHTING);
  printf("  test_packet_frame_flags PASSED\n");
}

bool32_t run_packet_constants_tests(void) {
  printf("Running packet constants tests...\n");
  test_packet_frame_constants();
  test_packet_material_constants();
  test_packet_frame_flags();
  printf("Packet constants tests PASSED\n");
  return true_v;
}
