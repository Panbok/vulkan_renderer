#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"

#define VKR_DOF_DEFAULT_FOCUS_DISTANCE 5.0f
#define VKR_DOF_DEFAULT_F_STOP 2.8f
#define VKR_DOF_SENSOR_HEIGHT_METRES 0.024f
#define VKR_DOF_MAX_RADIUS_PIXELS 16.0f
#define VKR_DOF_DISK_SAMPLE_COUNT 32u

/* Mirrors the production shader contract. The graph owns all image storage;
 * these values are borrowed only for the current submission. */
typedef struct VkrDofGpuParams {
  /* Radius scale in output pixels, focus distance, near and far in metres. */
  Vec4 lens;
  /* Canonical output UV to current raster-depth UV: scale.xy, offset.zw. */
  Vec4 depth_uv;
  uint32_t dimensions[4];
} VkrDofGpuParams;

_Static_assert(sizeof(VkrDofGpuParams) == 48u,
               "DoF parameter ABI must remain 48 bytes");

bool8_t vkr_dof_controls_valid(float32_t focus_distance, float32_t f_stop,
                               Mat4 projection);

/* Consumes validated controls and a nonzero output/internal extent. */
VkrDofGpuParams vkr_dof_prepare(float32_t focus_distance, float32_t f_stop,
                                Mat4 projection, Vec2 jitter_pixels,
                                uint32_t output_width, uint32_t output_height,
                                uint32_t depth_width, uint32_t depth_height);
