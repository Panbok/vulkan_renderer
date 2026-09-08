#pragma once

#include "defines.h"
#include "math/vec.h"

#define VKR_SUBSURFACE_PROFILE_COUNT 8u
#define VKR_SUBSURFACE_TAP_COUNT 32u
#define VKR_SUBSURFACE_TABLE_WIDTH 65u
#define VKR_SUBSURFACE_TABLE_HEIGHT 8u
#define VKR_SUBSURFACE_TABLE_BYTE_COUNT 8320u
#define VKR_SUBSURFACE_MAX_RADIUS_PIXELS 32.0f

typedef struct VkrSubsurfaceGpuParams {
  Vec4 projection;
  uint32_t dimensions[4];
} VkrSubsurfaceGpuParams;
_Static_assert(sizeof(VkrSubsurfaceGpuParams) == 32u,
               "Subsurface pass parameters must remain 32 bytes");

typedef struct VkrSubsurfaceProfile {
  /* Effective RGB diffusion distances in metres, not volume mean free paths. */
  Vec3 diffusion_distance;
} VkrSubsurfaceProfile;

/* Caller-owned, row-major RGBA32F upload payload. The texture owner consumes
 * or copies these bytes before their caller-provided storage is released.
 * Texel 2i: (offset.x, offset.y, weight.r, weight.g).
 * Texel 2i+1: (weight.b, 0, 0, 0).
 * Texel 64: (distance.r, distance.g, distance.b, maximum offset radius).
 * Offsets and maximum radius are in units of max(diffusion_distance).
 * Pixel offsets multiply by min(max_distance * pixels_per_metre,
 *                              32 / maximum_offset_radius).
 * This uniformly narrows the profile when its projected support exceeds the
 * radius cap. Offline profile evaluation and sampling retain the full tail.
 * Consumers divide accumulated RGB by the corresponding accepted weight sums;
 * this preserves constant fields after geometry rejection and float rounding.
 */
typedef struct VkrSubsurfaceTable {
  Vec4 texels[VKR_SUBSURFACE_TABLE_HEIGHT][VKR_SUBSURFACE_TABLE_WIDTH];
} VkrSubsurfaceTable;

_Static_assert(sizeof(VkrSubsurfaceTable) == VKR_SUBSURFACE_TABLE_BYTE_COUNT,
               "Subsurface profile table must remain 8320 bytes");

bool8_t vkr_subsurface_profile_valid(VkrSubsurfaceProfile profile);

/* Validates the entire input before writing. Zero profiles produce an empty
 * table; nonzero counts require a profile array. No allocation or retained
 * view. */
bool8_t vkr_subsurface_table_build(const VkrSubsurfaceProfile *profiles,
                                   uint32_t profile_count,
                                   VkrSubsurfaceTable *out_table);

/* These transport functions consume proven positive finite diffusion distance.
 * Radius is nonnegative. Area density has the model's integrable singularity
 * at zero; radial PDF and CDF remain finite there. Units are m^-2 and m^-1.
 * Sample u must lie strictly between zero and one. Its inverse-CDF result uses
 * the full profile, without the runtime pixel-radius cap. */
float64_t vkr_subsurface_profile_evaluate(float64_t radius,
                                          float64_t diffusion_distance);
float64_t vkr_subsurface_profile_radial_pdf(float64_t radius,
                                            float64_t diffusion_distance);
float64_t vkr_subsurface_profile_radial_cdf(float64_t radius,
                                            float64_t diffusion_distance);
float64_t vkr_subsurface_profile_sample_radius(float64_t diffusion_distance,
                                               float64_t u);
