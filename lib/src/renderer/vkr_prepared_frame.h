#pragma once

#include "renderer/vkr_frame_input.h"

/* Private recording input. The renderer owns derived frame values; input
 * arrays remain borrowed through the synchronous render call. GPU resources
 * retain their independent last-submission lifetime. */
typedef struct VkrPreparedFrame {
  VkrFrameInput input;
  bool8_t scene_rendering;
  bool8_t editor_image_available;
  uint32_t editor_image_width;
  uint32_t editor_image_height;
  Vec4 editor_image_rect_px;
  VkrTemporalFrame temporal;
  VkrExposureFrame exposure;
  VkrBloomFrame bloom;
  VkrGtaoFrame gtao;
} VkrPreparedFrame;
