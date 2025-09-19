#include "simd_test.h"

static bool32_t simd_vector_equals(VKR_SIMD_F32X4 a, VKR_SIMD_F32X4 b,
                                   float32_t epsilon) {
  return (abs_f32(a.x - b.x) < epsilon) && (abs_f32(a.y - b.y) < epsilon) &&
         (abs_f32(a.z - b.z) < epsilon) && (abs_f32(a.w - b.w) < epsilon);
}

static bool32_t simd_i32_vector_equals(VKR_SIMD_I32X4 a, VKR_SIMD_I32X4 b) {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z) && (a.w == b.w);
}

static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return abs_f32(a - b) < epsilon;
}

static void test_simd_i32_set(void) {
  printf("  Running test_simd_i32_set...\n");

  // Test vkr_simd_set_i32x4
  VKR_SIMD_I32X4 v1 = vkr_simd_set_i32x4(10, 20, 30, 40);
  assert(v1.x == 10 && "vkr_simd_set_i32x4 failed for x");
  assert(v1.y == 20 && "vkr_simd_set_i32x4 failed for y");
  assert(v1.z == 30 && "vkr_simd_set_i32x4 failed for z");
  assert(v1.w == 40 && "vkr_simd_set_i32x4 failed for w");

  // Test vkr_simd_set1_i32x4 (broadcast)
  VKR_SIMD_I32X4 v2 = vkr_simd_set1_i32x4(42);
  assert(v2.x == 42 && "vkr_simd_set1_i32x4 failed for x");
  assert(v2.y == 42 && "vkr_simd_set1_i32x4 failed for y");
  assert(v2.z == 42 && "vkr_simd_set1_i32x4 failed for z");
  assert(v2.w == 42 && "vkr_simd_set1_i32x4 failed for w");

  // Test element access patterns
  assert(v1.r == 10 && "Color access (r) failed");
  assert(v1.g == 20 && "Color access (g) failed");
  assert(v1.b == 30 && "Color access (b) failed");
  assert(v1.a == 40 && "Color access (a) failed");

  assert(v1.s == 10 && "Texture access (s) failed");
  assert(v1.t == 20 && "Texture access (t) failed");
  assert(v1.p == 30 && "Texture access (p) failed");
  assert(v1.q == 40 && "Texture access (q) failed");

  assert(v1.elements[0] == 10 && "Array access [0] failed");
  assert(v1.elements[1] == 20 && "Array access [1] failed");
  assert(v1.elements[2] == 30 && "Array access [2] failed");
  assert(v1.elements[3] == 40 && "Array access [3] failed");

  // Test with negative values
  VKR_SIMD_I32X4 v3 = vkr_simd_set_i32x4(-5, -10, -15, -20);
  assert(v3.x == -5 && "Negative value test failed for x");
  assert(v3.y == -10 && "Negative value test failed for y");
  assert(v3.z == -15 && "Negative value test failed for z");
  assert(v3.w == -20 && "Negative value test failed for w");

  printf("  test_simd_i32_set PASSED\n");
}

static void test_simd_i32_arithmetic(void) {
  printf("  Running test_simd_i32_arithmetic...\n");

  VKR_SIMD_I32X4 a = vkr_simd_set_i32x4(100, 200, 300, 400);
  VKR_SIMD_I32X4 b = vkr_simd_set_i32x4(10, 20, 30, 40);

  // Test addition
  VKR_SIMD_I32X4 add_result = vkr_simd_add_i32x4(a, b);
  VKR_SIMD_I32X4 expected_add = vkr_simd_set_i32x4(110, 220, 330, 440);
  assert(simd_i32_vector_equals(add_result, expected_add) &&
         "vkr_simd_add_i32x4 failed");

  // Test subtraction
  VKR_SIMD_I32X4 sub_result = vkr_simd_sub_i32x4(a, b);
  VKR_SIMD_I32X4 expected_sub = vkr_simd_set_i32x4(90, 180, 270, 360);
  assert(simd_i32_vector_equals(sub_result, expected_sub) &&
         "vkr_simd_sub_i32x4 failed");

  // Test multiplication
  VKR_SIMD_I32X4 mul_result = vkr_simd_mul_i32x4(a, b);
  VKR_SIMD_I32X4 expected_mul = vkr_simd_set_i32x4(1000, 4000, 9000, 16000);
  assert(simd_i32_vector_equals(mul_result, expected_mul) &&
         "vkr_simd_mul_i32x4 failed");

  // Test with negative values
  VKR_SIMD_I32X4 neg_a = vkr_simd_set_i32x4(-10, 15, -25, 35);
  VKR_SIMD_I32X4 neg_b = vkr_simd_set_i32x4(5, -3, 7, -2);

  VKR_SIMD_I32X4 neg_add = vkr_simd_add_i32x4(neg_a, neg_b);
  VKR_SIMD_I32X4 expected_neg_add = vkr_simd_set_i32x4(-5, 12, -18, 33);
  assert(simd_i32_vector_equals(neg_add, expected_neg_add) &&
         "vkr_simd_add_i32x4 failed with negative values");

  VKR_SIMD_I32X4 neg_sub = vkr_simd_sub_i32x4(neg_a, neg_b);
  VKR_SIMD_I32X4 expected_neg_sub = vkr_simd_set_i32x4(-15, 18, -32, 37);
  assert(simd_i32_vector_equals(neg_sub, expected_neg_sub) &&
         "vkr_simd_sub_i32x4 failed with negative values");

  VKR_SIMD_I32X4 neg_mul = vkr_simd_mul_i32x4(neg_a, neg_b);
  VKR_SIMD_I32X4 expected_neg_mul = vkr_simd_set_i32x4(-50, -45, -175, -70);
  assert(simd_i32_vector_equals(neg_mul, expected_neg_mul) &&
         "vkr_simd_mul_i32x4 failed with negative values");

  printf("  test_simd_i32_arithmetic PASSED\n");
}

static void test_simd_i32_edge_cases(void) {
  printf("  Running test_simd_i32_edge_cases...\n");

  // Test with zero
  VKR_SIMD_I32X4 zero = vkr_simd_set1_i32x4(0);
  VKR_SIMD_I32X4 any_value = vkr_simd_set_i32x4(123, -456, 789, -101);

  // Addition with zero
  VKR_SIMD_I32X4 zero_add = vkr_simd_add_i32x4(any_value, zero);
  assert(simd_i32_vector_equals(zero_add, any_value) &&
         "Addition with zero failed");

  // Subtraction with zero
  VKR_SIMD_I32X4 zero_sub = vkr_simd_sub_i32x4(any_value, zero);
  assert(simd_i32_vector_equals(zero_sub, any_value) &&
         "Subtraction with zero failed");

  // Multiplication with zero
  VKR_SIMD_I32X4 zero_mul = vkr_simd_mul_i32x4(any_value, zero);
  assert(simd_i32_vector_equals(zero_mul, zero) &&
         "Multiplication with zero failed");

  // Test with one
  VKR_SIMD_I32X4 one = vkr_simd_set1_i32x4(1);
  VKR_SIMD_I32X4 one_mul = vkr_simd_mul_i32x4(any_value, one);
  assert(simd_i32_vector_equals(one_mul, any_value) &&
         "Multiplication with one failed");

  // Test with maximum and minimum values
  VKR_SIMD_I32X4 max_vals = vkr_simd_set1_i32x4(2147483647);  // INT32_MAX
  VKR_SIMD_I32X4 min_vals = vkr_simd_set1_i32x4(-2147483648); // INT32_MIN

  // Test that we can create and access extreme values
  assert(max_vals.x == 2147483647 && "Max value access failed");
  assert(min_vals.x == -2147483648 && "Min value access failed");

  // Test mixed large values
  VKR_SIMD_I32X4 large_vals =
      vkr_simd_set_i32x4(1000000, -1000000, 500000, -500000);
  VKR_SIMD_I32X4 small_vals = vkr_simd_set_i32x4(2, -2, 3, -3);
  VKR_SIMD_I32X4 large_add = vkr_simd_add_i32x4(large_vals, small_vals);
  VKR_SIMD_I32X4 expected_large_add =
      vkr_simd_set_i32x4(1000002, -1000002, 500003, -500003);
  assert(simd_i32_vector_equals(large_add, expected_large_add) &&
         "Addition with large values failed");

  printf("  test_simd_i32_edge_cases PASSED\n");
}

static void test_simd_load_store(void) {
  printf("  Running test_simd_load_store...\n");

  float32_t input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float32_t output[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  VKR_SIMD_F32X4 v = vkr_simd_load_f32x4(input);

  assert(float_equals(v.x, 1.0f, FLOAT_EPSILON) &&
         "Load failed for x component");
  assert(float_equals(v.y, 2.0f, FLOAT_EPSILON) &&
         "Load failed for y component");
  assert(float_equals(v.z, 3.0f, FLOAT_EPSILON) &&
         "Load failed for z component");
  assert(float_equals(v.w, 4.0f, FLOAT_EPSILON) &&
         "Load failed for w component");

  vkr_simd_store_f32x4(output, v);

  assert(float_equals(output[0], 1.0f, FLOAT_EPSILON) &&
         "Store failed for element 0");
  assert(float_equals(output[1], 2.0f, FLOAT_EPSILON) &&
         "Store failed for element 1");
  assert(float_equals(output[2], 3.0f, FLOAT_EPSILON) &&
         "Store failed for element 2");
  assert(float_equals(output[3], 4.0f, FLOAT_EPSILON) &&
         "Store failed for element 3");

  printf("  test_simd_load_store PASSED\n");
}

static void test_simd_set(void) {
  printf("  Running test_simd_set...\n");

  // Test vkr_simd_set_f32x4
  VKR_SIMD_F32X4 v1 = vkr_simd_set_f32x4(1.5f, 2.5f, 3.5f, 4.5f);
  assert(float_equals(v1.x, 1.5f, FLOAT_EPSILON) &&
         "vkr_simd_set_f32x4 failed for x");
  assert(float_equals(v1.y, 2.5f, FLOAT_EPSILON) &&
         "vkr_simd_set_f32x4 failed for y");
  assert(float_equals(v1.z, 3.5f, FLOAT_EPSILON) &&
         "vkr_simd_set_f32x4 failed for z");
  assert(float_equals(v1.w, 4.5f, FLOAT_EPSILON) &&
         "vkr_simd_set_f32x4 failed for w");

  // Test vkr_simd_set1_f32x4 (broadcast)
  VKR_SIMD_F32X4 v2 = vkr_simd_set1_f32x4(7.0f);
  assert(float_equals(v2.x, 7.0f, FLOAT_EPSILON) &&
         "vkr_simd_set1_f32x4 failed for x");
  assert(float_equals(v2.y, 7.0f, FLOAT_EPSILON) &&
         "vkr_simd_set1_f32x4 failed for y");
  assert(float_equals(v2.z, 7.0f, FLOAT_EPSILON) &&
         "vkr_simd_set1_f32x4 failed for z");
  assert(float_equals(v2.w, 7.0f, FLOAT_EPSILON) &&
         "vkr_simd_set1_f32x4 failed for w");

  // Test element access patterns
  assert(float_equals(v1.r, 1.5f, FLOAT_EPSILON) && "Color access (r) failed");
  assert(float_equals(v1.g, 2.5f, FLOAT_EPSILON) && "Color access (g) failed");
  assert(float_equals(v1.b, 3.5f, FLOAT_EPSILON) && "Color access (b) failed");
  assert(float_equals(v1.a, 4.5f, FLOAT_EPSILON) && "Color access (a) failed");

  assert(float_equals(v1.s, 1.5f, FLOAT_EPSILON) &&
         "Texture access (s) failed");
  assert(float_equals(v1.t, 2.5f, FLOAT_EPSILON) &&
         "Texture access (t) failed");
  assert(float_equals(v1.p, 3.5f, FLOAT_EPSILON) &&
         "Texture access (p) failed");
  assert(float_equals(v1.q, 4.5f, FLOAT_EPSILON) &&
         "Texture access (q) failed");

  assert(float_equals(v1.elements[0], 1.5f, FLOAT_EPSILON) &&
         "Array access [0] failed");
  assert(float_equals(v1.elements[1], 2.5f, FLOAT_EPSILON) &&
         "Array access [1] failed");
  assert(float_equals(v1.elements[2], 3.5f, FLOAT_EPSILON) &&
         "Array access [2] failed");
  assert(float_equals(v1.elements[3], 4.5f, FLOAT_EPSILON) &&
         "Array access [3] failed");

  printf("  test_simd_set PASSED\n");
}

static void test_simd_arithmetic(void) {
  printf("  Running test_simd_arithmetic...\n");

  VKR_SIMD_F32X4 a = vkr_simd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
  VKR_SIMD_F32X4 b = vkr_simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  // Test addition
  VKR_SIMD_F32X4 add_result = vkr_simd_add_f32x4(a, b);
  VKR_SIMD_F32X4 expected_add = vkr_simd_set_f32x4(11.0f, 22.0f, 33.0f, 44.0f);
  assert(simd_vector_equals(add_result, expected_add, FLOAT_EPSILON) &&
         "vkr_simd_add_f32x4 failed");

  // Test subtraction
  VKR_SIMD_F32X4 sub_result = vkr_simd_sub_f32x4(a, b);
  VKR_SIMD_F32X4 expected_sub = vkr_simd_set_f32x4(9.0f, 18.0f, 27.0f, 36.0f);
  assert(simd_vector_equals(sub_result, expected_sub, FLOAT_EPSILON) &&
         "vkr_simd_sub_f32x4 failed");

  // Test multiplication
  VKR_SIMD_F32X4 mul_result = vkr_simd_mul_f32x4(a, b);
  VKR_SIMD_F32X4 expected_mul = vkr_simd_set_f32x4(10.0f, 40.0f, 90.0f, 160.0f);
  assert(simd_vector_equals(mul_result, expected_mul, FLOAT_EPSILON) &&
         "vkr_simd_mul_f32x4 failed");

  // Test division
  VKR_SIMD_F32X4 div_result = vkr_simd_div_f32x4(a, b);
  VKR_SIMD_F32X4 expected_div = vkr_simd_set_f32x4(10.0f, 10.0f, 10.0f, 10.0f);
  assert(simd_vector_equals(div_result, expected_div, FLOAT_EPSILON) &&
         "vkr_simd_div_f32x4 failed");

  printf("  test_simd_arithmetic PASSED\n");
}

static void test_simd_sqrt(void) {
  printf("  Running test_simd_sqrt...\n");

  VKR_SIMD_F32X4 v = vkr_simd_set_f32x4(4.0f, 9.0f, 16.0f, 25.0f);

  // Test square root
  VKR_SIMD_F32X4 sqrt_result = vkr_simd_sqrt_f32x4(v);
  VKR_SIMD_F32X4 expected_sqrt = vkr_simd_set_f32x4(2.0f, 3.0f, 4.0f, 5.0f);
  assert(simd_vector_equals(sqrt_result, expected_sqrt, FLOAT_EPSILON) &&
         "vkr_simd_sqrt_f32x4 failed");

  // Test reciprocal square root
  VKR_SIMD_F32X4 rsqrt_result = vkr_simd_rsqrt_f32x4(v);
  VKR_SIMD_F32X4 expected_rsqrt =
      vkr_simd_set_f32x4(0.5f, 1.0f / 3.0f, 0.25f, 0.2f);
  assert(simd_vector_equals(rsqrt_result, expected_rsqrt, 0.001f) &&
         "vkr_simd_rsqrt_f32x4 failed");

  // Test special case: sqrt of 1
  VKR_SIMD_F32X4 ones = vkr_simd_set1_f32x4(1.0f);
  VKR_SIMD_F32X4 sqrt_ones = vkr_simd_sqrt_f32x4(ones);
  assert(simd_vector_equals(sqrt_ones, ones, FLOAT_EPSILON) &&
         "sqrt(1) failed");

  // Test special case: rsqrt of 1
  VKR_SIMD_F32X4 rsqrt_ones = vkr_simd_rsqrt_f32x4(ones);
  assert(simd_vector_equals(rsqrt_ones, ones, 0.001f) && "rsqrt(1) failed");

  printf("  test_simd_sqrt PASSED\n");
}

static void test_simd_min_max(void) {
  printf("  Running test_simd_min_max...\n");

  VKR_SIMD_F32X4 a = vkr_simd_set_f32x4(1.0f, 5.0f, 2.0f, 8.0f);
  VKR_SIMD_F32X4 b = vkr_simd_set_f32x4(3.0f, 2.0f, 7.0f, 4.0f);

  // Test minimum
  VKR_SIMD_F32X4 min_result = vkr_simd_min_f32x4(a, b);
  VKR_SIMD_F32X4 expected_min = vkr_simd_set_f32x4(1.0f, 2.0f, 2.0f, 4.0f);
  assert(simd_vector_equals(min_result, expected_min, FLOAT_EPSILON) &&
         "vkr_simd_min_f32x4 failed");

  // Test maximum
  VKR_SIMD_F32X4 max_result = vkr_simd_max_f32x4(a, b);
  VKR_SIMD_F32X4 expected_max = vkr_simd_set_f32x4(3.0f, 5.0f, 7.0f, 8.0f);
  assert(simd_vector_equals(max_result, expected_max, FLOAT_EPSILON) &&
         "vkr_simd_max_f32x4 failed");

  // Test with negative values
  VKR_SIMD_F32X4 neg_a = vkr_simd_set_f32x4(-1.0f, -5.0f, -2.0f, -8.0f);
  VKR_SIMD_F32X4 neg_b = vkr_simd_set_f32x4(-3.0f, -2.0f, -7.0f, -4.0f);

  VKR_SIMD_F32X4 neg_min = vkr_simd_min_f32x4(neg_a, neg_b);
  VKR_SIMD_F32X4 expected_neg_min =
      vkr_simd_set_f32x4(-3.0f, -5.0f, -7.0f, -8.0f);
  assert(simd_vector_equals(neg_min, expected_neg_min, FLOAT_EPSILON) &&
         "vkr_simd_min_f32x4 failed for negative values");

  VKR_SIMD_F32X4 neg_max = vkr_simd_max_f32x4(neg_a, neg_b);
  VKR_SIMD_F32X4 expected_neg_max =
      vkr_simd_set_f32x4(-1.0f, -2.0f, -2.0f, -4.0f);
  assert(simd_vector_equals(neg_max, expected_neg_max, FLOAT_EPSILON) &&
         "vkr_simd_max_f32x4 failed for negative values");

  printf("  test_simd_min_max PASSED\n");
}

static void test_simd_fma(void) {
  printf("  Running test_simd_fma...\n");

  VKR_SIMD_F32X4 a = vkr_simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  VKR_SIMD_F32X4 b = vkr_simd_set_f32x4(2.0f, 3.0f, 4.0f, 5.0f);
  VKR_SIMD_F32X4 c = vkr_simd_set_f32x4(3.0f, 4.0f, 5.0f, 6.0f);

  // Test FMA: a + (b * c)
  VKR_SIMD_F32X4 fma_result = vkr_simd_fma_f32x4(a, b, c);
  VKR_SIMD_F32X4 expected_fma =
      vkr_simd_set_f32x4(1.0f + (2.0f * 3.0f), 2.0f + (3.0f * 4.0f),
                         3.0f + (4.0f * 5.0f), 4.0f + (5.0f * 6.0f));
  // expected_fma = {7.0f, 14.0f, 23.0f, 34.0f}
  assert(simd_vector_equals(fma_result, expected_fma, FLOAT_EPSILON) &&
         "vkr_simd_fma_f32x4 failed");

  // Test FMS: a - (b * c)
  VKR_SIMD_F32X4 fms_result = vkr_simd_fms_f32x4(a, b, c);
  VKR_SIMD_F32X4 expected_fms =
      vkr_simd_set_f32x4(1.0f - (2.0f * 3.0f), 2.0f - (3.0f * 4.0f),
                         3.0f - (4.0f * 5.0f), 4.0f - (5.0f * 6.0f));
  // expected_fms = {-5.0f, -10.0f, -17.0f, -26.0f}
  assert(simd_vector_equals(fms_result, expected_fms, FLOAT_EPSILON) &&
         "vkr_simd_fms_f32x4 failed");

  // Test FNMA: -(a + b * c)
  VKR_SIMD_F32X4 fnma_result = vkr_simd_fnma_f32x4(a, b, c);
  VKR_SIMD_F32X4 expected_fnma =
      vkr_simd_set_f32x4(-(1.0f + 2.0f * 3.0f), -(2.0f + 3.0f * 4.0f),
                         -(3.0f + 4.0f * 5.0f), -(4.0f + 5.0f * 6.0f));
  // expected_fnma = {-7.0f, -14.0f, -23.0f, -34.0f}
  assert(simd_vector_equals(fnma_result, expected_fnma, FLOAT_EPSILON) &&
         "vkr_simd_fnma_f32x4 failed");

  // Test FNMS: -(a - b * c)
  VKR_SIMD_F32X4 fnms_result = vkr_simd_fnms_f32x4(a, b, c);
  VKR_SIMD_F32X4 expected_fnms =
      vkr_simd_set_f32x4(-(1.0f - 2.0f * 3.0f), -(2.0f - 3.0f * 4.0f),
                         -(3.0f - 4.0f * 5.0f), -(4.0f - 5.0f * 6.0f));
  // expected_fnms = {5.0f, 10.0f, 17.0f, 26.0f}
  assert(simd_vector_equals(fnms_result, expected_fnms, FLOAT_EPSILON) &&
         "vkr_simd_fnms_f32x4 failed");

  printf("  test_simd_fma PASSED\n");
}

static void test_simd_dot_products(void) {
  printf("  Running test_simd_dot_products...\n");

  VKR_SIMD_F32X4 a = vkr_simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  VKR_SIMD_F32X4 b = vkr_simd_set_f32x4(5.0f, 6.0f, 7.0f, 8.0f);

  // Test horizontal add
  float32_t hadd_result = vkr_simd_hadd_f32x4(a);
  float32_t expected_hadd = 1.0f + 2.0f + 3.0f + 4.0f; // 10.0f
  assert(float_equals(hadd_result, expected_hadd, FLOAT_EPSILON) &&
         "vkr_simd_hadd_f32x4 failed");

  // Test 4D dot product
  float32_t dot4_result = vkr_simd_dot_f32x4(a, b);
  float32_t expected_dot4 =
      (1.0f * 5.0f) + (2.0f * 6.0f) + (3.0f * 7.0f) + (4.0f * 8.0f); // 70.0f
  assert(float_equals(dot4_result, expected_dot4, FLOAT_EPSILON) &&
         "vkr_simd_dot_f32x4 failed");

  // Test 3D dot product (ignores w component)
  float32_t dot3_result = vkr_simd_dot3_f32x4(a, b);
  float32_t expected_dot3 =
      (1.0f * 5.0f) + (2.0f * 6.0f) + (3.0f * 7.0f); // 38.0f
  assert(float_equals(dot3_result, expected_dot3, FLOAT_EPSILON) &&
         "vkr_simd_dot3_f32x4 failed");

  // Test 4D dot product alias
  float32_t dot4_alias_result = vkr_simd_dot4_f32x4(a, b);
  assert(float_equals(dot4_alias_result, expected_dot4, FLOAT_EPSILON) &&
         "vkr_simd_dot4_f32x4 failed");

  // Test with zero vector
  VKR_SIMD_F32X4 zero = vkr_simd_set1_f32x4(0.0f);
  float32_t zero_dot = vkr_simd_dot_f32x4(a, zero);
  assert(float_equals(zero_dot, 0.0f, FLOAT_EPSILON) &&
         "Dot product with zero vector failed");

  // Test with unit vectors
  VKR_SIMD_F32X4 unit_x = vkr_simd_set_f32x4(1.0f, 0.0f, 0.0f, 0.0f);
  VKR_SIMD_F32X4 unit_y = vkr_simd_set_f32x4(0.0f, 1.0f, 0.0f, 0.0f);
  float32_t orthogonal_dot = vkr_simd_dot_f32x4(unit_x, unit_y);
  assert(float_equals(orthogonal_dot, 0.0f, FLOAT_EPSILON) &&
         "Orthogonal vectors dot product failed");

  float32_t unit_dot = vkr_simd_dot_f32x4(unit_x, unit_x);
  assert(float_equals(unit_dot, 1.0f, FLOAT_EPSILON) &&
         "Unit vector self dot product failed");

  printf("  test_simd_dot_products PASSED\n");
}

static void test_simd_shuffle(void) {
  printf("  Running test_simd_shuffle...\n");

  VKR_SIMD_F32X4 v = vkr_simd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);

  // Test identity shuffle (0, 1, 2, 3)
  VKR_SIMD_F32X4 identity = vkr_simd_shuffle_f32x4(v, 0, 1, 2, 3);
  assert(simd_vector_equals(identity, v, FLOAT_EPSILON) &&
         "Identity shuffle failed");

  // Test reverse shuffle (3, 2, 1, 0)
  VKR_SIMD_F32X4 reverse = vkr_simd_shuffle_f32x4(v, 3, 2, 1, 0);
  VKR_SIMD_F32X4 expected_reverse =
      vkr_simd_set_f32x4(40.0f, 30.0f, 20.0f, 10.0f);
  assert(simd_vector_equals(reverse, expected_reverse, FLOAT_EPSILON) &&
         "Reverse shuffle failed");

  // Test broadcast shuffle (0, 0, 0, 0)
  VKR_SIMD_F32X4 broadcast = vkr_simd_shuffle_f32x4(v, 0, 0, 0, 0);
  VKR_SIMD_F32X4 expected_broadcast = vkr_simd_set1_f32x4(10.0f);
  assert(simd_vector_equals(broadcast, expected_broadcast, FLOAT_EPSILON) &&
         "Broadcast shuffle failed");

  // Test custom shuffle (1, 3, 0, 2)
  VKR_SIMD_F32X4 custom = vkr_simd_shuffle_f32x4(v, 1, 3, 0, 2);
  VKR_SIMD_F32X4 expected_custom =
      vkr_simd_set_f32x4(20.0f, 40.0f, 10.0f, 30.0f);
  assert(simd_vector_equals(custom, expected_custom, FLOAT_EPSILON) &&
         "Custom shuffle failed");

  printf("  test_simd_shuffle PASSED\n");
}

static void test_simd_edge_cases(void) {
  printf("  Running test_simd_edge_cases...\n");

  // Test with very small values
  VKR_SIMD_F32X4 tiny = vkr_simd_set1_f32x4(FLOAT_EPSILON);
  VKR_SIMD_F32X4 tiny_add = vkr_simd_add_f32x4(tiny, tiny);
  VKR_SIMD_F32X4 expected_tiny_add = vkr_simd_set1_f32x4(2.0f * FLOAT_EPSILON);
  assert(
      simd_vector_equals(tiny_add, expected_tiny_add, FLOAT_EPSILON / 10.0f) &&
      "Addition with tiny values failed");

  // Test with very large values
  VKR_SIMD_F32X4 large = vkr_simd_set1_f32x4(1000000.0f);
  VKR_SIMD_F32X4 large_add =
      vkr_simd_add_f32x4(large, vkr_simd_set1_f32x4(1.0f));
  VKR_SIMD_F32X4 expected_large_add = vkr_simd_set1_f32x4(1000001.0f);
  assert(simd_vector_equals(large_add, expected_large_add, 1.0f) &&
         "Addition with large values failed");

  // Test with mixed positive and negative values
  VKR_SIMD_F32X4 mixed = vkr_simd_set_f32x4(-10.0f, 5.0f, -2.0f, 8.0f);
  VKR_SIMD_F32X4 abs_like = vkr_simd_max_f32x4(
      mixed, vkr_simd_sub_f32x4(vkr_simd_set1_f32x4(0.0f), mixed));
  VKR_SIMD_F32X4 expected_abs = vkr_simd_set_f32x4(10.0f, 5.0f, 2.0f, 8.0f);
  assert(simd_vector_equals(abs_like, expected_abs, FLOAT_EPSILON) &&
         "Absolute value simulation failed");

  // Test multiplication by zero
  VKR_SIMD_F32X4 zero = vkr_simd_set1_f32x4(0.0f);
  VKR_SIMD_F32X4 any_value =
      vkr_simd_set_f32x4(123.0f, -456.0f, 789.0f, -101112.0f);
  VKR_SIMD_F32X4 zero_result = vkr_simd_mul_f32x4(any_value, zero);
  assert(simd_vector_equals(zero_result, zero, FLOAT_EPSILON) &&
         "Multiplication by zero failed");

  // Test multiplication by one
  VKR_SIMD_F32X4 one = vkr_simd_set1_f32x4(1.0f);
  VKR_SIMD_F32X4 one_result = vkr_simd_mul_f32x4(any_value, one);
  assert(simd_vector_equals(one_result, any_value, FLOAT_EPSILON) &&
         "Multiplication by one failed");

  printf("  test_simd_edge_cases PASSED\n");
}

static void test_simd_scatter_gather(void) {
  printf("  Running test_simd_scatter_gather...\n");

  // Test data
  VKR_SIMD_F32X4 source = vkr_simd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);

  // Test basic gather operation - identity indices
  VKR_SIMD_I32X4 identity_indices = vkr_simd_set_i32x4(0, 1, 2, 3);
  VKR_SIMD_F32X4 gather_identity =
      vkr_simd_gather_f32x4(source, identity_indices);
  assert(simd_vector_equals(gather_identity, source, FLOAT_EPSILON) &&
         "Gather with identity indices failed");

  // Test reverse gather
  VKR_SIMD_I32X4 reverse_indices = vkr_simd_set_i32x4(3, 2, 1, 0);
  VKR_SIMD_F32X4 gather_reverse =
      vkr_simd_gather_f32x4(source, reverse_indices);
  VKR_SIMD_F32X4 expected_reverse =
      vkr_simd_set_f32x4(40.0f, 30.0f, 20.0f, 10.0f);
  assert(simd_vector_equals(gather_reverse, expected_reverse, FLOAT_EPSILON) &&
         "Gather with reverse indices failed");

  // Test gather with duplicated indices
  VKR_SIMD_I32X4 dup_indices = vkr_simd_set_i32x4(0, 0, 2, 2);
  VKR_SIMD_F32X4 gather_dup = vkr_simd_gather_f32x4(source, dup_indices);
  VKR_SIMD_F32X4 expected_dup = vkr_simd_set_f32x4(10.0f, 10.0f, 30.0f, 30.0f);
  assert(simd_vector_equals(gather_dup, expected_dup, FLOAT_EPSILON) &&
         "Gather with duplicated indices failed");

  // Test gather with mixed indices
  VKR_SIMD_I32X4 mixed_indices = vkr_simd_set_i32x4(1, 3, 0, 2);
  VKR_SIMD_F32X4 gather_mixed = vkr_simd_gather_f32x4(source, mixed_indices);
  VKR_SIMD_F32X4 expected_mixed =
      vkr_simd_set_f32x4(20.0f, 40.0f, 10.0f, 30.0f);
  assert(simd_vector_equals(gather_mixed, expected_mixed, FLOAT_EPSILON) &&
         "Gather with mixed indices failed");

  // Test gather with out-of-bounds indices (should return 0.0f for invalid
  // indices)
  VKR_SIMD_I32X4 oob_indices = vkr_simd_set_i32x4(-1, 1, 4, 2);
  VKR_SIMD_F32X4 gather_oob = vkr_simd_gather_f32x4(source, oob_indices);
  VKR_SIMD_F32X4 expected_oob = vkr_simd_set_f32x4(0.0f, 20.0f, 0.0f, 30.0f);
  assert(simd_vector_equals(gather_oob, expected_oob, FLOAT_EPSILON) &&
         "Gather with out-of-bounds indices failed");

  // Test basic scatter operation - identity indices
  VKR_SIMD_F32X4 scatter_identity =
      vkr_simd_scatter_f32x4(source, identity_indices);
  assert(simd_vector_equals(scatter_identity, source, FLOAT_EPSILON) &&
         "Scatter with identity indices failed");

  // Test scatter with reverse indices
  VKR_SIMD_F32X4 scatter_reverse =
      vkr_simd_scatter_f32x4(source, reverse_indices);
  VKR_SIMD_F32X4 expected_scatter_reverse =
      vkr_simd_set_f32x4(40.0f, 30.0f, 20.0f, 10.0f);
  assert(simd_vector_equals(scatter_reverse, expected_scatter_reverse,
                            FLOAT_EPSILON) &&
         "Scatter with reverse indices failed");

  // Test scatter with mixed indices
  VKR_SIMD_F32X4 values_to_scatter =
      vkr_simd_set_f32x4(100.0f, 200.0f, 300.0f, 400.0f);
  VKR_SIMD_I32X4 scatter_indices = vkr_simd_set_i32x4(2, 0, 3, 1);
  VKR_SIMD_F32X4 scatter_mixed =
      vkr_simd_scatter_f32x4(values_to_scatter, scatter_indices);
  VKR_SIMD_F32X4 expected_scatter_mixed =
      vkr_simd_set_f32x4(200.0f, 400.0f, 100.0f, 300.0f);
  assert(simd_vector_equals(scatter_mixed, expected_scatter_mixed,
                            FLOAT_EPSILON) &&
         "Scatter with mixed indices failed");

  // Test scatter with duplicate indices (later writes should overwrite earlier
  // ones)
  VKR_SIMD_F32X4 dup_values = vkr_simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  VKR_SIMD_I32X4 dup_scatter_indices = vkr_simd_set_i32x4(0, 0, 1, 1);
  VKR_SIMD_F32X4 scatter_dup =
      vkr_simd_scatter_f32x4(dup_values, dup_scatter_indices);
  // Expected: position 0 gets overwritten (1.0f then 2.0f), position 1 gets
  // overwritten (3.0f then 4.0f) In our implementation, later indices overwrite
  // earlier ones due to the loop order
  VKR_SIMD_F32X4 expected_scatter_dup =
      vkr_simd_set_f32x4(2.0f, 4.0f, 0.0f, 0.0f);
  assert(simd_vector_equals(scatter_dup, expected_scatter_dup, FLOAT_EPSILON) &&
         "Scatter with duplicate indices failed");

  // Test scatter with out-of-bounds indices (should ignore invalid indices)
  VKR_SIMD_F32X4 oob_values = vkr_simd_set_f32x4(11.0f, 22.0f, 33.0f, 44.0f);
  VKR_SIMD_I32X4 oob_scatter_indices = vkr_simd_set_i32x4(-1, 1, 4, 2);
  VKR_SIMD_F32X4 scatter_oob =
      vkr_simd_scatter_f32x4(oob_values, oob_scatter_indices);
  VKR_SIMD_F32X4 expected_scatter_oob =
      vkr_simd_set_f32x4(0.0f, 22.0f, 44.0f, 0.0f);
  assert(simd_vector_equals(scatter_oob, expected_scatter_oob, FLOAT_EPSILON) &&
         "Scatter with out-of-bounds indices failed");

  printf("  test_simd_scatter_gather PASSED\n");
}

static void test_simd_scatter_gather_edge_cases(void) {
  printf("  Running test_simd_scatter_gather_edge_cases...\n");

  // Test with zero values
  VKR_SIMD_F32X4 zero_values = vkr_simd_set1_f32x4(0.0f);
  VKR_SIMD_I32X4 valid_indices = vkr_simd_set_i32x4(0, 1, 2, 3);

  VKR_SIMD_F32X4 gather_zero =
      vkr_simd_gather_f32x4(zero_values, valid_indices);
  assert(simd_vector_equals(gather_zero, zero_values, FLOAT_EPSILON) &&
         "Gather with zero values failed");

  VKR_SIMD_F32X4 scatter_zero =
      vkr_simd_scatter_f32x4(zero_values, valid_indices);
  assert(simd_vector_equals(scatter_zero, zero_values, FLOAT_EPSILON) &&
         "Scatter with zero values failed");

  // Test with negative values
  VKR_SIMD_F32X4 negative_values =
      vkr_simd_set_f32x4(-1.0f, -2.0f, -3.0f, -4.0f);
  VKR_SIMD_F32X4 gather_negative =
      vkr_simd_gather_f32x4(negative_values, valid_indices);
  assert(simd_vector_equals(gather_negative, negative_values, FLOAT_EPSILON) &&
         "Gather with negative values failed");

  VKR_SIMD_F32X4 scatter_negative =
      vkr_simd_scatter_f32x4(negative_values, valid_indices);
  assert(simd_vector_equals(scatter_negative, negative_values, FLOAT_EPSILON) &&
         "Scatter with negative values failed");

  // Test with large values
  VKR_SIMD_F32X4 large_values =
      vkr_simd_set_f32x4(1000000.0f, 2000000.0f, 3000000.0f, 4000000.0f);
  VKR_SIMD_F32X4 gather_large =
      vkr_simd_gather_f32x4(large_values, valid_indices);
  assert(simd_vector_equals(gather_large, large_values, 1.0f) &&
         "Gather with large values failed");

  VKR_SIMD_F32X4 scatter_large =
      vkr_simd_scatter_f32x4(large_values, valid_indices);
  assert(simd_vector_equals(scatter_large, large_values, 1.0f) &&
         "Scatter with large values failed");

  // Test round-trip consistency: scatter then gather should preserve data (with
  // identity indices)
  VKR_SIMD_F32X4 original = vkr_simd_set_f32x4(7.5f, 8.25f, 9.75f, 10.125f);
  VKR_SIMD_F32X4 scattered = vkr_simd_scatter_f32x4(original, valid_indices);
  VKR_SIMD_F32X4 gathered = vkr_simd_gather_f32x4(scattered, valid_indices);
  assert(simd_vector_equals(gathered, original, FLOAT_EPSILON) &&
         "Scatter-gather round-trip failed");

  // Test gather then scatter round-trip with permutation
  VKR_SIMD_I32X4 perm_indices = vkr_simd_set_i32x4(2, 0, 3, 1);
  VKR_SIMD_F32X4 gathered_perm = vkr_simd_gather_f32x4(original, perm_indices);
  VKR_SIMD_F32X4 scattered_perm =
      vkr_simd_scatter_f32x4(gathered_perm, perm_indices);
  assert(simd_vector_equals(scattered_perm, original, FLOAT_EPSILON) &&
         "Gather-scatter permutation round-trip failed");

  printf("  test_simd_scatter_gather_edge_cases PASSED\n");
}

bool32_t run_simd_tests(void) {
  printf("--- Starting SIMD Tests ---\n");

  // Float SIMD tests
  test_simd_load_store();
  test_simd_set();
  test_simd_arithmetic();
  test_simd_sqrt();
  test_simd_min_max();
  test_simd_fma();
  test_simd_dot_products();
  test_simd_shuffle();
  test_simd_scatter_gather();
  test_simd_scatter_gather_edge_cases();
  test_simd_edge_cases();

  // Integer SIMD tests
  test_simd_i32_set();
  test_simd_i32_arithmetic();
  test_simd_i32_edge_cases();

  printf("--- SIMD Tests Completed ---\n");
  return true;
}