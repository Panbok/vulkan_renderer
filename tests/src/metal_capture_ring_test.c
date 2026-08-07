#include "metal_capture_ring_test.h"

#include "renderer/metal/vkr_metal_capture_ring.h"

#include <assert.h>
#include <stdio.h>

static VkrCaptureBackendItemPlan capture_plan(VkrCaptureChannelId channel,
                                              uint64_t offset, uint64_t size) {
  return (VkrCaptureBackendItemPlan){
      .result = {.channel = channel,
                 .width = (uint32_t)size,
                 .height = 1,
                 .row_pitch = size,
                 .format = VKR_TEXTURE_FORMAT_R8_UNORM,
                 .value_kind = VKR_CAPTURE_VALUE_COLOR,
                 .data_size = size},
      .buffer_offset = offset,
  };
}

static void test_capture_pending_ready_release(void) {
  printf("  Running test_capture_pending_ready_release...\n");
  uint8_t storage[2 * 512] = {0};
  uint8_t staging[512] = {0};
  VkrMetalCaptureRing ring = {0};
  assert(vkr_metal_capture_ring_init(&ring, 2, 512, storage, sizeof(storage)));
  VkrCaptureItemRequest items[] = {{.channel = 0}, {.channel = 1}};
  VkrCaptureBatchRequest request = {
      .request_id = 1, .items = items, .item_count = ArrayCount(items)};
  VkrCaptureBackendItemPlan plans[] = {capture_plan(0, 0, 4),
                                       capture_plan(1, 256, 8)};
  for (uint32_t i = 0; i < 4; ++i)
    staging[i] = (uint8_t)(10 + i);
  for (uint32_t i = 0; i < 8; ++i)
    staging[256 + i] = (uint8_t)(20 + i);
  assert(vkr_metal_capture_ring_reserve(&ring, &request, plans, 17) ==
         VKR_RENDERER_ERROR_NONE);
  assert(vkr_metal_capture_ring_submit(&ring, request.request_id, 5, staging));

  VkrCapturePollResult poll = {0};
  assert(vkr_metal_capture_ring_poll(&ring, request.request_id, 4, &poll) ==
         VKR_CAPTURE_STATUS_PENDING);
  assert(!poll.items && poll.source_frame_index == 17 &&
         poll.submit_serial == 5);
  assert(vkr_metal_capture_ring_poll(&ring, request.request_id, 5, &poll) ==
         VKR_CAPTURE_STATUS_READY);
  assert(poll.item_count == 2 && poll.items &&
         MemCompare(poll.items[0].data, staging, 4) == 0 &&
         MemCompare(poll.items[1].data, staging + 256, 8) == 0);
  MemZero(staging, sizeof(staging));
  assert(((const uint8_t *)poll.items[0].data)[0] == 10 &&
         ((const uint8_t *)poll.items[1].data)[0] == 20);
  assert(vkr_metal_capture_ring_release(&ring, request.request_id));
  assert(vkr_metal_capture_ring_poll(&ring, request.request_id, 5, &poll) ==
         VKR_CAPTURE_STATUS_NOT_FOUND);
  printf("  test_capture_pending_ready_release PASSED\n");
}

static void test_capture_capacity_and_abandoned_retirement(void) {
  printf("  Running test_capture_capacity_and_abandoned_retirement...\n");
  uint8_t storage[2 * 256] = {0};
  uint8_t staging[256] = {0};
  VkrMetalCaptureRing ring = {0};
  assert(vkr_metal_capture_ring_init(&ring, 2, 256, storage, sizeof(storage)));
  VkrCaptureItemRequest item = {.channel = 0};
  VkrCaptureBackendItemPlan plan = capture_plan(0, 0, 4);
  VkrCaptureBatchRequest first = {
      .request_id = 10, .items = &item, .item_count = 1};
  VkrCaptureBatchRequest second = first;
  second.request_id = 11;
  VkrCaptureBatchRequest third = first;
  third.request_id = 12;
  assert(vkr_metal_capture_ring_reserve(&ring, &first, &plan, 1) ==
         VKR_RENDERER_ERROR_NONE);
  assert(vkr_metal_capture_ring_submit(&ring, first.request_id, 7, staging));
  assert(vkr_metal_capture_ring_release(&ring, first.request_id));
  assert(vkr_metal_capture_ring_reserve(&ring, &second, &plan, 2) ==
         VKR_RENDERER_ERROR_NONE);
  assert(vkr_metal_capture_ring_reserve(&ring, &third, &plan, 3) ==
         VKR_RENDERER_ERROR_CAPTURE_BUSY);
  vkr_metal_capture_ring_collect(&ring, 7);
  assert(vkr_metal_capture_ring_reserve(&ring, &third, &plan, 3) ==
         VKR_RENDERER_ERROR_NONE);
  assert(vkr_metal_capture_ring_release(&ring, second.request_id));
  assert(vkr_metal_capture_ring_release(&ring, third.request_id));
  printf("  test_capture_capacity_and_abandoned_retirement PASSED\n");
}

static void test_capture_failure_and_validation(void) {
  printf("  Running test_capture_failure_and_validation...\n");
  uint8_t storage[64] = {0};
  VkrMetalCaptureRing ring = {0};
  assert(vkr_metal_capture_ring_init(&ring, 1, 64, storage, sizeof(storage)));
  VkrCaptureItemRequest item = {.channel = 0};
  VkrCaptureBatchRequest request = {
      .request_id = 4, .items = &item, .item_count = 1};
  VkrCaptureBackendItemPlan oversized = capture_plan(0, 0, 65);
  assert(vkr_metal_capture_ring_reserve(&ring, &request, &oversized, 0) ==
         VKR_RENDERER_ERROR_OUT_OF_MEMORY);
  VkrCaptureBackendItemPlan plan = capture_plan(0, 0, 16);
  assert(vkr_metal_capture_ring_reserve(&ring, &request, &plan, 9) ==
         VKR_RENDERER_ERROR_NONE);
  assert(vkr_metal_capture_ring_fail(
      &ring, request.request_id, VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED));
  VkrCapturePollResult poll = {0};
  assert(vkr_metal_capture_ring_poll(&ring, request.request_id, 0, &poll) ==
         VKR_CAPTURE_STATUS_FAILED);
  assert(poll.error == VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
  assert(vkr_metal_capture_ring_release(&ring, request.request_id));
  assert(vkr_metal_capture_ring_reserve(&ring, &request, &plan, 9) ==
         VKR_RENDERER_ERROR_INVALID_PARAMETER);
  printf("  test_capture_failure_and_validation PASSED\n");
}

bool32_t run_metal_capture_ring_tests(void) {
  printf("Running Metal capture ring tests...\n");
  test_capture_pending_ready_release();
  test_capture_capacity_and_abandoned_retirement();
  test_capture_failure_and_validation();
  printf("Metal capture ring tests PASSED\n");
  return true_v;
}
