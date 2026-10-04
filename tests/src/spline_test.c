#include "spline_test.h"

#include "level/vkr_spline.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Spline samples (ADR-084). Oracles
   are curves whose shape is known: a straight line, a circle and the
   control points themselves. */

static bool8_t spline_test_near(float32_t a, float32_t b, float32_t tolerance) {
  return fabsf(a - b) <= tolerance;
}

/* Evenly spaced collinear points give the straight line between the ends:
   its exact length, samples on it at the asked spacing, and the last point
   as the final sample. */
static void spline_test_line(void) {
  printf("  Running spline_test_line...\n");
  const Vec3 points[4] = {
      vec3_new(0.0f, 1.0f, 0.0f), vec3_new(3.0f, 1.0f, 0.0f),
      vec3_new(6.0f, 1.0f, 0.0f), vec3_new(9.0f, 1.0f, 0.0f)};
  assert(vkr_spline_segment_count(4u, false_v) == 3u);
  assert(spline_test_near(vkr_spline_length(points, 4u, false_v), 9.0f, 1e-3f));
  VkrSplineSample samples[16];
  const uint32_t count =
      vkr_spline_sample(points, 4u, false_v, 2.0f, samples, 16u);
  /* 0, 2, 4, 6, 8, then the end at 9. */
  assert(count == 6u);
  for (uint32_t i = 0; i < count; ++i) {
    assert(spline_test_near(samples[i].position.y, 1.0f, 1e-4f));
    assert(spline_test_near(samples[i].position.z, 0.0f, 1e-4f));
    assert(spline_test_near(samples[i].tangent.x, 1.0f, 1e-3f));
  }
  assert(spline_test_near(samples[2].position.x, 4.0f, 1e-3f));
  assert(spline_test_near(samples[5].position.x, 9.0f, 1e-4f));
  assert(spline_test_near(samples[5].distance, 9.0f, 1e-3f));
  /* Too few points or no spacing give nothing. */
  assert(vkr_spline_sample(points, 1u, false_v, 2.0f, samples, 16u) == 0u);
  assert(vkr_spline_sample(points, 4u, false_v, 0.0f, samples, 16u) == 0u);
  printf("  spline_test_line PASSED\n");
}

/* A closed curve through eight points on a circle passes through them,
   stays near the circle between them and has nearly its circumference. */
static void spline_test_circle(void) {
  printf("  Running spline_test_circle...\n");
  const float32_t radius = 10.0f;
  Vec3 points[8];
  for (uint32_t i = 0; i < 8u; ++i) {
    const float32_t angle = (float32_t)i * 6.28318530718f / 8.0f;
    points[i] = vec3_new(radius * cosf(angle), 0.0f, radius * sinf(angle));
  }
  assert(vkr_spline_segment_count(8u, true_v) == 8u);
  for (uint32_t s = 0; s < 8u; ++s) {
    const Vec3 start = vkr_spline_evaluate(points, 8u, true_v, s, 0.0f, NULL);
    const Vec3 end = vkr_spline_evaluate(points, 8u, true_v, s, 1.0f, NULL);
    assert(vec3_length(vec3_sub(start, points[s])) < 1e-3f);
    assert(vec3_length(vec3_sub(end, points[(s + 1u) % 8u])) < 1e-3f);
    const Vec3 middle = vkr_spline_evaluate(points, 8u, true_v, s, 0.5f, NULL);
    assert(spline_test_near(vec3_length(middle), radius, 0.15f));
  }
  assert(spline_test_near(vkr_spline_length(points, 8u, true_v),
                          6.28318530718f * radius, 0.4f));
  /* A closed curve's samples stay a spacing apart around the loop. */
  VkrSplineSample samples[64];
  const uint32_t count =
      vkr_spline_sample(points, 8u, true_v, 2.0f, samples, 64u);
  assert(count >= 31u && count <= 32u);
  for (uint32_t i = 1; i < count; ++i) {
    assert(spline_test_near(
        vec3_length(vec3_sub(samples[i].position, samples[i - 1u].position)),
        2.0f, 0.05f));
  }
  printf("  spline_test_circle PASSED\n");
}

bool32_t run_spline_tests(void) {
  printf("--- Starting Spline Tests ---\n");
  spline_test_line();
  spline_test_circle();
  printf("--- Spline Tests Completed ---\n");
  return true_v;
}
