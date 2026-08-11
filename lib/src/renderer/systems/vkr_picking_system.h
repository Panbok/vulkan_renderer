#pragma once

#include "defines.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

typedef enum VkrPickingState {
  VKR_PICKING_STATE_IDLE = 0,
  VKR_PICKING_STATE_RENDER_PENDING,
  VKR_PICKING_STATE_RENDER_RECORDED,
  VKR_PICKING_STATE_READBACK_PENDING,
  VKR_PICKING_STATE_RESULT_READY,
} VkrPickingState;

typedef struct VkrPickingContext {
  uint32_t width;
  uint32_t height;
  VkrPickingState state;
  uint32_t requested_x;
  uint32_t requested_y;
  uint32_t result_object_id;
  bool8_t initialized;
} VkrPickingContext;

typedef struct VkrPickResult {
  uint32_t object_id;
  bool8_t hit;
} VkrPickResult;

bool8_t vkr_picking_init(struct s_RendererFrontend *renderer,
                         VkrPickingContext *ctx, uint32_t width,
                         uint32_t height);
void vkr_picking_resize(struct s_RendererFrontend *renderer,
                        VkrPickingContext *ctx, uint32_t width,
                        uint32_t height);
void vkr_picking_request(VkrPickingContext *ctx, uint32_t x, uint32_t y);
VkrPickResult vkr_picking_get_result(struct s_RendererFrontend *renderer,
                                     VkrPickingContext *ctx);
bool8_t vkr_picking_is_pending(const VkrPickingContext *ctx);
void vkr_picking_cancel(VkrPickingContext *ctx);
void vkr_picking_shutdown(struct s_RendererFrontend *renderer,
                          VkrPickingContext *ctx);
