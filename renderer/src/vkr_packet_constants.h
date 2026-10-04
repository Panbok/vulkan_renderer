#pragma once

#include "math/mat.h"
#include "math/vec.h"
#include "vkr_lighting.h"
#include "vkr_prepared_frame.h"

/** @file Shared frame-flag and immutable-material derivation. */

typedef enum VkrPacketFrameFlag {
  VKR_PACKET_FRAME_FLAG_LIGHTING = 1u << 0u,
  VKR_PACKET_FRAME_FLAG_IBL = 1u << 1u,
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
  /** Scale applied to inputs that cannot be lowered pre-exposed: material
      emissive, the baked diffuse volume and the no-sky background. */
  float32_t pre_exposure;
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
vkr_packet_derive_frame_constants(const VkrPreparedFrame *packet,
                                  uint32_t target_width,
                                  uint32_t target_height);

/** Scales the colour of copied world text vertices by the frame's
 * pre-exposure; alpha is coverage and stays unscaled. */
void vkr_packet_pre_expose_text_vertices(VkrTextVertex *vertices,
                                         uint32_t vertex_count,
                                         float32_t pre_exposure);

/** Derives the shared lighting and IBL flags. */
uint32_t vkr_packet_derive_frame_flags(const VkrPreparedFrame *packet,
                                       bool8_t lighting_pass,
                                       bool8_t ibl_resources_ready);

/** Publication-time lowering into an immutable backend GPU material row. */
VkrPacketMaterialConstants
vkr_packet_derive_material_constants(const VkrPbrProperties *pbr,
                                     float32_t alpha_cutoff,
                                     VkrMaterialAlphaMode alpha_mode);

/* A terrain layer's surface row: x metallic, y roughness, z normal scale,
   w occlusion strength, unclamped like the common row's surface. */
Vec4 vkr_packet_terrain_layer_surface(const VkrMaterialLayer *layer);

typedef struct VkrRenderGraphFrameInfo VkrRenderGraphFrameInfo;

/* The LOD policy of each culling view in their order: the camera, the
   cascades, then the opaque and transmitting local faces (ADR-084). */
void vkr_packet_write_lod_views(const VkrPreparedFrame *packet,
                                const VkrRenderGraphFrameInfo *frame,
                                VkrGpuLodView *out_views, uint32_t view_count);
