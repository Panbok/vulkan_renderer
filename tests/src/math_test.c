#include "math_test.h"
#include "renderer/vkr_color_transfer.h"

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

  // Test vkr_min_i32
  assert(vkr_min_i32(5, 3) == 3 && "vkr_min_i32 failed for 5, 3");
  assert(vkr_min_i32(-2, -5) == -5 && "vkr_min_i32 failed for negative values");
  assert(vkr_min_i32(1, 1) == 1 && "vkr_min_i32 failed for equal values");

  // Test vkr_max_i32
  assert(vkr_max_i32(5, 3) == 5 && "vkr_max_i32 failed for 5, 3");
  assert(vkr_max_i32(-2, -5) == -2 && "vkr_max_i32 failed for negative values");
  assert(vkr_max_i32(1, 1) == 1 && "vkr_max_i32 failed for equal values");

  // Test vkr_min_u32
  assert(vkr_min_u32(5, 3) == 3 && "vkr_min_u32 failed for 5, 3");
  assert(vkr_min_u32(2, 5) == 2 && "vkr_min_u32 failed for 2, 5");
  assert(vkr_min_u32(1, 1) == 1 && "vkr_min_u32 failed for equal values");

  // Test vkr_max_u32
  assert(vkr_max_u32(5, 3) == 5 && "vkr_max_u32 failed for 5, 3");
  assert(vkr_max_u32(2, 5) == 5 && "vkr_max_u32 failed for 2, 5");
  assert(vkr_max_u32(1, 1) == 1 && "vkr_max_u32 failed for equal values");

  // Test vkr_min_u64
  assert(vkr_min_u64(5, 3) == 3 && "vkr_min_u64 failed for 5, 3");
  assert(vkr_min_u64(2, 5) == 2 && "vkr_min_u64 failed for 2, 5");
  assert(vkr_min_u64(1, 1) == 1 && "vkr_min_u64 failed for equal values");

  // Test vkr_max_u64
  assert(vkr_max_u64(5, 3) == 5 && "vkr_max_u64 failed for 5, 3");
  assert(vkr_max_u64(2, 5) == 5 && "vkr_max_u64 failed for 2, 5");
  assert(vkr_max_u64(1, 1) == 1 && "vkr_max_u64 failed for equal values");

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

static void test_srgb_transfer(void) {
  printf("  Running test_srgb_transfer...\n");

  assert(float_equals(vkr_srgb_to_linear(0.0f), 0.0f, 1.0e-7f));
  assert(float_equals(vkr_srgb_to_linear(0.02f), 0.001547988f, 1.0e-7f));
  assert(float_equals(vkr_srgb_to_linear(0.04045f), 0.003130805f, 1.0e-7f));
  assert(float_equals(vkr_srgb_to_linear(0.5f), 0.21404114f, 1.0e-6f));
  assert(float_equals(vkr_srgb_to_linear(1.0f), 1.0f, 1.0e-7f));

  assert(float_equals(vkr_linear_to_srgb(0.0f), 0.0f, 1.0e-7f));
  assert(float_equals(vkr_linear_to_srgb(0.001f), 0.01292f, 1.0e-7f));
  assert(float_equals(vkr_linear_to_srgb(0.0031308f), 0.040449936f, 1.0e-7f));
  assert(float_equals(vkr_linear_to_srgb(0.18f), 0.46135613f, 1.0e-6f));
  assert(float_equals(vkr_linear_to_srgb(1.0f), 1.0f, 1.0e-7f));

  const Vec4 decoded =
      vkr_srgb_color_to_linear(vec4_new(0.5f, 0.02f, 1.0f, 0.25f));
  assert(float_equals(decoded.x, 0.21404114f, 1.0e-6f));
  assert(float_equals(decoded.y, 0.001547988f, 1.0e-7f));
  assert(float_equals(decoded.z, 1.0f, 1.0e-7f));
  assert(float_equals(decoded.w, 0.25f, 1.0e-7f));

  printf("  test_srgb_transfer PASSED\n");
}

bool32_t run_math_tests(void) {
  printf("--- Starting Math Tests ---\n");

  test_angle_conversion();
  test_basic_math_operations();
  test_interpolation();
  test_srgb_transfer();

  printf("--- Math Tests Completed ---\n");
  return true;
}