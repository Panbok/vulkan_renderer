/**
 * @file vkr_frustum.h
 * @brief Frustum culling utilities for view-frustum tests.
 *
 * Provides frustum plane extraction from view-projection matrices and
 * intersection tests for bounding spheres. Handles Vulkan clip conventions
 * (Y inverted, Z in [0,1]).
 */
#pragma once

#include "defines.h"
#include "mat.h"
#include "vec.h"

/**
 * @brief Plane in 3D space using normal-distance form.
 * Plane equation: dot(normal, point) + d >= 0 means point is on positive side.
 */
typedef struct VkrPlane {
  Vec3 normal;
  float32_t d;
} VkrPlane;

/**
 * @brief Frustum plane indices.
 */
typedef enum VkrFrustumPlane {
  VKR_FRUSTUM_PLANE_LEFT = 0,
  VKR_FRUSTUM_PLANE_RIGHT,
  VKR_FRUSTUM_PLANE_BOTTOM,
  VKR_FRUSTUM_PLANE_TOP,
  VKR_FRUSTUM_PLANE_NEAR,
  VKR_FRUSTUM_PLANE_FAR,
  VKR_FRUSTUM_PLANE_COUNT
} VkrFrustumPlane;

/**
 * @brief View frustum defined by 6 planes.
 */
typedef struct VkrFrustum {
  VkrPlane planes[VKR_FRUSTUM_PLANE_COUNT];
} VkrFrustum;

/**
 * @brief Extract frustum planes from view and projection matrices.
 *
 * Uses the Gribb/Hartmann method to extract planes from the combined
 * view-projection matrix. Handles Vulkan clip conventions (Z in [0,1]).
 *
 * @param view View matrix (camera transform).
 * @param projection Projection matrix (perspective or orthographic).
 * @return Frustum with 6 normalized planes.
 */
VkrFrustum vkr_frustum_from_view_projection(Mat4 view, Mat4 projection);

/**
 * @brief Extract frustum planes from a combined view-projection matrix.
 *
 * Assumes Vulkan clip conventions (Z in [0,1]). Useful when only a combined
 * matrix is available, such as shadow cascade view-projection.
 *
 * @param view_projection Combined view-projection matrix (P * V).
 * @return Frustum with 6 normalized planes.
 */
VkrFrustum vkr_frustum_from_matrix(Mat4 view_projection);

/**
 * @brief Test if a bounding sphere intersects or is inside the frustum.
 *
 * Conservative test: returns true if the sphere might be visible.
 * Only returns false if the sphere is completely outside at least one plane.
 *
 * @param frustum Pointer to the frustum to test against.
 * @param center World-space center of the bounding sphere.
 * @param radius Radius of the bounding sphere.
 * @return true if visible or intersecting, false if completely outside.
 */
bool8_t vkr_frustum_test_sphere(const VkrFrustum *frustum, Vec3 center,
                                 float32_t radius);
