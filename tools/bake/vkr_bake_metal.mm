#include "bake/vkr_bake_metal.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <new>

namespace {

/* Texels per command buffer: each buffer stays short so a long bake never
   holds the GPU long enough to trip the system watchdog. */
constexpr uint32_t kTexelsPerDispatch = 65536u;

const char *kKernelSource = R"METAL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

static uint mix_seed(uint x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
  return x ^ (x >> 16);
}

static float random_unit(uint seed) {
  return float(mix_seed(seed) >> 8) * (1.0f / 16777216.0f);
}

/* Cosine-weighted direction about a unit normal (same frame as the CPU
   baker's cosine_direction). */
static float3 cosine_direction(float3 normal, float u1, float u2) {
  float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
  float a = -1.0f / (sign + normal.z);
  float b = normal.x * normal.y * a;
  float3 tangent = float3(1.0f + sign * normal.x * normal.x * a, sign * b,
                          -sign * normal.x);
  float3 bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
  float radius = sqrt(u1);
  float phi = 6.28318530718f * u2;
  return normalize(tangent * (radius * cos(phi)) +
                   bitangent * (radius * sin(phi)) +
                   normal * sqrt(max(0.0f, 1.0f - u1)));
}

struct BenchmarkArgs {
  uint first_texel;
  uint texel_count;
  uint samples;
  uint seed;
};

kernel void lightmap_trace_benchmark(
    device const float4 *positions [[buffer(0)]],
    device const float4 *normals [[buffer(1)]],
    device float *hit_fraction [[buffer(2)]],
    constant BenchmarkArgs &args [[buffer(3)]],
    primitive_acceleration_structure scene [[buffer(4)]],
    uint id [[thread_position_in_grid]]) {
  if (id >= args.texel_count) return;
  uint texel = args.first_texel + id;
  float3 normal = normals[texel].xyz;
  float3 origin = positions[texel].xyz + normal * 1.0e-3f;
  intersector<triangle_data> query;
  uint texel_seed = mix_seed(args.seed ^ mix_seed(texel));
  uint hits = 0u;
  for (uint s = 0u; s < args.samples; ++s) {
    uint seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
    ray r;
    r.origin = origin;
    r.direction = cosine_direction(normal, random_unit(seed),
                                   random_unit(seed ^ 0x68bc21ebu));
    r.min_distance = 0.0f;
    r.max_distance = INFINITY;
    auto hit = query.intersect(r, scene);
    hits += hit.type != intersection_type::none ? 1u : 0u;
  }
  hit_fraction[texel] = float(hits) / float(args.samples);
}
)METAL";

struct BenchmarkArgs {
  uint32_t first_texel;
  uint32_t texel_count;
  uint32_t samples;
  uint32_t seed;
};

} // namespace

struct VkrBakeMetalContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLAccelerationStructure> scene = nil;
  id<MTLComputePipelineState> benchmark = nil;
};

bool vkr_bake_metal_available() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device && device.supportsRaytracing;
  }
}

VkrBakeMetalContext *
vkr_bake_metal_create(const VkrBakeTriangle *triangles,
                      uint32_t triangle_count) {
  @autoreleasepool {
    if (!triangles || triangle_count == 0u)
      return nullptr;
    VkrBakeMetalContext *context = new (std::nothrow) VkrBakeMetalContext();
    if (!context)
      return nullptr;
    context->device = MTLCreateSystemDefaultDevice();
    if (!context->device || !context->device.supportsRaytracing) {
      delete context;
      return nullptr;
    }
    context->queue = [context->device newCommandQueue];

    /* World-space corners, three per triangle, as packed float3. */
    id<MTLBuffer> vertices = [context->device
        newBufferWithLength:(NSUInteger)triangle_count * 9u * sizeof(float)
                    options:MTLResourceStorageModeShared];
    if (!vertices) {
      vkr_bake_metal_destroy(context);
      return nullptr;
    }
    float *corner = static_cast<float *>(vertices.contents);
    for (uint32_t t = 0u; t < triangle_count; ++t) {
      for (uint32_t c = 0u; c < 3u; ++c) {
        const Vec3 position = triangles[t].vertex[c].position;
        *corner++ = position.x;
        *corner++ = position.y;
        *corner++ = position.z;
      }
    }
    MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    geometry.vertexBuffer = vertices;
    geometry.vertexStride = 3u * sizeof(float);
    geometry.vertexFormat = MTLAttributeFormatFloat3;
    geometry.triangleCount = triangle_count;
    geometry.opaque = YES;
    MTLPrimitiveAccelerationStructureDescriptor *descriptor =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    descriptor.geometryDescriptors = @[ geometry ];
    const MTLAccelerationStructureSizes sizes =
        [context->device accelerationStructureSizesWithDescriptor:descriptor];
    context->scene = [context->device
        newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    id<MTLBuffer> scratch = [context->device
        newBufferWithLength:sizes.buildScratchBufferSize
                    options:MTLResourceStorageModePrivate];
    if (!context->scene || !scratch) {
      vkr_bake_metal_destroy(context);
      return nullptr;
    }
    id<MTLCommandBuffer> command = [context->queue commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> encoder =
        [command accelerationStructureCommandEncoder];
    [encoder buildAccelerationStructure:context->scene
                             descriptor:descriptor
                          scratchBuffer:scratch
                    scratchBufferOffset:0u];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr, "Acceleration structure build failed\n");
      vkr_bake_metal_destroy(context);
      return nullptr;
    }
    std::printf("gpu_acceleration_structure_mb=%.1f build_ms=%.1f\n",
                sizes.accelerationStructureSize / 1048576.0,
                (command.GPUEndTime - command.GPUStartTime) * 1000.0);

    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library =
        [context->device newLibraryWithSource:@(kKernelSource)
                                      options:options
                                        error:&error];
    id<MTLFunction> function =
        library ? [library newFunctionWithName:@"lightmap_trace_benchmark"]
                : nil;
    context->benchmark =
        function ? [context->device newComputePipelineStateWithFunction:function
                                                                   error:&error]
                 : nil;
    if (!context->benchmark) {
      std::fprintf(stderr, "Lightmap kernel compile failed: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown");
      vkr_bake_metal_destroy(context);
      return nullptr;
    }
    return context;
  }
}

void vkr_bake_metal_destroy(VkrBakeMetalContext *context) {
  delete context;
}

bool vkr_bake_metal_trace_benchmark(
    VkrBakeMetalContext *context,
    const std::vector<VkrBakeLightmapTexel> &texels, uint32_t samples,
    uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds) {
  @autoreleasepool {
    if (!context || !out_hit_fraction || !out_gpu_seconds || samples == 0u)
      return false;
    const NSUInteger count = texels.size();
    out_hit_fraction->assign(count, 0.0f);
    *out_gpu_seconds = 0.0;
    if (count == 0u)
      return true;
    id<MTLBuffer> positions = [context->device
        newBufferWithLength:count * 4u * sizeof(float)
                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> normals = [context->device
        newBufferWithLength:count * 4u * sizeof(float)
                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> hits =
        [context->device newBufferWithLength:count * sizeof(float)
                                     options:MTLResourceStorageModeShared];
    if (!positions || !normals || !hits)
      return false;
    float *p = static_cast<float *>(positions.contents);
    float *n = static_cast<float *>(normals.contents);
    for (NSUInteger i = 0u; i < count; ++i) {
      const VkrBakeLightmapTexel &texel = texels[i];
      p[4u * i + 0u] = texel.position.x;
      p[4u * i + 1u] = texel.position.y;
      p[4u * i + 2u] = texel.position.z;
      p[4u * i + 3u] = 1.0f;
      n[4u * i + 0u] = texel.normal.x;
      n[4u * i + 1u] = texel.normal.y;
      n[4u * i + 2u] = texel.normal.z;
      n[4u * i + 3u] = 0.0f;
    }
    const NSUInteger width = context->benchmark.threadExecutionWidth;
    for (NSUInteger first = 0u; first < count; first += kTexelsPerDispatch) {
      const BenchmarkArgs args = {
          (uint32_t)first,
          (uint32_t)std::min<NSUInteger>(kTexelsPerDispatch, count - first),
          samples, seed};
      id<MTLCommandBuffer> command = [context->queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:context->benchmark];
      [encoder setBuffer:positions offset:0u atIndex:0u];
      [encoder setBuffer:normals offset:0u atIndex:1u];
      [encoder setBuffer:hits offset:0u atIndex:2u];
      [encoder setBytes:&args length:sizeof(args) atIndex:3u];
      [encoder setAccelerationStructure:context->scene atBufferIndex:4u];
      [encoder useResource:context->scene usage:MTLResourceUsageRead];
      [encoder dispatchThreads:MTLSizeMake(args.texel_count, 1u, 1u)
          threadsPerThreadgroup:MTLSizeMake(width, 1u, 1u)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr, "Lightmap dispatch failed: %s\n",
                     command.error.localizedDescription.UTF8String);
        return false;
      }
      *out_gpu_seconds += command.GPUEndTime - command.GPUStartTime;
    }
    const float *result = static_cast<const float *>(hits.contents);
    std::copy(result, result + count, out_hit_fraction->begin());
    return true;
  }
}
