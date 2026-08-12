#include "renderer/metal/vkr_metal_material_table.h"

#if defined(PLATFORM_APPLE)

#import <Metal/Metal.h>

struct VkrMetalMaterialTableDevice {
  id<MTLDevice> device;
  id<MTLBuffer> buffer;
  id<MTLResidencySet> residency;
  void *core_storage;
  /** Owns this struct and core_storage; retained for the sized frees. */
  VkrAllocator *allocator;
  uint64_t core_storage_size;
  VkrMetalMaterialTableCore *core;
};

vkr_internal void *vkr_metal_material_device_alloc(VkrAllocator *allocator,
                                                   uint64_t size) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory)
    MemZero(memory, size);
  return memory;
}

VkrMetalMaterialStatus vkr_metal_material_table_device_create(
    const VkrMetalMaterialTableConfig *config, void *metal_device,
    VkrAllocator *allocator, VkrMetalMaterialTableDevice **out_table) {
  if (!config || !metal_device || !allocator || !out_table ||
      config->max_rows == 0 || config->max_retirements == 0)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  *out_table = NULL;
  if (@available(macOS 26.0, *)) {
    VkrMetalMaterialTableDevice *table =
        vkr_metal_material_device_alloc(allocator, sizeof(*table));
    if (!table)
      return VKR_METAL_MATERIAL_STATUS_NATIVE_ALLOCATION_FAILED;
    table->allocator = allocator;
    table->device = [(id<MTLDevice>)metal_device retain];
    const uint64_t buffer_size =
        (uint64_t)config->max_rows * sizeof(VkrMetalMaterialGpuRow);
    const uint64_t storage_size =
        vkr_metal_material_table_storage_requirement(config);
    table->core_storage_size = storage_size;
    table->core_storage =
        vkr_metal_material_device_alloc(allocator, storage_size);
    table->buffer = [table->device
        newBufferWithLength:buffer_size
                    options:MTLResourceStorageModeShared |
                            MTLResourceCPUCacheModeWriteCombined];
    if (!table->device || ![table->device supportsFamily:MTLGPUFamilyMetal4] ||
        !table->core_storage || !table->buffer ||
        table->buffer.gpuAddress == 0 ||
        vkr_metal_material_table_create(
            config, table->core_storage, storage_size, table->buffer.contents,
            &table->core) != VKR_METAL_MATERIAL_STATUS_OK) {
      vkr_metal_material_table_device_destroy(table);
      return VKR_METAL_MATERIAL_STATUS_NATIVE_ALLOCATION_FAILED;
    }

    MTLResidencySetDescriptor *descriptor = [MTLResidencySetDescriptor new];
    descriptor.label = @"VKR Metal immutable material rows";
    descriptor.initialCapacity = 1;
    NSError *error = nil;
    table->residency = [table->device newResidencySetWithDescriptor:descriptor
                                                              error:&error];
    [descriptor release];
    if (!table->residency) {
      (void)error;
      vkr_metal_material_table_device_destroy(table);
      return VKR_METAL_MATERIAL_STATUS_NATIVE_ALLOCATION_FAILED;
    }
    [table->residency addAllocation:table->buffer];
    [table->residency commit];
    [table->residency requestResidency];
    *out_table = table;
    return VKR_METAL_MATERIAL_STATUS_OK;
  }
  return VKR_METAL_MATERIAL_STATUS_NATIVE_ALLOCATION_FAILED;
}

VkrMetalMaterialStatus
vkr_metal_material_table_device_publish(VkrMetalMaterialTableDevice *table,
                                        const VkrMetalMaterialGpuRow *row,
                                        VkrMetalMaterialHandle *out_handle) {
  return table ? vkr_metal_material_table_publish(table->core, row, out_handle)
               : VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
}

VkrMetalMaterialStatus vkr_metal_material_table_device_replace(
    VkrMetalMaterialTableDevice *table, VkrMetalMaterialHandle old_handle,
    const VkrMetalMaterialGpuRow *new_row, uint64_t old_last_use_submit_value,
    VkrMetalMaterialHandle *out_new_handle) {
  return table ? vkr_metal_material_table_replace(
                     table->core, old_handle, new_row,
                     old_last_use_submit_value, out_new_handle)
               : VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
}

VkrMetalMaterialStatus
vkr_metal_material_table_device_retire(VkrMetalMaterialTableDevice *table,
                                       VkrMetalMaterialHandle handle,
                                       uint64_t last_use_submit_value) {
  return table ? vkr_metal_material_table_retire(table->core, handle,
                                                 last_use_submit_value)
               : VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
}

VkrMetalMaterialStatus
vkr_metal_material_table_device_resolve(VkrMetalMaterialTableDevice *table,
                                        VkrMetalMaterialHandle handle,
                                        uint32_t *out_row_index) {
  return table ? vkr_metal_material_table_resolve(table->core, handle,
                                                  out_row_index)
               : VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
}

VkrMetalMaterialStatus
vkr_metal_material_table_device_collect(VkrMetalMaterialTableDevice *table,
                                        uint64_t completed_submit_value,
                                        uint32_t *out_collected_count) {
  return table ? vkr_metal_material_table_collect(
                     table->core, completed_submit_value, out_collected_count)
               : VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
}

uint64_t vkr_metal_material_table_device_gpu_address(
    VkrMetalMaterialTableDevice *table) {
  return table ? table->buffer.gpuAddress : 0;
}

void *
vkr_metal_material_table_device_buffer(VkrMetalMaterialTableDevice *table) {
  return table ? table->buffer : nil;
}

void *vkr_metal_material_table_device_residency_set(
    VkrMetalMaterialTableDevice *table) {
  return table ? table->residency : nil;
}

void vkr_metal_material_table_device_get_metrics(
    VkrMetalMaterialTableDevice *table,
    VkrMetalMaterialTableMetrics *out_metrics) {
  vkr_metal_material_table_get_metrics(table ? table->core : NULL, out_metrics);
}

void vkr_metal_material_table_device_destroy(
    VkrMetalMaterialTableDevice *table) {
  if (!table)
    return;
  if (@available(macOS 26.0, *)) {
    [table->residency endResidency];
    [table->residency release];
    [table->buffer release];
    [table->device release];
  }
  VkrAllocator *allocator = table->allocator;
  if (allocator) {
    if (table->core_storage)
      vkr_allocator_free(allocator, table->core_storage,
                         table->core_storage_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    vkr_allocator_free(allocator, table, sizeof(*table),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
}

#endif
