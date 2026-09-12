#pragma once

// Bump when a cutout bake changes filtering or fixed encoding settings. Both
// material variant paths and packed metadata use this identity.
#define VKR_VKT_CUTOUT_POLICY_VERSION 1u
#define VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION 2u

#ifdef __cplusplus
extern "C" {
#endif

// Synchronously cook one sRGB cutout texture. Paths are null-terminated,
// borrowed until return; cutoff/factor are finite material values in [0, 1].
// Scratch images are owned and released by the cooker. Returns 1 on success
// (including a verified cache hit), 0 on failure. Publication is atomic.
int vkr_vkt_pack_cutout(const char *source, const char *output, float cutoff,
                        float factor);

enum VkrVktPairResult {
  VKR_VKT_PAIR_FAILED = 0,
  VKR_VKT_PAIR_SUCCESS = 1,
  VKR_VKT_PAIR_INCOMPATIBLE = 2
};

// Cook matched UV-space normal/MR images, folding normal strength and roughness
// factor into the pair. A null roughness source synthesizes AO/metallic = 1.
// Inputs are borrowed until return. Differing source extents are incompatible.
// Each file publishes atomically; callers publish material references only
// after both succeed. Recipe-addressed files may survive a later failure.
int vkr_vkt_pack_normal_roughness(const char *normal_source,
                                  const char *roughness_source,
                                  const char *normal_output,
                                  const char *roughness_output,
                                  float normal_scale, float roughness_factor);

int vkr_vkt_packer_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
