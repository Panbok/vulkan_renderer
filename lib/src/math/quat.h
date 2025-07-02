#pragma once

#include "defines.h"
#include "vec.h"

// ================================================
// Quaternion Constants
// ================================================

/**
 * @brief Threshold for switching from slerp to lerp
 * When quaternions are very close (dot product > 0.9995),
 * linear interpolation is more numerically stable
 */
#define QUAT_SLERP_THRESHOLD 0.9995f

/**
 * @brief Epsilon for quaternion operations
 * Used for checking near-zero conditions in normalization and axis extraction
 */
#define QUAT_EPSILON FLOAT_EPSILON

/**
 * @brief Gimbal lock threshold for Euler angle extraction
 * When pitch is within this range of ±90°, we're in gimbal lock territory
 */
#define QUAT_GIMBAL_LOCK_THRESHOLD 0.99999f

/**
 * @file quat.h
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
typedef Vec4 Quat;

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
static INLINE Quat quat_new(float32_t x, float32_t y, float32_t z,
                            float32_t w) {
  return vec4_new(x, y, z, w);
}

/**
 * @brief Returns the identity quaternion (no rotation)
 * @return Quaternion representing no rotation (0,0,0,1)
 */
static INLINE Quat quat_identity(void) {
  return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a quaternion from axis-angle representation
 * @param axis Normalized rotation axis
 * @param angle Rotation angle in radians
 * @return Quaternion representing the rotation
 */
static INLINE Quat quat_from_axis_angle(Vec3 axis, float32_t angle) {
  // Validate axis is not zero
  float32_t axis_len_sq = vec3_length_squared(axis);
  if (axis_len_sq < QUAT_EPSILON) {
    return quat_identity(); // No rotation for zero axis
  }

  // Normalize axis if needed
  Vec3 norm_axis = (axis_len_sq > 0.999f && axis_len_sq < 1.001f)
                       ? axis
                       : vec3_scale(axis, 1.0f / sqrt_f32(axis_len_sq));

  float32_t half_angle = angle * 0.5f;
  float32_t s = sin_f32(half_angle);
  float32_t c = cos_f32(half_angle);

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
static INLINE Quat quat_from_euler(float32_t roll, float32_t pitch,
                                   float32_t yaw) {
  float32_t cr = cos_f32(roll * 0.5f);
  float32_t sr = sin_f32(roll * 0.5f);
  float32_t cp = cos_f32(pitch * 0.5f);
  float32_t sp = sin_f32(pitch * 0.5f);
  float32_t cy = cos_f32(yaw * 0.5f);
  float32_t sy = sin_f32(yaw * 0.5f);

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
static INLINE Quat quat_normalize(Quat q) { return vec4_normalize(q); }

/**
 * @brief Returns the magnitude (length) of a quaternion
 * @param q Input quaternion
 * @return Magnitude of the quaternion
 */
static INLINE float32_t quat_length(Quat q) { return vec4_length(q); }

/**
 * @brief Returns the squared magnitude of a quaternion
 * @param q Input quaternion
 * @return Squared magnitude (avoids sqrt)
 */
static INLINE float32_t quat_length_squared(Quat q) {
  return vec4_length_squared(q);
}

/**
 * @brief Computes the conjugate of a quaternion
 * @param q Input quaternion
 * @return Conjugate quaternion (-x,-y,-z,w)
 */
static INLINE Quat quat_conjugate(Quat q) {
  Vec4 mask = vec4_new(-1.0f, -1.0f, -1.0f, 1.0f);
  return vec4_mul(q, mask);
}

/**
 * @brief Computes the inverse of a quaternion
 * @param q Input quaternion
 * @return Inverse quaternion
 */
static INLINE Quat quat_inverse(Quat q) {
  float32_t len_sq = quat_length_squared(q);
  if (len_sq > QUAT_EPSILON) {
    Quat conj = quat_conjugate(q);
    return vec4_scale(conj, 1.0f / len_sq);
  }
  return quat_identity();
}

/**
 * @brief Multiplies two quaternions (PROPER Hamilton product)
 * @param a First quaternion (applied second)
 * @param b Second quaternion (applied first)
 * @return Combined rotation a*b
 *
 * Formula: (a*b).w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
 *          (a*b).x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y
 *          (a*b).y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x
 *          (a*b).z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
 */
static INLINE Quat quat_mul(Quat a, Quat b) {
  // SIMD-optimized quaternion multiplication
  Vec4 a_wxyz = simd_shuffle_f32x4(a, 3, 0, 1, 2); // (w,x,y,z)
  Vec4 a_zwxy = simd_shuffle_f32x4(a, 2, 3, 0, 1); // (z,w,x,y)
  Vec4 a_yzwx = simd_shuffle_f32x4(a, 1, 2, 3, 0); // (y,z,w,x)

  Vec4 b_wxyz = simd_shuffle_f32x4(b, 3, 0, 1, 2); // (w,x,y,z)
  Vec4 b_zwxy = simd_shuffle_f32x4(b, 2, 3, 0, 1); // (z,w,x,y)
  Vec4 b_yzwx = simd_shuffle_f32x4(b, 1, 2, 3, 0); // (y,z,w,x)

  // Signs for the multiplication
  Vec4 sign1 = vec4_new(1.0f, 1.0f, 1.0f, -1.0f);
  Vec4 sign2 = vec4_new(-1.0f, 1.0f, -1.0f, -1.0f);
  Vec4 sign3 = vec4_new(-1.0f, -1.0f, 1.0f, -1.0f);

  // Compute result using FMA operations
  Vec4 result = vec4_mul(vec4_mul(a, b_wxyz), sign1);
  result = vec4_muladd(vec4_mul(a_wxyz, b), sign1, result);
  result = vec4_muladd(vec4_mul(a_yzwx, b_zwxy), sign2, result);
  result = vec4_muladd(vec4_mul(a_zwxy, b_yzwx), sign3, result);

  return result;
}

/**
 * @brief Adds two quaternions (rarely used in practice)
 * @param a First quaternion
 * @param b Second quaternion
 * @return Sum of quaternions
 */
static INLINE Quat quat_add(Quat a, Quat b) { return vec4_add(a, b); }

/**
 * @brief Subtracts two quaternions (rarely used in practice)
 * @param a First quaternion
 * @param b Second quaternion
 * @return Difference of quaternions
 */
static INLINE Quat quat_sub(Quat a, Quat b) { return vec4_sub(a, b); }

/**
 * @brief Scales a quaternion by a scalar
 * @param q Quaternion to scale
 * @param s Scalar value
 * @return Scaled quaternion
 */
static INLINE Quat quat_scale(Quat q, float32_t s) { return vec4_scale(q, s); }

/**
 * @brief Computes dot product of two quaternions
 * @param a First quaternion
 * @param b Second quaternion
 * @return Dot product (scalar)
 */
static INLINE float32_t quat_dot(Quat a, Quat b) { return vec4_dot(a, b); }

/**
 * @brief Linear interpolation between quaternions
 * @param a Start quaternion
 * @param b End quaternion
 * @param t Interpolation factor [0,1]
 * @return Interpolated quaternion (normalized)
 */
static INLINE Quat quat_lerp(Quat a, Quat b, float32_t t) {
  // Check if we need to negate for shortest path
  float32_t dot = quat_dot(a, b);
  Quat b_adjusted = dot < 0.0f ? vec4_negate(b) : b;
  return quat_normalize(vec4_lerp(a, b_adjusted, t));
}

/**
 * @brief Spherical linear interpolation between quaternions
 * @param a Start quaternion
 * @param b End quaternion
 * @param t Interpolation factor [0,1]
 * @return Smoothly interpolated quaternion
 */
static INLINE Quat quat_slerp(Quat a, Quat b, float32_t t) {
  // Normalize input quaternions
  Quat q1 = quat_normalize(a);
  Quat q2 = quat_normalize(b);

  // Calculate dot product to determine the shortest path
  float32_t dot = quat_dot(q1, q2);

  // If dot product is negative, negate q2 to take the shorter path
  Quat q2_adjusted = q2;
  if (dot < 0.0f) {
    q2_adjusted = vec4_negate(q2);
    dot = -dot;
  }

  // If quaternions are very close, use linear interpolation
  if (dot > QUAT_SLERP_THRESHOLD) {
    return quat_lerp(q1, q2_adjusted, t);
  }

  // Calculate the angle between quaternions
  float32_t theta = acos_f32(dot);

  // Pre-compute reciprocal of sin(theta) for performance
  float32_t inv_sin_theta = 1.0f / sin_f32(theta);

  // Calculate interpolation weights using pre-computed reciprocal
  float32_t w1 = sin_f32((1.0f - t) * theta) * inv_sin_theta;
  float32_t w2 = sin_f32(t * theta) * inv_sin_theta;

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
static INLINE Vec3 quat_rotate_vec3(Quat q, Vec3 v) {
  // Convert Vec3 to Vec4 for SIMD operations (w=0 for pure vector)
  Vec4 v4 = vec4_new(v.x, v.y, v.z, 0.0f);

  // Extract quaternion vector part as Vec4
  Vec4 qv = vec4_new(q.x, q.y, q.z, 0.0f);

  // First cross product: qv × v
  Vec4 c1 = vec4_new(q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z,
                     q.x * v.y - q.y * v.x, 0.0f);

  // Scale by q.w and add to c1: (qv × v + q.w * v)
  Vec4 c1_plus_wv = vec4_muladd(v4, vec4_new(q.w, q.w, q.w, 0.0f), c1);

  // Second cross product: qv × (qv × v + q.w * v)
  Vec3 temp = vec3_new(c1_plus_wv.x, c1_plus_wv.y, c1_plus_wv.z);
  Vec3 c2 = vec3_cross(vec3_new(q.x, q.y, q.z), temp);

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
static INLINE Quat quat_look_at(Vec3 forward, Vec3 up) {
  // Ensure forward is normalized
  Vec3 f = vec3_normalize(forward);

  // Calculate right vector (Forward × Up for right-handed)
  Vec3 r = vec3_normalize(vec3_cross(f, up));

  // Recalculate up to ensure orthogonality (Right × Forward)
  Vec3 u = vec3_cross(r, f);

  // Convert rotation matrix to quaternion
  float32_t trace = r.x + u.y + f.z;

  if (trace > 0.0f) {
    float32_t s = 0.5f / sqrt_f32(trace + 1.0f);
    return vec4_new((u.z - f.y) * s, (f.x - r.z) * s, (r.y - u.x) * s,
                    0.25f / s);
  } else if (r.x > u.y && r.x > f.z) {
    float32_t s = 2.0f * sqrt_f32(1.0f + r.x - u.y - f.z);
    return vec4_new(0.25f * s, (u.x + r.y) / s, (f.x + r.z) / s,
                    (u.z - f.y) / s);
  } else if (u.y > f.z) {
    float32_t s = 2.0f * sqrt_f32(1.0f + u.y - r.x - f.z);
    return vec4_new((u.x + r.y) / s, 0.25f * s, (f.y + u.z) / s,
                    (f.x - r.z) / s);
  } else {
    float32_t s = 2.0f * sqrt_f32(1.0f + f.z - r.x - u.y);
    return vec4_new((f.x + r.z) / s, (f.y + u.z) / s, 0.25f * s,
                    (r.y - u.x) / s);
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
static INLINE void quat_to_euler(Quat q, float32_t *roll, float32_t *pitch,
                                 float32_t *yaw) {
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

  if (abs_f32(sinp) >= QUAT_GIMBAL_LOCK_THRESHOLD) {
    // Gimbal lock case: pitch = ±90°
    *pitch = copysignf(HALF_PI, sinp);
    *roll = atan2_f32(-2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy));
    *yaw = 0.0f; // Set yaw to 0 in gimbal lock
  } else {
    *pitch = asin_f32(clamp_f32(sinp, -1.0f, 1.0f));
    *roll = atan2_f32(2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy));
    *yaw = atan2_f32(2.0f * (xy + wz), 1.0f - 2.0f * (yy + zz));
  }
}

/**
 * @brief Gets the angle of rotation from a quaternion
 * @param q Input quaternion
 * @return Angle in radians [0, 2π]
 */
static INLINE float32_t quat_angle(Quat q) {
  return 2.0f * acos_f32(clamp_f32(q.w, -1.0f, 1.0f));
}

/**
 * @brief Gets the rotation axis from a quaternion
 * @param q Input quaternion
 * @return Normalized rotation axis (or forward vector if no rotation)
 */
static INLINE Vec3 quat_axis(Quat q) {
  float32_t s = sqrt_f32(1.0f - q.w * q.w);
  if (s < QUAT_EPSILON) {
    return vec3_new(0.0f, 0.0f, 1.0f); // No rotation, return arbitrary axis
  }
  return vec3_scale(vec3_new(q.x, q.y, q.z), 1.0f / s);
}
