#pragma once

#include "defines.h"
#include "renderer/vkr_renderer.h"

/** Maximum indirect draw commands per frame. */
#define VKR_INDIRECT_DRAW_MAX_DRAWS 16384
/** Number of buffered indirect draw streams. */
#define VKR_INDIRECT_DRAW_FRAMES 3

/**
 * @brief GPU indirect draw command layout for indexed draws.
 *
 * Matches VkDrawIndexedIndirectCommand exactly.
 */
typedef struct VkrIndirectDrawCommand {
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
} VkrIndirectDrawCommand;

_Static_assert(sizeof(VkrIndirectDrawCommand) == 20,
               "VkrIndirectDrawCommand must match VkDrawIndexedIndirectCommand");

typedef struct VkrIndirectDrawBuffer {
  VkrBufferHandle buffer;
  VkrIndirectDrawCommand *mapped_ptr;
  uint32_t capacity;
  uint32_t write_offset;
  bool8_t needs_flush;
} VkrIndirectDrawBuffer;

typedef struct VkrIndirectDrawSystem {
  VkrIndirectDrawBuffer buffers[VKR_INDIRECT_DRAW_FRAMES];
  VkrRendererFrontendHandle renderer;
  uint32_t current_frame;
  uint32_t max_draws;
  bool8_t enabled;
  bool8_t initialized;
} VkrIndirectDrawSystem;

bool8_t vkr_indirect_draw_init(VkrIndirectDrawSystem *system,
                               VkrRendererFrontendHandle renderer,
                               uint32_t max_draws);
void vkr_indirect_draw_shutdown(VkrIndirectDrawSystem *system,
                                VkrRendererFrontendHandle renderer);

void vkr_indirect_draw_begin_frame(VkrIndirectDrawSystem *system,
                                   uint32_t frame_index);

bool8_t vkr_indirect_draw_alloc(VkrIndirectDrawSystem *system, uint32_t count,
                                uint32_t *out_base_draw,
                                VkrIndirectDrawCommand **out_ptr);

void vkr_indirect_draw_flush_range(VkrIndirectDrawSystem *system,
                                   uint32_t base_draw, uint32_t count);
void vkr_indirect_draw_flush_current(VkrIndirectDrawSystem *system);

VkrBufferHandle vkr_indirect_draw_get_current(
    const VkrIndirectDrawSystem *system);

uint32_t vkr_indirect_draw_remaining(const VkrIndirectDrawSystem *system);
