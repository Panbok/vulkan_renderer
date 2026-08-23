#pragma once

#include "math/mat.h"
#include "math/vec.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/vkr_render_packet.h"

/** @file Shared frame-flag and immutable-material derivation. */

typedef enum VkrPacketFrameFlag {
  VKR_PACKET_FRAME_FLAG_LIGHTING = 1u << 0u,
  VKR_PACKET_FRAME_FLAG_IBL = 1u << 1u,
  VKR_PACKET_FRAME_FLAG_TRANSMISSION = 1u << 2u,
} VkrPacketFrameFlag;

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
  uint32_t shadow_debug_mode;
  uint32_t prefilter_mip_count;
  uint32_t shadow_cascade_count;
  /** Receiver filter and bias, already normalized by the shadow system. */
  VkrShadowReceiverPacketData shadow_receiver;
  Mat4 view;
} VkrPacketFrameConstants;

/** Values constant across every draw that uses one material. */
typedef struct VkrPacketMaterialConstants {
  Vec4 emissive;
  /** xyz dielectric specular; w is reserved. */
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

/** Derives the shared lighting, IBL, and transmission pass flags. */
uint32_t vkr_packet_derive_frame_flags(const VkrRenderPacket *packet,
                                       bool8_t lighting_pass,
                                       bool8_t ibl_resources_ready,
                                       bool8_t transmission_pass);

/** Publication-time lowering into an immutable backend GPU material row. */
VkrPacketMaterialConstants
vkr_packet_derive_material_constants(const VkrPbrProperties *pbr,
                                     float32_t alpha_cutoff,
                                     VkrMaterialAlphaMode alpha_mode);
