#include "core/vkr_window.h"

#include "platform/vkr_window_internal.h"

#include <math.h>

static uint32_t vkr_window_float_bits(float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  return bits;
}

static float32_t vkr_window_bits_float(uint32_t bits) {
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static uint64_t vkr_window_pack_content_scale(uint32_t revision,
                                              float32_t value) {
  return ((uint64_t)revision << 32u) | (uint64_t)vkr_window_float_bits(value);
}

void vkr_window_content_scale_init(VkrWindow *window) {
  assert_log(window != NULL, "Window is NULL");
  vkr_atomic_uint64_store(&window->content_scale_state,
                          vkr_window_pack_content_scale(1u, 1.0f),
                          VKR_MEMORY_ORDER_RELEASE);
}

bool8_t vkr_window_content_scale_publish(VkrWindow *window,
                                         float32_t content_scale) {
  assert_log(window != NULL, "Window is NULL");
  if (!isfinite(content_scale) || content_scale <= 0.0f) {
    return false_v;
  }

  const uint32_t scale_bits = vkr_window_float_bits(content_scale);
  uint64_t expected = vkr_atomic_uint64_load(&window->content_scale_state,
                                             VKR_MEMORY_ORDER_ACQUIRE);
  for (;;) {
    if ((uint32_t)expected == scale_bits) {
      return false_v;
    }
    uint32_t revision = (uint32_t)(expected >> 32u) + 1u;
    if (revision == 0u) {
      revision = 1u;
    }
    const uint64_t desired = ((uint64_t)revision << 32u) | (uint64_t)scale_bits;
    if (vkr_atomic_uint64_compare_exchange(
            &window->content_scale_state, &expected, desired,
            VKR_MEMORY_ORDER_ACQ_REL, VKR_MEMORY_ORDER_ACQUIRE)) {
      return true_v;
    }
  }
}

VkrWindowContentScale vkr_window_get_content_scale(const VkrWindow *window) {
  assert_log(window != NULL, "Window is NULL");
  const uint64_t packed = vkr_atomic_uint64_load(&window->content_scale_state,
                                                 VKR_MEMORY_ORDER_ACQUIRE);
  const float32_t value = vkr_window_bits_float((uint32_t)packed);
  if (!isfinite(value) || value <= 0.0f) {
    return (VkrWindowContentScale){.value = 1.0f, .revision = 0u};
  }
  return (VkrWindowContentScale){
      .value = value,
      .revision = (uint32_t)(packed >> 32u),
  };
}

vkr_internal VkrSurfaceSize
vkr_window_render_surface_pixel_size(void *context) {
  VkrWindow *window = context;
  /* The render thread may ask; only the window's thread may query the
     platform, so it publishes the size. Before the first publication the
     caller is the window's thread creating the renderer. */
  const uint64_t packed = vkr_atomic_uint64_load(&window->pixel_size_state,
                                                 VKR_MEMORY_ORDER_ACQUIRE);
  if (packed == 0u) {
    const VkrWindowPixelSize size = vkr_window_get_pixel_size(window);
    return (VkrSurfaceSize){.width = size.width, .height = size.height};
  }
  return (VkrSurfaceSize){
      .width = (uint32_t)(packed >> 32u),
      .height = (uint32_t)packed,
  };
}

void vkr_window_publish_pixel_size(VkrWindow *window) {
  if (!window || !window->platform_state) {
    return;
  }
  const VkrWindowPixelSize size = vkr_window_get_pixel_size(window);
  vkr_atomic_uint64_store(&window->pixel_size_state,
                          ((uint64_t)size.width << 32u) | size.height,
                          VKR_MEMORY_ORDER_RELEASE);
}

vkr_internal VkrDisplayOutputSnapshot
vkr_window_render_surface_display_output(void *context) {
  return vkr_window_get_display_output((VkrWindow *)context);
}

VkrNativeSurface vkr_window_get_render_surface(VkrWindow *window) {
  assert_log(window && window->platform_state, "Window not initialized");
  VkrNativeSurface surface = {
      .context = window,
      .pixel_size = vkr_window_render_surface_pixel_size,
      .display_output_snapshot = vkr_window_render_surface_display_output,
  };
#if defined(PLATFORM_APPLE)
  surface.metal_layer = vkr_window_get_metal_layer(window);
#elif defined(PLATFORM_WINDOWS)
  surface.win32_instance = vkr_window_get_win32_instance(window);
  surface.win32_window = vkr_window_get_win32_handle(window);
#endif
  return surface;
}

bool8_t vkr_window_file_drop_begin(VkrWindow *window, int32_t x, int32_t y) {
  if (!window || vkr_atomic_bool_load(&window->file_drop_pending,
                                      VKR_MEMORY_ORDER_ACQUIRE)) {
    return false_v;
  }
  window->file_drop.count = 0u;
  window->file_drop.x = x;
  window->file_drop.y = y;
  return true_v;
}

void vkr_window_file_drop_add(VkrWindow *window, const char *path,
                              uint64_t length) {
  VkrWindowFileDrop *drop = &window->file_drop;
  if (!path || !length || length >= VKR_WINDOW_DROP_PATH_CAPACITY ||
      drop->count == VKR_WINDOW_DROP_PATH_MAX) {
    return;
  }
  MemCopy(drop->paths[drop->count], path, length);
  drop->paths[drop->count][length] = '\0';
  ++drop->count;
}

void vkr_window_file_drop_publish(VkrWindow *window) {
  if (window->file_drop.count) {
    vkr_atomic_bool_store(&window->file_drop_pending, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
  }
}

bool8_t vkr_window_take_file_drop(VkrWindow *window, VkrWindowFileDrop *out) {
  if (!window || !out ||
      !vkr_atomic_bool_load(&window->file_drop_pending,
                            VKR_MEMORY_ORDER_ACQUIRE)) {
    return false_v;
  }
  MemCopy(out, &window->file_drop, sizeof(*out));
  vkr_atomic_bool_store(&window->file_drop_pending, false_v,
                        VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}
