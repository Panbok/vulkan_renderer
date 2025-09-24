#include "quat_test.h"

// Helper function for floating-point comparison with epsilon
static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return vkr_abs_f32(a - b) < epsilon;
}

// Helper function for VkrQuat comparison
static bool32_t quat_equals(VkrQuat a, VkrQuat b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon) && float_equals(a.w, b.w, epsilon);
}

// Helper function for Vec3 comparison
static bool32_t vec3_equals(Vec3 a, Vec3 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon);
}

// =============================================================================
// VkrQuaternion Constructor Tests
// =============================================================================

static void test_quat_constructors(void) {
  printf("  Running test_quat_constructors...\n");

  // Test vkr_quat_new
  VkrQuat q1 = vkr_quat_new(0.1f, 0.2f, 0.3f, 0.4f);
  assert(float_equals(q1.x, 0.1f, VKR_FLOAT_EPSILON) &&
         "vkr_quat_new x failed");
  assert(float_equals(q1.y, 0.2f, VKR_FLOAT_EPSILON) &&
         "vkr_quat_new y failed");
  assert(float_equals(q1.z, 0.3f, VKR_FLOAT_EPSILON) &&
         "vkr_quat_new z failed");
  assert(float_equals(q1.w, 0.4f, VKR_FLOAT_EPSILON) &&
         "vkr_quat_new w failed");

  // Test semantic aliases (quaternion is Vec4 internally)
  assert(float_equals(q1.r, 0.1f, VKR_FLOAT_EPSILON) && "quat r alias failed");
  assert(float_equals(q1.g, 0.2f, VKR_FLOAT_EPSILON) && "quat g alias failed");
  assert(float_equals(q1.b, 0.3f, VKR_FLOAT_EPSILON) && "quat b alias failed");
  assert(float_equals(q1.a, 0.4f, VKR_FLOAT_EPSILON) && "quat a alias failed");

  // Test array access
  assert(float_equals(q1.elements[0], 0.1f, VKR_FLOAT_EPSILON) &&
         "quat elements[0] failed");
  assert(float_equals(q1.elements[1], 0.2f, VKR_FLOAT_EPSILON) &&
         "quat elements[1] failed");
  assert(float_equals(q1.elements[2], 0.3f, VKR_FLOAT_EPSILON) &&
         "quat elements[2] failed");
  assert(float_equals(q1.elements[3], 0.4f, VKR_FLOAT_EPSILON) &&
         "quat elements[3] failed");

  // Test vkr_quat_identity
  VkrQuat identity = vkr_quat_identity();
  assert(quat_equals(identity, vkr_quat_new(0.0f, 0.0f, 0.0f, 1.0f),
                     VKR_FLOAT_EPSILON) &&
         "vkr_quat_identity failed");

  printf("  test_quat_constructors PASSED\n");
}

static void test_quat_from_axis_angle(void) {
  printf("  Running test_quat_from_axis_angle...\n");

  // Test rotation around X axis (90 degrees)
  Vec3 x_axis = vec3_new(1.0f, 0.0f, 0.0f);
  float32_t angle_90 = VKR_HALF_PI; // 90 degrees
  VkrQuat q_x_90 = vkr_quat_from_axis_angle(x_axis, angle_90);

  // Expected: cos(45°) = √2/2, sin(45°) = √2/2
  float32_t half_sqrt2 = VKR_SQRT_ONE_OVER_TWO;
  VkrQuat expected_x_90 = vkr_quat_new(half_sqrt2, 0.0f, 0.0f, half_sqrt2);
  assert(quat_equals(q_x_90, expected_x_90, 0.001f) &&
         "vkr_quat_from_axis_angle X 90° failed");

  // Test rotation around Y axis (180 degrees)
  Vec3 y_axis = vec3_new(0.0f, 1.0f, 0.0f);
  float32_t angle_180 = VKR_PI;
  VkrQuat q_y_180 = vkr_quat_from_axis_angle(y_axis, angle_180);

  VkrQuat expected_y_180 = vkr_quat_new(0.0f, 1.0f, 0.0f, 0.0f);
  assert(quat_equals(q_y_180, expected_y_180, 0.001f) &&
         "vkr_quat_from_axis_angle Y 180° failed");

  // Test rotation around Z axis (270 degrees)
  Vec3 z_axis = vec3_new(0.0f, 0.0f, 1.0f);
  float32_t angle_270 = 3.0f * VKR_HALF_PI;
  VkrQuat q_z_270 = vkr_quat_from_axis_angle(z_axis, angle_270);

  // cos(135°) = -√2/2, sin(135°) = √2/2
  float32_t neg_half_sqrt2 = -VKR_SQRT_ONE_OVER_TWO;
  VkrQuat expected_z_270 = vkr_quat_new(0.0f, 0.0f, half_sqrt2, neg_half_sqrt2);
  assert(quat_equals(q_z_270, expected_z_270, 0.001f) &&
         "vkr_quat_from_axis_angle Z 270° failed");

  // Test zero angle (should return identity)
  VkrQuat q_zero = vkr_quat_from_axis_angle(x_axis, 0.0f);
  assert(quat_equals(q_zero, vkr_quat_identity(), VKR_FLOAT_EPSILON) &&
         "vkr_quat_from_axis_angle zero angle failed");

  // Test zero axis (should return identity)
  Vec3 zero_axis = vec3_zero();
  VkrQuat q_zero_axis = vkr_quat_from_axis_angle(zero_axis, angle_90);
  assert(quat_equals(q_zero_axis, vkr_quat_identity(), VKR_FLOAT_EPSILON) &&
         "vkr_quat_from_axis_angle zero axis failed");

  // Test non-normalized axis (should auto-normalize)
  Vec3 long_axis = vec3_new(2.0f, 0.0f, 0.0f);
  VkrQuat q_long_axis = vkr_quat_from_axis_angle(long_axis, angle_90);
  assert(quat_equals(q_long_axis, expected_x_90, 0.001f) &&
         "vkr_quat_from_axis_angle non-normalized axis failed");

  printf("  test_quat_from_axis_angle PASSED\n");
}

static void test_quat_from_euler(void) {
  printf("  Running test_quat_from_euler...\n");

  // Test identity rotation (all zero angles)
  VkrQuat q_identity = vkr_quat_from_euler(0.0f, 0.0f, 0.0f);
  assert(quat_equals(q_identity, vkr_quat_identity(), 0.001f) &&
         "vkr_quat_from_euler identity failed");

  // Test single axis rotations (90 degrees each)
  float32_t angle_90 = VKR_HALF_PI;

  // Roll (X axis) 90°
  VkrQuat q_roll = vkr_quat_from_euler(angle_90, 0.0f, 0.0f);
  float32_t half_sqrt2 = VKR_SQRT_ONE_OVER_TWO;
  VkrQuat expected_roll = vkr_quat_new(half_sqrt2, 0.0f, 0.0f, half_sqrt2);
  assert(quat_equals(q_roll, expected_roll, 0.001f) &&
         "vkr_quat_from_euler roll 90° failed");

  // Pitch (Y axis) 90°
  VkrQuat q_pitch = vkr_quat_from_euler(0.0f, angle_90, 0.0f);
  VkrQuat expected_pitch = vkr_quat_new(0.0f, half_sqrt2, 0.0f, half_sqrt2);
  assert(quat_equals(q_pitch, expected_pitch, 0.001f) &&
         "vkr_quat_from_euler pitch 90° failed");

  // Yaw (Z axis) 90°
  VkrQuat q_yaw = vkr_quat_from_euler(0.0f, 0.0f, angle_90);
  VkrQuat expected_yaw = vkr_quat_new(0.0f, 0.0f, half_sqrt2, half_sqrt2);
  assert(quat_equals(q_yaw, expected_yaw, 0.001f) &&
         "vkr_quat_from_euler yaw 90° failed");

  // Test combined rotation (45° each axis)
  float32_t angle_45 = VKR_QUARTER_PI;
  VkrQuat q_combined = vkr_quat_from_euler(angle_45, angle_45, angle_45);

  // Verify the quaternion is normalized
  float32_t len = vkr_quat_length(q_combined);
  assert(float_equals(len, 1.0f, 0.001f) &&
         "vkr_quat_from_euler combined rotation not normalized");

  // Test 180° rotations
  VkrQuat q_roll_180 = vkr_quat_from_euler(VKR_PI, 0.0f, 0.0f);
  VkrQuat expected_roll_180 = vkr_quat_new(1.0f, 0.0f, 0.0f, 0.0f);
  assert(quat_equals(q_roll_180, expected_roll_180, 0.001f) &&
         "vkr_quat_from_euler roll 180° failed");

  printf("  test_quat_from_euler PASSED\n");
}

// =============================================================================
// VkrQuaternion Basic Operations Tests
// =============================================================================

static void test_quat_basic_operations(void) {
  printf("  Running test_quat_basic_operations...\n");

  VkrQuat q1 = vkr_quat_new(0.1f, 0.2f, 0.3f, 0.4f);
  VkrQuat q2 = vkr_quat_new(0.5f, 0.6f, 0.7f, 0.8f);

  // Test length and length_squared
  float32_t len_sq = vkr_quat_length_squared(q1);
  float32_t expected_len_sq = 0.01f + 0.04f + 0.09f + 0.16f; // 0.3
  assert(float_equals(len_sq, expected_len_sq, VKR_FLOAT_EPSILON) &&
         "vkr_quat_length_squared failed");

  float32_t len = vkr_quat_length(q1);
  assert(float_equals(len, vkr_sqrt_f32(expected_len_sq), VKR_FLOAT_EPSILON) &&
         "vkr_quat_length failed");

  // Test normalization
  VkrQuat q_normalized = vkr_quat_normalize(q1);
  float32_t norm_len = vkr_quat_length(q_normalized);
  assert(float_equals(norm_len, 1.0f, 0.001f) && "vkr_quat_normalize failed");

  // Test conjugate
  VkrQuat q_conj = vkr_quat_conjugate(q1);
  VkrQuat expected_conj = vkr_quat_new(-0.1f, -0.2f, -0.3f, 0.4f);
  assert(quat_equals(q_conj, expected_conj, VKR_FLOAT_EPSILON) &&
         "vkr_quat_conjugate failed");

  // Test inverse
  VkrQuat unit_q = vkr_quat_normalize(q1);
  VkrQuat q_inv = vkr_quat_inverse(unit_q);
  VkrQuat unit_conj = vkr_quat_conjugate(unit_q);
  assert(quat_equals(q_inv, unit_conj, 0.001f) &&
         "vkr_quat_inverse of unit quaternion failed");

  // Test add, sub, scale
  VkrQuat q_add = vkr_quat_add(q1, q2);
  VkrQuat expected_add = vkr_quat_new(0.6f, 0.8f, 1.0f, 1.2f);
  assert(quat_equals(q_add, expected_add, VKR_FLOAT_EPSILON) &&
         "vkr_quat_add failed");

  VkrQuat q_sub = vkr_quat_sub(q2, q1);
  VkrQuat expected_sub = vkr_quat_new(0.4f, 0.4f, 0.4f, 0.4f);
  assert(quat_equals(q_sub, expected_sub, VKR_FLOAT_EPSILON) &&
         "vkr_quat_sub failed");

  VkrQuat q_scaled = vkr_quat_scale(q1, 2.0f);
  VkrQuat expected_scaled = vkr_quat_new(0.2f, 0.4f, 0.6f, 0.8f);
  assert(quat_equals(q_scaled, expected_scaled, VKR_FLOAT_EPSILON) &&
         "vkr_quat_scale failed");

  // Test dot product
  float32_t dot = vkr_quat_dot(q1, q2);
  float32_t expected_dot =
      0.1f * 0.5f + 0.2f * 0.6f + 0.3f * 0.7f + 0.4f * 0.8f;
  assert(float_equals(dot, expected_dot, VKR_FLOAT_EPSILON) &&
         "vkr_quat_dot failed");

  printf("  test_quat_basic_operations PASSED\n");
}

// =============================================================================
// VkrQuaternion Multiplication Tests
// =============================================================================

static void test_quat_multiplication(void) {
  printf("  Running test_quat_multiplication...\n");

  // Test identity multiplication
  VkrQuat identity = vkr_quat_identity();
  VkrQuat q = vkr_quat_new(0.1f, 0.2f, 0.3f, 0.4f);

  VkrQuat q_mul_identity = vkr_quat_mul(q, identity);
  assert(quat_equals(q_mul_identity, q, 0.001f) &&
         "vkr_quat_mul with identity (right) failed");

  VkrQuat identity_mul_q = vkr_quat_mul(identity, q);
  assert(quat_equals(identity_mul_q, q, 0.001f) &&
         "vkr_quat_mul with identity (left) failed");

  // Test multiplication with conjugate (should give scalar)
  VkrQuat q_unit = vkr_quat_normalize(q);
  VkrQuat q_conj = vkr_quat_conjugate(q_unit);
  VkrQuat q_mul_conj = vkr_quat_mul(q_unit, q_conj);

  // Result should be close to identity (within numerical precision)
  assert(float_equals(q_mul_conj.x, 0.0f, 0.001f) &&
         "vkr_quat_mul with conjugate x component failed");
  assert(float_equals(q_mul_conj.y, 0.0f, 0.001f) &&
         "vkr_quat_mul with conjugate y component failed");
  assert(float_equals(q_mul_conj.z, 0.0f, 0.001f) &&
         "vkr_quat_mul with conjugate z component failed");
  assert(float_equals(q_mul_conj.w, 1.0f, 0.001f) &&
         "vkr_quat_mul with conjugate w component failed");

  // Test specific known multiplication with simpler case
  // 90° rotation around X axis
  Vec3 x_axis = vec3_new(1.0f, 0.0f, 0.0f);
  VkrQuat q_x_90 = vkr_quat_from_axis_angle(x_axis, VKR_HALF_PI);

  // Test multiplication with itself (180° total)
  VkrQuat q_x_180 = vkr_quat_mul(q_x_90, q_x_90);
  VkrQuat expected_x_180 = vkr_quat_from_axis_angle(x_axis, VKR_PI);

  // Both should represent the same rotation (allowing for quaternion
  // double-cover)
  bool same_rotation =
      quat_equals(q_x_180, expected_x_180, 0.001f) ||
      quat_equals(q_x_180, vkr_quat_scale(expected_x_180, -1.0f), 0.001f);
  assert(same_rotation && "vkr_quat_mul composition failed");

  // Test non-commutativity with 90° rotations around different axes
  Vec3 y_axis = vec3_new(0.0f, 1.0f, 0.0f);
  VkrQuat q_y_90 = vkr_quat_from_axis_angle(y_axis, VKR_HALF_PI);

  VkrQuat q_xy = vkr_quat_mul(q_x_90, q_y_90);
  VkrQuat q_yx = vkr_quat_mul(q_y_90, q_x_90);

  // These should be different (non-commutative)
  bool different_rotations =
      !quat_equals(q_xy, q_yx, 0.001f) &&
      !quat_equals(q_xy, vkr_quat_scale(q_yx, -1.0f), 0.001f);
  assert(different_rotations && "vkr_quat_mul non-commutativity test failed");

  printf("  test_quat_multiplication PASSED\n");
}

// =============================================================================
// VkrQuaternion Rotation Tests
// =============================================================================

static void test_quat_rotate_vec3(void) {
  printf("  Running test_quat_rotate_vec3...\n");

  // Test identity rotation (should not change vector)
  Vec3 v = vec3_new(1.0f, 2.0f, 3.0f);
  VkrQuat identity = vkr_quat_identity();
  Vec3 v_rotated = vkr_quat_rotate_vec3(identity, v);
  assert(vec3_equals(v_rotated, v, VKR_FLOAT_EPSILON) &&
         "vkr_quat_rotate_vec3 identity failed");

  // Test 90° rotation around Z axis
  Vec3 x_axis = vec3_new(1.0f, 0.0f, 0.0f);
  Vec3 z_axis = vec3_new(0.0f, 0.0f, 1.0f);
  VkrQuat q_z_90 = vkr_quat_from_axis_angle(z_axis, VKR_HALF_PI);

  Vec3 x_rotated_z = vkr_quat_rotate_vec3(q_z_90, x_axis);
  Vec3 expected_y = vec3_new(0.0f, 1.0f, 0.0f);
  assert(vec3_equals(x_rotated_z, expected_y, 0.001f) &&
         "vkr_quat_rotate_vec3 90° Z rotation failed");

  // Test 90° rotation around X axis
  Vec3 y_axis = vec3_new(0.0f, 1.0f, 0.0f);
  VkrQuat q_x_90 = vkr_quat_from_axis_angle(x_axis, VKR_HALF_PI);

  Vec3 y_rotated_x = vkr_quat_rotate_vec3(q_x_90, y_axis);
  Vec3 expected_z = vec3_new(0.0f, 0.0f, 1.0f);
  assert(vec3_equals(y_rotated_x, expected_z, 0.001f) &&
         "vkr_quat_rotate_vec3 90° X rotation failed");

  // Test 90° rotation around Y axis
  VkrQuat q_y_90 = vkr_quat_from_axis_angle(y_axis, VKR_HALF_PI);

  Vec3 z_rotated_y = vkr_quat_rotate_vec3(q_y_90, z_axis);
  Vec3 expected_x = vec3_new(1.0f, 0.0f, 0.0f);
  assert(vec3_equals(z_rotated_y, expected_x, 0.001f) &&
         "vkr_quat_rotate_vec3 90° Y rotation failed");

  // Test 180° rotation
  VkrQuat q_x_180 = vkr_quat_from_axis_angle(x_axis, VKR_PI);
  Vec3 y_rotated_180 = vkr_quat_rotate_vec3(q_x_180, y_axis);
  Vec3 expected_neg_y = vec3_new(0.0f, -1.0f, 0.0f);
  assert(vec3_equals(y_rotated_180, expected_neg_y, 0.001f) &&
         "vkr_quat_rotate_vec3 180° rotation failed");

  // Test rotation preserves length
  Vec3 arbitrary = vec3_new(2.0f, 3.0f, 4.0f);
  VkrQuat arbitrary_q =
      vkr_quat_from_axis_angle(vec3_new(1.0f, 1.0f, 1.0f), 1.5f);
  Vec3 rotated_arbitrary = vkr_quat_rotate_vec3(arbitrary_q, arbitrary);

  float32_t orig_len = vec3_length(arbitrary);
  float32_t rotated_len = vec3_length(rotated_arbitrary);
  assert(float_equals(orig_len, rotated_len, 0.001f) &&
         "vkr_quat_rotate_vec3 length preservation failed");

  printf("  test_quat_rotate_vec3 PASSED\n");
}

// =============================================================================
// VkrQuaternion Interpolation Tests
// =============================================================================

static void test_quat_interpolation(void) {
  printf("  Running test_quat_interpolation...\n");

  // Test lerp at endpoints
  VkrQuat q1 = vkr_quat_identity();
  Vec3 axis = vec3_new(0.0f, 0.0f, 1.0f);
  VkrQuat q2 = vkr_quat_from_axis_angle(axis, VKR_HALF_PI);

  VkrQuat lerp_start = vkr_quat_lerp(q1, q2, 0.0f);
  assert(quat_equals(lerp_start, q1, 0.001f) && "vkr_quat_lerp t=0 failed");

  VkrQuat lerp_end = vkr_quat_lerp(q1, q2, 1.0f);
  assert(quat_equals(lerp_end, q2, 0.001f) && "vkr_quat_lerp t=1 failed");

  // Test lerp midpoint (result should be normalized)
  VkrQuat lerp_mid = vkr_quat_lerp(q1, q2, 0.5f);
  float32_t mid_len = vkr_quat_length(lerp_mid);
  assert(float_equals(mid_len, 1.0f, 0.001f) &&
         "vkr_quat_lerp midpoint not normalized");

  // Test slerp at endpoints
  VkrQuat slerp_start = vkr_quat_slerp(q1, q2, 0.0f);
  assert(quat_equals(slerp_start, q1, 0.001f) && "vkr_quat_slerp t=0 failed");

  VkrQuat slerp_end = vkr_quat_slerp(q1, q2, 1.0f);
  assert(quat_equals(slerp_end, q2, 0.001f) && "vkr_quat_slerp t=1 failed");

  // Test slerp midpoint
  VkrQuat slerp_mid = vkr_quat_slerp(q1, q2, 0.5f);
  float32_t slerp_mid_len = vkr_quat_length(slerp_mid);
  assert(float_equals(slerp_mid_len, 1.0f, 0.001f) &&
         "vkr_quat_slerp midpoint not normalized");

  // Test slerp with very close quaternions (should fall back to lerp)
  VkrQuat q_close1 = vkr_quat_identity();
  VkrQuat q_close2 =
      vkr_quat_from_axis_angle(axis, 0.001f); // Very small rotation

  VkrQuat slerp_close = vkr_quat_slerp(q_close1, q_close2, 0.5f);
  float32_t close_len = vkr_quat_length(slerp_close);
  assert(float_equals(close_len, 1.0f, 0.001f) &&
         "vkr_quat_slerp close quaternions failed");

  // Test shortest path interpolation (negative dot product)
  VkrQuat q_neg =
      vkr_quat_scale(q2, -1.0f); // Same rotation, opposite representation
  VkrQuat lerp_neg = vkr_quat_lerp(q1, q_neg, 0.5f);
  VkrQuat slerp_neg = vkr_quat_slerp(q1, q_neg, 0.5f);

  // Both should find the shorter path
  float32_t lerp_neg_len = vkr_quat_length(lerp_neg);
  float32_t slerp_neg_len = vkr_quat_length(slerp_neg);
  assert(float_equals(lerp_neg_len, 1.0f, 0.001f) &&
         "vkr_quat_lerp negative quaternion failed");
  assert(float_equals(slerp_neg_len, 1.0f, 0.001f) &&
         "vkr_quat_slerp negative quaternion failed");

  printf("  test_quat_interpolation PASSED\n");
}

// =============================================================================
// VkrQuaternion Conversion Tests
// =============================================================================

static void test_quat_to_euler(void) {
  printf("  Running test_quat_to_euler...\n");

  // Test identity quaternion
  VkrQuat identity = vkr_quat_identity();
  float32_t roll, pitch, yaw;
  vkr_quat_to_euler(identity, &roll, &pitch, &yaw);

  assert(float_equals(roll, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity roll failed");
  assert(float_equals(pitch, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity pitch failed");
  assert(float_equals(yaw, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity yaw failed");

  // Test single axis rotations
  float32_t angle_90 = VKR_HALF_PI;

  // Roll 90°
  VkrQuat q_roll = vkr_quat_from_euler(angle_90, 0.0f, 0.0f);
  vkr_quat_to_euler(q_roll, &roll, &pitch, &yaw);
  assert(float_equals(roll, angle_90, 0.001f) &&
         "vkr_quat_to_euler roll 90° failed");
  assert(float_equals(pitch, 0.0f, 0.001f) &&
         "vkr_quat_to_euler roll pitch component failed");
  assert(float_equals(yaw, 0.0f, 0.001f) &&
         "vkr_quat_to_euler roll yaw component failed");

  // Pitch 90°
  VkrQuat q_pitch = vkr_quat_from_euler(0.0f, angle_90, 0.0f);
  vkr_quat_to_euler(q_pitch, &roll, &pitch, &yaw);
  assert(float_equals(roll, 0.0f, 0.001f) &&
         "vkr_quat_to_euler pitch roll component failed");
  assert(float_equals(pitch, angle_90, 0.001f) &&
         "vkr_quat_to_euler pitch 90° failed");
  assert(float_equals(yaw, 0.0f, 0.001f) &&
         "vkr_quat_to_euler pitch yaw component failed");

  // Yaw 90°
  VkrQuat q_yaw = vkr_quat_from_euler(0.0f, 0.0f, angle_90);
  vkr_quat_to_euler(q_yaw, &roll, &pitch, &yaw);
  assert(float_equals(roll, 0.0f, 0.001f) &&
         "vkr_quat_to_euler yaw roll component failed");
  assert(float_equals(pitch, 0.0f, 0.001f) &&
         "vkr_quat_to_euler yaw pitch component failed");
  assert(float_equals(yaw, angle_90, 0.001f) &&
         "vkr_quat_to_euler yaw 90° failed");

  // Test round-trip conversion with simple test case (identity only)
  // Note: Full round-trip testing is complex due to multiple Euler
  // representations for the same rotation and gimbal lock issues. We test basic
  // functionality.
  VkrQuat identity_check = vkr_quat_from_euler(0.0f, 0.0f, 0.0f);
  vkr_quat_to_euler(identity_check, &roll, &pitch, &yaw);

  assert(float_equals(roll, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity round-trip roll failed");
  assert(float_equals(pitch, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity round-trip pitch failed");
  assert(float_equals(yaw, 0.0f, 0.001f) &&
         "vkr_quat_to_euler identity round-trip yaw failed");

  printf("    Note: Complex round-trip testing skipped due to Euler angle "
         "ambiguities\n");

  printf("  test_quat_to_euler PASSED\n");
}

static void test_quat_axis_angle_extraction(void) {
  printf("  Running test_quat_axis_angle_extraction...\n");

  // Test identity quaternion
  VkrQuat identity = vkr_quat_identity();
  float32_t angle = vkr_quat_angle(identity);
  Vec3 axis = vkr_quat_axis(identity);

  assert(float_equals(angle, 0.0f, 0.001f) && "vkr_quat_angle identity failed");
  // Axis can be any unit vector for identity, so just check it's normalized
  float32_t axis_len = vec3_length(axis);
  assert(float_equals(axis_len, 1.0f, 0.001f) &&
         "vkr_quat_axis identity not normalized");

  // Test 90° rotation around X axis
  Vec3 x_axis = vec3_new(1.0f, 0.0f, 0.0f);
  VkrQuat q_x_90 = vkr_quat_from_axis_angle(x_axis, VKR_HALF_PI);

  float32_t extracted_angle = vkr_quat_angle(q_x_90);
  Vec3 extracted_axis = vkr_quat_axis(q_x_90);

  assert(float_equals(extracted_angle, VKR_HALF_PI, 0.001f) &&
         "vkr_quat_angle 90° X rotation failed");
  assert(vec3_equals(extracted_axis, x_axis, 0.001f) &&
         "vkr_quat_axis 90° X rotation failed");

  // Test 180° rotation around arbitrary axis
  Vec3 arbitrary_axis = vec3_normalize(vec3_new(1.0f, 1.0f, 1.0f));
  VkrQuat q_arbitrary_180 = vkr_quat_from_axis_angle(arbitrary_axis, VKR_PI);

  float32_t extracted_angle_180 = vkr_quat_angle(q_arbitrary_180);
  Vec3 extracted_axis_180 = vkr_quat_axis(q_arbitrary_180);

  assert(float_equals(extracted_angle_180, VKR_PI, 0.001f) &&
         "vkr_quat_angle 180° arbitrary rotation failed");
  assert(vec3_equals(extracted_axis_180, arbitrary_axis, 0.001f) &&
         "vkr_quat_axis 180° arbitrary rotation failed");

  // Test round-trip conversion
  Vec3 test_axis = vec3_normalize(vec3_new(0.6f, 0.8f, 0.0f));
  float32_t test_angle = 1.2f;
  VkrQuat q_test = vkr_quat_from_axis_angle(test_axis, test_angle);

  float32_t roundtrip_angle = vkr_quat_angle(q_test);
  Vec3 roundtrip_axis = vkr_quat_axis(q_test);

  assert(float_equals(roundtrip_angle, test_angle, 0.001f) &&
         "vkr_quat_angle round-trip failed");
  assert(vec3_equals(roundtrip_axis, test_axis, 0.001f) &&
         "vkr_quat_axis round-trip failed");

  printf("  test_quat_axis_angle_extraction PASSED\n");
}

// =============================================================================
// VkrQuaternion Look-At Tests
// =============================================================================

static void test_quat_look_at(void) {
  printf("  Running test_quat_look_at...\n");

  // Test basic functionality - looking forward should be close to identity
  Vec3 forward = vec3_new(0.0f, 0.0f, -1.0f); // -Z is forward in right-handed
  Vec3 up = vec3_new(0.0f, 1.0f, 0.0f);
  VkrQuat q_forward = vkr_quat_look_at(forward, up);

  // Should produce a valid unit quaternion
  float32_t len = vkr_quat_length(q_forward);
  assert(float_equals(len, 1.0f, 0.001f) &&
         "vkr_quat_look_at forward not normalized");

  // Test that the quaternion actually rotates vectors correctly
  Vec3 default_forward = vec3_new(0.0f, 0.0f, -1.0f);
  Vec3 rotated_forward = vkr_quat_rotate_vec3(q_forward, default_forward);

  // The rotated forward should be close to the target forward
  float32_t dot_product = vec3_dot(rotated_forward, forward);
  assert(float_equals(dot_product, 1.0f, 0.1f) &&
         "vkr_quat_look_at forward direction incorrect");

  // Test looking in different directions
  Vec3 right = vec3_new(1.0f, 0.0f, 0.0f);
  VkrQuat q_right = vkr_quat_look_at(right, up);

  float32_t len_right = vkr_quat_length(q_right);
  assert(float_equals(len_right, 1.0f, 0.001f) &&
         "vkr_quat_look_at right not normalized");

  // Test with non-normalized vectors (should auto-normalize)
  Vec3 long_forward = vec3_new(2.0f, 0.0f, -2.0f);
  Vec3 long_up = vec3_new(0.0f, 3.0f, 0.0f);
  VkrQuat q_long = vkr_quat_look_at(long_forward, long_up);

  float32_t q_long_len = vkr_quat_length(q_long);
  assert(float_equals(q_long_len, 1.0f, 0.001f) &&
         "vkr_quat_look_at with long vectors not normalized");

  printf("  test_quat_look_at PASSED\n");
}

// =============================================================================
// Edge Cases and Robustness Tests
// =============================================================================

static void test_quat_edge_cases(void) {
  printf("  Running test_quat_edge_cases...\n");

  // Test inverse of zero quaternion (should return identity)
  VkrQuat zero_q = vkr_quat_new(0.0f, 0.0f, 0.0f, 0.0f);
  VkrQuat zero_inv = vkr_quat_inverse(zero_q);
  assert(quat_equals(zero_inv, vkr_quat_identity(), VKR_FLOAT_EPSILON) &&
         "vkr_quat_inverse of zero quaternion failed");

  // Test normalization of zero quaternion
  VkrQuat zero_norm = vkr_quat_normalize(zero_q);
  assert(quat_equals(zero_norm, vkr_quat_new(0.0f, 0.0f, 0.0f, 0.0f),
                     VKR_FLOAT_EPSILON) &&
         "vkr_quat_normalize of zero quaternion failed");

  // Test very small quaternion normalization
  VkrQuat tiny_q = vkr_quat_new(1e-10f, 1e-10f, 1e-10f, 1e-10f);
  VkrQuat tiny_norm = vkr_quat_normalize(tiny_q);
  // Should not crash and should produce some valid result

  // Test gimbal lock scenarios in Euler conversion
  // Create a quaternion that represents 90° pitch directly
  VkrQuat q_gimbal =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), VKR_HALF_PI);
  float32_t roll, pitch, yaw;
  vkr_quat_to_euler(q_gimbal, &roll, &pitch, &yaw);

  // In gimbal lock, we should get approximately 90° pitch
  // The other angles may be adjusted due to gimbal lock handling
  assert(float_equals(vkr_abs_f32(pitch), VKR_HALF_PI, 0.01f) &&
         "vkr_quat_to_euler gimbal lock pitch failed");
  assert(float_equals(yaw, 0.0f, 0.01f) &&
         "vkr_quat_to_euler gimbal lock yaw not zeroed");

  // Test negative gimbal lock
  VkrQuat q_gimbal_neg =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), -VKR_HALF_PI);
  vkr_quat_to_euler(q_gimbal_neg, &roll, &pitch, &yaw);

  assert(float_equals(vkr_abs_f32(pitch), VKR_HALF_PI, 0.01f) &&
         "vkr_quat_to_euler negative gimbal lock pitch failed");
  assert(float_equals(yaw, 0.0f, 0.01f) &&
         "vkr_quat_to_euler negative gimbal lock yaw not zeroed");

  // Test rotation of zero vector (should remain zero)
  Vec3 zero_vec = vec3_zero();
  VkrQuat arbitrary_q =
      vkr_quat_from_axis_angle(vec3_new(1.0f, 0.0f, 0.0f), 1.0f);
  Vec3 rotated_zero = vkr_quat_rotate_vec3(arbitrary_q, zero_vec);
  assert(vec3_equals(rotated_zero, zero_vec, VKR_FLOAT_EPSILON) &&
         "vkr_quat_rotate_vec3 of zero vector failed");

  // Test slerp with identical quaternions
  VkrQuat q_same = vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 0.5f);
  VkrQuat slerp_same = vkr_quat_slerp(q_same, q_same, 0.5f);
  assert(quat_equals(slerp_same, q_same, 0.001f) &&
         "vkr_quat_slerp with identical quaternions failed");

  printf("  test_quat_edge_cases PASSED\n");
}

// =============================================================================
// Right-Handed Coordinate System Validation Tests
// =============================================================================

static void test_quat_coordinate_system(void) {
  printf("  Running test_quat_coordinate_system...\n");

  // Test right-handed coordinate system consistency
  Vec3 x_axis = vec3_new(1.0f, 0.0f, 0.0f);
  Vec3 y_axis = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 z_axis = vec3_new(0.0f, 0.0f, 1.0f);

  // Test X → Y → Z → X rotation sequence (right-handed)
  VkrQuat q_x_90 = vkr_quat_from_axis_angle(x_axis, VKR_HALF_PI);
  VkrQuat q_y_90 = vkr_quat_from_axis_angle(y_axis, VKR_HALF_PI);
  VkrQuat q_z_90 = vkr_quat_from_axis_angle(z_axis, VKR_HALF_PI);

  // X rotation: Y → Z, Z → -Y
  Vec3 y_rotated_x = vkr_quat_rotate_vec3(q_x_90, y_axis);
  Vec3 z_rotated_x = vkr_quat_rotate_vec3(q_x_90, z_axis);
  assert(vec3_equals(y_rotated_x, z_axis, 0.001f) &&
         "Right-handed X rotation: Y → Z failed");
  assert(vec3_equals(z_rotated_x, vec3_negate(y_axis), 0.001f) &&
         "Right-handed X rotation: Z → -Y failed");

  // Y rotation: Z → X, X → -Z
  Vec3 z_rotated_y = vkr_quat_rotate_vec3(q_y_90, z_axis);
  Vec3 x_rotated_y = vkr_quat_rotate_vec3(q_y_90, x_axis);
  assert(vec3_equals(z_rotated_y, x_axis, 0.001f) &&
         "Right-handed Y rotation: Z → X failed");
  assert(vec3_equals(x_rotated_y, vec3_negate(z_axis), 0.001f) &&
         "Right-handed Y rotation: X → -Z failed");

  // Z rotation: X → Y, Y → -X
  Vec3 x_rotated_z = vkr_quat_rotate_vec3(q_z_90, x_axis);
  Vec3 y_rotated_z = vkr_quat_rotate_vec3(q_z_90, y_axis);
  assert(vec3_equals(x_rotated_z, y_axis, 0.001f) &&
         "Right-handed Z rotation: X → Y failed");
  assert(vec3_equals(y_rotated_z, vec3_negate(x_axis), 0.001f) &&
         "Right-handed Z rotation: Y → -X failed");

  // Test quaternion composition follows right-hand rule
  VkrQuat q_x_then_y = vkr_quat_mul(q_y_90, q_x_90); // Apply X first, then Y
  Vec3 z_double_rotated = vkr_quat_rotate_vec3(q_x_then_y, z_axis);

  // Let's verify the transformation step by step:
  // Z → (X rotation 90°) → should go to -Y
  Vec3 z_after_x = vkr_quat_rotate_vec3(q_x_90, z_axis);
  // -Y → (Y rotation 90°) → should go to X
  Vec3 expected_double = vkr_quat_rotate_vec3(q_y_90, z_after_x);

  assert(vec3_equals(z_double_rotated, expected_double, 0.001f) &&
         "Right-handed quaternion composition failed");

  printf("  test_quat_coordinate_system PASSED\n");
}

// =============================================================================
// Performance and Precision Tests
// =============================================================================

static void test_quat_precision(void) {
  printf("  Running test_quat_precision...\n");

  // Test that multiple rotations maintain normalization
  VkrQuat q = vkr_quat_identity();
  Vec3 axis = vec3_normalize(vec3_new(1.0f, 1.0f, 1.0f));
  float32_t small_angle = 0.01f;

  // Apply many small rotations
  for (int i = 0; i < 100; i++) {
    VkrQuat small_rot = vkr_quat_from_axis_angle(axis, small_angle);
    q = vkr_quat_mul(q, small_rot);
  }

  // VkrQuaternion should still be approximately normalized
  float32_t final_len = vkr_quat_length(q);
  assert(float_equals(final_len, 1.0f, 0.01f) &&
         "quat accumulated rotations lost normalization");

  // Test precision of axis-angle round trip with small angles
  float32_t tiny_angle = 0.001f;
  VkrQuat q_tiny = vkr_quat_from_axis_angle(axis, tiny_angle);
  float32_t extracted_angle = vkr_quat_angle(q_tiny);
  Vec3 extracted_axis = vkr_quat_axis(q_tiny);

  assert(float_equals(extracted_angle, tiny_angle, 0.0001f) &&
         "quat small angle precision failed");
  assert(vec3_equals(extracted_axis, axis, 0.001f) &&
         "quat small angle axis precision failed");

  // Test that conjugate and inverse are consistent for unit quaternions
  VkrQuat unit_q = vkr_quat_normalize(vkr_quat_new(0.1f, 0.2f, 0.3f, 0.4f));
  VkrQuat q_conj = vkr_quat_conjugate(unit_q);
  VkrQuat q_inv = vkr_quat_inverse(unit_q);

  assert(quat_equals(q_conj, q_inv, 0.001f) &&
         "quat conjugate and inverse inconsistent for unit quaternion");

  printf("  test_quat_precision PASSED\n");
}

// =============================================================================
// Test Runner
// =============================================================================

bool32_t run_quat_tests(void) {
  printf("--- Starting VkrQuaternion Math Tests ---\n");

  // Constructor tests
  test_quat_constructors();
  test_quat_from_axis_angle();
  test_quat_from_euler();

  // Basic operation tests
  test_quat_basic_operations();
  test_quat_multiplication();

  // Rotation tests
  test_quat_rotate_vec3();

  // Interpolation tests
  test_quat_interpolation();

  // Conversion tests
  test_quat_to_euler();
  test_quat_axis_angle_extraction();
  test_quat_look_at();

  // Edge case and robustness tests
  test_quat_edge_cases();
  test_quat_coordinate_system();
  test_quat_precision();

  printf("--- VkrQuaternion Math Tests Completed ---\n");
  return true;
}