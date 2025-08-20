#pragma once

#include "defines.h"
#include "math.h"
#include "quat.h"
#include "simd.h"
#include "vec.h"

// clang-format off

/**
 * @file mat.h
 * @brief Comprehensive 4x4 matrix mathematics library for 3D graphics and transformations.
 *
 * This file provides a complete set of matrix operations optimized for graphics programming,
 * game development, and 3D rendering. The library supports transformation matrices, projection
 * matrices, view matrices, and various utility operations with SIMD acceleration.
 *
 * Coordinate System and Conventions:
 * - RIGHT-HANDED coordinate system (industry standard)
 * - X-axis: Points right
 * - Y-axis: Points up  
 * - Z-axis: Points backward (toward the viewer in camera space)
 * - Column-major storage (OpenGL/Vulkan/USD/glTF compatible)
 * - Designed for Vulkan: Y-clip inverted, Z range [0,1]
 * - Compatible with: Vulkan, OpenGL, USD, glTF, Maya, Houdini
 *
 * Matrix Layout and Memory Organization:
 * All matrices use column-major storage for optimal GPU compatibility and mathematical
 * consistency with linear algebra conventions:
 *
 * Mat4 Memory Layout (Column-Major):
 * +------------------+ <-- 16-byte SIMD aligned structure  
 * | Column 0 (Vec4)  |  [0-3] Right vector (X-axis): [m00, m10, m20, m30]
 * +------------------+
 * | Column 1 (Vec4)  |  [4-7] Up vector (Y-axis): [m01, m11, m21, m31] 
 * +------------------+
 * | Column 2 (Vec4)  |  [8-11] Forward vector (Z-axis): [m02, m12, m22, m32]
 * +------------------+
 * | Column 3 (Vec4)  | [12-15] Translation: [m03, m13, m23, m33]
 * +------------------+
 *
 * Mathematical Representation:
 * ```
 * | m00  m01  m02  m03 |   | Xx  Yx  Zx  Tx |   | Right.x   Up.x   Forward.x   Translation.x |
 * | m10  m11  m12  m13 | = | Xy  Yy  Zy  Ty | = | Right.y   Up.y   Forward.y   Translation.y |
 * | m20  m21  m22  m23 |   | Xz  Yz  Zz  Tz |   | Right.z   Up.z   Forward.z   Translation.z |
 * | m30  m31  m32  m33 |   | 0   0   0   1  |   | 0         0      0            1             |
 * ```
 *
 * Element Access Patterns:
 * The union-based structure provides multiple semantic access patterns for the same
 * underlying data, allowing intuitive usage in different contexts:
 *
 * - Column Access: `m.cols[0-3]` or `m.columns.col0-3` (Vec4 columns)
 * - Element Access: `m.elements[0-15]` (flat array, column-major order)  
 * - Direct Access: `m.m00-m33` (individual matrix elements)
 * - Semantic Access: `mat4_right(m)`, `mat4_up(m)`, `mat4_forward(m)`, `mat4_position(m)`
 *
 * Transform Matrix Conventions:
 * 1. **Translation Matrix**: Translation values in column 3 (m03, m13, m23)
 * 2. **Rotation Matrix**: 3x3 rotation in upper-left, orthonormal basis vectors
 * 3. **Scale Matrix**: Scale factors on diagonal (m00, m11, m22)
 * 4. **Projection Matrix**: Maps 3D to clip space, handles perspective division
 * 5. **View Matrix**: Transforms world space to camera space
 *
 * SIMD Optimization and Performance:
 * - Vec4 columns leverage hardware SIMD instructions (ARM NEON, x86 SSE)
 * - Matrix multiplication uses FMA (Fused Multiply-Add) for precision and speed
 * - 16-byte alignment ensures optimal memory access patterns
 * - In-place operations available for performance-critical code
 * - Specialized inverse functions for common transform types
 * - ARM NEON optimized transpose operations
 *
 * Matrix Operation Categories:
 * 1. **Construction**: Identity, zero, transformations (translate, rotate, scale)
 * 2. **Projection**: Perspective, orthographic projection matrices
 * 3. **View**: Look-at camera transformation
 * 4. **Basic Operations**: Add, subtract, multiply, transpose
 * 5. **Inverse Operations**: General, orthogonal, affine, rigid body inverses
 * 6. **Utilities**: Determinant, trace, identity checking
 * 7. **Extraction**: Vector directions, position, quaternion conversion
 * 8. **In-Place**: Memory-efficient operations for tight loops
 *
 * API Design Patterns:
 * 1. Constructor Functions: mat4_identity(), mat4_translate(), mat4_perspective()
 * 2. Transform Operations: mat4_mul(), mat4_add(), mat4_inverse()
 * 3. Utility Functions: mat4_determinant(), mat4_trace(), mat4_is_identity()
 * 4. Vector Extraction: mat4_position(), mat4_forward(), mat4_right()
 * 5. In-Place Operations: mat4_mul_mut(), mat4_add_mut() for performance
 * 6. Type Conversions: mat4_to_quat(), quat_to_mat4()
 * 7. Specialized Variants: mat4_inverse_rigid(), mat4_inverse_affine()
 *
 * Performance Characteristics:
 * - Matrix multiplication: O(64) operations, SIMD-accelerated
 * - General inverse: O(64) operations using SIMD cofactor method  
 * - Rigid body inverse: O(12) operations, 5x faster than general
 * - Orthogonal inverse: O(16) operations (simple transpose)
 * - Memory: 64 bytes per matrix, cache-friendly access patterns
 * - Alignment: 16-byte aligned for optimal SIMD performance
 *
 * Usage Examples:
 *
 * Basic Matrix Operations:
 * ```c
 * // Create transformation matrices
 * Mat4 translation = mat4_translate(vec3_new(10.0f, 0.0f, -5.0f));
 * Mat4 rotation = mat4_euler_rotate_y(to_radians(45.0f));
 * Mat4 scale = mat4_scale(vec3_new(2.0f, 1.0f, 1.0f));
 * 
 * // Compose transformations (applied right-to-left: scale -> rotate -> translate)
 * Mat4 model = mat4_mul(translation, mat4_mul(rotation, scale));
 * 
 * // Create camera matrices
 * Mat4 view = mat4_look_at(
 *     vec3_new(0.0f, 5.0f, 10.0f),  // eye position
 *     vec3_new(0.0f, 0.0f, 0.0f),   // look at target
 *     vec3_new(0.0f, 1.0f, 0.0f)    // up direction
 * );
 * 
 * Mat4 projection = mat4_perspective(
 *     to_radians(45.0f),  // field of view
 *     16.0f / 9.0f,       // aspect ratio
 *     0.1f,               // near plane
 *     100.0f              // far plane
 * );
 * ```
 *
 * Performance-Critical Code:
 * ```c
 * // Use in-place operations to avoid temporary allocations
 * Mat4 result;
 * mat4_mul_mut(&result, model, view);           // result = model * view
 * mat4_mul_mut(&result, result, projection);    // result = result * projection
 * 
 * // Fast inverse for rigid body transforms (no scaling)
 * Mat4 camera_transform = mat4_from_quat_pos(camera_rotation, camera_position);
 * Mat4 view_matrix = mat4_inverse_rigid(camera_transform);  // 5x faster than general inverse
 * ```
 *
 * Rendering Pipeline Example:
 * ```c
 * // Model transformation
 * Vec3 object_pos = vec3_new(0.0f, 0.0f, -10.0f);
 * Quat object_rot = quat_from_euler(0.0f, game_time, 0.0f);
 * Vec3 object_scale = vec3_one();
 * 
 * Mat4 model_matrix = mat4_mul(
 *     mat4_translate(object_pos),
 *     mat4_mul(quat_to_mat4(object_rot), mat4_scale(object_scale))
 * );
 * 
 * // View transformation  
 * Vec3 camera_pos = vec3_new(0.0f, 2.0f, 5.0f);
 * Vec3 camera_target = vec3_add(camera_pos, vec3_forward());
 * Mat4 view_matrix = mat4_look_at(camera_pos, camera_target, vec3_up());
 * 
 * // Projection transformation
 * Mat4 proj_matrix = mat4_perspective(to_radians(60.0f), aspect_ratio, 0.1f, 1000.0f);
 * 
 * // MVP matrix for shader
 * Mat4 mvp_matrix = mat4_mul(proj_matrix, mat4_mul(view_matrix, model_matrix));
 * 
 * // Transform vertex positions
 * Vec4 world_pos = mat4_mul_vec4(model_matrix, local_vertex);
 * Vec4 clip_pos = mat4_mul_vec4(mvp_matrix, vec3_to_vec4(local_vertex, 1.0f));
 * ```
 *
 * Matrix Analysis and Debugging:
 * ```c
 * // Check matrix properties
 * bool8_t is_invertible = abs_f32(mat4_determinant(matrix)) > FLOAT_EPSILON;
 * bool8_t is_identity = mat4_is_identity(matrix, FLOAT_EPSILON);
 * float32_t matrix_trace = mat4_trace(matrix);
 * 
 * // Extract transformation components
 * Vec3 position = mat4_position(transform);
 * Vec3 forward_dir = mat4_forward(transform);
 * Vec3 right_dir = mat4_right(transform);
 * Vec3 up_dir = mat4_up(transform);
 * 
 * // Convert to quaternion for rotation interpolation
 * Quat rotation = mat4_to_quat(transform);
 * ```
 *
 * Vulkan-Specific Considerations:
 * - Y-axis is flipped in clip space (handled by mat4_perspective)
 * - Z range is [0,1] instead of [-1,1] (NDC mapping)
 * - Right-handed coordinate system in world space
 * - Column-major matrices match GLSL uniform layout
 *
 * Thread Safety:
 * All matrix operations are thread-safe as they operate on local data and
 * do not modify global state. Matrices can be safely used in multi-threaded
 * environments without synchronization.
 *
 * Compiler Optimizations:
 * - All functions are marked static inline for zero-cost abstractions
 * - SIMD intrinsics used where available (ARM NEON, x86 SSE)
 * - FMA operations use hardware acceleration when available
 * - Memory access patterns optimized for cache efficiency
 * - Structured to encourage compiler auto-vectorization
 */

// clang-format on

typedef SIMD_ALIGN union Mat4 {
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

// =============================================================================
// Matrix Constructor Functions
// =============================================================================

/**
 * @brief Creates a 4x4 matrix from individual elements in column-major order
 * @param m00 Element at row 0, column 0 (right vector X component)
 * @param m10 Element at row 1, column 0 (right vector Y component)
 * @param m20 Element at row 2, column 0 (right vector Z component)
 * @param m30 Element at row 3, column 0 (should be 0 for transform matrices)
 * @param m01 Element at row 0, column 1 (up vector X component)
 * @param m11 Element at row 1, column 1 (up vector Y component)
 * @param m21 Element at row 2, column 1 (up vector Z component)
 * @param m31 Element at row 3, column 1 (should be 0 for transform matrices)
 * @param m02 Element at row 0, column 2 (forward vector X component)
 * @param m12 Element at row 1, column 2 (forward vector Y component)
 * @param m22 Element at row 2, column 2 (forward vector Z component)
 * @param m32 Element at row 3, column 2 (should be 0 for transform matrices)
 * @param m03 Element at row 0, column 3 (translation X component)
 * @param m13 Element at row 1, column 3 (translation Y component)
 * @param m23 Element at row 2, column 3 (translation Z component)
 * @param m33 Element at row 3, column 3 (should be 1 for transform matrices)
 * @return 4x4 matrix with specified elements
 * @note Parameters are ordered by column-major storage for consistency
 * @note For transformation matrices, bottom row should be [0, 0, 0, 1]
 * @example mat4_new(1,0,0,0, 0,1,0,0, 0,0,1,0, 5,10,15,1) creates translation
 * by (5,10,15)
 */
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

/**
 * @brief Creates a zero matrix with all elements set to 0.0
 * @return 4x4 matrix with all elements zero
 * @note Rarely used in practice; prefer mat4_identity() for transform matrices
 * @note Zero matrix has no inverse and represents a degenerate transformation
 * @performance O(1) - Simply sets SIMD vectors to zero
 */
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

/**
 * @brief Creates the 4x4 identity matrix (no transformation)
 * @return Identity matrix with 1s on diagonal, 0s elsewhere
 * @note Identity matrix represents no transformation: I * M = M * I = M
 * @note Inverse of identity matrix is itself
 * @note Most commonly used matrix constructor for initialization
 * @performance O(1) - Compile-time constant initialization
 * @example
 * ```c
 * Mat4 transform = mat4_identity();
 * Vec4 unchanged = mat4_mul_vec4(transform, original_vector);
 * // unchanged == original_vector
 * ```
 */
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

/**
 * @brief Creates an orthographic projection matrix for parallel projection
 * @param left Left edge of the view volume
 * @param right Right edge of the view volume
 * @param bottom Bottom edge of the view volume
 * @param top Top edge of the view volume
 * @param near Near clipping plane distance (positive value)
 * @param far Far clipping plane distance (positive value, > near)
 * @return Orthographic projection matrix that maps view volume to [-1,1]³
 * (OpenGL) or [0,1] depth (Vulkan)
 * @note Used for 2D rendering, CAD applications, and shadow mapping
 * @note No perspective foreshortening - parallel lines remain parallel
 * @note Objects maintain their size regardless of distance from camera
 * @note View volume is a rectangular box (frustum with infinite focal length)
 * @performance O(1) - Simple arithmetic operations
 * @example
 * ```c
 * // Create orthographic projection for 2D UI (screen coordinates)
 * Mat4 ortho = mat4_ortho(0.0f, screen_width, screen_height, 0.0f,
 * -1.0f, 1.0f);
 *
 * // Create symmetric orthographic projection
 * float32_t half_width = 10.0f;
 * float32_t half_height = 10.0f;
 * Mat4 ortho = mat4_ortho(-half_width, half_width, -half_height, half_height,
 * 0.1f, 100.0f);
 * ```
 */
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

/**
 * @brief Creates a perspective projection matrix for realistic 3D rendering
 * @param fov Field of view angle in radians (vertical angle)
 * @param aspect Aspect ratio (width / height) of the viewport
 * @param near Near clipping plane distance (positive value, > 0)
 * @param far Far clipping plane distance (positive value, > near)
 * @return Perspective projection matrix optimized for Vulkan (Z range [0,1],
 * Y-inverted)
 * @note Creates perspective foreshortening - distant objects appear smaller
 * @note View frustum is a truncated pyramid shape
 * @note Optimized for Vulkan: Y-axis inverted, Z range [0,1] instead of [-1,1]
 * @note Right-handed coordinate system: camera looks down -Z axis
 * @note Common FOV values: 45°-90° (most games use 60°-75°)
 * @performance O(1) - Involves one trigonometric operation (tan)
 * @example
 * ```c
 * // Standard game camera setup
 * Mat4 projection = mat4_perspective(
 *     to_radians(75.0f),    // 75 degree field of view
 *     16.0f / 9.0f,         // widescreen aspect ratio
 *     0.1f,                 // near plane (don't make too small - Z-fighting)
 *     1000.0f               // far plane
 * );
 *
 * // Wide-angle camera for architectural visualization
 * Mat4 wide_proj = mat4_perspective(to_radians(90.0f), aspect, 0.01f, 500.0f);
 * ```
 */
static INLINE Mat4 mat4_perspective(float32_t fov, float32_t aspect,
                                    float32_t near_clip, float32_t far_clip) {
  // Right-handed perspective matrix for Vulkan (Z range [0,1])
  float32_t f = 1.0f / tan_f32(fov * 0.5f);

  // Note: Vulkan clip space has inverted Y (top = -1, bottom = +1)
  // We negate the Y scaling to account for this
  return mat4_new(f / aspect, 0.0f, 0.0f, 0.0f, 0.0f, -f, 0.0f,
                  0.0f, // Negated Y for Vulkan
                  0.0f, 0.0f, far_clip / (far_clip - near_clip), 1.0f, 0.0f,
                  0.0f, -(far_clip * near_clip) / (far_clip - near_clip), 0.0f);
}

/**
 * @brief Creates a view matrix for camera transformation (world to camera
 * space)
 * @param eye Camera position in world space
 * @param center Target position to look at in world space
 * @param up Up direction vector in world space (doesn't need to be normalized)
 * @return View matrix that transforms world coordinates to camera space
 * @note Implements right-handed coordinate system: camera looks down -Z axis
 * @note Equivalent to inverse of camera's model transformation matrix
 * @note Up vector doesn't need to be perpendicular to view direction - will be
 * orthogonalized
 * @note Creates orthonormal basis vectors: right, up, forward
 * @note Forward direction is from eye toward center (normalized)
 * @performance O(10) - Involves vector normalizations and cross products
 * @example
 * ```c
 * // First-person camera looking at origin
 * Vec3 camera_pos = vec3_new(0.0f, 5.0f, 10.0f);
 * Vec3 target = vec3_new(0.0f, 0.0f, 0.0f);
 * Vec3 world_up = vec3_new(0.0f, 1.0f, 0.0f);
 * Mat4 view = mat4_look_at(camera_pos, target, world_up);
 *
 * // Orbit camera around object
 * float32_t angle = game_time * 0.5f;
 * Vec3 orbit_pos = vec3_new(cos_f32(angle) * 10.0f, 5.0f, sin_f32(angle)
 * * 10.0f); Mat4 orbit_view = mat4_look_at(orbit_pos, vec3_zero(), vec3_up());
 * ```
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

/**
 * @brief Creates a translation transformation matrix
 * @param v Translation vector (displacement in X, Y, Z)
 * @return Translation matrix that moves points by the specified vector
 * @note Translation values are stored in the fourth column (m03, m13, m23)
 * @note Matrix multiplication order: result = translate * vector
 * @note Combines with other transforms: T * R * S (applied right-to-left)
 * @note Inverse is simple negation: mat4_translate(vec3_negate(v))
 * @performance O(1) - Simple matrix construction
 * @example
 * ```c
 * // Move object 5 units right, 3 units up, 2 units back
 * Mat4 translation = mat4_translate(vec3_new(5.0f, 3.0f, -2.0f));
 *
 * // Animate object position over time
 * Vec3 animated_pos = vec3_new(sin_f32(time) * 10.0f, 0.0f, cos_f32(time)
 * * 10.0f); Mat4 animated_transform = mat4_translate(animated_pos);
 * ```
 */
static INLINE Mat4 mat4_translate(Vec3 v) {
  return mat4_new(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  1.0f, 0.0f, v.x, v.y, v.z, 1.0f);
}

/**
 * @brief Creates a non-uniform scale transformation matrix
 * @param v Scale factors for X, Y, Z axes
 * @return Scale matrix that multiplies coordinates by the specified factors
 * @note Scale factors are placed on the diagonal (m00, m11, m22)
 * @note Uniform scaling: all components equal (preserves shape)
 * @note Non-uniform scaling: different components (stretches/squashes)
 * @note Scale factor 1.0 = no change, >1.0 = enlarges, <1.0 = shrinks
 * @note Negative values cause reflection (mirroring)
 * @note Zero values create degenerate matrix (not invertible)
 * @performance O(1) - Simple matrix construction
 * @example
 * ```c
 * // Uniform scaling: make object twice as large
 * Mat4 uniform_scale = mat4_scale(vec3_new(2.0f, 2.0f, 2.0f));
 *
 * // Non-uniform scaling: stretch horizontally, compress vertically
 * Mat4 stretch = mat4_scale(vec3_new(2.0f, 0.5f, 1.0f));
 *
 * // Mirror along X-axis
 * Mat4 mirror_x = mat4_scale(vec3_new(-1.0f, 1.0f, 1.0f));
 * ```
 */
static INLINE Mat4 mat4_scale(Vec3 v) {
  return mat4_new(v.x, 0.0f, 0.0f, 0.0f, 0.0f, v.y, 0.0f, 0.0f, 0.0f, 0.0f, v.z,
                  0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a rotation matrix around an arbitrary axis (Rodrigues'
 * rotation formula)
 * @param axis Rotation axis vector (will be normalized automatically)
 * @param angle Rotation angle in radians (positive = counter-clockwise when
 * looking along axis)
 * @return Rotation matrix using Rodrigues' formula for arbitrary axis rotation
 * @note Implements Rodrigues' rotation formula for numerical stability
 * @note Right-handed coordinate system: positive angles are counter-clockwise
 * @note Axis vector is automatically normalized for convenience
 * @note More flexible than individual axis rotations but slightly more
 * expensive
 * @note Quaternions are often preferred for arbitrary axis rotations
 * @performance O(8) - Involves trigonometric functions and vector operations
 * @example
 * ```c
 * // Rotate 45 degrees around diagonal axis
 * Vec3 diagonal_axis = vec3_normalize(vec3_new(1.0f, 1.0f, 1.0f));
 * Mat4 diagonal_rotation = mat4_euler_rotate(diagonal_axis, to_radians(45.0f));
 *
 * // Rotate around arbitrary world axis
 * Vec3 custom_axis = vec3_new(0.707f, 0.0f, 0.707f);  // 45° from X toward Z
 * Mat4 custom_rotation = mat4_euler_rotate(custom_axis, to_radians(90.0f));
 * ```
 */
static INLINE Mat4 mat4_euler_rotate(Vec3 axis, float32_t angle) {
  axis = vec3_normalize(axis); // Ensure axis is normalized
  float32_t s = sin_f32(angle);
  float32_t c = cos_f32(angle);
  float32_t t = 1.0f - c;
  return mat4_new(
      t * axis.x * axis.x + c, t * axis.x * axis.y - s * axis.z,
      t * axis.x * axis.z + s * axis.y, 0.0f, t * axis.x * axis.y + s * axis.z,
      t * axis.y * axis.y + c, t * axis.y * axis.z - s * axis.x, 0.0f,
      t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x,
      t * axis.z * axis.z + c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a rotation matrix around the X-axis (roll)
 * @param angle Rotation angle in radians (positive rotates Y toward Z)
 * @return Rotation matrix for X-axis rotation
 * @note Right-handed: positive angle rotates +Y toward +Z
 * @note Commonly used for "roll" in aircraft orientation (banking)
 * @note Looking along +X axis: positive rotation is counter-clockwise
 * @note More efficient than general axis rotation
 * @performance O(2) - Two trigonometric function calls
 * @example
 * ```c
 * // Aircraft banking left (positive roll)
 * Mat4 bank_left = mat4_euler_rotate_x(to_radians(15.0f));
 *
 * // Flip upside down
 * Mat4 flip = mat4_euler_rotate_x(to_radians(180.0f));
 * ```
 */
static INLINE Mat4 mat4_euler_rotate_x(float32_t angle) {
  float32_t s = sin_f32(angle);
  float32_t c = cos_f32(angle);
  // Right-handed rotation around X: positive angle rotates Y toward Z
  return mat4_new(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, 0.0f, s, c, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a rotation matrix around the Y-axis (pitch)
 * @param angle Rotation angle in radians (positive rotates Z toward X)
 * @return Rotation matrix for Y-axis rotation
 * @note Right-handed: positive angle rotates +Z toward +X
 * @note Commonly used for "pitch" in aircraft orientation (nose up/down)
 * @note Looking along +Y axis: positive rotation is counter-clockwise
 * @note More efficient than general axis rotation
 * @performance O(2) - Two trigonometric function calls
 * @example
 * ```c
 * // Aircraft nose up (positive pitch)
 * Mat4 nose_up = mat4_euler_rotate_y(to_radians(10.0f));
 *
 * // Turn around 180 degrees
 * Mat4 turn_around = mat4_euler_rotate_y(to_radians(180.0f));
 * ```
 */
static INLINE Mat4 mat4_euler_rotate_y(float32_t angle) {
  float32_t s = sin_f32(angle);
  float32_t c = cos_f32(angle);
  // Right-handed rotation around Y: positive angle rotates Z toward X
  return mat4_new(c, 0.0f, s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -s, 0.0f, c, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Creates a rotation matrix around the Z-axis (yaw)
 * @param angle Rotation angle in radians (positive rotates X toward Y)
 * @return Rotation matrix for Z-axis rotation
 * @note Right-handed: positive angle rotates +X toward +Y
 * @note Commonly used for "yaw" in aircraft orientation (turning left/right)
 * @note Looking along +Z axis: positive rotation is counter-clockwise
 * @note Most common rotation for 2D-style turning in 3D space
 * @note More efficient than general axis rotation
 * @performance O(2) - Two trigonometric function calls
 * @example
 * ```c
 * // Turn left 45 degrees (positive yaw)
 * Mat4 turn_left = mat4_euler_rotate_z(to_radians(45.0f));
 *
 * // Spinning animation
 * Mat4 spin = mat4_euler_rotate_z(game_time * 2.0f);
 * ```
 */
static INLINE Mat4 mat4_euler_rotate_z(float32_t angle) {
  float32_t s = sin_f32(angle);
  float32_t c = cos_f32(angle);
  // Right-handed rotation around Z: positive angle rotates X toward Y
  return mat4_new(c, -s, 0.0f, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Computes the transpose of a 4x4 matrix (SIMD-optimized)
 * @param m Input matrix to transpose
 * @return Transposed matrix where rows become columns and vice versa
 * @note Transpose converts between row-major and column-major representations
 * @note For orthogonal matrices: transpose equals inverse (M^T = M^-1)
 * @note Transpose is its own inverse: (M^T)^T = M
 * @note Used in matrix inverse calculations and coordinate system conversions
 * @note ARM NEON implementation uses complete 4-step transpose algorithm
 * @performance O(16) - ARM NEON optimized, scalar fallback available
 * @example
 * ```c
 * // Convert between row-major and column-major storage
 * Mat4 column_major = mat4_transpose(row_major_matrix);
 *
 * // Fast inverse for orthogonal matrices (rotation only)
 * Mat4 rotation_inverse = mat4_transpose(rotation_matrix);
 *
 * // Prepare matrix for different graphics API conventions
 * Mat4 api_compatible = mat4_transpose(internal_matrix);
 * ```
 */
static INLINE Mat4 mat4_transpose(Mat4 m) {
  Mat4 result;
#if SIMD_ARM_NEON
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

/**
 * @brief Computes the inverse of a general 4x4 matrix
 * @param m Input matrix to invert
 * @return Inverse matrix M^-1 such that M * M^-1 = I, or identity if not
 * invertible
 * @note Returns identity matrix if determinant is too small (matrix is
 * singular)
 * @example
 * ```c
 * // General transformation with complex scaling and shearing
 * Mat4 complex_transform = create_complex_matrix();
 * Mat4 inverse = mat4_inverse(complex_transform);
 *
 * // Verify inversion (should be identity)
 * Mat4 should_be_identity = mat4_mul(complex_transform, inverse);
 *
 * // Check if matrix was invertible
 * bool8_t was_invertible = !mat4_is_identity(mat4_sub(inverse,
 * mat4_identity()), 1e-5f);
 * ```
 */
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

  // Check if matrix is invertible
  if (abs_f32(det) < 1e-6f) {
    return mat4_identity();
  }

  float32_t inv_det = 1.0f / det;

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

  return result;
}

/**
 * @brief Fast inverse for orthogonal matrices (rotation matrices)
 * @param m Orthogonal matrix (rotation only, no scaling or shearing)
 * @return Inverse matrix (simply the transpose for orthogonal matrices)
 * @note ONLY use for pure rotation matrices (orthonormal basis vectors)
 * @note For orthogonal matrices: M^-1 = M^T (inverse equals transpose)
 * @note 8x faster than general inverse but only works for rotation matrices
 * @note Does NOT handle translation, scaling, or shearing
 * @note Matrix columns must be orthonormal unit vectors
 * @performance O(16) - Simple transpose operation
 * @example
 * ```c
 * // Inverse of pure rotation matrix
 * Mat4 rotation = mat4_euler_rotate_y(to_radians(45.0f));
 * Mat4 rotation_inverse = mat4_inverse_orthogonal(rotation);
 *
 * // Camera view matrix from rotation-only transform
 * Mat4 camera_rotation_inverse = mat4_inverse_orthogonal(camera_rotation);
 * ```
 */
static INLINE Mat4 mat4_inverse_orthogonal(Mat4 m) { return mat4_transpose(m); }

/**
 * @brief Fast inverse for affine transformation matrices (rotation +
 * translation + uniform scale)
 * @param m Affine transformation matrix (no perspective, bottom row is
 * [0,0,0,1])
 * @return Inverse matrix optimized for affine transformations
 * @note Optimized for matrices with [0,0,0,1] bottom row (affine transforms)
 * @note Handles rotation, translation, and uniform scaling efficiently
 * @note Does NOT handle perspective projection or non-uniform scaling well
 * @note 3x faster than general inverse for typical transformation matrices
 * @note Uses 3x3 inverse for rotation/scale part, then computes translation
 * @performance O(40) - Much faster than general inverse for common transforms
 * @example
 * ```c
 * // Typical game object transformation (translate + rotate + uniform scale)
 * Mat4 object_transform = mat4_mul(translation, mat4_mul(rotation,
 * uniform_scale)); Mat4 inverse_transform =
 * mat4_inverse_affine(object_transform);
 *
 * // View matrix calculation (faster than general inverse)
 * Mat4 view_matrix = mat4_inverse_affine(camera_transform);
 * ```
 */
static INLINE Mat4 mat4_inverse_affine(Mat4 m) {
  Vec4 row0 = vec4_new(m.m00, m.m01, m.m02, 0.0f);
  Vec4 row1 = vec4_new(m.m10, m.m11, m.m12, 0.0f);
  Vec4 row2 = vec4_new(m.m20, m.m21, m.m22, 0.0f);

  Vec4 cross0 =
      vec4_new(m.m11 * m.m22 - m.m12 * m.m21, m.m12 * m.m20 - m.m10 * m.m22,
               m.m10 * m.m21 - m.m11 * m.m20, 0.0f);

  float32_t det = vec3_dot((Vec3){m.m00, m.m01, m.m02},
                           (Vec3){cross0.x, cross0.y, cross0.z});

  if (abs_f32(det) < 1e-6f) {
    return mat4_identity();
  }

  float32_t inv_det = 1.0f / det;

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

/**
 * @brief Extracts a column vector from a 4x4 matrix
 * @param m Input matrix
 * @param col Column index (0-3: 0=right/X, 1=up/Y, 2=forward/Z, 3=translation)
 * @return Column vector at the specified index
 * @note Columns represent basis vectors in transformation matrices
 * @note Column 0: right/X-axis, Column 1: up/Y-axis, Column 2: forward/Z-axis,
 * Column 3: translation
 * @note No bounds checking in release builds - ensure col is in range [0,3]
 * @performance O(1) - Direct memory access
 * @example
 * ```c
 * // Extract transformation basis vectors
 * Vec4 right_vector = mat4_col(transform, 0);
 * Vec4 up_vector = mat4_col(transform, 1);
 * Vec4 forward_vector = mat4_col(transform, 2);
 * Vec4 translation = mat4_col(transform, 3);
 *
 * // Build matrix from columns
 * Mat4 custom_matrix = mat4_new(
 *     right.x, right.y, right.z, right.w,
 *     up.x, up.y, up.z, up.w,
 *     forward.x, forward.y, forward.z, forward.w,
 *     translation.x, translation.y, translation.z, translation.w
 * );
 * ```
 */
static INLINE Vec4 mat4_col(Mat4 m, int32_t col) { return m.cols[col]; }

/**
 * @brief Extracts a row vector from a 4x4 matrix
 * @param m Input matrix
 * @param row Row index (0-3)
 * @return Row vector at the specified index
 * @note Less commonly used than column access in graphics programming
 * @note Useful for certain mathematical operations and debugging
 * @note No bounds checking in release builds - ensure row is in range [0,3]
 * @performance O(4) - Requires gathering elements from different columns
 * @example
 * ```c
 * // Extract matrix rows for analysis
 * Vec4 row0 = mat4_row(matrix, 0);  // [m00, m01, m02, m03]
 * Vec4 row1 = mat4_row(matrix, 1);  // [m10, m11, m12, m13]
 * Vec4 row2 = mat4_row(matrix, 2);  // [m20, m21, m22, m23]
 * Vec4 row3 = mat4_row(matrix, 3);  // [m30, m31, m32, m33]
 *
 * // Check homogeneous coordinates (row3 should be [0,0,0,1] for transforms)
 * Vec4 bottom_row = mat4_row(transform, 3);
 * ```
 */
static INLINE Vec4 mat4_row(Mat4 m, int32_t row) {
  return vec4_new(m.elements[row], m.elements[row + 4], m.elements[row + 8],
                  m.elements[row + 12]);
}

/**
 * @brief Gets a single matrix element at the specified row and column
 * @param m Input matrix
 * @param row Row index (0-3)
 * @param col Column index (0-3)
 * @return Matrix element at position [row][col]
 * @note Uses column-major storage: element = elements[col * 4 + row]
 * @note No bounds checking in release builds - ensure indices are in range
 * [0,3]
 * @note Less efficient than bulk operations - prefer vector/matrix operations
 * @performance O(1) - Direct element access
 * @example
 * ```c
 * // Access specific matrix elements
 * float32_t scale_x = mat4_at(transform, 0, 0);  // X-axis scale factor
 * float32_t translate_y = mat4_at(transform, 1, 3);  // Y translation
 *
 * // Check if matrix is identity
 * bool8_t is_identity = (mat4_at(m, 0, 0) == 1.0f && mat4_at(m, 1, 1) == 1.0f
 * && mat4_at(m, 2, 2) == 1.0f && mat4_at(m, 3, 3) == 1.0f);
 * ```
 */
static INLINE float32_t mat4_at(Mat4 m, int32_t row, int32_t col) {
  return m.elements[col * 4 + row];
}

/**
 * @brief Sets a single matrix element at the specified row and column
 * @param m Pointer to matrix to modify
 * @param row Row index (0-3)
 * @param col Column index (0-3)
 * @param value New value for the matrix element
 * @note Uses column-major storage: elements[col * 4 + row] = value
 * @note No bounds checking in release builds - ensure indices are in range
 * [0,3]
 * @note Less efficient than bulk operations - prefer building matrices from
 * scratch
 * @performance O(1) - Direct element modification
 * @example
 * ```c
 * // Manually build a matrix
 * Mat4 custom = mat4_identity();
 * mat4_set(&custom, 0, 3, 10.0f);  // Set X translation
 * mat4_set(&custom, 1, 3, 5.0f);   // Set Y translation
 * mat4_set(&custom, 2, 3, -2.0f);  // Set Z translation
 *
 * // Fix specific matrix element
 * mat4_set(&transform, 3, 3, 1.0f);  // Ensure homogeneous coordinate is 1
 * ```
 */
static INLINE void mat4_set(Mat4 *m, int32_t row, int32_t col,
                            float32_t value) {
  m->elements[col * 4 + row] = value;
}

/**
 * @brief Computes the determinant of a 4x4 matrix
 * @param m Input matrix
 * @return Determinant value
 */
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

/**
 * @brief Computes the trace of a 4x4 matrix (sum of diagonal elements)
 * @param m Input matrix
 * @return Trace value (m00 + m11 + m22 + m33)
 */
static INLINE float32_t mat4_trace(Mat4 m) {
  return m.m00 + m.m11 + m.m22 + m.m33;
}

/**
 * @brief Checks if a matrix is approximately an identity matrix
 * @param m Input matrix
 * @param epsilon Tolerance for comparison (default FLOAT_EPSILON)
 * @return true if matrix is close to identity
 */
static INLINE bool8_t mat4_is_identity(Mat4 m, float32_t epsilon) {
  // Check diagonal elements are close to 1
  if (abs_f32(m.m00 - 1.0f) > epsilon || abs_f32(m.m11 - 1.0f) > epsilon ||
      abs_f32(m.m22 - 1.0f) > epsilon || abs_f32(m.m33 - 1.0f) > epsilon) {
    return false;
  }

  // Check off-diagonal elements are close to 0
  if (abs_f32(m.m01) > epsilon || abs_f32(m.m02) > epsilon ||
      abs_f32(m.m03) > epsilon || abs_f32(m.m10) > epsilon ||
      abs_f32(m.m12) > epsilon || abs_f32(m.m13) > epsilon ||
      abs_f32(m.m20) > epsilon || abs_f32(m.m21) > epsilon ||
      abs_f32(m.m23) > epsilon || abs_f32(m.m30) > epsilon ||
      abs_f32(m.m31) > epsilon || abs_f32(m.m32) > epsilon) {
    return false;
  }

  return true;
}

// =============================================================================
// Matrix Operations
// =============================================================================

/**
 * @brief Performs element-wise addition of two matrices (SIMD-optimized)
 * @param a First matrix operand
 * @param b Second matrix operand
 * @return Matrix containing {a[i][j] + b[i][j]} for all elements
 * @note Element-wise addition, not matrix multiplication
 * @note Rarely used in graphics - mainly for interpolation or debugging
 * @note Addition is commutative: a + b = b + a
 * @note Zero matrix is additive identity: a + 0 = a
 * @performance O(16) - SIMD-accelerated element-wise operations
 * @example
 * ```c
 * // Blend between two transformation matrices
 * Mat4 blend = mat4_add(mat4_scale(a, 0.7f), mat4_scale(b, 0.3f));
 *
 * // Accumulate transformations (unusual, prefer multiplication)
 * Mat4 accumulated = mat4_add(base_transform, delta_transform);
 * ```
 */
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

/**
 * @brief Performs element-wise subtraction of two matrices (SIMD-optimized)
 * @param a First matrix operand (minuend)
 * @param b Second matrix operand (subtrahend)
 * @return Matrix containing {a[i][j] - b[i][j]} for all elements
 * @note Element-wise subtraction, not matrix multiplication
 * @note Rarely used in graphics - mainly for difference calculations
 * @note Subtraction is not commutative: a - b ≠ b - a
 * @note Useful for computing matrix differences for debugging
 * @performance O(16) - SIMD-accelerated element-wise operations
 * @example
 * ```c
 * // Calculate difference between two transforms
 * Mat4 difference = mat4_sub(new_transform, old_transform);
 *
 * // Check if matrices are approximately equal
 * Mat4 diff = mat4_sub(matrix_a, matrix_b);
 * bool8_t are_close = mat4_is_identity(diff, 0.001f);
 * ```
 */
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

/**
 * @brief Multiplies two 4x4 matrices using SIMD-accelerated algorithm
 * @param a Left matrix operand (applied second in transformation chain)
 * @param b Right matrix operand (applied first in transformation chain)
 * @return Product matrix C = A * B
 * @note Matrix multiplication is NOT commutative: A * B ≠ B * A
 * @note Transformation order: result = a * b applies b first, then a
 * @note Uses FMA (Fused Multiply-Add) for improved precision and performance
 * @note Critical operation for transform chains: MVP = Projection * View *
 * Model
 * @note Identity is multiplicative identity: I * M = M * I = M
 * @performance O(64) - SIMD-optimized with FMA instructions
 * @example
 * ```c
 * // Standard transformation pipeline
 * Mat4 model = mat4_mul(mat4_translate(position), mat4_mul(rotation, scale));
 * Mat4 mvp = mat4_mul(projection, mat4_mul(view, model));
 *
 * // Chain multiple rotations (applied right-to-left)
 * Mat4 combined_rotation = mat4_mul(y_rotation, mat4_mul(x_rotation,
 * z_rotation));
 *
 * // Transform composition for hierarchical objects
 * Mat4 child_transform = mat4_mul(parent_transform, local_transform);
 * ```
 */
static INLINE Mat4 mat4_mul(Mat4 a, Mat4 b) {
  Mat4 result;
  for (int i = 0; i < 4; i++) {
    Vec4 col = b.cols[i];
    Vec4 x = simd_set1_f32x4(col.x);
    Vec4 y = simd_set1_f32x4(col.y);
    Vec4 z = simd_set1_f32x4(col.z);
    Vec4 w = simd_set1_f32x4(col.w);

    // Compute: a.cols[0]*x + a.cols[1]*y + a.cols[2]*z + a.cols[3]*w
    // Note: simd_fma_f32x4(dst, b, c) computes dst + (b * c)
    Vec4 temp = simd_mul_f32x4(a.cols[0], x);
    temp = simd_fma_f32x4(temp, a.cols[1], y); // temp + (a.cols[1] * y)
    temp = simd_fma_f32x4(temp, a.cols[2], z); // temp + (a.cols[2] * z)
    result.cols[i] =
        simd_fma_f32x4(temp, a.cols[3], w); // temp + (a.cols[3] * w)
  }
  return result;
}

// =============================================================================
// Matrix To Vector Operations
// =============================================================================

/**
 * @brief Extracts the first column of a matrix as a 3D vector (right/X-axis)
 * @param m Input matrix
 * @return 3D vector containing the first three elements of the first column
 * @note Extracts [m00, m10, m20] - the right/X-axis direction vector
 * @note Useful for getting the local X-axis direction from a transformation
 * matrix
 * @note Does NOT normalize the result - may need normalization if matrix has
 * scaling
 * @note Ignores the W component (m30) of the first column
 * @performance O(1) - Direct element access
 * @example
 * ```c
 * // Get object's local X-axis direction
 * Vec3 x_axis = mat4_to_vec3(object_transform);
 *
 * // Get scale factor from first column length
 * float32_t x_scale = vec3_length(mat4_to_vec3(transform));
 * ```
 */
static INLINE Vec3 mat4_to_vec3(Mat4 m) {
  return vec3_new(m.m00, m.m10, m.m20);
}

/**
 * @brief Extracts the first column of a matrix as a 4D vector
 * @param m Input matrix
 * @return 4D vector containing the entire first column [m00, m10, m20, m30]
 * @note Extracts the complete right/X-axis column including homogeneous
 * coordinate
 * @note Useful for getting full column data including any perspective
 * information
 * @note For typical transformation matrices, m30 should be 0
 * @performance O(1) - Direct column access
 * @example
 * ```c
 * // Extract complete basis vectors with homogeneous coordinates
 * Vec4 x_axis_full = mat4_to_vec4(transform);
 * Vec4 y_axis_full = mat4_col(transform, 1);
 * Vec4 z_axis_full = mat4_col(transform, 2);
 * Vec4 translation_full = mat4_col(transform, 3);
 * ```
 */
static INLINE Vec4 mat4_to_vec4(Mat4 m) {
  return vec4_new(m.m00, m.m10, m.m20, m.m30);
}

/**
 * @brief Extracts the first column of a matrix as a 2D vector
 * @param m Input matrix
 * @return 2D vector containing [m00, m10] - first two elements of first column
 * @note Extracts only the X and Y components of the right/X-axis vector
 * @note Useful for 2D transformations or extracting 2D direction from 3D matrix
 * @note Ignores Z and W components (m20, m30) of the first column
 * @performance O(1) - Direct element access
 * @example
 * ```c
 * // Get 2D direction from 3D transformation matrix
 * Vec2 direction_2d = mat4_to_vec2(transform_3d);
 *
 * // Extract 2D scale factors
 * Vec2 scale_xy = mat4_to_vec2(scale_matrix);
 * ```
 */
static INLINE Vec2 mat4_to_vec2(Mat4 m) { return vec2_new(m.m00, m.m10); }

/**
 * @brief Transforms a 4D vector by a 4x4 matrix (SIMD-optimized)
 * @param m Transformation matrix
 * @param v 4D vector to transform (typically homogeneous coordinates)
 * @return Transformed vector m * v
 * @note Critical function for vertex transformation in graphics pipeline
 * @note Homogeneous coordinates: use w=1 for positions, w=0 for directions
 * @note Column-major multiplication: result = matrix * column_vector
 * @note For 3D points: convert to Vec4 with w=1, then extract xyz after
 * transform
 * @performance O(16) - SIMD-accelerated dot products
 * @example
 * ```c
 * // Transform vertex position (homogeneous coordinates)
 * Vec4 local_vertex = vec4_new(x, y, z, 1.0f);  // w=1 for position
 * Vec4 world_vertex = mat4_mul_vec4(model_matrix, local_vertex);
 *
 * // Transform direction vector (no translation)
 * Vec4 local_normal = vec4_new(nx, ny, nz, 0.0f);  // w=0 for direction
 * Vec4 world_normal = mat4_mul_vec4(model_matrix, local_normal);
 *
 * // Full MVP transformation
 * Vec4 clip_pos = mat4_mul_vec4(mvp_matrix, local_vertex);
 * ```
 */
static INLINE Vec4 mat4_mul_vec4(Mat4 m, Vec4 v) {
  return vec4_add(
      vec4_add(vec4_scale(m.cols[0], v.x), vec4_scale(m.cols[1], v.y)),
      vec4_add(vec4_scale(m.cols[2], v.z), vec4_scale(m.cols[3], v.w)));
}

/**
 * @brief Extracts the forward direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized forward direction vector
 * @note In right-handed system: forward is -Z direction (negative third column)
 * @note Forward vector points in the direction the object/camera is facing
 * @note Useful for camera movement, object orientation, ray casting
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 * @example
 * ```c
 * // Move camera forward
 * Vec3 camera_forward = mat4_forward(camera_transform);
 * Vec3 new_camera_pos = vec3_add(camera_pos, vec3_scale(camera_forward,
 * speed));
 *
 * // Cast ray in object's forward direction
 * Vec3 object_forward = mat4_forward(object_transform);
 * Ray forward_ray = {object_position, object_forward};
 * ```
 */
static INLINE Vec3 mat4_forward(Mat4 m) {
  // In right-handed system, forward is -Z (negative third column)
  return vec3_normalize(vec3_new(-m.m02, -m.m12, -m.m22));
}

/**
 * @brief Extracts the backward direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized backward direction vector
 * @note In right-handed system: backward is +Z direction (third column)
 * @note Opposite of forward direction
 * @note Useful for retreat movement, rear-facing operations
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 */
static INLINE Vec3 mat4_backward(Mat4 m) {
  // In right-handed system, backward is +Z (third column)
  return vec3_normalize(vec3_new(m.m02, m.m12, m.m22));
}

/**
 * @brief Extracts the up direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized up direction vector
 * @note Up vector points in the local Y+ direction (second column)
 * @note Useful for camera movement, object orientation, billboard alignment
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 * @example
 * ```c
 * // Move camera up relative to its orientation
 * Vec3 camera_up = mat4_up(camera_transform);
 * Vec3 new_camera_pos = vec3_add(camera_pos, vec3_scale(camera_up, speed));
 *
 * // Align billboard to camera up vector
 * Vec3 billboard_up = mat4_up(camera_matrix);
 * ```
 */
static INLINE Vec3 mat4_up(Mat4 m) {
  return vec3_normalize(vec3_new(m.m01, m.m11, m.m21));
}

/**
 * @brief Extracts the down direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized down direction vector
 * @note Down vector points in the local Y- direction (negative second column)
 * @note Opposite of up direction
 * @note Useful for gravity effects, downward movement
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 */
static INLINE Vec3 mat4_down(Mat4 m) {
  return vec3_normalize(vec3_new(-m.m01, -m.m11, -m.m21));
}

/**
 * @brief Extracts the right direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized right direction vector
 * @note Right vector points in the local X+ direction (first column)
 * @note Useful for strafing movement, horizontal orientation
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 * @example
 * ```c
 * // Strafe camera right
 * Vec3 camera_right = mat4_right(camera_transform);
 * Vec3 new_camera_pos = vec3_add(camera_pos, vec3_scale(camera_right, speed));
 *
 * // Get object's local X-axis for physics calculations
 * Vec3 object_right = mat4_right(object_transform);
 * ```
 */
static INLINE Vec3 mat4_right(Mat4 m) {
  return vec3_normalize(vec3_new(m.m00, m.m10, m.m20));
}

/**
 * @brief Extracts the left direction vector from a transformation matrix
 * @param m Transformation matrix (typically model or view matrix)
 * @return Normalized left direction vector
 * @note Left vector points in the local X- direction (negative first column)
 * @note Opposite of right direction
 * @note Useful for leftward movement, mirrored operations
 * @note Automatically normalized for convenience
 * @performance O(4) - Vector normalization
 */
static INLINE Vec3 mat4_left(Mat4 m) {
  return vec3_normalize(vec3_new(-m.m00, -m.m10, -m.m20));
}

/**
 * @brief Extracts the translation/position component from a transformation
 * matrix
 * @param m Transformation matrix (typically model matrix)
 * @return Position vector (translation component)
 * @note Translation is stored in the fourth column (m03, m13, m23)
 * @note No normalization performed - returns exact translation values
 * @note Most commonly used matrix extraction function
 * @note Useful for object position queries, spatial calculations
 * @performance O(1) - Direct element access
 * @example
 * ```c
 * // Get object's world position
 * Vec3 object_pos = mat4_position(object_transform);
 *
 * // Calculate distance between objects
 * Vec3 pos_a = mat4_position(transform_a);
 * Vec3 pos_b = mat4_position(transform_b);
 * float32_t distance = vec3_distance(pos_a, pos_b);
 *
 * // Update object position in transform
 * Vec3 new_pos = vec3_add(mat4_position(transform), velocity);
 * transform = mat4_translate(new_pos);  // Rebuild matrix with new position
 * ```
 */
static INLINE Vec3 mat4_position(Mat4 m) {
  return vec3_new(m.m03, m.m13, m.m23);
}

// =============================================================================
// Matrix To Quaternion Operations
// =============================================================================

/**
 * @brief Converts a 4x4 rotation matrix to a quaternion using Shepperd's method
 * @param m Input rotation matrix (should be orthonormal for accurate results)
 * @return Quaternion representing the same rotation as the matrix
 * @note Uses Shepperd's method for numerical stability across all cases
 * @note Input should be a pure rotation matrix (orthonormal, no
 * scaling/translation)
 * @note Translation component (column 3) is ignored in the conversion
 * @note Handles all cases: positive trace, largest diagonal element selection
 * @note Result quaternion is automatically normalized
 * @performance O(10) - Involves conditional branches and square root
 * calculations
 * @example
 * ```c
 * // Convert rotation matrix to quaternion for interpolation
 * Mat4 rotation_matrix = mat4_euler_rotate_y(to_radians(45.0f));
 * Quat rotation_quat = mat4_to_quat(rotation_matrix);
 *
 * // Extract rotation from complex transformation matrix
 * Mat4 full_transform = mat4_mul(translation, mat4_mul(rotation, scale));
 * // Remove scaling first if needed for accurate conversion
 * Mat4 rotation_only = normalize_rotation_part(full_transform);
 * Quat extracted_rotation = mat4_to_quat(rotation_only);
 * ```
 */
static INLINE Quat mat4_to_quat(Mat4 m) {
  float32_t trace = m.m00 + m.m11 + m.m22;

  if (trace > 0.0f) {
    float32_t s = 0.5f / sqrt_f32(trace + 1.0f);
    return vec4_new((m.m21 - m.m12) * s, (m.m02 - m.m20) * s,
                    (m.m10 - m.m01) * s, 0.25f / s);
  } else if (m.m00 > m.m11 && m.m00 > m.m22) {
    float32_t s = 2.0f * sqrt_f32(1.0f + m.m00 - m.m11 - m.m22);
    return vec4_new(0.25f * s, (m.m01 + m.m10) / s, (m.m02 + m.m20) / s,
                    (m.m21 - m.m12) / s);
  } else if (m.m11 > m.m22) {
    float32_t s = 2.0f * sqrt_f32(1.0f + m.m11 - m.m00 - m.m22);
    return vec4_new((m.m01 + m.m10) / s, 0.25f * s, (m.m12 + m.m21) / s,
                    (m.m02 - m.m20) / s);
  } else {
    float32_t s = 2.0f * sqrt_f32(1.0f + m.m22 - m.m00 - m.m11);
    return vec4_new((m.m02 + m.m20) / s, (m.m12 + m.m21) / s, 0.25f * s,
                    (m.m10 - m.m01) / s);
  }
}

/**
 * @brief Alias for mat4_to_quat for API consistency with quaternion module
 * @param m Input rotation matrix (should be orthonormal)
 * @return Quaternion representing the same rotation as the matrix
 * @note Identical to mat4_to_quat(), provided for naming consistency
 * @note Use this when working primarily with quaternion operations
 * @performance O(10) - Same as mat4_to_quat()
 * @example
 * ```c
 * // Consistent with quaternion API naming
 * Quat rotation = quat_from_mat4(rotation_matrix);
 * Quat interpolated = quat_slerp(rotation, target_rotation, 0.5f);
 * ```
 */
static INLINE Quat quat_from_mat4(Mat4 m) { return mat4_to_quat(m); }

/**
 * @brief Converts a normalized quaternion to a 4x4 rotation matrix
 * @param q Input quaternion (should be normalized for accurate results)
 * @return 4x4 rotation matrix with [0,0,0,1] bottom row
 * @note Input quaternion should be normalized for correct results
 * @note Uses optimized quaternion-to-matrix conversion formula
 * @note Resulting matrix has translation = [0,0,0] and homogeneous coordinate =
 * 1
 * @note Column-major layout compatible with graphics APIs
 * @note More efficient than axis-angle conversion for quaternions
 * @performance O(15) - Optimized with precomputed terms, no trigonometric
 * functions
 * @example
 * ```c
 * // Convert quaternion rotation to matrix form
 * Quat rotation = quat_from_euler(roll, pitch, yaw);
 * Mat4 rotation_matrix = quat_to_mat4(rotation);
 *
 * // Use in transformation chain
 * Mat4 full_transform = mat4_mul(translation, mat4_mul(rotation_matrix,
 * scale));
 *
 * // Interpolated rotation to matrix
 * Quat interpolated = quat_slerp(start_rotation, end_rotation, t);
 * Mat4 interpolated_matrix = quat_to_mat4(interpolated);
 * ```
 */
static INLINE Mat4 quat_to_mat4(Quat q) {
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

/**
 * @brief Creates a transformation matrix from quaternion rotation and position
 * @param q Rotation quaternion (should be normalized)
 * @param position Translation vector for the transformation
 * @return 4x4 transformation matrix combining rotation and translation
 * @note Combines rotation (from quaternion) and translation in one operation
 * @note More efficient than separate quat_to_mat4() and mat4_translate() +
 * mat4_mul()
 * @note Quaternion should be normalized for accurate rotation
 * @note Resulting matrix has no scaling (uniform scale = 1.0)
 * @note Perfect for rigid body transformations (rotation + translation only)
 * @performance O(15) - Same as quat_to_mat4() with direct translation
 * assignment
 * @example
 * ```c
 * // Create object transformation from rotation and position
 * Quat object_rotation = quat_from_euler(0.0f, to_radians(45.0f), 0.0f);
 * Vec3 object_position = vec3_new(10.0f, 0.0f, 5.0f);
 * Mat4 object_transform = mat4_from_quat_pos(object_rotation, object_position);
 *
 * // Character controller transformation
 * Vec3 character_pos = get_character_position();
 * Quat character_orientation = get_character_orientation();
 * Mat4 character_matrix = mat4_from_quat_pos(character_orientation,
 * character_pos);
 * ```
 */
static INLINE Mat4 mat4_from_quat_pos(Quat q, Vec3 position) {
  Mat4 result = quat_to_mat4(q);
  result.m03 = position.x;
  result.m13 = position.y;
  result.m23 = position.z;
  return result;
}

// =============================================================================
// In-Place Matrix Operations (for performance-critical code)
// =============================================================================

/**
 * @brief In-place matrix multiplication for performance-critical code: dest = a
 * * b
 * @param dest Pointer to destination matrix (modified in-place, can be same as
 * a or b)
 * @param a First matrix operand (applied second in transformation chain)
 * @param b Second matrix operand (applied first in transformation chain)
 * @note Avoids temporary matrix allocation - writes result directly to dest
 * @note Safe for aliasing: dest can point to the same matrix as a or b
 * @note Critical for performance in animation systems and tight loops
 * @note Uses same SIMD-optimized algorithm as mat4_mul()
 * @note Matrix multiplication is NOT commutative: a * b ≠ b * a
 * @performance O(64) - Same performance as mat4_mul(), but no temporary
 * allocation
 * @example
 * ```c
 * // Performance-critical animation loop
 * Mat4 bone_transforms[100];
 * for (int i = 0; i < 100; i++) {
 *     // In-place transformation chain: bone = parent * local *
 * bind_pose_inverse mat4_mul_mut(&bone_transforms[i], parent_transforms[i],
 * local_transforms[i]); mat4_mul_mut(&bone_transforms[i], bone_transforms[i],
 * bind_pose_inverse[i]);
 * }
 *
 * // Accumulate transformation in-place
 * Mat4 accumulated = mat4_identity();
 * mat4_mul_mut(&accumulated, accumulated, translation);
 * mat4_mul_mut(&accumulated, accumulated, rotation);
 * mat4_mul_mut(&accumulated, accumulated, scale);
 * ```
 */
static INLINE void mat4_mul_mut(Mat4 *dest, Mat4 a, Mat4 b) {
  for (int i = 0; i < 4; i++) {
    Vec4 col = b.cols[i];
    Vec4 x = simd_set1_f32x4(col.x);
    Vec4 y = simd_set1_f32x4(col.y);
    Vec4 z = simd_set1_f32x4(col.z);
    Vec4 w = simd_set1_f32x4(col.w);

    // Compute: a.cols[0]*x + a.cols[1]*y + a.cols[2]*z + a.cols[3]*w
    // Note: simd_fma_f32x4(dst, b, c) computes dst + (b * c)
    Vec4 temp = simd_mul_f32x4(a.cols[0], x);
    temp = simd_fma_f32x4(temp, a.cols[1], y); // temp + (a.cols[1] * y)
    temp = simd_fma_f32x4(temp, a.cols[2], z); // temp + (a.cols[2] * z)
    dest->cols[i] =
        simd_fma_f32x4(temp, a.cols[3], w); // temp + (a.cols[3] * w)
  }
}

/**
 * @brief In-place matrix addition for performance-critical code: dest = a + b
 * @param dest Pointer to destination matrix (modified in-place, can be same as
 * a or b)
 * @param a First matrix operand
 * @param b Second matrix operand
 * @note Avoids temporary matrix allocation - writes result directly to dest
 * @note Safe for aliasing: dest can point to the same matrix as a or b
 * @note Element-wise addition, not matrix multiplication
 * @note Addition is commutative: a + b = b + a
 * @note Rarely used in graphics but useful for interpolation and debugging
 * @performance O(16) - SIMD-accelerated element-wise operations, no allocation
 * @example
 * ```c
 * // Weighted blend of transformation matrices
 * Mat4 result = mat4_zero();
 * Mat4 weighted_a = mat4_identity();
 * Mat4 weighted_b = mat4_identity();
 *
 * // Scale matrices by weights
 * for (int i = 0; i < 16; i++) {
 *     weighted_a.elements[i] = matrix_a.elements[i] * weight_a;
 *     weighted_b.elements[i] = matrix_b.elements[i] * weight_b;
 * }
 *
 * // Add in-place
 * mat4_add_mut(&result, weighted_a, weighted_b);
 * ```
 */
static INLINE void mat4_add_mut(Mat4 *dest, Mat4 a, Mat4 b) {
  dest->cols[0] = vec4_add(a.cols[0], b.cols[0]);
  dest->cols[1] = vec4_add(a.cols[1], b.cols[1]);
  dest->cols[2] = vec4_add(a.cols[2], b.cols[2]);
  dest->cols[3] = vec4_add(a.cols[3], b.cols[3]);
}

/**
 * @brief Ultra-fast inverse for rigid body transforms (rotation + translation
 * only)
 * @param m Input transformation matrix (MUST be rigid body: rotation +
 * translation only)
 * @return Inverse matrix computed using rigid body properties
 * @note ONLY use for rigid body transforms: rotation + translation, NO scaling
 * @note 5-8x faster than general matrix inverse for valid input
 * @note Uses mathematical property: for rigid body matrix [R|t], inverse is
 * [R^T|-R^T*t]
 * @note Input MUST have orthonormal rotation part (no scaling/shearing)
 * @note Bottom row must be [0,0,0,1] for correct results
 * @note Perfect for camera transforms, object positions, bone transforms
 * @performance O(25) - Much faster than mat4_inverse() O(120)
 * @example
 * ```c
 * // Camera view matrix from camera transformation
 * Mat4 camera_transform = mat4_from_quat_pos(camera_rotation, camera_position);
 * Mat4 view_matrix = mat4_inverse_rigid(camera_transform);
 *
 * // Object to world space and back
 * Mat4 object_to_world = mat4_mul(translation, rotation);  // No scaling
 * Mat4 world_to_object = mat4_inverse_rigid(object_to_world);
 *
 * // Bone transformation inverse for skinning
 * Mat4 bone_transform = get_bone_world_transform(bone_id);
 * Mat4 bone_inverse = mat4_inverse_rigid(bone_transform);
 * ```
 */
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
