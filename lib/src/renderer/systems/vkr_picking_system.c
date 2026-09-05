#include "renderer/systems/vkr_picking_system.h"

#include "core/logger.h"
#include "renderer/vkr_renderer.h"

static bool8_t picking_set_target_size(VkrPickingContext *ctx, uint32_t width,
                                       uint32_t height) {
  if (!ctx || width == 0 || height == 0) {
    return false_v;
  }
  ctx->width = width;
  ctx->height = height;
  return true_v;
}

bool8_t vkr_picking_init(VkrPickingContext *ctx, uint32_t width,
                         uint32_t height) {
  if (!ctx || !picking_set_target_size(ctx, width, height)) {
    return false_v;
  }
  ctx->state = VKR_PICKING_STATE_IDLE;
  ctx->initialized = true_v;
  return true_v;
}

void vkr_picking_resize(VkrPickingContext *ctx, uint32_t width,
                        uint32_t height) {
  if (!ctx || !ctx->initialized ||
      (ctx->width == width && ctx->height == height)) {
    return;
  }
  if (!picking_set_target_size(ctx, width, height)) {
    log_warn("Invalid picking dimensions: %ux%u", width, height);
  }
}

void vkr_picking_request(VkrPickingContext *ctx, uint32_t x, uint32_t y) {
  if (!ctx || !ctx->initialized || ctx->state != VKR_PICKING_STATE_IDLE) {
    return;
  }
  if (x >= ctx->width || y >= ctx->height) {
    log_warn("Pick coordinates out of bounds: (%u, %u) vs (%u, %u)", x, y,
             ctx->width, ctx->height);
    return;
  }
  ctx->requested_x = x;
  ctx->requested_y = y;
  ctx->result_object_id = 0;
  ctx->state = VKR_PICKING_STATE_RENDER_PENDING;
}

VkrPickResult vkr_picking_get_result(struct VkrRenderer *renderer,
                                     VkrPickingContext *ctx) {
  VkrPickResult result = {0};
  if (!renderer || !ctx || !ctx->initialized) {
    return result;
  }
  result.object_id = ctx->result_object_id;
  result.hit = result.object_id != 0;
  if (ctx->state == VKR_PICKING_STATE_RESULT_READY) {
    ctx->state = VKR_PICKING_STATE_IDLE;
    return result;
  }
  if (ctx->state != VKR_PICKING_STATE_READBACK_PENDING) {
    return result;
  }
  VkrPixelReadbackResult readback = {0};
  if (vkr_renderer_get_pixel_readback_result(renderer, &readback) !=
      VKR_RENDERER_ERROR_NONE) {
    ctx->state = VKR_PICKING_STATE_IDLE;
    return result;
  }
  if (readback.status == VKR_READBACK_STATUS_READY) {
    if (readback.valid) {
      ctx->result_object_id = readback.data;
      result.object_id = readback.data;
      result.hit = readback.data != 0;
    }
    ctx->state = VKR_PICKING_STATE_IDLE;
  } else if (readback.status == VKR_READBACK_STATUS_ERROR) {
    ctx->state = VKR_PICKING_STATE_IDLE;
  }
  return result;
}

bool8_t vkr_picking_is_pending(const VkrPickingContext *ctx) {
  return ctx && (ctx->state == VKR_PICKING_STATE_RENDER_PENDING ||
                 ctx->state == VKR_PICKING_STATE_RENDER_RECORDED ||
                 ctx->state == VKR_PICKING_STATE_READBACK_PENDING);
}

void vkr_picking_cancel(VkrPickingContext *ctx) {
  if (ctx) {
    ctx->state = VKR_PICKING_STATE_IDLE;
    ctx->result_object_id = 0;
  }
}

void vkr_picking_shutdown(VkrPickingContext *ctx) {
  if (!ctx) {
    return;
  }
  MemZero(ctx, sizeof(*ctx));
}
