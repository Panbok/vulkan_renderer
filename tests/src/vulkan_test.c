#include "vulkan_test.h"

#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "vkr_gpu_abi.h"
#include "vkr_gpu_memory.h"
#include "vkr_geometry_ranges.h"
#include "vkr_gpu_slot_table.h"
#include "vkr_gpu_submit_ring.h"
#include "vulkan/vkr_vulkan_device.h"
#include "vulkan/vkr_vulkan_internal.h"
#include "vulkan/vkr_vulkan_memory.h"
#include "vulkan/vkr_vulkan_renderer.h"
#include "vulkan/vkr_vulkan_wsi.h"

#include <assert.h>
#include <stdio.h>

typedef struct PresentCase {
  VkResult result;
  VkrVulkanPresentResult expected;
} PresentCase;

static void assert_present_result(VkrVulkanPresentResult actual,
                                  VkrVulkanPresentResult expected) {
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
  const VkrVulkanPresentResult enqueued = {
      .enqueue_state_known = true_v,
      .queue_operations_enqueued = true_v,
      .present_completion_tracking_required = true_v,
  };
  const VkrVulkanPresentResult enqueued_recreate = {
      .enqueue_state_known = true_v,
      .queue_operations_enqueued = true_v,
      .present_completion_tracking_required = true_v,
      .target_recreate_required = true_v,
  };
  const VkrVulkanPresentResult not_enqueued = {
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
      {VK_ERROR_DEVICE_LOST, (VkrVulkanPresentResult){.device_lost = true_v}},
      {VK_ERROR_FORMAT_NOT_SUPPORTED, {0}},
  };
  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    assert_present_result(vkr_vulkan_present_result_classify(cases[i].result),
                          cases[i].expected);
  }
  printf("  test_present_result_classifier PASSED\n");
}

static void test_reacquisition_completion_contract(void) {
  printf("  Running test_reacquisition_completion_contract...\n");
  VkrVulkanReacquireState state = {0};
  vkr_vulkan_reacquire_record(&state, false_v, 7u);
  VkrVulkanReacquireResult result = vkr_vulkan_reacquire_complete(&state, 7u);
  assert(!result.image_present_complete && !result.collect_retired_swapchains &&
         !state.pending_wait_submit_value && !state.successor_present_complete);
  vkr_vulkan_reacquire_record(&state, true_v, 7u);
  assert(state.pending_wait_submit_value == 7u);
  result = vkr_vulkan_reacquire_complete(&state, 6u);
  assert(!result.image_present_complete && !result.collect_retired_swapchains &&
         state.pending_wait_submit_value == 7u &&
         !state.successor_present_complete);
  result = vkr_vulkan_reacquire_complete(&state, 7u);
  assert(result.image_present_complete && result.collect_retired_swapchains &&
         !state.pending_wait_submit_value && state.successor_present_complete);
  assert(!vkr_vulkan_reacquire_complete(NULL, 7u).image_present_complete);
  printf("  test_reacquisition_completion_contract PASSED\n");
}

static void test_surface_extension_classifier(void) {
  printf("  Running test_surface_extension_classifier...\n");
  assert(
      vkr_vulkan_instance_extension_is_surface(VK_KHR_SURFACE_EXTENSION_NAME));
  assert(vkr_vulkan_instance_extension_is_surface(
      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
  assert(vkr_vulkan_instance_extension_is_surface(
      VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME));
  assert(vkr_vulkan_instance_extension_is_surface("VK_KHR_win32_surface"));
  assert(!vkr_vulkan_instance_extension_is_surface(
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME));
  assert(!vkr_vulkan_instance_extension_is_surface(NULL));
  printf("  test_surface_extension_classifier PASSED\n");
}

static void test_srgb_surface_format_selection(void) {
  printf("  Running test_srgb_surface_format_selection...\n");
  const VkSurfaceFormatKHR formats[] = {
      {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
  };
  const bool8_t all_usable[] = {true_v, true_v, true_v, true_v};
  VkSurfaceFormatKHR selected = vkr_vulkan_device_choose_surface_format(
      formats, all_usable, ArrayCount(formats));
  assert(selected.format == VK_FORMAT_B8G8R8A8_SRGB &&
         selected.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

  selected = vkr_vulkan_device_choose_surface_format(formats, all_usable, 1u);
  assert(selected.format == VK_FORMAT_R8G8B8A8_SRGB &&
         selected.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

  const bool8_t rgba_fallback[] = {true_v, false_v, true_v, true_v};
  selected = vkr_vulkan_device_choose_surface_format(formats, rgba_fallback,
                                                     ArrayCount(formats));
  assert(selected.format == VK_FORMAT_R8G8B8A8_SRGB &&
         selected.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

  selected =
      vkr_vulkan_device_choose_surface_format(formats + 2u, all_usable, 2u);
  assert(selected.format == VK_FORMAT_UNDEFINED);

  const VkSurfaceFormatKHR unrestricted = {VK_FORMAT_UNDEFINED,
                                           VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  const bool8_t unrestricted_usable = true_v;
  selected = vkr_vulkan_device_choose_surface_format(&unrestricted,
                                                     &unrestricted_usable, 1u);
  assert(selected.format == VK_FORMAT_B8G8R8A8_SRGB &&
         selected.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
  printf("  test_srgb_surface_format_selection PASSED\n");
}

static void test_noncoherent_atom_ranges(void) {
  printf("  Running test_noncoherent_atom_ranges...\n");
  VkrVulkanMappedRange range = {0};
  assert(vkr_vulkan_noncoherent_range(129u, 1u, 1024u, 128u, &range));
  assert(range.offset == 128u && range.size == 128u);
  assert(vkr_vulkan_noncoherent_range(900u, 124u, 1024u, 128u, &range));
  assert(range.offset == 896u && range.size == 128u);
  assert(vkr_vulkan_noncoherent_range(1000u, 24u, 1024u, 128u, &range));
  assert(range.offset == 896u && range.size == 128u);
  assert(!vkr_vulkan_noncoherent_range(1024u, 1u, 1024u, 128u, &range));
  assert(!vkr_vulkan_noncoherent_range(0u, 1u, 1024u, 96u, &range));
  assert(!vkr_vulkan_noncoherent_range(UINT64_MAX - 3u, 8u, UINT64_MAX, 128u,
                                       &range));
  printf("  test_noncoherent_atom_ranges PASSED\n");
}

static void test_memory_pool_topology_contract(void) {
  printf("  Running test_memory_pool_topology_contract...\n");
  /* Adjacent live descriptor rows may share any atom size. Publication must
     reject every non-coherent placement, regardless of device locality. */
  for (uint32_t device_local = 0u; device_local < 2u; ++device_local) {
    const VkMemoryPropertyFlags properties =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0u);
    assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_PUBLICATION,
                                       properties) < 0);
    assert(vkr_vulkan_memory_type_rank(
               VKR_VULKAN_MEMORY_CLASS_PUBLICATION,
               properties | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) >= 0);
  }
  assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_PUBLICATION,
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) < 0);
  assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_DEVICE,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0);
  assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_DEVICE,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ==
         1);
  assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_UPLOAD,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
         0);
  assert(vkr_vulkan_memory_type_rank(VKR_VULKAN_MEMORY_CLASS_UPLOAD,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
         -1);
  assert(vkr_vulkan_memory_type_rank(
             VKR_VULKAN_MEMORY_CLASS_READBACK,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0);

  const VkrVulkanMemoryPoolKey buffer = {
      .memory_class = VKR_VULKAN_MEMORY_CLASS_DEVICE,
      .kind = VKR_VULKAN_MEMORY_KIND_BUFFER,
      .memory_type_index = 2u,
      .device_address_required = true_v,
  };
  assert(vkr_vulkan_memory_pool_key_equal(buffer, buffer));
  VkrVulkanMemoryPoolKey different = buffer;
  different.memory_class = VKR_VULKAN_MEMORY_CLASS_UPLOAD;
  assert(!vkr_vulkan_memory_pool_key_equal(buffer, different));
  different = buffer;
  different.kind = VKR_VULKAN_MEMORY_KIND_IMAGE;
  assert(!vkr_vulkan_memory_pool_key_equal(buffer, different));
  different = buffer;
  different.memory_type_index++;
  assert(!vkr_vulkan_memory_pool_key_equal(buffer, different));
  different = buffer;
  different.device_address_required = false_v;
  assert(!vkr_vulkan_memory_pool_key_equal(buffer, different));

  uint64_t block_size = 0u;
  assert(vkr_vulkan_memory_block_size(1024u, 2049u, 256u, &block_size));
  assert(block_size == 2304u);
  assert(!vkr_vulkan_memory_block_size(1024u, 1u, 96u, &block_size));
  assert(
      !vkr_vulkan_memory_block_size(UINT64_MAX, UINT64_MAX, 256u, &block_size));
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT] = {0};
  vkr_vulkan_memory_owner_record_allocate(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 64u);
  vkr_vulkan_memory_owner_record_allocate(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 32u);
  VkrGpuAllocationOwnerTotals *texture =
      &owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE];
  assert(texture->live_bytes == 96u && texture->peak_bytes == 96u &&
         texture->total_bytes == 96u && texture->live_allocation_count == 2u &&
         texture->peak_allocation_count == 2u &&
         texture->total_allocation_count == 2u);
  assert(vkr_vulkan_memory_owner_record_release(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 64u));
  vkr_vulkan_memory_owner_record_allocate(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 16u);
  assert(texture->live_bytes == 48u && texture->peak_bytes == 96u &&
         texture->total_bytes == 112u && texture->live_allocation_count == 2u &&
         texture->peak_allocation_count == 2u &&
         texture->total_allocation_count == 3u);
  assert(vkr_vulkan_memory_owner_record_release(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 32u));
  assert(vkr_vulkan_memory_owner_record_release(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 16u));
  assert(!vkr_vulkan_memory_owner_record_release(
      owners, VKR_GPU_ALLOCATION_OWNER_TEXTURE, 1u));
  vkr_vulkan_memory_owner_record_allocate(
      owners, (VkrGpuAllocationOwner)VKR_GPU_ALLOCATION_OWNER_COUNT, 7u);
  assert(owners[VKR_GPU_ALLOCATION_OWNER_UNKNOWN].live_bytes == 7u);
  printf("  test_memory_pool_topology_contract PASSED\n");
}

static void test_renderer_create_failure_is_transactional(void) {
  printf("  Running test_renderer_create_failure_is_transactional...\n");
  VkrDMemory memory = {0};
  assert(vkr_dmemory_create(MB(16), MB(16), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  const uint64_t free_before = vkr_dmemory_get_free_space(&memory);
  const VkrVulkanRendererConfig config = {
      .allocator = &allocator,
      .graph_path =
          "build/missing_vulkan_renderer_create_test.rendergraph.json",
      .target_kind = VKR_PRESENT_TARGET_OFFSCREEN,
      .width = 1u,
      .height = 1u,
      .image_count = 1u,
      .sampled_image_capacity = 1u,
      .storage_image_capacity = 1u,
      .sampler_capacity = 3u,
      .geometry_capacity = 1u,
      .texture_capacity = 1u,
      .material_record_capacity = 1u,
      .material_slot_capacity = 3u,
      .device_buffer_block_size = 1u,
      .device_image_block_size = 1u,
      .upload_buffer_block_size = 1u,
      .readback_buffer_block_size = 1u,
      .memory_block_capacity = 1u,
      .memory_blocks_per_pool = 1u,
      .memory_block_allocation_capacity = 1u,
      .publication_staging_capacity = 2u,
      .max_pending_texture_upload_bytes = MB(1),
  };
  VkrVulkanRenderer *renderer = NULL;
  assert(!vkr_vulkan_renderer_create(&config, &renderer));
  assert(renderer == NULL);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  vkr_dmemory_destroy(&memory);
  printf("  test_renderer_create_failure_is_transactional PASSED\n");
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
  VkrGpuGeometryRow geometry = {
      .vertex_address = 0x1000u,
      .index_address = 0x2000u,
      .publication_generation = 3u,
      .decode_address = 0x1180u,
  };
  vkr_gpu_geometry_row_relocate(&geometry, 0x8000u, 0xa000u, 4u);
  assert(geometry.vertex_address == 0x8000u);
  assert(geometry.index_address == 0xa000u);
  assert(geometry.decode_address == 0x8180u);
  assert(geometry.publication_generation == 4u);
  const uint32_t candidate_flags = 0x5u;
  const uint32_t packed_draw_state = vkr_gpu_draw_state_flags(
      VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED, candidate_flags);
  assert((packed_draw_state & VKR_GPU_DRAW_STATE_BUCKET_MASK) ==
         VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED);
  assert((packed_draw_state >> VKR_GPU_DRAW_STATE_BUCKET_BITS) ==
         candidate_flags);
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

static void test_shared_gpu_memory_bounded_placement(void) {
  printf("  Running test_shared_gpu_memory_bounded_placement...\n");
  const VkrGpuMemoryConfig config = {256u, 4u, 4u, 5u};
  uint8_t storage[4096] = {0};
  VkrGpuMemoryCore *memory = NULL;
  assert(vkr_gpu_memory_storage_requirement(&config) <= sizeof(storage));
  assert(vkr_gpu_memory_create(&config, storage, sizeof(storage), &memory) ==
         VKR_GPU_MEMORY_STATUS_OK);
  VkrGpuAllocationHandle middle = {0}, prefix = {0}, suffix = {0}, tail = {0};
  VkrGpuPlacement placement = {0};
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 60u, 64u, VKR_GPU_MEMORY_CLASS_BUFFER, 100u, 128u,
             &middle, &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 100u && placement.reserved_size == 88u);
  assert(placement.resource_offset == 128u && placement.resource_size == 60u);

  /* The span has 40 bytes left, despite 168 free bytes in the full core. */
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 41u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 100u, 128u,
             &tail, &placement) == VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 40u, 64u, VKR_GPU_MEMORY_CLASS_BUFFER, 100u, 128u,
             &tail, &placement) == VKR_GPU_MEMORY_STATUS_FRAGMENTED);
  assert(vkr_gpu_memory_allocate(memory, 100u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER,
                                 &prefix, &placement) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 0u && placement.reserved_size == 100u);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 28u, 1u, VKR_GPU_MEMORY_CLASS_TEXTURE, 228u, 28u,
             &suffix, &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 228u && placement.reserved_size == 28u);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 40u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 100u, 128u,
             &tail, &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 188u && placement.reserved_size == 40u);

  assert(vkr_gpu_memory_retire(memory, middle, 7u) == VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_retire(memory, tail, 7u) == VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_resolve(memory, middle, &placement) ==
         VKR_GPU_MEMORY_STATUS_STALE_HANDLE);
  uint32_t collected = 0;
  assert(vkr_gpu_memory_collect(memory, 6u, NULL, NULL, &collected) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(collected == 0u);
  VkrGpuMemoryMetrics metrics = {0};
  vkr_gpu_memory_get_metrics(memory, &metrics);
  assert(metrics.free_bytes == 0u && metrics.retired_reserved_bytes == 128u);
  assert(vkr_gpu_memory_collect(memory, 7u, NULL, NULL, &collected) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(collected == 2u);
  VkrGpuAllocationHandle replacement = {0};
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 128u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 100u, 128u,
             &replacement, &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 100u && placement.reserved_size == 128u);
  assert(replacement.generation != middle.generation);
  assert(vkr_gpu_memory_retire(memory, prefix, 9u) == VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_retire(memory, replacement, 9u) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_retire(memory, suffix, 9u) == VKR_GPU_MEMORY_STATUS_OK);
  assert(vkr_gpu_memory_collect(memory, 9u, NULL, NULL, &collected) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(collected == 3u);
  vkr_gpu_memory_get_metrics(memory, &metrics);
  assert(metrics.free_bytes == 256u && metrics.largest_free_range == 256u);
  assert(metrics.live_allocations == 0u && metrics.retired_allocations == 0u);
  assert(vkr_gpu_memory_allocate(memory, 256u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER,
                                 &replacement, &placement) ==
         VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.reserved_offset == 0u && placement.reserved_size == 256u);
  printf("  test_shared_gpu_memory_bounded_placement PASSED\n");
}

static void test_shared_gpu_memory_range_failure_is_transactional(void) {
  printf("  Running test_shared_gpu_memory_range_failure_is_transactional...\n");
  const VkrGpuMemoryConfig config = {256u, 1u, 1u, 1u};
  uint8_t storage[2048] = {0};
  VkrGpuMemoryCore *memory = NULL;
  assert(vkr_gpu_memory_storage_requirement(&config) <= sizeof(storage));
  assert(vkr_gpu_memory_create(&config, storage, sizeof(storage), &memory) ==
         VKR_GPU_MEMORY_STATUS_OK);
  VkrGpuAllocationHandle handle = {19u, 23u};
  VkrGpuPlacement placement = {.resource_offset = 17u, .reserved_size = 29u};
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 64u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 64u, 64u,
             &handle, &placement) == VKR_GPU_MEMORY_STATUS_OUT_OF_RANGE_METADATA);
  assert(handle.index == 19u && handle.generation == 23u);
  assert(placement.resource_offset == 17u && placement.reserved_size == 29u);
  VkrGpuMemoryMetrics metrics = {0};
  vkr_gpu_memory_get_metrics(memory, &metrics);
  assert(metrics.free_bytes == 256u && metrics.largest_free_range == 256u);
  assert(metrics.live_allocations == 0u && metrics.allocations_created == 0u);
  assert(metrics.range_metadata_failures == 1u);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 1u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, UINT64_MAX, 2u,
             &handle, &placement) == VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 1u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 128u, UINT64_MAX,
             &handle, &placement) == VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT);
  assert(vkr_gpu_memory_allocate_in_range(
             memory, 1u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER, 0u, 0u,
             &handle, &placement) == VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT);
  assert(vkr_gpu_memory_allocate(memory, 256u, 1u, VKR_GPU_MEMORY_CLASS_BUFFER,
                                 &handle, &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(handle.index == 0u && handle.generation == 1u);
  assert(placement.resource_offset == 0u && placement.reserved_size == 256u);
  printf("  test_shared_gpu_memory_range_failure_is_transactional PASSED\n");
}

/* These CPU oracles exercise paired reservation and retirement directly. A
 * renderer run cannot cheaply force the second stream's address-space failure.
 */
static void test_geometry_ranges_reuse_completed_holes(void) {
  printf("  Running test_geometry_ranges_reuse_completed_holes...\n");
  VkrDMemory memory = {0};
  assert(vkr_dmemory_create(MB(1), MB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  const uint64_t free_before = vkr_dmemory_get_free_space(&memory);
  VkrGeometryRanges ranges = {0};
  assert(vkr_geometry_ranges_create(&ranges, &allocator, 4u));
  VkrGeometryRangeAllocation defaults, first, pending, replacement;
  assert(vkr_geometry_ranges_allocate(&ranges, 1u, 1u, 1u, 1024u, 256u,
                                      &defaults));
  assert(defaults.vertex_offset == 0u && defaults.decode_offset == 32u &&
         defaults.vertex_end == 64u && defaults.index_offset == 0u);
  assert(
      vkr_geometry_ranges_allocate(&ranges, 3u, 2u, 6u, 1024u, 256u, &first));
  assert(first.vertex_offset == 64u && first.decode_offset == 160u &&
         first.vertex_end == 224u && first.index_offset == 4u);
  assert(vkr_geometry_ranges_retire(&ranges, first, 10u));
  assert(vkr_geometry_ranges_collect(&ranges, 9u));
  assert(
      vkr_geometry_ranges_allocate(&ranges, 2u, 1u, 3u, 1024u, 256u, &pending));
  assert(pending.vertex_offset == 224u && pending.index_offset == 28u);
  assert(vkr_geometry_ranges_retire(&ranges, pending, 11u));
  assert(vkr_geometry_ranges_collect(&ranges, 10u));
  assert(vkr_geometry_ranges_allocate(&ranges, 3u, 2u, 6u, 1024u, 256u,
                                      &replacement));
  assert(replacement.vertex_offset == first.vertex_offset &&
         replacement.decode_offset == first.decode_offset &&
         replacement.index_offset == first.index_offset);
  VkrGpuPlacement placement = {0};
  assert(vkr_gpu_memory_resolve(ranges.vertices, defaults.vertices,
                                &placement) == VKR_GPU_MEMORY_STATUS_OK);
  assert(placement.resource_offset == 0u && placement.resource_size == 64u);
  assert(!vkr_geometry_ranges_retire(&ranges, first, 12u));
  assert(vkr_geometry_ranges_retire(&ranges, replacement, 12u));
  assert(vkr_geometry_ranges_collect(&ranges, 12u));
  VkrGeometryMegabufferMetrics metrics = {.vertex_capacity_bytes = 1024u,
                                          .index_capacity_bytes = 256u};
  vkr_geometry_ranges_metrics(&ranges, &metrics);
  assert(metrics.live_range_count == 2u && metrics.retired_range_count == 0u &&
         metrics.reusable_range_bytes == 1212u);
  assert(vkr_geometry_ranges_retire(&ranges, defaults, 12u));
  assert(vkr_geometry_ranges_collect(&ranges, 12u));
  vkr_geometry_ranges_destroy(&ranges);
  assert(vkr_dmemory_get_free_space(&memory) == free_before);
  vkr_dmemory_destroy(&memory);
  printf("  test_geometry_ranges_reuse_completed_holes PASSED\n");
}

static void test_geometry_ranges_second_stream_failure_is_transactional(void) {
  printf("  Running "
         "test_geometry_ranges_second_stream_failure_is_transactional...\n");
  VkrDMemory memory = {0};
  assert(vkr_dmemory_create(MB(1), MB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrGeometryRanges ranges = {0};
  assert(vkr_geometry_ranges_create(&ranges, &allocator, 2u));
  VkrGeometryRangeAllocation first, rejected, tail;
  assert(vkr_geometry_ranges_allocate(&ranges, 1u, 1u, UINT32_MAX, 0u, 0u,
                                      &first));
  assert(!vkr_geometry_ranges_allocate(&ranges, 1u, 1u, 2u, 0u, 0u, &rejected));
  VkrGpuMemoryMetrics metrics = {0};
  vkr_gpu_memory_get_metrics(ranges.vertices, &metrics);
  assert(metrics.live_allocations == 1u && metrics.retired_allocations == 0u &&
         metrics.live_requested_bytes == 64u);
  assert(vkr_geometry_ranges_allocate(&ranges, 1u, 1u, 1u, 0u, 0u, &tail));
  assert(tail.vertex_offset == 64u &&
         tail.index_offset == (uint64_t)UINT32_MAX * 4u);
  assert(vkr_geometry_ranges_retire(&ranges, first, 0u));
  assert(vkr_geometry_ranges_retire(&ranges, tail, 0u));
  assert(vkr_geometry_ranges_collect(&ranges, 0u));
  vkr_geometry_ranges_destroy(&ranges);
  vkr_dmemory_destroy(&memory);
  printf(
      "  test_geometry_ranges_second_stream_failure_is_transactional PASSED\n");
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

/* Exercise CPU publication admission with real native records. No Vulkan
   command or completion behavior is simulated by this test. */
static void test_direct_draw_publication_admission(void) {
  VkrVulkanRenderer renderer = {0};
  VkrVulkanSubmeshRange range = {.index_count = 3u};
  VkrVulkanPublishedGeometry geometry[2] = {
      {.handle = {.id = 1u, .generation = 7u},
       .submeshes = &range,
       .submesh_count = 1u,
       .live = true_v},
      {.handle = {.id = 2u, .generation = 8u},
       .submeshes = &range,
       .submesh_count = 1u,
       .live = true_v,
       .pending_initialization_count = 1u},
  };
  VkrVulkanPublishedMaterial materials[2] = {
      {.handle = {.id = 1u, .generation = 11u}, .live = true_v},
      {.handle = {.id = 2u, .generation = 12u},
       .live = true_v,
       .pending_texture_count = 1u},
  };
  VkrDrawItem draws[4] = {
      {.geometry = geometry[0].handle,
       .material = materials[0].handle,
       .first_instance = 3u,
       .instance_count = 1u},
      {.geometry = geometry[1].handle,
       .material = materials[0].handle,
       .first_instance = 5u,
       .instance_count = 1u},
      {.geometry = geometry[0].handle,
       .material = materials[1].handle,
       .first_instance = 9u,
       .instance_count = 1u},
      {.geometry = geometry[0].handle,
       .material = materials[0].handle,
       .first_instance = 17u,
       .instance_count = 1u},
  };
  renderer.config.geometry_capacity = 2u;
  renderer.config.material_record_capacity = 2u;
  renderer.published_geometries = geometry;
  renderer.published_materials = materials;
  uint64_t storage[128] = {0};
  VkrVulkanFrameSlot *slot = &renderer.frame_slots[0];
  slot->frame_upload.allocation.mapped = storage;
  slot->frame_upload.size = sizeof(storage);
  VkrInstanceDataGPU instances[18];
  for (uint32_t i = 0u; i < ArrayCount(instances); ++i)
    instances[i] = (VkrInstanceDataGPU){.model = mat4_identity()};
  VkrWorldPassPayload world = {.transparent_draws = draws,
                               .transparent_draw_count = 4u,
                               .instances = instances,
                               .instance_count = ArrayCount(instances)};
  assert(vkr_vk_prepare_direct_draws(&renderer, slot, &world));
  assert(slot->direct_draw_count == 2u);
  assert(slot->direct_draws[0].first_instance == 3u);
  assert(slot->direct_draws[1].first_instance == 17u);

  geometry[1].pending_initialization_count = 0u;
  materials[1].pending_texture_count = 0u;
  slot->frame_upload_cursor = 0u;
  assert(vkr_vk_prepare_direct_draws(&renderer, slot, &world));
  assert(slot->direct_draw_count == 4u);
  assert(slot->direct_draws[1].first_instance == 5u);
  assert(slot->direct_draws[2].first_instance == 9u);

  geometry[1].pending_initialization_count = 1u;
  draws[1].submesh_index = 9u;
  slot->frame_upload_cursor = 0u;
  assert(!vkr_vk_prepare_direct_draws(&renderer, slot, &world));
  draws[1].submesh_index = 0u;
  draws[1].geometry.generation++;
  slot->frame_upload_cursor = 0u;
  assert(!vkr_vk_prepare_direct_draws(&renderer, slot, &world));
  draws[1].geometry = geometry[1].handle;

  slot->frame_upload_cursor = 0u;
  slot->frame_upload.size = 1u;
  assert(!vkr_vk_prepare_direct_draws(&renderer, slot, &world));
  assert(slot->frame_upload_cursor == 0u);
  assert(slot->direct_draw_count == 0u);
  assert(slot->frame_upload_exhaustions == 1u);
  printf("  test_direct_draw_publication_admission PASSED\n");
}

static void test_cancelled_asset_use_serials(void) {
  printf("  Running test_cancelled_asset_use_serials...\n");
  VkrVulkanPublishedGeometry geometry[] = {
      {.last_use_submit_value = 0u},
      {.last_use_submit_value = 5u},
      {.last_use_submit_value = 7u},
      {.last_use_submit_value = 8u},
  };
  VkrVulkanPublishedTexture textures[] = {
      {.last_use_submit_value = 8u},
      {.last_use_submit_value = 7u},
      {.last_use_submit_value = 5u},
      {.last_use_submit_value = 0u},
  };
  VkrVulkanRenderer renderer = {
      .config = {.geometry_capacity = ArrayCount(geometry),
                 .texture_capacity = ArrayCount(textures)},
      .published_geometries = geometry,
      .published_textures = textures,
      .submit_value = 7u,
      .completed_value = 5u,
  };
  vkr_vk_discard_unsubmitted_asset_uses(&renderer);
  // Serial 8 was only recorded. Submitted serial 7 must still await completion.
  assert(geometry[0].last_use_submit_value == 0u);
  assert(geometry[1].last_use_submit_value == 5u);
  assert(geometry[2].last_use_submit_value == 7u);
  assert(geometry[3].last_use_submit_value == 7u);
  assert(textures[0].last_use_submit_value == 7u);
  assert(textures[1].last_use_submit_value == 7u);
  assert(textures[2].last_use_submit_value == 5u);
  assert(textures[3].last_use_submit_value == 0u);
  assert(renderer.submit_value == 7u && renderer.completed_value == 5u);
  printf("  test_cancelled_asset_use_serials PASSED\n");
}

static void test_shared_graph_metalfx_capability_boundary(void) {
  printf("  Running test_shared_graph_metalfx_capability_boundary...\n");
  // Exercise production binding and validation without creating a GPU device.
  // All graph storage belongs to this arena and is released after validation.
  Arena *arena = arena_create(MB(16), MB(2));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrVulkanRenderer renderer = {.allocator = &allocator};
  assert(vkr_rg_executor_registry_init(&renderer.executors, &allocator));
  assert(vkr_vk_register_graph_executors(&renderer));
  assert(vkr_rg_json_load_file(&allocator,
                               "assets/render_graphs/main.rendergraph.json",
                               &renderer.json_graph));
  assert(vkr_rg_json_bind_executors(&renderer.json_graph, &renderer.executors));
  renderer.graph = vkr_rg_create(&allocator);
  assert(renderer.graph);

  VkrRenderGraphFrameInfo frame = {
      .scene_rendering = true_v,
      .gpu_draw_candidate_capacity = 1u,
      .gpu_draw_visible_capacity = 8u,
      .transmission_gpu_draw_candidate_capacity = 1u,
      .transmission_gpu_draw_visible_capacity = 8u,
      .editor_image_available = true_v,
      .editor_image_width = 640u,
      .editor_image_height = 480u,
      .target_width = 640u,
      .target_height = 480u,
      .window_width = 640u,
      .window_height = 480u,
      .scene_output_width = 640u,
      .scene_output_height = 480u,
      .viewport_width = 640u,
      .viewport_height = 480u,
      .render_scale = 1.0f,
      .target_color_format = VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
      .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_map_size = 2048u,
      .shadow_map_layer_count = 4u,
  };
  for (uint32_t mode = 0u; mode < 4u; ++mode) {
    frame.editor_enabled = (mode & 1u) != 0u;
    frame.metalfx_enabled = (mode & 2u) != 0u;
    assert(vkr_rg_begin_frame(renderer.graph, &frame));
    assert(
        vkr_rg_build_from_json(renderer.graph, &renderer.json_graph, &frame));
    assert(vkr_rg_compile_schedule(renderer.graph));
    assert(vkr_vk_validate_graph(&renderer) == !frame.metalfx_enabled);
    vkr_rg_end_frame(renderer.graph);
  }

  vkr_rg_destroy(renderer.graph);
  vkr_rg_json_destroy(&renderer.json_graph);
  vkr_rg_executor_registry_destroy(&renderer.executors);
  arena_destroy(arena);
  printf("  test_shared_graph_metalfx_capability_boundary PASSED\n");
}

bool32_t run_vulkan_tests(void) {
  printf("--- Running Vulkan tests... ---\n");
  test_shared_graph_metalfx_capability_boundary();
  test_cancelled_asset_use_serials();
  test_direct_draw_publication_admission();
  test_present_result_classifier();
  test_reacquisition_completion_contract();
  test_surface_extension_classifier();
  test_srgb_surface_format_selection();
  test_noncoherent_atom_ranges();
  test_memory_pool_topology_contract();
  test_renderer_create_failure_is_transactional();
  test_shared_submit_ring_completion_contract();
  test_shared_gpu_memory_and_abi_contracts();
  test_shared_gpu_memory_bounded_placement();
  test_shared_gpu_memory_range_failure_is_transactional();
  test_geometry_ranges_reuse_completed_holes();
  test_geometry_ranges_second_stream_failure_is_transactional();
  test_shared_slot_table_metric_contract();
  test_shared_slot_table_retirement_preflight();
  printf("--- Vulkan tests completed. ---\n");
  return true;
}
