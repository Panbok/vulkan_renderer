#pragma once

#include "defines.h"
#include "vec.h"
#include "vkr_simd.h"

// ================================================
// Quaternion Constants
// ================================================

/**
 * @brief Threshold for switching from slerp to lerp
 * When quaternions are very close (dot product > 0.9995),
 * linear interpolation is more numerically stable
 */
#define VKR_QUAT_SLERP_THRESHOLD 0.9995f

/**
 * @brief Epsilon for quaternion operations
 * Used for checking near-zero conditions in normalization and axis extraction
 */
#define VKR_QUAT_EPSILON VKR_FLOAT_EPSILON

/**
 * @brief Gimbal lock threshold for Euler angle extraction
 * When pitch is within this range of ±90°, we're in gimbal lock territory
 */
#define VKR_QUAT_GIMBAL_LOCK_THRESHOLD 0.99999f

/**
 * @file vkr_quat.h
 * @brief SIMD-optimized quaternion mathematics for 3D rotations
 *
 * Quaternions represent rotations using 4 components (x,y,z,w) where:
 * - (x,y,z) represents the vector part (imaginary components)
 * - w represents the scalar part (real component)
 *
 * Memory layout matches Vec4 for SIMD optimization.
 *
 * Coordinate System:
 * - RIGHT-HANDED coordinate system (industry standard)
 * - Positive rotations are counter-clockwise when looking along positive axis
 * - Compatible with Vulkan, USD, glTF standards
 *
 * Conventions:
 * - Euler angles use XYZ order (roll, pitch, yaw) - right-handed standard
 * - Quaternion multiplication: q1 * q2 applies q2 first, then q1
 * - Unit quaternions are assumed for rotation operations
 */

/**
 * @brief Quaternion type
 * @note Memory layout matches Vec4 for SIMD optimization
 */
typedef Vec4 VkrQuat;

// ================================================
// Quaternion Construction
// ================================================

/**
 * @brief Creates a quaternion from individual components
 * @param x Vector x component (i)
 * @param y Vector y component (j)
 * @param z Vector z component (k)
 * @param w Scalar component
 */
vkr_internal INLINE VkrQuat vkr_quat_new(float32_t x, float32_t y, float32_t z,
                                         float32_t w) {
  return vec4_new(x, y, z, w);
}

/**
 * @brief Returns the identity quaternion (no rotation)
 * @return Quaternion representing no rotation (0,0,0,1)
 */
vkr_internal INLINE VkrQuat vkr_quat_identity(void) {
  return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a quaternion from axis-angle representation
 * @param axis Normalized rotation axis
 * @param angle Rotation angle in radians
 * @return Quaternion representing the rotation
 */
vkr_internal INLINE VkrQuat vkr_quat_from_axis_angle(Vec3 axis,
                                                     float32_t angle) {
  // Validate axis is not zero
  float32_t axis_len_sq = vec3_length_squared(axis);
  if (axis_len_sq < VKR_QUAT_EPSILON) {
    return vkr_quat_identity(); // No rotation for zero axis
  }

  // Normalize axis if needed
  Vec3 norm_axis = (axis_len_sq > 0.999f && axis_len_sq < 1.001f)
                       ? axis
                       : vec3_scale(axis, 1.0f / vkr_sqrt_f32(axis_len_sq));

  float32_t half_angle = angle * 0.5f;
  float32_t s = vkr_sin_f32(half_angle);
  float32_t c = vkr_cos_f32(half_angle);

  return vec4_new(norm_axis.x * s, norm_axis.y * s, norm_axis.z * s, c);
}

/**
 * @brief Creates a quaternion from Euler angles (XYZ order - right-handed
 * convention)
 * @param roll Rotation around X axis (radians)
 * @param pitch Rotation around Y axis (radians)
 * @param yaw Rotation around Z axis (radians)
 * @return Quaternion representing the combined rotation
 * @note Rotation order: first X (roll), then Y (pitch), then Z (yaw)
 * @note Right-handed coordinate system standard
 */
vkr_internal INLINE VkrQuat vkr_quat_from_euler(float32_t roll, float32_t pitch,
                                                float32_t yaw) {
  float32_t cr = vkr_cos_f32(roll * 0.5f);
  float32_t sr = vkr_sin_f32(roll * 0.5f);
  float32_t cp = vkr_cos_f32(pitch * 0.5f);
  float32_t sp = vkr_sin_f32(pitch * 0.5f);
  float32_t cy = vkr_cos_f32(yaw * 0.5f);
  float32_t sy = vkr_sin_f32(yaw * 0.5f);

  // XYZ order multiplication (right-handed standard)
  return vec4_new(sr * cp * cy + cr * sp * sy, // x
                  cr * sp * cy - sr * cp * sy, // y
                  cr * cp * sy + sr * sp * cy, // z
                  cr * cp * cy - sr * sp * sy  // w
  );
}

// ================================================
// Quaternion Operations
// ================================================

/**
 * @brief Normalizes a quaternion to unit length
 * @param q Quaternion to normalize
 * @return Unit quaternion
 */
vkr_internal INLINE VkrQuat vkr_quat_normalize(VkrQuat q) {
  return vec4_normalize(q);
}

/**
 * @brief Returns the magnitude (length) of a quaternion
 * @param q Input quaternion
 * @return Magnitude of the quaternion
 */
vkr_internal INLINE float32_t vkr_quat_length(VkrQuat q) {
  return vec4_length(q);
}

/**
 * @brief Returns the squared magnitude of a quaternion
 * @param q Input quaternion
 * @return Squared magnitude (avoids sqrt)
 */
vkr_internal INLINE float32_t vkr_quat_length_squared(VkrQuat q) {
  return vec4_length_squared(q);
}

/**
 * @brief Computes the conjugate of a quaternion
 * @param q Input quaternion
 * @return Conjugate quaternion (-x,-y,-z,w)
 */
vkr_internal INLINE VkrQuat vkr_quat_conjugate(VkrQuat q) {
  Vec4 mask = vec4_new(-1.0f, -1.0f, -1.0f, 1.0f);
  return vec4_mul(q, mask);
}

/**
 * @brief Computes the inverse of a quaternion
 * @param q Input quaternion
 * @return Inverse quaternion
 */
vkr_internal INLINE VkrQuat vkr_quat_inverse(VkrQuat q) {
  float32_t len_sq = vkr_quat_length_squared(q);
  if (len_sq > VKR_QUAT_EPSILON) {
    VkrQuat conj = vkr_quat_conjugate(q);
    return vec4_scale(conj, 1.0f / len_sq);
  }
  return vkr_quat_identity();
}

/**
 * @brief Multiplies two quaternions (SIMD-optimized)
 * @param a First quaternion (applied second)
 * @param b Second quaternion (applied first)
 * @return Combined rotation a*b
 *
 * Formula: (a*b).w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
 *          (a*b).x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y
 *          (a*b).y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x
 *          (a*b).z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
 *
 * Uses straightforward SIMD approach for better readability and correctness
 */
vkr_internal VkrQuat vkr_quat_mul(VkrQuat a, VkrQuat b) {
  // Calculate w: a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
  Vec4 a_for_w = vkr_simd_shuffle_f32x4(a, 3, 0, 1, 2); // [a.w, a.x, a.y, a.z]
  Vec4 b_for_w = vkr_simd_shuffle_f32x4(b, 3, 0, 1, 2); // [b.w, b.x, b.y, b.z]
  Vec4 sign_w = vkr_simd_set_f32x4(1.0f, -1.0f, -1.0f, -1.0f);
  Vec4 terms_w =
      vkr_simd_mul_f32x4(a_for_w, vkr_simd_mul_f32x4(b_for_w, sign_w));
  float32_t w = vkr_simd_hadd_f32x4(terms_w);

  // Calculate x: a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y
  Vec4 a_for_x = vkr_simd_shuffle_f32x4(a, 3, 0, 1, 2); // [a.w, a.x, a.y, a.z]
  Vec4 b_for_x = vkr_simd_shuffle_f32x4(b, 0, 3, 2, 1); // [b.x, b.w, b.z, b.y]
  Vec4 sign_x = vkr_simd_set_f32x4(1.0f, 1.0f, 1.0f, -1.0f);
  Vec4 terms_x =
      vkr_simd_mul_f32x4(a_for_x, vkr_simd_mul_f32x4(b_for_x, sign_x));
  float32_t x = vkr_simd_hadd_f32x4(terms_x);

  // Calculate y: a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x
  Vec4 a_for_y = vkr_simd_shuffle_f32x4(a, 3, 0, 1, 2); // [a.w, a.x, a.y, a.z]
  Vec4 b_for_y = vkr_simd_shuffle_f32x4(b, 1, 2, 3, 0); // [b.y, b.z, b.w, b.x]
  Vec4 sign_y = vkr_simd_set_f32x4(1.0f, -1.0f, 1.0f, 1.0f);
  Vec4 terms_y =
      vkr_simd_mul_f32x4(a_for_y, vkr_simd_mul_f32x4(b_for_y, sign_y));
  float32_t y = vkr_simd_hadd_f32x4(terms_y);

  // Calculate z: a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
  Vec4 a_for_z = vkr_simd_shuffle_f32x4(a, 3, 0, 1, 2); // [a.w, a.x, a.y, a.z]
  Vec4 b_for_z = vkr_simd_shuffle_f32x4(b, 2, 1, 0, 3); // [b.z, b.y, b.x, b.w]
  Vec4 sign_z = vkr_simd_set_f32x4(1.0f, 1.0f, -1.0f, 1.0f);
  Vec4 terms_z =
      vkr_simd_mul_f32x4(a_for_z, vkr_simd_mul_f32x4(b_for_z, sign_z));
  float32_t z = vkr_simd_hadd_f32x4(terms_z);

  return vec4_new(x, y, z, w);
}

/**
 * @brief Adds two quaternions (rarely used in practice)
 * @param a First quaternion
 * @param b Second quaternion
 * @return Sum of quaternions
 */
vkr_internal INLINE VkrQuat vkr_quat_add(VkrQuat a, VkrQuat b) {
  return vec4_add(a, b);
}

/**
 * @brief Subtracts two quaternions (rarely used in practice)
 * @param a First quaternion
 * @param b Second quaternion
 * @return Difference of quaternions
 */
vkr_internal INLINE VkrQuat vkr_quat_sub(VkrQuat a, VkrQuat b) {
  return vec4_sub(a, b);
}

/**
 * @brief Scales a quaternion by a scalar
 * @param q Quaternion to scale
 * @param s Scalar value
 * @return Scaled quaternion
 */
vkr_internal INLINE VkrQuat vkr_quat_scale(VkrQuat q, float32_t s) {
  return vec4_scale(q, s);
}

/**
 * @brief Computes dot product of two quaternions
 * @param a First quaternion
 * @param b Second quaternion
 * @return Dot product (scalar)
 */
vkr_internal INLINE float32_t vkr_quat_dot(VkrQuat a, VkrQuat b) {
  return vec4_dot(a, b);
}

/**
 * @brief Linear interpolation between quaternions
 * @param a Start quaternion
 * @param b End quaternion
 * @param t Interpolation factor [0,1]
 * @return Interpolated quaternion (normalized)
 */
vkr_internal INLINE VkrQuat vkr_quat_lerp(VkrQuat a, VkrQuat b, float32_t t) {
  // Check if we need to negate for shortest path
  float32_t dot = vkr_quat_dot(a, b);
  VkrQuat b_adjusted = dot < 0.0f ? vec4_negate(b) : b;
  return vkr_quat_normalize(vec4_lerp(a, b_adjusted, t));
}

/**
 * @brief Spherical linear interpolation between quaternions
 * @param a Start quaternion
 * @param b End quaternion
 * @param t Interpolation factor [0,1]
 * @return Smoothly interpolated quaternion
 */
vkr_internal INLINE VkrQuat vkr_quat_slerp(VkrQuat a, VkrQuat b, float32_t t) {
  // Normalize input quaternions
  VkrQuat q1 = vkr_quat_normalize(a);
  VkrQuat q2 = vkr_quat_normalize(b);

  // Calculate dot product to determine the shortest path
  float32_t dot = vkr_quat_dot(q1, q2);

  // If dot product is negative, negate q2 to take the shorter path
  VkrQuat q2_adjusted = q2;
  if (dot < 0.0f) {
    q2_adjusted = vec4_negate(q2);
    dot = -dot;
  }

  // If quaternions are very close, use linear interpolation
  if (dot > VKR_QUAT_SLERP_THRESHOLD) {
    return vkr_quat_lerp(q1, q2_adjusted, t);
  }

  // Calculate the angle between quaternions
  float32_t theta = vkr_acos_f32(dot);

  // Pre-compute reciprocal of sin(theta) for performance
  float32_t inv_sin_theta = 1.0f / vkr_sin_f32(theta);

  // Calculate interpolation weights using pre-computed reciprocal
  float32_t w1 = vkr_sin_f32((1.0f - t) * theta) * inv_sin_theta;
  float32_t w2 = vkr_sin_f32(t * theta) * inv_sin_theta;

  // Perform spherical linear interpolation using SIMD operations
  Vec4 w1_vec = vec4_new(w1, w1, w1, w1);
  Vec4 w2_vec = vec4_new(w2, w2, w2, w2);

  return vec4_add(
      vec4_mul(q1, w1_vec),
      vec4_mul(q2_adjusted, w2_vec)); // Already normalized by construction
}

// ================================================
// Rotation Operations
// ================================================

/**
 * @brief Rotates a 3D vector by a quaternion (SIMD-optimized)
 * @param q Rotation quaternion (should be normalized)
 * @param v Vector to rotate
 * @return Rotated vector
 *
 * Uses the optimized Rodrigues' formula: v' = v + 2 * q.xyz × (q.xyz × v + q.w
 * * v) This is mathematically equivalent to: v' = q * v * q^-1
 */
vkr_internal INLINE Vec3 vkr_quat_rotate_vec3(VkrQuat q, Vec3 v) {
  // First cross product: q × v
  Vec4 c1 = vec4_cross3(q, v);

  // Scale by q.w and add to c1: (qv × v + q.w * v)
  Vec4 c1_plus_wv = vec4_muladd(v, vec4_new(q.w, q.w, q.w, 0.0f), c1);

  // Second cross product: qv × (qv × v + q.w * v)
  Vec3 c2 = vec3_cross(q, c1_plus_wv);

  // Final result: v + 2 * c2
  return vec3_add(v, vec3_scale(c2, 2.0f));
}

/**
 * @brief Creates a look-at quaternion (right-handed system)
 * @param forward Forward direction (normalized)
 * @param up Up direction (normalized)
 * @return Quaternion representing the rotation
 * @note In right-handed system: Right = Forward × Up, Up = Right × Forward
 */
vkr_internal INLINE VkrQuat vkr_quat_look_at(Vec3 forward, Vec3 up) {
  // Ensure inputs are normalized
  Vec3 f = vec3_normalize(forward);
  Vec3 u = vec3_normalize(up);

  // Calculate right vector (Forward × Up for right-handed)
  Vec3 r = vec3_normalize(vec3_cross(f, u));

  // Recalculate up to ensure orthogonality (Right × Forward)
  u = vec3_cross(r, f);

  // Build rotation matrix (column-major: right, up, -forward)
  // In right-handed system, we look down negative Z, so we need -forward for Z
  // column
  float32_t m00 = r.x, m01 = u.x, m02 = -f.x;
  float32_t m10 = r.y, m11 = u.y, m12 = -f.y;
  float32_t m20 = r.z, m21 = u.z, m22 = -f.z;

  // Convert rotation matrix to quaternion using Shepperd's method
  float32_t trace = m00 + m11 + m22;

  if (trace > 0.0f) {
    float32_t s = vkr_sqrt_f32(trace + 1.0f) * 2.0f;    // s = 4 * qw
    return vkr_quat_normalize(vec4_new((m21 - m12) / s, // qx
                                       (m02 - m20) / s, // qy
                                       (m10 - m01) / s, // qz
                                       0.25f * s        // qw
                                       ));
  } else if (m00 > m11 && m00 > m22) {
    float32_t s = vkr_sqrt_f32(1.0f + m00 - m11 - m22) * 2.0f; // s = 4 * qx
    return vkr_quat_normalize(vec4_new(0.25f * s,              // qx
                                       (m01 + m10) / s,        // qy
                                       (m02 + m20) / s,        // qz
                                       (m21 - m12) / s         // qw
                                       ));
  } else if (m11 > m22) {
    float32_t s = vkr_sqrt_f32(1.0f + m11 - m00 - m22) * 2.0f; // s = 4 * qy
    return vkr_quat_normalize(vec4_new((m01 + m10) / s,        // qx
                                       0.25f * s,              // qy
                                       (m12 + m21) / s,        // qz
                                       (m02 - m20) / s         // qw
                                       ));
  } else {
    float32_t s = vkr_sqrt_f32(1.0f + m22 - m00 - m11) * 2.0f; // s = 4 * qz
    return vkr_quat_normalize(vec4_new((m02 + m20) / s,        // qx
                                       (m12 + m21) / s,        // qy
                                       0.25f * s,              // qz
                                       (m10 - m01) / s         // qw
                                       ));
  }
}

/**
 * @brief Extracts Euler angles from quaternion (XYZ order - right-handed
 * convention)
 * @param q Input quaternion
 * @param[out] roll Rotation around X axis (radians)
 * @param[out] pitch Rotation around Y axis (radians)
 * @param[out] yaw Rotation around Z axis (radians)
 * @note Rotation order: first X (roll), then Y (pitch), then Z (yaw)
 * @note Right-handed coordinate system standard
 */
vkr_internal INLINE void vkr_quat_to_euler(VkrQuat q, float32_t *roll,
                                           float32_t *pitch, float32_t *yaw) {
  // Convert quaternion to rotation matrix elements we need
  float32_t xx = q.x * q.x;
  float32_t yy = q.y * q.y;
  float32_t zz = q.z * q.z;
  float32_t xy = q.x * q.y;
  float32_t xz = q.x * q.z;
  float32_t yz = q.y * q.z;
  float32_t wx = q.w * q.x;
  float32_t wy = q.w * q.y;
  float32_t wz = q.w * q.z;

  // Extract angles for XYZ order (right-handed)
  float32_t sinp = 2.0f * (wy - xz);

  if (vkr_abs_f32(sinp) >= VKR_QUAT_GIMBAL_LOCK_THRESHOLD) {
    // Gimbal lock case: pitch = ±90°
    *pitch = copysignf(VKR_HALF_PI, sinp);
    *roll = vkr_atan2_f32(-2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy));
    *yaw = 0.0f; // Set yaw to 0 in gimbal lock
  } else {
    *pitch = vkr_asin_f32(vkr_clamp_f32(sinp, -1.0f, 1.0f));
    *roll = vkr_atan2_f32(2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy));
    *yaw = vkr_atan2_f32(2.0f * (xy + wz), 1.0f - 2.0f * (yy + zz));
  }
}

/**
 * @brief Gets the angle of rotation from a quaternion
 * @param q Input quaternion
 * @return Angle in radians [0, 2π]
 */
vkr_internal INLINE float32_t vkr_quat_angle(VkrQuat q) {
  return 2.0f * vkr_acos_f32(vkr_clamp_f32(q.w, -1.0f, 1.0f));
}

/**
 * @brief Gets the rotation axis from a quaternion with improved numerical
 * stability
 * @param q Input quaternion
 * @return Normalized rotation axis (or forward vector if no rotation)
 * @note Improved precision for small angles using alternative computation
 */
vkr_internal INLINE Vec3 vkr_quat_axis(VkrQuat q) {
  // For small angles, use alternative stable computation
  // When angle is small, sin(angle/2) ≈ angle/2, so we can use the vector part
  // directly
  Vec3 vec_part = vec3_new(q.x, q.y, q.z);
  float32_t vec_length_sq = vec3_length_squared(vec_part);

  // Threshold for small angle detection (roughly 0.1 radians or ~5.7 degrees)
  const float32_t small_angle_threshold_sq = 0.0025f; // (0.05)^2

  if (vec_length_sq < small_angle_threshold_sq) {
    // For very small rotations, use the vector part directly if it's
    // significant
    if (vec_length_sq > VKR_QUAT_EPSILON * VKR_QUAT_EPSILON) {
      float32_t inv_vec_length = 1.0f / vkr_sqrt_f32(vec_length_sq);
      return vec3_scale(vec_part, inv_vec_length);
    } else {
      // Essentially no rotation, return arbitrary normalized axis
      return vec3_new(0.0f, 0.0f, 1.0f);
    }
  } else {
    // Standard computation for larger angles
    float32_t s = vkr_sqrt_f32(1.0f - q.w * q.w);
    if (s < VKR_QUAT_EPSILON) {
      return vec3_new(0.0f, 0.0f, 1.0f); // No rotation, return arbitrary axis
    }
    return vec3_scale(vec_part, 1.0f / s);
  }
}
