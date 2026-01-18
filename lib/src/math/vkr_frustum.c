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
  // Compute view-projection matrix: VP = P * V
  Mat4 vp = mat4_mul(projection, view);

  // Extract matrix rows for plane computation
  // Row i contains (m[i][0], m[i][1], m[i][2], m[i][3])
  Vec4 r0 = mat4_row(vp, 0);
  Vec4 r1 = mat4_row(vp, 1);
  Vec4 r2 = mat4_row(vp, 2);
  Vec4 r3 = mat4_row(vp, 3);

  VkrFrustum frustum;

  // Gribb/Hartmann plane extraction method
  // For a point p in clip space: -w <= x <= w, -w <= y <= w, 0 <= z <= w
  // These correspond to: x + w >= 0 (left), w - x >= 0 (right), etc.

  // Left plane: r3 + r0 (x >= -w)
  frustum.planes[VKR_FRUSTUM_PLANE_LEFT] =
      vkr_plane_from_vec4(vec4_add(r3, r0));

  // Right plane: r3 - r0 (x <= w)
  frustum.planes[VKR_FRUSTUM_PLANE_RIGHT] =
      vkr_plane_from_vec4(vec4_sub(r3, r0));

  // Bottom plane: r3 + r1 (y >= -w)
  frustum.planes[VKR_FRUSTUM_PLANE_BOTTOM] =
      vkr_plane_from_vec4(vec4_add(r3, r1));

  // Top plane: r3 - r1 (y <= w)
  frustum.planes[VKR_FRUSTUM_PLANE_TOP] = vkr_plane_from_vec4(vec4_sub(r3, r1));

  // Near/Far planes depend on depth range
  // Vulkan uses Z in [0, 1]: 0 <= z <= w
  // Our mat4_perspective outputs Vulkan-style projection (m33=0, m32!=0)

  // Check if this is a Vulkan-style perspective projection
  // Perspective: m33 == 0 and m32 != 0
  bool8_t is_vulkan_perspective =
      (vkr_abs_f32(projection.m33) < VKR_FLOAT_EPSILON) &&
      (vkr_abs_f32(projection.m32) > VKR_FLOAT_EPSILON);

  if (is_vulkan_perspective) {
    // Vulkan Z in [0, w]: z >= 0, z <= w
    // Near: r2 (z >= 0)
    frustum.planes[VKR_FRUSTUM_PLANE_NEAR] = vkr_plane_from_vec4(r2);
  } else {
    // OpenGL-style Z in [-w, w]: z >= -w
    // Near: r3 + r2
    frustum.planes[VKR_FRUSTUM_PLANE_NEAR] =
        vkr_plane_from_vec4(vec4_add(r3, r2));
  }

  // Far plane: r3 - r2 (z <= w) - same for both conventions
  frustum.planes[VKR_FRUSTUM_PLANE_FAR] = vkr_plane_from_vec4(vec4_sub(r3, r2));

  return frustum;
}

/**
 * @brief Construct frustum directly from a combined view-projection matrix.
 * @note Assumes Vulkan clip range (0 <= z <= w). For OpenGL-style matrices,
 *       use vkr_frustum_from_view_projection() instead.
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
