#include "metal/vkr_metal_memory_device.h"
#include "core/logger.h"
#include "metal/vkr_metal_diagnostics.h"

#if defined(PLATFORM_APPLE)

#import <Metal/Metal.h>

typedef enum VkrMetalHeapGroup {
  VKR_METAL_HEAP_PERSISTENT,
  VKR_METAL_HEAP_ASSET_TEXTURES,
  VKR_METAL_HEAP_SCENE_IMAGES,
  VKR_METAL_HEAP_SCENE_BUFFERS,
} VkrMetalHeapGroup;

typedef struct VkrMetalNativeHeap {
  id<MTLHeap> heap;
  uint64_t logical_offset;
  uint64_t logical_size;
  uint64_t charged_size;
  uint32_t allocation_count;
  VkrMetalHeapGroup group;
} VkrMetalNativeHeap;

struct VkrMetalMemoryDevice {
  id<MTLDevice> device;
  VkrMetalNativeHeap *heaps;
  uint32_t *resource_heaps;
  uint64_t heaps_size;
  uint64_t resource_heaps_size;
  uint64_t managed_budget_size;
  uint64_t heap_chunk_size;
  uint64_t native_heap_charged_size;
  uint64_t managed_peak_allocated_size;
  uint64_t external_allocation_count;
  uint64_t native_heap_count;
  uint64_t native_heap_peak_count;
  uint64_t native_heap_total_count;
  uint64_t external_allocated_size;
  uint64_t transfer_ring_allocated_size;
  id<MTLBuffer> upload_buffer;
  id<MTLBuffer> readback_buffer;
  id<MTLResidencySet> residency;
  id<MTLResource> *native_resources;
  uint64_t *logical_lengths;
  VkrGpuAllocationOwner *logical_owners;
  void *core_storage;
  void *upload_ring_storage;
  void *readback_ring_storage;
  /** Owns every host allocation above; retained for the sized frees. */
  VkrAllocator *allocator;
  VkrMetalDiagnostics *diagnostics;
  uint64_t core_storage_size;
  uint64_t ring_storage_size;
  uint64_t native_resources_size;
  uint64_t logical_lengths_size;
  uint64_t logical_owners_size;
  VkrMetalMemoryCore *core;
  VkrMetalSubmitRing upload_ring;
  VkrMetalSubmitRing readback_ring;
  VkrMetalAddressPair upload_addresses;
  VkrMetalAddressPair readback_addresses;
  uint32_t max_allocations;
  uint32_t heap_slot_count;
  uint64_t native_live_resources;
  uint64_t native_resources_released;
  uint64_t native_heap_peak_allocated_size;
  uint64_t upload_ring_max_requested_bytes;
  uint64_t upload_ring_oversize_failures;
  uint64_t readback_ring_max_requested_bytes;
  uint64_t readback_ring_oversize_failures;
  uint64_t upload_ring_max_size;
  uint64_t readback_ring_max_size;
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];
};

vkr_internal void *vkr_metal_memory_device_alloc(VkrAllocator *allocator,
                                                 uint64_t size) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory)
    MemZero(memory, size);
  return memory;
}

vkr_internal void vkr_metal_memory_device_free(VkrAllocator *allocator,
                                               void *memory, uint64_t size) {
  if (memory && size)
    vkr_allocator_free(allocator, memory, size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

vkr_internal void vkr_metal_memory_release(id object) {
  if (object)
    [object release];
}

vkr_internal uint64_t
vkr_metal_memory_managed_size(const VkrMetalMemoryDevice *device) {
  return device->native_heap_charged_size + device->external_allocated_size +
         device->transfer_ring_allocated_size;
}

vkr_internal void vkr_metal_memory_update_peaks(VkrMetalMemoryDevice *device) {
  device->managed_peak_allocated_size =
      Max(device->managed_peak_allocated_size,
          vkr_metal_memory_managed_size(device));
  uint64_t allocated = 0;
  for (uint32_t i = 0; i < device->heap_slot_count; ++i)
    allocated += device->heaps[i].heap.currentAllocatedSize;
  device->native_heap_peak_allocated_size =
      Max(device->native_heap_peak_allocated_size, allocated);
}

vkr_internal bool8_t vkr_metal_memory_has_budget(VkrMetalMemoryDevice *device,
                                                 uint64_t bytes) {
  return bytes <=
         device->managed_budget_size - vkr_metal_memory_managed_size(device);
}

vkr_internal void vkr_metal_memory_snapshot(VkrMetalMemoryDevice *device,
                                            const char *event) {
  if (!device->diagnostics || !device->diagnostics->enabled)
    return;
  VkrMetalMemoryMetrics metrics = {0};
  vkr_metal_memory_get_metrics(device->core, &metrics);
  vkr_metal_diagnostics_record(
      device->diagnostics, event, 0u, 0u,
      "budget=%llu managed=%llu heaps=%llu external=%llu rings=%llu "
      "logical_free=%llu residency=%llu driver=%llu",
      (unsigned long long)device->managed_budget_size,
      (unsigned long long)vkr_metal_memory_managed_size(device),
      (unsigned long long)device->native_heap_charged_size,
      (unsigned long long)device->external_allocated_size,
      (unsigned long long)device->transfer_ring_allocated_size,
      (unsigned long long)metrics.free_bytes,
      (unsigned long long)device->residency.allocatedSize,
      (unsigned long long)device->device.currentAllocatedSize);
}

vkr_internal void vkr_metal_memory_budget_reject(VkrMetalMemoryDevice *device,
                                                 uint64_t requested_bytes,
                                                 bool8_t report_error) {
  if (report_error)
    log_error("Metal managed GPU budget exhausted: requested=%llu managed=%llu "
              "cap=%llu bytes",
              (unsigned long long)requested_bytes,
              (unsigned long long)vkr_metal_memory_managed_size(device),
              (unsigned long long)device->managed_budget_size);
  if (device->diagnostics && device->diagnostics->enabled)
    vkr_metal_diagnostics_record(
        device->diagnostics, "memory.budget_reject", 0u, 0u,
        "requested=%llu managed=%llu budget=%llu",
        (unsigned long long)requested_bytes,
        (unsigned long long)vkr_metal_memory_managed_size(device),
        (unsigned long long)device->managed_budget_size);
}

bool8_t vkr_metal_memory_device_reserve_external(VkrMetalMemoryDevice *device,
                                                 uint64_t bytes) {
  if (!device)
    return false_v;
  if (!vkr_metal_memory_has_budget(device, bytes)) {
    vkr_metal_memory_budget_reject(device, bytes, true_v);
    return false_v;
  }
  device->external_allocated_size += bytes;
  device->external_allocation_count += bytes != 0;
  vkr_metal_memory_update_peaks(device);
  vkr_metal_memory_snapshot(device, "memory.external_reserve");
  return true_v;
}

bool8_t vkr_metal_memory_device_reconcile_external(VkrMetalMemoryDevice *device,
                                                   uint64_t reserved_bytes,
                                                   uint64_t allocated_bytes) {
  if (!device || reserved_bytes > device->external_allocated_size)
    return false_v;
  if (allocated_bytes > reserved_bytes &&
      !vkr_metal_memory_has_budget(device, allocated_bytes - reserved_bytes)) {
    vkr_metal_memory_budget_reject(device, allocated_bytes - reserved_bytes, true_v);
    return false_v;
  }
  device->external_allocation_count -= reserved_bytes != 0;
  device->external_allocation_count += allocated_bytes != 0;
  device->external_allocated_size -= reserved_bytes;
  device->external_allocated_size += allocated_bytes;
  vkr_metal_memory_update_peaks(device);
  vkr_metal_memory_snapshot(device, "memory.external_reconcile");
  return true_v;
}

void vkr_metal_memory_device_release_external(VkrMetalMemoryDevice *device,
                                              uint64_t bytes) {
  if (!device || bytes > device->external_allocated_size)
    return;
  device->external_allocated_size -= bytes;
  device->external_allocation_count -= bytes != 0;
  vkr_metal_memory_snapshot(device, "memory.external_release");
}

// The core callback runs only after the allocation's last submission completes.
vkr_internal void vkr_metal_memory_release_heap(VkrMetalMemoryDevice *device,
                                                uint32_t heap_index) {
  VkrMetalNativeHeap *heap = &device->heaps[heap_index];
  [device->residency removeAllocation:heap->heap];
  [device->residency commit];
  vkr_metal_memory_release(heap->heap);
  device->native_heap_charged_size -= heap->charged_size;
  device->native_heap_count--;
  *heap = (VkrMetalNativeHeap){0};
  while (device->heap_slot_count &&
         !device->heaps[device->heap_slot_count - 1].heap)
    device->heap_slot_count--;
  vkr_metal_memory_snapshot(device, "memory.heap_release");
}

vkr_internal void
vkr_metal_memory_release_retired(void *context, uint32_t slot_index,
                                 const VkrMetalPlacement *placement) {
  (void)placement;
  VkrMetalMemoryDevice *device = context;
  id<MTLResource> resource = device->native_resources[slot_index];
  vkr_metal_memory_release(resource);
  device->native_resources[slot_index] = nil;
  device->logical_lengths[slot_index] = 0;
  device->logical_owners[slot_index] = VKR_GPU_ALLOCATION_OWNER_UNKNOWN;
  if (resource) {
    device->native_live_resources--;
    device->native_resources_released++;
  }
  const uint32_t heap_index = device->resource_heaps[slot_index];
  if (--device->heaps[heap_index].allocation_count == 0)
    vkr_metal_memory_release_heap(device, heap_index);
}

// Creation reserves retirement metadata for every handle and range metadata for
// every possible free gap. An unpublished allocation can therefore always
// retire and collect at zero without borrowing a submission's completion proof.
vkr_internal VkrMetalMemoryStatus vkr_metal_memory_abandon(
    VkrMetalMemoryDevice *device, VkrMetalAllocationHandle handle) {
  VkrMetalMemoryStatus status =
      vkr_metal_memory_retire(device->core, handle, 0);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  return vkr_metal_memory_device_collect(device, 0, NULL);
}

// Heap spans stay disjoint even when their unused logical ranges are free in
// the shared core. The creation scan never moves a live GPU address.
vkr_internal bool8_t vkr_metal_memory_find_span(VkrMetalMemoryDevice *device,
                                                uint64_t size,
                                                uint64_t alignment,
                                                uint64_t *out_offset) {
  uint64_t offset = 0;
  for (;;) {
    if (offset > UINT64_MAX - (alignment - 1))
      return false_v;
    offset = AlignPow2(offset, alignment);
    if (offset > device->managed_budget_size ||
        size > device->managed_budget_size - offset)
      return false_v;
    bool8_t overlaps = false_v;
    for (uint32_t i = 0; i < device->heap_slot_count; ++i) {
      const VkrMetalNativeHeap *heap = &device->heaps[i];
      if (heap->heap && offset < heap->logical_offset + heap->logical_size &&
          heap->logical_offset < offset + size) {
        offset = heap->logical_offset + heap->logical_size;
        overlaps = true_v;
        break;
      }
    }
    if (!overlaps) {
      *out_offset = offset;
      return true_v;
    }
  }
}

vkr_internal VkrMetalHeapGroup
vkr_metal_memory_heap_group(VkrGpuAllocationOwner owner, uint32_t kind) {
  if (owner == VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH)
    return kind == VKR_METAL_RESOURCE_KIND_TEXTURE
               ? VKR_METAL_HEAP_SCENE_IMAGES
               : VKR_METAL_HEAP_SCENE_BUFFERS;
  if (owner == VKR_GPU_ALLOCATION_OWNER_TEXTURE &&
      kind == VKR_METAL_RESOURCE_KIND_TEXTURE)
    return VKR_METAL_HEAP_ASSET_TEXTURES;
  return VKR_METAL_HEAP_PERSISTENT;
}

vkr_internal VkrMetalMemoryStatus vkr_metal_memory_place(
    VkrMetalMemoryDevice *device, MTLSizeAndAlign size_align, uint32_t kind,
    VkrGpuAllocationOwner owner, VkrMetalAllocationHandle *out_handle,
    VkrMetalPlacement *out_placement, uint32_t *out_heap_index) {
  if (!size_align.size || !size_align.align ||
      (size_align.align & (size_align.align - 1)))
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  const VkrMetalHeapGroup group = vkr_metal_memory_heap_group(owner, kind);
  /* Scene image realization reports rejection after its bounded retry. */
  uint32_t empty_index = UINT32_MAX;
  for (uint32_t i = 0; i < device->heap_slot_count; ++i) {
    VkrMetalNativeHeap *heap = &device->heaps[i];
    if (!heap->heap) {
      if (empty_index == UINT32_MAX)
        empty_index = i;
      continue;
    }
    /* Avoid pinning resized Scene images behind persistent buffers/assets. */
    if (heap->group != group || heap->logical_offset % size_align.align ||
        size_align.size > heap->logical_size)
      continue;
    VkrMetalMemoryStatus status =
        (VkrMetalMemoryStatus)vkr_gpu_memory_allocate_in_range(
            device->core, size_align.size, size_align.align, kind,
            heap->logical_offset, heap->logical_size, out_handle,
            out_placement);
    if (status == VKR_METAL_MEMORY_STATUS_OK) {
      heap->allocation_count++;
      device->resource_heaps[out_handle->index] = i;
      *out_heap_index = i;
      return status;
    }
    if (status != VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES &&
        status != VKR_METAL_MEMORY_STATUS_FRAGMENTED)
      return status;
  }
  if (empty_index == UINT32_MAX) {
    if (device->heap_slot_count == device->max_allocations)
      return VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES;
    empty_index = device->heap_slot_count;
  }
  const uint64_t available =
      device->managed_budget_size - vkr_metal_memory_managed_size(device);
  if (size_align.size > available) {
    vkr_metal_memory_budget_reject(
        device, size_align.size, group != VKR_METAL_HEAP_SCENE_IMAGES);
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  }
  uint64_t size = Max((uint64_t)size_align.size, device->heap_chunk_size);
  size = Min(size, available);
  uint64_t offset = 0;
  const uint64_t base_alignment = Max((uint64_t)size_align.align, 65536ull);
  if (!vkr_metal_memory_find_span(device, size, base_alignment, &offset)) {
    size = size_align.size;
    if (!vkr_metal_memory_find_span(device, size, base_alignment, &offset))
      return VKR_METAL_MEMORY_STATUS_FRAGMENTED;
  }
  MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
  descriptor.type = MTLHeapTypePlacement;
  descriptor.size = size;
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  id<MTLHeap> native_heap = [device->device newHeapWithDescriptor:descriptor];
  uint64_t charged_size = native_heap
      ? Max((uint64_t)native_heap.size,
            (uint64_t)native_heap.currentAllocatedSize)
      : 0u;
  /* Native heap size can round above the remaining budget. Drop optional
     chunk headroom and retry the exact resource size before rejecting it. */
  if (native_heap && charged_size > available && size > size_align.size) {
    [native_heap release];
    size = size_align.size;
    descriptor.size = size;
    native_heap = [device->device newHeapWithDescriptor:descriptor];
    charged_size = native_heap
        ? Max((uint64_t)native_heap.size,
              (uint64_t)native_heap.currentAllocatedSize)
        : 0u;
  }
  [descriptor release];
  if (!native_heap) {
    vkr_metal_memory_record_native_failure(device->core);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  if (native_heap.size < size ||
      !vkr_metal_memory_has_budget(device, charged_size)) {
    [native_heap release];
    vkr_metal_memory_budget_reject(
        device, charged_size, group != VKR_METAL_HEAP_SCENE_IMAGES);
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  }
  VkrMetalMemoryStatus status =
      (VkrMetalMemoryStatus)vkr_gpu_memory_allocate_in_range(
          device->core, size_align.size, size_align.align, kind, offset, size,
          out_handle, out_placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    [native_heap release];
    return status;
  }
  device->heaps[empty_index] =
      (VkrMetalNativeHeap){.heap = native_heap,
                           .logical_offset = offset,
                           .logical_size = size,
                           .charged_size = charged_size,
                           .allocation_count = 1u,
                           .group = group};
  device->heap_slot_count = Max(device->heap_slot_count, empty_index + 1u);
  device->native_heap_charged_size += charged_size;
  device->native_heap_count++;
  device->native_heap_total_count++;
  device->native_heap_peak_count =
      Max(device->native_heap_peak_count, device->native_heap_count);
  vkr_metal_memory_update_peaks(device);
  device->resource_heaps[out_handle->index] = empty_index;
  *out_heap_index = empty_index;
  [device->residency addAllocation:native_heap];
  [device->residency commit];
  vkr_metal_memory_snapshot(device, "memory.heap_create");
  return VKR_METAL_MEMORY_STATUS_OK;
}

vkr_internal bool8_t vkr_metal_memory_reconcile_heap(
    VkrMetalMemoryDevice *device, uint32_t heap_index) {
  VkrMetalNativeHeap *heap = &device->heaps[heap_index];
  const uint64_t actual =
      Max((uint64_t)heap->heap.size, (uint64_t)heap->heap.currentAllocatedSize);
  if (actual > heap->charged_size) {
    const uint64_t extra = actual - heap->charged_size;
    if (!vkr_metal_memory_has_budget(device, extra)) {
      vkr_metal_memory_budget_reject(
          device, extra, heap->group != VKR_METAL_HEAP_SCENE_IMAGES);
      return false_v;
    }
    device->native_heap_charged_size += extra;
    heap->charged_size = actual;
  }
  vkr_metal_memory_update_peaks(device);
  return true_v;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_create(const VkrMetalMemoryDeviceConfig *config,
                               VkrMetalMemoryDevice **out_device) {
  if (!config || !out_device || !config->metal_device || !config->allocator ||
      config->managed_budget_size == 0 || config->heap_chunk_size == 0 ||
      config->upload_ring_size == 0 || config->readback_ring_size == 0 ||
      config->upload_ring_size > config->upload_ring_max_size ||
      config->readback_ring_size > config->readback_ring_max_size ||
      config->ring_slot_count == 0 || config->max_allocations == 0 ||
      config->max_allocations == UINT32_MAX ||
      config->max_retirements < config->max_allocations ||
      config->max_free_ranges < config->max_allocations + 1u)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  *out_device = NULL;
  if (config->upload_ring_size > config->managed_budget_size ||
      config->readback_ring_size >
          config->managed_budget_size - config->upload_ring_size) {
    log_error("Metal transfer rings exceed managed GPU budget: upload=%llu "
              "readback=%llu cap=%llu bytes",
              (unsigned long long)config->upload_ring_size,
              (unsigned long long)config->readback_ring_size,
              (unsigned long long)config->managed_budget_size);
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  }

  if (@available(macOS 26.0, *)) {
    VkrMetalMemoryDevice *memory =
        vkr_metal_memory_device_alloc(config->allocator, sizeof(*memory));
    if (!memory)
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    memory->allocator = config->allocator;
    memory->upload_ring_max_size = config->upload_ring_max_size;
    memory->readback_ring_max_size = config->readback_ring_max_size;
    memory->diagnostics = config->diagnostics;
    memory->max_allocations = config->max_allocations;
    memory->managed_budget_size = config->managed_budget_size;
    memory->heap_chunk_size = config->heap_chunk_size;
    memory->device = [(id<MTLDevice>)config->metal_device retain];
    if (!memory->device ||
        ![memory->device supportsFamily:MTLGPUFamilyMetal4]) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }

    const VkrMetalMemoryConfig core_config = {
        .heap_size = config->managed_budget_size,
        .max_allocations = config->max_allocations,
        .max_retirements = config->max_retirements,
        .max_free_ranges = config->max_free_ranges,
    };
    const uint64_t core_storage_size =
        vkr_metal_memory_storage_requirement(&core_config);
    const uint64_t ring_storage_size =
        vkr_metal_submit_ring_storage_requirement(config->ring_slot_count);
    memory->core_storage_size = core_storage_size;
    memory->ring_storage_size = ring_storage_size;
    memory->native_resources_size =
        (uint64_t)config->max_allocations * sizeof(*memory->native_resources);
    memory->logical_lengths_size =
        (uint64_t)config->max_allocations * sizeof(*memory->logical_lengths);
    memory->logical_owners_size =
        (uint64_t)config->max_allocations * sizeof(*memory->logical_owners);
    memory->heaps_size =
        (uint64_t)config->max_allocations * sizeof(*memory->heaps);
    memory->resource_heaps_size =
        (uint64_t)config->max_allocations * sizeof(*memory->resource_heaps);
    memory->heaps =
        vkr_metal_memory_device_alloc(config->allocator, memory->heaps_size);
    memory->resource_heaps = vkr_metal_memory_device_alloc(
        config->allocator, memory->resource_heaps_size);
    memory->core_storage =
        vkr_metal_memory_device_alloc(config->allocator, core_storage_size);
    memory->upload_ring_storage =
        vkr_metal_memory_device_alloc(config->allocator, ring_storage_size);
    memory->readback_ring_storage =
        vkr_metal_memory_device_alloc(config->allocator, ring_storage_size);
    memory->native_resources = vkr_metal_memory_device_alloc(
        config->allocator, memory->native_resources_size);
    memory->logical_lengths = vkr_metal_memory_device_alloc(
        config->allocator, memory->logical_lengths_size);
    memory->logical_owners = vkr_metal_memory_device_alloc(
        config->allocator, memory->logical_owners_size);
    if (!memory->heaps || !memory->resource_heaps || !memory->core_storage ||
        !memory->upload_ring_storage || !memory->readback_ring_storage ||
        !memory->native_resources || !memory->logical_lengths ||
        !memory->logical_owners ||
        vkr_metal_memory_create(&core_config, memory->core_storage,
                                core_storage_size,
                                &memory->core) != VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_submit_ring_create(
            &memory->upload_ring, config->upload_ring_size,
            config->ring_slot_count, memory->upload_ring_storage,
            ring_storage_size) != VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_submit_ring_create(
            &memory->readback_ring, config->readback_ring_size,
            config->ring_slot_count, memory->readback_ring_storage,
            ring_storage_size) != VKR_METAL_MEMORY_STATUS_OK) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }

    memory->upload_buffer = [memory->device
        newBufferWithLength:config->upload_ring_size
                    options:MTLResourceStorageModeShared |
                            MTLResourceCPUCacheModeWriteCombined];
    if (!memory->upload_buffer || memory->upload_buffer.gpuAddress == 0) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }
    const uint64_t upload_size =
        Max(config->upload_ring_size,
            (uint64_t)memory->upload_buffer.allocatedSize);
    if (!vkr_metal_memory_has_budget(memory, upload_size)) {
      vkr_metal_memory_budget_reject(memory, upload_size, true_v);
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
    }
    memory->transfer_ring_allocated_size = upload_size;
    if (!vkr_metal_memory_has_budget(memory, config->readback_ring_size)) {
      vkr_metal_memory_budget_reject(memory, config->readback_ring_size, true_v);
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
    }
    memory->readback_buffer = [memory->device
        newBufferWithLength:config->readback_ring_size
                    options:MTLResourceStorageModeShared |
                            MTLResourceCPUCacheModeDefaultCache];
    if (!memory->readback_buffer || memory->readback_buffer.gpuAddress == 0) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }
    const uint64_t readback_size =
        Max(config->readback_ring_size,
            (uint64_t)memory->readback_buffer.allocatedSize);
    if (!vkr_metal_memory_has_budget(memory, readback_size)) {
      vkr_metal_memory_budget_reject(memory, readback_size, true_v);
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
    }
    memory->transfer_ring_allocated_size += readback_size;
    vkr_metal_memory_update_peaks(memory);
    memory->upload_addresses = (VkrMetalAddressPair){
        memory->upload_buffer.contents, memory->upload_buffer.gpuAddress,
        config->upload_ring_size};
    memory->readback_addresses = (VkrMetalAddressPair){
        memory->readback_buffer.contents, memory->readback_buffer.gpuAddress,
        config->readback_ring_size};

    MTLResidencySetDescriptor *residency_desc = [MTLResidencySetDescriptor new];
    residency_desc.label = @"VKR Metal placement heap and transfer rings";
    residency_desc.initialCapacity = Min(config->max_allocations, 64u) + 2u;
    NSError *error = nil;
    memory->residency =
        [memory->device newResidencySetWithDescriptor:residency_desc
                                                error:&error];
    [residency_desc release];
    if (!memory->residency) {
      (void)error;
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }
    [memory->residency addAllocation:memory->upload_buffer];
    [memory->residency addAllocation:memory->readback_buffer];
    [memory->residency commit];
    [memory->residency requestResidency];
    vkr_metal_memory_owner_record_allocate(memory->owners,
                                           VKR_GPU_ALLOCATION_OWNER_STAGING,
                                           config->upload_ring_size);
    vkr_metal_memory_owner_record_allocate(memory->owners,
                                           VKR_GPU_ALLOCATION_OWNER_READBACK,
                                           config->readback_ring_size);
    vkr_metal_memory_snapshot(memory, "memory.create");
    *out_device = memory;
    return VKR_METAL_MEMORY_STATUS_OK;
  }
  return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
}

VkrMetalMemoryStatus vkr_metal_memory_device_create_buffer(
    VkrMetalMemoryDevice *device, uint64_t length, VkrGpuAllocationOwner owner,
    VkrMetalBufferResource *out_buffer) {
  if (!device || !out_buffer || length == 0)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  const MTLResourceOptions options =
      MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
  const MTLSizeAndAlign size_align =
      [device->device heapBufferSizeAndAlignWithLength:length options:options];
  VkrMetalAllocationHandle handle = {0};
  VkrMetalPlacement placement = {0};
  uint32_t heap_index = 0;
  VkrMetalMemoryStatus status =
      vkr_metal_memory_place(device, size_align, VKR_METAL_RESOURCE_KIND_BUFFER,
                             owner, &handle, &placement, &heap_index);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  const VkrGpuAllocationOwner normalized_owner =
      vkr_gpu_allocation_owner_normalize(owner);
  id<MTLBuffer> buffer = [device->heaps[heap_index].heap
      newBufferWithLength:length
                  options:options
                   offset:placement.resource_offset -
                          device->heaps[heap_index].logical_offset];
  if (!buffer || buffer.gpuAddress == 0 ||
      !vkr_metal_memory_reconcile_heap(device, heap_index)) {
    [buffer release];
    vkr_metal_memory_record_native_failure(device->core);
    (void)vkr_metal_memory_abandon(device, handle);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  device->logical_owners[handle.index] = normalized_owner;
  vkr_metal_memory_owner_record_allocate(device->owners, normalized_owner,
                                         size_align.size);
  device->native_resources[handle.index] = buffer;
  device->logical_lengths[handle.index] = length;
  device->native_live_resources++;
  *out_buffer =
      (VkrMetalBufferResource){handle, buffer, buffer.gpuAddress, length};
  if (device->diagnostics && device->diagnostics->enabled)
    vkr_metal_diagnostics_record(
        device->diagnostics, "memory.buffer_create", 0u, 0u,
        "handle=%u:%u owner=%u bytes=%llu length=%llu offset=%llu "
        "gpu_address=%llu",
        handle.index, handle.generation, (uint32_t)normalized_owner,
        (unsigned long long)placement.resource_size, (unsigned long long)length,
        (unsigned long long)placement.resource_offset,
        (unsigned long long)out_buffer->gpu_address);
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus vkr_metal_memory_device_create_texture(
    VkrMetalMemoryDevice *device, void *metal_texture_descriptor,
    VkrGpuAllocationOwner owner, VkrMetalTextureResource *out_texture) {
  if (!device || !metal_texture_descriptor || !out_texture)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  MTLTextureDescriptor *descriptor =
      [(MTLTextureDescriptor *)metal_texture_descriptor copy];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  const MTLSizeAndAlign size_align =
      [device->device heapTextureSizeAndAlignWithDescriptor:descriptor];
  VkrMetalAllocationHandle handle = {0};
  VkrMetalPlacement placement = {0};
  uint32_t heap_index = 0;
  VkrMetalMemoryStatus status = vkr_metal_memory_place(
      device, size_align, VKR_METAL_RESOURCE_KIND_TEXTURE, owner, &handle, &placement,
      &heap_index);
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    [descriptor release];
    return status;
  }
  const VkrGpuAllocationOwner normalized_owner =
      vkr_gpu_allocation_owner_normalize(owner);
  id<MTLTexture> texture = [device->heaps[heap_index].heap
      newTextureWithDescriptor:descriptor
                        offset:placement.resource_offset -
                               device->heaps[heap_index].logical_offset];
  [descriptor release];
  if (!texture || texture.gpuResourceID._impl == 0 ||
      !vkr_metal_memory_reconcile_heap(device, heap_index)) {
    [texture release];
    vkr_metal_memory_record_native_failure(device->core);
    (void)vkr_metal_memory_abandon(device, handle);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  device->logical_owners[handle.index] = normalized_owner;
  vkr_metal_memory_owner_record_allocate(device->owners, normalized_owner,
                                         size_align.size);
  device->native_resources[handle.index] = texture;
  device->native_live_resources++;
  *out_texture =
      (VkrMetalTextureResource){handle, texture, texture.gpuResourceID._impl};
  if (device->diagnostics && device->diagnostics->enabled)
    vkr_metal_diagnostics_record(
        device->diagnostics, "memory.texture_create", 0u, 0u,
        "handle=%u:%u owner=%u bytes=%llu offset=%llu resource_id=%llu",
        handle.index, handle.generation, (uint32_t)normalized_owner,
        (unsigned long long)placement.resource_size,
        (unsigned long long)placement.resource_offset,
        (unsigned long long)out_texture->resource_id);
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_resolve_buffer(VkrMetalMemoryDevice *device,
                                       VkrMetalAllocationHandle handle,
                                       VkrMetalBufferResource *out_buffer) {
  if (!device || !out_buffer)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalPlacement placement = {0};
  VkrMetalMemoryStatus status =
      vkr_metal_memory_resolve(device->core, handle, &placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  if (placement.kind != VKR_METAL_RESOURCE_KIND_BUFFER ||
      !device->native_resources[handle.index])
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  id<MTLBuffer> buffer = (id<MTLBuffer>)device->native_resources[handle.index];
  *out_buffer = (VkrMetalBufferResource){handle, buffer, buffer.gpuAddress,
                                         device->logical_lengths[handle.index]};
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_resolve_texture(VkrMetalMemoryDevice *device,
                                        VkrMetalAllocationHandle handle,
                                        VkrMetalTextureResource *out_texture) {
  if (!device || !out_texture)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalPlacement placement = {0};
  VkrMetalMemoryStatus status =
      vkr_metal_memory_resolve(device->core, handle, &placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  if (placement.kind != VKR_METAL_RESOURCE_KIND_TEXTURE ||
      !device->native_resources[handle.index])
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  id<MTLTexture> texture =
      (id<MTLTexture>)device->native_resources[handle.index];
  *out_texture =
      (VkrMetalTextureResource){handle, texture, texture.gpuResourceID._impl};
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_retire(VkrMetalMemoryDevice *device,
                               VkrMetalAllocationHandle handle,
                               uint64_t last_use_submit_value) {
  if (!device)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalPlacement placement = {0};
  VkrMetalMemoryStatus status =
      vkr_metal_memory_resolve(device->core, handle, &placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  const VkrGpuAllocationOwner owner = device->logical_owners[handle.index];
  const VkrGpuAllocationOwnerTotals *totals = &device->owners[owner];
  if (!totals->live_allocation_count ||
      totals->live_bytes < placement.resource_size)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  status = vkr_metal_memory_retire(device->core, handle, last_use_submit_value);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  status = vkr_metal_memory_owner_record_release(device->owners, owner,
                                                 placement.resource_size)
               ? VKR_METAL_MEMORY_STATUS_OK
               : VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  if (device->diagnostics && device->diagnostics->enabled)
    vkr_metal_diagnostics_record(
        device->diagnostics, "memory.retire", 0u, 0u,
        "handle=%u:%u owner=%u bytes=%llu retire_after=%llu status=%u",
        handle.index, handle.generation, (uint32_t)owner,
        (unsigned long long)placement.resource_size,
        (unsigned long long)last_use_submit_value, (uint32_t)status);
  return status;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_collect(VkrMetalMemoryDevice *device,
                                uint64_t completed_submit_value,
                                uint32_t *out_collected_count) {
  if (!device)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  uint32_t collected_count = 0u;
  uint32_t *collected =
      out_collected_count ? out_collected_count : &collected_count;
  const VkrMetalMemoryStatus status = vkr_metal_memory_collect(
      device->core, completed_submit_value, vkr_metal_memory_release_retired,
      device, collected);
  if (device->diagnostics && device->diagnostics->enabled &&
      (status != VKR_METAL_MEMORY_STATUS_OK || *collected != 0u))
    vkr_metal_diagnostics_record(
        device->diagnostics, "memory.collect", 0u, completed_submit_value,
        "reclaimed_count=%u native_live=%llu status=%u", *collected,
        (unsigned long long)device->native_live_resources, (uint32_t)status);
  return status;
}

vkr_internal VkrMetalSubmitRing *vkr_metal_memory_select_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    VkrMetalAddressPair **out_addresses, id<MTLBuffer> *out_buffer) {
  if (ring_kind == VKR_METAL_RING_KIND_UPLOAD) {
    *out_addresses = &device->upload_addresses;
    *out_buffer = device->upload_buffer;
    return &device->upload_ring;
  }
  if (ring_kind == VKR_METAL_RING_KIND_READBACK) {
    *out_addresses = &device->readback_addresses;
    *out_buffer = device->readback_buffer;
    return &device->readback_ring;
  }
  return NULL;
}

VkrMetalMemoryStatus vkr_metal_memory_device_acquire_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    uint64_t requested_size, uint64_t completed_submit_value,
    VkrMetalRingSlice *out_slice, VkrMetalAddressPair *out_addresses,
    void **out_metal_buffer) {
  if (!device || !out_slice || !out_addresses || !out_metal_buffer)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalAddressPair *whole = NULL;
  id<MTLBuffer> buffer = nil;
  VkrMetalSubmitRing *ring =
      vkr_metal_memory_select_ring(device, ring_kind, &whole, &buffer);
  if (!ring || !buffer || !whole->cpu_address)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  uint64_t *max_requested = ring_kind == VKR_METAL_RING_KIND_UPLOAD
                                ? &device->upload_ring_max_requested_bytes
                                : &device->readback_ring_max_requested_bytes;
  uint64_t *oversize_failures = ring_kind == VKR_METAL_RING_KIND_UPLOAD
                                    ? &device->upload_ring_oversize_failures
                                    : &device->readback_ring_oversize_failures;
  *max_requested = Max(*max_requested, requested_size);
  if (requested_size > ring->slot_size)
    (*oversize_failures)++;
  VkrMetalMemoryStatus status = vkr_metal_submit_ring_acquire(
      ring, requested_size, completed_submit_value, out_slice);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  *out_addresses = vkr_metal_address_pair_slice(*whole, *out_slice);
  *out_metal_buffer = buffer;
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus vkr_metal_memory_device_submit_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    VkrMetalRingSlice slice, uint64_t submit_value) {
  if (!device)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalAddressPair *addresses = NULL;
  id<MTLBuffer> buffer = nil;
  VkrMetalSubmitRing *ring =
      vkr_metal_memory_select_ring(device, ring_kind, &addresses, &buffer);
  (void)addresses;
  (void)buffer;
  return ring ? vkr_metal_submit_ring_submit(ring, slice, submit_value)
              : VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
}

uint64_t
vkr_metal_memory_device_ring_slot_capacity(const VkrMetalMemoryDevice *device,
                                           VkrMetalRingKind ring_kind) {
  if (!device)
    return 0u;
  if (ring_kind == VKR_METAL_RING_KIND_UPLOAD && !device->upload_buffer)
    return 0u;
  if (ring_kind == VKR_METAL_RING_KIND_READBACK && !device->readback_buffer)
    return 0u;
  return ring_kind == VKR_METAL_RING_KIND_UPLOAD ? device->upload_ring.slot_size
         : ring_kind == VKR_METAL_RING_KIND_READBACK
             ? device->readback_ring.slot_size
             : 0u;
}

VkrMetalMemoryStatus vkr_metal_memory_device_grow_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    uint64_t required_slot_size, uint64_t completed_submit_value) {
  if (!device || required_slot_size == 0u)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  VkrMetalAddressPair *addresses = NULL;
  id<MTLBuffer> *buffer = NULL;
  VkrMetalSubmitRing *ring = NULL;
  uint64_t max_size = 0u;
  if (ring_kind == VKR_METAL_RING_KIND_UPLOAD) {
    addresses = &device->upload_addresses;
    buffer = &device->upload_buffer;
    ring = &device->upload_ring;
    max_size = device->upload_ring_max_size;
  } else if (ring_kind == VKR_METAL_RING_KIND_READBACK) {
    addresses = &device->readback_addresses;
    buffer = &device->readback_buffer;
    ring = &device->readback_ring;
    max_size = device->readback_ring_max_size;
  } else {
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  }
  if (*buffer && required_slot_size <= ring->slot_size)
    return VKR_METAL_MEMORY_STATUS_OK;
  if (required_slot_size > max_size / ring->slot_count)
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  for (uint32_t i = 0u; i < ring->slot_count; ++i) {
    const VkrMetalSubmitRingSlot *slot = &ring->slots[i];
    if (slot->state == 1u || (slot->state == 2u && slot->retire_submit_value >
                                                       completed_submit_value))
      return VKR_METAL_MEMORY_STATUS_RING_BUSY;
  }
  uint64_t total_size = ring->total_size;
  while (total_size / ring->slot_count < required_slot_size) {
    if (total_size >= max_size)
      return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
    total_size = Min(total_size * 2u, max_size);
  }
  const uint64_t old_total_size = ring->total_size;
  const VkrGpuAllocationOwner owner = ring_kind == VKR_METAL_RING_KIND_UPLOAD
                                          ? VKR_GPU_ALLOCATION_OWNER_STAGING
                                          : VKR_GPU_ALLOCATION_OWNER_READBACK;
  const uint64_t old_size =
      *buffer ? Max(old_total_size, (uint64_t)(*buffer).allocatedSize) : 0u;
  const MTLResourceOptions options =
      ring_kind == VKR_METAL_RING_KIND_UPLOAD
          ? MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined
          : MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
  if (total_size > device->managed_budget_size -
                       (vkr_metal_memory_managed_size(device) - old_size))
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  /* Every old slice and CPU readback consumer is proven drained above. Release
     the old backing first so a growth never creates an old-plus-new peak. */
  if (*buffer) {
    [device->residency removeAllocation:*buffer];
    [device->residency commit];
    vkr_metal_memory_release(*buffer);
    (void)vkr_metal_memory_owner_record_release(device->owners, owner,
                                                old_total_size);
  }
  *buffer = nil;
  *addresses = (VkrMetalAddressPair){0};
  device->transfer_ring_allocated_size -= old_size;
  id<MTLBuffer> replacement = [device->device newBufferWithLength:total_size
                                                          options:options];
  if (!replacement || replacement.gpuAddress == 0u) {
    vkr_metal_memory_release(replacement);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  const uint64_t replacement_size =
      Max(total_size, (uint64_t)replacement.allocatedSize);
  if (!vkr_metal_memory_has_budget(device, replacement_size)) {
    vkr_metal_memory_release(replacement);
    vkr_metal_memory_budget_reject(device, replacement_size, true_v);
    return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
  }
  [device->residency addAllocation:replacement];
  [device->residency commit];
  *buffer = replacement;
  device->transfer_ring_allocated_size += replacement_size;
  vkr_metal_memory_owner_record_allocate(device->owners, owner, total_size);
  const uint64_t acquires = ring->acquires;
  const uint64_t reuses = ring->reuses;
  const uint64_t busy_failures = ring->busy_failures;
  (void)vkr_metal_submit_ring_create(ring, total_size, ring->slot_count,
                                     ring_kind == VKR_METAL_RING_KIND_UPLOAD
                                         ? device->upload_ring_storage
                                         : device->readback_ring_storage,
                                     device->ring_storage_size);
  ring->acquires = acquires;
  ring->reuses = reuses;
  ring->busy_failures = busy_failures;
  *addresses = (VkrMetalAddressPair){replacement.contents,
                                     replacement.gpuAddress, total_size};
  vkr_metal_memory_update_peaks(device);
  return VKR_METAL_MEMORY_STATUS_OK;
}

void vkr_metal_memory_device_cancel_ring(VkrMetalMemoryDevice *device,
                                         VkrMetalRingKind ring_kind,
                                         VkrMetalRingSlice slice) {
  if (!device)
    return;
  VkrMetalAddressPair *addresses = NULL;
  id<MTLBuffer> buffer = nil;
  VkrMetalSubmitRing *ring =
      vkr_metal_memory_select_ring(device, ring_kind, &addresses, &buffer);
  (void)addresses;
  (void)buffer;
  if (ring)
    vkr_metal_submit_ring_cancel(ring, slice);
}

void *vkr_metal_memory_device_residency_set(VkrMetalMemoryDevice *device) {
  return device ? device->residency : nil;
}

void vkr_metal_memory_device_get_metrics(
    VkrMetalMemoryDevice *device, VkrMetalMemoryDeviceMetrics *out_metrics) {
  if (!out_metrics)
    return;
  if (!device) {
    *out_metrics = (VkrMetalMemoryDeviceMetrics){0};
    return;
  }
  *out_metrics = (VkrMetalMemoryDeviceMetrics){0};
  vkr_metal_memory_get_metrics(device->core, &out_metrics->suballocations);
  MemCopy(out_metrics->owners, device->owners, sizeof(out_metrics->owners));
  out_metrics->managed_budget_size = device->managed_budget_size;
  out_metrics->managed_peak_allocated_size =
      device->managed_peak_allocated_size;
  out_metrics->external_allocation_count = device->external_allocation_count;
  out_metrics->native_heap_peak_count = device->native_heap_peak_count;
  out_metrics->native_heap_total_count = device->native_heap_total_count;
  out_metrics->max_native_heaps = device->max_allocations;
  out_metrics->managed_allocated_size = vkr_metal_memory_managed_size(device);
  out_metrics->external_allocated_size = device->external_allocated_size;
  out_metrics->transfer_ring_allocated_size =
      device->transfer_ring_allocated_size;
  for (uint32_t i = 0; i < device->heap_slot_count; ++i) {
    id<MTLHeap> heap = device->heaps[i].heap;
    if (!heap)
      continue;
    out_metrics->native_heap_count++;
    if (device->heaps[i].group == VKR_METAL_HEAP_ASSET_TEXTURES)
      out_metrics->texture_heap_capacity_bytes += device->heaps[i].charged_size;
    out_metrics->native_heap_size += heap.size;
    out_metrics->native_heap_used_size += heap.usedSize;
    out_metrics->native_heap_allocated_size += heap.currentAllocatedSize;
    out_metrics->native_heap_largest_free_range =
        Max(out_metrics->native_heap_largest_free_range,
            (uint64_t)[heap maxAvailableSizeWithAlignment:1]);
  }
  device->native_heap_peak_allocated_size =
      Max(device->native_heap_peak_allocated_size,
          out_metrics->native_heap_allocated_size);
  out_metrics->native_heap_peak_allocated_size =
      device->native_heap_peak_allocated_size;
  out_metrics->residency_allocated_size = device->residency.allocatedSize;
  out_metrics->driver_current_allocated_size =
      device->device.currentAllocatedSize;
  out_metrics->driver_recommended_working_set_size =
      device->device.recommendedMaxWorkingSetSize;
  out_metrics->residency_allocation_count = device->residency.allocationCount;
  out_metrics->native_live_resources = device->native_live_resources;
  out_metrics->native_resources_released = device->native_resources_released;
  out_metrics->upload_ring_acquires = device->upload_ring.acquires;
  out_metrics->upload_ring_reuses = device->upload_ring.reuses;
  out_metrics->upload_ring_busy_failures = device->upload_ring.busy_failures;
  out_metrics->upload_ring_total_capacity_bytes =
      device->upload_buffer ? device->upload_ring.total_size : 0u;
  out_metrics->upload_ring_slot_capacity_bytes =
      device->upload_buffer ? device->upload_ring.slot_size : 0u;
  out_metrics->upload_ring_max_requested_bytes =
      device->upload_ring_max_requested_bytes;
  out_metrics->upload_ring_oversize_failures =
      device->upload_ring_oversize_failures;
  out_metrics->readback_ring_acquires = device->readback_ring.acquires;
  out_metrics->readback_ring_reuses = device->readback_ring.reuses;
  out_metrics->readback_ring_busy_failures =
      device->readback_ring.busy_failures;
  out_metrics->readback_ring_total_capacity_bytes =
      device->readback_buffer ? device->readback_ring.total_size : 0u;
  out_metrics->readback_ring_slot_capacity_bytes =
      device->readback_buffer ? device->readback_ring.slot_size : 0u;
  out_metrics->readback_ring_max_requested_bytes =
      device->readback_ring_max_requested_bytes;
  out_metrics->readback_ring_oversize_failures =
      device->readback_ring_oversize_failures;
}

void vkr_metal_memory_device_destroy(VkrMetalMemoryDevice *device) {
  if (!device)
    return;
  if (@available(macOS 26.0, *)) {
    if (device->core)
      (void)vkr_metal_memory_device_collect(device, UINT64_MAX, NULL);
    if (device->native_resources) {
      for (uint32_t i = 0; i < device->max_allocations; ++i)
        vkr_metal_memory_release(device->native_resources[i]);
    }
    [device->residency endResidency];
    vkr_metal_memory_release(device->residency);
    vkr_metal_memory_release(device->readback_buffer);
    vkr_metal_memory_release(device->upload_buffer);
    if (device->heaps) {
      for (uint32_t i = 0; i < device->heap_slot_count; ++i)
        vkr_metal_memory_release(device->heaps[i].heap);
    }
    vkr_metal_memory_release(device->device);
  }
  VkrAllocator *allocator = device->allocator;
  vkr_metal_memory_device_free(allocator, device->resource_heaps,
                               device->resource_heaps_size);
  vkr_metal_memory_device_free(allocator, device->heaps, device->heaps_size);
  vkr_metal_memory_device_free(allocator, device->logical_owners,
                               device->logical_owners_size);
  vkr_metal_memory_device_free(allocator, device->logical_lengths,
                               device->logical_lengths_size);
  vkr_metal_memory_device_free(allocator, device->native_resources,
                               device->native_resources_size);
  vkr_metal_memory_device_free(allocator, device->readback_ring_storage,
                               device->ring_storage_size);
  vkr_metal_memory_device_free(allocator, device->upload_ring_storage,
                               device->ring_storage_size);
  vkr_metal_memory_device_free(allocator, device->core_storage,
                               device->core_storage_size);
  vkr_metal_memory_device_free(allocator, device, sizeof(*device));
}

#endif
