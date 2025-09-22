#include "math_test.h"

static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return vkr_abs_f32(a - b) < epsilon;
}

static void test_angle_conversion(void) {
  printf("  Running test_angle_conversion...\n");

  // Test degrees to radians
  float32_t deg_90 = 90.0f;
  float32_t rad_90 = vkr_to_radians(deg_90);
  assert(float_equals(rad_90, VKR_HALF_PI, VKR_FLOAT_EPSILON) &&
         "90 degrees to radians conversion failed");

  float32_t deg_180 = 180.0f;
  float32_t rad_180 = vkr_to_radians(deg_180);
  assert(float_equals(rad_180, VKR_PI, VKR_FLOAT_EPSILON) &&
         "180 degrees to radians conversion failed");

  float32_t deg_360 = 360.0f;
  float32_t rad_360 = vkr_to_radians(deg_360);
  assert(float_equals(rad_360, VKR_PI_2, VKR_FLOAT_EPSILON) &&
         "360 degrees to radians conversion failed");

  // Test radians to degrees
  float32_t rad_pi = VKR_PI;
  float32_t deg_pi = vkr_to_degrees(rad_pi);
  assert(float_equals(deg_pi, 180.0f, VKR_FLOAT_EPSILON) &&
         "VKR_PI radians to degrees conversion failed");

  float32_t rad_half_pi = VKR_HALF_PI;
  float32_t deg_half_pi = vkr_to_degrees(rad_half_pi);
  assert(float_equals(deg_half_pi, 90.0f, VKR_FLOAT_EPSILON) &&
         "VKR_PI/2 radians to degrees conversion failed");

  // Test round-trip conversion
  float32_t original_deg = 45.0f;
  float32_t converted_rad = vkr_to_radians(original_deg);
  float32_t back_to_deg = vkr_to_degrees(converted_rad);
  assert(float_equals(original_deg, back_to_deg, VKR_FLOAT_EPSILON) &&
         "Round-trip conversion failed");

  printf("  test_angle_conversion PASSED\n");
}

static void test_basic_math_operations(void) {
  printf("  Running test_basic_math_operations...\n");

  // Test vkr_min_f32
  assert(float_equals(vkr_min_f32(5.0f, 3.0f), 3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_min_f32 failed for 5.0, 3.0");
  assert(float_equals(vkr_min_f32(-2.0f, -5.0f), -5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_min_f32 failed for negative values");
  assert(float_equals(vkr_min_f32(1.0f, 1.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_min_f32 failed for equal values");

  // Test vkr_max_f32
  assert(float_equals(vkr_max_f32(5.0f, 3.0f), 5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_max_f32 failed for 5.0, 3.0");
  assert(float_equals(vkr_max_f32(-2.0f, -5.0f), -2.0f, VKR_FLOAT_EPSILON) &&
         "vkr_max_f32 failed for negative values");
  assert(float_equals(vkr_max_f32(1.0f, 1.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_max_f32 failed for equal values");

  // Test vkr_clamp_f32
  assert(
      float_equals(vkr_clamp_f32(5.0f, 0.0f, 10.0f), 5.0f, VKR_FLOAT_EPSILON) &&
      "vkr_clamp_f32 failed for value in range");
  assert(float_equals(vkr_clamp_f32(-5.0f, 0.0f, 10.0f), 0.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_clamp_f32 failed for value below range");
  assert(float_equals(vkr_clamp_f32(15.0f, 0.0f, 10.0f), 10.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_clamp_f32 failed for value above range");
  assert(
      float_equals(vkr_clamp_f32(0.0f, 0.0f, 10.0f), 0.0f, VKR_FLOAT_EPSILON) &&
      "vkr_clamp_f32 failed for min boundary");
  assert(float_equals(vkr_clamp_f32(10.0f, 0.0f, 10.0f), 10.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_clamp_f32 failed for max boundary");

  // Test vkr_abs_f32
  assert(float_equals(vkr_abs_f32(5.0f), 5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_abs_f32 failed for positive value");
  assert(float_equals(vkr_abs_f32(-5.0f), 5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_abs_f32 failed for negative value");
  assert(float_equals(vkr_abs_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_abs_f32 failed for zero");

  // Test vkr_sign_f32
  assert(float_equals(vkr_sign_f32(5.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sign_f32 failed for positive value");
  assert(float_equals(vkr_sign_f32(-5.0f), -1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sign_f32 failed for negative value");
  assert(float_equals(vkr_sign_f32(0.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sign_f32 failed for zero");

  printf("  test_basic_math_operations PASSED\n");
}

static void test_interpolation(void) {
  printf("  Running test_interpolation...\n");

  // Test vkr_lerp_f32
  assert(
      float_equals(vkr_lerp_f32(0.0f, 10.0f, 0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
      "vkr_lerp_f32 failed for t=0");
  assert(
      float_equals(vkr_lerp_f32(0.0f, 10.0f, 1.0f), 10.0f, VKR_FLOAT_EPSILON) &&
      "vkr_lerp_f32 failed for t=1");
  assert(
      float_equals(vkr_lerp_f32(0.0f, 10.0f, 0.5f), 5.0f, VKR_FLOAT_EPSILON) &&
      "vkr_lerp_f32 failed for t=0.5");
  assert(float_equals(vkr_lerp_f32(10.0f, 20.0f, 0.3f), 13.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_lerp_f32 failed for arbitrary values");

  // Test extrapolation
  assert(
      float_equals(vkr_lerp_f32(0.0f, 10.0f, 2.0f), 20.0f, VKR_FLOAT_EPSILON) &&
      "vkr_lerp_f32 failed for extrapolation t=2");
  assert(float_equals(vkr_lerp_f32(0.0f, 10.0f, -0.5f), -5.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_lerp_f32 failed for extrapolation t=-0.5");

  printf("  test_interpolation PASSED\n");
}

static void test_power_and_root_functions(void) {
  printf("  Running test_power_and_root_functions...\n");

  // Test vkr_sqrt_f32
  assert(float_equals(vkr_sqrt_f32(4.0f), 2.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sqrt_f32 failed for 4.0");
  assert(float_equals(vkr_sqrt_f32(9.0f), 3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sqrt_f32 failed for 9.0");
  assert(float_equals(vkr_sqrt_f32(1.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sqrt_f32 failed for 1.0");
  assert(float_equals(vkr_sqrt_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sqrt_f32 failed for 0.0");
  assert(float_equals(vkr_sqrt_f32(2.0f), VKR_SQRT_TWO, VKR_FLOAT_EPSILON) &&
         "vkr_sqrt_f32 failed for sqrt(2)");

  // Test vkr_pow_f32
  assert(float_equals(vkr_pow_f32(2.0f, 3.0f), 8.0f, VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 2^3");
  assert(float_equals(vkr_pow_f32(5.0f, 2.0f), 25.0f, VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 5^2");
  assert(float_equals(vkr_pow_f32(10.0f, 0.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 10^0");
  assert(float_equals(vkr_pow_f32(2.0f, 0.5f), vkr_sqrt_f32(2.0f),
                      VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 2^0.5");

  // Test vkr_exp_f32
  assert(float_equals(vkr_exp_f32(0.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_exp_f32 failed for exp(0)");
  assert(float_equals(vkr_exp_f32(1.0f), 2.71828182845905f, 0.0001f) &&
         "vkr_exp_f32 failed for exp(1)");

  // Test vkr_log_f32
  assert(float_equals(vkr_log_f32(1.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_log_f32 failed for ln(1)");
  assert(float_equals(vkr_log_f32(2.71828182845905f), 1.0f, 0.0001f) &&
         "vkr_log_f32 failed for ln(e)");

  printf("  test_power_and_root_functions PASSED\n");
}

static void test_rounding_functions(void) {
  printf("  Running test_rounding_functions...\n");

  // Test vkr_floor_f32
  assert(float_equals(vkr_floor_f32(3.7f), 3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_floor_f32 failed for 3.7");
  assert(float_equals(vkr_floor_f32(-2.3f), -3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_floor_f32 failed for -2.3");
  assert(float_equals(vkr_floor_f32(5.0f), 5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_floor_f32 failed for 5.0");
  assert(float_equals(vkr_floor_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_floor_f32 failed for 0.0");

  // Test vkr_ceil_f32
  assert(float_equals(vkr_ceil_f32(3.2f), 4.0f, VKR_FLOAT_EPSILON) &&
         "vkr_ceil_f32 failed for 3.2");
  assert(float_equals(vkr_ceil_f32(-2.7f), -2.0f, VKR_FLOAT_EPSILON) &&
         "vkr_ceil_f32 failed for -2.7");
  assert(float_equals(vkr_ceil_f32(5.0f), 5.0f, VKR_FLOAT_EPSILON) &&
         "vkr_ceil_f32 failed for 5.0");
  assert(float_equals(vkr_ceil_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_ceil_f32 failed for 0.0");

  // Test vkr_round_f32
  assert(float_equals(vkr_round_f32(3.6f), 4.0f, VKR_FLOAT_EPSILON) &&
         "vkr_round_f32 failed for 3.6");
  assert(float_equals(vkr_round_f32(3.4f), 3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_round_f32 failed for 3.4");
  assert(float_equals(vkr_round_f32(3.5f), 4.0f, VKR_FLOAT_EPSILON) &&
         "vkr_round_f32 failed for 3.5");
  assert(float_equals(vkr_round_f32(-2.6f), -3.0f, VKR_FLOAT_EPSILON) &&
         "vkr_round_f32 failed for -2.6");
  assert(float_equals(vkr_round_f32(-2.4f), -2.0f, VKR_FLOAT_EPSILON) &&
         "vkr_round_f32 failed for -2.4");

  printf("  test_rounding_functions PASSED\n");
}

static void test_trigonometric_functions(void) {
  printf("  Running test_trigonometric_functions...\n");

  // Test vkr_sin_f32
  assert(float_equals(vkr_sin_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sin_f32 failed for 0");
  assert(float_equals(vkr_sin_f32(VKR_HALF_PI), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_sin_f32 failed for π/2");
  assert(float_equals(vkr_sin_f32(VKR_PI), 0.0f, 0.0001f) &&
         "vkr_sin_f32 failed for π");
  assert(float_equals(vkr_sin_f32(VKR_PI + VKR_HALF_PI), -1.0f,
                      VKR_FLOAT_EPSILON) &&
         "vkr_sin_f32 failed for 3π/2");

  // Test vkr_cos_f32
  assert(float_equals(vkr_cos_f32(0.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_cos_f32 failed for 0");
  assert(float_equals(vkr_cos_f32(VKR_HALF_PI), 0.0f, 0.0001f) &&
         "vkr_cos_f32 failed for π/2");
  assert(float_equals(vkr_cos_f32(VKR_PI), -1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_cos_f32 failed for π");
  assert(float_equals(vkr_cos_f32(VKR_PI + VKR_HALF_PI), 0.0f, 0.0001f) &&
         "vkr_cos_f32 failed for 3π/2");

  // Test vkr_tan_f32
  assert(float_equals(vkr_tan_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_tan_f32 failed for 0");
  assert(float_equals(vkr_tan_f32(VKR_QUARTER_PI), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_tan_f32 failed for π/4");
  assert(float_equals(vkr_tan_f32(VKR_PI), 0.0f, 0.0001f) &&
         "vkr_tan_f32 failed for π");

  // Test vkr_asin_f32
  assert(float_equals(vkr_asin_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_asin_f32 failed for 0");
  assert(float_equals(vkr_asin_f32(1.0f), VKR_HALF_PI, 0.0001f) &&
         "vkr_asin_f32 failed for 1");
  assert(float_equals(vkr_asin_f32(-1.0f), -VKR_HALF_PI, 0.0001f) &&
         "vkr_asin_f32 failed for -1");
  assert(float_equals(vkr_asin_f32(0.5f), VKR_PI / 6.0f, 0.0001f) &&
         "vkr_asin_f32 failed for 0.5");

  // Test vkr_acos_f32
  assert(float_equals(vkr_acos_f32(1.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_acos_f32 failed for 1");
  assert(float_equals(vkr_acos_f32(0.0f), VKR_HALF_PI, 0.0001f) &&
         "vkr_acos_f32 failed for 0");
  assert(float_equals(vkr_acos_f32(-1.0f), VKR_PI, 0.0001f) &&
         "vkr_acos_f32 failed for -1");
  assert(float_equals(vkr_acos_f32(0.5f), VKR_PI / 3.0f, 0.0001f) &&
         "vkr_acos_f32 failed for 0.5");

  // Test vkr_atan_f32
  assert(float_equals(vkr_atan_f32(0.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_atan_f32 failed for 0");
  assert(float_equals(vkr_atan_f32(1.0f), VKR_QUARTER_PI, VKR_FLOAT_EPSILON) &&
         "vkr_atan_f32 failed for 1");
  assert(
      float_equals(vkr_atan_f32(-1.0f), -VKR_QUARTER_PI, VKR_FLOAT_EPSILON) &&
      "vkr_atan_f32 failed for -1");

  // Test vkr_atan2_f32
  assert(float_equals(vkr_atan2_f32(1.0f, 1.0f), VKR_QUARTER_PI, 0.0001f) &&
         "vkr_atan2_f32 failed for (1,1)");
  assert(float_equals(vkr_atan2_f32(1.0f, 0.0f), VKR_HALF_PI, 0.0001f) &&
         "vkr_atan2_f32 failed for (1,0)");
  assert(float_equals(vkr_atan2_f32(0.0f, 1.0f), 0.0f, VKR_FLOAT_EPSILON) &&
         "vkr_atan2_f32 failed for (0,1)");
  assert(float_equals(vkr_atan2_f32(-1.0f, -1.0f), -3.0f * VKR_QUARTER_PI,
                      0.0001f) &&
         "vkr_atan2_f32 failed for (-1,-1)");

  printf("  test_trigonometric_functions PASSED\n");
}

static void test_random_functions(void) {
  printf("  Running test_random_functions...\n");

  // Test vkr_rand_f32 range
  for (int i = 0; i < 100; i++) {
    float32_t r = vkr_rand_f32();
    assert(r >= 0.0f && r <= 1.0f &&
           "vkr_rand_f32 generated value outside [0,1] range");
  }

  // Test vkr_rand_range_f32
  float32_t min_val = 5.0f;
  float32_t max_val = 10.0f;
  for (int i = 0; i < 100; i++) {
    float32_t r = vkr_rand_range_f32(min_val, max_val);
    assert(r >= min_val && r <= max_val &&
           "vkr_rand_range_f32 generated value outside specified range");
  }

  // Test vkr_rand_i32 non-negative
  for (int i = 0; i < 100; i++) {
    int32_t r = vkr_rand_i32();
    assert(r >= 0 && "vkr_rand_i32 generated negative value");
  }

  // Test vkr_rand_range_i32
  int32_t min_int = 1;
  int32_t max_int = 6;
  for (int i = 0; i < 100; i++) {
    int32_t r = vkr_rand_range_i32(min_int, max_int);
    assert(r >= min_int && r <= max_int &&
           "vkr_rand_range_i32 generated value outside specified range");
  }

  // Test that consecutive calls produce different values (probabilistic test)
  float32_t r1 = vkr_rand_f32();
  float32_t r2 = vkr_rand_f32();
  float32_t r3 = vkr_rand_f32();
  bool32_t all_different = (r1 != r2) && (r2 != r3) && (r1 != r3);
  assert(all_different &&
         "Random number generator appears to be producing identical values");

  printf("  test_random_functions PASSED\n");
}

static void test_edge_cases(void) {
  printf("  Running test_edge_cases...\n");

  // Test with very small values
  float32_t tiny = VKR_FLOAT_EPSILON;
  assert(float_equals(vkr_abs_f32(tiny), tiny, VKR_FLOAT_EPSILON / 10.0f) &&
         "vkr_abs_f32 failed for very small positive value");
  assert(float_equals(vkr_abs_f32(-tiny), tiny, VKR_FLOAT_EPSILON / 10.0f) &&
         "vkr_abs_f32 failed for very small negative value");

  // Test with very large values
  float32_t large = 1000000.0f;
  assert(float_equals(vkr_min_f32(large, large + 1.0f), large, 1.0f) &&
         "vkr_min_f32 failed for large values");
  assert(float_equals(vkr_max_f32(large, large + 1.0f), large + 1.0f, 1.0f) &&
         "vkr_max_f32 failed for large values");

  // Test clamp with inverted min/max (undefined behavior, but should handle
  // gracefully)
  float32_t clamped = vkr_clamp_f32(5.0f, 10.0f, 0.0f);
  // We don't assert specific behavior here since it's undefined, just ensure it
  // doesn't crash
  (void)clamped;

  // Test lerp with equal start and end values
  assert(
      float_equals(vkr_lerp_f32(5.0f, 5.0f, 0.7f), 5.0f, VKR_FLOAT_EPSILON) &&
      "vkr_lerp_f32 failed for equal start and end values");

  // Test pow with special cases
  assert(float_equals(vkr_pow_f32(0.0f, 0.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 0^0");
  assert(float_equals(vkr_pow_f32(1.0f, 1000000.0f), 1.0f, VKR_FLOAT_EPSILON) &&
         "vkr_pow_f32 failed for 1^large");

  printf("  test_edge_cases PASSED\n");
}

static void test_mathematical_identities(void) {
  printf("  Running test_mathematical_identities...\n");

  // Test Pythagorean identity: sin²(x) + cos²(x) = 1
  float32_t angles[] = {0.0f, VKR_QUARTER_PI, VKR_HALF_PI, VKR_PI,
                        VKR_PI + VKR_QUARTER_PI};
  for (int i = 0; i < 5; i++) {
    float32_t angle = angles[i];
    float32_t sin_val = vkr_sin_f32(angle);
    float32_t cos_val = vkr_cos_f32(angle);
    float32_t identity = sin_val * sin_val + cos_val * cos_val;
    assert(float_equals(identity, 1.0f, 0.0001f) &&
           "Pythagorean identity failed");
  }

  // Test that sin(π - x) = sin(x)
  float32_t test_angle = VKR_QUARTER_PI;
  float32_t sin_angle = vkr_sin_f32(test_angle);
  float32_t sin_supplement = vkr_sin_f32(VKR_PI - test_angle);
  assert(float_equals(sin_angle, sin_supplement, 0.0001f) &&
         "sin(π - x) = sin(x) identity failed");

  // Test that cos(π - x) = -cos(x)
  float32_t cos_angle = vkr_cos_f32(test_angle);
  float32_t cos_supplement = vkr_cos_f32(VKR_PI - test_angle);
  assert(float_equals(cos_angle, -cos_supplement, 0.0001f) &&
         "cos(π - x) = -cos(x) identity failed");

  // Test inverse function relationships
  float32_t test_val = 0.7f;
  assert(float_equals(vkr_sin_f32(vkr_asin_f32(test_val)), test_val, 0.0001f) &&
         "sin(asin(x)) = x identity failed");
  assert(float_equals(vkr_cos_f32(vkr_acos_f32(test_val)), test_val, 0.0001f) &&
         "cos(acos(x)) = x identity failed");
  assert(float_equals(vkr_tan_f32(vkr_atan_f32(test_val)), test_val, 0.0001f) &&
         "tan(atan(x)) = x identity failed");

  // Test exp/log relationship
  float32_t test_exp = 2.5f;
  assert(float_equals(vkr_exp_f32(vkr_log_f32(test_exp)), test_exp, 0.0001f) &&
         "exp(ln(x)) = x identity failed");
  assert(float_equals(vkr_log_f32(vkr_exp_f32(test_exp)), test_exp, 0.0001f) &&
         "ln(exp(x)) = x identity failed");

  // Test sqrt/pow relationship
  float32_t test_sqrt = 16.0f;
  assert(float_equals(vkr_pow_f32(vkr_sqrt_f32(test_sqrt), 2.0f), test_sqrt,
                      0.0001f) &&
         "(√x)² = x identity failed");
  assert(float_equals(vkr_sqrt_f32(vkr_pow_f32(test_sqrt, 2.0f)), test_sqrt,
                      0.0001f) &&
         "√(x²) = x identity failed");

  printf("  test_mathematical_identities PASSED\n");
}

bool32_t run_math_tests(void) {
  printf("--- Starting Math Tests ---\n");

  test_angle_conversion();
  test_basic_math_operations();
  test_interpolation();
  test_power_and_root_functions();
  test_rounding_functions();
  test_trigonometric_functions();
  test_random_functions();
  test_edge_cases();
  test_mathematical_identities();

  printf("--- Math Tests Completed ---\n");
  return true;
}