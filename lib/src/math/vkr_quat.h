/**
 * @file vkr_quat.h
 * @brief Quaternion rotations stored as Vec4 {x, y, z, w}, w the scalar part.
 *
 * Right-handed. Rotation functions expect unit quaternions, and a * b applies
 * b first. Euler angles are radians: vkr_quat_from_euler rotates about X,
 * then the rotated Y, then the rotated Z (equivalently fixed Z, Y, X), and
 * vkr_quat_to_euler inverts it.
 */

#pragma once

#include "defines.h"
#include "vec.h"
#include "vkr_simd.h"

// ================================================
// Quaternion Constants
// ================================================

/* Above this dot product slerp falls back to normalized lerp, where sin(theta)
 * is too small to divide by. */
#define VKR_QUAT_SLERP_THRESHOLD 0.9995f

/* Near-zero threshold for axis lengths and the inverse. */
#define VKR_QUAT_EPSILON VKR_FLOAT_EPSILON

/* |sin(pitch)| at or above which vkr_quat_to_euler treats pitch as +-90
 * degrees. */
#define VKR_QUAT_GIMBAL_LOCK_THRESHOLD 0.99999f

typedef Vec4 VkrQuat;

// ================================================
// Quaternion Construction
// ================================================

vkr_internal INLINE VkrQuat vkr_quat_new(float32_t x, float32_t y, float32_t z,
                                         float32_t w) {
  return vec4_new(x, y, z, w);
}

vkr_internal INLINE VkrQuat vkr_quat_identity(void) {
  return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
}

/* Angle in radians. A zero axis returns the identity; other axes are
 * normalized unless their squared length is already within 0.1% of one. */
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

/* Radians. Builds qx(roll) * qy(pitch) * qz(yaw). */
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

/* Returns the zero quaternion when the length is at most VKR_FLOAT_EPSILON. */
vkr_internal INLINE VkrQuat vkr_quat_normalize(VkrQuat q) {
  return vec4_normalize(q);
}

vkr_internal INLINE float32_t vkr_quat_length(VkrQuat q) {
  return vec4_length(q);
}

vkr_internal INLINE float32_t vkr_quat_length_squared(VkrQuat q) {
  return vec4_length_squared(q);
}

vkr_internal INLINE VkrQuat vkr_quat_conjugate(VkrQuat q) {
  Vec4 mask = vec4_new(-1.0f, -1.0f, -1.0f, 1.0f);
  return vec4_mul(q, mask);
}

/* Conjugate over the squared length; a near-zero quaternion returns the
 * identity. */
vkr_internal INLINE VkrQuat vkr_quat_inverse(VkrQuat q) {
  float32_t len_sq = vkr_quat_length_squared(q);
  if (len_sq > VKR_QUAT_EPSILON) {
    VkrQuat conj = vkr_quat_conjugate(q);
    return vec4_scale(conj, 1.0f / len_sq);
  }
  return vkr_quat_identity();
}

/* a * b applies b first, then a. */
vkr_internal INLINE VkrQuat vkr_quat_mul(VkrQuat a, VkrQuat b) {
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

vkr_internal INLINE VkrQuat vkr_quat_add(VkrQuat a, VkrQuat b) {
  return vec4_add(a, b);
}

vkr_internal INLINE VkrQuat vkr_quat_sub(VkrQuat a, VkrQuat b) {
  return vec4_sub(a, b);
}

vkr_internal INLINE VkrQuat vkr_quat_scale(VkrQuat q, float32_t s) {
  return vec4_scale(q, s);
}

vkr_internal INLINE float32_t vkr_quat_dot(VkrQuat a, VkrQuat b) {
  return vec4_dot(a, b);
}

/* Takes the shorter arc and normalizes the result. */
vkr_internal INLINE VkrQuat vkr_quat_lerp(VkrQuat a, VkrQuat b, float32_t t) {
  // Check if we need to negate for shortest path
  float32_t dot = vkr_quat_dot(a, b);
  VkrQuat b_adjusted = dot < 0.0f ? vec4_negate(b) : b;
  return vkr_quat_normalize(vec4_lerp(a, b_adjusted, t));
}

/* Normalizes both inputs and takes the shorter arc; nearly parallel inputs use
 * vkr_quat_lerp. */
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

/* Expects a unit quaternion. Computes v + 2 q.xyz x (q.xyz x v + q.w v), which
 * equals q v q^-1. */
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

/* The result maps -Z to forward and +Y toward up. Inputs need not be unit
 * length but must not be parallel. */
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

/* Inverts vkr_quat_from_euler. At the gimbal-lock threshold yaw is 0 and roll
 * carries the combined angle. */
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

  // vkr_quat_from_euler builds qx(roll) * qy(pitch) * qz(yaw), whose matrix
  // Rx * Ry * Rz has m02 = sin(pitch), m12 = -sin(roll) cos(pitch) and
  // m01 = -cos(pitch) sin(yaw).
  float32_t sinp = 2.0f * (xz + wy);

  if (vkr_abs_f32(sinp) >= VKR_QUAT_GIMBAL_LOCK_THRESHOLD) {
    // cos(pitch) is ~0, so only roll + yaw (pitch > 0) or roll - yaw
    // (pitch < 0) is determined; yaw is fixed at 0.
    *pitch = vkr_copysign_f32(VKR_HALF_PI, sinp);
    const float32_t m10 = 2.0f * (xy + wz);
    const float32_t m11 = 1.0f - 2.0f * (xx + zz);
    *roll = vkr_atan2_f32(sinp > 0.0f ? m10 : -m10, m11);
    *yaw = 0.0f;
  } else {
    *pitch = vkr_asin_f32(vkr_clamp_f32(sinp, -1.0f, 1.0f));
    *roll = vkr_atan2_f32(2.0f * (wx - yz), 1.0f - 2.0f * (xx + yy));
    *yaw = vkr_atan2_f32(2.0f * (wz - xy), 1.0f - 2.0f * (yy + zz));
  }
}

/* Radians in [0, 2 pi]. */
vkr_internal INLINE float32_t vkr_quat_angle(VkrQuat q) {
  return 2.0f * vkr_acos_f32(vkr_clamp_f32(q.w, -1.0f, 1.0f));
}

/* Unit rotation axis; +Z when the rotation is too small to define one. */
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
