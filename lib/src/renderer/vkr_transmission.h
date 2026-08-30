#pragma once

#include "math/vec.h"

/** CPU reference for the shared production transmission composition kernel. */
typedef struct VkrTransmissionLobes {
  Vec3 diffuse;
  Vec3 specular;
  Vec3 emissive;
} VkrTransmissionLobes;

typedef struct VkrTransmissionExit {
  Vec3 position;
  Vec3 direction;
  float32_t path_length;
} VkrTransmissionExit;

/**
 * Composes the Khronos-style dielectric transmission partition used by both
 * production shaders. Inputs are linear radiance or linear color.
 */
Vec3 vkr_transmission_compose(VkrTransmissionLobes lobes, Vec3 background,
                              Vec3 base_color, Vec3 fresnel,
                              float32_t transmission, float32_t metallic);

/** Resolves a scalar material factor against its sampled texture channel. */
float32_t vkr_transmission_resolve_factor(float32_t factor,
                                          float32_t texture_sample);

/** CPU reference for scaled refracted exit-point construction. */
VkrTransmissionExit
vkr_transmission_exit_point(Vec3 world_position, Vec3 camera_position,
                            Vec3 oriented_normal, Vec3 axis_x, Vec3 axis_y,
                            Vec3 axis_z, float32_t ior, float32_t thickness);

/** Converts projected clip coordinates to a backend's normalized texture UV. */
Vec2 vkr_transmission_project_uv(Vec4 clip, bool8_t flip_y);

/** Maps perceptual roughness and IOR to the available feedback-pyramid LOD. */
float32_t vkr_transmission_rough_lod(float32_t roughness, float32_t ior,
                                     uint32_t mip_count);

/** Returns zero for ordered feedback or one for the rough opaque pyramid. */
float32_t vkr_transmission_feedback_blend(float32_t roughness);
