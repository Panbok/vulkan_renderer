/**
 * @file vkr_frustum.c
 * @brief Implementation of frustum culling utilities.
 */

#include "vkr_frustum.h"
#include "vkr_math.h"

/**
 * @brief Normalize a plane (make normal unit length, adjust d accordingly).
 */
vkr_internal VkrPlane vkr_plane_normalize(VkrPlane plane) {
  float32_t len = vec3_length(plane.normal);
  if (len > VKR_FLOAT_EPSILON) {
    float32_t inv_len = 1.0f / len;
    plane.normal = vec3_scale(plane.normal, inv_len);
    plane.d *= inv_len;
  }
  return plane;
}

/**
 * @brief Create plane from Vec4 (xyz = normal, w = d).
 */
vkr_internal VkrPlane vkr_plane_from_vec4(Vec4 v) {
  VkrPlane plane = {
      .normal = vec3_new(v.x, v.y, v.z),
      .d = v.w,
  };
  return vkr_plane_normalize(plane);
}

VkrFrustum vkr_frustum_from_view_projection(Mat4 view, Mat4 projection) {
  // Renderer camera projections are canonical Vulkan [0,1] matrices for both
  // perspective and orthographic cameras. Inferring the depth convention from
  // m33 misclassified Vulkan orthographic projections as OpenGL matrices.
  return vkr_frustum_from_matrix(mat4_mul(projection, view));
}

/**
 * @brief Construct frustum directly from a combined view-projection matrix.
 * @note Assumes Vulkan clip range (0 <= z <= w).
 */
VkrFrustum vkr_frustum_from_matrix(Mat4 view_projection) {
  Mat4 vp = view_projection;

  Vec4 r0 = mat4_row(vp, 0);
  Vec4 r1 = mat4_row(vp, 1);
  Vec4 r2 = mat4_row(vp, 2);
  Vec4 r3 = mat4_row(vp, 3);

  VkrFrustum frustum;
  frustum.planes[VKR_FRUSTUM_PLANE_LEFT] =
      vkr_plane_from_vec4(vec4_add(r3, r0));
  frustum.planes[VKR_FRUSTUM_PLANE_RIGHT] =
      vkr_plane_from_vec4(vec4_sub(r3, r0));
  frustum.planes[VKR_FRUSTUM_PLANE_BOTTOM] =
      vkr_plane_from_vec4(vec4_add(r3, r1));
  frustum.planes[VKR_FRUSTUM_PLANE_TOP] = vkr_plane_from_vec4(vec4_sub(r3, r1));

  // Vulkan clip range: 0 <= z <= w
  frustum.planes[VKR_FRUSTUM_PLANE_NEAR] = vkr_plane_from_vec4(r2);
  frustum.planes[VKR_FRUSTUM_PLANE_FAR] = vkr_plane_from_vec4(vec4_sub(r3, r2));

  return frustum;
}

bool8_t vkr_frustum_test_sphere(const VkrFrustum *frustum, Vec3 center,
                                float32_t radius) {
  for (uint32_t i = 0; i < VKR_FRUSTUM_PLANE_COUNT; i++) {
    const VkrPlane *plane = &frustum->planes[i];

    // Signed distance from center to plane
    float32_t dist = vec3_dot(plane->normal, center) + plane->d;

    // If center is more than radius behind the plane, sphere is fully outside
    if (dist < -radius) {
      return false_v; // Culled
    }
  }

  // Sphere is at least partially inside all planes
  return true_v;
}
