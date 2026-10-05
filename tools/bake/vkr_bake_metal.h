#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include "vkr_bake_geometry.h"
}
#include "vkr_bake_lightmap.h"

/*
 * Metal ray-traced lightmap transport for the bake tools (ADR-087, owner
 * decision 2026-10-05: a GPU baker for M-series). The CPU integrator remains
 * the reference and the path on hosts without Metal ray tracing. The context
 * owns one primitive acceleration structure over the bake scene's world-space
 * triangles; it is created once and borrowed by every dispatch.
 */
struct VkrBakeMetalContext;

/* Whether this host can run Metal ray-traced bakes. */
bool vkr_bake_metal_available();

/* Builds the acceleration structure; the triangles are copied. */
VkrBakeMetalContext *vkr_bake_metal_create(const VkrBakeTriangle *triangles,
                                           uint32_t triangle_count);

void vkr_bake_metal_destroy(VkrBakeMetalContext *context);

/* Throughput measurement: traces `samples` cosine-weighted closest-hit rays
 * from every texel and writes each texel's hit fraction. Reports the GPU time
 * of the dispatches in seconds. */
bool vkr_bake_metal_trace_benchmark(
    VkrBakeMetalContext *context,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t samples,
    uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds);
