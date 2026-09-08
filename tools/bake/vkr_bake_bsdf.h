#pragma once

#include "bake/vkr_bake_material.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Local directions are unit length in the oriented tangent frame: +Z points
   toward the incident path vertex and wo is therefore in the upper hemisphere.
   Alpha decisions, frame construction, Beer attenuation, and medium-stack
   mutation are path-tracer responsibilities. */
typedef struct VkrBakeBsdf {
  VkrBakeMaterialSample surface;
  float32_t eta_incident;
  float32_t eta_transmitted;
  bool8_t thin_walled;
  /* Unit, upper-side normals in the already oriented base local frame. */
  Vec3 clearcoat_normal;
  Vec3 geometric_normal;
  /* Roughness-only immutable coefficients resolved at BSDF creation. */
  float32_t sheen_normalization;
  Vec2 sheen_allocation_reserve;
} VkrBakeBsdf;

typedef struct VkrBakeBsdfEval {
  Vec3 f;
  float32_t pdf;
} VkrBakeBsdfEval;

typedef struct VkrBakeBsdfSample {
  Vec3 wi;
  Vec3 f;
  float32_t pdf;
  /* eta_transmitted / eta_incident for a volume crossing. `f` already has
     the radiance-mode eta correction; callers must not apply eta again. */
  float32_t eta_ratio;
  /* Diffuse sheet crossings are transmitted, non-delta and eta_ratio=1.
     They do not create a refractive medium boundary. */
  bool8_t transmitted;
  bool8_t delta;
} VkrBakeBsdfSample;

bool8_t vkr_bake_bsdf_init(VkrBakeMaterialSample surface,
                           float32_t eta_incident, float32_t eta_transmitted,
                           bool8_t thin_walled, Vec3 clearcoat_normal,
                           Vec3 geometric_normal, VkrBakeBsdf *out_bsdf);
/* `wo` is unit length in the same base local frame as the supplied normals.
   This is the directional energy left for emission and the base BSDF. */
float32_t vkr_bake_bsdf_base_transmission(const VkrBakeBsdf *bsdf, Vec3 wo);
/* Direction-independent sqrt(albedo * nonmetal) and the same
 * amplitude times the existing outgoing GGX/coat/sheen diffuse reserve.
 * Spatial transport multiplies min(endpoint strengths), and applies the
 * material amplitude once at each endpoint;
 * the angular reserve stays at the receiver, as in the local diffuse model. */
Vec3 vkr_bake_subsurface_amplitude(VkrBakeMaterialSample material);
Vec3 vkr_bake_bsdf_subsurface_receiver(const VkrBakeBsdf *bsdf, Vec3 wo);
VkrBakeBsdfEval vkr_bake_bsdf_evaluate(const VkrBakeBsdf *bsdf, Vec3 wo,
                                       Vec3 wi);
VkrBakeBsdfSample vkr_bake_bsdf_sample(const VkrBakeBsdf *bsdf, Vec3 wo,
                                       float32_t u_lobe, float32_t u1,
                                       float32_t u2);

#ifdef __cplusplus
}
#endif
