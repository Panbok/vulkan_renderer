#pragma once

#include "defines.h"
#include "vec.h"
#include "vkr_quat.h"
#include "vkr_simd.h"

/**
 * @file mat.h
 * @brief Column-major 4x4 float matrices.
 *
 * Right-handed: X right, Y up, Z toward the viewer, so cameras look down -Z.
 * Storage is column-major (elements[col * 4 + row]): columns 0-2 are the
 * images of +X, +Y and +Z and column 3 is the translation. Matrices multiply
 * column vectors, so mat4_mul(a, b) applies b first. Angles are radians, and a
 * positive rotation turns counter-clockwise seen from the tip of its axis, as
 * vkr_quat_from_axis_angle does. Renderer projections use depth in [0, 1] with
 * clip-space Y inverted (mat4_perspective, mat4_ortho_zo_yinv).
 */

typedef VKR_SIMD_ALIGN union Mat4 {
  // Column-major layout (OpenGL/GLM style)
  struct {
    Vec4 col0, col1, col2, col3;
  } columns;

  Vec4 cols[4];           // Array of columns
  float32_t elements[16]; // Flat array access (column-major order)

  // Direct element access (column-major)
  struct {
    float32_t m00, m10, m20, m30; // First column (x-axis)
    float32_t m01, m11, m21, m31; // Second column (y-axis)
    float32_t m02, m12, m22, m32; // Third column (z-axis)
    float32_t m03, m13, m23, m33; // Fourth column (translation)
  };
} Mat4;

// Conservative affine sphere stretch: ||A||2 <= sqrt(||transpose(A) A||inf).
// Double intermediates avoid overflow for finite float32 transforms. One
// outward float32 ULP covers both intermediate and final conversion rounding.
static INLINE float32_t mat4_affine_sphere_scale(Mat4 model) {
  const float64_t a0 = model.m00, a1 = model.m10, a2 = model.m20;
  const float64_t b0 = model.m01, b1 = model.m11, b2 = model.m21;
  const float64_t c0 = model.m02, c1 = model.m12, c2 = model.m22;
  const float64_t ab = fabs(a0 * b0 + a1 * b1 + a2 * b2);
  const float64_t ac = fabs(a0 * c0 + a1 * c1 + a2 * c2);
  const float64_t bc = fabs(b0 * c0 + b1 * c1 + b2 * c2);
  const float64_t row0 = a0 * a0 + a1 * a1 + a2 * a2 + ab + ac;
  const float64_t row1 = b0 * b0 + b1 * b1 + b2 * b2 + ab + bc;
  const float64_t row2 = c0 * c0 + c1 * c1 + c2 * c2 + ac + bc;
  const float64_t bound = sqrt(Max(row0, Max(row1, row2)));
  const float32_t rounded = (float32_t)bound;
  return rounded > 0.0f && isfinite(rounded) ? nextafterf(rounded, INFINITY)
                                             : rounded;
}

/* True when the upper 3x3 has a negative determinant (a reflection), matching
 * the prepared normal-basis sign. */
static INLINE bool8_t mat4_affine_mirrored(Mat4 model) {
  const float64_t a0 = model.m00, a1 = model.m10, a2 = model.m20;
  const float64_t b0 = model.m01, b1 = model.m11, b2 = model.m21;
  const float64_t c0 = model.m02, c1 = model.m12, c2 = model.m22;
  const float64_t determinant = a0 * (b1 * c2 - b2 * c1) +
                                a1 * (b2 * c0 - b0 * c2) +
                                a2 * (b0 * c1 - b1 * c0);
  return determinant < 0.0;
}

// =============================================================================
// Matrix Constructor Functions
// =============================================================================

/* Arguments are in column-major order, column 0 first. */
static INLINE Mat4 mat4_new(float32_t m00, float32_t m10, float32_t m20,
                            float32_t m30, float32_t m01, float32_t m11,
                            float32_t m21, float32_t m31, float32_t m02,
                            float32_t m12, float32_t m22, float32_t m32,
                            float32_t m03, float32_t m13, float32_t m23,
                            float32_t m33) {
  return (Mat4){
      .columns =
          {
              .col0 = vec4_new(m00, m10, m20, m30),
              .col1 = vec4_new(m01, m11, m21, m31),
              .col2 = vec4_new(m02, m12, m22, m32),
              .col3 = vec4_new(m03, m13, m23, m33),
          },
  };
}

static INLINE Mat4 mat4_zero(void) {
  return (Mat4){
      .columns =
          {
              .col0 = vec4_zero(),
              .col1 = vec4_zero(),
              .col2 = vec4_zero(),
              .col3 = vec4_zero(),
          },
  };
}

static INLINE Mat4 mat4_identity(void) {
  return (Mat4){
      .columns =
          {
              .col0 = vec4_new(1.0f, 0.0f, 0.0f, 0.0f),
              .col1 = vec4_new(0.0f, 1.0f, 0.0f, 0.0f),
              .col2 = vec4_new(0.0f, 0.0f, 1.0f, 0.0f),
              .col3 = vec4_new(0.0f, 0.0f, 0.0f, 1.0f),
          },
  };
}

/* OpenGL depth convention: maps z = -near..-far to [-1, 1] with Y up. Renderer
 * cameras and shadows use mat4_ortho_zo_yinv. */
static INLINE Mat4 mat4_ortho(float32_t left, float32_t right, float32_t bottom,
                              float32_t top, float32_t near_clip,
                              float32_t far_clip) {
  float32_t tx = -((right + left) / (right - left));
  float32_t ty = -((top + bottom) / (top - bottom));
  float32_t tz = -((far_clip + near_clip) / (far_clip - near_clip));
  return mat4_new(2.0f / (right - left), 0.0f, 0.0f, 0.0f, 0.0f,
                  2.0f / (top - bottom), 0.0f, 0.0f, 0.0f, 0.0f,
                  -2.0f / (far_clip - near_clip), 0.0f, tx, ty, tz, 1.0f);
}

/* Renderer convention: maps z = -near to depth 0 and z = -far to 1, and inverts
 * clip-space Y like mat4_perspective. */
static INLINE Mat4 mat4_ortho_zo_yinv(float32_t left, float32_t right,
                                      float32_t bottom, float32_t top,
                                      float32_t near_clip, float32_t far_clip) {
  float32_t rl = (right - left);
  float32_t tb = (top - bottom);
  float32_t fn = (far_clip - near_clip);

  return mat4_new(2.0f / rl, 0.0f, 0.0f, 0.0f, 0.0f, -2.0f / tb, 0.0f, 0.0f,
                  0.0f, 0.0f, -1.0f / fn, 0.0f, -((right + left) / rl),
                  +((top + bottom) / tb), -(near_clip / fn), 1.0f);
}

/* Vertical fov in radians. Maps z = -near to depth 0 and z = -far to 1, with
 * clip-space Y inverted. */
static INLINE Mat4 mat4_perspective(float32_t fov, float32_t aspect,
                                    float32_t near_clip, float32_t far_clip) {
  // Right-handed perspective matrix for Vulkan (Z range [0,1])
  float32_t f = 1.0f / vkr_tan_f32(fov * 0.5f);

  // Note: Vulkan clip space has inverted Y (top = -1, bottom = +1)
  // We negate the Y scaling to account for this
  return mat4_new(f / aspect, 0.0f, 0.0f, 0.0f, 0.0f, -f, 0.0f,
                  0.0f, // Negated Y for Vulkan
                  0.0f, 0.0f, far_clip / (near_clip - far_clip), -1.0f, 0.0f,
                  0.0f, (far_clip * near_clip) / (near_clip - far_clip), 0.0f);
}

/* World-to-view matrix looking from eye toward center down -Z. up need not be
 * unit length or perpendicular, but must not be parallel to the view direction.
 */
static INLINE Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up) {
  // Right-handed coordinate system: camera looks down -Z
  Vec3 f = vec3_normalize(vec3_sub(center, eye)); // Forward direction
  Vec3 s = vec3_normalize(vec3_cross(f, up));     // Right = Forward × Up
  Vec3 u = vec3_cross(s, f);                      // Up = Right × Forward

  // View matrix for right-handed system
  return mat4_new(s.x, u.x, -f.x, 0.0f, s.y, u.y, -f.y, 0.0f, s.z, u.z, -f.z,
                  0.0f, -vec3_dot(s, eye), -vec3_dot(u, eye), vec3_dot(f, eye),
                  1.0f);
}

static INLINE Mat4 mat4_translate(Vec3 v) {
  return mat4_new(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  1.0f, 0.0f, v.x, v.y, v.z, 1.0f);
}

static INLINE Mat4 mat4_scale(Vec3 v) {
  return mat4_new(v.x, 0.0f, 0.0f, 0.0f, 0.0f, v.y, 0.0f, 0.0f, 0.0f, 0.0f, v.z,
                  0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

/* Rotates by angle about axis, which is normalized here; equals
 * vkr_quat_to_mat4(vkr_quat_from_axis_angle(axis, angle)). */
static INLINE Mat4 mat4_euler_rotate(Vec3 axis, float32_t angle) {
  axis = vec3_normalize(axis); // Ensure axis is normalized
  float32_t s = vkr_sin_f32(angle);
  float32_t c = vkr_cos_f32(angle);
  float32_t t = 1.0f - c;
  // Columns are the images of +X, +Y and +Z, matching vkr_quat_to_mat4.
  return mat4_new(
      t * axis.x * axis.x + c, t * axis.x * axis.y + s * axis.z,
      t * axis.x * axis.z - s * axis.y, 0.0f, t * axis.x * axis.y - s * axis.z,
      t * axis.y * axis.y + c, t * axis.y * axis.z + s * axis.x, 0.0f,
      t * axis.x * axis.z + s * axis.y, t * axis.y * axis.z - s * axis.x,
      t * axis.z * axis.z + c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

/* Positive angles turn +Y toward +Z. */
static INLINE Mat4 mat4_euler_rotate_x(float32_t angle) {
  float32_t s = vkr_sin_f32(angle);
  float32_t c = vkr_cos_f32(angle);
  // Right-handed rotation around X: positive angle rotates Y toward Z
  return mat4_new(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, c, s, 0.0f, 0.0f, -s, c, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

/* Positive angles turn +Z toward +X. */
static INLINE Mat4 mat4_euler_rotate_y(float32_t angle) {
  float32_t s = vkr_sin_f32(angle);
  float32_t c = vkr_cos_f32(angle);
  // Right-handed rotation around Y: positive angle rotates Z toward X
  return mat4_new(c, 0.0f, -s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, 0.0f, c, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

/* Positive angles turn +X toward +Y. */
static INLINE Mat4 mat4_euler_rotate_z(float32_t angle) {
  float32_t s = vkr_sin_f32(angle);
  float32_t c = vkr_cos_f32(angle);
  // Right-handed rotation around Z: positive angle rotates X toward Y
  return mat4_new(c, s, 0.0f, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

static INLINE Mat4 mat4_transpose(Mat4 m) {
  Mat4 result;
#if VKR_SIMD_ARM_NEON
  // Complete 4x4 transpose using ARM NEON 4-step algorithm
  // Step 1: Transpose 2x2 blocks within each pair of vectors
  float32x4x2_t t0 = vtrnq_f32(m.cols[0].neon, m.cols[1].neon);
  float32x4x2_t t1 = vtrnq_f32(m.cols[2].neon, m.cols[3].neon);

  // Step 2: Extract low and high 64-bit parts
  float32x2_t a0_lo = vget_low_f32(t0.val[0]);  // [m00, m10]
  float32x2_t a0_hi = vget_high_f32(t0.val[0]); // [m20, m30]
  float32x2_t a1_lo = vget_low_f32(t0.val[1]);  // [m01, m11]
  float32x2_t a1_hi = vget_high_f32(t0.val[1]); // [m21, m31]

  float32x2_t b0_lo = vget_low_f32(t1.val[0]);  // [m02, m12]
  float32x2_t b0_hi = vget_high_f32(t1.val[0]); // [m22, m32]
  float32x2_t b1_lo = vget_low_f32(t1.val[1]);  // [m03, m13]
  float32x2_t b1_hi = vget_high_f32(t1.val[1]); // [m23, m33]

  // Step 3: Combine to complete the transpose
  result.cols[0].neon = vcombine_f32(a0_lo, b0_lo); // [m00, m10, m02, m12]
  result.cols[1].neon = vcombine_f32(a1_lo, b1_lo); // [m01, m11, m03, m13]
  result.cols[2].neon = vcombine_f32(a0_hi, b0_hi); // [m20, m30, m22, m32]
  result.cols[3].neon = vcombine_f32(a1_hi, b1_hi); // [m21, m31, m23, m33]
#else
  // Fallback implementation - still faster than individual element access
  result.cols[0] = vec4_new(m.cols[0].x, m.cols[1].x, m.cols[2].x, m.cols[3].x);
  result.cols[1] = vec4_new(m.cols[0].y, m.cols[1].y, m.cols[2].y, m.cols[3].y);
  result.cols[2] = vec4_new(m.cols[0].z, m.cols[1].z, m.cols[2].z, m.cols[3].z);
  result.cols[3] = vec4_new(m.cols[0].w, m.cols[1].w, m.cols[2].w, m.cols[3].w);
#endif
  return result;
}

/* Returns the identity for singular, numerically cancelled or non-finite
 * inverses. A small projection scale alone does not make a matrix singular. */
static INLINE Mat4 mat4_inverse(Mat4 m) {
  // Calculate determinant using cofactor expansion along first row
  float32_t m00 = m.m00, m01 = m.m01, m02 = m.m02, m03 = m.m03;
  float32_t m10 = m.m10, m11 = m.m11, m12 = m.m12, m13 = m.m13;
  float32_t m20 = m.m20, m21 = m.m21, m22 = m.m22, m23 = m.m23;
  float32_t m30 = m.m30, m31 = m.m31, m32 = m.m32, m33 = m.m33;

  // Calculate 3x3 cofactors for determinant
  float32_t c00 = m11 * (m22 * m33 - m23 * m32) -
                  m12 * (m21 * m33 - m23 * m31) + m13 * (m21 * m32 - m22 * m31);
  float32_t c01 = m10 * (m22 * m33 - m23 * m32) -
                  m12 * (m20 * m33 - m23 * m30) + m13 * (m20 * m32 - m22 * m30);
  float32_t c02 = m10 * (m21 * m33 - m23 * m31) -
                  m11 * (m20 * m33 - m23 * m30) + m13 * (m20 * m31 - m21 * m30);
  float32_t c03 = m10 * (m21 * m32 - m22 * m31) -
                  m11 * (m20 * m32 - m22 * m30) + m12 * (m20 * m31 - m21 * m30);

  float32_t det = m00 * c00 - m01 * c01 + m02 * c02 - m03 * c03;

  // Compare cancellation against the terms that formed the determinant, not
  // an absolute world-unit threshold. Wide orthographic projections have small
  // determinants even though their axes are independent and well conditioned.
  const float32_t determinant_terms =
      fabsf(m00 * c00) + fabsf(m01 * c01) + fabsf(m02 * c02) + fabsf(m03 * c03);
  if (!isfinite(det) || !isfinite(determinant_terms) ||
      fabsf(det) <= 1e-6f * determinant_terms) {
    return mat4_identity();
  }

  float32_t inv_det = 1.0f / det;
  if (!isfinite(inv_det)) {
    return mat4_identity();
  }

  // Calculate cofactor matrix using optimized method
  Mat4 result;

  // Row 0
  result.m00 = inv_det * c00;
  result.m10 = -inv_det * c01;
  result.m20 = inv_det * c02;
  result.m30 = -inv_det * c03;

  // Row 1
  result.m01 = -inv_det *
               (m01 * (m22 * m33 - m23 * m32) - m02 * (m21 * m33 - m23 * m31) +
                m03 * (m21 * m32 - m22 * m31));
  result.m11 =
      inv_det * (m00 * (m22 * m33 - m23 * m32) - m02 * (m20 * m33 - m23 * m30) +
                 m03 * (m20 * m32 - m22 * m30));
  result.m21 = -inv_det *
               (m00 * (m21 * m33 - m23 * m31) - m01 * (m20 * m33 - m23 * m30) +
                m03 * (m20 * m31 - m21 * m30));
  result.m31 =
      inv_det * (m00 * (m21 * m32 - m22 * m31) - m01 * (m20 * m32 - m22 * m30) +
                 m02 * (m20 * m31 - m21 * m30));

  // Row 2
  result.m02 =
      inv_det * (m01 * (m12 * m33 - m13 * m32) - m02 * (m11 * m33 - m13 * m31) +
                 m03 * (m11 * m32 - m12 * m31));
  result.m12 = -inv_det *
               (m00 * (m12 * m33 - m13 * m32) - m02 * (m10 * m33 - m13 * m30) +
                m03 * (m10 * m32 - m12 * m30));
  result.m22 =
      inv_det * (m00 * (m11 * m33 - m13 * m31) - m01 * (m10 * m33 - m13 * m30) +
                 m03 * (m10 * m31 - m11 * m30));
  result.m32 = -inv_det *
               (m00 * (m11 * m32 - m12 * m31) - m01 * (m10 * m32 - m12 * m30) +
                m02 * (m10 * m31 - m11 * m30));

  // Row 3
  result.m03 = -inv_det *
               (m01 * (m12 * m23 - m13 * m22) - m02 * (m11 * m23 - m13 * m21) +
                m03 * (m11 * m22 - m12 * m21));
  result.m13 =
      inv_det * (m00 * (m12 * m23 - m13 * m22) - m02 * (m10 * m23 - m13 * m20) +
                 m03 * (m10 * m22 - m12 * m20));
  result.m23 = -inv_det *
               (m00 * (m11 * m23 - m13 * m21) - m01 * (m10 * m23 - m13 * m20) +
                m03 * (m10 * m21 - m11 * m20));
  result.m33 =
      inv_det * (m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) +
                 m02 * (m10 * m21 - m11 * m20));

  for (uint32_t i = 0u; i < ArrayCount(result.elements); ++i) {
    if (!isfinite(result.elements[i])) {
      return mat4_identity();
    }
  }

  return result;
}

/* The transpose; only for pure rotations. */
static INLINE Mat4 mat4_inverse_orthogonal(Mat4 m) { return mat4_transpose(m); }

/* For a [0, 0, 0, 1] bottom row: inverts the upper 3x3 (any scale or shear) and
 * the translation. Returns the identity when that 3x3 is singular relative to
 * its own terms. */
static INLINE Mat4 mat4_inverse_affine(Mat4 m) {
  Vec4 cross0 =
      vec4_new(m.m11 * m.m22 - m.m12 * m.m21, m.m12 * m.m20 - m.m10 * m.m22,
               m.m10 * m.m21 - m.m11 * m.m20, 0.0f);

  float32_t det = vec3_dot((Vec3){m.m00, m.m01, m.m02},
                           (Vec3){cross0.x, cross0.y, cross0.z});

  // Relative to the terms that formed the determinant, as in mat4_inverse. An
  // absolute threshold rejected well-conditioned small scales: 0.01 cubed is
  // below 1e-6.
  const float32_t determinant_terms = fabsf(m.m00 * cross0.x) +
                                      fabsf(m.m01 * cross0.y) +
                                      fabsf(m.m02 * cross0.z);
  if (!isfinite(det) || !isfinite(determinant_terms) ||
      fabsf(det) <= 1e-6f * determinant_terms) {
    return mat4_identity();
  }

  float32_t inv_det = 1.0f / det;
  if (!isfinite(inv_det)) {
    return mat4_identity();
  }

  Mat4 result;
  result.m00 = cross0.x * inv_det;
  result.m10 = cross0.y * inv_det;
  result.m20 = cross0.z * inv_det;
  result.m30 = 0.0f;

  result.m01 = (m.m02 * m.m21 - m.m01 * m.m22) * inv_det;
  result.m11 = (m.m00 * m.m22 - m.m02 * m.m20) * inv_det;
  result.m21 = (m.m01 * m.m20 - m.m00 * m.m21) * inv_det;
  result.m31 = 0.0f;

  result.m02 = (m.m01 * m.m12 - m.m02 * m.m11) * inv_det;
  result.m12 = (m.m02 * m.m10 - m.m00 * m.m12) * inv_det;
  result.m22 = (m.m00 * m.m11 - m.m01 * m.m10) * inv_det;
  result.m32 = 0.0f;

  result.m03 = -(result.m00 * m.m03 + result.m01 * m.m13 + result.m02 * m.m23);
  result.m13 = -(result.m10 * m.m03 + result.m11 * m.m13 + result.m12 * m.m23);
  result.m23 = -(result.m20 * m.m03 + result.m21 * m.m13 + result.m22 * m.m23);
  result.m33 = 1.0f;

  return result;
}

// =============================================================================
// Matrix Accessors
// =============================================================================

/* The index is not checked. */
static INLINE Vec4 mat4_col(Mat4 m, int32_t col) { return m.cols[col]; }

/* The index is not checked. */
static INLINE Vec4 mat4_row(Mat4 m, int32_t row) {
  return vec4_new(m.elements[row], m.elements[row + 4], m.elements[row + 8],
                  m.elements[row + 12]);
}

/* Row and column are not checked. */
static INLINE float32_t mat4_at(Mat4 m, int32_t row, int32_t col) {
  return m.elements[col * 4 + row];
}

/* Row and column are not checked. */
static INLINE void mat4_set(Mat4 *m, int32_t row, int32_t col,
                            float32_t value) {
  m->elements[col * 4 + row] = value;
}

static INLINE float32_t mat4_determinant(Mat4 m) {
  // Calculate determinant using cofactor expansion along first row
  // det(M) = m00*C00 - m01*C01 + m02*C02 - m03*C03
  // where Cij is the cofactor (determinant of 3x3 minor)

  float32_t m00 = m.m00, m01 = m.m01, m02 = m.m02, m03 = m.m03;
  float32_t m10 = m.m10, m11 = m.m11, m12 = m.m12, m13 = m.m13;
  float32_t m20 = m.m20, m21 = m.m21, m22 = m.m22, m23 = m.m23;
  float32_t m30 = m.m30, m31 = m.m31, m32 = m.m32, m33 = m.m33;

  // Calculate 3x3 cofactors
  float32_t c00 = m11 * (m22 * m33 - m23 * m32) -
                  m12 * (m21 * m33 - m23 * m31) + m13 * (m21 * m32 - m22 * m31);
  float32_t c01 = m10 * (m22 * m33 - m23 * m32) -
                  m12 * (m20 * m33 - m23 * m30) + m13 * (m20 * m32 - m22 * m30);
  float32_t c02 = m10 * (m21 * m33 - m23 * m31) -
                  m11 * (m20 * m33 - m23 * m30) + m13 * (m20 * m31 - m21 * m30);
  float32_t c03 = m10 * (m21 * m32 - m22 * m31) -
                  m11 * (m20 * m32 - m22 * m30) + m12 * (m20 * m31 - m21 * m30);

  return m00 * c00 - m01 * c01 + m02 * c02 - m03 * c03;
}

static INLINE float32_t mat4_trace(Mat4 m) {
  return m.m00 + m.m11 + m.m22 + m.m33;
}

/* True when every element is within epsilon of the identity. */
static INLINE bool8_t mat4_is_identity(Mat4 m, float32_t epsilon) {
  // Check diagonal elements are close to 1
  if (vkr_abs_f32(m.m00 - 1.0f) > epsilon ||
      vkr_abs_f32(m.m11 - 1.0f) > epsilon ||
      vkr_abs_f32(m.m22 - 1.0f) > epsilon ||
      vkr_abs_f32(m.m33 - 1.0f) > epsilon) {
    return false;
  }

  // Check off-diagonal elements are close to 0
  if (vkr_abs_f32(m.m01) > epsilon || vkr_abs_f32(m.m02) > epsilon ||
      vkr_abs_f32(m.m03) > epsilon || vkr_abs_f32(m.m10) > epsilon ||
      vkr_abs_f32(m.m12) > epsilon || vkr_abs_f32(m.m13) > epsilon ||
      vkr_abs_f32(m.m20) > epsilon || vkr_abs_f32(m.m21) > epsilon ||
      vkr_abs_f32(m.m23) > epsilon || vkr_abs_f32(m.m30) > epsilon ||
      vkr_abs_f32(m.m31) > epsilon || vkr_abs_f32(m.m32) > epsilon) {
    return false;
  }

  return true;
}

// =============================================================================
// Matrix Operations
// =============================================================================

static INLINE Mat4 mat4_add(Mat4 a, Mat4 b) {
  return (Mat4){
      .columns =
          {
              .col0 = vec4_add(a.columns.col0, b.columns.col0),
              .col1 = vec4_add(a.columns.col1, b.columns.col1),
              .col2 = vec4_add(a.columns.col2, b.columns.col2),
              .col3 = vec4_add(a.columns.col3, b.columns.col3),
          },
  };
}

static INLINE Mat4 mat4_sub(Mat4 a, Mat4 b) {
  return (Mat4){
      .columns =
          {
              .col0 = vec4_sub(a.columns.col0, b.columns.col0),
              .col1 = vec4_sub(a.columns.col1, b.columns.col1),
              .col2 = vec4_sub(a.columns.col2, b.columns.col2),
              .col3 = vec4_sub(a.columns.col3, b.columns.col3),
          },
  };
}

/* a * b applies b first, then a. */
static INLINE Mat4 mat4_mul(Mat4 a, Mat4 b) {
  Mat4 result;
  for (int i = 0; i < 4; i++) {
    Vec4 col = b.cols[i];
    Vec4 x = vkr_simd_set1_f32x4(col.x);
    Vec4 y = vkr_simd_set1_f32x4(col.y);
    Vec4 z = vkr_simd_set1_f32x4(col.z);
    Vec4 w = vkr_simd_set1_f32x4(col.w);

    // Compute: a.cols[0]*x + a.cols[1]*y + a.cols[2]*z + a.cols[3]*w
    // Note: vkr_simd_fma_f32x4(dst, b, c) computes dst + (b * c)
    Vec4 temp = vkr_simd_mul_f32x4(a.cols[0], x);
    temp = vkr_simd_fma_f32x4(temp, a.cols[1], y); // temp + (a.cols[1] * y)
    temp = vkr_simd_fma_f32x4(temp, a.cols[2], z); // temp + (a.cols[2] * z)
    result.cols[i] =
        vkr_simd_fma_f32x4(temp, a.cols[3], w); // temp + (a.cols[3] * w)
  }
  return result;
}

// =============================================================================
// Matrix To Vector Operations
// =============================================================================

/* The first column (the image of +X), not normalized. */
static INLINE Vec3 mat4_to_vec3(Mat4 m) {
  return vec3_new(m.m00, m.m10, m.m20);
}

/* The first column, including W. */
static INLINE Vec4 mat4_to_vec4(Mat4 m) {
  return vec4_new(m.m00, m.m10, m.m20, m.m30);
}

/* X and Y of the first column. */
static INLINE Vec2 mat4_to_vec2(Mat4 m) { return vec2_new(m.m00, m.m10); }

static INLINE Vec4 mat4_mul_vec4(Mat4 m, Vec4 v) {
  return vec4_add(
      vec4_add(vec4_scale(m.cols[0], v.x), vec4_scale(m.cols[1], v.y)),
      vec4_add(vec4_scale(m.cols[2], v.z), vec4_scale(m.cols[3], v.w)));
}

/* Treats v as a point (w = 1) and does not divide by w; use mat4_mul_vec4 for
 * directions or projections. */
static INLINE Vec3 mat4_mul_vec3(Mat4 m, Vec3 v) {
  return vec3_add(
      vec3_add(vec3_add(vec3_scale(m.cols[0], v.x), vec3_scale(m.cols[1], v.y)),
               vec3_scale(m.cols[2], v.z)),
      vec3_scale(m.cols[3], 1.0f));
}

/* The unit -Z column, the facing direction. */
static INLINE Vec3 mat4_forward(Mat4 m) {
  // In right-handed system, forward is -Z (negative third column)
  return vec3_normalize(vec3_new(-m.m02, -m.m12, -m.m22));
}

/* The unit +Z column. */
static INLINE Vec3 mat4_backward(Mat4 m) {
  // In right-handed system, backward is +Z (third column)
  return vec3_normalize(vec3_new(m.m02, m.m12, m.m22));
}

/* The unit +Y column. */
static INLINE Vec3 mat4_up(Mat4 m) {
  return vec3_normalize(vec3_new(m.m01, m.m11, m.m21));
}

/* The unit -Y column. */
static INLINE Vec3 mat4_down(Mat4 m) {
  return vec3_normalize(vec3_new(-m.m01, -m.m11, -m.m21));
}

/* The unit +X column. */
static INLINE Vec3 mat4_right(Mat4 m) {
  return vec3_normalize(vec3_new(m.m00, m.m10, m.m20));
}

/* The unit -X column. */
static INLINE Vec3 mat4_left(Mat4 m) {
  return vec3_normalize(vec3_new(-m.m00, -m.m10, -m.m20));
}

/* The translation column. */
static INLINE Vec3 mat4_position(Mat4 m) {
  return vec3_new(m.m03, m.m13, m.m23);
}

// =============================================================================
// Matrix To VkrQuaternion Operations
// =============================================================================

/* Expects an orthonormal rotation; translation is ignored. Uses Shepperd's
 * method. */
static INLINE VkrQuat mat4_to_quat(Mat4 m) {
  float32_t trace = m.m00 + m.m11 + m.m22;

  if (trace > 0.0f) {
    float32_t s = 0.5f / vkr_sqrt_f32(trace + 1.0f);
    return vec4_new((m.m21 - m.m12) * s, (m.m02 - m.m20) * s,
                    (m.m10 - m.m01) * s, 0.25f / s);
  } else if (m.m00 > m.m11 && m.m00 > m.m22) {
    float32_t s = 2.0f * vkr_sqrt_f32(1.0f + m.m00 - m.m11 - m.m22);
    return vec4_new(0.25f * s, (m.m01 + m.m10) / s, (m.m02 + m.m20) / s,
                    (m.m21 - m.m12) / s);
  } else if (m.m11 > m.m22) {
    float32_t s = 2.0f * vkr_sqrt_f32(1.0f + m.m11 - m.m00 - m.m22);
    return vec4_new((m.m01 + m.m10) / s, 0.25f * s, (m.m12 + m.m21) / s,
                    (m.m02 - m.m20) / s);
  } else {
    float32_t s = 2.0f * vkr_sqrt_f32(1.0f + m.m22 - m.m00 - m.m11);
    return vec4_new((m.m02 + m.m20) / s, (m.m12 + m.m21) / s, 0.25f * s,
                    (m.m10 - m.m01) / s);
  }
}

/* Same as mat4_to_quat. */
static INLINE VkrQuat vkr_quat_from_mat4(Mat4 m) { return mat4_to_quat(m); }

/* Expects a unit quaternion; the result has no translation. */
static INLINE Mat4 vkr_quat_to_mat4(VkrQuat q) {
  // Extract quaternion components
  float32_t x = q.x, y = q.y, z = q.z, w = q.w;

  // Precompute common terms for efficiency
  float32_t xx = x * x;
  float32_t yy = y * y;
  float32_t zz = z * z;
  float32_t xy = x * y;
  float32_t xz = x * z;
  float32_t yz = y * z;
  float32_t wx = w * x;
  float32_t wy = w * y;
  float32_t wz = w * z;

  // Build rotation matrix from quaternion
  // Using the standard quaternion to rotation matrix conversion formula
  // Note: Column-major layout
  return mat4_new(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy),
                  0.0f, 2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz),
                  2.0f * (yz + wx), 0.0f, 2.0f * (xz + wy), 2.0f * (yz - wx),
                  1.0f - 2.0f * (xx + yy), 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

/* Rotation from a unit quaternion, then translation to position; no scale. */
static INLINE Mat4 mat4_from_vkr_quat_pos(VkrQuat q, Vec3 position) {
  Mat4 result = vkr_quat_to_mat4(q);
  result.m03 = position.x;
  result.m13 = position.y;
  result.m23 = position.z;
  return result;
}

// =============================================================================
// In-Place Matrix Operations (for performance-critical code)
// =============================================================================

/* dest may alias a or b. */
static INLINE void mat4_mul_mut(Mat4 *dest, Mat4 a, Mat4 b) {
  for (int i = 0; i < 4; i++) {
    Vec4 col = b.cols[i];
    Vec4 x = vkr_simd_set1_f32x4(col.x);
    Vec4 y = vkr_simd_set1_f32x4(col.y);
    Vec4 z = vkr_simd_set1_f32x4(col.z);
    Vec4 w = vkr_simd_set1_f32x4(col.w);

    // Compute: a.cols[0]*x + a.cols[1]*y + a.cols[2]*z + a.cols[3]*w
    // Note: vkr_simd_fma_f32x4(dst, b, c) computes dst + (b * c)
    Vec4 temp = vkr_simd_mul_f32x4(a.cols[0], x);
    temp = vkr_simd_fma_f32x4(temp, a.cols[1], y); // temp + (a.cols[1] * y)
    temp = vkr_simd_fma_f32x4(temp, a.cols[2], z); // temp + (a.cols[2] * z)
    dest->cols[i] =
        vkr_simd_fma_f32x4(temp, a.cols[3], w); // temp + (a.cols[3] * w)
  }
}

/* dest may alias a or b. */
static INLINE void mat4_add_mut(Mat4 *dest, Mat4 a, Mat4 b) {
  dest->cols[0] = vec4_add(a.cols[0], b.cols[0]);
  dest->cols[1] = vec4_add(a.cols[1], b.cols[1]);
  dest->cols[2] = vec4_add(a.cols[2], b.cols[2]);
  dest->cols[3] = vec4_add(a.cols[3], b.cols[3]);
}

/* Only for rotation plus translation, no scale: returns R^T and -R^T t. */
static INLINE Mat4 mat4_inverse_rigid(Mat4 m) {
  // For rigid body transforms: R^-1 = R^T, t^-1 = -R^T * t

  // Transpose the rotation part (upper-left 3x3)
  Mat4 result;
  result.m00 = m.m00;
  result.m01 = m.m10;
  result.m02 = m.m20;
  result.m10 = m.m01;
  result.m11 = m.m11;
  result.m12 = m.m21;
  result.m20 = m.m02;
  result.m21 = m.m12;
  result.m22 = m.m22;

  // Clear the W components of the first three columns
  result.m30 = 0.0f;
  result.m31 = 0.0f;
  result.m32 = 0.0f;

  // Compute -R^T * t for the translation column
  Vec3 translation = vec3_new(m.m03, m.m13, m.m23);
  // Get the rows of the transposed rotation matrix (which are the columns of
  // the original rotation matrix)
  Vec3 row0 = vec3_new(result.m00, result.m01, result.m02); // First row of R^T
  Vec3 row1 = vec3_new(result.m10, result.m11, result.m12); // Second row of R^T
  Vec3 row2 = vec3_new(result.m20, result.m21, result.m22); // Third row of R^T

  Vec3 rotated_translation =
      vec3_new(-vec3_dot(row0, translation), -vec3_dot(row1, translation),
               -vec3_dot(row2, translation));

  result.m03 = rotated_translation.x;
  result.m13 = rotated_translation.y;
  result.m23 = rotated_translation.z;
  result.m33 = 1.0f;

  return result;
}
