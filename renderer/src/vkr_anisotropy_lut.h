#pragma once

#include <stdint.h>

#define VKR_ANISOTROPY_MIN_ROUGHNESS 0.04f
#define VKR_ANISOTROPY_MIN_NOV 0.0001f
#define VKR_ANISOTROPY_LUT_SIZE 64u
#define VKR_ANISOTROPY_LUT_STRENGTH_COUNT 8u
#define VKR_ANISOTROPY_LUT_AZIMUTH_COUNT 8u
#define VKR_ANISOTROPY_LUT_LAYER_COUNT 64u
#define VKR_ANISOTROPY_LUT_TABLE_COUNT 3u
#define VKR_ANISOTROPY_LUT_CHANNEL_COUNT 4u
#define VKR_ANISOTROPY_LUT_TABLE_TEXEL_COUNT                                   \
  (VKR_ANISOTROPY_LUT_SIZE * VKR_ANISOTROPY_LUT_SIZE *                         \
   VKR_ANISOTROPY_LUT_LAYER_COUNT)
#define VKR_ANISOTROPY_LUT_TABLE_BYTE_SIZE                                     \
  (VKR_ANISOTROPY_LUT_TABLE_TEXEL_COUNT * 4u * sizeof(uint16_t))

/* Immutable RGBA16F array textures; each layer is 64x64. X is normalized
 * roughness [.04,1], Y is (1-sqrt(max(N.V,1e-4)))/.99. Layer=s*8+phi, with
 * uniform strength [0,1] and first-quadrant view azimuth [0,pi/2]. Filter X/Y,
 * then interpolate the four adjacent strength/azimuth layers before decoding.
 *
 * Table 0: {log2(a),log2(b),rectangle A,h}; table 1: {j,k,u.x,u.y};
 * table 2: {u.z,rectangle B,DFG A,DFG B}. Canonically c=1. C has rows (a,0,0),
 * (h,b,0), (j,k,c). Form w=normalize(h*k-j*b,-k*a,a*b), the transformed
 * physical horizon. Its positive Z supports a continuous Frisvad basis e1/e2/w.
 * Normalize the filtered u and decode support q=e1*u.x+e2*u.y+w*u.z.
 * Stored u.z is positive, so every interpolant is nonzero and physical
 * hemisphere mass is exactly .5*(1+normalize(u).z). The inverse LTC is
 * Q*C, where Q has orthonormal rows ending in q (a robust Frisvad basis).
 * Its determinant abc stays positive under all interpolations.
 * DFG A/B store physical directional GGX energy. Rectangle A/B retain the
 * current isotropic amplitude at strength zero and directional DFG at other
 * knots. Clip rectangles to the receiver horizon and apply
 * (F0*rectangle_A + F90*rectangle_B) / mass, then the GGX energy scale.
 * Existing isotropic tables remain authoritative when strength is zero.
 */
extern const uint16_t
    vkr_anisotropy_lut_pixels[VKR_ANISOTROPY_LUT_TABLE_COUNT]
                             [VKR_ANISOTROPY_LUT_TABLE_TEXEL_COUNT *
                              VKR_ANISOTROPY_LUT_CHANNEL_COUNT];

#ifdef __cplusplus
extern "C" {
#endif
/* Caller supplies finite material-domain values and first-quadrant azimuth. */
void vkr_anisotropy_dfg_sample(float no_v, float roughness, float strength,
                               float azimuth, float out_ab[2]);
#ifdef __cplusplus
}
#endif
