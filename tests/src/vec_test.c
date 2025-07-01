#include "vec_test.h"

// Helper function for floating-point comparison with epsilon
static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return abs_f32(a - b) < epsilon;
}

// Helper function for Vec2 comparison
static bool32_t vec2_equals(Vec2 a, Vec2 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon);
}

// Helper function for Vec3 comparison (Vec3 is now Vec4 internally)
static bool32_t vec3_equals(Vec3 a, Vec3 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon); // Ignore W component
}

// Helper function for Vec4 comparison
static bool32_t vec4_equals(Vec4 a, Vec4 b, float32_t epsilon) {
  return float_equals(a.x, b.x, epsilon) && float_equals(a.y, b.y, epsilon) &&
         float_equals(a.z, b.z, epsilon) && float_equals(a.w, b.w, epsilon);
}

// =============================================================================
// Vec2 Tests
// =============================================================================

static void test_vec2_constructors(void) {
  printf("  Running test_vec2_constructors...\n");

  // Test vec2_new
  Vec2 v1 = vec2_new(3.0f, 4.0f);
  assert(float_equals(v1.x, 3.0f, FLOAT_EPSILON) && "vec2_new x failed");
  assert(float_equals(v1.y, 4.0f, FLOAT_EPSILON) && "vec2_new y failed");

  // Test semantic aliases
  assert(float_equals(v1.r, 3.0f, FLOAT_EPSILON) && "vec2 r alias failed");
  assert(float_equals(v1.g, 4.0f, FLOAT_EPSILON) && "vec2 g alias failed");
  assert(float_equals(v1.s, 3.0f, FLOAT_EPSILON) && "vec2 s alias failed");
  assert(float_equals(v1.t, 4.0f, FLOAT_EPSILON) && "vec2 t alias failed");
  assert(float_equals(v1.u, 3.0f, FLOAT_EPSILON) && "vec2 u alias failed");
  assert(float_equals(v1.v, 4.0f, FLOAT_EPSILON) && "vec2 v alias failed");

  // Test array access
  assert(float_equals(v1.elements[0], 3.0f, FLOAT_EPSILON) &&
         "vec2 elements[0] failed");
  assert(float_equals(v1.elements[1], 4.0f, FLOAT_EPSILON) &&
         "vec2 elements[1] failed");

  // Test vec2_zero
  Vec2 v2 = vec2_zero();
  assert(vec2_equals(v2, vec2_new(0.0f, 0.0f), FLOAT_EPSILON) &&
         "vec2_zero failed");

  // Test vec2_one
  Vec2 v3 = vec2_one();
  assert(vec2_equals(v3, vec2_new(1.0f, 1.0f), FLOAT_EPSILON) &&
         "vec2_one failed");

  printf("  test_vec2_constructors PASSED\n");
}

static void test_vec2_arithmetic(void) {
  printf("  Running test_vec2_arithmetic...\n");

  Vec2 a = vec2_new(3.0f, 4.0f);
  Vec2 b = vec2_new(1.0f, 2.0f);

  // Test addition
  Vec2 add_result = vec2_add(a, b);
  assert(vec2_equals(add_result, vec2_new(4.0f, 6.0f), FLOAT_EPSILON) &&
         "vec2_add failed");

  // Test subtraction
  Vec2 sub_result = vec2_sub(a, b);
  assert(vec2_equals(sub_result, vec2_new(2.0f, 2.0f), FLOAT_EPSILON) &&
         "vec2_sub failed");

  // Test multiplication
  Vec2 mul_result = vec2_mul(a, b);
  assert(vec2_equals(mul_result, vec2_new(3.0f, 8.0f), FLOAT_EPSILON) &&
         "vec2_mul failed");

  // Test division
  Vec2 div_result = vec2_div(a, b);
  assert(vec2_equals(div_result, vec2_new(3.0f, 2.0f), FLOAT_EPSILON) &&
         "vec2_div failed");

  // Test scaling
  Vec2 scale_result = vec2_scale(a, 2.0f);
  assert(vec2_equals(scale_result, vec2_new(6.0f, 8.0f), FLOAT_EPSILON) &&
         "vec2_scale failed");

  // Test negation
  Vec2 neg_result = vec2_negate(a);
  assert(vec2_equals(neg_result, vec2_new(-3.0f, -4.0f), FLOAT_EPSILON) &&
         "vec2_negate failed");

  printf("  test_vec2_arithmetic PASSED\n");
}

static void test_vec2_geometric(void) {
  printf("  Running test_vec2_geometric...\n");

  Vec2 a = vec2_new(3.0f, 4.0f);
  Vec2 b = vec2_new(1.0f, 0.0f);

  // Test dot product
  float dot_result = vec2_dot(a, b);
  assert(float_equals(dot_result, 3.0f, FLOAT_EPSILON) && "vec2_dot failed");

  // Test length squared
  float len_sq = vec2_length_squared(a);
  assert(float_equals(len_sq, 25.0f, FLOAT_EPSILON) &&
         "vec2_length_squared failed");

  // Test length
  float len = vec2_length(a);
  assert(float_equals(len, 5.0f, FLOAT_EPSILON) && "vec2_length failed");

  // Test normalization
  Vec2 normalized = vec2_normalize(a);
  assert(vec2_equals(normalized, vec2_new(0.6f, 0.8f), 0.001f) &&
         "vec2_normalize failed");

  // Test normalization of zero vector
  Vec2 zero_norm = vec2_normalize(vec2_zero());
  assert(vec2_equals(zero_norm, vec2_zero(), FLOAT_EPSILON) &&
         "vec2_normalize zero failed");

  // Test distance
  float dist = vec2_distance(a, b);
  assert(float_equals(dist, sqrt_f32(20.0f), FLOAT_EPSILON) &&
         "vec2_distance failed");

  // Test linear interpolation
  Vec2 lerp_result = vec2_lerp(a, b, 0.5f);
  assert(vec2_equals(lerp_result, vec2_new(2.0f, 2.0f), FLOAT_EPSILON) &&
         "vec2_lerp failed");

  printf("  test_vec2_geometric PASSED\n");
}

// =============================================================================
// Vec3 Tests
// =============================================================================

static void test_vec3_constructors(void) {
  printf("  Running test_vec3_constructors...\n");

  // Test vec3_new
  Vec3 v1 = vec3_new(1.0f, 2.0f, 3.0f);
  assert(float_equals(v1.x, 1.0f, FLOAT_EPSILON) && "vec3_new x failed");
  assert(float_equals(v1.y, 2.0f, FLOAT_EPSILON) && "vec3_new y failed");
  assert(float_equals(v1.z, 3.0f, FLOAT_EPSILON) && "vec3_new z failed");

  // Test semantic aliases
  assert(float_equals(v1.r, 1.0f, FLOAT_EPSILON) && "vec3 r alias failed");
  assert(float_equals(v1.g, 2.0f, FLOAT_EPSILON) && "vec3 g alias failed");
  assert(float_equals(v1.b, 3.0f, FLOAT_EPSILON) && "vec3 b alias failed");
  assert(float_equals(v1.s, 1.0f, FLOAT_EPSILON) && "vec3 s alias failed");
  assert(float_equals(v1.t, 2.0f, FLOAT_EPSILON) && "vec3 t alias failed");
  assert(float_equals(v1.p, 3.0f, FLOAT_EPSILON) && "vec3 p alias failed");

  // Test array access (Vec3 now has 4 elements internally)
  assert(float_equals(v1.elements[0], 1.0f, FLOAT_EPSILON) &&
         "vec3 elements[0] failed");
  assert(float_equals(v1.elements[1], 2.0f, FLOAT_EPSILON) &&
         "vec3 elements[1] failed");
  assert(float_equals(v1.elements[2], 3.0f, FLOAT_EPSILON) &&
         "vec3 elements[2] failed");
  assert(float_equals(v1.elements[3], 0.0f, FLOAT_EPSILON) &&
         "vec3 elements[3] (w) should be 0");

  // Test vec3_zero
  Vec3 v2 = vec3_zero();
  assert(vec3_equals(v2, vec3_new(0.0f, 0.0f, 0.0f), FLOAT_EPSILON) &&
         "vec3_zero failed");

  // Test vec3_one
  Vec3 v3 = vec3_one();
  assert(vec3_equals(v3, vec3_new(1.0f, 1.0f, 1.0f), FLOAT_EPSILON) &&
         "vec3_one failed");

  printf("  test_vec3_constructors PASSED\n");
}

static void test_vec3_arithmetic(void) {
  printf("  Running test_vec3_arithmetic...\n");

  Vec3 a = vec3_new(2.0f, 3.0f, 4.0f);
  Vec3 b = vec3_new(1.0f, 2.0f, 1.0f);

  // Test addition
  Vec3 add_result = vec3_add(a, b);
  assert(vec3_equals(add_result, vec3_new(3.0f, 5.0f, 5.0f), FLOAT_EPSILON) &&
         "vec3_add failed");

  // Test subtraction
  Vec3 sub_result = vec3_sub(a, b);
  assert(vec3_equals(sub_result, vec3_new(1.0f, 1.0f, 3.0f), FLOAT_EPSILON) &&
         "vec3_sub failed");

  // Test multiplication
  Vec3 mul_result = vec3_mul(a, b);
  assert(vec3_equals(mul_result, vec3_new(2.0f, 6.0f, 4.0f), FLOAT_EPSILON) &&
         "vec3_mul failed");

  // Test division
  Vec3 div_result = vec3_div(a, b);
  assert(vec3_equals(div_result, vec3_new(2.0f, 1.5f, 4.0f), FLOAT_EPSILON) &&
         "vec3_div failed");

  // Test scaling
  Vec3 scale_result = vec3_scale(a, 2.0f);
  assert(vec3_equals(scale_result, vec3_new(4.0f, 6.0f, 8.0f), FLOAT_EPSILON) &&
         "vec3_scale failed");

  // Test negation
  Vec3 neg_result = vec3_negate(a);
  assert(
      vec3_equals(neg_result, vec3_new(-2.0f, -3.0f, -4.0f), FLOAT_EPSILON) &&
      "vec3_negate failed");

  printf("  test_vec3_arithmetic PASSED\n");
}

static void test_vec3_geometric(void) {
  printf("  Running test_vec3_geometric...\n");

  Vec3 a = vec3_new(1.0f, 2.0f, 3.0f);
  Vec3 b = vec3_new(4.0f, 5.0f, 6.0f);
  Vec3 unit_x = vec3_new(1.0f, 0.0f, 0.0f);
  Vec3 unit_y = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 unit_z = vec3_new(0.0f, 0.0f, 1.0f);

  // Test dot product
  float dot_result = vec3_dot(a, b);
  assert(float_equals(dot_result, 32.0f, FLOAT_EPSILON) && "vec3_dot failed");

  // Test cross product
  Vec3 cross_x_y = vec3_cross(unit_x, unit_y);
  assert(vec3_equals(cross_x_y, unit_z, FLOAT_EPSILON) &&
         "vec3_cross x×y failed");

  Vec3 cross_y_z = vec3_cross(unit_y, unit_z);
  assert(vec3_equals(cross_y_z, unit_x, FLOAT_EPSILON) &&
         "vec3_cross y×z failed");

  Vec3 cross_z_x = vec3_cross(unit_z, unit_x);
  assert(vec3_equals(cross_z_x, unit_y, FLOAT_EPSILON) &&
         "vec3_cross z×x failed");

  // Test length squared
  float len_sq = vec3_length_squared(a);
  assert(float_equals(len_sq, 14.0f, FLOAT_EPSILON) &&
         "vec3_length_squared failed");

  // Test length
  float len = vec3_length(a);
  assert(float_equals(len, sqrt_f32(14.0f), FLOAT_EPSILON) &&
         "vec3_length failed");

  // Test normalization (use larger epsilon due to SIMD rsqrt approximation)
  Vec3 normalized = vec3_normalize(unit_x);
  assert(vec3_equals(normalized, unit_x, 0.001f) &&
         "vec3_normalize unit vector failed");

  // Test normalization of zero vector
  Vec3 zero_norm = vec3_normalize(vec3_zero());
  assert(vec3_equals(zero_norm, vec3_zero(), FLOAT_EPSILON) &&
         "vec3_normalize zero failed");

  // Test distance
  float dist = vec3_distance(a, b);
  assert(float_equals(dist, sqrt_f32(27.0f), FLOAT_EPSILON) &&
         "vec3_distance failed");

  // Test reflection
  Vec3 incident = vec3_new(1.0f, -1.0f, 0.0f);
  Vec3 normal = vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 reflected = vec3_reflect(incident, normal);
  assert(vec3_equals(reflected, vec3_new(1.0f, 1.0f, 0.0f), FLOAT_EPSILON) &&
         "vec3_reflect failed");

  // Test linear interpolation
  Vec3 lerp_result = vec3_lerp(a, b, 0.5f);
  assert(vec3_equals(lerp_result, vec3_new(2.5f, 3.5f, 4.5f), FLOAT_EPSILON) &&
         "vec3_lerp failed");

  printf("  test_vec3_geometric PASSED\n");
}

// =============================================================================
// Vec4 Tests
// =============================================================================

static void test_vec4_constructors(void) {
  printf("  Running test_vec4_constructors...\n");

  // Test vec4_new
  Vec4 v1 = vec4_new(1.0f, 2.0f, 3.0f, 4.0f);
  assert(float_equals(v1.x, 1.0f, FLOAT_EPSILON) && "vec4_new x failed");
  assert(float_equals(v1.y, 2.0f, FLOAT_EPSILON) && "vec4_new y failed");
  assert(float_equals(v1.z, 3.0f, FLOAT_EPSILON) && "vec4_new z failed");
  assert(float_equals(v1.w, 4.0f, FLOAT_EPSILON) && "vec4_new w failed");

  // Test vec4_zero
  Vec4 v2 = vec4_zero();
  assert(vec4_equals(v2, vec4_new(0.0f, 0.0f, 0.0f, 0.0f), FLOAT_EPSILON) &&
         "vec4_zero failed");

  // Test vec4_one
  Vec4 v3 = vec4_one();
  assert(vec4_equals(v3, vec4_new(1.0f, 1.0f, 1.0f, 1.0f), FLOAT_EPSILON) &&
         "vec4_one failed");

  printf("  test_vec4_constructors PASSED\n");
}

static void test_vec4_arithmetic(void) {
  printf("  Running test_vec4_arithmetic...\n");

  Vec4 a = vec4_new(2.0f, 3.0f, 4.0f, 5.0f);
  Vec4 b = vec4_new(1.0f, 2.0f, 1.0f, 2.0f);

  // Test addition
  Vec4 add_result = vec4_add(a, b);
  assert(vec4_equals(add_result, vec4_new(3.0f, 5.0f, 5.0f, 7.0f),
                     FLOAT_EPSILON) &&
         "vec4_add failed");

  // Test subtraction
  Vec4 sub_result = vec4_sub(a, b);
  assert(vec4_equals(sub_result, vec4_new(1.0f, 1.0f, 3.0f, 3.0f),
                     FLOAT_EPSILON) &&
         "vec4_sub failed");

  // Test multiplication
  Vec4 mul_result = vec4_mul(a, b);
  assert(vec4_equals(mul_result, vec4_new(2.0f, 6.0f, 4.0f, 10.0f),
                     FLOAT_EPSILON) &&
         "vec4_mul failed");

  // Test division
  Vec4 div_result = vec4_div(a, b);
  assert(vec4_equals(div_result, vec4_new(2.0f, 1.5f, 4.0f, 2.5f),
                     FLOAT_EPSILON) &&
         "vec4_div failed");

  // Test scaling
  Vec4 scale_result = vec4_scale(a, 2.0f);
  assert(vec4_equals(scale_result, vec4_new(4.0f, 6.0f, 8.0f, 10.0f),
                     FLOAT_EPSILON) &&
         "vec4_scale failed");

  // Test negation
  Vec4 neg_result = vec4_negate(a);
  assert(vec4_equals(neg_result, vec4_new(-2.0f, -3.0f, -4.0f, -5.0f),
                     FLOAT_EPSILON) &&
         "vec4_negate failed");

  printf("  test_vec4_arithmetic PASSED\n");
}

static void test_vec4_geometric(void) {
  printf("  Running test_vec4_geometric...\n");

  Vec4 a = vec4_new(1.0f, 2.0f, 3.0f, 4.0f);
  Vec4 b = vec4_new(2.0f, 3.0f, 4.0f, 5.0f);

  // Test dot product
  float dot_result = vec4_dot(a, b);
  assert(float_equals(dot_result, 40.0f, FLOAT_EPSILON) && "vec4_dot failed");

  // Test 3D dot product (ignoring w component)
  float dot3_result = vec4_dot3(a, b);
  assert(float_equals(dot3_result, 20.0f, FLOAT_EPSILON) && "vec4_dot3 failed");

  // Test length squared
  float len_sq = vec4_length_squared(a);
  assert(float_equals(len_sq, 30.0f, FLOAT_EPSILON) &&
         "vec4_length_squared failed");

  // Test 3D length squared
  float len3_sq = vec4_length3_squared_fast(a);
  assert(float_equals(len3_sq, 14.0f, FLOAT_EPSILON) &&
         "vec4_length3_squared_fast failed");

  // Test length
  float len = vec4_length(a);
  assert(float_equals(len, sqrt_f32(30.0f), FLOAT_EPSILON) &&
         "vec4_length failed");

  // Test normalization
  Vec4 test_vec = vec4_new(2.0f, 0.0f, 0.0f, 0.0f);
  Vec4 normalized = vec4_normalize(test_vec);
  Vec4 expected = vec4_new(1.0f, 0.0f, 0.0f, 0.0f);
  assert(vec4_equals(normalized, expected, 0.001f) && "vec4_normalize failed");

  // Test normalization of zero vector
  Vec4 zero_norm = vec4_normalize(vec4_zero());
  assert(vec4_equals(zero_norm, vec4_zero(), FLOAT_EPSILON) &&
         "vec4_normalize zero failed");

  // Test distance
  float dist = vec4_distance(a, b);
  assert(float_equals(dist, sqrt_f32(4.0f), FLOAT_EPSILON) &&
         "vec4_distance failed");

  // Test linear interpolation
  Vec4 lerp_start = vec4_new(0.0f, 0.0f, 0.0f, 0.0f);
  Vec4 lerp_end = vec4_new(2.0f, 4.0f, 6.0f, 8.0f);
  Vec4 lerp_result = vec4_lerp(lerp_start, lerp_end, 0.5f);
  assert(vec4_equals(lerp_result, vec4_new(1.0f, 2.0f, 3.0f, 4.0f), 0.001f) &&
         "vec4_lerp failed");

  printf("  test_vec4_geometric PASSED\n");
}

// =============================================================================
// Integer Vector Tests
// =============================================================================

static void test_ivec2_operations(void) {
  printf("  Running test_ivec2_operations...\n");

  // Test constructors
  IVec2 v1 = ivec2_new(3, 4);
  assert(v1.x == 3 && "ivec2_new x failed");
  assert(v1.y == 4 && "ivec2_new y failed");

  // Test semantic aliases
  assert(v1.r == 3 && "ivec2 r alias failed");
  assert(v1.g == 4 && "ivec2 g alias failed");
  assert(v1.s == 3 && "ivec2 s alias failed");
  assert(v1.t == 4 && "ivec2 t alias failed");
  assert(v1.u == 3 && "ivec2 u alias failed");
  assert(v1.v == 4 && "ivec2 v alias failed");

  // Test array access
  assert(v1.elements[0] == 3 && "ivec2 elements[0] failed");
  assert(v1.elements[1] == 4 && "ivec2 elements[1] failed");

  IVec2 v2 = ivec2_zero();
  assert(v2.x == 0 && v2.y == 0 && "ivec2_zero failed");

  // Test arithmetic
  IVec2 a = ivec2_new(5, 6);
  IVec2 b = ivec2_new(2, 3);

  IVec2 add_result = ivec2_add(a, b);
  assert(add_result.x == 7 && add_result.y == 9 && "ivec2_add failed");

  IVec2 sub_result = ivec2_sub(a, b);
  assert(sub_result.x == 3 && sub_result.y == 3 && "ivec2_sub failed");

  IVec2 mul_result = ivec2_mul(a, b);
  assert(mul_result.x == 10 && mul_result.y == 18 && "ivec2_mul failed");

  IVec2 scale_result = ivec2_scale(a, 2);
  assert(scale_result.x == 10 && scale_result.y == 12 && "ivec2_scale failed");

  printf("  test_ivec2_operations PASSED\n");
}

static void test_ivec3_operations(void) {
  printf("  Running test_ivec3_operations...\n");

  // Test constructors
  IVec3 v1 = ivec3_new(1, 2, 3);
  assert(v1.x == 1 && "ivec3_new x failed");
  assert(v1.y == 2 && "ivec3_new y failed");
  assert(v1.z == 3 && "ivec3_new z failed");

  // Test semantic aliases
  assert(v1.r == 1 && "ivec3 r alias failed");
  assert(v1.g == 2 && "ivec3 g alias failed");
  assert(v1.b == 3 && "ivec3 b alias failed");

  // Test array access
  assert(v1.elements[0] == 1 && "ivec3 elements[0] failed");
  assert(v1.elements[1] == 2 && "ivec3 elements[1] failed");
  assert(v1.elements[2] == 3 && "ivec3 elements[2] failed");

  IVec3 v2 = ivec3_zero();
  assert(v2.x == 0 && v2.y == 0 && v2.z == 0 && "ivec3_zero failed");

  // Test arithmetic
  IVec3 a = ivec3_new(4, 5, 6);
  IVec3 b = ivec3_new(1, 2, 2);

  IVec3 add_result = ivec3_add(a, b);
  assert(add_result.x == 5 && add_result.y == 7 && add_result.z == 8 &&
         "ivec3_add failed");

  IVec3 sub_result = ivec3_sub(a, b);
  assert(sub_result.x == 3 && sub_result.y == 3 && sub_result.z == 4 &&
         "ivec3_sub failed");

  IVec3 mul_result = ivec3_mul(a, b);
  assert(mul_result.x == 4 && mul_result.y == 10 && mul_result.z == 12 &&
         "ivec3_mul failed");

  IVec3 scale_result = ivec3_scale(a, 3);
  assert(scale_result.x == 12 && scale_result.y == 15 && scale_result.z == 18 &&
         "ivec3_scale failed");

  printf("  test_ivec3_operations PASSED\n");
}

static void test_ivec4_operations(void) {
  printf("  Running test_ivec4_operations...\n");

  // Test constructors
  IVec4 v1 = ivec4_new(1, 2, 3, 4);
  assert(v1.x == 1 && "ivec4_new x failed");
  assert(v1.y == 2 && "ivec4_new y failed");
  assert(v1.z == 3 && "ivec4_new z failed");
  assert(v1.w == 4 && "ivec4_new w failed");

  IVec4 v2 = ivec4_zero();
  assert(v2.x == 0 && v2.y == 0 && v2.z == 0 && v2.w == 0 &&
         "ivec4_zero failed");

  // Test SIMD-accelerated arithmetic
  IVec4 a = ivec4_new(6, 8, 10, 12);
  IVec4 b = ivec4_new(2, 2, 2, 4);

  IVec4 add_result = ivec4_add(a, b);
  assert(add_result.x == 8 && add_result.y == 10 && add_result.z == 12 &&
         add_result.w == 16 && "ivec4_add failed");

  IVec4 sub_result = ivec4_sub(a, b);
  assert(sub_result.x == 4 && sub_result.y == 6 && sub_result.z == 8 &&
         sub_result.w == 8 && "ivec4_sub failed");

  IVec4 mul_result = ivec4_mul(a, b);
  assert(mul_result.x == 12 && mul_result.y == 16 && mul_result.z == 20 &&
         mul_result.w == 48 && "ivec4_mul failed");

  IVec4 scale_result = ivec4_scale(a, 2);
  assert(scale_result.x == 12 && scale_result.y == 16 && scale_result.z == 20 &&
         scale_result.w == 24 && "ivec4_scale failed");

  printf("  test_ivec4_operations PASSED\n");
}

// =============================================================================
// Type Conversion Tests
// =============================================================================

static void test_type_conversions(void) {
  printf("  Running test_type_conversions...\n");

  // Test Vec2 to Vec3
  Vec2 v2 = vec2_new(1.5f, 2.5f);
  Vec3 v2_to_v3 = vec2_to_vec3(v2, 3.5f);
  assert(vec3_equals(v2_to_v3, vec3_new(1.5f, 2.5f, 3.5f), FLOAT_EPSILON) &&
         "vec2_to_vec3 failed");

  // Test Vec3 to Vec2
  Vec3 v3 = vec3_new(4.0f, 5.0f, 6.0f);
  Vec2 v3_to_v2 = vec3_to_vec2(v3);
  assert(vec2_equals(v3_to_v2, vec2_new(4.0f, 5.0f), FLOAT_EPSILON) &&
         "vec3_to_vec2 failed");

  // Test Vec3 to Vec4
  Vec4 v3_to_v4 = vec3_to_vec4(v3, 7.0f);
  assert(
      vec4_equals(v3_to_v4, vec4_new(4.0f, 5.0f, 6.0f, 7.0f), FLOAT_EPSILON) &&
      "vec3_to_vec4 failed");

  // Test Vec4 to Vec3
  Vec4 v4 = vec4_new(8.0f, 9.0f, 10.0f, 11.0f);
  Vec3 v4_to_v3 = vec4_to_vec3(v4);
  assert(vec3_equals(v4_to_v3, vec3_new(8.0f, 9.0f, 10.0f), FLOAT_EPSILON) &&
         "vec4_to_vec3 failed");

  printf("  test_type_conversions PASSED\n");
}

// =============================================================================
// FMA and Mutable Operations Tests
// =============================================================================

static void test_fma_operations(void) {
  printf("  Running test_fma_operations...\n");

  Vec4 a = vec4_new(1.0f, 2.0f, 3.0f, 4.0f);
  Vec4 b = vec4_new(2.0f, 3.0f, 4.0f, 5.0f);
  Vec4 c = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);

  // Test vec4_muladd: a * b + c
  Vec4 muladd_result = vec4_muladd(a, b, c);
  Vec4 expected_muladd = vec4_new(3.0f, 7.0f, 13.0f, 21.0f);
  assert(vec4_equals(muladd_result, expected_muladd, 0.001f) &&
         "vec4_muladd failed");

  // Test vec4_mulsub: a * b - c
  Vec4 mulsub_result = vec4_mulsub(a, b, c);
  Vec4 expected_mulsub = vec4_new(1.0f, 5.0f, 11.0f, 19.0f);
  assert(vec4_equals(mulsub_result, expected_mulsub, 0.001f) &&
         "vec4_mulsub failed");

  // Test vec4_scaleadd: a + v * scale
  Vec4 scaleadd_result = vec4_scaleadd(a, b, 2.0f);
  Vec4 expected_scaleadd = vec4_new(5.0f, 8.0f, 11.0f, 14.0f);
  assert(vec4_equals(scaleadd_result, expected_scaleadd, 0.001f) &&
         "vec4_scaleadd failed");

  printf("  test_fma_operations PASSED\n");
}

static void test_mutable_operations(void) {
  printf("  Running test_mutable_operations...\n");

  Vec4 a = vec4_new(2.0f, 4.0f, 6.0f, 8.0f);
  Vec4 b = vec4_new(1.0f, 2.0f, 3.0f, 4.0f);
  Vec4 result = vec4_zero();

  // Test vec4_add_mut
  vec4_add_mut(&result, a, b);
  assert(
      vec4_equals(result, vec4_new(3.0f, 6.0f, 9.0f, 12.0f), FLOAT_EPSILON) &&
      "vec4_add_mut failed");

  // Test vec4_sub_mut
  vec4_sub_mut(&result, a, b);
  assert(vec4_equals(result, vec4_new(1.0f, 2.0f, 3.0f, 4.0f), FLOAT_EPSILON) &&
         "vec4_sub_mut failed");

  // Test vec4_mul_mut
  vec4_mul_mut(&result, a, b);
  assert(
      vec4_equals(result, vec4_new(2.0f, 8.0f, 18.0f, 32.0f), FLOAT_EPSILON) &&
      "vec4_mul_mut failed");

  // Test vec4_scale_mut
  vec4_scale_mut(&result, a, 0.5f);
  assert(vec4_equals(result, vec4_new(1.0f, 2.0f, 3.0f, 4.0f), FLOAT_EPSILON) &&
         "vec4_scale_mut failed");

  printf("  test_mutable_operations PASSED\n");
}

// =============================================================================
// Test Runner
// =============================================================================

bool32_t run_vec_tests(void) {
  printf("--- Starting Vector Math Tests ---\n");

  // Vec2 tests
  test_vec2_constructors();
  test_vec2_arithmetic();
  test_vec2_geometric();

  // Vec3 tests
  test_vec3_constructors();
  test_vec3_arithmetic();
  test_vec3_geometric();

  // Vec4 tests
  test_vec4_constructors();
  test_vec4_arithmetic();
  test_vec4_geometric();

  // Integer vector tests
  test_ivec2_operations();
  test_ivec3_operations();
  test_ivec4_operations();

  // Type conversion tests
  test_type_conversions();

  // Advanced operation tests
  test_fma_operations();
  test_mutable_operations();

  printf("--- Vector Math Tests Completed ---\n");
  return true;
}