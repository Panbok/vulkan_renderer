#include "renderer/vkr_renderer.h"

VkrGpuStageFlags vkr_gpu_stages_for_buffer_access(VkrBufferAccessFlags access,
                                                  bool8_t is_src) {
  if (access == VKR_BUFFER_ACCESS_NONE) {
    return is_src ? VKR_GPU_STAGE_TOP : VKR_GPU_STAGE_BOTTOM;
  }

  VkrGpuStageFlags stages = VKR_GPU_STAGE_NONE;
  if (access & (VKR_BUFFER_ACCESS_VERTEX | VKR_BUFFER_ACCESS_INDEX)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_VERTEX_INPUT);
  }
  if (access & (VKR_BUFFER_ACCESS_UNIFORM | VKR_BUFFER_ACCESS_STORAGE_READ |
                VKR_BUFFER_ACCESS_STORAGE_WRITE)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_ALL_GRAPHICS |
                                VKR_GPU_STAGE_COMPUTE_SHADER);
  }
  if (access &
      (VKR_BUFFER_ACCESS_TRANSFER_SRC | VKR_BUFFER_ACCESS_TRANSFER_DST)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_TRANSFER);
  }
  if (access & VKR_BUFFER_ACCESS_INDIRECT_READ) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_DRAW_INDIRECT);
  }
  return stages ? stages
                : (VkrGpuStageFlags)(VKR_GPU_STAGE_ALL_GRAPHICS |
                                     VKR_GPU_STAGE_COMPUTE_SHADER |
                                     VKR_GPU_STAGE_TRANSFER);
}

VkrGpuStageFlags vkr_gpu_stages_for_image_access(VkrImageAccessFlags access,
                                                 bool8_t is_src) {
  if (access == VKR_IMAGE_ACCESS_NONE) {
    return is_src ? VKR_GPU_STAGE_TOP : VKR_GPU_STAGE_BOTTOM;
  }

  VkrGpuStageFlags stages = VKR_GPU_STAGE_NONE;
  if (access & (VKR_IMAGE_ACCESS_SAMPLED | VKR_IMAGE_ACCESS_STORAGE_READ |
                VKR_IMAGE_ACCESS_STORAGE_WRITE)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_ALL_GRAPHICS |
                                VKR_GPU_STAGE_COMPUTE_SHADER);
  }
  if (access & VKR_IMAGE_ACCESS_COLOR_ATTACHMENT) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_COLOR_OUTPUT);
  }
  if (access &
      (VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT | VKR_IMAGE_ACCESS_DEPTH_READ_ONLY)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_EARLY_DEPTH |
                                VKR_GPU_STAGE_LATE_DEPTH);
  }
  if (access & VKR_IMAGE_ACCESS_DEPTH_READ_ONLY) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_FRAGMENT_SHADER);
  }
  if (access &
      (VKR_IMAGE_ACCESS_TRANSFER_SRC | VKR_IMAGE_ACCESS_TRANSFER_DST)) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_TRANSFER);
  }
  if (access & VKR_IMAGE_ACCESS_PRESENT) {
    stages = (VkrGpuStageFlags)(stages | VKR_GPU_STAGE_BOTTOM);
  }
  return stages ? stages
                : (VkrGpuStageFlags)(VKR_GPU_STAGE_ALL_GRAPHICS |
                                     VKR_GPU_STAGE_COMPUTE_SHADER |
                                     VKR_GPU_STAGE_TRANSFER);
}

VkrGpuDependency
vkr_gpu_buffer_dependency_default(VkrBufferAccessFlags src_access,
                                  VkrBufferAccessFlags dst_access) {
  const bool8_t src_writes =
      (src_access &
       (VKR_BUFFER_ACCESS_STORAGE_WRITE | VKR_BUFFER_ACCESS_TRANSFER_DST)) != 0;
  return (VkrGpuDependency){
      .src_stages = vkr_gpu_stages_for_buffer_access(src_access, true_v),
      .dst_stages = vkr_gpu_stages_for_buffer_access(dst_access, false_v),
      .visibility =
          src_writes ? VKR_GPU_VISIBILITY_DEVICE : VKR_GPU_VISIBILITY_NONE,
  };
}

VkrGpuDependency
vkr_gpu_image_dependency_default(VkrImageAccessFlags src_access,
                                 VkrImageAccessFlags dst_access) {
  return (VkrGpuDependency){
      .src_stages = vkr_gpu_stages_for_image_access(src_access, true_v),
      .dst_stages = vkr_gpu_stages_for_image_access(dst_access, false_v),
      .visibility = vkr_image_access_is_write(src_access)
                        ? VKR_GPU_VISIBILITY_DEVICE
                        : VKR_GPU_VISIBILITY_NONE,
  };
}
