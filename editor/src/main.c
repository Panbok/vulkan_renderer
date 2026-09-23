#include "editor_application.h"
#include "platform/vkr_entry.h"

VKR_MAIN(argc, argv) {
  VkrEditorApplication editor = {0};
  VkrSampleRuntimeConfig config =
      vkr_editor_application_config(&editor, argc, argv);
  return vkr_sample_runtime_run(argc, argv, &config);
}
