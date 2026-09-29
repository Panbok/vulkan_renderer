#include "app_ui.h"
#include "fps_module.h"
#include "platform/vkr_entry.h"
#include "vkr_sample_runtime.h"

VKR_MAIN(argc, argv) {
  VkrAppUi ui = {0};
  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = "VKR Renderer";
  config.ui = vkr_app_ui_client(&ui);
  static const VkrScriptModuleEntry script_modules[] = {vkr_script_module_fps};
  config.script_modules = script_modules;
  config.script_module_count = ArrayCount(script_modules);
  return vkr_sample_runtime_run(argc, argv, &config);
}
