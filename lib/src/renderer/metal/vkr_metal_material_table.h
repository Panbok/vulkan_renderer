#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"

typedef struct VkrMetalMaterialTableCore VkrMetalMaterialTableCore;
typedef struct VkrMetalMaterialTableDevice VkrMetalMaterialTableDevice;

typedef enum VkrMetalMaterialStatus {
  VKR_METAL_MATERIAL_STATUS_OK = 0,
  VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT,
  VKR_METAL_MATERIAL_STATUS_CAPACITY_EXHAUSTED,
  VKR_METAL_MATERIAL_STATUS_RETIREMENT_CAPACITY_EXHAUSTED,
  VKR_METAL_MATERIAL_STATUS_STALE_HANDLE,
  VKR_METAL_MATERIAL_STATUS_NATIVE_ALLOCATION_FAILED,
} VkrMetalMaterialStatus;

typedef struct VkrMetalMaterialHandle {
  uint32_t index;
  uint32_t generation;
} VkrMetalMaterialHandle;

// This is the host ABI of the Slang/Metal material row. Texture fields contain
// the 64-bit payload of MTLResourceID, not CPU handles or heap offsets.
typedef struct VKR_SIMD_ALIGN VkrMetalMaterialGpuRow {
  float32_t tint[4];
  uint64_t base_color_texture_id;
  uint64_t normal_texture_id;
  uint64_t orm_texture_id;
  uint64_t emissive_texture_id;
  uint64_t base_color_sampler_id;
  uint64_t normal_sampler_id;
  uint64_t orm_sampler_id;
  uint64_t emissive_sampler_id;
  uint32_t material_id;
  uint32_t flags;
  uint64_t reserved;
} VkrMetalMaterialGpuRow;

typedef struct VkrMetalMaterialTableConfig {
  uint32_t max_rows;
  uint32_t max_retirements;
} VkrMetalMaterialTableConfig;

typedef struct VkrMetalMaterialTableMetrics {
  uint64_t rows_live;
  uint64_t rows_retired;
  uint64_t rows_peak;
  uint64_t rows_published;
  uint64_t rows_replaced;
  uint64_t rows_collected;
  uint64_t capacity_failures;
  uint64_t retirement_capacity_failures;
  uint64_t stale_handle_failures;
} VkrMetalMaterialTableMetrics;

uint64_t vkr_metal_material_table_storage_requirement(
    const VkrMetalMaterialTableConfig *config);

VkrMetalMaterialStatus
vkr_metal_material_table_create(const VkrMetalMaterialTableConfig *config,
                                void *storage, uint64_t storage_size,
                                VkrMetalMaterialGpuRow *mapped_rows,
                                VkrMetalMaterialTableCore **out_table);

VkrMetalMaterialStatus
vkr_metal_material_table_publish(VkrMetalMaterialTableCore *table,
                                 const VkrMetalMaterialGpuRow *row,
                                 VkrMetalMaterialHandle *out_handle);

VkrMetalMaterialStatus vkr_metal_material_table_replace(
    VkrMetalMaterialTableCore *table, VkrMetalMaterialHandle old_handle,
    const VkrMetalMaterialGpuRow *new_row, uint64_t old_last_use_submit_value,
    VkrMetalMaterialHandle *out_new_handle);

VkrMetalMaterialStatus
vkr_metal_material_table_retire(VkrMetalMaterialTableCore *table,
                                VkrMetalMaterialHandle handle,
                                uint64_t last_use_submit_value);

VkrMetalMaterialStatus
vkr_metal_material_table_resolve(VkrMetalMaterialTableCore *table,
                                 VkrMetalMaterialHandle handle,
                                 uint32_t *out_row_index);

VkrMetalMaterialStatus
vkr_metal_material_table_collect(VkrMetalMaterialTableCore *table,
                                 uint64_t completed_submit_value,
                                 uint32_t *out_collected_count);

void vkr_metal_material_table_get_metrics(
    const VkrMetalMaterialTableCore *table,
    VkrMetalMaterialTableMetrics *out_metrics);

VkrMetalMaterialStatus vkr_metal_material_table_device_create(
    const VkrMetalMaterialTableConfig *config,
    // Borrowed id<MTLDevice>; the adapter retains it for its own lifetime.
    void *metal_device,
    // Owns the adapter's host allocations. Required.
    VkrAllocator *allocator, VkrMetalMaterialTableDevice **out_table);

VkrMetalMaterialStatus
vkr_metal_material_table_device_publish(VkrMetalMaterialTableDevice *table,
                                        const VkrMetalMaterialGpuRow *row,
                                        VkrMetalMaterialHandle *out_handle);

VkrMetalMaterialStatus vkr_metal_material_table_device_replace(
    VkrMetalMaterialTableDevice *table, VkrMetalMaterialHandle old_handle,
    const VkrMetalMaterialGpuRow *new_row, uint64_t old_last_use_submit_value,
    VkrMetalMaterialHandle *out_new_handle);

VkrMetalMaterialStatus
vkr_metal_material_table_device_retire(VkrMetalMaterialTableDevice *table,
                                       VkrMetalMaterialHandle handle,
                                       uint64_t last_use_submit_value);

VkrMetalMaterialStatus
vkr_metal_material_table_device_resolve(VkrMetalMaterialTableDevice *table,
                                        VkrMetalMaterialHandle handle,
                                        uint32_t *out_row_index);

VkrMetalMaterialStatus
vkr_metal_material_table_device_collect(VkrMetalMaterialTableDevice *table,
                                        uint64_t completed_submit_value,
                                        uint32_t *out_collected_count);

uint64_t
vkr_metal_material_table_device_gpu_address(VkrMetalMaterialTableDevice *table);

void *
vkr_metal_material_table_device_buffer(VkrMetalMaterialTableDevice *table);

void *vkr_metal_material_table_device_residency_set(
    VkrMetalMaterialTableDevice *table);

void vkr_metal_material_table_device_get_metrics(
    VkrMetalMaterialTableDevice *table,
    VkrMetalMaterialTableMetrics *out_metrics);

// The caller must prove all submissions that can read table rows are done.
void vkr_metal_material_table_device_destroy(
    VkrMetalMaterialTableDevice *table);
