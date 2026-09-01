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
