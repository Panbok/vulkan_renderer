#include "mat_test.h"

// Helper function for floating-point comparison with epsilon
static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return abs_f32(a - b) < epsilon;
}

// Helper function for Mat4 comparison
static bool32_t mat4_equals(Mat4 a, Mat4 b, float32_t epsilon) {
  for (int i = 0; i < 16; i++) {
    if (!float_equals(a.elements[i], b.elements[i], epsilon)) {
      return false;
    }
  }
  return true;
}

// Helper function for Vec3 comparison
static bool32_t vec3_equals(Vec3 a, Vec3 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon);
}

// Helper function for Vec4 comparison
static bool32_t vec4_equals(Vec4 a, Vec4 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon) && float_equals(a.w, b.w, epsilon);
}

// =============================================================================
// Matrix Constructor Tests
// =============================================================================

static void test_mat4_constructors(void) {
  printf("  Running test_mat4_constructors...\n");

  // Test mat4_new
  Mat4 m1 = mat4_new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 10, 15, 1);
  assert(float_equals(m1.m00, 1.0f, FLOAT_EPSILON) && "mat4_new m00 failed");
  assert(float_equals(m1.m03, 5.0f, FLOAT_EPSILON) && "mat4_new m03 failed");
  assert(float_equals(m1.m13, 10.0f, FLOAT_EPSILON) && "mat4_new m13 failed");
  assert(float_equals(m1.m23, 15.0f, FLOAT_EPSILON) && "mat4_new m23 failed");

  // Test mat4_zero
  Mat4 zero = mat4_zero();
  Mat4 expected_zero = mat4_new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  assert(mat4_equals(zero, expected_zero, FLOAT_EPSILON) && "mat4_zero failed");

  // Test mat4_identity
  Mat4 identity = mat4_identity();
  Mat4 expected_identity =
      mat4_new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
  assert(mat4_equals(identity, expected_identity, FLOAT_EPSILON) &&
         "mat4_identity failed");

  // Test mat4_translate
  Mat4 translate = mat4_translate(vec3_new(2.0f, 3.0f, 4.0f));
  Mat4 expected_translate =
      mat4_new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1);
  assert(mat4_equals(translate, expected_translate, FLOAT_EPSILON) &&
         "mat4_translate failed");

  // Test mat4_scale
  Mat4 scale = mat4_scale(vec3_new(2.0f, 3.0f, 4.0f));
  Mat4 expected_scale =
      mat4_new(2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1);
  assert(mat4_equals(scale, expected_scale, FLOAT_EPSILON) &&
         "mat4_scale failed");

  printf("  test_mat4_constructors PASSED\n");
}

static void test_mat4_rotation_constructors(void) {
  printf("  Running test_mat4_rotation_constructors...\n");

  // Test mat4_euler_rotate_x (90 degrees)
  Mat4 rot_x = mat4_euler_rotate_x(to_radians(90.0f));
  Vec3 test_y = vec3_up();
  Vec3 rotated_y =
      vec4_to_vec3(mat4_mul_vec4(rot_x, vec3_to_vec4(test_y, 1.0f)));
  assert(vec3_equals(rotated_y, vec3_forward(), 0.001f) && "X rotation failed");

  // Test mat4_euler_rotate_y (90 degrees)
  Mat4 rot_y = mat4_euler_rotate_y(to_radians(90.0f));
  Vec3 test_x = vec3_right();
  Vec3 rotated_x =
      vec4_to_vec3(mat4_mul_vec4(rot_y, vec3_to_vec4(test_x, 1.0f)));
  assert(vec3_equals(rotated_x, vec3_back(), 0.001f) && "Y rotation failed");

  // Test mat4_euler_rotate_z (90 degrees)
  // In right-handed system: +90° around +Z rotates +X toward -Y (clockwise when
  // looking down +Z)
  Mat4 rot_z = mat4_euler_rotate_z(to_radians(90.0f));
  Vec3 test_x_z = vec3_right();
  Vec3 rotated_x_z =
      vec4_to_vec3(mat4_mul_vec4(rot_z, vec3_to_vec4(test_x_z, 1.0f)));
  assert(vec3_equals(rotated_x_z, vec3_down(), 0.001f) && "Z rotation failed");

  // Test arbitrary axis rotation
  Vec3 axis = vec3_normalize(vec3_new(1.0f, 1.0f, 1.0f));
  Mat4 rot_axis = mat4_euler_rotate(axis, to_radians(120.0f));
  // Just verify the matrix is orthogonal (rotation matrices preserve
  // orthogonality)
  Mat4 rot_transpose = mat4_transpose(rot_axis);
  Mat4 should_be_identity = mat4_mul(rot_axis, rot_transpose);
  assert(mat4_is_identity(should_be_identity, 0.001f) &&
         "Arbitrary axis rotation not orthogonal");

  printf("  test_mat4_rotation_constructors PASSED\n");
}

static void test_mat4_accessors(void) {
  printf("  Running test_mat4_accessors...\n");

  Mat4 test_matrix =
      mat4_new(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);

  // Test mat4_col
  Vec4 col0 = mat4_col(test_matrix, 0);
  assert(vec4_equals(col0, vec4_new(1, 2, 3, 4), FLOAT_EPSILON) &&
         "mat4_col 0 failed");

  Vec4 col3 = mat4_col(test_matrix, 3);
  assert(vec4_equals(col3, vec4_new(13, 14, 15, 16), FLOAT_EPSILON) &&
         "mat4_col 3 failed");

  // Test mat4_row
  Vec4 row0 = mat4_row(test_matrix, 0);
  assert(vec4_equals(row0, vec4_new(1, 5, 9, 13), FLOAT_EPSILON) &&
         "mat4_row 0 failed");

  Vec4 row3 = mat4_row(test_matrix, 3);
  assert(vec4_equals(row3, vec4_new(4, 8, 12, 16), FLOAT_EPSILON) &&
         "mat4_row 3 failed");

  // Test mat4_at
  assert(float_equals(mat4_at(test_matrix, 0, 0), 1.0f, FLOAT_EPSILON) &&
         "mat4_at(0,0) failed");
  assert(float_equals(mat4_at(test_matrix, 2, 1), 7.0f, FLOAT_EPSILON) &&
         "mat4_at(2,1) failed");
  assert(float_equals(mat4_at(test_matrix, 3, 3), 16.0f, FLOAT_EPSILON) &&
         "mat4_at(3,3) failed");

  // Test mat4_set
  Mat4 set_test = mat4_identity();
  mat4_set(&set_test, 1, 2, 42.0f);
  assert(float_equals(mat4_at(set_test, 1, 2), 42.0f, FLOAT_EPSILON) &&
         "mat4_set failed");

  // Test mat4_determinant
  Mat4 det_test = mat4_identity();
  float det = mat4_determinant(det_test);
  assert(float_equals(det, 1.0f, 0.001f) && "mat4_determinant identity failed");

  // Test determinant of zero matrix
  Mat4 zero = mat4_zero();
  float zero_det = mat4_determinant(zero);
  assert(float_equals(zero_det, 0.0f, FLOAT_EPSILON) &&
         "mat4_determinant zero failed");

  // Test determinant of 2x scale matrix (should be 8.0 for uniform 2x scale)
  Mat4 scale_test = mat4_scale(vec3_new(2.0f, 2.0f, 2.0f));
  float scale_det = mat4_determinant(scale_test);
  assert(float_equals(scale_det, 8.0f, 0.001f) &&
         "mat4_determinant scale failed");

  // Test mat4_trace
  float trace = mat4_trace(test_matrix);
  assert(float_equals(trace, 34.0f, FLOAT_EPSILON) &&
         "mat4_trace failed"); // 1+6+11+16 = 34

  // Test mat4_is_identity
  assert(mat4_is_identity(mat4_identity(), FLOAT_EPSILON) &&
         "mat4_is_identity true failed");
  assert(!mat4_is_identity(test_matrix, FLOAT_EPSILON) &&
         "mat4_is_identity false failed");

  printf("  test_mat4_accessors PASSED\n");
}

static void test_mat4_operations(void) {
  printf("  Running test_mat4_operations...\n");

  Mat4 a = mat4_new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1);
  Mat4 b = mat4_new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1);

  // Test mat4_add
  Mat4 add_result = mat4_add(a, b);
  Mat4 expected_add = mat4_new(2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 5, 7, 9, 2);
  assert(mat4_equals(add_result, expected_add, FLOAT_EPSILON) &&
         "mat4_add failed");

  // Test mat4_sub
  Mat4 sub_result = mat4_sub(a, b);
  Mat4 expected_sub =
      mat4_new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -3, -3, -3, 0);
  assert(mat4_equals(sub_result, expected_sub, FLOAT_EPSILON) &&
         "mat4_sub failed");

  // Test mat4_mul (matrix multiplication)
  Mat4 scale2 = mat4_scale(vec3_new(2.0f, 2.0f, 2.0f));
  Mat4 translate1 = mat4_translate(vec3_new(1.0f, 2.0f, 3.0f));
  Mat4 mul_result = mat4_mul(translate1, scale2);

  // Matrix multiplication order: translate1 * scale2 applies scale first, then
  // translate
  // Point (0,0,0) -> scale by (2,2,2) -> (0,0,0) -> translate by (1,2,3) ->
  // (1,2,3)
  Vec4 test_point = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
  Vec4 transformed = mat4_mul_vec4(mul_result, test_point);
  assert(vec4_equals(transformed, vec4_new(1.0f, 2.0f, 3.0f, 1.0f), 0.001f) &&
         "mat4_mul transformation failed");

  // Test mat4_transpose
  Mat4 test_transpose =
      mat4_new(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
  Mat4 transposed = mat4_transpose(test_transpose);
  Mat4 expected_transpose =
      mat4_new(1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 4, 8, 12, 16);
  assert(mat4_equals(transposed, expected_transpose, FLOAT_EPSILON) &&
         "mat4_transpose failed");

  // Test in-place operations
  Mat4 mut_test = mat4_identity();
  mat4_mul_mut(&mut_test, translate1, scale2);
  assert(mat4_equals(mut_test, mul_result, FLOAT_EPSILON) &&
         "mat4_mul_mut failed");

  printf("  test_mat4_operations PASSED\n");
}

static void test_mat4_vector_extraction(void) {
  printf("  Running test_mat4_vector_extraction...\n");

  // Create a transform matrix with known orientation
  Mat4 transform = mat4_mul(mat4_translate(vec3_new(10.0f, 20.0f, 30.0f)),
                            mat4_euler_rotate_y(to_radians(90.0f)));

  // Test position extraction
  Vec3 position = mat4_position(transform);
  assert(vec3_equals(position, vec3_new(10.0f, 20.0f, 30.0f), 0.001f) &&
         "mat4_position failed");

  // Test direction vector extraction (after 90° Y rotation)
  Vec3 right = mat4_right(transform);
  Vec3 up = mat4_up(transform);
  Vec3 forward = mat4_forward(transform);

  // After 90° Y rotation: right becomes backward, forward becomes right
  assert(vec3_equals(right, vec3_back(), 0.001f) && "mat4_right failed");
  assert(vec3_equals(up, vec3_up(), 0.001f) && "mat4_up failed");
  assert(vec3_equals(forward, vec3_right(), 0.001f) && "mat4_forward failed");

  // Test vector conversion functions
  Vec3 first_col = mat4_to_vec3(transform);
  assert(vec3_equals(first_col, vec3_back(), 0.001f) && "mat4_to_vec3 failed");

  Vec4 first_col_4d = mat4_to_vec4(transform);
  assert(vec4_equals(first_col_4d, vec4_new(0.0f, 0.0f, 1.0f, 0.0f), 0.001f) &&
         "mat4_to_vec4 failed");

  printf("  test_mat4_vector_extraction PASSED\n");
}

static void test_mat4_inverse_operations(void) {
  printf("  Running test_mat4_inverse_operations...\n");

  // Test general inverse with identity
  Mat4 identity = mat4_identity();
  Mat4 identity_inv = mat4_inverse(identity);
  assert(mat4_is_identity(identity_inv, 0.001f) && "Identity inverse failed");

  // Test orthogonal inverse (rotation matrix)
  Mat4 rotation = mat4_euler_rotate_z(to_radians(45.0f));
  Mat4 rotation_inv = mat4_inverse_orthogonal(rotation);
  Mat4 should_be_identity = mat4_mul(rotation, rotation_inv);
  assert(mat4_is_identity(should_be_identity, 0.001f) &&
         "Orthogonal inverse failed");

  // Test rigid body inverse (rotation + translation)
  Mat4 rigid_transform = mat4_mul(mat4_translate(vec3_new(5.0f, 10.0f, 15.0f)),
                                  mat4_euler_rotate_x(to_radians(30.0f)));
  Mat4 rigid_inv = mat4_inverse_rigid(rigid_transform);
  Mat4 rigid_identity = mat4_mul(rigid_transform, rigid_inv);
  assert(mat4_is_identity(rigid_identity, 0.001f) &&
         "Rigid body inverse failed");

  // Test affine inverse
  Mat4 affine_transform =
      mat4_mul(mat4_translate(vec3_new(2.0f, 3.0f, 4.0f)),
               mat4_mul(mat4_euler_rotate_z(to_radians(45.0f)),
                        mat4_scale(vec3_new(2.0f, 2.0f, 2.0f))));
  Mat4 affine_inv = mat4_inverse_affine(affine_transform);
  Mat4 affine_identity = mat4_mul(affine_transform, affine_inv);
  assert(mat4_is_identity(affine_identity, 0.01f) && "Affine inverse failed");

  // Test general inverse with simple identity matrix first
  Mat4 simple_test = mat4_identity();
  Mat4 simple_inv = mat4_inverse(simple_test);
  Mat4 simple_result = mat4_mul(simple_test, simple_inv);
  assert(mat4_is_identity(simple_result, 0.001f) &&
         "General inverse identity failed");

  // Test general inverse with the complex transform
  Mat4 general_inv = mat4_inverse(affine_transform);
  Mat4 general_identity = mat4_mul(affine_transform, general_inv);
  assert(mat4_is_identity(general_identity, 0.01f) && "General inverse failed");

  printf("  test_mat4_inverse_operations PASSED\n");
}

static void test_mat4_projection_matrices(void) {
  printf("  Running test_mat4_projection_matrices...\n");

  // Test orthographic projection
  Mat4 ortho = mat4_ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);

  // Test that corners of the view volume map correctly
  Vec4 corner1 = mat4_mul_vec4(ortho, vec4_new(-10.0f, -10.0f, -0.1f, 1.0f));
  Vec4 corner2 = mat4_mul_vec4(ortho, vec4_new(10.0f, 10.0f, -100.0f, 1.0f));

  // Should map to NDC space [-1, 1]
  assert(float_equals(corner1.x, -1.0f, 0.001f) && "Ortho left edge failed");
  assert(float_equals(corner1.y, -1.0f, 0.001f) && "Ortho bottom edge failed");
  assert(float_equals(corner2.x, 1.0f, 0.001f) && "Ortho right edge failed");
  assert(float_equals(corner2.y, 1.0f, 0.001f) && "Ortho top edge failed");

  // Test perspective projection
  Mat4 perspective =
      mat4_perspective(to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

  // Test that center point at near plane has w equal to original z coordinate
  Vec4 center_near =
      mat4_mul_vec4(perspective, vec4_new(0.0f, 0.0f, -0.1f, 1.0f));
  assert(float_equals(center_near.w, -0.1f, 0.001f) &&
         "Perspective near plane w failed");

  // Test look-at matrix
  Vec3 eye = vec3_new(0.0f, 0.0f, 5.0f);
  Vec3 center = vec3_new(0.0f, 0.0f, 0.0f);
  Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  Mat4 view = mat4_look_at(eye, center, up);

  // The eye position should transform to origin
  Vec4 eye_transformed = mat4_mul_vec4(view, vec3_to_vec4(eye, 1.0f));
  assert(vec3_equals(vec4_to_vec3(eye_transformed), vec3_zero(), 0.001f) &&
         "Look-at eye transform failed");

  printf("  test_mat4_projection_matrices PASSED\n");
}

static void test_mat4_quaternion_conversion(void) {
  printf("  Running test_mat4_quaternion_conversion...\n");

  // Test quaternion to matrix conversion
  Quat rotation_quat =
      quat_from_euler(to_radians(30.0f), to_radians(45.0f), to_radians(60.0f));
  Mat4 quat_matrix = quat_to_mat4(rotation_quat);

  // Matrix should be orthogonal (rotation preserves orthogonality)
  Mat4 quat_transpose = mat4_transpose(quat_matrix);
  Mat4 quat_identity = mat4_mul(quat_matrix, quat_transpose);
  assert(mat4_is_identity(quat_identity, 0.001f) &&
         "Quaternion matrix not orthogonal");

  // Test matrix to quaternion conversion
  Mat4 rotation_matrix = mat4_euler_rotate_y(to_radians(90.0f));
  Quat extracted_quat = mat4_to_quat(rotation_matrix);
  Mat4 reconstructed_matrix = quat_to_mat4(extracted_quat);
  assert(mat4_equals(rotation_matrix, reconstructed_matrix, 0.001f) &&
         "Matrix to quaternion conversion failed");

  // Test mat4_from_quat_pos
  Vec3 position = vec3_new(5.0f, 10.0f, 15.0f);
  Mat4 quat_pos_matrix = mat4_from_quat_pos(rotation_quat, position);

  // Check position is correctly set
  Vec3 extracted_pos = mat4_position(quat_pos_matrix);
  assert(vec3_equals(extracted_pos, position, 0.001f) &&
         "mat4_from_quat_pos position failed");

  // Check rotation part matches
  Mat4 rotation_part = quat_to_mat4(rotation_quat);
  rotation_part.m03 = quat_pos_matrix.m03;
  rotation_part.m13 = quat_pos_matrix.m13;
  rotation_part.m23 = quat_pos_matrix.m23;
  assert(mat4_equals(quat_pos_matrix, rotation_part, 0.001f) &&
         "mat4_from_quat_pos rotation failed");

  printf("  test_mat4_quaternion_conversion PASSED\n");
}

static void test_mat4_edge_cases(void) {
  printf("  Running test_mat4_edge_cases...\n");

  // Test zero matrix determinant
  Mat4 zero = mat4_zero();
  float zero_det = mat4_determinant(zero);
  assert(float_equals(zero_det, 0.0f, FLOAT_EPSILON) &&
         "Zero matrix determinant failed");

  // Test singular matrix inverse (should return identity)
  Mat4 singular = mat4_zero();
  singular.m33 = 1.0f; // Make it have some structure but still singular
  Mat4 singular_inv = mat4_inverse(singular);
  assert(mat4_is_identity(singular_inv, 0.001f) &&
         "Singular matrix inverse should be identity");

  // Test very small scaling (near-zero)
  Mat4 tiny_scale = mat4_scale(vec3_new(1e-10f, 1e-10f, 1e-10f));
  Mat4 tiny_inv = mat4_inverse(tiny_scale);
  // Should return identity for near-singular matrix
  assert(mat4_is_identity(tiny_inv, 0.001f) &&
         "Tiny scale inverse should be identity");

  // Test coordinate system consistency
  Mat4 transform = mat4_mul(mat4_translate(vec3_new(1.0f, 2.0f, 3.0f)),
                            mat4_euler_rotate_y(to_radians(90.0f)));

  Vec3 right = mat4_right(transform);
  Vec3 up = mat4_up(transform);
  Vec3 forward = mat4_forward(transform);

  // Test orthogonality
  assert(float_equals(vec3_dot(right, up), 0.0f, 0.001f) &&
         "Right-Up not orthogonal");
  assert(float_equals(vec3_dot(up, forward), 0.0f, 0.001f) &&
         "Up-Forward not orthogonal");
  assert(float_equals(vec3_dot(forward, right), 0.0f, 0.001f) &&
         "Forward-Right not orthogonal");

  // Test unit length
  assert(float_equals(vec3_length(right), 1.0f, 0.001f) &&
         "Right not unit length");
  assert(float_equals(vec3_length(up), 1.0f, 0.001f) && "Up not unit length");
  assert(float_equals(vec3_length(forward), 1.0f, 0.001f) &&
         "Forward not unit length");

  // Test right-handed coordinate system
  // In right-handed system: right × up = backward (or -forward)
  Vec3 cross_right_up = vec3_cross(right, up);
  Vec3 backward = mat4_backward(transform);
  assert(vec3_equals(cross_right_up, backward, 0.001f) &&
         "Right-handed rule failed");

  printf("  test_mat4_edge_cases PASSED\n");
}

// Test runner function declaration
bool32_t run_mat_tests(void) {
  printf("--- Starting Matrix Math Tests ---\n");

  // Matrix constructor tests
  test_mat4_constructors();
  test_mat4_rotation_constructors();

  // Matrix accessor tests
  test_mat4_accessors();

  // Matrix operation tests
  test_mat4_operations();
  test_mat4_vector_extraction();

  // Matrix inverse tests
  test_mat4_inverse_operations();

  // Projection matrix tests
  test_mat4_projection_matrices();

  // Quaternion conversion tests
  test_mat4_quaternion_conversion();

  // Edge case and validation tests
  test_mat4_edge_cases();

  printf("--- Matrix Math Tests Completed ---\n");
  return true;
}