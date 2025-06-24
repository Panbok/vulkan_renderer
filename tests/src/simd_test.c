#include "simd_test.h"

static bool32_t simd_vector_equals(SIMD_F32X4 a, SIMD_F32X4 b,
                                   float32_t epsilon) {
  return (abs_f32(a.x - b.x) < epsilon) && (abs_f32(a.y - b.y) < epsilon) &&
         (abs_f32(a.z - b.z) < epsilon) && (abs_f32(a.w - b.w) < epsilon);
}

static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return abs_f32(a - b) < epsilon;
}

static void test_simd_load_store(void) {
  printf("  Running test_simd_load_store...\n");

  float32_t input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float32_t output[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  SIMD_F32X4 v = simd_load_f32x4(input);

  assert(float_equals(v.x, 1.0f, FLOAT_EPSILON) &&
         "Load failed for x component");
  assert(float_equals(v.y, 2.0f, FLOAT_EPSILON) &&
         "Load failed for y component");
  assert(float_equals(v.z, 3.0f, FLOAT_EPSILON) &&
         "Load failed for z component");
  assert(float_equals(v.w, 4.0f, FLOAT_EPSILON) &&
         "Load failed for w component");

  simd_store_f32x4(output, v);

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

  // Test simd_set_f32x4
  SIMD_F32X4 v1 = simd_set_f32x4(1.5f, 2.5f, 3.5f, 4.5f);
  assert(float_equals(v1.x, 1.5f, FLOAT_EPSILON) &&
         "simd_set_f32x4 failed for x");
  assert(float_equals(v1.y, 2.5f, FLOAT_EPSILON) &&
         "simd_set_f32x4 failed for y");
  assert(float_equals(v1.z, 3.5f, FLOAT_EPSILON) &&
         "simd_set_f32x4 failed for z");
  assert(float_equals(v1.w, 4.5f, FLOAT_EPSILON) &&
         "simd_set_f32x4 failed for w");

  // Test simd_set1_f32x4 (broadcast)
  SIMD_F32X4 v2 = simd_set1_f32x4(7.0f);
  assert(float_equals(v2.x, 7.0f, FLOAT_EPSILON) &&
         "simd_set1_f32x4 failed for x");
  assert(float_equals(v2.y, 7.0f, FLOAT_EPSILON) &&
         "simd_set1_f32x4 failed for y");
  assert(float_equals(v2.z, 7.0f, FLOAT_EPSILON) &&
         "simd_set1_f32x4 failed for z");
  assert(float_equals(v2.w, 7.0f, FLOAT_EPSILON) &&
         "simd_set1_f32x4 failed for w");

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

  SIMD_F32X4 a = simd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
  SIMD_F32X4 b = simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  // Test addition
  SIMD_F32X4 add_result = simd_add_f32x4(a, b);
  SIMD_F32X4 expected_add = simd_set_f32x4(11.0f, 22.0f, 33.0f, 44.0f);
  assert(simd_vector_equals(add_result, expected_add, FLOAT_EPSILON) &&
         "simd_add_f32x4 failed");

  // Test subtraction
  SIMD_F32X4 sub_result = simd_sub_f32x4(a, b);
  SIMD_F32X4 expected_sub = simd_set_f32x4(9.0f, 18.0f, 27.0f, 36.0f);
  assert(simd_vector_equals(sub_result, expected_sub, FLOAT_EPSILON) &&
         "simd_sub_f32x4 failed");

  // Test multiplication
  SIMD_F32X4 mul_result = simd_mul_f32x4(a, b);
  SIMD_F32X4 expected_mul = simd_set_f32x4(10.0f, 40.0f, 90.0f, 160.0f);
  assert(simd_vector_equals(mul_result, expected_mul, FLOAT_EPSILON) &&
         "simd_mul_f32x4 failed");

  // Test division
  SIMD_F32X4 div_result = simd_div_f32x4(a, b);
  SIMD_F32X4 expected_div = simd_set_f32x4(10.0f, 10.0f, 10.0f, 10.0f);
  assert(simd_vector_equals(div_result, expected_div, FLOAT_EPSILON) &&
         "simd_div_f32x4 failed");

  printf("  test_simd_arithmetic PASSED\n");
}

static void test_simd_sqrt(void) {
  printf("  Running test_simd_sqrt...\n");

  SIMD_F32X4 v = simd_set_f32x4(4.0f, 9.0f, 16.0f, 25.0f);

  // Test square root
  SIMD_F32X4 sqrt_result = simd_sqrt_f32x4(v);
  SIMD_F32X4 expected_sqrt = simd_set_f32x4(2.0f, 3.0f, 4.0f, 5.0f);
  assert(simd_vector_equals(sqrt_result, expected_sqrt, FLOAT_EPSILON) &&
         "simd_sqrt_f32x4 failed");

  // Test reciprocal square root
  SIMD_F32X4 rsqrt_result = simd_rsqrt_f32x4(v);
  SIMD_F32X4 expected_rsqrt = simd_set_f32x4(0.5f, 1.0f / 3.0f, 0.25f, 0.2f);
  assert(simd_vector_equals(rsqrt_result, expected_rsqrt, 0.001f) &&
         "simd_rsqrt_f32x4 failed");

  // Test special case: sqrt of 1
  SIMD_F32X4 ones = simd_set1_f32x4(1.0f);
  SIMD_F32X4 sqrt_ones = simd_sqrt_f32x4(ones);
  assert(simd_vector_equals(sqrt_ones, ones, FLOAT_EPSILON) &&
         "sqrt(1) failed");

  // Test special case: rsqrt of 1
  SIMD_F32X4 rsqrt_ones = simd_rsqrt_f32x4(ones);
  assert(simd_vector_equals(rsqrt_ones, ones, 0.001f) && "rsqrt(1) failed");

  printf("  test_simd_sqrt PASSED\n");
}

static void test_simd_min_max(void) {
  printf("  Running test_simd_min_max...\n");

  SIMD_F32X4 a = simd_set_f32x4(1.0f, 5.0f, 2.0f, 8.0f);
  SIMD_F32X4 b = simd_set_f32x4(3.0f, 2.0f, 7.0f, 4.0f);

  // Test minimum
  SIMD_F32X4 min_result = simd_min_f32x4(a, b);
  SIMD_F32X4 expected_min = simd_set_f32x4(1.0f, 2.0f, 2.0f, 4.0f);
  assert(simd_vector_equals(min_result, expected_min, FLOAT_EPSILON) &&
         "simd_min_f32x4 failed");

  // Test maximum
  SIMD_F32X4 max_result = simd_max_f32x4(a, b);
  SIMD_F32X4 expected_max = simd_set_f32x4(3.0f, 5.0f, 7.0f, 8.0f);
  assert(simd_vector_equals(max_result, expected_max, FLOAT_EPSILON) &&
         "simd_max_f32x4 failed");

  // Test with negative values
  SIMD_F32X4 neg_a = simd_set_f32x4(-1.0f, -5.0f, -2.0f, -8.0f);
  SIMD_F32X4 neg_b = simd_set_f32x4(-3.0f, -2.0f, -7.0f, -4.0f);

  SIMD_F32X4 neg_min = simd_min_f32x4(neg_a, neg_b);
  SIMD_F32X4 expected_neg_min = simd_set_f32x4(-3.0f, -5.0f, -7.0f, -8.0f);
  assert(simd_vector_equals(neg_min, expected_neg_min, FLOAT_EPSILON) &&
         "simd_min_f32x4 failed for negative values");

  SIMD_F32X4 neg_max = simd_max_f32x4(neg_a, neg_b);
  SIMD_F32X4 expected_neg_max = simd_set_f32x4(-1.0f, -2.0f, -2.0f, -4.0f);
  assert(simd_vector_equals(neg_max, expected_neg_max, FLOAT_EPSILON) &&
         "simd_max_f32x4 failed for negative values");

  printf("  test_simd_min_max PASSED\n");
}

static void test_simd_fma(void) {
  printf("  Running test_simd_fma...\n");

  SIMD_F32X4 a = simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  SIMD_F32X4 b = simd_set_f32x4(2.0f, 3.0f, 4.0f, 5.0f);
  SIMD_F32X4 c = simd_set_f32x4(3.0f, 4.0f, 5.0f, 6.0f);

  // Test FMA: a + (b * c)
  SIMD_F32X4 fma_result = simd_fma_f32x4(a, b, c);
  SIMD_F32X4 expected_fma =
      simd_set_f32x4(1.0f + (2.0f * 3.0f), 2.0f + (3.0f * 4.0f),
                     3.0f + (4.0f * 5.0f), 4.0f + (5.0f * 6.0f));
  // expected_fma = {7.0f, 14.0f, 23.0f, 34.0f}
  assert(simd_vector_equals(fma_result, expected_fma, FLOAT_EPSILON) &&
         "simd_fma_f32x4 failed");

  // Test FMS: a - (b * c)
  SIMD_F32X4 fms_result = simd_fms_f32x4(a, b, c);
  SIMD_F32X4 expected_fms =
      simd_set_f32x4(1.0f - (2.0f * 3.0f), 2.0f - (3.0f * 4.0f),
                     3.0f - (4.0f * 5.0f), 4.0f - (5.0f * 6.0f));
  // expected_fms = {-5.0f, -10.0f, -17.0f, -26.0f}
  assert(simd_vector_equals(fms_result, expected_fms, FLOAT_EPSILON) &&
         "simd_fms_f32x4 failed");

  // Test FNMA: -(a + b * c)
  SIMD_F32X4 fnma_result = simd_fnma_f32x4(a, b, c);
  SIMD_F32X4 expected_fnma =
      simd_set_f32x4(-(1.0f + 2.0f * 3.0f), -(2.0f + 3.0f * 4.0f),
                     -(3.0f + 4.0f * 5.0f), -(4.0f + 5.0f * 6.0f));
  // expected_fnma = {-7.0f, -14.0f, -23.0f, -34.0f}
  assert(simd_vector_equals(fnma_result, expected_fnma, FLOAT_EPSILON) &&
         "simd_fnma_f32x4 failed");

  // Test FNMS: -(a - b * c)
  SIMD_F32X4 fnms_result = simd_fnms_f32x4(a, b, c);
  SIMD_F32X4 expected_fnms =
      simd_set_f32x4(-(1.0f - 2.0f * 3.0f), -(2.0f - 3.0f * 4.0f),
                     -(3.0f - 4.0f * 5.0f), -(4.0f - 5.0f * 6.0f));
  // expected_fnms = {5.0f, 10.0f, 17.0f, 26.0f}
  assert(simd_vector_equals(fnms_result, expected_fnms, FLOAT_EPSILON) &&
         "simd_fnms_f32x4 failed");

  printf("  test_simd_fma PASSED\n");
}

static void test_simd_dot_products(void) {
  printf("  Running test_simd_dot_products...\n");

  SIMD_F32X4 a = simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  SIMD_F32X4 b = simd_set_f32x4(5.0f, 6.0f, 7.0f, 8.0f);

  // Test horizontal add
  float32_t hadd_result = simd_hadd_f32x4(a);
  float32_t expected_hadd = 1.0f + 2.0f + 3.0f + 4.0f; // 10.0f
  assert(float_equals(hadd_result, expected_hadd, FLOAT_EPSILON) &&
         "simd_hadd_f32x4 failed");

  // Test 4D dot product
  float32_t dot4_result = simd_dot_f32x4(a, b);
  float32_t expected_dot4 =
      (1.0f * 5.0f) + (2.0f * 6.0f) + (3.0f * 7.0f) + (4.0f * 8.0f); // 70.0f
  assert(float_equals(dot4_result, expected_dot4, FLOAT_EPSILON) &&
         "simd_dot_f32x4 failed");

  // Test 3D dot product (ignores w component)
  float32_t dot3_result = simd_dot3_f32x4(a, b);
  float32_t expected_dot3 =
      (1.0f * 5.0f) + (2.0f * 6.0f) + (3.0f * 7.0f); // 38.0f
  assert(float_equals(dot3_result, expected_dot3, FLOAT_EPSILON) &&
         "simd_dot3_f32x4 failed");

  // Test 4D dot product alias
  float32_t dot4_alias_result = simd_dot4_f32x4(a, b);
  assert(float_equals(dot4_alias_result, expected_dot4, FLOAT_EPSILON) &&
         "simd_dot4_f32x4 failed");

  // Test with zero vector
  SIMD_F32X4 zero = simd_set1_f32x4(0.0f);
  float32_t zero_dot = simd_dot_f32x4(a, zero);
  assert(float_equals(zero_dot, 0.0f, FLOAT_EPSILON) &&
         "Dot product with zero vector failed");

  // Test with unit vectors
  SIMD_F32X4 unit_x = simd_set_f32x4(1.0f, 0.0f, 0.0f, 0.0f);
  SIMD_F32X4 unit_y = simd_set_f32x4(0.0f, 1.0f, 0.0f, 0.0f);
  float32_t orthogonal_dot = simd_dot_f32x4(unit_x, unit_y);
  assert(float_equals(orthogonal_dot, 0.0f, FLOAT_EPSILON) &&
         "Orthogonal vectors dot product failed");

  float32_t unit_dot = simd_dot_f32x4(unit_x, unit_x);
  assert(float_equals(unit_dot, 1.0f, FLOAT_EPSILON) &&
         "Unit vector self dot product failed");

  printf("  test_simd_dot_products PASSED\n");
}

static void test_simd_shuffle(void) {
  printf("  Running test_simd_shuffle...\n");

  SIMD_F32X4 v = simd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);

  // Test identity shuffle (0, 1, 2, 3)
  SIMD_F32X4 identity = simd_shuffle_f32x4(v, 0, 1, 2, 3);
  assert(simd_vector_equals(identity, v, FLOAT_EPSILON) &&
         "Identity shuffle failed");

  // Test reverse shuffle (3, 2, 1, 0)
  SIMD_F32X4 reverse = simd_shuffle_f32x4(v, 3, 2, 1, 0);
  SIMD_F32X4 expected_reverse = simd_set_f32x4(40.0f, 30.0f, 20.0f, 10.0f);
  assert(simd_vector_equals(reverse, expected_reverse, FLOAT_EPSILON) &&
         "Reverse shuffle failed");

  // Test broadcast shuffle (0, 0, 0, 0)
  SIMD_F32X4 broadcast = simd_shuffle_f32x4(v, 0, 0, 0, 0);
  SIMD_F32X4 expected_broadcast = simd_set1_f32x4(10.0f);
  assert(simd_vector_equals(broadcast, expected_broadcast, FLOAT_EPSILON) &&
         "Broadcast shuffle failed");

  // Test custom shuffle (1, 3, 0, 2)
  SIMD_F32X4 custom = simd_shuffle_f32x4(v, 1, 3, 0, 2);
  SIMD_F32X4 expected_custom = simd_set_f32x4(20.0f, 40.0f, 10.0f, 30.0f);
  assert(simd_vector_equals(custom, expected_custom, FLOAT_EPSILON) &&
         "Custom shuffle failed");

  printf("  test_simd_shuffle PASSED\n");
}

static void test_simd_edge_cases(void) {
  printf("  Running test_simd_edge_cases...\n");

  // Test with very small values
  SIMD_F32X4 tiny = simd_set1_f32x4(FLOAT_EPSILON);
  SIMD_F32X4 tiny_add = simd_add_f32x4(tiny, tiny);
  SIMD_F32X4 expected_tiny_add = simd_set1_f32x4(2.0f * FLOAT_EPSILON);
  assert(
      simd_vector_equals(tiny_add, expected_tiny_add, FLOAT_EPSILON / 10.0f) &&
      "Addition with tiny values failed");

  // Test with very large values
  SIMD_F32X4 large = simd_set1_f32x4(1000000.0f);
  SIMD_F32X4 large_add = simd_add_f32x4(large, simd_set1_f32x4(1.0f));
  SIMD_F32X4 expected_large_add = simd_set1_f32x4(1000001.0f);
  assert(simd_vector_equals(large_add, expected_large_add, 1.0f) &&
         "Addition with large values failed");

  // Test with mixed positive and negative values
  SIMD_F32X4 mixed = simd_set_f32x4(-10.0f, 5.0f, -2.0f, 8.0f);
  SIMD_F32X4 abs_like =
      simd_max_f32x4(mixed, simd_sub_f32x4(simd_set1_f32x4(0.0f), mixed));
  SIMD_F32X4 expected_abs = simd_set_f32x4(10.0f, 5.0f, 2.0f, 8.0f);
  assert(simd_vector_equals(abs_like, expected_abs, FLOAT_EPSILON) &&
         "Absolute value simulation failed");

  // Test multiplication by zero
  SIMD_F32X4 zero = simd_set1_f32x4(0.0f);
  SIMD_F32X4 any_value = simd_set_f32x4(123.0f, -456.0f, 789.0f, -101112.0f);
  SIMD_F32X4 zero_result = simd_mul_f32x4(any_value, zero);
  assert(simd_vector_equals(zero_result, zero, FLOAT_EPSILON) &&
         "Multiplication by zero failed");

  // Test multiplication by one
  SIMD_F32X4 one = simd_set1_f32x4(1.0f);
  SIMD_F32X4 one_result = simd_mul_f32x4(any_value, one);
  assert(simd_vector_equals(one_result, any_value, FLOAT_EPSILON) &&
         "Multiplication by one failed");

  printf("  test_simd_edge_cases PASSED\n");
}

bool32_t run_simd_tests(void) {
  printf("--- Starting SIMD Tests ---\n");

  test_simd_load_store();
  test_simd_set();
  test_simd_arithmetic();
  test_simd_sqrt();
  test_simd_min_max();
  test_simd_fma();
  test_simd_dot_products();
  test_simd_shuffle();
  test_simd_edge_cases();

  printf("--- SIMD Tests Completed ---\n");
  return true;
}