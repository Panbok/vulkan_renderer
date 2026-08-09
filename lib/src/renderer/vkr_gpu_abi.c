#include "renderer/vkr_gpu_abi.h"

#include "renderer/vkr_buffer.h"
#include "renderer/vkr_instance_buffer.h"

#include <stddef.h>

#define VKR_GPU_ABI_FIELD(TYPE, HOST, SHADER, OFFSET)                          \
  {#HOST, SHADER, OFFSET, (uint32_t)offsetof(TYPE, HOST)}
#define VKR_GPU_ABI_RECORD(TYPE, SHADER, SIZE, ALIGNMENT, FIELDS)              \
  {#TYPE,                                                                      \
   SHADER,                                                                     \
   SIZE,                                                                       \
   ALIGNMENT,                                                                  \
   (uint32_t)sizeof(TYPE),                                                     \
   (uint32_t)_Alignof(TYPE),                                                   \
   FIELDS,                                                                     \
   ArrayCount(FIELDS)}

static const VkrGpuAbiField vkr_gpu_vertex_fields[] = {
    VKR_GPU_ABI_FIELD(VkrVertex3d, position.x, "position_x", 0),
    VKR_GPU_ABI_FIELD(VkrVertex3d, position.y, "position_y", 4),
    VKR_GPU_ABI_FIELD(VkrVertex3d, position.z, "position_z", 8),
    VKR_GPU_ABI_FIELD(VkrVertex3d, normal.x, "normal_x", 12),
    VKR_GPU_ABI_FIELD(VkrVertex3d, normal.y, "normal_y", 16),
    VKR_GPU_ABI_FIELD(VkrVertex3d, normal.z, "normal_z", 20),
    VKR_GPU_ABI_FIELD(VkrVertex3d, texcoord, "texcoord", 24),
    VKR_GPU_ABI_FIELD(VkrVertex3d, colour, "color", 32),
    VKR_GPU_ABI_FIELD(VkrVertex3d, tangent, "tangent", 48),
};

static const VkrGpuAbiField vkr_gpu_instance_fields[] = {
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, model, "model", 0),
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, object_id, "object_id", 64),
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, reserved, "reserved_0", 68),
};

static const VkrGpuAbiField vkr_gpu_text_vertex_fields[] = {
    VKR_GPU_ABI_FIELD(VkrTextVertex, position, "position", 0),
    VKR_GPU_ABI_FIELD(VkrTextVertex, texcoord, "texcoord", 8),
    VKR_GPU_ABI_FIELD(VkrTextVertex, color, "color", 16),
};

static const VkrGpuAbiRecord vkr_gpu_abi_records[VKR_GPU_ABI_RECORD_COUNT] = {
    [VKR_GPU_ABI_VERTEX] = VKR_GPU_ABI_RECORD(
        VkrVertex3d, "VkrMetalPacketVertex", 64, 16, vkr_gpu_vertex_fields),
    [VKR_GPU_ABI_INSTANCE] =
        VKR_GPU_ABI_RECORD(VkrInstanceDataGPU, "VkrMetalPacketInstance", 80, 16,
                           vkr_gpu_instance_fields),
    [VKR_GPU_ABI_TEXT_VERTEX] =
        VKR_GPU_ABI_RECORD(VkrTextVertex, "VkrMetalPacketTextVertex", 32, 16,
                           vkr_gpu_text_vertex_fields),
};

const VkrGpuAbiRecord *vkr_gpu_abi_record(VkrGpuAbiRecordId id) {
  return id < VKR_GPU_ABI_RECORD_COUNT ? &vkr_gpu_abi_records[id] : NULL;
}

bool8_t vkr_gpu_abi_validate_host(void) {
  for (uint32_t record_index = 0; record_index < VKR_GPU_ABI_RECORD_COUNT;
       ++record_index) {
    const VkrGpuAbiRecord *record = &vkr_gpu_abi_records[record_index];
    if (record->host_size != record->expected_size ||
        record->host_alignment != record->expected_alignment)
      return false_v;
    for (uint32_t field_index = 0; field_index < record->field_count;
         ++field_index) {
      if (record->fields[field_index].host_offset !=
          record->fields[field_index].expected_offset)
        return false_v;
    }
  }
  return true_v;
}

#undef VKR_GPU_ABI_RECORD
#undef VKR_GPU_ABI_FIELD
