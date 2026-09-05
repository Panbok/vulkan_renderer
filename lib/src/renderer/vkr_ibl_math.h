#pragma once

#include "defines.h"

/** Shipping IBL target contract shared by every renderer implementation. */
#define VKR_IBL_PREFILTER_SIZE 256u
#define VKR_IBL_PREFILTER_MIP_COUNT 9u

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

/*
 * Second-order spherical-harmonic diffuse response (ADR-038).
 *
 * The stored signal is the shader-facing response D(n) = E(n) / pi, not raw
 * irradiance E. Projection folds the normalized clamped-cosine transfer factors
 * K0 = 1, K1 = 2/3, K2 = 1/4 into the radiance coefficients, so a constant
 * source radiance L evaluates to L and shading multiplies the result by diffuse
 * albedo with no further /pi.
 *
 * This header defines slot sizing and the host packed layout.
 * shaders/shared/sh_l2_kernel.slangh owns projection and evaluation math.
 */

#define VKR_SH_L2_COEFFICIENT_COUNT 9u
/** Sloan shader-optimized form: cAr cAg cAb cBr cBg cBb cC. */
#define VKR_SH_PACKED_VECTOR_COUNT 7u
#define VKR_SH_SLOT_BYTES (VKR_SH_PACKED_VECTOR_COUNT * 4u * 4u)
/** Greatest projection face extent; §2.1 of the implementation spec. */
#define VKR_SH_PROJECTION_MAX_FACE_SIZE 32u

/*
 * Coefficient slot-pool capacity. These live here rather than beside the pool
 * so the packet header can bound-check a slot without depending on the pool.
 * vkr_ibl_sh_pool.c static-asserts them against the scene probe maximum.
 */
#define VKR_SH_SLOT_BLACK 0u
/** Retained fallback environment, active scene environment, and every probe. */
#define VKR_SH_LOGICAL_MAX 18u
#define VKR_SH_GENERATION_COUNT 2u
#define VKR_SH_REUSABLE_SLOTS (VKR_SH_LOGICAL_MAX * VKR_SH_GENERATION_COUNT)
#define VKR_SH_SLOT_CAPACITY (VKR_SH_REUSABLE_SLOTS + 1u)
#define VKR_SH_BUFFER_BYTES (VKR_SH_SLOT_CAPACITY * VKR_SH_SLOT_BYTES)

/** Slot-resident packed form: 112 bytes consumed as seven dot products. */
typedef struct VkrShL2Packed {
  float32_t v[VKR_SH_PACKED_VECTOR_COUNT][4];
} VkrShL2Packed;

_Static_assert(sizeof(VkrShL2Packed) == VKR_SH_SLOT_BYTES,
               "SH packed-slot ABI drift");

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
