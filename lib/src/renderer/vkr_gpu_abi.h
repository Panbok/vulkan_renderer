#pragma once

#include "defines.h"
#include "math/mat.h"

/** Maximum packet instance records accepted per frame. */
#define VKR_INSTANCE_BUFFER_MAX_INSTANCES 65536

/** GPU-visible instance record shared by the selected implementations. */
typedef struct VkrInstanceDataGPU {
  Mat4 model;
  uint32_t object_id;
  uint32_t reserved[3];
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 80,
               "VkrInstanceDataGPU must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0,
               "VkrInstanceDataGPU must be 16-byte aligned");

typedef enum VkrGpuAbiRecordId {
  VKR_GPU_ABI_VERTEX = 0,
  VKR_GPU_ABI_INSTANCE,
  VKR_GPU_ABI_TEXT_VERTEX,
  VKR_GPU_ABI_RECORD_COUNT,
} VkrGpuAbiRecordId;

typedef struct VkrGpuAbiField {
  const char *host_name;
  const char *shader_name;
  uint32_t expected_offset;
  uint32_t host_offset;
} VkrGpuAbiField;

typedef struct VkrGpuAbiRecord {
  const char *host_name;
  const char *shader_name;
  uint32_t expected_size;
  uint32_t expected_alignment;
  uint32_t host_size;
  uint32_t host_alignment;
  const VkrGpuAbiField *fields;
  uint32_t field_count;
} VkrGpuAbiRecord;

const VkrGpuAbiRecord *vkr_gpu_abi_record(VkrGpuAbiRecordId id);
bool8_t vkr_gpu_abi_validate_host(void);
