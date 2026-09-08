#pragma once
#include "defines.h"
#include "math/vec.h"
#include "vkr_gpu_abi.h"

typedef enum VkrPointLightKind {
  VKR_POINT_LIGHT_KIND_POLYNOMIAL = 0,
  VKR_POINT_LIGHT_KIND_GLTF_POINT = 1,
  VKR_POINT_LIGHT_KIND_GLTF_SPOT = 2,
} VkrPointLightKind;

#define VKR_MAX_SCENE_POINT_LIGHTS 128u
#define VKR_POINT_LIGHT_GRID_MASK_WORDS 4u
#define VKR_POINT_LIGHT_GRID_MAX_CELLS 384u
#define VKR_POINT_LIGHT_GRID_MIN_CELL_SIZE 4.0f

_Static_assert(VKR_MAX_SCENE_POINT_LIGHTS ==
                   VKR_POINT_LIGHT_GRID_MASK_WORDS * 32u,
               "The light-grid mask must represent the complete scene table");

typedef struct VkrPointLight {
  Vec3 position;
  Vec3 color;
  float32_t intensity;
  float32_t constant;
  float32_t linear;
  float32_t quadratic;
  float32_t range;
  Vec3 direction;
  float32_t inner_cone_angle;
  float32_t outer_cone_angle;
  VkrPointLightKind kind;
  uint32_t render_id;
  bool8_t casts_shadow;
} VkrPointLight;

#define VKR_MAX_SCENE_RECTANGLE_LIGHTS 8u

/** One-sided emitter with orthonormal world axes and world-unit half sizes.
 * Emission faces -cross(right, up); radiance is per unit projected area. */
typedef struct VkrRectangleLight {
  Vec3 position;
  Vec3 right;
  Vec3 up;
  Vec3 color;
  float32_t half_width;
  float32_t half_height;
  float32_t radiance;
  uint32_t render_id;
} VkrRectangleLight;

bool8_t vkr_rectangle_light_valid(const VkrRectangleLight *light);
void vkr_rectangle_light_pack(const VkrRectangleLight *light,
                             VkrGpuRectangleLightRow *row);

/** Packs one canonical point light into the shared four-Vec4 GPU row. */
void vkr_point_light_pack(const VkrPointLight *light, VkrGpuPointLightRow *row);

/** Raw 128-bit light membership. Its bytes are uploaded through a float4
 * uniform and recovered with asuint() in Slang to stay within the existing
 * reflection type vocabulary. */
typedef struct VkrPointLightMask {
  uint32_t words[VKR_POINT_LIGHT_GRID_MASK_WORDS];
} VkrPointLightMask;

_Static_assert(sizeof(VkrPointLightMask) == sizeof(Vec4),
               "One light-grid mask must occupy one std140 float4 slot");

/** Camera-independent, conservative world-space lookup for fragment-local
 * punctual-light evaluation. Finite lights populate cells; unbounded legacy
 * polynomial lights populate global_mask and are evaluated in every cell. */
typedef struct VkrPointLightGrid {
  Vec3 origin;
  float32_t cell_size;
  uint32_t dimensions[3];
  uint32_t cell_count;
  VkrPointLightMask global_mask;
  VkrPointLightMask masks[VKR_POINT_LIGHT_GRID_MAX_CELLS];
  uint32_t reference_count;
  uint32_t max_lights_per_cell;
  uint32_t global_light_count;
} VkrPointLightGrid;
