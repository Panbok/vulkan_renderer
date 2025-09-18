#include "clock_test.h"

static void test_clock_create(void) {
  printf("  Running test_clock_create...\n");
  VkrClock clock = vkr_clock_create();
  assert(clock.start_time == 0.000000 && "Clock start time is not 0.0");
  assert(clock.elapsed == 0.000000 && "Clock elapsed time is not 0.0");
  printf("  test_clock_create PASSED\n");
}

static void test_clock_start(void) {
  printf("  Running test_clock_start...\n");
  VkrClock clock = vkr_clock_create();
  vkr_clock_start(&clock);
  assert(clock.start_time != 0.000000 && "Clock start time is 0.0");
  printf("  test_clock_start PASSED\n");
}

static void test_clock_stop(void) {
  printf("  Running test_clock_stop...\n");
  VkrClock clock = vkr_clock_create();
  vkr_clock_start(&clock);
  vkr_platform_sleep(1);
  vkr_clock_update(&clock);
  vkr_clock_stop(&clock);
  assert(clock.elapsed != 0.000000 && "Clock elapsed time is 0.0");
  printf("  test_clock_stop PASSED\n");
}

static void test_clock_update_zero_start_time(void) {
  printf("  Running test_clock_update_zero_start_time...\n");
  VkrClock clock = vkr_clock_create();
  vkr_clock_update(&clock);
  assert(clock.elapsed == 0.000000 && "Clock elapsed time is not 0.0");
  printf("  test_clock_update_zero_start_time PASSED\n");
}

static void test_clock_update_non_zero_start_time(void) {
  printf("  Running test_clock_update_non_zero_start_time...\n");
  VkrClock clock = vkr_clock_create();
  vkr_clock_start(&clock);
  vkr_platform_sleep(1);
  vkr_clock_update(&clock);
  assert(clock.elapsed != 0.000000 && "Clock elapsed time is 0.0");
  printf("  test_clock_update_non_zero_start_time PASSED\n");
}

bool32_t run_clock_tests(void) {
  printf("--- Starting Clock Tests ---\n");

  test_clock_create();
  test_clock_start();
  test_clock_stop();
  test_clock_update_zero_start_time();
  test_clock_update_non_zero_start_time();

  printf("--- Clock Tests Completed ---\n");
  return true;
}
