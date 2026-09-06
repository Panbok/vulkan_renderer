#pragma once

#include "memory/vkr_allocator.h"
#include "renderer/metal/vkr_metal_memory.h"

typedef struct VkrMetalMemoryDevice VkrMetalMemoryDevice;
typedef struct VkrMetalDiagnostics VkrMetalDiagnostics;

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
  // Optional borrowed sink; its renderer-owned state outlives this adapter.
  VkrMetalDiagnostics *diagnostics;
  // Includes reserved heap capacity, transfer rings and external reservations.
  uint64_t managed_budget_size;
  uint64_t heap_chunk_size;
  uint64_t upload_ring_size;
  uint64_t upload_ring_max_size;
  uint64_t readback_ring_size;
  uint64_t readback_ring_max_size;
  uint32_t ring_slot_count;
  uint32_t max_allocations;
  // At least max_allocations; failed native creation must always retire.
  uint32_t max_retirements;
  // At least max_allocations + 1, including every possible logical free gap.
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
  uint64_t managed_budget_size;
  uint64_t managed_allocated_size;
  uint64_t texture_heap_capacity_bytes;
  uint64_t managed_peak_allocated_size;
  uint64_t external_allocation_count;
  uint64_t external_allocated_size;
  uint64_t transfer_ring_allocated_size;
  uint64_t residency_allocated_size;
  uint64_t native_heap_count;
  uint64_t native_heap_peak_count;
  uint64_t native_heap_total_count;
  uint64_t max_native_heaps;
  uint64_t native_heap_size;
  uint64_t native_heap_used_size;
  uint64_t native_heap_allocated_size;
  uint64_t native_heap_largest_free_range;
  uint64_t native_heap_peak_allocated_size;
  uint64_t driver_current_allocated_size;
  uint64_t driver_recommended_working_set_size;
  uint64_t residency_allocation_count;
  uint64_t native_live_resources;
  uint64_t native_resources_released;
  uint64_t upload_ring_acquires;
  uint64_t upload_ring_reuses;
  uint64_t upload_ring_busy_failures;
  uint64_t upload_ring_total_capacity_bytes;
  uint64_t upload_ring_slot_capacity_bytes;
  uint64_t upload_ring_max_requested_bytes;
  uint64_t upload_ring_oversize_failures;
  uint64_t readback_ring_acquires;
  uint64_t readback_ring_reuses;
  uint64_t readback_ring_busy_failures;
  uint64_t readback_ring_total_capacity_bytes;
  uint64_t readback_ring_slot_capacity_bytes;
  uint64_t readback_ring_max_requested_bytes;
  uint64_t readback_ring_oversize_failures;
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
VkrMetalMemoryStatus vkr_metal_memory_device_grow_ring(
    VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind,
    uint64_t required_slot_size, uint64_t completed_submit_value);
uint64_t vkr_metal_memory_device_ring_slot_capacity(
    const VkrMetalMemoryDevice *device, VkrMetalRingKind ring_kind);

// External native resources remain caller-owned. Reserve before creation, then
// reconcile with actual allocatedSize. A failed reconcile preserves the
// original reservation; release the native resource before releasing that
// reservation. Release accounted bytes only after the caller proves the
// resource's GPU last use.
bool8_t vkr_metal_memory_device_reserve_external(VkrMetalMemoryDevice *device,
                                                 uint64_t bytes);
bool8_t vkr_metal_memory_device_reconcile_external(VkrMetalMemoryDevice *device,
                                                   uint64_t reserved_bytes,
                                                   uint64_t allocated_bytes);
void vkr_metal_memory_device_release_external(VkrMetalMemoryDevice *device,
                                              uint64_t bytes);

void *vkr_metal_memory_device_residency_set(VkrMetalMemoryDevice *device);

void vkr_metal_memory_device_get_metrics(
    VkrMetalMemoryDevice *device, VkrMetalMemoryDeviceMetrics *out_metrics);

// The caller must prove all submissions that can reference the device are done.
void vkr_metal_memory_device_destroy(VkrMetalMemoryDevice *device);
