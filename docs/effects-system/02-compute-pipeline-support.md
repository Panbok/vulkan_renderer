---
status: proposed
updated: 2026-07-31
authority: design
---
# Phase 2: Compute Pipeline Support

## Overview

Add compute shader and compute pipeline support to the Vulkan renderer. This enables GPU-based effects that modify vertex data, perform simulations, or process textures.

## Prerequisites

- Understanding of Vulkan compute pipelines and dispatch
- Familiarity with `vulkan_backend.c` and `vulkan_types.h`
- Understanding of `vkr_pipeline_registry.c` for graphics pipelines

## Current State

From `lib/src/renderer/vkr_renderer.h`:
```c
typedef enum VkrShaderStageBits {
  VKR_SHADER_STAGE_VERTEX_BIT   = 0x01,
  VKR_SHADER_STAGE_FRAGMENT_BIT = 0x02,
  VKR_SHADER_STAGE_COMPUTE_BIT  = 0x04,  // Future
  // ...
} VkrShaderStageBits;
```

Compute is marked "Future" but the bit is defined. Graphics pipelines work; compute does not.

## Design

### Compute Pipeline Description

```c
// lib/src/renderer/vkr_renderer.h - Add new types

/**
 * @brief Description for creating a compute pipeline.
 * @param shader_path Path to compiled compute shader (SPIR-V)
 * @param entry_point Shader entry point name (default: "main" or "computeMain")
 * @param push_constant_size Size of push constants in bytes (0 if none)
 * @param descriptor_layout Descriptor set layout configuration
 */
typedef struct VkrComputePipelineDescription {
  String8 shader_path;
  String8 entry_point;
  uint32_t push_constant_size;

  // Descriptor bindings
  uint32_t storage_buffer_count;    // Read-write storage buffers
  uint32_t uniform_buffer_count;    // Read-only uniform buffers
  uint32_t storage_image_count;     // Read-write images
  uint32_t sampled_image_count;     // Read-only sampled images

  // Specialization constants (optional)
  uint32_t spec_constant_count;
  const VkrSpecializationConstant *spec_constants;
} VkrComputePipelineDescription;

/**
 * @brief Specialization constant for compute shader.
 */
typedef struct VkrSpecializationConstant {
  uint32_t constant_id;
  uint32_t size;
  const void *data;
} VkrSpecializationConstant;

/**
 * @brief Opaque handle to compute pipeline.
 */
typedef struct s_ComputePipeline *VkrComputePipelineHandle;
```

### Storage Buffer Support

```c
// lib/src/renderer/vkr_buffer.h - Extend buffer types

/**
 * @brief Storage buffer for compute shader read-write access.
 * @param size Total buffer size in bytes
 * @param usage How the buffer will be used
 * @param flags Additional buffer flags
 */
typedef struct VkrStorageBuffer {
  uint64_t size;
  VkrBufferUsage usage;
  VkrBufferFlags flags;
} VkrStorageBuffer;

typedef enum VkrBufferUsage {
  VKR_BUFFER_USAGE_VERTEX_SOURCE    = 0x01,  // Source for vertex shader reads
  VKR_BUFFER_USAGE_COMPUTE_STORAGE  = 0x02,  // Read-write in compute
  VKR_BUFFER_USAGE_TRANSFER_SRC     = 0x04,  // Can copy from
  VKR_BUFFER_USAGE_TRANSFER_DST     = 0x08,  // Can copy to
} VkrBufferUsage;

typedef enum VkrBufferFlags {
  VKR_BUFFER_FLAG_HOST_VISIBLE      = 0x01,  // CPU can map
  VKR_BUFFER_FLAG_DEVICE_LOCAL      = 0x02,  // GPU-only (faster)
  VKR_BUFFER_FLAG_HOST_COHERENT     = 0x04,  // No manual flush needed
} VkrBufferFlags;
```

### Dispatch Command

```c
// lib/src/renderer/vkr_renderer.h - Add dispatch info

/**
 * @brief Dispatch parameters for compute shader execution.
 */
typedef struct VkrComputeDispatch {
  uint32_t group_count_x;
  uint32_t group_count_y;
  uint32_t group_count_z;
} VkrComputeDispatch;
```

## Implementation

### Step 1: Vulkan Backend Types

Add to `lib/src/renderer/vulkan/vulkan_types.h`:

```c
/**
 * @brief Internal compute pipeline state.
 */
struct s_ComputePipeline {
  VkPipeline pipeline;
  VkPipelineLayout layout;
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;

  // Per-frame descriptor sets (triple buffered)
  VkDescriptorSet descriptor_sets[BUFFERING_FRAMES];

  // Layout info for binding
  uint32_t storage_buffer_count;
  uint32_t uniform_buffer_count;
  uint32_t storage_image_count;
  uint32_t sampled_image_count;

  uint32_t push_constant_size;
};

/**
 * @brief Storage buffer internal state.
 */
typedef struct VulkanStorageBuffer {
  VkBuffer buffer;
  VkDeviceMemory memory;
  uint64_t size;
  void *mapped_ptr;  // Non-null if host-visible and mapped
  VkrBufferUsage usage;
} VulkanStorageBuffer;
```

### Step 2: Create Compute Pipeline Function

Add to `lib/src/renderer/vulkan/vulkan_compute.h`:

```c
/**
 * @file vulkan_compute.h
 * @brief Vulkan compute pipeline and dispatch operations.
 */
#pragma once

#include "renderer/vkr_renderer.h"
#include "renderer/vulkan/vulkan_types.h"

/**
 * @brief Create a compute pipeline from description.
 * @param state Vulkan backend state
 * @param desc Pipeline description
 * @param out_pipeline Output pipeline handle
 * @param out_error Output error code
 * @return true on success
 */
bool8_t vulkan_compute_pipeline_create(
    VulkanBackendState *state,
    const VkrComputePipelineDescription *desc,
    struct s_ComputePipeline **out_pipeline,
    VkrRendererError *out_error);

/**
 * @brief Destroy a compute pipeline.
 */
void vulkan_compute_pipeline_destroy(
    VulkanBackendState *state,
    struct s_ComputePipeline *pipeline);

/**
 * @brief Bind compute pipeline for dispatch.
 * @param state Vulkan backend state
 * @param command_buffer Command buffer to record into
 * @param pipeline Pipeline to bind
 */
void vulkan_compute_pipeline_bind(
    VulkanBackendState *state,
    VkCommandBuffer command_buffer,
    struct s_ComputePipeline *pipeline);

/**
 * @brief Update descriptor set bindings for compute pipeline.
 * @param state Vulkan backend state
 * @param pipeline Compute pipeline
 * @param frame_index Current frame index (for triple buffering)
 * @param storage_buffers Array of storage buffer handles
 * @param uniform_buffers Array of uniform buffer data pointers
 * @param uniform_sizes Array of uniform buffer sizes
 */
void vulkan_compute_update_descriptors(
    VulkanBackendState *state,
    struct s_ComputePipeline *pipeline,
    uint32_t frame_index,
    const VulkanStorageBuffer *storage_buffers,
    const void **uniform_buffers,
    const uint64_t *uniform_sizes);

/**
 * @brief Dispatch compute shader.
 * @param state Vulkan backend state
 * @param command_buffer Command buffer to record into
 * @param pipeline Bound compute pipeline
 * @param dispatch Dispatch parameters
 */
void vulkan_compute_dispatch(
    VulkanBackendState *state,
    VkCommandBuffer command_buffer,
    struct s_ComputePipeline *pipeline,
    const VkrComputeDispatch *dispatch);

/**
 * @brief Insert memory barrier for compute-to-vertex synchronization.
 * Ensures compute writes are visible to subsequent vertex shader reads.
 */
void vulkan_compute_barrier_to_vertex(
    VkCommandBuffer command_buffer);

/**
 * @brief Insert memory barrier for vertex-to-compute synchronization.
 * Ensures vertex processing is complete before compute reads.
 */
void vulkan_compute_barrier_from_vertex(
    VkCommandBuffer command_buffer);
```

### Step 3: Implement Compute Pipeline Creation

Create `lib/src/renderer/vulkan/vulkan_compute.c`:

```c
#include "renderer/vulkan/vulkan_compute.h"
#include "core/logger.h"
#include "memory/vkr_allocator.h"
#include "renderer/vulkan/vulkan_shaders.h"
#include <string.h>

bool8_t vulkan_compute_pipeline_create(
    VulkanBackendState *state,
    const VkrComputePipelineDescription *desc,
    struct s_ComputePipeline **out_pipeline,
    VkrRendererError *out_error) {

  *out_error = VKR_RENDERER_ERROR_NONE;
  *out_pipeline = NULL;

  // Allocate pipeline structure
  struct s_ComputePipeline *pipeline =
      vkr_dmemory_alloc(&state->dmemory, sizeof(struct s_ComputePipeline),
                        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pipeline) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  memset(pipeline, 0, sizeof(*pipeline));

  pipeline->storage_buffer_count = desc->storage_buffer_count;
  pipeline->uniform_buffer_count = desc->uniform_buffer_count;
  pipeline->storage_image_count = desc->storage_image_count;
  pipeline->sampled_image_count = desc->sampled_image_count;
  pipeline->push_constant_size = desc->push_constant_size;

  // --- Create Descriptor Set Layout ---
  uint32_t binding_count = desc->storage_buffer_count +
                           desc->uniform_buffer_count +
                           desc->storage_image_count +
                           desc->sampled_image_count;

  VkDescriptorSetLayoutBinding *bindings = NULL;
  if (binding_count > 0) {
    bindings = vkr_dmemory_alloc(&state->dmemory,
                                  sizeof(VkDescriptorSetLayoutBinding) * binding_count,
                                  VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!bindings) {
      vkr_dmemory_free(&state->dmemory, pipeline);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    uint32_t binding_index = 0;

    // Storage buffers: binding 0..N-1
    for (uint32_t i = 0; i < desc->storage_buffer_count; i++) {
      bindings[binding_index++] = (VkDescriptorSetLayoutBinding){
          .binding = i,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = NULL,
      };
    }

    // Uniform buffers: binding N..N+M-1
    uint32_t uniform_base = desc->storage_buffer_count;
    for (uint32_t i = 0; i < desc->uniform_buffer_count; i++) {
      bindings[binding_index++] = (VkDescriptorSetLayoutBinding){
          .binding = uniform_base + i,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = NULL,
      };
    }

    // Storage images
    uint32_t storage_img_base = uniform_base + desc->uniform_buffer_count;
    for (uint32_t i = 0; i < desc->storage_image_count; i++) {
      bindings[binding_index++] = (VkDescriptorSetLayoutBinding){
          .binding = storage_img_base + i,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = NULL,
      };
    }

    // Sampled images
    uint32_t sampled_img_base = storage_img_base + desc->storage_image_count;
    for (uint32_t i = 0; i < desc->sampled_image_count; i++) {
      bindings[binding_index++] = (VkDescriptorSetLayoutBinding){
          .binding = sampled_img_base + i,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = NULL,
      };
    }
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = binding_count,
      .pBindings = bindings,
  };

  VkResult result = vkCreateDescriptorSetLayout(
      state->device.logical_device, &layout_info, state->allocator,
      &pipeline->descriptor_layout);

  if (bindings) {
    vkr_dmemory_free(&state->dmemory, bindings);
  }

  if (result != VK_SUCCESS) {
    vkr_dmemory_free(&state->dmemory, pipeline);
    *out_error = VKR_RENDERER_ERROR_VULKAN_INTERNAL;
    return false_v;
  }

  // --- Create Pipeline Layout ---
  VkPushConstantRange push_range = {0};
  if (desc->push_constant_size > 0) {
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = desc->push_constant_size;
  }

  VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &pipeline->descriptor_layout,
      .pushConstantRangeCount = desc->push_constant_size > 0 ? 1 : 0,
      .pPushConstantRanges = desc->push_constant_size > 0 ? &push_range : NULL,
  };

  result = vkCreatePipelineLayout(state->device.logical_device,
                                   &pipeline_layout_info, state->allocator,
                                   &pipeline->layout);
  if (result != VK_SUCCESS) {
    vkDestroyDescriptorSetLayout(state->device.logical_device,
                                  pipeline->descriptor_layout, state->allocator);
    vkr_dmemory_free(&state->dmemory, pipeline);
    *out_error = VKR_RENDERER_ERROR_VULKAN_INTERNAL;
    return false_v;
  }

  // --- Load Compute Shader ---
  VkShaderModule shader_module = VK_NULL_HANDLE;
  if (!vulkan_shader_create_module(state, desc->shader_path, &shader_module)) {
    vkDestroyPipelineLayout(state->device.logical_device, pipeline->layout,
                             state->allocator);
    vkDestroyDescriptorSetLayout(state->device.logical_device,
                                  pipeline->descriptor_layout, state->allocator);
    vkr_dmemory_free(&state->dmemory, pipeline);
    *out_error = VKR_RENDERER_ERROR_SHADER_COMPILATION_FAILED;
    return false_v;
  }

  // --- Create Compute Pipeline ---
  const char *entry_point = desc->entry_point.str && desc->entry_point.length > 0
                                ? (const char *)desc->entry_point.str
                                : "main";

  VkComputePipelineCreateInfo compute_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
          {
              .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
              .module = shader_module,
              .pName = entry_point,
              .pSpecializationInfo = NULL,  // TODO: specialization constants
          },
      .layout = pipeline->layout,
  };

  result = vkCreateComputePipelines(state->device.logical_device,
                                     VK_NULL_HANDLE,  // pipeline cache
                                     1, &compute_info, state->allocator,
                                     &pipeline->pipeline);

  vkDestroyShaderModule(state->device.logical_device, shader_module,
                         state->allocator);

  if (result != VK_SUCCESS) {
    vkDestroyPipelineLayout(state->device.logical_device, pipeline->layout,
                             state->allocator);
    vkDestroyDescriptorSetLayout(state->device.logical_device,
                                  pipeline->descriptor_layout, state->allocator);
    vkr_dmemory_free(&state->dmemory, pipeline);
    *out_error = VKR_RENDERER_ERROR_VULKAN_INTERNAL;
    return false_v;
  }

  // --- Create Descriptor Pool and Sets ---
  if (binding_count > 0) {
    VkDescriptorPoolSize pool_sizes[4];
    uint32_t pool_size_count = 0;

    if (desc->storage_buffer_count > 0) {
      pool_sizes[pool_size_count++] = (VkDescriptorPoolSize){
          .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = desc->storage_buffer_count * BUFFERING_FRAMES,
      };
    }
    if (desc->uniform_buffer_count > 0) {
      pool_sizes[pool_size_count++] = (VkDescriptorPoolSize){
          .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = desc->uniform_buffer_count * BUFFERING_FRAMES,
      };
    }
    if (desc->storage_image_count > 0) {
      pool_sizes[pool_size_count++] = (VkDescriptorPoolSize){
          .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = desc->storage_image_count * BUFFERING_FRAMES,
      };
    }
    if (desc->sampled_image_count > 0) {
      pool_sizes[pool_size_count++] = (VkDescriptorPoolSize){
          .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = desc->sampled_image_count * BUFFERING_FRAMES,
      };
    }

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = BUFFERING_FRAMES,
        .poolSizeCount = pool_size_count,
        .pPoolSizes = pool_sizes,
    };

    result = vkCreateDescriptorPool(state->device.logical_device, &pool_info,
                                     state->allocator, &pipeline->descriptor_pool);
    if (result != VK_SUCCESS) {
      vulkan_compute_pipeline_destroy(state, pipeline);
      *out_error = VKR_RENDERER_ERROR_VULKAN_INTERNAL;
      return false_v;
    }

    // Allocate descriptor sets
    VkDescriptorSetLayout layouts[BUFFERING_FRAMES];
    for (uint32_t i = 0; i < BUFFERING_FRAMES; i++) {
      layouts[i] = pipeline->descriptor_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pipeline->descriptor_pool,
        .descriptorSetCount = BUFFERING_FRAMES,
        .pSetLayouts = layouts,
    };

    result = vkAllocateDescriptorSets(state->device.logical_device, &alloc_info,
                                       pipeline->descriptor_sets);
    if (result != VK_SUCCESS) {
      vulkan_compute_pipeline_destroy(state, pipeline);
      *out_error = VKR_RENDERER_ERROR_VULKAN_INTERNAL;
      return false_v;
    }
  }

  *out_pipeline = pipeline;
  return true_v;
}

void vulkan_compute_pipeline_destroy(VulkanBackendState *state,
                                      struct s_ComputePipeline *pipeline) {
  if (!pipeline) return;

  vkDeviceWaitIdle(state->device.logical_device);

  if (pipeline->descriptor_pool) {
    vkDestroyDescriptorPool(state->device.logical_device,
                             pipeline->descriptor_pool, state->allocator);
  }
  if (pipeline->pipeline) {
    vkDestroyPipeline(state->device.logical_device, pipeline->pipeline,
                       state->allocator);
  }
  if (pipeline->layout) {
    vkDestroyPipelineLayout(state->device.logical_device, pipeline->layout,
                             state->allocator);
  }
  if (pipeline->descriptor_layout) {
    vkDestroyDescriptorSetLayout(state->device.logical_device,
                                  pipeline->descriptor_layout, state->allocator);
  }

  vkr_dmemory_free(&state->dmemory, pipeline);
}

void vulkan_compute_pipeline_bind(VulkanBackendState *state,
                                   VkCommandBuffer command_buffer,
                                   struct s_ComputePipeline *pipeline) {
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                     pipeline->pipeline);
}

void vulkan_compute_dispatch(VulkanBackendState *state,
                              VkCommandBuffer command_buffer,
                              struct s_ComputePipeline *pipeline,
                              const VkrComputeDispatch *dispatch) {
  // Bind descriptor set for current frame
  uint32_t frame_index = state->current_frame;

  if (pipeline->storage_buffer_count > 0 || pipeline->uniform_buffer_count > 0 ||
      pipeline->storage_image_count > 0 || pipeline->sampled_image_count > 0) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipeline->layout, 0, 1,
                             &pipeline->descriptor_sets[frame_index], 0, NULL);
  }

  vkCmdDispatch(command_buffer, dispatch->group_count_x, dispatch->group_count_y,
                 dispatch->group_count_z);
}

void vulkan_compute_barrier_to_vertex(VkCommandBuffer command_buffer) {
  VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
  };

  vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                        0, 1, &barrier, 0, NULL, 0, NULL);
}

void vulkan_compute_barrier_from_vertex(VkCommandBuffer command_buffer) {
  VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
  };

  vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 1, &barrier, 0, NULL, 0, NULL);
}
```

### Step 4: Storage Buffer Creation

Add to `lib/src/renderer/vulkan/vulkan_buffer.c` (or new file):

```c
bool8_t vulkan_storage_buffer_create(
    VulkanBackendState *state,
    uint64_t size,
    VkrBufferUsage usage,
    VkrBufferFlags flags,
    VulkanStorageBuffer *out_buffer) {

  out_buffer->size = size;
  out_buffer->usage = usage;
  out_buffer->mapped_ptr = NULL;

  VkBufferUsageFlags vk_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & VKR_BUFFER_USAGE_VERTEX_SOURCE) {
    vk_usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  }
  if (usage & VKR_BUFFER_USAGE_TRANSFER_SRC) {
    vk_usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  }
  if (usage & VKR_BUFFER_USAGE_TRANSFER_DST) {
    vk_usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = vk_usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VkResult result = vkCreateBuffer(state->device.logical_device, &buffer_info,
                                    state->allocator, &out_buffer->buffer);
  if (result != VK_SUCCESS) return false_v;

  VkMemoryRequirements mem_reqs;
  vkGetBufferMemoryRequirements(state->device.logical_device, out_buffer->buffer,
                                 &mem_reqs);

  VkMemoryPropertyFlags mem_flags = 0;
  if (flags & VKR_BUFFER_FLAG_DEVICE_LOCAL) {
    mem_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  if (flags & VKR_BUFFER_FLAG_HOST_VISIBLE) {
    mem_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  }
  if (flags & VKR_BUFFER_FLAG_HOST_COHERENT) {
    mem_flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }

  uint32_t mem_type_index = vulkan_find_memory_type(
      state, mem_reqs.memoryTypeBits, mem_flags);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = mem_type_index,
  };

  result = vkAllocateMemory(state->device.logical_device, &alloc_info,
                             state->allocator, &out_buffer->memory);
  if (result != VK_SUCCESS) {
    vkDestroyBuffer(state->device.logical_device, out_buffer->buffer,
                     state->allocator);
    return false_v;
  }

  vkBindBufferMemory(state->device.logical_device, out_buffer->buffer,
                      out_buffer->memory, 0);

  // Map if host-visible
  if (flags & VKR_BUFFER_FLAG_HOST_VISIBLE) {
    vkMapMemory(state->device.logical_device, out_buffer->memory, 0, size, 0,
                 &out_buffer->mapped_ptr);
  }

  return true_v;
}

void vulkan_storage_buffer_destroy(VulkanBackendState *state,
                                    VulkanStorageBuffer *buffer) {
  if (buffer->mapped_ptr) {
    vkUnmapMemory(state->device.logical_device, buffer->memory);
  }
  if (buffer->buffer) {
    vkDestroyBuffer(state->device.logical_device, buffer->buffer,
                     state->allocator);
  }
  if (buffer->memory) {
    vkFreeMemory(state->device.logical_device, buffer->memory, state->allocator);
  }
  memset(buffer, 0, sizeof(*buffer));
}
```

### Step 5: Frontend API

Add to `lib/src/renderer/renderer_frontend.h`:

```c
/**
 * @brief Create a compute pipeline.
 * @param renderer Renderer handle
 * @param desc Pipeline description
 * @param out_error Error code
 * @return Pipeline handle or NULL on failure
 */
VkrComputePipelineHandle
vkr_renderer_create_compute_pipeline(VkrRendererFrontendHandle renderer,
                                      const VkrComputePipelineDescription *desc,
                                      VkrRendererError *out_error);

/**
 * @brief Destroy a compute pipeline.
 */
void vkr_renderer_destroy_compute_pipeline(VkrRendererFrontendHandle renderer,
                                            VkrComputePipelineHandle pipeline);

/**
 * @brief Begin compute pass (before graphics rendering).
 * @param renderer Renderer handle
 * @return true if compute pass started
 */
bool8_t vkr_renderer_begin_compute(VkrRendererFrontendHandle renderer);

/**
 * @brief End compute pass and transition to graphics.
 */
void vkr_renderer_end_compute(VkrRendererFrontendHandle renderer);

/**
 * @brief Dispatch compute shader.
 * @param renderer Renderer handle
 * @param pipeline Compute pipeline to dispatch
 * @param dispatch Dispatch parameters
 * @param push_data Push constant data (or NULL)
 * @param push_size Push constant size
 */
void vkr_renderer_dispatch_compute(VkrRendererFrontendHandle renderer,
                                    VkrComputePipelineHandle pipeline,
                                    const VkrComputeDispatch *dispatch,
                                    const void *push_data,
                                    uint32_t push_size);

/**
 * @brief Create storage buffer for compute operations.
 */
VkrStorageBufferHandle
vkr_renderer_create_storage_buffer(VkrRendererFrontendHandle renderer,
                                    uint64_t size,
                                    VkrBufferUsage usage,
                                    VkrBufferFlags flags,
                                    VkrRendererError *out_error);

/**
 * @brief Destroy storage buffer.
 */
void vkr_renderer_destroy_storage_buffer(VkrRendererFrontendHandle renderer,
                                          VkrStorageBufferHandle buffer);

/**
 * @brief Bind storage buffer to compute pipeline.
 * @param renderer Renderer handle
 * @param pipeline Compute pipeline
 * @param binding_index Storage buffer binding index
 * @param buffer Storage buffer to bind
 */
void vkr_renderer_bind_storage_buffer(VkrRendererFrontendHandle renderer,
                                       VkrComputePipelineHandle pipeline,
                                       uint32_t binding_index,
                                       VkrStorageBufferHandle buffer);
```

### Step 6: Shader Loading Extension

Modify `lib/src/renderer/vulkan/vulkan_shaders.c` to handle compute shaders:

```c
// vulkan_shader_create_module should already work for compute
// as it just loads SPIR-V bytes. Verify entry point detection.

// For Slang compilation, ensure compute stage is supported:
// slangc -profile sm_6_0 -target spirv -entry computeMain wave.slang -o wave.comp.spv
```

## Slang Compute Shader Example

Create `assets/shaders/effects/test_compute.slang`:

```slang
// Simple test compute shader that writes to a storage buffer

struct Vertex {
    float3 position;
    float3 normal;
    float2 texcoord;
    float4 color;
    float3 tangent;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<Vertex> vertices;

[[vk::binding(1, 0)]]
cbuffer Params {
    float time;
    uint vertex_count;
};

[numthreads(64, 1, 1)]
void computeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    uint idx = dispatch_id.x;
    if (idx >= vertex_count) return;

    Vertex v = vertices[idx];

    // Simple test: offset Y by sin wave
    v.position.y += sin(v.position.x * 2.0 + time) * 0.1;

    vertices[idx] = v;
}
```

Compile with:
```bash
slangc -profile sm_6_0 -target spirv -entry computeMain \
  assets/shaders/effects/test_compute.slang \
  -o build/app/assets/shaders/effects/test_compute.comp.spv
```

## Testing

### Unit Test

```c
// tests/test_compute_pipeline.c

void test_compute_pipeline_creation(void) {
  VkrRendererFrontendHandle renderer = /* initialize */;

  VkrComputePipelineDescription desc = {
      .shader_path = string8_lit("assets/shaders/effects/test_compute.comp.spv"),
      .entry_point = string8_lit("computeMain"),
      .storage_buffer_count = 1,
      .uniform_buffer_count = 1,
      .push_constant_size = 0,
  };

  VkrRendererError err;
  VkrComputePipelineHandle pipeline =
      vkr_renderer_create_compute_pipeline(renderer, &desc, &err);

  assert(pipeline != NULL);
  assert(err == VKR_RENDERER_ERROR_NONE);

  vkr_renderer_destroy_compute_pipeline(renderer, pipeline);
}

void test_compute_dispatch(void) {
  VkrRendererFrontendHandle renderer = /* initialize */;
  VkrComputePipelineHandle pipeline = /* create pipeline */;

  // Create storage buffer
  VkrRendererError err;
  VkrStorageBufferHandle buffer = vkr_renderer_create_storage_buffer(
      renderer,
      sizeof(VkrVertex3d) * 1024,
      VKR_BUFFER_USAGE_COMPUTE_STORAGE | VKR_BUFFER_USAGE_VERTEX_SOURCE,
      VKR_BUFFER_FLAG_DEVICE_LOCAL,
      &err);

  assert(buffer != NULL);

  // Begin frame
  vkr_renderer_begin_frame(renderer, /* delta_time */);

  // Compute pass
  vkr_renderer_begin_compute(renderer);
  vkr_renderer_bind_storage_buffer(renderer, pipeline, 0, buffer);
  vkr_renderer_dispatch_compute(renderer, pipeline,
                                 &(VkrComputeDispatch){.group_count_x = 16,
                                                       .group_count_y = 1,
                                                       .group_count_z = 1},
                                 NULL, 0);
  vkr_renderer_end_compute(renderer);

  // Graphics pass would follow...

  vkr_renderer_destroy_storage_buffer(renderer, buffer);
  vkr_renderer_destroy_compute_pipeline(renderer, pipeline);
}
```

### Validation

Enable Vulkan validation layers and check for:
- No synchronization errors (memory barriers)
- No descriptor binding errors
- No shader compilation errors
- Compute dispatch completes without timeout

## Completion Criteria

- [ ] `VkrComputePipelineDescription` type defined
- [ ] Compute pipeline creation works (unit test passes)
- [ ] Compute shader loads from SPIR-V
- [ ] Storage buffer creation works
- [ ] Descriptor set binding works
- [ ] `vkCmdDispatch` executes without errors
- [ ] Memory barriers prevent data races
- [ ] Test compute shader modifies buffer data

## Next Steps

After completing this phase, proceed to:
- **03-effects-system-design.md**: Build effect registry using compute pipelines
- **04-wave-effect-demo.md**: Implement wave effect using this infrastructure
