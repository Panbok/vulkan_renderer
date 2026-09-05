#include "renderer/vkr_gpu_abi.h"

#include "renderer/vkr_buffer.h"

#include <stddef.h>
#include <math.h>

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

VkrPreparedInstanceGPU vkr_gpu_prepare_instance(const VkrInstanceDataGPU *source) {
  const Mat4 model = source->model;
  /* Double intermediates keep cofactors finite for finite binary32 scales.
     A common positive scale cancels when shaders normalize the result. */
  const float64_t a0 = model.m00, a1 = model.m10, a2 = model.m20;
  const float64_t b0 = model.m01, b1 = model.m11, b2 = model.m21;
  const float64_t c0 = model.m02, c1 = model.m12, c2 = model.m22;
  float64_t cofactor[9] = {
      b1 * c2 - b2 * c1, b2 * c0 - b0 * c2, b0 * c1 - b1 * c0,
      c1 * a2 - c2 * a1, c2 * a0 - c0 * a2, c0 * a1 - c1 * a0,
      a1 * b2 - a2 * b1, a2 * b0 - a0 * b2, a0 * b1 - a1 * b0,
  };
  const float64_t determinant =
      a0 * cofactor[0] + a1 * cofactor[1] + a2 * cofactor[2];
  const float32_t handedness = determinant < 0.0 ? -1.0f : 1.0f;
  float64_t magnitude = 0.0;
  for (uint32_t i = 0u; i < ArrayCount(cofactor); ++i)
    magnitude = Max(magnitude, fabs(cofactor[i]));
  const float64_t scale = magnitude > 0.0 ? handedness / magnitude : 0.0;
  for (uint32_t i = 0u; i < ArrayCount(cofactor); ++i)
    cofactor[i] *= scale;
  return (VkrPreparedInstanceGPU){
      .model = model,
      .object_id = source->object_id,
      .temporal_index = source->temporal_index,
      .temporal_generation = source->temporal_generation,
      .temporal_flags = source->temporal_flags,
      .normal_column0 = {(float32_t)cofactor[0], (float32_t)cofactor[1],
                         (float32_t)cofactor[2], handedness},
      .normal_column1 = {(float32_t)cofactor[3], (float32_t)cofactor[4],
                         (float32_t)cofactor[5], 0.0f},
      .normal_column2 = {(float32_t)cofactor[6], (float32_t)cofactor[7],
                         (float32_t)cofactor[8], 0.0f},
  };
}

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
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, model, "model", 0),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, object_id, "object_id", 64),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, temporal_index, "temporal_index", 68),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, temporal_generation,
                      "temporal_generation", 72),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, temporal_flags, "temporal_flags", 76),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, normal_column0, "normal_column0", 80),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, normal_column1, "normal_column1", 96),
    VKR_GPU_ABI_FIELD(VkrPreparedInstanceGPU, normal_column2, "normal_column2", 112),
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
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, decode_index, "decode_index", 24),
    VKR_GPU_ABI_FIELD(VkrGpuCandidateDrawRow, state_flags, "state_flags", 28),
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
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, decode_index, "decode_index", 24),
    VKR_GPU_ABI_FIELD(VkrGpuVisibleDrawRow, state_flags, "state_flags", 28),
};

vkr_global const VkrGpuAbiField vkr_gpu_point_light_row_fields[] = {
    VKR_GPU_ABI_FIELD(VkrGpuPointLightRow, p0, "p0", 0),
    VKR_GPU_ABI_FIELD(VkrGpuPointLightRow, p1, "p1", 16),
    VKR_GPU_ABI_FIELD(VkrGpuPointLightRow, p2, "p2", 32),
    VKR_GPU_ABI_FIELD(VkrGpuPointLightRow, p3, "p3", 48),
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
            VKR_GPU_ABI_RECORD(VkrPreparedInstanceGPU, "VkrMetalPacketInstance", 128,
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
        [VKR_GPU_ABI_POINT_LIGHT_ROW] =
            VKR_GPU_ABI_RECORD(VkrGpuPointLightRow, "VkrGpuPointLightRow", 64,
                               16, vkr_gpu_point_light_row_fields),
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
