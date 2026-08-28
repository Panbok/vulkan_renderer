#include "metal_memory_test.h"

#include "renderer/metal/internal/vkr_metal_packet_waits.h"
#include "renderer/metal/vkr_metal_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct MetalMemoryFixture {
  void *storage;
  VkrMetalMemoryCore *memory;
} MetalMemoryFixture;

static MetalMemoryFixture metal_memory_fixture(VkrMetalMemoryConfig config) {
  const uint64_t storage_size = vkr_metal_memory_storage_requirement(&config);
  void *storage = malloc(storage_size);
  assert(storage);
  VkrMetalMemoryCore *memory = NULL;
  assert(vkr_metal_memory_create(&config, storage, storage_size, &memory) ==
         VKR_METAL_MEMORY_STATUS_OK);
  return (MetalMemoryFixture){storage, memory};
}

static void test_metal_memory_alignment_and_balance(void) {
  printf("  Running test_metal_memory_alignment_and_balance...\n");
  MetalMemoryFixture fixture =
      metal_memory_fixture((VkrMetalMemoryConfig){1024, 4, 4, 5});
  VkrMetalAllocationHandle a = {0}, b = {0};
  VkrMetalPlacement pa = {0}, pb = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 100, 64, 1, &a, &pa) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_allocate(fixture.memory, 100, 256, 2, &b, &pb) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(pa.reserved_offset == 0 && pa.reserved_size == 100);
  assert(pa.resource_offset == 0 && pa.resource_offset % pa.alignment == 0);
  assert(pb.reserved_offset == 100 && pb.reserved_size == 256);
  assert(pb.resource_offset == 256 && pb.resource_offset % pb.alignment == 0);
  assert(pb.resource_offset + pb.resource_size <=
         pb.reserved_offset + pb.reserved_size);
  VkrMetalMemoryMetrics metrics = {0};
  vkr_metal_memory_get_metrics(fixture.memory, &metrics);
  assert(metrics.peak_requested_bytes == 200);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_BUFFER].live_allocations == 1);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_BUFFER].live_requested_bytes ==
         100);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_TEXTURE].live_allocations == 1);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_TEXTURE].live_reserved_bytes ==
         256);
  assert(metrics.alignment_waste_bytes == 156);

  assert(vkr_metal_memory_retire(fixture.memory, a, 3) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, b, 3) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_collect(fixture.memory, 3, NULL, NULL, NULL) ==
         VKR_METAL_MEMORY_STATUS_OK);
  vkr_metal_memory_get_metrics(fixture.memory, &metrics);
  assert(metrics.free_bytes == 1024 && metrics.largest_free_range == 1024);
  assert(metrics.live_allocations == 0 && metrics.retired_allocations == 0);
  assert(metrics.live_reserved_bytes == 0 &&
         metrics.retired_reserved_bytes == 0);
  assert(metrics.retirements_collected == 2);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_BUFFER].live_allocations == 0);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_BUFFER].retired_allocations ==
         0);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_BUFFER].allocations_created ==
         1);
  assert(metrics.classes[VKR_METAL_MEMORY_CLASS_TEXTURE].peak_allocations == 1);
  free(fixture.storage);
  printf("  test_metal_memory_alignment_and_balance PASSED\n");
}

static void test_metal_memory_reserved_capacity_admission(void) {
  printf("  Running test_metal_memory_reserved_capacity_admission...\n");
  const VkrMetalMemoryMetrics metrics = {
      .heap_size = 1000u,
      .free_bytes = 200u,
      .largest_free_range = 150u,
  };
  assert(vkr_metal_memory_effective_budget(1000u, 0u) == 1000u);
  assert(vkr_metal_memory_effective_budget(1000u, 1200u) == 1000u);
  assert(vkr_metal_memory_effective_budget(1000u, 750u) == 750u);
  assert(vkr_metal_memory_effective_free_bytes(1000u, 200u, 0u) == 200u);
  assert(vkr_metal_memory_effective_free_bytes(1000u, 200u, 1200u) == 200u);
  assert(vkr_metal_memory_effective_free_bytes(1000u, 200u, 900u) == 100u);
  assert(vkr_metal_memory_effective_free_bytes(1000u, 200u, 800u) == 0u);
  assert(vkr_metal_memory_effective_free_bytes(1000u, 200u, 750u) == 0u);
  assert(vkr_metal_memory_can_allocate_before_reserve(&metrics, 100u, 100u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 101u, 100u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 200u, 100u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 151u, 0u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 1u, 201u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 1u, 200u));
  assert(!vkr_metal_memory_can_allocate_before_reserve(&metrics, 0u, 100u));
  printf("  test_metal_memory_reserved_capacity_admission PASSED\n");
}

typedef struct RetirementTrace {
  uint32_t indices[4];
  uint32_t count;
} RetirementTrace;

static void metal_memory_trace_release(void *context, uint32_t slot_index,
                                       const VkrMetalPlacement *placement) {
  RetirementTrace *trace = context;
  assert(placement->resource_size != 0);
  trace->indices[trace->count++] = slot_index;
}

static void test_metal_memory_stale_handle_and_submit_order(void) {
  printf("  Running test_metal_memory_stale_handle_and_submit_order...\n");
  MetalMemoryFixture fixture =
      metal_memory_fixture((VkrMetalMemoryConfig){512, 3, 3, 4});
  VkrMetalAllocationHandle late = {0}, early = {0};
  VkrMetalPlacement placement = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 64, 16, 0, &late,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_allocate(fixture.memory, 64, 16, 0, &early,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, late, 5) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_resolve(fixture.memory, late, &placement) ==
         VKR_METAL_MEMORY_STATUS_STALE_HANDLE);
  assert(vkr_metal_memory_retire(fixture.memory, early, 2) ==
         VKR_METAL_MEMORY_STATUS_OK);

  RetirementTrace trace = {0};
  uint32_t collected = 0;
  assert(vkr_metal_memory_collect(fixture.memory, 2, metal_memory_trace_release,
                                  &trace,
                                  &collected) == VKR_METAL_MEMORY_STATUS_OK);
  assert(collected == 1 && trace.count == 1 && trace.indices[0] == early.index);
  VkrMetalMemoryMetrics metrics = {0};
  vkr_metal_memory_get_metrics(fixture.memory, &metrics);
  assert(metrics.retired_allocations == 1);
  assert(vkr_metal_memory_collect(fixture.memory, 5, metal_memory_trace_release,
                                  &trace,
                                  &collected) == VKR_METAL_MEMORY_STATUS_OK);
  assert(collected == 1 && trace.count == 2 && trace.indices[1] == late.index);

  VkrMetalAllocationHandle replacement = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 64, 16, 0, &replacement,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(replacement.index == late.index || replacement.index == early.index);
  if (replacement.index == late.index)
    assert(replacement.generation != late.generation);
  if (replacement.index == early.index)
    assert(replacement.generation != early.generation);
  free(fixture.storage);
  printf("  test_metal_memory_stale_handle_and_submit_order PASSED\n");
}

static void test_metal_memory_failure_classification(void) {
  printf("  Running test_metal_memory_failure_classification...\n");
  MetalMemoryFixture fixture =
      metal_memory_fixture((VkrMetalMemoryConfig){512, 6, 6, 7});
  VkrMetalAllocationHandle handles[3] = {0};
  VkrMetalPlacement placement = {0};
  for (uint32_t i = 0; i < 3; ++i)
    assert(vkr_metal_memory_allocate(fixture.memory, 100, 1, 0, &handles[i],
                                     &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, handles[0], 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, handles[2], 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_collect(fixture.memory, 1, NULL, NULL, NULL) ==
         VKR_METAL_MEMORY_STATUS_OK);
  VkrMetalAllocationHandle ignored = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 350, 1, 0, &ignored,
                                   &placement) ==
         VKR_METAL_MEMORY_STATUS_FRAGMENTED);
  assert(vkr_metal_memory_allocate(fixture.memory, 500, 1, 0, &ignored,
                                   &placement) ==
         VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES);
  VkrMetalMemoryMetrics metrics = {0};
  vkr_metal_memory_get_metrics(fixture.memory, &metrics);
  assert(metrics.fragmentation_failures == 1);
  assert(metrics.byte_exhaustion_failures == 1);
  free(fixture.storage);

  fixture = metal_memory_fixture((VkrMetalMemoryConfig){300, 2, 2, 1});
  VkrMetalAllocationHandle first = {0}, second = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 100, 1, 0, &first,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_allocate(fixture.memory, 100, 1, 0, &second,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, first, 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_collect(fixture.memory, 1, NULL, NULL, NULL) ==
         VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA);
  vkr_metal_memory_get_metrics(fixture.memory, &metrics);
  assert(metrics.range_metadata_failures == 1);
  assert(metrics.retired_allocations == 1);
  free(fixture.storage);
  printf("  test_metal_memory_failure_classification PASSED\n");
}

static void test_metal_submit_ring_reuse(void) {
  printf("  Running test_metal_submit_ring_reuse...\n");
  VkrMetalSubmitRingSlot slots[2] = {0};
  VkrMetalSubmitRing ring = {0};
  assert(vkr_metal_submit_ring_create(&ring, 512, 2, slots, sizeof(slots)) ==
         VKR_METAL_MEMORY_STATUS_OK);
  VkrMetalRingSlice a = {0}, b = {0}, ignored = {0};
  assert(vkr_metal_submit_ring_acquire(&ring, 128, 0, &a) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_submit_ring_submit(&ring, a, 3) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_submit_ring_acquire(&ring, 64, 0, &b) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_submit_ring_submit(&ring, b, 4) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_submit_ring_acquire(&ring, 64, 2, &ignored) ==
         VKR_METAL_MEMORY_STATUS_RING_BUSY);
  assert(vkr_metal_submit_ring_acquire(&ring, 64, 3, &ignored) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(ignored.slot_index == a.slot_index && ring.reuses == 1);

  uint8_t bytes[512] = {0};
  VkrMetalAddressPair whole = {bytes, 0x100000, sizeof(bytes)};
  VkrMetalAddressPair pair = vkr_metal_address_pair_slice(whole, ignored);
  assert(pair.cpu_address == bytes + ignored.offset);
  assert(pair.gpu_address == 0x100000 + ignored.offset);
  assert(pair.size == ignored.size);
  vkr_metal_submit_ring_cancel(&ring, ignored);
  printf("  test_metal_submit_ring_reuse PASSED\n");
}

static void test_metal_packet_wait_counter_reset(void) {
  printf("  Running test_metal_packet_wait_counter_reset...\n");
  VkrMetalPacketWaitCounters counters = {0};

  vkr_metal_packet_wait_counters_record(&counters, false_v);
  vkr_metal_packet_wait_counters_record(&counters, true_v);
  vkr_metal_packet_wait_counters_record(&counters, false_v);

  assert(vkr_metal_packet_wait_counters_take_upload(&counters) == 1u);
  assert(vkr_metal_packet_wait_counters_take_upload(&counters) == 0u);
  assert(vkr_metal_packet_wait_counters_take_command_slot(&counters) == 3u);
  assert(vkr_metal_packet_wait_counters_take_command_slot(&counters) == 0u);

  printf("  test_metal_packet_wait_counter_reset PASSED\n");
}

/*
 * The allocator reserves a handle slot before it searches for a byte range, so
 * every range-search failure must return that slot. Without the return, each
 * fragmentation or byte-exhaustion failure would permanently consume one
 * handle and the core would report OUT_OF_HANDLES long before it was true.
 */
static void test_metal_memory_failed_allocation_returns_handle(void) {
  printf("  Running test_metal_memory_failed_allocation_returns_handle...\n");
  /* heap=1024, 3 handles. */
  MetalMemoryFixture fixture =
      metal_memory_fixture((VkrMetalMemoryConfig){1024, 3, 4, 5});
  VkrMetalAllocationHandle handle = {0};
  VkrMetalPlacement placement = {0};

  /* Consume all but 24 bytes, leaving two free handles. */
  VkrMetalAllocationHandle bulk = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 1000, 1, 1, &bulk,
                                   &placement) == VKR_METAL_MEMORY_STATUS_OK);

  /* Sixteen byte-exhaustion failures. Each reserves and must release a slot;
     without the release the third attempt would report OUT_OF_HANDLES. */
  for (uint32_t i = 0; i < 16; ++i) {
    assert(vkr_metal_memory_allocate(fixture.memory, 512, 1, 1, &handle,
                                     &placement) ==
           VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES);
  }

  /* Both remaining handles must still be available. */
  VkrMetalAllocationHandle a = {0}, b = {0};
  assert(vkr_metal_memory_allocate(fixture.memory, 8, 1, 1, &a, &placement) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_allocate(fixture.memory, 8, 1, 1, &b, &placement) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(
      vkr_metal_memory_allocate(fixture.memory, 8, 1, 1, &handle, &placement) ==
      VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES);

  /* Collection must return every slot to the free pool for reuse. */
  assert(vkr_metal_memory_retire(fixture.memory, bulk, 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, a, 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(vkr_metal_memory_retire(fixture.memory, b, 1) ==
         VKR_METAL_MEMORY_STATUS_OK);
  uint32_t collected = 0;
  assert(vkr_metal_memory_collect(fixture.memory, 1, NULL, NULL, &collected) ==
         VKR_METAL_MEMORY_STATUS_OK);
  assert(collected == 3);
  for (uint32_t i = 0; i < 3; ++i) {
    assert(vkr_metal_memory_allocate(fixture.memory, 8, 1, 1, &handle,
                                     &placement) == VKR_METAL_MEMORY_STATUS_OK);
  }

  free(fixture.storage);
  printf("  test_metal_memory_failed_allocation_returns_handle PASSED\n");
}

bool32_t run_metal_memory_tests(void) {
  printf("Running Metal memory tests...\n");
  test_metal_memory_alignment_and_balance();
  test_metal_memory_reserved_capacity_admission();
  test_metal_memory_stale_handle_and_submit_order();
  test_metal_memory_failure_classification();
  test_metal_memory_failed_allocation_returns_handle();
  test_metal_submit_ring_reuse();
  test_metal_packet_wait_counter_reset();
  printf("Metal memory tests PASSED\n");
  return true_v;
}
