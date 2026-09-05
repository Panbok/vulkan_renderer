#include "debug_overlay_test.h"

#include "app_ui.h"
#include <assert.h>
#include <stdio.h>

static void test_app_ui_client_owns_visibility(void) {
  printf("  Running test_app_ui_client_owns_visibility...\n");
  VkrAppUi ui = {0};
  VkrSampleUiClient client = vkr_app_ui_client(&ui);
  InputState input = {0};

  assert(client.state == &ui);
  assert(client.initialize && client.handle_input && client.build &&
         client.shutdown);
  client.initialize(client.state, NULL);
  assert(ui.visible);

  input.previous_keys.keys[KEY_F6] = true_v;
  client.handle_input(client.state, &input);
  assert(!ui.visible);
  assert(client.shutdown(client.state, NULL));

  printf("  test_app_ui_client_owns_visibility PASSED\n");
}

bool32_t run_debug_overlay_tests(void) {
  printf("Running debug overlay tests...\n");
  test_app_ui_client_owns_visibility();
  printf("Debug overlay tests PASSED\n");
  return true;
}
