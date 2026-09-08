#pragma once

#include "defines.h"

typedef enum VkrDisplayOutputMode {
  VKR_DISPLAY_OUTPUT_SDR = 0,
  VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR,
} VkrDisplayOutputMode;

/** Platform-owned display capability, copied at the frame boundary. Headroom
 * is relative to current SDR white; output_scale converts that white to native
 * extended-linear units (1 for Metal EDR, SDR-white nits / 80 for scRGB). */
typedef struct VkrDisplayOutputSnapshot {
  float32_t headroom;
  float32_t output_scale;
  uint64_t revision;
  bool8_t available;
} VkrDisplayOutputSnapshot;

/** Selected output contract shared by final presentation and UI shaders. */
typedef struct VkrDisplayOutputParams {
  float32_t headroom;
  float32_t output_scale;
  uint32_t extended_linear;
  uint32_t reserved;
} VkrDisplayOutputParams;

_Static_assert(sizeof(VkrDisplayOutputParams) == 16,
               "Display output parameters must occupy 16 bytes");

VkrDisplayOutputParams
vkr_display_output_resolve(VkrDisplayOutputMode mode,
                           VkrDisplayOutputSnapshot snapshot, bool8_t windowed);
