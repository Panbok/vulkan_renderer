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
  uint32_t rr_start_depth = 2u;
  uint32_t seed = 1u;
  /* World distance within which a first hit occludes, for gathers that
     return ambient occlusion. */
  float occlusion_radius = 0.0f;
  /* Largest luminance one gathered sample may carry; a brighter sample is
     scaled down to it, which removes that energy (biased dark). Zero keeps
     every sample. */
  float indirect_clamp = 0.0f;
  /* Whether the gather returns ambient occlusion (needs a positive
     occlusion_radius) and the back-face fraction of first hits. */
  bool occlusion = false;
  bool backface = false;
};

/* One layer's gathered light per texel. Direct is the layer lights'
   irradiance at the texel when texel_direct is set: point, spot and
   directional lights are evaluated once per texel, rectangle lights take one
   point per sample. Indirect is the cosine-weighted hemisphere integral of
   incoming radiance (bounce light, emission and sky), and
   indirect_variance the variance of its luminance mean over the samples. */
struct VkrBakeMetalGatherResult {
  std::vector<Vec3> direct;
  std::vector<Vec3> indirect;
  std::vector<float32_t> indirect_variance;
  /* Ambient visibility in [0, 1], when settings.occlusion. */
  std::vector<float32_t> occlusion;
  /* Share of first-bounce rays in [0, 1] that hit the back of a one-sided
     surface, when settings.backface: near one for a texel inside a solid. */
  std::vector<float32_t> backface;
  double gpu_seconds = 0.0;
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

/* Gathers one layer's light at every texel into `out_result` (see
 * VkrBakeMetalGatherResult) and reports the GPU time in seconds. Occlusion
 * and back-face fractions come from the same first-bounce rays as the
 * indirect light. */
bool vkr_bake_metal_gather(VkrBakeMetalContext *context,
                           const std::vector<VkrBakeLightmapTexel> &texels,
                           const VkrBakeMetalLayer &layer,
                           const VkrBakeMetalGatherSettings &settings,
                           VkrBakeMetalGatherResult *out_result);
