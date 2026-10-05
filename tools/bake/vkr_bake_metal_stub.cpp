#include "bake/vkr_bake_metal.h"

/* Hosts without Metal bake on the CPU integrator. */

bool vkr_bake_metal_available() { return false; }

VkrBakeMetalContext *vkr_bake_metal_create(const VkrBakeTriangle *triangles,
                                           uint32_t triangle_count) {
  (void)triangles;
  (void)triangle_count;
  return nullptr;
}

void vkr_bake_metal_destroy(VkrBakeMetalContext *context) { (void)context; }

bool vkr_bake_metal_trace_benchmark(
    VkrBakeMetalContext *context,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t samples,
    uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds) {
  (void)context;
  (void)texels;
  (void)samples;
  (void)seed;
  (void)out_hit_fraction;
  (void)out_gpu_seconds;
  return false;
}
