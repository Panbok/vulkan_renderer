#include "math_utils_test.h"

static bool32_t float_equals(float32_t a, float32_t b, float32_t epsilon) {
  return abs_f32(a - b) < epsilon;
}

static void test_angle_conversion(void) {
  printf("  Running test_angle_conversion...\n");

  // Test degrees to radians
  float32_t deg_90 = 90.0f;
  float32_t rad_90 = to_radians(deg_90);
  assert(float_equals(rad_90, HALF_PI, FLOAT_EPSILON) &&
         "90 degrees to radians conversion failed");

  float32_t deg_180 = 180.0f;
  float32_t rad_180 = to_radians(deg_180);
  assert(float_equals(rad_180, PI, FLOAT_EPSILON) &&
         "180 degrees to radians conversion failed");

  float32_t deg_360 = 360.0f;
  float32_t rad_360 = to_radians(deg_360);
  assert(float_equals(rad_360, PI_2, FLOAT_EPSILON) &&
         "360 degrees to radians conversion failed");

  // Test radians to degrees
  float32_t rad_pi = PI;
  float32_t deg_pi = to_degrees(rad_pi);
  assert(float_equals(deg_pi, 180.0f, FLOAT_EPSILON) &&
         "PI radians to degrees conversion failed");

  float32_t rad_half_pi = HALF_PI;
  float32_t deg_half_pi = to_degrees(rad_half_pi);
  assert(float_equals(deg_half_pi, 90.0f, FLOAT_EPSILON) &&
         "PI/2 radians to degrees conversion failed");

  // Test round-trip conversion
  float32_t original_deg = 45.0f;
  float32_t converted_rad = to_radians(original_deg);
  float32_t back_to_deg = to_degrees(converted_rad);
  assert(float_equals(original_deg, back_to_deg, FLOAT_EPSILON) &&
         "Round-trip conversion failed");

  printf("  test_angle_conversion PASSED\n");
}

static void test_basic_math_operations(void) {
  printf("  Running test_basic_math_operations...\n");

  // Test min_f32
  assert(float_equals(min_f32(5.0f, 3.0f), 3.0f, FLOAT_EPSILON) &&
         "min_f32 failed for 5.0, 3.0");
  assert(float_equals(min_f32(-2.0f, -5.0f), -5.0f, FLOAT_EPSILON) &&
         "min_f32 failed for negative values");
  assert(float_equals(min_f32(1.0f, 1.0f), 1.0f, FLOAT_EPSILON) &&
         "min_f32 failed for equal values");

  // Test max_f32
  assert(float_equals(max_f32(5.0f, 3.0f), 5.0f, FLOAT_EPSILON) &&
         "max_f32 failed for 5.0, 3.0");
  assert(float_equals(max_f32(-2.0f, -5.0f), -2.0f, FLOAT_EPSILON) &&
         "max_f32 failed for negative values");
  assert(float_equals(max_f32(1.0f, 1.0f), 1.0f, FLOAT_EPSILON) &&
         "max_f32 failed for equal values");

  // Test clamp_f32
  assert(float_equals(clamp_f32(5.0f, 0.0f, 10.0f), 5.0f, FLOAT_EPSILON) &&
         "clamp_f32 failed for value in range");
  assert(float_equals(clamp_f32(-5.0f, 0.0f, 10.0f), 0.0f, FLOAT_EPSILON) &&
         "clamp_f32 failed for value below range");
  assert(float_equals(clamp_f32(15.0f, 0.0f, 10.0f), 10.0f, FLOAT_EPSILON) &&
         "clamp_f32 failed for value above range");
  assert(float_equals(clamp_f32(0.0f, 0.0f, 10.0f), 0.0f, FLOAT_EPSILON) &&
         "clamp_f32 failed for min boundary");
  assert(float_equals(clamp_f32(10.0f, 0.0f, 10.0f), 10.0f, FLOAT_EPSILON) &&
         "clamp_f32 failed for max boundary");

  // Test abs_f32
  assert(float_equals(abs_f32(5.0f), 5.0f, FLOAT_EPSILON) &&
         "abs_f32 failed for positive value");
  assert(float_equals(abs_f32(-5.0f), 5.0f, FLOAT_EPSILON) &&
         "abs_f32 failed for negative value");
  assert(float_equals(abs_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "abs_f32 failed for zero");

  // Test sign_f32
  assert(float_equals(sign_f32(5.0f), 1.0f, FLOAT_EPSILON) &&
         "sign_f32 failed for positive value");
  assert(float_equals(sign_f32(-5.0f), -1.0f, FLOAT_EPSILON) &&
         "sign_f32 failed for negative value");
  assert(float_equals(sign_f32(0.0f), 1.0f, FLOAT_EPSILON) &&
         "sign_f32 failed for zero");

  printf("  test_basic_math_operations PASSED\n");
}

static void test_interpolation(void) {
  printf("  Running test_interpolation...\n");

  // Test lerp_f32
  assert(float_equals(lerp_f32(0.0f, 10.0f, 0.0f), 0.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for t=0");
  assert(float_equals(lerp_f32(0.0f, 10.0f, 1.0f), 10.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for t=1");
  assert(float_equals(lerp_f32(0.0f, 10.0f, 0.5f), 5.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for t=0.5");
  assert(float_equals(lerp_f32(10.0f, 20.0f, 0.3f), 13.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for arbitrary values");

  // Test extrapolation
  assert(float_equals(lerp_f32(0.0f, 10.0f, 2.0f), 20.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for extrapolation t=2");
  assert(float_equals(lerp_f32(0.0f, 10.0f, -0.5f), -5.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for extrapolation t=-0.5");

  printf("  test_interpolation PASSED\n");
}

static void test_power_and_root_functions(void) {
  printf("  Running test_power_and_root_functions...\n");

  // Test sqrt_f32
  assert(float_equals(sqrt_f32(4.0f), 2.0f, FLOAT_EPSILON) &&
         "sqrt_f32 failed for 4.0");
  assert(float_equals(sqrt_f32(9.0f), 3.0f, FLOAT_EPSILON) &&
         "sqrt_f32 failed for 9.0");
  assert(float_equals(sqrt_f32(1.0f), 1.0f, FLOAT_EPSILON) &&
         "sqrt_f32 failed for 1.0");
  assert(float_equals(sqrt_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "sqrt_f32 failed for 0.0");
  assert(float_equals(sqrt_f32(2.0f), SQRT_TWO, FLOAT_EPSILON) &&
         "sqrt_f32 failed for sqrt(2)");

  // Test pow_f32
  assert(float_equals(pow_f32(2.0f, 3.0f), 8.0f, FLOAT_EPSILON) &&
         "pow_f32 failed for 2^3");
  assert(float_equals(pow_f32(5.0f, 2.0f), 25.0f, FLOAT_EPSILON) &&
         "pow_f32 failed for 5^2");
  assert(float_equals(pow_f32(10.0f, 0.0f), 1.0f, FLOAT_EPSILON) &&
         "pow_f32 failed for 10^0");
  assert(float_equals(pow_f32(2.0f, 0.5f), sqrt_f32(2.0f), FLOAT_EPSILON) &&
         "pow_f32 failed for 2^0.5");

  // Test exp_f32
  assert(float_equals(exp_f32(0.0f), 1.0f, FLOAT_EPSILON) &&
         "exp_f32 failed for exp(0)");
  assert(float_equals(exp_f32(1.0f), 2.71828182845905f, 0.0001f) &&
         "exp_f32 failed for exp(1)");

  // Test log_f32
  assert(float_equals(log_f32(1.0f), 0.0f, FLOAT_EPSILON) &&
         "log_f32 failed for ln(1)");
  assert(float_equals(log_f32(2.71828182845905f), 1.0f, 0.0001f) &&
         "log_f32 failed for ln(e)");

  printf("  test_power_and_root_functions PASSED\n");
}

static void test_rounding_functions(void) {
  printf("  Running test_rounding_functions...\n");

  // Test floor_f32
  assert(float_equals(floor_f32(3.7f), 3.0f, FLOAT_EPSILON) &&
         "floor_f32 failed for 3.7");
  assert(float_equals(floor_f32(-2.3f), -3.0f, FLOAT_EPSILON) &&
         "floor_f32 failed for -2.3");
  assert(float_equals(floor_f32(5.0f), 5.0f, FLOAT_EPSILON) &&
         "floor_f32 failed for 5.0");
  assert(float_equals(floor_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "floor_f32 failed for 0.0");

  // Test ceil_f32
  assert(float_equals(ceil_f32(3.2f), 4.0f, FLOAT_EPSILON) &&
         "ceil_f32 failed for 3.2");
  assert(float_equals(ceil_f32(-2.7f), -2.0f, FLOAT_EPSILON) &&
         "ceil_f32 failed for -2.7");
  assert(float_equals(ceil_f32(5.0f), 5.0f, FLOAT_EPSILON) &&
         "ceil_f32 failed for 5.0");
  assert(float_equals(ceil_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "ceil_f32 failed for 0.0");

  // Test round_f32
  assert(float_equals(round_f32(3.6f), 4.0f, FLOAT_EPSILON) &&
         "round_f32 failed for 3.6");
  assert(float_equals(round_f32(3.4f), 3.0f, FLOAT_EPSILON) &&
         "round_f32 failed for 3.4");
  assert(float_equals(round_f32(3.5f), 4.0f, FLOAT_EPSILON) &&
         "round_f32 failed for 3.5");
  assert(float_equals(round_f32(-2.6f), -3.0f, FLOAT_EPSILON) &&
         "round_f32 failed for -2.6");
  assert(float_equals(round_f32(-2.4f), -2.0f, FLOAT_EPSILON) &&
         "round_f32 failed for -2.4");

  printf("  test_rounding_functions PASSED\n");
}

static void test_trigonometric_functions(void) {
  printf("  Running test_trigonometric_functions...\n");

  // Test sin_f32
  assert(float_equals(sin_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "sin_f32 failed for 0");
  assert(float_equals(sin_f32(HALF_PI), 1.0f, FLOAT_EPSILON) &&
         "sin_f32 failed for π/2");
  assert(float_equals(sin_f32(PI), 0.0f, 0.0001f) && "sin_f32 failed for π");
  assert(float_equals(sin_f32(PI + HALF_PI), -1.0f, FLOAT_EPSILON) &&
         "sin_f32 failed for 3π/2");

  // Test cos_f32
  assert(float_equals(cos_f32(0.0f), 1.0f, FLOAT_EPSILON) &&
         "cos_f32 failed for 0");
  assert(float_equals(cos_f32(HALF_PI), 0.0f, 0.0001f) &&
         "cos_f32 failed for π/2");
  assert(float_equals(cos_f32(PI), -1.0f, FLOAT_EPSILON) &&
         "cos_f32 failed for π");
  assert(float_equals(cos_f32(PI + HALF_PI), 0.0f, 0.0001f) &&
         "cos_f32 failed for 3π/2");

  // Test tan_f32
  assert(float_equals(tan_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "tan_f32 failed for 0");
  assert(float_equals(tan_f32(QUARTER_PI), 1.0f, FLOAT_EPSILON) &&
         "tan_f32 failed for π/4");
  assert(float_equals(tan_f32(PI), 0.0f, 0.0001f) && "tan_f32 failed for π");

  // Test asin_f32
  assert(float_equals(asin_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "asin_f32 failed for 0");
  assert(float_equals(asin_f32(1.0f), HALF_PI, 0.0001f) &&
         "asin_f32 failed for 1");
  assert(float_equals(asin_f32(-1.0f), -HALF_PI, 0.0001f) &&
         "asin_f32 failed for -1");
  assert(float_equals(asin_f32(0.5f), PI / 6.0f, 0.0001f) &&
         "asin_f32 failed for 0.5");

  // Test acos_f32
  assert(float_equals(acos_f32(1.0f), 0.0f, FLOAT_EPSILON) &&
         "acos_f32 failed for 1");
  assert(float_equals(acos_f32(0.0f), HALF_PI, 0.0001f) &&
         "acos_f32 failed for 0");
  assert(float_equals(acos_f32(-1.0f), PI, 0.0001f) &&
         "acos_f32 failed for -1");
  assert(float_equals(acos_f32(0.5f), PI / 3.0f, 0.0001f) &&
         "acos_f32 failed for 0.5");

  // Test atan_f32
  assert(float_equals(atan_f32(0.0f), 0.0f, FLOAT_EPSILON) &&
         "atan_f32 failed for 0");
  assert(float_equals(atan_f32(1.0f), QUARTER_PI, FLOAT_EPSILON) &&
         "atan_f32 failed for 1");
  assert(float_equals(atan_f32(-1.0f), -QUARTER_PI, FLOAT_EPSILON) &&
         "atan_f32 failed for -1");

  // Test atan2_f32
  assert(float_equals(atan2_f32(1.0f, 1.0f), QUARTER_PI, 0.0001f) &&
         "atan2_f32 failed for (1,1)");
  assert(float_equals(atan2_f32(1.0f, 0.0f), HALF_PI, 0.0001f) &&
         "atan2_f32 failed for (1,0)");
  assert(float_equals(atan2_f32(0.0f, 1.0f), 0.0f, FLOAT_EPSILON) &&
         "atan2_f32 failed for (0,1)");
  assert(float_equals(atan2_f32(-1.0f, -1.0f), -3.0f * QUARTER_PI, 0.0001f) &&
         "atan2_f32 failed for (-1,-1)");

  printf("  test_trigonometric_functions PASSED\n");
}

static void test_random_functions(void) {
  printf("  Running test_random_functions...\n");

  // Test rand_f32 range
  for (int i = 0; i < 100; i++) {
    float32_t r = rand_f32();
    assert(r >= 0.0f && r <= 1.0f &&
           "rand_f32 generated value outside [0,1] range");
  }

  // Test rand_range_f32
  float32_t min_val = 5.0f;
  float32_t max_val = 10.0f;
  for (int i = 0; i < 100; i++) {
    float32_t r = rand_range_f32(min_val, max_val);
    assert(r >= min_val && r <= max_val &&
           "rand_range_f32 generated value outside specified range");
  }

  // Test rand_i32 non-negative
  for (int i = 0; i < 100; i++) {
    int32_t r = rand_i32();
    assert(r >= 0 && "rand_i32 generated negative value");
  }

  // Test rand_range_i32
  int32_t min_int = 1;
  int32_t max_int = 6;
  for (int i = 0; i < 100; i++) {
    int32_t r = rand_range_i32(min_int, max_int);
    assert(r >= min_int && r <= max_int &&
           "rand_range_i32 generated value outside specified range");
  }

  // Test that consecutive calls produce different values (probabilistic test)
  float32_t r1 = rand_f32();
  float32_t r2 = rand_f32();
  float32_t r3 = rand_f32();
  bool32_t all_different = (r1 != r2) && (r2 != r3) && (r1 != r3);
  assert(all_different &&
         "Random number generator appears to be producing identical values");

  printf("  test_random_functions PASSED\n");
}

static void test_edge_cases(void) {
  printf("  Running test_edge_cases...\n");

  // Test with very small values
  float32_t tiny = FLOAT_EPSILON;
  assert(float_equals(abs_f32(tiny), tiny, FLOAT_EPSILON / 10.0f) &&
         "abs_f32 failed for very small positive value");
  assert(float_equals(abs_f32(-tiny), tiny, FLOAT_EPSILON / 10.0f) &&
         "abs_f32 failed for very small negative value");

  // Test with very large values
  float32_t large = 1000000.0f;
  assert(float_equals(min_f32(large, large + 1.0f), large, 1.0f) &&
         "min_f32 failed for large values");
  assert(float_equals(max_f32(large, large + 1.0f), large + 1.0f, 1.0f) &&
         "max_f32 failed for large values");

  // Test clamp with inverted min/max (undefined behavior, but should handle
  // gracefully)
  float32_t clamped = clamp_f32(5.0f, 10.0f, 0.0f);
  // We don't assert specific behavior here since it's undefined, just ensure it
  // doesn't crash
  (void)clamped;

  // Test lerp with equal start and end values
  assert(float_equals(lerp_f32(5.0f, 5.0f, 0.7f), 5.0f, FLOAT_EPSILON) &&
         "lerp_f32 failed for equal start and end values");

  // Test pow with special cases
  assert(float_equals(pow_f32(0.0f, 0.0f), 1.0f, FLOAT_EPSILON) &&
         "pow_f32 failed for 0^0");
  assert(float_equals(pow_f32(1.0f, 1000000.0f), 1.0f, FLOAT_EPSILON) &&
         "pow_f32 failed for 1^large");

  printf("  test_edge_cases PASSED\n");
}

static void test_mathematical_identities(void) {
  printf("  Running test_mathematical_identities...\n");

  // Test Pythagorean identity: sin²(x) + cos²(x) = 1
  float32_t angles[] = {0.0f, QUARTER_PI, HALF_PI, PI, PI + QUARTER_PI};
  for (int i = 0; i < 5; i++) {
    float32_t angle = angles[i];
    float32_t sin_val = sin_f32(angle);
    float32_t cos_val = cos_f32(angle);
    float32_t identity = sin_val * sin_val + cos_val * cos_val;
    assert(float_equals(identity, 1.0f, 0.0001f) &&
           "Pythagorean identity failed");
  }

  // Test that sin(π - x) = sin(x)
  float32_t test_angle = QUARTER_PI;
  float32_t sin_angle = sin_f32(test_angle);
  float32_t sin_supplement = sin_f32(PI - test_angle);
  assert(float_equals(sin_angle, sin_supplement, 0.0001f) &&
         "sin(π - x) = sin(x) identity failed");

  // Test that cos(π - x) = -cos(x)
  float32_t cos_angle = cos_f32(test_angle);
  float32_t cos_supplement = cos_f32(PI - test_angle);
  assert(float_equals(cos_angle, -cos_supplement, 0.0001f) &&
         "cos(π - x) = -cos(x) identity failed");

  // Test inverse function relationships
  float32_t test_val = 0.7f;
  assert(float_equals(sin_f32(asin_f32(test_val)), test_val, 0.0001f) &&
         "sin(asin(x)) = x identity failed");
  assert(float_equals(cos_f32(acos_f32(test_val)), test_val, 0.0001f) &&
         "cos(acos(x)) = x identity failed");
  assert(float_equals(tan_f32(atan_f32(test_val)), test_val, 0.0001f) &&
         "tan(atan(x)) = x identity failed");

  // Test exp/log relationship
  float32_t test_exp = 2.5f;
  assert(float_equals(exp_f32(log_f32(test_exp)), test_exp, 0.0001f) &&
         "exp(ln(x)) = x identity failed");
  assert(float_equals(log_f32(exp_f32(test_exp)), test_exp, 0.0001f) &&
         "ln(exp(x)) = x identity failed");

  // Test sqrt/pow relationship
  float32_t test_sqrt = 16.0f;
  assert(float_equals(pow_f32(sqrt_f32(test_sqrt), 2.0f), test_sqrt, 0.0001f) &&
         "(√x)² = x identity failed");
  assert(float_equals(sqrt_f32(pow_f32(test_sqrt, 2.0f)), test_sqrt, 0.0001f) &&
         "√(x²) = x identity failed");

  printf("  test_mathematical_identities PASSED\n");
}

bool32_t run_math_utils_tests(void) {
  printf("--- Starting Math Utils Tests ---\n");

  test_angle_conversion();
  test_basic_math_operations();
  test_interpolation();
  test_power_and_root_functions();
  test_rounding_functions();
  test_trigonometric_functions();
  test_random_functions();
  test_edge_cases();
  test_mathematical_identities();

  printf("--- Math Utils Tests Completed ---\n");
  return true;
}