#include "bindless_vulkan_test.h"

#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_gpu_submit_ring.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_memory.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_wsi.h"

#include <assert.h>
#include <stdio.h>

typedef struct PresentCase {
  VkResult result;
  VkrBindlessVulkanPresentResult expected;
} PresentCase;

static void assert_present_result(VkrBindlessVulkanPresentResult actual,
                                  VkrBindlessVulkanPresentResult expected) {
  assert(actual.enqueue_state_known == expected.enqueue_state_known);
  assert(actual.queue_operations_enqueued ==
         expected.queue_operations_enqueued);
  assert(actual.present_completion_tracking_required ==
         expected.present_completion_tracking_required);
  assert(actual.acquired_image_recovery_required ==
         expected.acquired_image_recovery_required);
  assert(actual.target_recreate_required == expected.target_recreate_required);
  assert(actual.device_lost == expected.device_lost);
}

static void test_present_result_classifier(void) {
  printf("  Running test_present_result_classifier...\n");
  const VkrBindlessVulkanPresentResult enqueued = {
      .enqueue_state_known = true_v,
      .queue_operations_enqueued = true_v,
      .present_completion_tracking_required = true_v,
  };
  const VkrBindlessVulkanPresentResult enqueued_recreate = {
      .enqueue_state_known = true_v,
      .queue_operations_enqueued = true_v,
      .present_completion_tracking_required = true_v,
      .target_recreate_required = true_v,
  };
  const VkrBindlessVulkanPresentResult not_enqueued = {
      .enqueue_state_known = true_v,
      .acquired_image_recovery_required = true_v,
  };
  const PresentCase cases[] = {
      {VK_SUCCESS, enqueued},
      {VK_SUBOPTIMAL_KHR, enqueued_recreate},
      {VK_ERROR_OUT_OF_DATE_KHR, enqueued_recreate},
      {VK_ERROR_SURFACE_LOST_KHR, enqueued_recreate},
      {VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT, enqueued_recreate},
      {VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT, enqueued_recreate},
      {VK_ERROR_OUT_OF_HOST_MEMORY, not_enqueued},
      {VK_ERROR_OUT_OF_DEVICE_MEMORY, not_enqueued},
      {VK_ERROR_DEVICE_LOST,
       (VkrBindlessVulkanPresentResult){.device_lost = true_v}},
      {VK_ERROR_FORMAT_NOT_SUPPORTED, {0}},
  };
  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    assert_present_result(
        vkr_bindless_vulkan_present_result_classify(cases[i].result),
        cases[i].expected);
  }
  printf("  test_present_result_classifier PASSED\n");
}

static void test_reacquisition_completion_contract(void) {
  printf("  Running test_reacquisition_completion_contract...\n");
  VkrBindlessVulkanReacquireState state = {0};
  VkrBindlessVulkanReacquireResult result =
      vkr_bindless_vulkan_reacquire_record(&state, false_v);
  assert(!result.image_present_complete && !result.collect_retired_swapchains &&
         !state.successor_present_complete);
  result = vkr_bindless_vulkan_reacquire_record(&state, true_v);
  assert(result.image_present_complete && result.collect_retired_swapchains &&
         state.successor_present_complete);
  assert(!vkr_bindless_vulkan_reacquire_record(NULL, true_v)
              .image_present_complete);
  printf("  test_reacquisition_completion_contract PASSED\n");
}

static void test_surface_extension_classifier(void) {
  printf("  Running test_surface_extension_classifier...\n");
  assert(vkr_bindless_vulkan_instance_extension_is_surface(
      VK_KHR_SURFACE_EXTENSION_NAME));
  assert(vkr_bindless_vulkan_instance_extension_is_surface(
      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
  assert(vkr_bindless_vulkan_instance_extension_is_surface(
      VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME));
  assert(vkr_bindless_vulkan_instance_extension_is_surface(
      "VK_KHR_win32_surface"));
  assert(!vkr_bindless_vulkan_instance_extension_is_surface(
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME));
  assert(!vkr_bindless_vulkan_instance_extension_is_surface(NULL));
  printf("  test_surface_extension_classifier PASSED\n");
}

static void test_noncoherent_atom_ranges(void) {
  printf("  Running test_noncoherent_atom_ranges...\n");
  VkrBindlessVkMappedRange range = {0};
  assert(vkr_bindless_vulkan_noncoherent_range(129u, 1u, 1024u, 128u, &range));
  assert(range.offset == 128u && range.size == 128u);
  assert(
      vkr_bindless_vulkan_noncoherent_range(900u, 124u, 1024u, 128u, &range));
  assert(range.offset == 896u && range.size == 128u);
  assert(
      vkr_bindless_vulkan_noncoherent_range(1000u, 24u, 1024u, 128u, &range));
  assert(range.offset == 896u && range.size == 128u);
  assert(
      !vkr_bindless_vulkan_noncoherent_range(1024u, 1u, 1024u, 128u, &range));
  assert(!vkr_bindless_vulkan_noncoherent_range(0u, 1u, 1024u, 96u, &range));
  assert(!vkr_bindless_vulkan_noncoherent_range(UINT64_MAX - 3u, 8u, UINT64_MAX,
                                                128u, &range));
  printf("  test_noncoherent_atom_ranges PASSED\n");
}

static void test_shared_submit_ring_completion_contract(void) {
  printf("  Running test_shared_submit_ring_completion_contract...\n");
  VkrGpuSubmitRingSlot slots[2] = {0};
  VkrGpuSubmitRing ring = {0};
  assert(vkr_gpu_submit_ring_create(&ring, 512u, ArrayCount(slots), slots,
                                    sizeof(slots)) ==
         VKR_GPU_SUBMIT_RING_STATUS_OK);

  VkrGpuRingSlice first = {0}, second = {0}, retry = {0};
  assert(vkr_gpu_submit_ring_acquire(&ring, 128u, 0u, &first) ==
         VKR_GPU_SUBMIT_RING_STATUS_OK);
  assert(vkr_gpu_submit_ring_submit(&ring, first, 7u) ==
         VKR_GPU_SUBMIT_RING_STATUS_OK);
  assert(vkr_gpu_submit_ring_acquire(&ring, 64u, 0u, &second) ==
         VKR_GPU_SUBMIT_RING_STATUS_OK);
  vkr_gpu_submit_ring_cancel(&ring, second);
  assert(vkr_gpu_submit_ring_acquire(&ring, 64u, 6u, &retry) ==
         VKR_GPU_SUBMIT_RING_STATUS_BUSY);
  assert(ring.busy_failures == 1u);
  assert(vkr_gpu_submit_ring_acquire(&ring, 64u, 7u, &retry) ==
         VKR_GPU_SUBMIT_RING_STATUS_OK);
  assert(retry.slot_index == first.slot_index && ring.reuses == 1u);

  uint8_t bytes[512] = {0};
  const VkrGpuAddressPair slice = vkr_gpu_address_pair_slice(
      (VkrGpuAddressPair){bytes, 0x100000u, sizeof(bytes)}, retry);
  assert(slice.cpu_address == bytes + retry.offset);
  assert(slice.gpu_address == 0x100000u + retry.offset);
  assert(slice.size == retry.size);
  vkr_gpu_submit_ring_cancel(&ring, retry);
  printf("  test_shared_submit_ring_completion_contract PASSED\n");
}

static void test_shared_gpu_memory_and_abi_contracts(void) {
  printf("  Running test_shared_gpu_memory_and_abi_contracts...\n");
  assert(vkr_gpu_abi_validate_host());
  assert(vkr_gpu_abi_record(VKR_GPU_ABI_VERTEX)->expected_size == 64u);
  assert(vkr_gpu_abi_record(VKR_GPU_ABI_INSTANCE)->expected_size == 80u);
  assert(vkr_gpu_abi_record(VKR_GPU_ABI_TEXT_VERTEX)->expected_size == 32u);

  const VkrGpuMemoryConfig config = {256u, 2u, 2u, 3u};
  uint8_t storage[2048] = {0};
  const uint64_t required = vkr_gpu_memory_storage_requirement(&config);
  assert(required <= sizeof(storage));
  VkrGpuMemoryCore *memory = NULL;
  assert(vkr_gpu_memory_create(&config, storage, sizeof(storage), &memory) ==
         VKR_GPU_MEMORY_STATUS_OK);
  VkrGpuAllocationHandle allocation = {0};
  VkrGpuPlacement placement = {0};
  assert(vkr_gpu_memory_allocate(memory, 65u, 64u, VKR_GPU_MEMORY_CLASS_BUFFER,
                                 &allocation,
                                 &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.resource_offset == 0u && placement.reserved_size == 65u);
  assert(vkr_gpu_memory_retire(memory, allocation, 9u) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_collect(memory, 8u, NULL, NULL, NULL) ==
         VKR_GPU_MEMORY_STATUS_OK);
  VkrGpuMemoryMetrics metrics = {0};
  vkr_gpu_memory_get_metrics(memory, &metrics);
  assert(metrics.retired_allocations == 1u && metrics.free_bytes == 191u);
  assert(vkr_gpu_memory_collect(memory, 9u, NULL, NULL, NULL) ==
         VKR_GPU_MEMORY_STATUS_OK);
  vkr_gpu_memory_get_metrics(memory, &metrics);
  assert(metrics.live_allocations == 0u && metrics.retired_allocations == 0u &&
         metrics.free_bytes == 256u && metrics.retirements_collected == 1u);
  printf("  test_shared_gpu_memory_and_abi_contracts PASSED\n");
}

static void test_shared_slot_table_metric_contract(void) {
  printf("  Running test_shared_slot_table_metric_contract...\n");
  const VkrGpuSlotTableConfig config = {3u, 3u, sizeof(uint32_t)};
  uint8_t storage[1024] = {0};
  uint32_t rows[3] = {0};
  assert(vkr_gpu_slot_table_storage_requirement(&config) <= sizeof(storage));
  VkrGpuSlotTable *table = NULL;
  assert(vkr_gpu_slot_table_create(&config, storage, sizeof(storage), rows,
                                   &table) == VKR_GPU_SLOT_STATUS_OK);

  const uint32_t first_row = 17u;
  const uint32_t replacement_row = 29u;
  VkrGpuSlotHandle first = {0};
  VkrGpuSlotHandle replacement = {0};
  assert(vkr_gpu_slot_table_publish(table, &first_row, &first) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_replace(table, first, &replacement_row, 11u,
                                    &replacement) == VKR_GPU_SLOT_STATUS_OK);
  VkrGpuSlotTableMetrics metrics = {0};
  vkr_gpu_slot_table_get_metrics(table, &metrics);
  assert(metrics.slots_live == 1u && metrics.slots_retired == 1u &&
         metrics.slots_retirements == 1u && metrics.slots_published == 2u &&
         metrics.slots_replaced == 1u && metrics.slots_capacity == 3u);
  assert(vkr_gpu_slot_table_collect(table, 11u, NULL) ==
         VKR_GPU_SLOT_STATUS_OK);
  vkr_gpu_slot_table_get_metrics(table, &metrics);
  assert(metrics.slots_retired == 0u && metrics.slots_retirements == 1u &&
         metrics.slots_collected == 1u);
  assert(vkr_gpu_slot_table_retire(table, replacement, 12u) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_collect(table, 12u, NULL) ==
         VKR_GPU_SLOT_STATUS_OK);
  VkrGpuSlotHandle capacity_handles[3] = {0};
  for (uint32_t i = 0; i < ArrayCount(capacity_handles); ++i) {
    assert(
        vkr_gpu_slot_table_publish(table, &first_row, &capacity_handles[i]) ==
        VKR_GPU_SLOT_STATUS_OK);
  }
  VkrGpuSlotHandle overflow = {0};
  assert(vkr_gpu_slot_table_publish(table, &first_row, &overflow) ==
         VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED);
  vkr_gpu_slot_table_get_metrics(table, &metrics);
  assert(metrics.slots_live == 3u && metrics.slots_retired == 0u &&
         metrics.slots_retirements == 2u && metrics.slots_collected == 2u &&
         metrics.capacity_failures == 1u);
  printf("  test_shared_slot_table_metric_contract PASSED\n");
}

static void test_shared_slot_table_retirement_preflight(void) {
  printf("  Running test_shared_slot_table_retirement_preflight...\n");
  const VkrGpuSlotTableConfig config = {3u, 1u, sizeof(uint32_t)};
  uint8_t storage[1024] = {0};
  uint32_t rows[3] = {0};
  VkrGpuSlotTable *table = NULL;
  assert(vkr_gpu_slot_table_create(&config, storage, sizeof(storage), rows,
                                   &table) == VKR_GPU_SLOT_STATUS_OK);

  const uint32_t row = 17u;
  VkrGpuSlotHandle first = {0};
  VkrGpuSlotHandle second = {0};
  assert(vkr_gpu_slot_table_publish(table, &row, &first) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_publish(table, &row, &second) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_retire(table, first, 11u) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_can_retire(table, second) ==
         VKR_GPU_SLOT_STATUS_RETIREMENT_CAPACITY_EXHAUSTED);
  uint32_t second_index = UINT32_MAX;
  assert(vkr_gpu_slot_table_resolve(table, second, &second_index) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(second_index == second.index);
  assert(vkr_gpu_slot_table_collect(table, 11u, NULL) ==
         VKR_GPU_SLOT_STATUS_OK);
  assert(vkr_gpu_slot_table_can_retire(table, second) ==
         VKR_GPU_SLOT_STATUS_OK);
  printf("  test_shared_slot_table_retirement_preflight PASSED\n");
}

bool32_t run_bindless_vulkan_tests(void) {
  printf("--- Running bindless Vulkan tests... ---\n");
  test_present_result_classifier();
  test_reacquisition_completion_contract();
  test_surface_extension_classifier();
  test_noncoherent_atom_ranges();
  test_shared_submit_ring_completion_contract();
  test_shared_gpu_memory_and_abi_contracts();
  test_shared_slot_table_metric_contract();
  test_shared_slot_table_retirement_preflight();
  printf("--- Bindless Vulkan tests completed. ---\n");
  return true;
}
