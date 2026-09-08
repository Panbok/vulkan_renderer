#pragma once

#include "defines.h"
#include "math/mat.h"

#define VKR_FROXEL_FOG_BOX_COUNT_MAX 16u
#define VKR_FROXEL_FOG_LOCAL_LIGHT_COUNT_MAX 2u
#define VKR_FROXEL_FOG_CELL_PIXELS 16u
#define VKR_FROXEL_FOG_DEPTH 64u

typedef struct VkrFroxelDensityBox {
  Vec3 minimum;
  Vec3 maximum;
  float32_t density_multiplier;
} VkrFroxelDensityBox;

/** Scene-owned medium. Boxes add density relative to the global height medium. */
typedef struct VkrFroxelFogSettings {
  bool8_t enabled;
  Vec3 color;
  float32_t density;
  float32_t base_height;
  float32_t height_falloff;
  float32_t max_distance;
  uint32_t box_count;
  VkrFroxelDensityBox boxes[VKR_FROXEL_FOG_BOX_COUNT_MAX];
} VkrFroxelFogSettings;

typedef struct VkrFroxelDensityBoxGpu {
  Vec4 min_density;
  Vec4 max_reserved;
} VkrFroxelDensityBoxGpu;

/** Frame-slot storage owns this record until its last GPU use completes. */
typedef struct VkrFroxelFogGpuParams {
  Mat4 inverse_view_projection;
  Mat4 previous_view_projection;
  Mat4 previous_view;
  Vec4 color_density;
  Vec4 height_distance_phase;
  Vec4 depth_mapping;
  Vec4 temporal_clamp;
  uint32_t grid_dimensions_cell_pixels[4];
  /** Two source-light indices, selected count, active box count. */
  uint32_t selected_local_indices_count[4];
  VkrFroxelDensityBoxGpu boxes[VKR_FROXEL_FOG_BOX_COUNT_MAX];
  Mat4 current_view_projection;
  Mat4 inverse_raster_view_projection;
} VkrFroxelFogGpuParams;

_Static_assert(sizeof(VkrFroxelFogGpuParams) == 928u, "Froxel fog ABI drift");
_Static_assert(offsetof(VkrFroxelFogGpuParams, boxes) == 288u,
               "Froxel box ABI drift");
_Static_assert(offsetof(VkrFroxelFogGpuParams, current_view_projection) == 800u,
               "Froxel camera ABI drift");

VkrFroxelFogSettings vkr_froxel_fog_settings_defaults(void);
bool8_t vkr_froxel_fog_settings_valid(const VkrFroxelFogSettings *settings);
bool8_t vkr_froxel_fog_projection_valid(const VkrFroxelFogSettings *settings,
                                       Mat4 projection);
struct VkrFrameInput;
/** Cold preparation consumes validated settings and shadow payloads. */
VkrFroxelFogGpuParams vkr_froxel_fog_prepare(
    const struct VkrFrameInput *input, uint32_t width, uint32_t height);
uint64_t vkr_froxel_fog_content_signature(
    const struct VkrFrameInput *input, const VkrFroxelFogGpuParams *params);
