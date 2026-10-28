#include "vkr_decal.h"

bool8_t vkr_decal_set_box(VkrDecal *decal, Mat4 box_to_world) {
  const Mat4 *m = &box_to_world;
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (!vkr_is_finite_f32(m->elements[i])) {
      return false_v;
    }
  }
  /* The threshold matches mat4_inverse_affine's, which returns the identity
     for a box it rejects. */
  const float32_t cofactor[3] = {
      m->m11 * m->m22 - m->m12 * m->m21,
      m->m12 * m->m20 - m->m10 * m->m22,
      m->m10 * m->m21 - m->m11 * m->m20,
  };
  const float32_t determinant =
      m->m00 * cofactor[0] + m->m01 * cofactor[1] + m->m02 * cofactor[2];
  const float32_t determinant_terms = vkr_abs_f32(m->m00 * cofactor[0]) +
                                      vkr_abs_f32(m->m01 * cofactor[1]) +
                                      vkr_abs_f32(m->m02 * cofactor[2]);
  if (!vkr_is_finite_f32(determinant) ||
      !vkr_is_finite_f32(determinant_terms) ||
      vkr_abs_f32(determinant) <= 1e-6f * determinant_terms) {
    return false_v;
  }

  /* The box's corners lie at its centre plus or minus half of each column,
     so its bounds reach half the absolute column sum along each axis. */
  const Vec3 centre = vec3_new(m->m03, m->m13, m->m23);
  const Vec3 reach = vec3_new(
      0.5f * (vkr_abs_f32(m->m00) + vkr_abs_f32(m->m01) + vkr_abs_f32(m->m02)),
      0.5f * (vkr_abs_f32(m->m10) + vkr_abs_f32(m->m11) + vkr_abs_f32(m->m12)),
      0.5f * (vkr_abs_f32(m->m20) + vkr_abs_f32(m->m21) + vkr_abs_f32(m->m22)));
  decal->world_to_box = mat4_inverse_affine(box_to_world);
  decal->bounds_min = vec3_sub(centre, reach);
  decal->bounds_max = vec3_add(centre, reach);
  return true_v;
}

bool8_t vkr_decal_valid(const VkrDecal *decal) {
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (!vkr_is_finite_f32(decal->world_to_box.elements[i])) {
      return false_v;
    }
  }
  const Mat4 *m = &decal->world_to_box;
  const float32_t determinant = m->m00 * (m->m11 * m->m22 - m->m12 * m->m21) -
                                m->m01 * (m->m10 * m->m22 - m->m12 * m->m20) +
                                m->m02 * (m->m10 * m->m21 - m->m11 * m->m20);
  return m->m30 == 0.0f && m->m31 == 0.0f && m->m32 == 0.0f && m->m33 == 1.0f &&
         determinant != 0.0f && vkr_is_finite_f32(determinant) &&
         vec3_is_finite(decal->bounds_min) &&
         vec3_is_finite(decal->bounds_max) &&
         decal->bounds_min.x <= decal->bounds_max.x &&
         decal->bounds_min.y <= decal->bounds_max.y &&
         decal->bounds_min.z <= decal->bounds_max.z && decal->opacity >= 0.0f &&
         decal->opacity <= 1.0f && decal->fade_cos_start <= 1.0f &&
         decal->fade_cos_start > decal->fade_cos_end &&
         decal->fade_cos_end >= -1.0f && decal->depth_fade >= 0.0f &&
         decal->depth_fade <= 1.0f;
}
