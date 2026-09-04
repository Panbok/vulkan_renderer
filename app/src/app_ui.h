#pragma once

#include "vkr_sample_runtime.h"

typedef struct VkrAppUi {
  bool8_t visible;
} VkrAppUi;

VkrSampleUiClient vkr_app_ui_client(VkrAppUi *ui);
