#include "renderer/metal/vkr_metal_dependency.h"

#import <Metal/Metal.h>

vkr_internal MTLStages vkr_metal_lower_stages(VkrGpuStageFlags stages) {
  MTLStages result = 0;
  if (stages & VKR_GPU_STAGE_ALL_GRAPHICS) {
    result |= MTLStageVertex | MTLStageFragment | MTLStageTile |
              MTLStageObject | MTLStageMesh;
  }
  if (stages & (VKR_GPU_STAGE_VERTEX_INPUT | VKR_GPU_STAGE_VERTEX_SHADER)) {
    result |= MTLStageVertex;
  }
  if (stages & VKR_GPU_STAGE_DRAW_INDIRECT)
    result |= MTLStageVertex | MTLStageDispatch;
  if (stages & (VKR_GPU_STAGE_FRAGMENT_SHADER | VKR_GPU_STAGE_EARLY_DEPTH |
                VKR_GPU_STAGE_LATE_DEPTH | VKR_GPU_STAGE_COLOR_OUTPUT)) {
    result |= MTLStageFragment;
  }
  if (stages & VKR_GPU_STAGE_COMPUTE_SHADER) {
    result |= MTLStageDispatch;
  }
  if (stages & VKR_GPU_STAGE_TRANSFER) {
    result |= MTLStageBlit;
  }
  if (stages &
      (VKR_GPU_STAGE_HOST | VKR_GPU_STAGE_TOP | VKR_GPU_STAGE_BOTTOM)) {
    result |= MTLStageAll;
  }
  return result;
}

bool8_t vkr_metal_dependency_lower(const VkrGpuDependency *dependency,
                                   VkrMetalDependency *out_dependency) {
  if (!dependency || !out_dependency) {
    return false_v;
  }

  const MTLStages src = vkr_metal_lower_stages(dependency->src_stages);
  const MTLStages dst = vkr_metal_lower_stages(dependency->dst_stages);
  if (!src || !dst) {
    return false_v;
  }

  MTL4VisibilityOptions visibility = MTL4VisibilityOptionNone;
  if (dependency->visibility & VKR_GPU_VISIBILITY_DEVICE) {
    visibility |= MTL4VisibilityOptionDevice;
  }
  if (dependency->visibility & VKR_GPU_VISIBILITY_RESOURCE_ALIAS) {
    visibility |= MTL4VisibilityOptionResourceAlias;
  }
  *out_dependency = (VkrMetalDependency){
      .src_stages = (uint64_t)src,
      .dst_stages = (uint64_t)dst,
      .visibility_options = (uint64_t)visibility,
  };
  return true_v;
}

bool8_t
vkr_metal_dependency_encode_producer(id<MTL4CommandEncoder> encoder,
                                     const VkrMetalDependency *dependency) {
  if (!encoder || !dependency) {
    return false_v;
  }
  [encoder
      barrierAfterStages:(MTLStages)dependency->src_stages
       beforeQueueStages:(MTLStages)dependency->dst_stages
       visibilityOptions:(MTL4VisibilityOptions)dependency->visibility_options];
  return true_v;
}

bool8_t
vkr_metal_dependency_encode_consumer(id<MTL4CommandEncoder> encoder,
                                     const VkrMetalDependency *dependency) {
  if (!encoder || !dependency) {
    return false_v;
  }
  [encoder barrierAfterQueueStages:(MTLStages)dependency->src_stages
                      beforeStages:(MTLStages)dependency->dst_stages
                 visibilityOptions:(MTL4VisibilityOptions)dependency->
                                   visibility_options];
  return true_v;
}

bool8_t
vkr_metal_dependency_encode_intra(id<MTL4CommandEncoder> encoder,
                                  const VkrMetalDependency *dependency) {
  if (!encoder || !dependency) {
    return false_v;
  }
  [encoder barrierAfterEncoderStages:(MTLStages)dependency->src_stages
                 beforeEncoderStages:(MTLStages)dependency->dst_stages
                   visibilityOptions:(MTL4VisibilityOptions)dependency->
                                     visibility_options];
  return true_v;
}
