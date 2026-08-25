#include "renderer/vkr_gpu_abi.h"

#include "renderer/vkr_buffer.h"

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

void vkr_gpu_geometry_row_relocate(VkrGpuGeometryRow *row,
                                   uint64_t vertex_address,
                                   uint64_t index_address,
                                   uint32_t publication_generation) {
  assert_log(row != NULL, "Geometry row is NULL");
  assert_log(row->decode_address >= row->vertex_address,
             "Geometry decode address precedes the vertex megabuffer");
  const uint64_t decode_offset = row->decode_address - row->vertex_address;
  assert_log(vertex_address <= UINT64_MAX - decode_offset,
             "Relocated geometry decode address overflows");
  row->vertex_address = vertex_address;
  row->index_address = index_address;
  row->decode_address = vertex_address + decode_offset;
  row->publication_generation = publication_generation;
}

vkr_global const VkrGpuAbiField vkr_gpu_vertex_fields[] = {
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

vkr_global const VkrGpuAbiField vkr_gpu_packed_static_vertex_fields[] = {
    VKR_GPU_ABI_FIELD(VkrPackedStaticVertex, words, "words", 0),
};

vkr_global const VkrGpuAbiField vkr_gpu_geometry_decode_record_fields[] = {
    VKR_GPU_ABI_FIELD(VkrGpuGeometryDecodeRecord, position_bias,
                      "position_bias", 0),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryDecodeRecord, flags, "flags", 12),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryDecodeRecord, position_scale,
                      "position_scale", 16),
};

vkr_global const VkrGpuAbiField vkr_gpu_instance_fields[] = {
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, model, "model", 0),
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, object_id, "object_id", 64),
    VKR_GPU_ABI_FIELD(VkrInstanceDataGPU, reserved, "reserved_0", 68),
};

vkr_global const VkrGpuAbiField vkr_gpu_text_vertex_fields[] = {
    VKR_GPU_ABI_FIELD(VkrTextVertex, position, "position", 0),
    VKR_GPU_ABI_FIELD(VkrTextVertex, texcoord, "texcoord", 8),
    VKR_GPU_ABI_FIELD(VkrTextVertex, color, "color", 16),
};

vkr_global const VkrGpuAbiField vkr_gpu_geometry_row_fields[] = {
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, vertex_address, "vertex_address", 0),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, index_address, "index_address", 8),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, first_vertex, "first_vertex", 16),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, first_index, "first_index", 20),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, vertex_stride, "vertex_stride", 24),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, vertex_layout, "vertex_layout", 28),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, publication_generation,
                      "publication_generation", 32),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, flags, "flags", 36),
    VKR_GPU_ABI_FIELD(VkrGpuGeometryRow, decode_address, "decode_address", 40),
};

vkr_global const VkrGpuAbiField vkr_gpu_candidate_draw_row_fields[] = {
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, geometry_index, "geometry_index",
                      0),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, material_index, "material_index",
                      4),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, instance_index, "instance_index",
                      8),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, first_index, "first_index", 12),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, index_count, "index_count", 16),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, vertex_offset, "vertex_offset",
                      20),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, state_bucket, "state_bucket", 24),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, flags, "flags", 28),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, local_bounding_sphere,
                      "local_bounding_sphere", 32),
};

vkr_global const VkrGpuAbiField vkr_gpu_visible_draw_row_fields[] = {
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, geometry_index, "geometry_index",
                      0),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, material_index, "material_index",
                      4),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, instance_index, "instance_index",
                      8),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, first_index, "first_index", 12),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, index_count, "index_count", 16),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, vertex_offset, "vertex_offset", 20),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, state_bucket, "state_bucket", 24),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, flags, "flags", 28),
};

vkr_global const VkrGpuAbiRecord
    vkr_gpu_abi_records[VKR_GPU_ABI_RECORD_COUNT] = {
        [VKR_GPU_ABI_VERTEX] = VKR_GPU_ABI_RECORD(
            VkrVertex3d, "VkrMetalPacketVertex", 64, 16, vkr_gpu_vertex_fields),
        [VKR_GPU_ABI_PACKED_STATIC_VERTEX] =
            VKR_GPU_ABI_RECORD(VkrPackedStaticVertex, "VkrPackedStaticVertex",
                               32, 4, vkr_gpu_packed_static_vertex_fields),
        [VKR_GPU_ABI_GEOMETRY_DECODE_RECORD] = VKR_GPU_ABI_RECORD(
            VkrGpuGeometryDecodeRecord, "VkrGpuGeometryDecodeRecord", 32, 4,
            vkr_gpu_geometry_decode_record_fields),
        [VKR_GPU_ABI_INSTANCE] =
            VKR_GPU_ABI_RECORD(VkrInstanceDataGPU, "VkrMetalPacketInstance", 80,
                               16, vkr_gpu_instance_fields),
        [VKR_GPU_ABI_TEXT_VERTEX] =
            VKR_GPU_ABI_RECORD(VkrTextVertex, "VkrMetalPacketTextVertex", 32,
                               16, vkr_gpu_text_vertex_fields),
        [VKR_GPU_ABI_GEOMETRY_ROW] =
            VKR_GPU_ABI_RECORD(VkrGpuGeometryRow, "VkrGpuGeometryRow", 48, 8,
                               vkr_gpu_geometry_row_fields),
        [VKR_GPU_ABI_CANDIDATE_DRAW_ROW] =
            VKR_GPU_ABI_RECORD(VkrGpuCandidateDrawRow, "VkrGpuCandidateDrawRow",
                               48, 16, vkr_gpu_candidate_draw_row_fields),
        [VKR_GPU_ABI_VISIBLE_DRAW_ROW] =
            VKR_GPU_ABI_RECORD(VkrGpuVisibleDrawRow, "VkrGpuVisibleDrawRow", 32,
                               4, vkr_gpu_visible_draw_row_fields),
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
