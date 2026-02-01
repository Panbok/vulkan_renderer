#include "renderer/vkr_indirect_draw.h"

#include "core/logger.h"

vkr_internal bool8_t vkr_indirect_draw_try_init(
    VkrIndirectDrawSystem *system, VkrRendererFrontendHandle renderer,
    uint32_t max_draws, VkrMemoryPropertyFlags memory_flags,
    bool8_t needs_flush) {
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  uint64_t size_bytes =
      (uint64_t)max_draws * sizeof(VkrIndirectDrawCommand);
  VkrBufferDescription desc = {
      .size = size_bytes,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDIRECT |
                                                VKR_BUFFER_USAGE_TRANSFER_DST),
      .memory_properties = memory_flags,
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
      .persistently_mapped = true_v,
  };

  for (uint32_t i = 0; i < VKR_INDIRECT_DRAW_FRAMES; ++i) {
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    VkrBufferHandle buffer =
        vkr_renderer_create_buffer(renderer, &desc, NULL, &err);
    if (err != VKR_RENDERER_ERROR_NONE || !buffer) {
      log_warn("Indirect draw buffer allocation failed with flags 0x%02x",
               bitset8_get_value(&memory_flags));
      for (uint32_t j = 0; j < i; ++j) {
        if (system->buffers[j].buffer) {
          vkr_renderer_destroy_buffer(renderer, system->buffers[j].buffer);
        }
        system->buffers[j] = (VkrIndirectDrawBuffer){0};
      }
      return false_v;
    }

    system->buffers[i] = (VkrIndirectDrawBuffer){.buffer = buffer,
                                                 .mapped_ptr = NULL,
                                                 .capacity = max_draws,
                                                 .write_offset = 0,
                                                 .needs_flush = needs_flush};
    system->buffers[i].mapped_ptr = (VkrIndirectDrawCommand *)
        vkr_renderer_buffer_get_mapped_ptr(renderer, buffer);
    if (!system->buffers[i].mapped_ptr) {
      log_error("Indirect draw buffer mapping failed");
      for (uint32_t j = 0; j <= i; ++j) {
        if (system->buffers[j].buffer) {
          vkr_renderer_destroy_buffer(renderer, system->buffers[j].buffer);
        }
        system->buffers[j] = (VkrIndirectDrawBuffer){0};
      }
      return false_v;
    }
  }

  return true_v;
}

bool8_t vkr_indirect_draw_init(VkrIndirectDrawSystem *system,
                               VkrRendererFrontendHandle renderer,
                               uint32_t max_draws) {
  assert_log(system != NULL, "Indirect draw system is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  MemZero(system, sizeof(*system));
  system->renderer = renderer;
  system->max_draws =
      max_draws > 0 ? max_draws : VKR_INDIRECT_DRAW_MAX_DRAWS;
  system->enabled = true_v;

  VkrMemoryPropertyFlags preferred = vkr_memory_property_flags_from_bits(
      VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT |
      VKR_MEMORY_PROPERTY_DEVICE_LOCAL);
  VkrMemoryPropertyFlags fallback = vkr_memory_property_flags_from_bits(
      VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT);
  VkrMemoryPropertyFlags fallback_no_coherent =
      vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_HOST_VISIBLE);

  if (vkr_indirect_draw_try_init(system, renderer, system->max_draws, preferred,
                                false_v)) {
    system->initialized = true_v;
    return true_v;
  }

  if (vkr_indirect_draw_try_init(system, renderer, system->max_draws, fallback,
                                false_v)) {
    system->initialized = true_v;
    return true_v;
  }

  if (vkr_indirect_draw_try_init(system, renderer, system->max_draws,
                                fallback_no_coherent, true_v)) {
    system->initialized = true_v;
    return true_v;
  }

  system->enabled = false_v;
  return false_v;
}

void vkr_indirect_draw_shutdown(VkrIndirectDrawSystem *system,
                                VkrRendererFrontendHandle renderer) {
  if (!system || !system->initialized) {
    return;
  }

  for (uint32_t i = 0; i < VKR_INDIRECT_DRAW_FRAMES; ++i) {
    if (system->buffers[i].buffer) {
      vkr_renderer_destroy_buffer(renderer, system->buffers[i].buffer);
    }
    system->buffers[i] = (VkrIndirectDrawBuffer){0};
  }

  system->initialized = false_v;
  system->enabled = false_v;
  system->renderer = NULL;
}

void vkr_indirect_draw_begin_frame(VkrIndirectDrawSystem *system,
                                   uint32_t frame_index) {
  if (!system || !system->initialized) {
    return;
  }

  system->current_frame = frame_index % VKR_INDIRECT_DRAW_FRAMES;
  VkrIndirectDrawBuffer *buffer = &system->buffers[system->current_frame];
  buffer->write_offset = 0;
}

bool8_t vkr_indirect_draw_alloc(VkrIndirectDrawSystem *system, uint32_t count,
                                uint32_t *out_base_draw,
                                VkrIndirectDrawCommand **out_ptr) {
  assert_log(system != NULL, "Indirect draw system is NULL");
  assert_log(out_base_draw != NULL, "Out base draw is NULL");
  assert_log(out_ptr != NULL, "Out pointer is NULL");

  if (!system->initialized || count == 0) {
    return false_v;
  }

  VkrIndirectDrawBuffer *buffer = &system->buffers[system->current_frame];
  if (buffer->write_offset + count > buffer->capacity) {
    log_warn("Indirect draw buffer overflow: %u + %u > %u",
             buffer->write_offset, count, buffer->capacity);
    return false_v;
  }

  *out_base_draw = buffer->write_offset;
  *out_ptr = buffer->mapped_ptr + buffer->write_offset;
  buffer->write_offset += count;
  return true_v;
}

void vkr_indirect_draw_flush_range(VkrIndirectDrawSystem *system,
                                   uint32_t base_draw, uint32_t count) {
  if (!system || !system->initialized || count == 0) {
    return;
  }

  VkrIndirectDrawBuffer *buffer = &system->buffers[system->current_frame];
  if (!buffer->needs_flush) {
    return;
  }

  uint64_t offset_bytes =
      (uint64_t)base_draw * sizeof(VkrIndirectDrawCommand);
  uint64_t size_bytes = (uint64_t)count * sizeof(VkrIndirectDrawCommand);
  vkr_renderer_flush_buffer(system->renderer, buffer->buffer, offset_bytes,
                            size_bytes);
}

void vkr_indirect_draw_flush_current(VkrIndirectDrawSystem *system) {
  if (!system || !system->initialized) {
    return;
  }

  VkrIndirectDrawBuffer *buffer = &system->buffers[system->current_frame];
  vkr_indirect_draw_flush_range(system, 0, buffer->write_offset);
}

VkrBufferHandle vkr_indirect_draw_get_current(
    const VkrIndirectDrawSystem *system) {
  if (!system || !system->initialized) {
    return NULL;
  }
  return system->buffers[system->current_frame].buffer;
}

uint32_t vkr_indirect_draw_remaining(const VkrIndirectDrawSystem *system) {
  if (!system || !system->initialized) {
    return 0;
  }

  const VkrIndirectDrawBuffer *buffer = &system->buffers[system->current_frame];
  if (buffer->write_offset >= buffer->capacity) {
    return 0;
  }

  return buffer->capacity - buffer->write_offset;
}
