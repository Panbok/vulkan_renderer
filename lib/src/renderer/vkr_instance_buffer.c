#include "renderer/vkr_instance_buffer.h"

#include "core/logger.h"

vkr_internal bool8_t vkr_instance_buffer_pool_try_init(
    VkrInstanceBufferPool *pool, VkrRendererFrontendHandle renderer,
    uint32_t max_instances, VkrMemoryPropertyFlags memory_flags,
    bool8_t needs_flush) {
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  uint64_t size_bytes = (uint64_t)max_instances * sizeof(VkrInstanceDataGPU);
  VkrBufferDescription desc = {
      .size = size_bytes,
      .usage = vkr_buffer_usage_flags_from_bits(
          VKR_BUFFER_USAGE_STORAGE | VKR_BUFFER_USAGE_TRANSFER_DST),
      .memory_properties = memory_flags,
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
      .persistently_mapped = true_v,
  };

  for (uint32_t i = 0; i < VKR_INSTANCE_BUFFER_FRAMES; ++i) {
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    VkrBufferHandle buffer =
        vkr_renderer_create_buffer(renderer, &desc, NULL, &err);
    if (err != VKR_RENDERER_ERROR_NONE || !buffer) {
      log_warn("Instance buffer allocation failed with memory flags 0x%02x",
               bitset8_get_value(&memory_flags));
      for (uint32_t j = 0; j < i; ++j) {
        if (pool->buffers[j].buffer) {
          vkr_renderer_destroy_buffer(renderer, pool->buffers[j].buffer);
        }
        pool->buffers[j] = (VkrInstanceBuffer){0};
      }
      return false_v;
    }
    pool->buffers[i] = (VkrInstanceBuffer){.buffer = buffer,
                                           .mapped_ptr = NULL,
                                           .capacity = max_instances,
                                           .write_offset = 0,
                                           .needs_flush = needs_flush};
    pool->buffers[i].mapped_ptr = (VkrInstanceDataGPU *)
        vkr_renderer_buffer_get_mapped_ptr(renderer, buffer);
    if (!pool->buffers[i].mapped_ptr) {
      log_error("Instance buffer mapping failed");
      for (uint32_t j = 0; j <= i; ++j) {
        if (pool->buffers[j].buffer) {
          vkr_renderer_destroy_buffer(renderer, pool->buffers[j].buffer);
        }
        pool->buffers[j] = (VkrInstanceBuffer){0};
      }
      return false_v;
    }
  }

  return true_v;
}

bool8_t vkr_instance_buffer_pool_init(VkrInstanceBufferPool *pool,
                                      VkrRendererFrontendHandle renderer,
                                      uint32_t max_instances) {
  assert_log(pool != NULL, "Pool is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  MemZero(pool, sizeof(*pool));
  pool->renderer = renderer;
  pool->max_instances = max_instances > 0 ? max_instances
                                          : VKR_INSTANCE_BUFFER_MAX_INSTANCES;

  VkrMemoryPropertyFlags preferred = vkr_memory_property_flags_from_bits(
      VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT |
      VKR_MEMORY_PROPERTY_DEVICE_LOCAL);
  VkrMemoryPropertyFlags fallback = vkr_memory_property_flags_from_bits(
      VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT);
  VkrMemoryPropertyFlags fallback_no_coherent =
      vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_HOST_VISIBLE);

  if (vkr_instance_buffer_pool_try_init(pool, renderer, pool->max_instances,
                                        preferred, false_v)) {
    pool->initialized = true_v;
    return true_v;
  }

  if (vkr_instance_buffer_pool_try_init(pool, renderer, pool->max_instances,
                                        fallback, false_v)) {
    pool->initialized = true_v;
    return true_v;
  }

  if (vkr_instance_buffer_pool_try_init(pool, renderer, pool->max_instances,
                                        fallback_no_coherent, true_v)) {
    pool->initialized = true_v;
    return true_v;
  }

  return false_v;
}

void vkr_instance_buffer_pool_shutdown(VkrInstanceBufferPool *pool,
                                       VkrRendererFrontendHandle renderer) {
  if (!pool || !pool->initialized) {
    return;
  }

  for (uint32_t i = 0; i < VKR_INSTANCE_BUFFER_FRAMES; ++i) {
    if (pool->buffers[i].buffer) {
      vkr_renderer_destroy_buffer(renderer, pool->buffers[i].buffer);
    }
    pool->buffers[i].buffer = NULL;
    pool->buffers[i].mapped_ptr = NULL;
    pool->buffers[i].capacity = 0;
    pool->buffers[i].write_offset = 0;
  }

  pool->initialized = false_v;
  pool->renderer = NULL;
}

void vkr_instance_buffer_begin_frame(VkrInstanceBufferPool *pool,
                                     uint32_t frame_index) {
  if (!pool || !pool->initialized) {
    return;
  }

  pool->current_frame = frame_index % VKR_INSTANCE_BUFFER_FRAMES;
  VkrInstanceBuffer *buffer = &pool->buffers[pool->current_frame];
  buffer->write_offset = 0;

  vkr_renderer_set_instance_buffer(pool->renderer, buffer->buffer);
}

bool8_t vkr_instance_buffer_alloc(VkrInstanceBufferPool *pool, uint32_t count,
                                  uint32_t *out_base_instance,
                                  VkrInstanceDataGPU **out_ptr) {
  assert_log(pool != NULL, "Pool is NULL");
  assert_log(out_base_instance != NULL, "Base instance is NULL");
  assert_log(out_ptr != NULL, "Out ptr is NULL");

  if (!pool->initialized || count == 0) {
    return false_v;
  }

  VkrInstanceBuffer *buffer = &pool->buffers[pool->current_frame];
  if (buffer->write_offset + count > buffer->capacity) {
    log_warn("Instance buffer overflow: %u + %u > %u", buffer->write_offset,
             count, buffer->capacity);
    return false_v;
  }

  *out_base_instance = buffer->write_offset;
  *out_ptr = buffer->mapped_ptr + buffer->write_offset;
  buffer->write_offset += count;
  return true_v;
}

void vkr_instance_buffer_flush_range(VkrInstanceBufferPool *pool,
                                     uint32_t base_instance,
                                     uint32_t count) {
  if (!pool || !pool->initialized || count == 0) {
    return;
  }

  VkrInstanceBuffer *buffer = &pool->buffers[pool->current_frame];
  if (!buffer->needs_flush) {
    return;
  }

  uint64_t offset_bytes = (uint64_t)base_instance * sizeof(VkrInstanceDataGPU);
  uint64_t size_bytes = (uint64_t)count * sizeof(VkrInstanceDataGPU);
  vkr_renderer_flush_buffer(pool->renderer, buffer->buffer, offset_bytes,
                            size_bytes);
}

void vkr_instance_buffer_flush_current(VkrInstanceBufferPool *pool) {
  if (!pool || !pool->initialized) {
    return;
  }

  VkrInstanceBuffer *buffer = &pool->buffers[pool->current_frame];
  vkr_instance_buffer_flush_range(pool, 0, buffer->write_offset);
}

VkrBufferHandle vkr_instance_buffer_get_current(
    const VkrInstanceBufferPool *pool) {
  if (!pool || !pool->initialized) {
    return NULL;
  }
  return pool->buffers[pool->current_frame].buffer;
}
