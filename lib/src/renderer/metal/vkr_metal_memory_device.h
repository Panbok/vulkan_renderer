#pragma once

#include "memory/vkr_allocator.h"
#include "renderer/metal/vkr_metal_memory.h"

typedef struct VkrMetalMemoryDevice VkrMetalMemoryDevice;

typedef enum VkrMetalResourceKind {
  VKR_METAL_RESOURCE_KIND_BUFFER = 1,
  VKR_METAL_RESOURCE_KIND_TEXTURE = 2,
} VkrMetalResourceKind;

_Static_assert((uint32_t)VKR_METAL_RESOURCE_KIND_BUFFER ==
                   (uint32_t)VKR_METAL_MEMORY_CLASS_BUFFER,
               "Buffer resource and allocator class values must match");
_Static_assert((uint32_t)VKR_METAL_RESOURCE_KIND_TEXTURE ==
                   (uint32_t)VKR_METAL_MEMORY_CLASS_TEXTURE,
               "Texture resource and allocator class values must match");

typedef enum VkrMetalRingKind {
  VKR_METAL_RING_KIND_UPLOAD = 0,
  VKR_METAL_RING_KIND_READBACK,
} VkrMetalRingKind;

typedef struct VkrMetalMemoryDeviceConfig {
  // Borrowed id<MTLDevice>; the adapter retains it for its own lifetime.
  void *metal_device;
  // Owns every host-side allocation the adapter makes. Required: host bytes go
  // through the engine allocator so they enter tag and leak accounting.
  VkrAllocator *allocator;
  uint64_t heap_size;
  uint64_t upload_ring_size;
  uint64_t readback_ring_size;
  uint32_t ring_slot_count;
  uint32_t max_allocations;
  uint32_t max_retirements;
  uint32_t max_free_ranges;
} VkrMetalMemoryDeviceConfig;

typedef struct VkrMetalBufferResource {
  VkrMetalAllocationHandle handle;
  void *metal_buffer;
  uint64_t gpu_address;
  uint64_t length;
} VkrMetalBufferResource;

typedef struct VkrMetalTextureResource {
  VkrMetalAllocationHandle handle;
  void *metal_texture;
  uint64_t resource_id;
} VkrMetalTextureResource;

typedef struct VkrMetalMemoryDeviceMetrics {
  VkrMetalMemoryMetrics suballocations;
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];
  uint64_t native_heap_size;
  uint64_t native_heap_used_size;
  uint64_t native_heap_allocated_size;
  uint64_t native_heap_largest_free_range;
  uint64_t native_heap_peak_allocated_size;
  uint64_t driver_current_allocated_size;
  uint64_t driver_recommended_working_set_size;
  uint64_t pending_texture_upload_bytes;
  uint64_t residency_allocation_count;
  uint64_t native_live_resources;
  uint64_t native_resources_released;
  uint64_t upload_ring_acquires;
  uint64_t upload_ring_reuses;
  uint64_t upload_ring_busy_failures;
  uint64_t readback_ring_acquires;
  uint64_t readback_ring_reuses;
  uint64_t readback_ring_busy_failures;
} VkrMetalMemoryDeviceMetrics;

VkrMetalMemoryStatus
vkr_metal_memory_device_create(const VkrMetalMemoryDeviceConfig *config,
                               VkrMetalMemoryDevice **out_device);

VkrMetalMemoryStatus vkr_metal_memory_device_create_buffer(
    VkrMetalMemoryDevice *device, uint64_t length, VkrGpuAllocationOwner owner,
    VkrMetalBufferResource *out_buffer);

// metal_texture_descriptor is a borrowed MTLTextureDescriptor pointer.
VkrMetalMemoryStatus vkr_metal_memory_device_create_texture(
    VkrMetalMemoryDevice *device, void *metal_texture_descriptor,
    VkrGpuAllocationOwner owner, VkrMetalTextureResource *out_texture);

VkrMetalMemoryStatus
vkr_metal_memory_device_resolve_buffer(VkrMetalMemoryDevice *device,
                                       VkrMetalAllocationHandle handle,
                                       VkrMetalBufferResource *out_buffer);

VkrMetalMemoryStatus
vkr_metal_memory_device_resolve_texture(VkrMetalMemoryDevice *device,
                                        VkrMetalAllocationHandle handle,
                                        VkrMetalTextureResource *out_texture);

VkrMetalMemoryStatus
vkr_metal_memory_device_retire(VkrMetalMemoryDevice *device,
                               VkrMetalAllocationHandle handle,
                               uint64_t last_use_submit_value);

VkrMetalMemoryStatus
vkr_metal_memory_device_collect(VkrMetalMemoryDevice *device,
                                uint64_t completed_submit_value,
                                uint32_t *out_collected_count);

VkrMetalMemoryStatus vkr_metal_memory_device_acquire_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    uint64_t requested_size, uint64_t completed_submit_value,
    VkrMetalRingSlice *out_slice, VkrMetalAddressPair *out_addresses,
    void **out_metal_buffer);

VkrMetalMemoryStatus vkr_metal_memory_device_submit_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    VkrMetalRingSlice slice, uint64_t submit_value);

void vkr_metal_memory_device_cancel_ring(VkrMetalMemoryDevice *device,
                                         VkrMetalRingKind ring_kind,
                                         VkrMetalRingSlice slice);

void *vkr_metal_memory_device_residency_set(VkrMetalMemoryDevice *device);

void vkr_metal_memory_device_get_metrics(
    VkrMetalMemoryDevice *device, VkrMetalMemoryDeviceMetrics *out_metrics);

// The caller must prove all submissions that can reference the device are done.
void vkr_metal_memory_device_destroy(VkrMetalMemoryDevice *device);
