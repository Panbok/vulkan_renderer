#include "transform_test.h"

static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return vkr_abs_f32(a - b) < epsilon;
}

static bool32_t vec3_equals(Vec3 a, Vec3 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon);
}

static bool32_t quat_equals(VkrQuat a, VkrQuat b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon) && float_equals(a.w, b.w, epsilon);
}

static bool32_t mat4_equals(Mat4 a, Mat4 b, float32_t epsilon) {
  for (int i = 0; i < 16; ++i) {
    if (!float_equals(a.elements[i], b.elements[i], epsilon)) {
      return false;
    }
  }
  return true;
}

static void test_transform_new_initialization(void) {
  printf("  Running test_transform_new_initialization...\n");

  Vec3 position = vec3_new(1.0f, -2.0f, 3.0f);
  VkrQuat non_unit_rotation = vkr_quat_new(0.0f, 0.0f, 0.0f, 2.0f);
  Vec3 scale = vec3_new(2.0f, 3.0f, 4.0f);

  VkrTransform transform =
      vkr_transform_new(position, non_unit_rotation, scale);

  VkrQuat expected_rotation = vkr_quat_normalize(non_unit_rotation);

  assert(vec3_equals(transform.position, position, VKR_FLOAT_EPSILON) &&
         "transform position not preserved");
  assert(quat_equals(transform.rotation, expected_rotation, 0.0001f) &&
         "transform rotation not normalized");
  assert(vec3_equals(transform.scale, scale, VKR_FLOAT_EPSILON) &&
         "transform scale not preserved");
  assert(transform.is_dirty == true && "transform should start dirty");
  assert(mat4_equals(transform.local, mat4_identity(), VKR_FLOAT_EPSILON) &&
         "local matrix should start as identity");
  assert(transform.parent == NULL && "new transform should not have parent");

  printf("  test_transform_new_initialization PASSED\n");
}

static void test_transform_local_matrix_and_dirty_flag(void) {
  printf("  Running test_transform_local_matrix_and_dirty_flag...\n");

  VkrTransform transform = vkr_transform_identity();

  Mat4 initial = vkr_transform_get_local(&transform);
  assert(mat4_equals(initial, mat4_identity(), VKR_FLOAT_EPSILON) &&
         "identity transform local matrix mismatch");
  assert(transform.is_dirty == false && "identity fetch should clear dirty");

  Vec3 translation = vec3_new(3.0f, -2.0f, 5.0f);
  vkr_transform_translate(&transform, translation);
  assert(transform.is_dirty == true && "translate should mark dirty");

  VkrQuat rotation =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), VKR_HALF_PI);
  vkr_transform_rotate(&transform, rotation);
  assert(transform.is_dirty == true && "rotate should keep dirty flag set");

  Vec3 scale = vec3_new(2.0f, 0.5f, 1.5f);
  vkr_transform_scale(&transform, scale);
  assert(transform.is_dirty == true && "scale should keep dirty flag set");

  Mat4 updated = vkr_transform_get_local(&transform);
  assert(transform.is_dirty == false && "get_local should clear dirty flag");

  Mat4 expected =
      mat4_mul(mat4_translate(transform.position),
               mat4_mul(vkr_quat_to_mat4(transform.rotation),
                        mat4_scale(transform.scale)));
  assert(mat4_equals(updated, expected, 0.0001f) &&
         "local matrix computation mismatch");

  Mat4 cached = vkr_transform_get_local(&transform);
  assert(mat4_equals(cached, expected, 0.0001f) &&
         "cached local matrix should remain identical");

  printf("  test_transform_local_matrix_and_dirty_flag PASSED\n");
}

static void test_transform_rotation_normalization(void) {
  printf("  Running test_transform_rotation_normalization...\n");

  VkrTransform transform = vkr_transform_identity();

  VkrQuat delta_raw = vkr_quat_new(0.0f, 0.6f, 0.0f, 0.6f);
  vkr_transform_rotate(&transform, delta_raw);
  VkrQuat normalized_delta = vkr_quat_normalize(delta_raw);

  assert(quat_equals(transform.rotation, normalized_delta, 0.0001f) &&
         "first rotation should match normalized delta");
  assert(float_equals(vkr_quat_length(transform.rotation), 1.0f, 0.0001f) &&
         "rotation should remain unit length");

  VkrQuat second_delta =
      vkr_quat_from_axis_angle(vec3_new(1.0f, 0.0f, 0.0f), VKR_QUARTER_PI);
  vkr_transform_rotate(&transform, second_delta);

  VkrQuat expected =
      vkr_quat_normalize(vkr_quat_mul(normalized_delta,
                                      vkr_quat_normalize(second_delta)));
  assert(quat_equals(transform.rotation, expected, 0.0001f) &&
         "sequential rotations should compose correctly");

  printf("  test_transform_rotation_normalization PASSED\n");
}

static void test_transform_world_with_parent(void) {
  printf("  Running test_transform_world_with_parent...\n");

  VkrTransform parent =
      vkr_transform_new(vec3_new(2.0f, 0.0f, 0.0f),
                        vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f),
                                                 VKR_HALF_PI),
                        vec3_one());

  VkrTransform child =
      vkr_transform_new(vec3_new(0.0f, 1.0f, 0.0f),
                        vkr_quat_from_axis_angle(vec3_new(1.0f, 0.0f, 0.0f),
                                                 VKR_QUARTER_PI),
                        vec3_one());

  child.parent = &parent;

  Mat4 parent_world = vkr_transform_get_world(&parent);
  Mat4 child_local = vkr_transform_get_local(&child);
  Mat4 expected_world = mat4_mul(parent_world, child_local);

  Mat4 actual_world = vkr_transform_get_world(&child);
  assert(mat4_equals(actual_world, expected_world, 0.0001f) &&
         "child world matrix should combine parent and child transforms");

  Vec3 expected_position = mat4_position(expected_world);
  Vec3 actual_position = mat4_position(actual_world);
  assert(vec3_equals(actual_position, expected_position, 0.0001f) &&
         "world matrix translation mismatch");

  printf("  test_transform_world_with_parent PASSED\n");
}

bool32_t run_transform_tests(void) {
  printf("--- Starting Transform Tests ---\n");

  test_transform_new_initialization();
  test_transform_local_matrix_and_dirty_flag();
  test_transform_rotation_normalization();
  test_transform_world_with_parent();

  printf("--- Transform Tests Completed ---\n");
  return true;
}
