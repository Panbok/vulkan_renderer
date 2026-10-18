#include "app_ui.h"
#include "fps_module.h"
#include "platform/vkr_crash.h"
#include "platform/vkr_entry.h"
#include "vkr_sample_runtime.h"

VKR_MAIN(argc, argv) {
  /* This program opens no session log, so a crash report goes to stderr,
     where the harness keeps it. */
  (void)vkr_crash_install(NULL, "renderer");
  VkrAppUi ui = {0};
  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = "VKR Renderer";
  config.ui = vkr_app_ui_client(&ui);
  static const VkrModuleEntry script_modules[] = {vkr_module_fps};
  config.script_modules = script_modules;
  config.script_module_count = ArrayCount(script_modules);
  return vkr_sample_runtime_run(argc, argv, &config);
}
