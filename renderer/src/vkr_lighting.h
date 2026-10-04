#pragma once
#include "defines.h"
#include "math/vec.h"
#include "vkr_gpu_abi.h"

typedef enum VkrPointLightKind {
  VKR_POINT_LIGHT_KIND_POLYNOMIAL = 0,
  VKR_POINT_LIGHT_KIND_GLTF_POINT = 1,
  VKR_POINT_LIGHT_KIND_GLTF_SPOT = 2,
} VkrPointLightKind;

/** Apparent solar-disc diameter of a directional light that authors none. */
#define VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES 0.53f
/** The light's diameter drives both the sky's disc and the shadow penumbra,
 * so it shares the sky's disc limit (ADR-058). */
#define VKR_DIRECTIONAL_LIGHT_MAX_SUN_ANGULAR_DIAMETER_DEGREES 5.0f
/** Largest authored point/spot emitter radius in metres. Loading clamps larger
 * values; the local shadow filter caps the resulting penumbra in face texels.
 */
#define VKR_POINT_LIGHT_SOURCE_RADIUS_MAX 1.0f

#define VKR_MAX_SCENE_POINT_LIGHTS 128u
#define VKR_POINT_LIGHT_GRID_MASK_WORDS 4u
#define VKR_POINT_LIGHT_GRID_MAX_CELLS 384u
#define VKR_POINT_LIGHT_GRID_MIN_CELL_SIZE 4.0f

_Static_assert(VKR_MAX_SCENE_POINT_LIGHTS ==
                   VKR_POINT_LIGHT_GRID_MASK_WORDS * 32u,
               "The light-grid mask must represent the complete scene table");

/* Fixed-point units per fully lit pixel in a light-contribution sample. A
 * 4K frame of fully lit pixels stays below 2^31. */
#define VKR_LOCAL_LIGHT_CONTRIBUTION_SCALE 256u

/* Deferred lighting measures light contribution in frames whose index is a
 * multiple of this period. With two or three frames of readback latency the
 * newest sample stays younger than the local shadow cache's eight-frame
 * feedback limit, and the other frames skip the per-light counters. */
#define VKR_LOCAL_LIGHT_CONTRIBUTION_PERIOD 4u

/**
 * Visible contribution of each punctual light, measured by deferred lighting
 * in the source frame: over its shaded pixels, the light's unshadowed
 * luminance at the surface after pre-exposure, x, compressed per pixel to
 * x / (1 + x), summed and scaled by VKR_LOCAL_LIGHT_CONTRIBUTION_SCALE. Lights
 * behind walls or out of view measure zero; light that would leak through
 * geometry counts, and shadowing never changes the measure. Entries follow the
 * source frame's light table, identified by render id.
 */
typedef struct VkrLocalLightContributionSample {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint32_t light_count;
  uint32_t render_ids[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t contribution[VKR_MAX_SCENE_POINT_LIGHTS];
  bool8_t valid;
} VkrLocalLightContributionSample;

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
  /* Emitter sphere radius in metres, finite in
   * [0, VKR_POINT_LIGHT_SOURCE_RADIUS_MAX]. It widens the light's shadow
   * penumbra with blocker distance; zero keeps the fixed shadow filter. */
  float32_t source_radius;
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
/** Packs one rectangle light; `radiance_scale` is the frame's pre-exposure,
 * or 1 for an unscaled content signature. */
void vkr_rectangle_light_pack(const VkrRectangleLight *light,
                              float32_t radiance_scale,
                              VkrGpuRectangleLightRow *row);

/** Packs one canonical point light into the shared four-Vec4 GPU row;
 * `radiance_scale` multiplies its intensity as for rectangle lights. */
void vkr_point_light_pack(const VkrPointLight *light, float32_t radiance_scale,
                          VkrGpuPointLightRow *row);

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
