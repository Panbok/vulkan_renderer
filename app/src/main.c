#include "app_ui.h"
#include "platform/vkr_entry.h"
#include "vkr_sample_runtime.h"

VKR_MAIN(argc, argv) {
  VkrAppUi ui = {0};
  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = "VKR Renderer";
  config.ui = vkr_app_ui_client(&ui);
  return vkr_sample_runtime_run(argc, argv, &config);
}
