#include "renderer/metal/vkr_metal_memory_device.h"

#if defined(PLATFORM_APPLE)

#import <Metal/Metal.h>

#include <stdlib.h>

struct VkrMetalMemoryDevice {
  id<MTLDevice> device;
  id<MTLHeap> heap;
  id<MTLBuffer> upload_buffer;
  id<MTLBuffer> readback_buffer;
  id<MTLResidencySet> residency;
  id<MTLResource> *native_resources;
  uint64_t *logical_lengths;
  void *core_storage;
  void *upload_ring_storage;
  void *readback_ring_storage;
  VkrMetalMemoryCore *core;
  VkrMetalSubmitRing upload_ring;
  VkrMetalSubmitRing readback_ring;
  VkrMetalAddressPair upload_addresses;
  VkrMetalAddressPair readback_addresses;
  uint32_t max_allocations;
  uint64_t native_live_resources;
  uint64_t native_resources_released;
  uint64_t native_heap_peak_allocated_size;
};

static void vkr_metal_memory_release(id object) {
  if (object)
    [object release];
}

static void
vkr_metal_memory_release_retired(void *context, uint32_t slot_index,
                                 const VkrMetalPlacement *placement) {
  (void)placement;
  VkrMetalMemoryDevice *device = context;
  id<MTLResource> resource = device->native_resources[slot_index];
  vkr_metal_memory_release(resource);
  device->native_resources[slot_index] = nil;
  device->logical_lengths[slot_index] = 0;
  if (resource) {
    device->native_live_resources--;
    device->native_resources_released++;
  }
}

static VkrMetalMemoryStatus
vkr_metal_memory_abandon(VkrMetalMemoryDevice *device,
                         VkrMetalAllocationHandle handle) {
  VkrMetalMemoryStatus status =
      vkr_metal_memory_retire(device->core, handle, 0);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  return vkr_metal_memory_collect(
      device->core, 0, vkr_metal_memory_release_retired, device, NULL);
}

VkrMetalMemoryStatus
vkr_metal_memory_device_create(const VkrMetalMemoryDeviceConfig *config,
                               VkrMetalMemoryDevice **out_device) {
  if (!config || !out_device || !config->metal_device ||
      config->heap_size == 0 || config->upload_ring_size == 0 ||
      config->readback_ring_size == 0 || config->ring_slot_count == 0 ||
      config->max_allocations == 0 || config->max_retirements == 0 ||
      config->max_free_ranges == 0)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  *out_device = NULL;

  if (@available(macOS 26.0, *)) {
    VkrMetalMemoryDevice *memory = calloc(1, sizeof(*memory));
    if (!memory)
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    memory->max_allocations = config->max_allocations;
    memory->device = [(id<MTLDevice>)config->metal_device retain];
    if (!memory->device ||
        ![memory->device supportsFamily:MTLGPUFamilyMetal4]) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }

    const VkrMetalMemoryConfig core_config = {
        .heap_size = config->heap_size,
        .max_allocations = config->max_allocations,
        .max_retirements = config->max_retirements,
        .max_free_ranges = config->max_free_ranges,
    };
    const uint64_t core_storage_size =
        vkr_metal_memory_storage_requirement(&core_config);
    const uint64_t ring_storage_size =
        vkr_metal_submit_ring_storage_requirement(config->ring_slot_count);
    memory->core_storage = malloc(core_storage_size);
    memory->upload_ring_storage = malloc(ring_storage_size);
    memory->readback_ring_storage = malloc(ring_storage_size);
    memory->native_resources =
        calloc(config->max_allocations, sizeof(*memory->native_resources));
    memory->logical_lengths =
        calloc(config->max_allocations, sizeof(*memory->logical_lengths));
    if (!memory->core_storage || !memory->upload_ring_storage ||
        !memory->readback_ring_storage || !memory->native_resources ||
        !memory->logical_lengths ||
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

    MTLHeapDescriptor *heap_desc = [MTLHeapDescriptor new];
    heap_desc.type = MTLHeapTypePlacement;
    heap_desc.size = config->heap_size;
    heap_desc.storageMode = MTLStorageModePrivate;
    heap_desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    memory->heap = [memory->device newHeapWithDescriptor:heap_desc];
    [heap_desc release];
    memory->upload_buffer = [memory->device
        newBufferWithLength:config->upload_ring_size
                    options:MTLResourceStorageModeShared |
                            MTLResourceCPUCacheModeWriteCombined];
    memory->readback_buffer = [memory->device
        newBufferWithLength:config->readback_ring_size
                    options:MTLResourceStorageModeShared |
                            MTLResourceCPUCacheModeDefaultCache];
    if (!memory->heap || !memory->upload_buffer || !memory->readback_buffer ||
        memory->upload_buffer.gpuAddress == 0 ||
        memory->readback_buffer.gpuAddress == 0) {
      vkr_metal_memory_device_destroy(memory);
      return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
    }
    memory->upload_addresses = (VkrMetalAddressPair){
        memory->upload_buffer.contents, memory->upload_buffer.gpuAddress,
        config->upload_ring_size};
    memory->readback_addresses = (VkrMetalAddressPair){
        memory->readback_buffer.contents, memory->readback_buffer.gpuAddress,
        config->readback_ring_size};

    MTLResidencySetDescriptor *residency_desc = [MTLResidencySetDescriptor new];
    residency_desc.label = @"VKR Metal placement heap and transfer rings";
    residency_desc.initialCapacity = 3;
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
    [memory->residency addAllocation:memory->heap];
    [memory->residency addAllocation:memory->upload_buffer];
    [memory->residency addAllocation:memory->readback_buffer];
    [memory->residency commit];
    [memory->residency requestResidency];
    *out_device = memory;
    return VKR_METAL_MEMORY_STATUS_OK;
  }
  return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_create_buffer(VkrMetalMemoryDevice *device,
                                      uint64_t length,
                                      VkrMetalBufferResource *out_buffer) {
  if (!device || !out_buffer || length == 0)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  const MTLResourceOptions options =
      MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
  const MTLSizeAndAlign size_align =
      [device->device heapBufferSizeAndAlignWithLength:length options:options];
  VkrMetalAllocationHandle handle = {0};
  VkrMetalPlacement placement = {0};
  VkrMetalMemoryStatus status = vkr_metal_memory_allocate(
      device->core, size_align.size, size_align.align,
      VKR_METAL_RESOURCE_KIND_BUFFER, &handle, &placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK)
    return status;
  id<MTLBuffer> buffer =
      [device->heap newBufferWithLength:length
                                options:options
                                 offset:placement.resource_offset];
  if (!buffer || buffer.gpuAddress == 0) {
    [buffer release];
    vkr_metal_memory_record_native_failure(device->core);
    (void)vkr_metal_memory_abandon(device, handle);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  device->native_resources[handle.index] = buffer;
  device->logical_lengths[handle.index] = length;
  device->native_live_resources++;
  *out_buffer =
      (VkrMetalBufferResource){handle, buffer, buffer.gpuAddress, length};
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus
vkr_metal_memory_device_create_texture(VkrMetalMemoryDevice *device,
                                       void *metal_texture_descriptor,
                                       VkrMetalTextureResource *out_texture) {
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
  VkrMetalMemoryStatus status = vkr_metal_memory_allocate(
      device->core, size_align.size, size_align.align,
      VKR_METAL_RESOURCE_KIND_TEXTURE, &handle, &placement);
  if (status != VKR_METAL_MEMORY_STATUS_OK) {
    [descriptor release];
    return status;
  }
  id<MTLTexture> texture =
      [device->heap newTextureWithDescriptor:descriptor
                                      offset:placement.resource_offset];
  [descriptor release];
  if (!texture || texture.gpuResourceID._impl == 0) {
    [texture release];
    vkr_metal_memory_record_native_failure(device->core);
    (void)vkr_metal_memory_abandon(device, handle);
    return VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED;
  }
  device->native_resources[handle.index] = texture;
  device->native_live_resources++;
  *out_texture =
      (VkrMetalTextureResource){handle, texture, texture.gpuResourceID._impl};
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
  return vkr_metal_memory_retire(device->core, handle, last_use_submit_value);
}

VkrMetalMemoryStatus
vkr_metal_memory_device_collect(VkrMetalMemoryDevice *device,
                                uint64_t completed_submit_value,
                                uint32_t *out_collected_count) {
  if (!device)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  return vkr_metal_memory_collect(device->core, completed_submit_value,
                                  vkr_metal_memory_release_retired, device,
                                  out_collected_count);
}

static VkrMetalSubmitRing *vkr_metal_memory_select_ring(
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
  if (!ring)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
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
  out_metrics->native_heap_size = device->heap.size;
  out_metrics->native_heap_used_size = device->heap.usedSize;
  out_metrics->native_heap_allocated_size = device->heap.currentAllocatedSize;
  device->native_heap_peak_allocated_size =
      Max(device->native_heap_peak_allocated_size,
          out_metrics->native_heap_allocated_size);
  out_metrics->native_heap_peak_allocated_size =
      device->native_heap_peak_allocated_size;
  out_metrics->native_heap_largest_free_range =
      [device->heap maxAvailableSizeWithAlignment:1];
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
  out_metrics->readback_ring_acquires = device->readback_ring.acquires;
  out_metrics->readback_ring_reuses = device->readback_ring.reuses;
  out_metrics->readback_ring_busy_failures =
      device->readback_ring.busy_failures;
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
    vkr_metal_memory_release(device->heap);
    vkr_metal_memory_release(device->device);
  }
  free(device->logical_lengths);
  free(device->native_resources);
  free(device->readback_ring_storage);
  free(device->upload_ring_storage);
  free(device->core_storage);
  free(device);
}

#endif
