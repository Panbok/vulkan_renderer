#pragma once

#include "defines.h"

/** Shipping IBL target contract shared by every renderer implementation. */
#define VKR_IBL_IRRADIANCE_SIZE 64u
#define VKR_IBL_PREFILTER_SIZE 256u
#define VKR_IBL_BRDF_SIZE 128u
#define VKR_IBL_PREFILTER_MIP_COUNT 9u

typedef struct VkrIblUv {
  float32_t u;
  float32_t v;
} VkrIblUv;

typedef struct VkrIblDirection {
  float32_t x;
  float32_t y;
  float32_t z;
} VkrIblDirection;

/** Vulkan cubemap array-layer order. */
typedef enum VkrIblCubeFace {
  VKR_IBL_CUBE_FACE_POSITIVE_X = 0,
  VKR_IBL_CUBE_FACE_NEGATIVE_X = 1,
  VKR_IBL_CUBE_FACE_POSITIVE_Y = 2,
  VKR_IBL_CUBE_FACE_NEGATIVE_Y = 3,
  VKR_IBL_CUBE_FACE_POSITIVE_Z = 4,
  VKR_IBL_CUBE_FACE_NEGATIVE_Z = 5,
  VKR_IBL_CUBE_FACE_COUNT = 6,
} VkrIblCubeFace;

/** Converts IEEE-754 binary32 to binary16 with round-to-nearest-even. */
uint16_t vkr_float32_to_float16(float32_t value);

/** Converts IEEE-754 binary16 to binary32 exactly. */
float32_t vkr_float16_to_float32(uint16_t value);

/**
 * Validates a 2:1 equirect source and derives a power-of-two cube extent and
 * complete mip chain within the published device limits.
 */
bool8_t
vkr_ibl_derive_cubemap_size(uint32_t equirect_width, uint32_t equirect_height,
                            uint32_t max_cube_extent, uint32_t max_mip_levels,
                            uint32_t *out_face_size, uint32_t *out_mip_count);

/** Canonical equirect mapping used by the conversion shader. */
VkrIblUv vkr_ibl_direction_to_equirect_uv(VkrIblDirection direction);

/** Inverse of the canonical mapping away from the poles' arbitrary longitude.
 */
VkrIblDirection vkr_ibl_equirect_uv_to_direction(VkrIblUv uv);

/**
 * Maps normalized face coordinates to the unnormalized lookup direction used
 * by Vulkan cubemap sampling. Face coordinates use image convention: (0,0) is
 * the top-left texel and (1,1) is the bottom-right texel.
 */
VkrIblDirection vkr_ibl_cube_face_uv_to_direction(VkrIblCubeFace face,
                                                  VkrIblUv uv);

/**
 * Computes the source-cubemap mip for GGX prefiltered importance sampling.
 * The returned value is finite and clamped to the initialized source range.
 */
float32_t vkr_ibl_prefilter_source_lod(float32_t no_h, float32_t vo_h,
                                       float32_t roughness,
                                       uint32_t sample_count,
                                       uint32_t source_face_size,
                                       uint32_t source_mip_count);

/*
 * Second-order spherical-harmonic diffuse response (ADR-038).
 *
 * The stored signal is the shader-facing response D(n) = E(n) / pi, not raw
 * irradiance E. Projection folds the normalized clamped-cosine transfer factors
 * K0 = 1, K1 = 2/3, K2 = 1/4 into the radiance coefficients, so a constant
 * source radiance L evaluates to L and shading multiplies the result by diffuse
 * albedo with no further /pi.
 *
 * This header is the single authority for basis ordering, signs, and packing.
 * Both backend shaders mirror it exactly; the axis-direction tests in
 * tests/src/ibl_math_tests.c are normative for signs and cubemap handedness.
 */

#define VKR_SH_L2_COEFFICIENT_COUNT 9u
/** Sloan shader-optimized form: cAr cAg cAb cBr cBg cBb cC. */
#define VKR_SH_PACKED_VECTOR_COUNT 7u
#define VKR_SH_SLOT_BYTES (VKR_SH_PACKED_VECTOR_COUNT * 4u * 4u)
/** Greatest projection face extent; §2.1 of the implementation spec. */
#define VKR_SH_PROJECTION_MAX_FACE_SIZE 32u

/** Nine real L2 coefficients per colour channel, in basis order. */
typedef struct VkrShL2Rgb {
  float32_t c[VKR_SH_L2_COEFFICIENT_COUNT][3];
} VkrShL2Rgb;

/** Slot-resident packed form: 112 bytes consumed as seven dot products. */
typedef struct VkrShL2Packed {
  float32_t v[VKR_SH_PACKED_VECTOR_COUNT][4];
} VkrShL2Packed;

/** Reports what the shader-side max(D, 0) policy changes for a slot. */
typedef struct VkrShL2ClampDiagnostics {
  uint32_t sample_count;
  /** Samples where any channel reconstructed below zero. */
  uint32_t negative_sample_count;
  /** Most negative channel value over the sphere; zero when none ring. */
  float32_t min_value;
  /** Mean response over the sphere, before and after the clamp. */
  float32_t mean_unclamped[3];
  float32_t mean_clamped[3];
} VkrShL2ClampDiagnostics;

/**
 * Samples one texel of a cubemap face. `x` and `y` are texel indices in image
 * convention, matching vkr_ibl_cube_face_uv_to_direction().
 */
typedef void (*VkrIblCubeSampleFn)(void *user, VkrIblCubeFace face, uint32_t x,
                                   uint32_t y, float32_t out_rgb[3]);

/** Evaluates the nine real L2 basis functions for a non-zero direction. */
void vkr_ibl_sh_basis(VkrIblDirection direction,
                      float32_t out_basis[VKR_SH_L2_COEFFICIENT_COUNT]);

/**
 * Exact solid angle of the cube-face region [s0, s1] x [t0, t1], where both
 * ranges are in the [-1, 1] face parameterization. Summing every texel of every
 * face yields 4pi.
 */
float32_t vkr_ibl_cube_texel_solid_angle(float32_t s0, float32_t s1,
                                         float32_t t0, float32_t t1);

/**
 * Deringing window applied to band `band` at projection time:
 * pow(sinc_pi(band / 3), deringing). A `deringing` of zero is the identity.
 */
float32_t vkr_ibl_sh_window_factor(uint32_t band, float32_t deringing);

/**
 * Selects the projection mip: the greatest available face extent not larger
 * than VKR_SH_PROJECTION_MAX_FACE_SIZE, clamped to the last available mip. A
 * source already at or below that extent projects mip 0.
 */
bool8_t vkr_ibl_sh_projection_mip(uint32_t source_face_size,
                                  uint32_t source_mip_count, uint32_t *out_mip,
                                  uint32_t *out_face_size);

/**
 * Full-resolution reference projection of one cube face level into radiance
 * coefficients, weighting each texel by its exact solid angle and normalizing
 * by the accumulated total. Accumulates in double precision: this is the
 * reference the bounded GPU kernel is measured against, not its mirror.
 *
 * `out_total_solid_angle` receives the unnormalized accumulated weight, which
 * the caller can compare against 4pi.
 */
bool8_t vkr_ibl_sh_project_cube(uint32_t face_size, VkrIblCubeSampleFn sample,
                                void *user, VkrShL2Rgb *out_radiance,
                                float32_t *out_total_solid_angle);

/**
 * Folds the normalized clamped-cosine transfer factors and the deringing window
 * into radiance coefficients, producing the stored response coefficients.
 */
void vkr_ibl_sh_apply_diffuse_transfer(VkrShL2Rgb *coefficients,
                                       float32_t deringing);

/** Packs stored coefficients into the seven-vector shader form. */
void vkr_ibl_sh_pack(const VkrShL2Rgb *coefficients, VkrShL2Packed *out_packed);

/**
 * Reconstructs the unclamped response from the nine stored coefficients. The
 * shader-facing value is max(result, 0); see VkrShL2ClampDiagnostics for what
 * that policy costs.
 */
void vkr_ibl_sh_evaluate(const VkrShL2Rgb *coefficients,
                         VkrIblDirection direction, float32_t out_rgb[3]);

/** Packed-form evaluation; must agree with vkr_ibl_sh_evaluate(). */
void vkr_ibl_sh_evaluate_packed(const VkrShL2Packed *packed,
                                VkrIblDirection direction,
                                float32_t out_rgb[3]);

/**
 * Measures negative-lobe frequency and the integrated response drift the
 * non-negativity clamp introduces, over a deterministic solid-angle-weighted
 * sphere grid of `latitude_steps` by `2 * latitude_steps` samples.
 */
void vkr_ibl_sh_clamp_diagnostics(const VkrShL2Packed *packed,
                                  uint32_t latitude_steps,
                                  VkrShL2ClampDiagnostics *out_diagnostics);
