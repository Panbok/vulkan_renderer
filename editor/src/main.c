#include "editor_application.h"

int main(int argc, char **argv) {
  VkrEditorApplication editor = {0};
  VkrSampleRuntimeConfig config =
      vkr_editor_application_config(&editor, argc, argv);
  return vkr_sample_runtime_run(argc, argv, &config);
}
