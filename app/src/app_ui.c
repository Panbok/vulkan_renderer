#include "app_ui.h"

#include "core/logger.h"
#include "debug_overlay.h"

static bool8_t app_ui_initialize(void *state, VkrUiDockTree *dock,
                                 VkrUiSystem *system) {
  (void)system;
  (void)dock;
  VkrAppUi *ui = state;
  ui->visible = true_v;
  log_info("Debug UI enabled; press F6 to toggle it");
  return true_v;
}

static void app_ui_handle_input(void *state, const InputState *input) {
  VkrAppUi *ui = state;
  if (!input_key_just_released(input, KEY_F6))
    return;

  ui->visible = !ui->visible;
  log_info("Debug UI %s", ui->visible ? "enabled" : "disabled");
}

static VkrUiDockInputCapture app_ui_build(void *state,
                                          const VkrSampleUiFrame *frame) {
  const VkrAppUi *ui = state;
  if (ui->visible) {
    String8 performance = string8_create_formatted(
        frame->ui->frame_allocator, "%.*s\nRender %ux%u\nOutput %ux%u\n%.*s",
        (int32_t)frame->text.performance.length, frame->text.performance.str,
        frame->scene_render_width, frame->scene_render_height,
        frame->scene_output_width, frame->scene_output_height,
        (int32_t)frame->text.system.length, frame->text.system.str);
    vkr_debug_overlay_build(frame->ui, frame->text.camera, performance);
  }
  return (VkrUiDockInputCapture){0};
}

static bool8_t app_ui_shutdown(void *state, const VkrUiDockTree *dock,
                               VkrUiSystem *system) {
  (void)system;
  (void)state;
  (void)dock;
  return true_v;
}

VkrSampleUiClient vkr_app_ui_client(VkrAppUi *ui) {
  return (VkrSampleUiClient){
      .state = ui,
      .initialize = app_ui_initialize,
      .handle_input = app_ui_handle_input,
      .build = app_ui_build,
      .shutdown = app_ui_shutdown,
  };
}
