#pragma once

#include "renderer/vkr_frame_input.h"

/* Private recording input. The renderer owns derived frame values; input
 * arrays remain borrowed through the synchronous render call. GPU resources
 * retain their independent last-submission lifetime. */
typedef struct VkrPreparedFrame {
  VkrFrameInput input;
  VkrTemporalFrame temporal;
  VkrExposureFrame exposure;
  VkrBloomFrame bloom;
  VkrGtaoFrame gtao;
} VkrPreparedFrame;
