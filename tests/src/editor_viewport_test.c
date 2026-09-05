#include "editor_viewport_test.h"

#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/vkr_exposure.h"
#include "renderer/vkr_renderer_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_editor_viewport_mapping(void) {
  printf("  Running test_editor_viewport_mapping...\n");
  VkrViewportMapping mapping = {0};
  assert(vkr_editor_viewport_compute_mapping(
      640u, 480u, VKR_VIEWPORT_FIT_STRETCH, 1.0f, &mapping));
  assert(mapping.panel_rect_px.x == 228.0f);
  assert(mapping.panel_rect_px.y == 40.0f);
  assert(mapping.panel_rect_px.z == 124.0f);
  assert(mapping.panel_rect_px.w == 252.0f);
  assert(memcmp(&mapping.image_rect_px, &mapping.panel_rect_px,
                sizeof(mapping.image_rect_px)) == 0);
  assert(mapping.target_width == 124u);
  assert(mapping.target_height == 252u);

  VkrEditorPassPayload payload = {0};
  assert(vkr_editor_viewport_build_payload(&mapping, &payload));
  assert(memcmp(&payload.image_rect_px, &mapping.image_rect_px,
                sizeof(payload.image_rect_px)) == 0);

  assert(vkr_editor_viewport_compute_mapping(
      640u, 480u, VKR_VIEWPORT_FIT_CONTAIN, 0.5f, &mapping));
  assert(mapping.target_width == 62u);
  assert(mapping.target_height == 126u);
  assert(mapping.image_rect_px.x == 228.0f);
  assert(mapping.image_rect_px.y == 40.0f);
  assert(mapping.image_rect_px.z == 124.0f);
  assert(mapping.image_rect_px.w == 252.0f);

  const Vec4 dock_panel = {100.0f, 50.0f, 400.0f, 200.0f};
  assert(vkr_editor_viewport_mapping_from_panel_rect(
      dock_panel, VKR_VIEWPORT_FIT_STRETCH, 0.5f, &mapping));
  assert(memcmp(&mapping.panel_rect_px, &dock_panel, sizeof(dock_panel)) == 0);
  assert(mapping.target_width == 200u && mapping.target_height == 100u);
  assert(vkr_editor_viewport_mapping_from_panel_rect_and_target(
      dock_panel, VKR_VIEWPORT_FIT_CONTAIN, 160u, 160u, &mapping));
  assert(mapping.target_width == 160u && mapping.target_height == 160u);
  assert(mapping.image_rect_px.x == 200.0f);
  assert(mapping.image_rect_px.y == 50.0f);
  assert(mapping.image_rect_px.z == 200.0f);
  assert(mapping.image_rect_px.w == 200.0f);
  assert(!vkr_editor_viewport_mapping_from_panel_rect_and_target(
      dock_panel, VKR_VIEWPORT_FIT_STRETCH, 0u, 100u, &mapping));
  assert(!vkr_editor_viewport_mapping_from_panel_rect(
      (Vec4){0.0f, 0.0f, NAN, 10.0f}, VKR_VIEWPORT_FIT_STRETCH, 1.0f,
      &mapping));
  printf("  test_editor_viewport_mapping PASSED\n");
}

static VkrFrameInput
editor_viewport_packet(const VkrEditorPassPayload *payload) {
  return (VkrFrameInput){
      .version = VKR_FRAME_INPUT_VERSION,
      .frame =
          {
              .window_width = 640u,
              .window_height = 480u,
              .viewport_width = 124u,
              .viewport_height = 252u,
              .editor_enabled = true_v,
          },
      .globals = {.manual_exposure = VKR_DEFAULT_EXPOSURE},
      .editor = payload,
  };
}

static void test_editor_viewport_packet_validation(void) {
  printf("  Running test_editor_viewport_packet_validation...\n");
  VkrEditorPassPayload payload = {
      .image_rect_px = {228.0f, 40.0f, 124.0f, 252.0f},
  };
  VkrFrameInput packet = editor_viewport_packet(&payload);
  VkrValidationError validation = {0};
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_NONE);

  packet.frame.editor_enabled = false_v;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.editor") == 0);

  packet.frame.editor_enabled = true_v;
  payload.image_rect_px.x = 228.5f;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  assert(strcmp(validation.field_path, "packet.editor.image_rect_px") == 0);

  payload.image_rect_px = (Vec4){600.0f, 40.0f, 124.0f, 252.0f};
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);

  payload.image_rect_px = (Vec4){NAN, 40.0f, 124.0f, 252.0f};
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);

  payload.image_rect_px = (Vec4){228.0f, 40.0f, 124.0f, 252.0f};
  packet.frame.viewport_width = 0u;
  assert(vkr_frame_input_validate(&packet, &validation) ==
         VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
  printf("  test_editor_viewport_packet_validation PASSED\n");
}

static void test_scene_output_extent_restore(void) {
  printf("  Running test_scene_output_extent_restore...\n");
  VkrRenderer renderer = {
      .backend_type = VKR_RENDERER_BACKEND_TYPE_METAL,
      .render_scale = 0.5f,
      .scene_output_width = 800u,
      .scene_output_height = 600u,
      .render_width = 400u,
      .render_height = 300u,
      .last_window_width = 800u,
      .last_window_height = 600u,
  };
  assert(vkr_renderer_set_scene_output_extent(&renderer, 400u, 300u) ==
         VKR_RENDERER_ERROR_NONE);
  assert(renderer.scene_output_extent_overridden);
  assert(renderer.scene_output_width == 400u &&
         renderer.scene_output_height == 300u);
  assert(renderer.render_width == 200u && renderer.render_height == 150u);

  renderer.last_window_width = 1024u;
  renderer.last_window_height = 768u;
  assert(vkr_renderer_restore_scene_output_extent(&renderer) ==
         VKR_RENDERER_ERROR_NONE);
  assert(!renderer.scene_output_extent_overridden);
  assert(renderer.scene_output_width == 1024u &&
         renderer.scene_output_height == 768u);
  assert(renderer.render_width == 512u && renderer.render_height == 384u);
  assert(vkr_renderer_restore_scene_output_extent(&renderer) ==
         VKR_RENDERER_ERROR_NONE);
  printf("  test_scene_output_extent_restore PASSED\n");
}

bool32_t run_editor_viewport_tests(void) {
  printf("Running editor viewport tests...\n");
  test_editor_viewport_mapping();
  test_editor_viewport_packet_validation();
  test_scene_output_extent_restore();
  printf("Editor viewport tests PASSED\n");
  return true_v;
}
