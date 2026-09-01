/**
 * @file vkr_ui_system.c
 * @brief Stateless UI text and pipeline resources.
 */

#include "renderer/systems/vkr_ui_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_window.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/vkr_render_packet.h"

#include <math.h>

#define VKR_UI_SYSTEM_MAX_TEXTS 16

vkr_internal bool8_t vkr_ui_system_get_layout_size(RendererFrontend *rf,
                                                   VkrUiSystem *system,
                                                   uint32_t *out_width,
                                                   uint32_t *out_height) {
  if (!rf || !system || !out_width || !out_height) {
    return false_v;
  }

  uint32_t width = rf->last_window_width;
  uint32_t height = rf->last_window_height;
  if ((width == 0 || height == 0) && rf->window) {
    VkrWindowPixelSize size = vkr_window_get_pixel_size(rf->window);
    width = size.width;
    height = size.height;
  }

  if (system->offscreen_enabled && system->offscreen_width > 0 &&
      system->offscreen_height > 0) {
    *out_width = system->offscreen_width;
    *out_height = system->offscreen_height;
    return true_v;
  }

  *out_width = width;
  *out_height = height;
  return true_v;
}

vkr_internal VkrWindowContentScale
vkr_ui_system_get_content_scale(RendererFrontend *rf, VkrUiSystem *system) {
  if (system->offscreen_enabled) {
    return (VkrWindowContentScale){
        .value = system->offscreen_content_scale,
        .revision = system->offscreen_content_scale_revision,
    };
  }
  if (rf->window) {
    return vkr_window_get_content_scale(rf->window);
  }
  return (VkrWindowContentScale){.value = 1.0f, .revision = 1u};
}

vkr_internal bool8_t vkr_ui_system_update_text_content_scale(
    VkrUiSystem *system, VkrWindowContentScale snapshot) {
  if (!isfinite(snapshot.value) || snapshot.value <= 0.0f) {
    snapshot = (VkrWindowContentScale){.value = 1.0f, .revision = 0u};
  }
  if (system->text_content_scale == snapshot.value &&
      system->content_scale_revision == snapshot.revision) {
    return false_v;
  }

  const bool8_t value_changed = system->text_content_scale != snapshot.value;
  system->text_content_scale = snapshot.value;
  system->content_scale_revision = snapshot.revision;
  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    if (value_changed) {
      vkr_ui_text_set_content_scale(&slot->text, snapshot.value);
    } else {
      slot->text.layout_dirty = true_v;
      slot->text.buffers_dirty = true_v;
    }
  }
  return true_v;
}

vkr_internal void vkr_ui_system_position_slot(VkrUiTextSlot *slot,
                                              uint32_t width, uint32_t height,
                                              float32_t content_scale);

vkr_internal void vkr_ui_system_refresh_layout(RendererFrontend *rf,
                                               VkrUiSystem *system) {
  if (!rf || !system) {
    return;
  }

  uint32_t prev_width = system->screen_width;
  uint32_t prev_height = system->screen_height;
  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  if (!vkr_ui_system_get_layout_size(rf, system, &layout_width,
                                     &layout_height)) {
    return;
  }

  const VkrWindowContentScale content_scale =
      vkr_ui_system_get_content_scale(rf, system);
  const bool8_t scale_changed =
      vkr_ui_system_update_text_content_scale(system, content_scale);
  if (layout_width == prev_width && layout_height == prev_height &&
      !scale_changed) {
    return;
  }

  system->screen_width = layout_width;
  system->screen_height = layout_height;

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_system_position_slot(slot, layout_width, layout_height,
                                system->text_content_scale);
  }
}

vkr_internal void vkr_ui_system_position_slot(VkrUiTextSlot *slot,
                                              uint32_t width, uint32_t height,
                                              float32_t content_scale) {
  if (!slot || !slot->active || width == 0 || height == 0) {
    return;
  }

  VkrTextBounds bounds = vkr_ui_text_get_bounds(&slot->text);
  const float32_t scaled_width = bounds.size.x;
  const float32_t scaled_height = bounds.size.y;
  const Vec2 device_padding = vec2_scale(slot->padding, content_scale);
  float32_t x = device_padding.x;
  float32_t y = device_padding.y;

  switch (slot->anchor) {
  case VKR_UI_TEXT_ANCHOR_TOP_RIGHT:
    x = (float32_t)width - scaled_width - device_padding.x;
    y = (float32_t)height - scaled_height - device_padding.y;
    break;
  case VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT:
    break;
  case VKR_UI_TEXT_ANCHOR_BOTTOM_RIGHT:
    x = (float32_t)width - scaled_width - device_padding.x;
    break;
  case VKR_UI_TEXT_ANCHOR_TOP_LEFT:
  default:
    y = (float32_t)height - scaled_height - device_padding.y;
    break;
  }

  vkr_transform_set_position(&slot->text.transform, vec3_new(x, y, 0.0f));
  vkr_transform_set_scale(&slot->text.transform, vec3_one());
}

vkr_internal bool8_t vkr_ui_system_ensure_slot(VkrUiSystem *system,
                                               uint32_t text_id,
                                               VkrUiTextSlot **out_slot) {
  if (!system || !out_slot || !system->text_slots.data) {
    return false_v;
  }
  if (text_id >= system->text_slots.length) {
    log_error("UI text id %u exceeds max (%llu)", text_id,
              (unsigned long long)system->text_slots.length);
    return false_v;
  }

  *out_slot = &system->text_slots.data[text_id];
  return true_v;
}

vkr_internal bool8_t vkr_ui_system_find_free_slot(VkrUiSystem *system,
                                                  uint32_t *out_text_id,
                                                  VkrUiTextSlot **out_slot) {
  if (!system || !out_text_id || !out_slot || !system->text_slots.data) {
    return false_v;
  }

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      *out_text_id = (uint32_t)i;
      *out_slot = slot;
      return true_v;
    }
  }

  log_error("UI text slots exhausted (max %llu)",
            (unsigned long long)system->text_slots.length);
  return false_v;
}

vkr_internal VkrUiTextSlot *vkr_ui_system_get_active_slot(VkrUiSystem *system,
                                                          uint32_t text_id) {
  if (!system || !system->text_slots.data ||
      text_id >= system->text_slots.length) {
    return NULL;
  }

  VkrUiTextSlot *slot = &system->text_slots.data[text_id];
  return slot->active ? slot : NULL;
}

bool8_t vkr_ui_system_init(RendererFrontend *rf, VkrUiSystem *system) {
  if (!rf || !system) {
    return false_v;
  }
  MemZero(system, sizeof(*system));
  system->text_slots =
      array_create_VkrUiTextSlot(&rf->allocator, VKR_UI_SYSTEM_MAX_TEXTS);
  if (!system->text_slots.data) {
    return false_v;
  }
  MemZero(system->text_slots.data,
          sizeof(VkrUiTextSlot) * (uint64_t)system->text_slots.length);
  system->screen_width = rf->last_window_width;
  system->screen_height = rf->last_window_height;
  system->offscreen_content_scale = 1.0f;
  system->offscreen_content_scale_revision = 1u;
  const VkrWindowContentScale content_scale =
      vkr_ui_system_get_content_scale(rf, system);
  system->text_content_scale = content_scale.value;
  system->content_scale_revision = content_scale.revision;
  system->initialized = true_v;
  return true_v;
}

void vkr_ui_system_shutdown(RendererFrontend *rf, VkrUiSystem *system) {
  if (!rf || !system) {
    return;
  }

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (slot->active) {
      vkr_ui_text_destroy(&slot->text);
      slot->active = false_v;
    }
  }
  array_destroy_VkrUiTextSlot(&system->text_slots);

  system->initialized = false_v;
}

void vkr_ui_system_resize(RendererFrontend *rf, VkrUiSystem *system,
                          uint32_t width, uint32_t height) {
  if (!rf || !system) {
    return;
  }

  rf->globals.ui_view = mat4_identity();
  rf->globals.ui_projection =
      mat4_ortho(0.0f, (float32_t)width, (float32_t)height, 0.0f, -1.0f, 1.0f);

  uint32_t layout_width = width;
  uint32_t layout_height = height;
  if (system->offscreen_enabled && system->offscreen_width > 0 &&
      system->offscreen_height > 0) {
    layout_width = system->offscreen_width;
    layout_height = system->offscreen_height;
  }

  system->screen_width = layout_width;
  system->screen_height = layout_height;
  (void)vkr_ui_system_update_text_content_scale(
      system, vkr_ui_system_get_content_scale(rf, system));

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_system_position_slot(slot, layout_width, layout_height,
                                system->text_content_scale);
  }
}

void vkr_ui_system_set_offscreen_size(RendererFrontend *rf, VkrUiSystem *system,
                                      bool8_t enabled, uint32_t width,
                                      uint32_t height) {
  if (!rf || !system) {
    return;
  }

  system->offscreen_enabled = enabled;
  system->offscreen_width = width;
  system->offscreen_height = height;
  vkr_ui_system_refresh_layout(rf, system);
}

void vkr_ui_system_set_offscreen_content_scale(RendererFrontend *rf,
                                               VkrUiSystem *system,
                                               float32_t content_scale) {
  if (!rf || !system || !isfinite(content_scale) || content_scale <= 0.0f ||
      system->offscreen_content_scale == content_scale) {
    return;
  }
  system->offscreen_content_scale = content_scale;
  system->offscreen_content_scale_revision++;
  if (system->offscreen_content_scale_revision == 0u) {
    system->offscreen_content_scale_revision = 1u;
  }
  if (system->offscreen_enabled) {
    vkr_ui_system_refresh_layout(rf, system);
  }
}

bool8_t vkr_ui_system_text_create(RendererFrontend *rf, VkrUiSystem *system,
                                  const VkrUiTextCreateData *payload,
                                  uint32_t *out_text_id) {
  if (!rf || !system || !payload) {
    return false_v;
  }

  uint32_t text_id = payload->text_id;
  VkrUiTextSlot *slot = NULL;
  if (text_id == VKR_INVALID_ID) {
    if (!vkr_ui_system_find_free_slot(system, &text_id, &slot)) {
      return false_v;
    }
  } else {
    if (!vkr_ui_system_ensure_slot(system, text_id, &slot)) {
      return false_v;
    }
  }

  if (slot->active) {
    vkr_ui_text_destroy(&slot->text);
    slot->active = false_v;
  }

  const VkrUiTextConfig *config = payload->config;
  VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_ui_text_create(&rf->allocator, &rf->font_system, payload->content,
                          config, &slot->text, &text_err)) {
    String8 err = vkr_renderer_get_error_string(text_err);
    log_error("Failed to create UI text: %s", string8_cstr(&err));
    return false_v;
  }

  slot->active = true_v;
  slot->anchor = payload->anchor;
  slot->padding = payload->padding;
  vkr_ui_text_set_content_scale(&slot->text, system->text_content_scale);

  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  vkr_ui_system_get_layout_size(rf, system, &layout_width, &layout_height);
  vkr_ui_system_position_slot(slot, layout_width, layout_height,
                              system->text_content_scale);

  if (out_text_id) {
    *out_text_id = text_id;
  }
  return true_v;
}

bool8_t vkr_ui_system_text_update(RendererFrontend *rf, VkrUiSystem *system,
                                  uint32_t text_id, String8 content) {
  if (!rf || !system) {
    return false_v;
  }

  VkrUiTextSlot *slot = vkr_ui_system_get_active_slot(system, text_id);
  if (!slot) {
    log_warn("UI text id %u not found for update", text_id);
    return false_v;
  }

  if (!vkr_ui_text_set_content(&slot->text, content)) {
    log_error("Failed to update UI text content");
    return false_v;
  }

  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  vkr_ui_system_get_layout_size(rf, system, &layout_width, &layout_height);
  vkr_ui_system_position_slot(slot, layout_width, layout_height,
                              system->text_content_scale);
  return true_v;
}

bool8_t vkr_ui_system_text_destroy(RendererFrontend *rf, VkrUiSystem *system,
                                   uint32_t text_id) {
  (void)rf;

  if (!system) {
    return false_v;
  }

  VkrUiTextSlot *slot = vkr_ui_system_get_active_slot(system, text_id);
  if (!slot) {
    log_warn("UI text id %u not found for destroy", text_id);
    return false_v;
  }

  vkr_ui_text_destroy(&slot->text);
  slot->active = false_v;
  return true_v;
}

uint32_t vkr_ui_system_prepare_text_draws(VkrUiSystem *system,
                                          VkrPreparedTextDraw *out_draws,
                                          uint32_t capacity) {
  uint32_t count = 0;
  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active || !vkr_ui_text_prepare_geometry(&slot->text) ||
        slot->text.geometry.index_count == 0)
      continue;
    if (count == capacity) {
      log_error("UI text packet capacity exceeded (%u)", capacity);
      break;
    }
    VkrFont *font = slot->text.resolved_font;
    if (!font || font->atlas.id == 0 ||
        font->atlas.generation == VKR_INVALID_ID)
      continue;
    Vec2 unit_range = {0};
    uint32_t font_mode = 0;
    if (font->type == VKR_FONT_TYPE_MTSDF) {
      font_mode = 1;
      unit_range = font->mtsdf_unit_range;
    }
    out_draws[count++] = (VkrPreparedTextDraw){
        .vertices = slot->text.geometry.vertices,
        .vertex_count = slot->text.geometry.vertex_count,
        .indices = slot->text.geometry.indices,
        .index_count = slot->text.geometry.index_count,
        .max_index = slot->text.geometry.vertex_count - 1u,
        .atlas = font->atlas,
        .model = vkr_transform_get_world(&slot->text.transform),
        .unit_range = unit_range,
        .font_mode = font_mode,
        .object_id =
            vkr_picking_encode_id(VKR_PICKING_ID_KIND_UI_TEXT, (uint32_t)i),
        .revision = slot->text.geometry.revision,
    };
  }
  return count;
}
