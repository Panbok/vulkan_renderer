#include "picking_state_test.h"

#include "renderer/systems/vkr_picking_system.h"

#include <assert.h>
#include <stdio.h>

bool32_t run_picking_state_tests(void) {
  printf("--- Starting Picking State Tests ---\n");

  VkrPickingContext picking = {0};
  assert(vkr_picking_is_pending(&picking) == false_v);

  picking.state = VKR_PICKING_STATE_RENDER_PENDING;
  assert(vkr_picking_is_pending(&picking) == true_v);

  picking.state = VKR_PICKING_STATE_RENDER_RECORDED;
  assert(vkr_picking_is_pending(&picking) == true_v);
  vkr_picking_cancel(&picking);
  assert(picking.state == VKR_PICKING_STATE_IDLE);
  assert(vkr_picking_is_pending(&picking) == false_v);

  picking.state = VKR_PICKING_STATE_READBACK_PENDING;
  assert(vkr_picking_is_pending(&picking) == true_v);

  picking.state = VKR_PICKING_STATE_RESULT_READY;
  assert(vkr_picking_is_pending(&picking) == false_v);
  assert(vkr_picking_is_pending(NULL) == false_v);

  printf("--- Picking State Tests Completed ---\n");
  return true;
}
