#include "vkr_shadow.h"

vkr_internal uint64_t vkr_shadow_hash_projection_bytes(const void *data,
                                                       uint64_t size) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const uint8_t *bytes = data;
  for (uint64_t i = 0u; i < size; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

uint64_t vkr_shadow_projection_generation(const Mat4 *projection) {
  return projection
             ? vkr_shadow_hash_projection_bytes(projection, sizeof(*projection))
             : 0u;
}
