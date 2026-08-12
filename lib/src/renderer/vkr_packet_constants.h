#pragma once

#include "math/mat.h"
#include "math/vec.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/vkr_render_packet.h"

/**
 * @file vkr_packet_constants.h
 * @brief Backend-neutral derivation of the per-frame and per-material values a
 * packet draw root carries.
 *
 * Both selected implementations lower a render packet into a per-draw root
 * record. The record *layouts* differ — Metal carries 64-bit resource IDs,
 * Vulkan carries 32-bit descriptor-heap indices — but the arithmetic that
 * produces the scalar and vector fields is identical, and it used to be written
 * out twice. A separate environment-BRDF divergence exposed how easily the two
 * lowering paths could evolve independently; the BRDF implementation itself is
 * shader-owned and is not part of these constants.
 *
 * These functions are the single derivation. Each backend computes the frame
 * block once per pass, derives the material block for each draw, and copies the
 * results into its own root layout. A change to these shared lighting or
 * material values therefore lands on both backends or neither.
 *
 * Deliberately **not** shared here, because the two backends genuinely differ:
 * resource references (IDs versus heap indices), buffer device addresses, and
 * the IBL-ready predicate that drives `flags` / `material_flags`.
 */

/** Values constant across every draw in a pass. */
typedef struct VkrPacketFrameConstants {
  Vec4 view_position;
  /** xyz intensities; w is the reciprocal of the resolved target width. */
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  /** xyz ambient; w is the reciprocal of the resolved target height. */
  Vec4 ambient_color;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t render_mode;
  uint32_t prefilter_mip_count;
  uint32_t shadow_cascade_count;
  float32_t shadow_bias;
  Mat4 view;
} VkrPacketFrameConstants;

/** Values constant across every draw that uses one material. */
typedef struct VkrPacketMaterialConstants {
  Vec4 emissive;
  /** xyz dielectric specular; w flags the transmission pass. */
  Vec4 dielectric_specular;
  /** metallic, roughness, normal scale, occlusion strength. */
  Vec4 surface;
  /** alpha cutoff, transmission factor, IOR, thickness factor. */
  Vec4 alpha;
  /** xyz attenuation colour; w attenuation distance. */
  Vec4 attenuation_color;
  uint32_t alpha_mode;
} VkrPacketMaterialConstants;

/**
 * @param target_width  Resolved pass width in pixels. Zero is defensively
 *                      treated as one. The caller resolves
 *                      viewport-versus-window itself, because the two backends
 *                      resolve it at different points.
 * @param target_height Resolved pass height in pixels. Zero is defensively
 *                      treated as one.
 */
VkrPacketFrameConstants
vkr_packet_derive_frame_constants(const VkrRenderPacket *packet,
                                  uint32_t target_width,
                                  uint32_t target_height);

/** Hot-path material lowering; force-inlined because Release does not use LTO.
 */
vkr_internal INLINE VkrPacketMaterialConstants
vkr_packet_derive_material_constants(const VkrPbrProperties *pbr,
                                     float32_t alpha_cutoff,
                                     VkrMaterialAlphaMode alpha_mode,
                                     bool8_t transmission_pass) {
  if (!pbr)
    return (VkrPacketMaterialConstants){0};
  return (VkrPacketMaterialConstants){
      .emissive = {pbr->emissive_factor.x, pbr->emissive_factor.y,
                   pbr->emissive_factor.z, 0.0f},
      .dielectric_specular = {pbr->dielectric_specular.x,
                              pbr->dielectric_specular.y,
                              pbr->dielectric_specular.z,
                              transmission_pass ? 1.0f : 0.0f},
      .surface = {pbr->metallic, pbr->roughness, pbr->normal_scale,
                  pbr->occlusion_strength},
      .alpha = {alpha_cutoff, pbr->transmission_factor, pbr->ior,
                pbr->thickness_factor},
      .attenuation_color = {pbr->attenuation_color.x, pbr->attenuation_color.y,
                            pbr->attenuation_color.z,
                            pbr->attenuation_distance},
      .alpha_mode = (uint32_t)alpha_mode,
  };
}
