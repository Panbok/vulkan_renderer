#include "app_ui.h"
#include "vkr_sample_runtime.h"

int main(int argc, char **argv) {
  VkrAppUi ui = {0};
  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = "VKR Renderer";
  config.ui = vkr_app_ui_client(&ui);
  return vkr_sample_runtime_run(argc, argv, &config);
}
