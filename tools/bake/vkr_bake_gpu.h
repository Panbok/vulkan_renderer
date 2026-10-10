#pragma once

#include <cstdint>
#include <vector>

#include "vkr_bake_lightmap.h"
#include "vkr_bake_scene.h"

/*
 * GPU ray-traced lightmap transport for the bake tools (ADR-088). Apple hosts
 * implement it with Metal ray tracing (vkr_bake_metal.mm, owner decision
 * 2026-10-05: a GPU baker for M-series), other hosts with Vulkan ray queries
 * (vkr_bake_vulkan.cpp, owner decision 2026-10-09: a Windows host bakes on
 * its desktop GPU). Both run the same kernel contract with the same per-texel
 * and per-sample seeds. The CPU integrator remains the reference and the path
 * on hosts without GPU ray tracing.
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
struct VkrBakeGpuContext;

/* One lightmap layer's light: which scene lights contribute, whether escaped
   paths take the sky, whether surfaces add emission, and whether the lights
   also light the texel directly. Sun keys bake bounce and sky light without
   the sun's direct term, which stays at runtime; lamp layers include it. */
struct VkrBakeGpuLayer {
  std::vector<uint32_t> lights;
  bool sky = false;
  bool emission = false;
  bool texel_direct = false;
};

struct VkrBakeGpuGatherSettings {
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
  /* Whether the gather returns each texel's luminance-weighted incident
     direction (ADR-104). */
  bool direction = false;
  /* Whether the gather takes only the texel direct term (and, with
     `backface`, the first hits' back-face fraction), for lamp-direct pages
     (ADR-108); indirect light stays zero. */
  bool direct_only = false;
};

/* One layer's gathered light per texel. Direct is the layer lights'
   irradiance at the texel when texel_direct is set: point, spot and
   directional lights are evaluated once per texel, rectangle lights take one
   point per sample. Indirect is the cosine-weighted hemisphere integral of
   incoming radiance (bounce light, emission and sky), and
   indirect_variance the variance of its luminance mean over the samples. */
struct VkrBakeGpuGatherResult {
  std::vector<Vec3> direct;
  std::vector<Vec3> indirect;
  std::vector<float32_t> indirect_variance;
  /* Ambient visibility in [0, 1], when settings.occlusion. */
  std::vector<float32_t> occlusion;
  /* Share of first-bounce rays in [0, 1] that hit the back of a one-sided
     surface, when settings.backface: near one for a texel inside a solid. */
  std::vector<float32_t> backface;
  /* When settings.direction, per texel the sum over its contributions of
     their luminance times their world-space incident direction (xyz) and the
     sum of their luminance (w): each direct-light contribution of the texel
     direct term along its light direction, and each indirect sample's
     irradiance along the sample's first direction. xyz / w is the mean
     incident direction; its length, the directionality, is at most one. */
  std::vector<Vec4> direction;
  double gpu_seconds = 0.0;
};

/* Whether this host can run GPU ray-traced bakes. */
bool vkr_bake_gpu_available();

/* Uploads the scene; it is only read. */
VkrBakeGpuContext *vkr_bake_gpu_create(const VkrBakeScene &scene);

void vkr_bake_gpu_destroy(VkrBakeGpuContext *context);

/* Uploads the scene's lights and sky again after the bake changed them, as
   between sun keys. The light count is unchanged. No dispatch is in flight
   between calls, so the previous buffers are free. */
bool vkr_bake_gpu_update_lighting(VkrBakeGpuContext *context,
                                  const VkrBakeScene &scene);

/* Throughput measurement: traces `samples` cosine-weighted closest-hit rays
 * from every texel and writes each texel's hit fraction. Reports the GPU time
 * of the dispatches in seconds. */
bool vkr_bake_gpu_trace_benchmark(
    VkrBakeGpuContext *context, const std::vector<VkrBakeLightmapTexel> &texels,
    uint32_t samples, uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds);

/* Gathers one layer's light at every texel into `out_result` (see
 * VkrBakeGpuGatherResult) and reports the GPU time in seconds. Occlusion
 * and back-face fractions come from the same first-bounce rays as the
 * indirect light. */
bool vkr_bake_gpu_gather(VkrBakeGpuContext *context,
                         const std::vector<VkrBakeLightmapTexel> &texels,
                         const VkrBakeGpuLayer &layer,
                         const VkrBakeGpuGatherSettings &settings,
                         VkrBakeGpuGatherResult *out_result);

/* One diffuse-volume layer's probe gather (sparse diffuse volumes, phase 4).
   Each probe takes 6 * face_size^2 cube-face pixels of `samples` paths, from
   jittered points of the pixel, through the same path transport as the texel
   gather's indirect light but from a free point: no surface at the origin and
   no texel direct term. Caustic photons are not modeled. */
struct VkrBakeGpuProbeSettings {
  uint32_t face_size = 8u;
  uint32_t samples = 4u;
  uint32_t max_depth = 12u;
  /* Zero disables Russian roulette; otherwise it starts at this depth. */
  uint32_t rr_start_depth = 4u;
  /* vkr_bake_sh_project_l1's band-1 deringing strength. */
  float32_t deringing = 0.0f;
};

/* Gathers one layer at every probe of `positions` and projects each probe's
   cube to L1 SH as vkr_bake_sh_project_l1 does: 12 floats per probe in
   `out_sh`, channel-major, each channel's x, y, z and constant term of E/pi.
   `seeds` holds each probe's path seed, which the CPU bake derives the same
   way. Reports the GPU time in seconds. False when a probe's radiance is not
   finite. */
bool vkr_bake_gpu_gather_probes(VkrBakeGpuContext *context,
                                const std::vector<Vec3> &positions,
                                const std::vector<uint32_t> &seeds,
                                const VkrBakeGpuLayer &layer,
                                const VkrBakeGpuProbeSettings &settings,
                                std::vector<float32_t> *out_sh,
                                double *out_gpu_seconds);

/* Whether this host's GPU transport has the probe gather: every host whose
   GPU traces rays (Vulkan ray queries or Metal ray tracing) does. */
bool vkr_bake_gpu_probe_gather_available();

/*
 * BC6H and BC7 encoding of desktop lightmap pages (desktop baked lamps,
 * owner decision 2026-10-09: each platform bakes its own data, so only the
 * Windows/Vulkan host writes BC planes and their bytes need not match across
 * hosts). Vulkan runs DirectXTex's compute encoders
 * (vendor/directxtex.md): BC6H tries every mode and partition; BC7 tries
 * modes 1, 3 and 4 to 7, without the rarely chosen three-subset modes 0 and
 * 2, as DirectXTex does by default. Metal has no encoder; an Apple host
 * reports it unavailable.
 *
 * Pages are row-major with width and height nonzero multiples of 4. Blocks
 * are 16 bytes each, row-major by 4x4 block, ready to upload as one mip
 * level. Each call is synchronous, reads its input only until it returns,
 * and reports the GPU time of its dispatches in seconds.
 */

/* Whether `context`'s device has the encoders: Vulkan 1.3 integer dot
   products, 16-bit floats and integers in shaders, and BC textures. */
bool vkr_bake_gpu_bc_available(const VkrBakeGpuContext *context);

/* Encodes `rgba` (4 linear floats per texel; alpha is ignored) as
   BC6H_UFLOAT. Negative and NaN values encode as zero and values above the
   largest half float (65504) as 65504. */
bool vkr_bake_gpu_encode_bc6h(VkrBakeGpuContext *context, const float32_t *rgba,
                              uint32_t width, uint32_t height,
                              std::vector<uint8_t> *out_blocks,
                              double *out_gpu_seconds);

/* Encodes `rgba8` (4 UNORM bytes per texel, linear data) as BC7. */
bool vkr_bake_gpu_encode_bc7(VkrBakeGpuContext *context, const uint8_t *rgba8,
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t> *out_blocks,
                             double *out_gpu_seconds);

enum class VkrBakeGpuBcFormat : uint8_t {
  Bc6hUfloat,
  Bc7,
};

/* Decodes (width / 4) * (height / 4) blocks of `format` with the GPU's
   texture unit into 4 floats per texel, for error reports: BC6H texels are
   linear values with alpha one, BC7 texels UNORM values in [0, 1]. */
bool vkr_bake_gpu_decode_bc(VkrBakeGpuContext *context,
                            VkrBakeGpuBcFormat format, const uint8_t *blocks,
                            uint32_t width, uint32_t height,
                            std::vector<float32_t> *out_rgba);
