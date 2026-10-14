#include "math/vkr_math.h"

/* The C library rand() sequence stays behind these functions so callers of
 * vkr_math.h do not need <stdlib.h>. */

float32_t vkr_rand_f32(void) { return (float32_t)rand() / (float32_t)RAND_MAX; }

void vkr_rand_seed(uint64_t seed) { srand((uint32_t)seed); }

float32_t vkr_rand_range_f32(float32_t min, float32_t max) {
  return min + vkr_rand_f32() * (max - min);
}

int32_t vkr_rand_i32(void) { return rand(); }

int32_t vkr_rand_range_i32(int32_t min, int32_t max) {
  return (vkr_rand_i32() % (max - min + 1)) + min;
}
