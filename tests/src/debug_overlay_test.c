#include "debug_overlay_test.h"

#include "app_ui.h"
#include "math/vkr_math.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_app_ui_client_owns_visibility(void) {
  printf("  Running test_app_ui_client_owns_visibility...\n");
  VkrAppUi ui = {0};
  VkrSampleUiClient client = vkr_app_ui_client(&ui);
  InputState input = {0};

  assert(client.state == &ui);
  assert(client.initialize && client.handle_input && client.build &&
         client.shutdown);
  assert(client.initialize(client.state, NULL, NULL));
  assert(ui.visible);

  input.previous_keys.keys[KEY_F6] = true_v;
  client.handle_input(client.state, &input);
  assert(!ui.visible);
  assert(client.shutdown(client.state, NULL, NULL));

  printf("  test_app_ui_client_owns_visibility PASSED\n");
}

/* A pixel-space oracle catches distance/render-scale coupling without needing
   GPU readback or rounded screenshot measurements. Geometry is not rendered. */
static void test_gizmo_display_size(void) {
  VkrGizmoSystem gizmo = {
      .mode = VKR_GIZMO_MODE_TRANSLATE,
      .initialized = true_v,
      .visible = true_v,
      .config = {.screen_size = 150.0f},
      .orientation = vkr_quat_identity(),
  };
  for (uint32_t i = 0u; i < VKR_EDITOR_OVERLAY_DRAW_MAX; ++i)
    gizmo.geometries[i] = (VkrGeometryHandle){.id = i + 1u, .generation = 1u};
  const Mat4 projection =
      mat4_perspective(vkr_to_radians(60.0f), 4.0f / 3.0f, 0.1f, 100.0f);
  const float32_t depths[] = {3.0f, 30.0f};
  VkrEditorOverlayDraw draws[VKR_EDITOR_OVERLAY_DRAW_MAX];
  for (uint32_t extent = 1u; extent <= 2u; ++extent) {
    for (uint32_t scale = 1u; scale <= 2u; ++scale) {
      const VkrViewportMapping mapping = {
          .image_rect_px = {40.0f, 80.0f, 800.0f * extent, 600.0f * extent},
          .target_width = 800u * extent / scale,
          .target_height = 600u * extent / scale,
      };
      for (uint32_t depth = 0u; depth < ArrayCount(depths); ++depth) {
        gizmo.position = vec3_new(0.0f, 0.0f, -depths[depth]);
        const uint32_t count = vkr_gizmo_system_build_draws(
            &gizmo, mat4_identity(), projection, &mapping, draws);
        assert(count == 9u);
        const Mat4 mvp = mat4_mul(projection, draws[0].model);
        const Vec4 origin = mat4_mul_vec4(mvp, vec4_new(0, 0, 0, 1));
        const Vec4 tip = mat4_mul_vec4(mvp, vec4_new(0, 1, 0, 1));
        const float32_t pixels = fabsf(tip.y / tip.w - origin.y / origin.w) *
                                 mapping.image_rect_px.w * 0.5f;
        assert(fabsf(pixels - 150.0f) < 0.001f);
      }
    }
  }
  const VkrViewportMapping mapping = {.image_rect_px = {0, 0, 800, 600},
                                      .target_width = 800,
                                      .target_height = 600};
  gizmo.position = vec3_new(0, 0, 3);
  assert(vkr_gizmo_system_build_draws(&gizmo, mat4_identity(), projection,
                                      &mapping, draws) == 0u);
  gizmo.position.z = -3;
  gizmo.visible = false_v;
  assert(vkr_gizmo_system_build_draws(&gizmo, mat4_identity(), projection,
                                      &mapping, draws) == 0u);
  printf("  test_gizmo_display_size PASSED\n");
}

bool32_t run_debug_overlay_tests(void) {
  printf("Running debug overlay tests...\n");
  test_app_ui_client_owns_visibility();
  test_gizmo_display_size();
  printf("Debug overlay tests PASSED\n");
  return true;
}
