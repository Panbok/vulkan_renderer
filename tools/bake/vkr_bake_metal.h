#pragma once

#include <cstdint>
#include <vector>

#include "vkr_bake_lightmap.h"
#include "vkr_bake_scene.h"

/*
 * Metal ray-traced lightmap transport for the bake tools (ADR-087, owner
 * decision 2026-10-05: a GPU baker for M-series). The CPU integrator remains
 * the reference and the path on hosts without Metal ray tracing.
 *
 * The GPU transport is a lightmap subset of the ADR-054 integrator: paths
 * scatter through one cosine lobe whose albedo is the CPU BSDF's total opaque
 * reflectance (split-sum specular R from the shared DFG table plus the
 * diffuse residual (1 - R)(1 - metallic) base, over the non-glass fraction),
 * so specular energy is kept but redistributed diffusely. Paths skip cutout
 * surfaces below their alpha cutoff and blended surfaces stochastically by
 * alpha, add surface emission, take the sky on escape, and sample every layer
 * light at each surface with shadow rays through the CPU shadow walk, using
 * the CPU light falloff, cone and rectangle-area formulas. Russian roulette
 * matches the CPU rule. A surface's glass fraction passes paths straight
 * through tinted by base color instead of refracting them. Clearcoat, sheen,
 * subsurface and anisotropy are not modeled.
 *
 * The context owns the scene's GPU copy: an acceleration structure over the
 * world-space triangles, per-corner attributes, materials with base color and
 * emission textures resampled to 128x128 layers, the lights, the DFG table
 * and an equirectangular sky map. It is created once and borrowed by every
 * dispatch.
 */
struct VkrBakeMetalContext;

/* One lightmap layer's light: which scene lights contribute, whether escaped
   paths take the sky, whether surfaces add emission, and whether the lights
   also light the texel directly. Sun keys bake bounce and sky light without
   the sun's direct term, which stays at runtime; lamp layers include it. */
struct VkrBakeMetalLayer {
  std::vector<uint32_t> lights;
  bool sky = false;
  bool emission = false;
  bool texel_direct = false;
};

struct VkrBakeMetalGatherSettings {
  uint32_t samples = 16u;
  uint32_t max_depth = 4u;
  /* Zero disables Russian roulette; otherwise it starts at this depth. */
  uint32_t rr_start_depth = 4u;
  uint32_t seed = 1u;
  /* World distance within which a first hit occludes, for gathers that
     return ambient occlusion. */
  float occlusion_radius = 0.0f;
};

/* Whether this host can run Metal ray-traced bakes. */
bool vkr_bake_metal_available();

/* Uploads the scene; it is only read. */
VkrBakeMetalContext *vkr_bake_metal_create(const VkrBakeScene &scene);

void vkr_bake_metal_destroy(VkrBakeMetalContext *context);

/* Uploads the scene's lights and sky again after the bake changed them, as
   between sun keys. The light count is unchanged. No dispatch is in flight
   between calls, so the previous buffers are free. */
bool vkr_bake_metal_update_lighting(VkrBakeMetalContext *context,
                                    const VkrBakeScene &scene);

/* Throughput measurement: traces `samples` cosine-weighted closest-hit rays
 * from every texel and writes each texel's hit fraction. Reports the GPU time
 * of the dispatches in seconds. */
bool vkr_bake_metal_trace_benchmark(
    VkrBakeMetalContext *context,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t samples,
    uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds);

/* Gathers one layer's irradiance at every texel: the cosine-weighted
 * hemisphere integral of incoming radiance, plus the layer lights' direct
 * irradiance when texel_direct is set. Writes one RGB value per texel and
 * reports the GPU time in seconds. With `out_occlusion`, also writes each
 * texel's ambient visibility in [0, 1] from the same first-bounce rays, which
 * needs a positive settings.occlusion_radius. */
bool vkr_bake_metal_gather(VkrBakeMetalContext *context,
                           const std::vector<VkrBakeLightmapTexel> &texels,
                           const VkrBakeMetalLayer &layer,
                           const VkrBakeMetalGatherSettings &settings,
                           std::vector<Vec3> *out_irradiance,
                           std::vector<float32_t> *out_occlusion,
                           double *out_gpu_seconds);
