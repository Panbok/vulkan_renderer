#include "assets/vkr_mesh_encode.h"

#include "assets/vkr_mesh_decode.h"
#include "assets/vkr_meshoptimizer_encode.h"
#include "core/logger.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <math.h>

typedef struct VkrMeshCookedDependencyBuild {
  String8 path;
  String8 physical_path;
  uint64_t byte_size;
  uint8_t hash[32];
  uint64_t string_offset;
} VkrMeshCookedDependencyBuild;

typedef struct VkrMeshCookedSkinVertex {
  VkrVertex3d vertex;
  VkrMeshSkinVertex skin;
} VkrMeshCookedSkinVertex;

typedef struct VkrMeshCookedRangeBuild {
  VkrMeshSkinVertex *skin_vertices;
  const VkrGeometryUploadRange *source;
  uint8_t *encoded_vertices;
  uint64_t encoded_vertex_size;
  uint8_t *encoded_indices;
  uint64_t encoded_index_size;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t first_index;
  uint64_t material_offset;
  uint64_t shader_offset;
  uint64_t vertex_stream_offset;
  uint64_t index_stream_offset;
  uint32_t vertex_crc;
  uint32_t index_crc;
  VkrGeometryQuantizationMetrics quantization;
  VkrGpuGeometryDecodeRecord decode;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
} VkrMeshCookedRangeBuild;

static bool8_t vkr_mesh_cooked_path_is_absolute(String8 path) {
  return (path.length > 0 && (path.str[0] == '/' || path.str[0] == '\\')) ||
         (path.length > 1 && path.str[1] == ':');
}

static bool8_t vkr_mesh_cooked_read_file(VkrAllocator *allocator, String8 path,
                                         uint8_t **out_data,
                                         uint64_t *out_size) {
  String8 owned_path = string8_duplicate(allocator, &path);
  if (!owned_path.str) {
    return false_v;
  }
  FilePath file_path = file_path_create(string8_cstr(&owned_path), allocator,
                                        vkr_mesh_cooked_path_is_absolute(path)
                                            ? FILE_PATH_TYPE_ABSOLUTE
                                            : FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  FileError error = file_read_all(&file, allocator, out_data, out_size);
  file_close(&file);
  return error == FILE_ERROR_NONE;
}

static bool8_t vkr_mesh_cooked_hash_dependencies(
    VkrAllocator *scratch_allocator, VkrMeshCookedDependencyBuild *dependencies,
    uint32_t dependency_count, uint8_t out_hash[32]) {
  VkrSha256 aggregate;
  vkr_sha256_init(&aggregate);
  for (uint32_t i = 0; i < dependency_count; ++i) {
    VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      return false_v;
    }
    uint8_t *bytes = NULL;
    uint64_t size = 0;
    bool8_t ok = vkr_mesh_cooked_read_file(
        scratch_allocator, dependencies[i].physical_path, &bytes, &size);
    if (ok) {
      vkr_sha256(bytes, size, dependencies[i].hash);
      dependencies[i].byte_size = size;

      uint8_t path_length_le[8];
      uint8_t byte_size_le[8];
      vkr_store_le_u64(path_length_le, dependencies[i].path.length);
      vkr_store_le_u64(byte_size_le, size);
      vkr_sha256_update(&aggregate, path_length_le, sizeof(path_length_le));
      vkr_sha256_update(&aggregate, dependencies[i].path.str,
                        dependencies[i].path.length);
      vkr_sha256_update(&aggregate, byte_size_le, sizeof(byte_size_le));
      vkr_sha256_update(&aggregate, bytes, size);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    if (!ok) {
      log_error("MeshCooked: failed to read dependency '%.*s'",
                (int32_t)dependencies[i].path.length, dependencies[i].path.str);
      return false_v;
    }
  }
  vkr_sha256_final(&aggregate, out_hash);
  return true_v;
}

static bool8_t vkr_mesh_cooked_string_is_valid(String8 value,
                                               bool8_t allow_empty) {
  if (value.length == 0) {
    return allow_empty;
  }
  if (!value.str || value.length > VKR_MESH_COOKED_MAX_STRING_LENGTH) {
    return false_v;
  }
  for (uint64_t i = 0; i < value.length; ++i) {
    if (value.str[i] == '\0') {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_mesh_cooked_vertex_is_finite(const VkrVertex3d *vertex) {
  return isfinite(vertex->position.x) && isfinite(vertex->position.y) &&
         isfinite(vertex->position.z) && isfinite(vertex->normal.x) &&
         isfinite(vertex->normal.y) && isfinite(vertex->normal.z) &&
         isfinite(vertex->texcoord.x) && isfinite(vertex->texcoord.y) &&
         isfinite(vertex->colour.x) && isfinite(vertex->colour.y) &&
         isfinite(vertex->colour.z) && isfinite(vertex->colour.w) &&
         isfinite(vertex->tangent.x) && isfinite(vertex->tangent.y) &&
         isfinite(vertex->tangent.z) && isfinite(vertex->tangent.w);
}

static bool8_t vkr_mesh_cooked_compute_range_bounds(const VkrVertex3d *vertices,
                                                    uint32_t count,
                                                    Vec3 *out_center,
                                                    Vec3 *out_min,
                                                    Vec3 *out_max) {
  if (!vertices || count == 0 || !out_center || !out_min || !out_max) {
    return false_v;
  }
  Vec3 min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_mesh_cooked_vertex_is_finite(&vertices[i])) {
      return false_v;
    }
    const Vec3 position = vkr_vertex_unpack_vec3(vertices[i].position);
    min.x = Min(min.x, position.x);
    min.y = Min(min.y, position.y);
    min.z = Min(min.z, position.z);
    max.x = Max(max.x, position.x);
    max.y = Max(max.y, position.y);
    max.z = Max(max.z, position.z);
  }
  *out_min = min;
  *out_max = max;
  *out_center = vec3_scale(vec3_add(min, max), 0.5f);
  return true_v;
}

static bool8_t
vkr_mesh_cooked_write_source_metadata(VkrByteWriter *writer,
                                      const VkrMeshSource *source) {
  bool8_t ok = true_v;
  ok = ok && vkr_byte_writer_u64(writer, source->fingerprint);
  ok = ok && vkr_byte_writer_u32(writer, source->animation_count);
  ok = ok && vkr_byte_writer_u32(writer, 0u);
  for (uint64_t i = 0; ok && i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    ok = vkr_byte_writer_u32(writer, node->parent) &&
         vkr_byte_writer_u32(writer, node->mesh) &&
         vkr_byte_writer_u32(writer, node->mesh_variant) &&
         vkr_byte_writer_u32(writer, node->camera) &&
         vkr_byte_writer_u32(writer, node->skin) &&
         vkr_byte_writer_u32(writer, node->light) &&
         vkr_byte_writer_u32(writer, node->in_scene);
    for (uint32_t f = 0; f < 16u; ++f) {
      ok = ok && vkr_byte_writer_f32(writer, node->local.elements[f]);
    }
    ok = ok && vkr_byte_writer_f32(writer, node->punctual.color.x) &&
         vkr_byte_writer_f32(writer, node->punctual.color.y) &&
         vkr_byte_writer_f32(writer, node->punctual.color.z) &&
         vkr_byte_writer_f32(writer, node->punctual.intensity) &&
         vkr_byte_writer_f32(writer, node->punctual.range) &&
         vkr_byte_writer_f32(writer, node->punctual.inner_cone) &&
         vkr_byte_writer_f32(writer, node->punctual.outer_cone) &&
         vkr_byte_writer_u32(writer, node->punctual.kind);
    ok = ok && vkr_byte_writer_u32(writer, (uint32_t)node->name.length) &&
         vkr_byte_writer_bytes(writer, node->name.str, node->name.length);
  }
  for (uint64_t i = 0; ok && i < source->meshes.length; ++i) {
    ok =
        vkr_byte_writer_u32(writer, source->meshes.data[i].source_mesh_index) &&
        vkr_byte_writer_u32(writer, source->meshes.data[i].first_range) &&
        vkr_byte_writer_u32(writer, source->meshes.data[i].range_count);
  }
  return ok;
}

bool8_t vkr_mesh_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrMeshCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size) {
  if (!scratch_allocator || !info || !out_data || !out_size ||
      !vkr_mesh_cooked_string_is_valid(info->source_path, false_v) ||
      !info->dependency_paths || info->dependency_count == 0 ||
      info->dependency_count > VKR_MESH_COOKED_MAX_DEPENDENCIES ||
      info->range_count > VKR_MESH_COOKED_MAX_RANGES ||
      (info->range_count &&
       (!info->ranges || info->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
        info->mesh_buffer.index_size != sizeof(uint32_t) ||
        !info->mesh_buffer.vertex_count || !info->mesh_buffer.index_count ||
        !info->mesh_buffer.vertices || !info->mesh_buffer.indices)) ||
      (!info->range_count &&
       (!info->source.nodes.length || info->mesh_buffer.vertex_count ||
        info->mesh_buffer.index_count)) ||
      !isfinite(info->budgets.position_relative) ||
      !isfinite(info->budgets.normal_degrees) ||
      !isfinite(info->budgets.tangent_degrees) ||
      !isfinite(info->budgets.uv_absolute) ||
      !isfinite(info->budgets.color_absolute) ||
      info->budgets.position_relative <= 0.0f ||
      info->budgets.normal_degrees <= 0.0f ||
      info->budgets.tangent_degrees <= 0.0f ||
      info->budgets.uv_absolute <= 0.0f ||
      info->budgets.color_absolute <= 0.0f) {
    return false_v;
  }

  *out_data = NULL;
  *out_size = 0;
  if (!vkr_mesh_cooked_source_validate(scratch_allocator, &info->source,
                                       info->range_count))
    return false_v;
  VkrMeshCookedDependencyBuild *dependencies = vkr_allocator_alloc(
      scratch_allocator,
      (uint64_t)info->dependency_count * sizeof(*dependencies),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshCookedRangeBuild *ranges =
      info->range_count
          ? vkr_allocator_alloc(scratch_allocator,
                                (uint64_t)info->range_count * sizeof(*ranges),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (!dependencies || (info->range_count && !ranges)) {
    return false_v;
  }
  MemZero(dependencies,
          (uint64_t)info->dependency_count * sizeof(*dependencies));
  if (info->range_count)
    MemZero(ranges, (uint64_t)info->range_count * sizeof(*ranges));

  uint64_t string_size = info->source_path.length;
  for (uint32_t i = 0; i < info->dependency_count; ++i) {
    if (!vkr_mesh_cooked_string_is_valid(info->dependency_paths[i], false_v)) {
      return false_v;
    }
    dependencies[i].physical_path = info->dependency_paths[i];
    dependencies[i].path = info->dependency_references
                               ? info->dependency_references[i]
                               : info->dependency_paths[i];
    if (!vkr_mesh_cooked_string_is_valid(dependencies[i].path, false_v)) {
      return false_v;
    }
    dependencies[i].string_offset = string_size;
    if (!vkr_checked_add_u64(string_size, dependencies[i].path.length,
                             &string_size)) {
      return false_v;
    }
  }

  const VkrVertex3d *source_vertices =
      (const VkrVertex3d *)info->mesh_buffer.vertices;
  const uint32_t *source_indices = (const uint32_t *)info->mesh_buffer.indices;
  for (uint32_t i = 0; i < info->mesh_buffer.vertex_count; ++i) {
    if (!vkr_mesh_cooked_vertex_is_finite(&source_vertices[i])) {
      return false_v;
    }
  }
  const bool8_t has_skin = info->skin.skin_count != 0u;
  if (has_skin) {
    /* Validate indexed access before the binding validator consumes ranges. */
    for (uint32_t i = 0; i < info->range_count; ++i) {
      const VkrGeometryUploadRange *range = &info->ranges[i];
      if (range->first_index > info->mesh_buffer.index_count ||
          range->index_count >
              info->mesh_buffer.index_count - range->first_index ||
          range->vertex_offset != 0) {
        return false_v;
      }
      for (uint32_t j = 0; j < range->index_count; ++j) {
        if (source_indices[range->first_index + j] >=
            info->mesh_buffer.vertex_count) {
          return false_v;
        }
      }
    }
    if (!vkr_mesh_skin_validate(&info->skin, &info->source, info->ranges,
                                info->range_count, source_indices,
                                info->mesh_buffer.index_count,
                                info->mesh_buffer.vertex_count)) {
      return false_v;
    }
  } else if (info->skin.vertex_count || info->skin.vertices ||
             info->skin.joint_counts || info->skin.animation_fingerprint) {
    return false_v;
  }
  VkrGpuGeometryDecodeRecord geometry_decode = {0};
  VkrGeometryQuantizationMetrics quantization_max = {0};
  uint64_t total_vertices = 0;
  uint64_t total_indices = 0;
  for (uint32_t i = 0; i < info->range_count; ++i) {
    const VkrGeometryUploadRange *range = &info->ranges[i];
    if (range->range_id != i || range->index_count == 0 ||
        range->index_count % 3u != 0 ||
        range->first_index > info->mesh_buffer.index_count ||
        range->index_count >
            info->mesh_buffer.index_count - range->first_index ||
        range->pipeline_domain >= VKR_PIPELINE_DOMAIN_COUNT ||
        !vkr_mesh_cooked_string_is_valid(range->material_name, true_v) ||
        !vkr_mesh_cooked_string_is_valid(range->shader_override, true_v)) {
      return false_v;
    }

    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      uint32_t index = source_indices[range->first_index + j];
      if (index >= info->mesh_buffer.vertex_count) {
        return false_v;
      }
      min_vertex = index < min_vertex ? index : min_vertex;
      max_vertex = index > max_vertex ? index : max_vertex;
    }
    uint64_t span64 = (uint64_t)max_vertex - min_vertex + 1u;
    if (span64 > UINT32_MAX) {
      return false_v;
    }
    uint32_t span = (uint32_t)span64;
    uint32_t *source_local_indices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)range->index_count * sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *optimized_indices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)range->index_count * sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrVertex3d *optimized_vertices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)span * sizeof(VkrVertex3d),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!source_local_indices || !optimized_indices || !optimized_vertices) {
      return false_v;
    }
    for (uint32_t j = 0; j < range->index_count; ++j) {
      source_local_indices[j] =
          source_indices[range->first_index + j] - min_vertex;
    }

    VkrMeshSkinVertex *optimized_skin = NULL;
    size_t optimized_vertex_count = 0;
    if (has_skin) {
      VkrMeshCookedSkinVertex *combined = vkr_allocator_alloc(
          scratch_allocator, (uint64_t)span * sizeof(*combined),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      VkrMeshCookedSkinVertex *remapped = vkr_allocator_alloc(
          scratch_allocator, (uint64_t)span * sizeof(*remapped),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      optimized_skin = vkr_allocator_alloc(
          scratch_allocator, (uint64_t)span * sizeof(*optimized_skin),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!combined || !remapped || !optimized_skin) {
        return false_v;
      }
      MemZero(combined, (uint64_t)span * sizeof(*combined));
      for (uint32_t j = 0; j < span; ++j) {
        combined[j].vertex = source_vertices[min_vertex + j];
        combined[j].skin = info->skin.vertices[min_vertex + j];
      }
      optimized_vertex_count = vkr_meshopt_optimize_range(
          remapped, optimized_indices, combined, source_local_indices, span,
          range->index_count, sizeof(*combined));
      for (size_t j = 0; j < optimized_vertex_count; ++j) {
        optimized_vertices[j] = remapped[j].vertex;
        optimized_skin[j] = remapped[j].skin;
      }
    } else {
      optimized_vertex_count = vkr_meshopt_optimize_range(
          optimized_vertices, optimized_indices, source_vertices + min_vertex,
          source_local_indices, span, range->index_count, sizeof(VkrVertex3d));
    }
    if (optimized_vertex_count == 0 || optimized_vertex_count > UINT32_MAX) {
      log_error("MeshCooked: range %u meshoptimizer locality pass failed", i);
      return false_v;
    }
    Vec3 optimized_center = vec3_zero();
    Vec3 optimized_min = vec3_zero();
    Vec3 optimized_max = vec3_zero();
    if (!vkr_mesh_cooked_compute_range_bounds(
            optimized_vertices, (uint32_t)optimized_vertex_count,
            &optimized_center, &optimized_min, &optimized_max)) {
      log_error("MeshCooked: range %u has invalid optimized bounds", i);
      return false_v;
    }

    VkrPackedStaticVertex *packed_vertices = vkr_allocator_alloc(
        scratch_allocator,
        optimized_vertex_count * sizeof(VkrPackedStaticVertex),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrGpuGeometryDecodeRecord range_decode = {0};
    VkrGeometryQuantizationMetrics range_quantization = {0};
    if (!packed_vertices) {
      return false_v;
    }
    if (!vkr_packed_geometry_pack(
            optimized_vertices, (uint32_t)optimized_vertex_count, optimized_min,
            optimized_max, &info->budgets, packed_vertices, &range_decode,
            &range_quantization)) {
      log_error("MeshCooked: range %u exceeds quantization budgets "
                "(position=%g normal=%g tangent=%g uv=%g color=%g)",
                i, range_quantization.position_max,
                range_quantization.normal_degrees_max,
                range_quantization.tangent_degrees_max,
                range_quantization.uv_max, range_quantization.color_max);
      return false_v;
    }
    if (i == 0)
      geometry_decode = range_decode;
    quantization_max.position_max =
        Max(quantization_max.position_max, range_quantization.position_max);
    quantization_max.normal_degrees_max =
        Max(quantization_max.normal_degrees_max,
            range_quantization.normal_degrees_max);
    quantization_max.tangent_degrees_max =
        Max(quantization_max.tangent_degrees_max,
            range_quantization.tangent_degrees_max);
    quantization_max.uv_max =
        Max(quantization_max.uv_max, range_quantization.uv_max);
    quantization_max.color_max =
        Max(quantization_max.color_max, range_quantization.color_max);

    size_t vertex_bound = vkr_meshopt_vertex_encode_bound(
        optimized_vertex_count, sizeof(VkrPackedStaticVertex));
    size_t index_bound = vkr_meshopt_index_encode_bound(range->index_count,
                                                        optimized_vertex_count);
    uint8_t *encoded_vertices = vkr_allocator_alloc(
        scratch_allocator, vertex_bound, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    uint8_t *encoded_indices = vkr_allocator_alloc(
        scratch_allocator, index_bound, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    if (!encoded_vertices || !encoded_indices) {
      return false_v;
    }
    size_t vertex_encoded_size = vkr_meshopt_encode_vertices(
        encoded_vertices, vertex_bound, packed_vertices, optimized_vertex_count,
        sizeof(VkrPackedStaticVertex));
    size_t index_encoded_size = vkr_meshopt_encode_indices(
        encoded_indices, index_bound, optimized_indices, range->index_count,
        optimized_vertex_count);
    if (vertex_encoded_size == 0 || index_encoded_size == 0 ||
        vkr_meshopt_vertex_codec_version(encoded_vertices,
                                         vertex_encoded_size) !=
            (int)VKR_MESHOPT_VERTEX_CODEC_VERSION ||
        vkr_meshopt_index_codec_version(encoded_indices, index_encoded_size) !=
            (int)VKR_MESHOPT_INDEX_CODEC_VERSION) {
      return false_v;
    }

    ranges[i] = (VkrMeshCookedRangeBuild){
        .source = range,
        .encoded_vertices = encoded_vertices,
        .skin_vertices = optimized_skin,
        .encoded_vertex_size = vertex_encoded_size,
        .encoded_indices = encoded_indices,
        .encoded_index_size = index_encoded_size,
        .vertex_count = (uint32_t)optimized_vertex_count,
        .index_count = range->index_count,
        .first_index = (uint32_t)total_indices,
        .material_offset = string_size,
        .vertex_crc = vkr_crc32(encoded_vertices, vertex_encoded_size),
        .index_crc = vkr_crc32(encoded_indices, index_encoded_size),
        .quantization = range_quantization,
        .decode = range_decode,
        .center = optimized_center,
        .min_extents = optimized_min,
        .max_extents = optimized_max,
    };
    if (!vkr_checked_add_u64(string_size, range->material_name.length,
                             &string_size)) {
      return false_v;
    }
    ranges[i].shader_offset = string_size;
    if (!vkr_checked_add_u64(string_size, range->shader_override.length,
                             &string_size)) {
      return false_v;
    }
    total_vertices += optimized_vertex_count;
    total_indices += range->index_count;
    if (total_vertices > UINT32_MAX || total_indices > UINT32_MAX) {
      return false_v;
    }
  }

  uint8_t source_hash[32];
  if (!vkr_mesh_cooked_hash_dependencies(scratch_allocator, dependencies,
                                         info->dependency_count, source_hash)) {
    return false_v;
  }
  uint8_t settings_hash[32];
  vkr_mesh_cooked_hash_settings(&info->budgets, settings_hash);

  uint64_t range_bytes = 0;
  uint64_t dependency_bytes = 0;
  if (!vkr_checked_mul_u64(info->range_count, VKR_MESH_COOKED_RANGE_SIZE,
                           &range_bytes) ||
      !vkr_checked_mul_u64(info->dependency_count,
                           VKR_MESH_COOKED_DEPENDENCY_SIZE,
                           &dependency_bytes)) {
    return false_v;
  }
  const VkrMeshSource *source = &info->source;
  uint64_t source_bytes =
      16u + source->nodes.length * 128u + source->meshes.length * 12u;
  for (uint64_t i = 0; i < source->nodes.length; ++i) {
    if (source->nodes.data[i].name.length > VKR_MESH_COOKED_MAX_STRING_LENGTH ||
        !vkr_checked_add_u64(source_bytes, source->nodes.data[i].name.length,
                             &source_bytes))
      return false_v;
  }
  if (has_skin &&
      !vkr_checked_add_u64(source_bytes,
                           16u + (uint64_t)info->skin.skin_count * 4u +
                               total_vertices * 32u,
                           &source_bytes)) {
    return false_v;
  }
  const uint64_t directory_offset = VKR_MESH_COOKED_HEADER_SIZE;
  uint64_t dependency_offset = 0;
  uint64_t string_offset = 0;
  uint64_t stream_offset = 0;
  if (!vkr_checked_add_u64(directory_offset, range_bytes, &dependency_offset) ||
      !vkr_checked_add_u64(dependency_offset, dependency_bytes + source_bytes,
                           &string_offset) ||
      !vkr_checked_add_u64(string_offset, string_size, &stream_offset)) {
    return false_v;
  }
  stream_offset =
      vkr_align_up_u64(stream_offset, VKR_MESH_COOKED_STREAM_ALIGNMENT);
  uint64_t file_size = stream_offset;
  for (uint32_t i = 0; i < info->range_count; ++i) {
    ranges[i].vertex_stream_offset = file_size;
    if (!vkr_checked_add_u64(file_size, ranges[i].encoded_vertex_size,
                             &file_size)) {
      return false_v;
    }
    file_size = vkr_align_up_u64(file_size, VKR_MESH_COOKED_STREAM_ALIGNMENT);
    ranges[i].index_stream_offset = file_size;
    if (!vkr_checked_add_u64(file_size, ranges[i].encoded_index_size,
                             &file_size)) {
      return false_v;
    }
    file_size = vkr_align_up_u64(file_size, VKR_MESH_COOKED_STREAM_ALIGNMENT);
  }
  if (file_size > VKR_MESH_COOKED_MAX_FILE_SIZE || file_size > SIZE_MAX) {
    return false_v;
  }

  uint8_t *artifact = vkr_allocator_alloc(scratch_allocator, file_size,
                                          VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!artifact) {
    return false_v;
  }
  MemZero(artifact, file_size);
  VkrByteWriter writer = {.data = artifact, .size = file_size, .offset = 0};
  bool8_t ok = true_v;
  ok = ok && vkr_byte_writer_u32(&writer, VKR_MESH_COOKED_MAGIC);
  ok =
      ok && vkr_byte_writer_u32(&writer, has_skin ? VKR_MESH_COOKED_SKIN_VERSION
                                                  : VKR_MESH_COOKED_VERSION);
  ok = ok && vkr_byte_writer_u32(&writer, VKR_MESH_COOKED_ENDIAN_TAG);
  ok = ok && vkr_byte_writer_u32(&writer, VKR_MESH_COOKED_HEADER_SIZE);
  ok = ok && vkr_byte_writer_u32(&writer, vkr_meshopt_library_version());
  ok = ok && vkr_byte_writer_u32(&writer, VKR_MESHOPT_VERTEX_CODEC_VERSION);
  ok = ok && vkr_byte_writer_u32(&writer, VKR_MESHOPT_INDEX_CODEC_VERSION);
  ok = ok &&
       vkr_byte_writer_u32(&writer, VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1);
  ok = ok && vkr_byte_writer_u32(&writer, sizeof(VkrPackedStaticVertex));
  ok = ok && vkr_byte_writer_u32(&writer, sizeof(uint32_t));
  ok = ok && vkr_byte_writer_u32(&writer, info->range_count);
  ok = ok && vkr_byte_writer_u32(&writer, info->dependency_count);
  ok = ok && vkr_byte_writer_u64(&writer, directory_offset);
  ok = ok && vkr_byte_writer_u64(&writer, dependency_offset);
  ok = ok && vkr_byte_writer_u64(&writer, string_offset);
  ok = ok && vkr_byte_writer_u64(&writer, string_size);
  ok = ok && vkr_byte_writer_u64(&writer, stream_offset);
  ok = ok && vkr_byte_writer_u64(&writer, file_size);
  ok = ok && vkr_byte_writer_u64(&writer, 0u);
  ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)info->source_path.length);
  ok = ok && vkr_byte_writer_u32(&writer, 0u);
  ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)total_vertices);
  ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)total_indices);
  ok = ok && vkr_byte_writer_bytes(&writer, source_hash, 32u);
  ok = ok && vkr_byte_writer_bytes(&writer, settings_hash, 32u);
  ok = ok && vkr_byte_writer_u32(&writer, 0u);
  ok = ok && vkr_byte_writer_u32(&writer, 0u);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok && vkr_byte_writer_f32(&writer, geometry_decode.position_bias[i]);
  ok = ok && vkr_byte_writer_u32(&writer, geometry_decode.flags);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok && vkr_byte_writer_f32(&writer, geometry_decode.position_scale[i]);
  ok = ok && vkr_byte_writer_u32(&writer, geometry_decode.reserved);
  ok = ok && vkr_byte_writer_f32(&writer, info->budgets.position_relative);
  ok = ok && vkr_byte_writer_f32(&writer, info->budgets.normal_degrees);
  ok = ok && vkr_byte_writer_f32(&writer, info->budgets.tangent_degrees);
  ok = ok && vkr_byte_writer_f32(&writer, info->budgets.uv_absolute);
  ok = ok && vkr_byte_writer_f32(&writer, info->budgets.color_absolute);
  ok = ok && vkr_byte_writer_f32(&writer, quantization_max.position_max);
  ok = ok && vkr_byte_writer_f32(&writer, quantization_max.normal_degrees_max);
  ok = ok && vkr_byte_writer_f32(&writer, quantization_max.tangent_degrees_max);
  ok = ok && vkr_byte_writer_f32(&writer, quantization_max.uv_max);
  ok = ok && vkr_byte_writer_f32(&writer, quantization_max.color_max);
  ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)source->nodes.length);
  ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)source->meshes.length);
  if (!ok || writer.offset != VKR_MESH_COOKED_HEADER_SIZE) {
    return false_v;
  }

  writer.offset = directory_offset;
  for (uint32_t i = 0; ok && i < info->range_count; ++i) {
    const VkrMeshCookedRangeBuild *range = &ranges[i];
    const VkrGeometryUploadRange *upload_range = range->source;
    ok = ok && vkr_byte_writer_u32(&writer, i);
    ok = ok && vkr_byte_writer_u32(&writer, range->first_index);
    ok = ok && vkr_byte_writer_u32(&writer, range->index_count);
    ok = ok && vkr_byte_writer_u32(&writer, range->vertex_count);
    ok = ok && vkr_byte_writer_i32(&writer, 0);
    ok = ok && vkr_byte_writer_u32(&writer, upload_range->pipeline_domain);
    ok = ok && vkr_byte_writer_u64(&writer, range->material_offset);
    ok = ok && vkr_byte_writer_u32(
                   &writer, (uint32_t)upload_range->material_name.length);
    ok = ok && vkr_byte_writer_u32(
                   &writer, (uint32_t)upload_range->shader_override.length);
    ok = ok && vkr_byte_writer_u64(&writer, range->shader_offset);
    ok = ok && vkr_byte_writer_u64(&writer, range->vertex_stream_offset);
    ok = ok && vkr_byte_writer_u64(&writer, range->encoded_vertex_size);
    ok = ok && vkr_byte_writer_u64(&writer, (uint64_t)range->vertex_count *
                                                sizeof(VkrPackedStaticVertex));
    ok = ok && vkr_byte_writer_u32(&writer, range->vertex_crc);
    ok = ok && vkr_byte_writer_u32(&writer, range->index_crc);
    ok = ok && vkr_byte_writer_u64(&writer, range->index_stream_offset);
    ok = ok && vkr_byte_writer_u64(&writer, range->encoded_index_size);
    ok = ok && vkr_byte_writer_u64(&writer, (uint64_t)range->index_count *
                                                sizeof(uint32_t));
    ok = ok && vkr_byte_writer_f32(&writer, range->center.x);
    ok = ok && vkr_byte_writer_f32(&writer, range->center.y);
    ok = ok && vkr_byte_writer_f32(&writer, range->center.z);
    ok = ok && vkr_byte_writer_f32(&writer, range->min_extents.x);
    ok = ok && vkr_byte_writer_f32(&writer, range->min_extents.y);
    ok = ok && vkr_byte_writer_f32(&writer, range->min_extents.z);
    ok = ok && vkr_byte_writer_f32(&writer, range->max_extents.x);
    ok = ok && vkr_byte_writer_f32(&writer, range->max_extents.y);
    ok = ok && vkr_byte_writer_f32(&writer, range->max_extents.z);
    ok = ok && vkr_byte_writer_u32(&writer, 0u);
    ok = ok && vkr_byte_writer_f32(&writer, range->quantization.position_max);
    ok = ok &&
         vkr_byte_writer_f32(&writer, range->quantization.normal_degrees_max);
    ok = ok &&
         vkr_byte_writer_f32(&writer, range->quantization.tangent_degrees_max);
    ok = ok && vkr_byte_writer_f32(&writer, range->quantization.uv_max);
    ok = ok && vkr_byte_writer_f32(&writer, range->quantization.color_max);
    ok = ok && vkr_byte_writer_u32(&writer, 0u);
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok =
          ok && vkr_byte_writer_f32(&writer, range->decode.position_bias[axis]);
    ok = ok && vkr_byte_writer_u32(&writer, range->decode.flags);
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok = ok &&
           vkr_byte_writer_f32(&writer, range->decode.position_scale[axis]);
    ok = ok && vkr_byte_writer_u32(&writer, range->decode.reserved);
  }
  if (!ok || writer.offset != dependency_offset) {
    return false_v;
  }

  writer.offset = dependency_offset;
  for (uint32_t i = 0; ok && i < info->dependency_count; ++i) {
    ok = ok && vkr_byte_writer_u64(&writer, dependencies[i].string_offset);
    ok = ok &&
         vkr_byte_writer_u32(&writer, (uint32_t)dependencies[i].path.length);
    ok = ok && vkr_byte_writer_u32(&writer, 0u);
    ok = ok && vkr_byte_writer_u64(&writer, dependencies[i].byte_size);
    ok = ok && vkr_byte_writer_bytes(&writer, dependencies[i].hash, 32u);
    ok = ok && vkr_byte_writer_u64(&writer, 0u);
  }
  ok = ok && vkr_mesh_cooked_write_source_metadata(&writer, source);
  if (has_skin) {
    ok = ok && vkr_byte_writer_u64(&writer, info->skin.animation_fingerprint);
    ok = ok && vkr_byte_writer_u32(&writer, info->skin.skin_count);
    ok = ok && vkr_byte_writer_u32(&writer, (uint32_t)total_vertices);
    for (uint32_t i = 0; ok && i < info->skin.skin_count; ++i) {
      ok = vkr_byte_writer_u32(&writer, info->skin.joint_counts[i]);
    }
    for (uint32_t i = 0; ok && i < info->range_count; ++i) {
      for (uint32_t j = 0; ok && j < ranges[i].vertex_count; ++j) {
        const VkrMeshSkinVertex *vertex = &ranges[i].skin_vertices[j];
        for (uint32_t k = 0; k < 4u; ++k) {
          ok = ok && vkr_byte_writer_u32(&writer, vertex->joints[k]);
        }
        for (uint32_t k = 0; k < 4u; ++k) {
          ok = ok && vkr_byte_writer_f32(&writer, vertex->weights[k]);
        }
      }
    }
  }
  if (!ok || writer.offset != string_offset) {
    return false_v;
  }

  writer.offset = string_offset;
  ok = vkr_byte_writer_bytes(&writer, info->source_path.str,
                             info->source_path.length);
  for (uint32_t i = 0; ok && i < info->dependency_count; ++i) {
    ok = vkr_byte_writer_bytes(&writer, dependencies[i].path.str,
                               dependencies[i].path.length);
  }
  for (uint32_t i = 0; ok && i < info->range_count; ++i) {
    ok = vkr_byte_writer_bytes(&writer, ranges[i].source->material_name.str,
                               ranges[i].source->material_name.length);
    ok = ok &&
         vkr_byte_writer_bytes(&writer, ranges[i].source->shader_override.str,
                               ranges[i].source->shader_override.length);
  }
  if (!ok || writer.offset != string_offset + string_size) {
    return false_v;
  }

  for (uint32_t i = 0; i < info->range_count; ++i) {
    MemCopy(artifact + ranges[i].vertex_stream_offset,
            ranges[i].encoded_vertices, ranges[i].encoded_vertex_size);
    MemCopy(artifact + ranges[i].index_stream_offset, ranges[i].encoded_indices,
            ranges[i].encoded_index_size);
  }
  vkr_store_le_u32(
      artifact + VKR_MESH_COOKED_METADATA_CRC_OFFSET,
      vkr_crc32(artifact + directory_offset, stream_offset - directory_offset));
  vkr_store_le_u32(artifact + VKR_MESH_COOKED_HEADER_CRC_OFFSET,
                   vkr_crc32(artifact, VKR_MESH_COOKED_HEADER_SIZE));

  *out_data = artifact;
  *out_size = file_size;
  return true_v;
}

bool8_t vkr_mesh_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size) {
  if (!scratch_allocator || !output_path.str || output_path.length == 0 ||
      !data || size == 0) {
    return false_v;
  }
  String8 output_directory =
      file_path_get_directory(scratch_allocator, output_path);
  if (output_directory.length > 0 &&
      !file_ensure_directory(scratch_allocator, &output_directory)) {
    return false_v;
  }
  String8 temporary_path = string8_create_formatted(
      scratch_allocator, "%.*s.tmp.%u", (int32_t)output_path.length,
      output_path.str, vkr_platform_get_process_id());
  FilePathType path_type = vkr_mesh_cooked_path_is_absolute(output_path)
                               ? FILE_PATH_TYPE_ABSOLUTE
                               : FILE_PATH_TYPE_RELATIVE;
  FilePath output = file_path_create(string8_cstr(&output_path),
                                     scratch_allocator, path_type);
  FilePath temporary = file_path_create(string8_cstr(&temporary_path),
                                        scratch_allocator, path_type);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  uint64_t written = 0;
  if (file_open(&temporary, mode, &file) != FILE_ERROR_NONE ||
      file_write(&file, size, data, &written) != FILE_ERROR_NONE ||
      written != size || file_sync(&file) != FILE_ERROR_NONE) {
    file_close(&file);
    (void)file_remove(&temporary);
    return false_v;
  }
  file_close(&file);
  if (file_rename(&temporary, &output, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temporary);
    return false_v;
  }
  return true_v;
}

/* The caller decoded input successfully and only edits source-node values.
 * Preserve compressed geometry streams and every string/offset verbatim. */
bool8_t vkr_mesh_cooked_source_variant(VkrAllocator *allocator,
                                       const uint8_t *input, uint64_t size,
                                       const VkrMeshSource *source,
                                       uint8_t **out_data) {
  if (!allocator || !input || size < VKR_MESH_COOKED_HEADER_SIZE || !source ||
      !out_data) {
    return false_v;
  }
  const uint32_t version = vkr_load_le_u32(input + 4u);
  const uint32_t range_count = vkr_load_le_u32(input + 40u);
  const uint32_t dependency_count = vkr_load_le_u32(input + 44u);
  const uint64_t directory_offset = vkr_load_le_u64(input + 48u);
  const uint64_t dependency_offset = vkr_load_le_u64(input + 56u);
  const uint64_t string_offset = vkr_load_le_u64(input + 64u);
  const uint64_t stream_offset = vkr_load_le_u64(input + 80u);
  const uint32_t node_count = vkr_load_le_u32(input + 264u);
  const uint32_t mesh_count = vkr_load_le_u32(input + 268u);
  uint64_t metadata_offset =
      dependency_offset +
      (uint64_t)dependency_count * VKR_MESH_COOKED_DEPENDENCY_SIZE;
  if (node_count != source->nodes.length ||
      mesh_count != source->meshes.length ||
      directory_offset > metadata_offset || metadata_offset > string_offset ||
      string_offset > stream_offset || stream_offset > size ||
      !vkr_mesh_cooked_source_validate(allocator, source, range_count)) {
    return false_v;
  }
  if (version != VKR_MESH_COOKED_VERSION &&
      version != VKR_MESH_COOKED_SKIN_VERSION) {
    return false_v;
  }
  /* The caller decoded the original. Keep its mesh/skin binding invariant so
   * a same-size metadata patch cannot reinterpret validated influence indices.
   * Removing or rebinding a mesh in a skinned artifact requires a fresh cook.
   */
  VkrByteReader original = {
      .data = input, .size = string_offset, .offset = metadata_offset};
  uint64_t fingerprint = 0;
  uint32_t animations = 0;
  uint32_t reserved = 0;
  if (!vkr_byte_reader_u64(&original, &fingerprint) ||
      !vkr_byte_reader_u32(&original, &animations) ||
      !vkr_byte_reader_u32(&original, &reserved)) {
    return false_v;
  }
  for (uint32_t i = 0; i < node_count; ++i) {
    uint32_t fields[7] = {0};
    for (uint32_t j = 0; j < ArrayCount(fields); ++j) {
      if (!vkr_byte_reader_u32(&original, &fields[j])) {
        return false_v;
      }
    }
    if (version == VKR_MESH_COOKED_SKIN_VERSION &&
        (fields[1] != source->nodes.data[i].mesh ||
         fields[2] != source->nodes.data[i].mesh_variant ||
         fields[4] != source->nodes.data[i].skin)) {
      return false_v;
    }
    /* Matrix and punctual light occupy 96 bytes. */
    if (original.offset > original.size ||
        96u > original.size - original.offset) {
      return false_v;
    }
    original.offset += 96u;
    uint32_t name_length = 0;
    if (!vkr_byte_reader_u32(&original, &name_length) ||
        name_length != source->nodes.data[i].name.length ||
        name_length > original.size - original.offset) {
      return false_v;
    }
    original.offset += name_length;
  }
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t fields[3] = {0};
    for (uint32_t j = 0; j < ArrayCount(fields); ++j) {
      if (!vkr_byte_reader_u32(&original, &fields[j])) {
        return false_v;
      }
    }
    const VkrMeshSourceMesh *mesh = &source->meshes.data[i];
    if (version == VKR_MESH_COOKED_SKIN_VERSION &&
        (fields[0] != mesh->source_mesh_index ||
         fields[1] != mesh->first_range || fields[2] != mesh->range_count)) {
      return false_v;
    }
  }
  const uint64_t source_end = original.offset;
  if ((version == VKR_MESH_COOKED_VERSION && source_end != string_offset) ||
      (version == VKR_MESH_COOKED_SKIN_VERSION &&
       string_offset - source_end < 16u)) {
    return false_v;
  }
  uint8_t *copy =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, input, size);
  VkrByteWriter writer = {
      .data = copy, .size = source_end, .offset = metadata_offset};
  if (!vkr_mesh_cooked_write_source_metadata(&writer, source) ||
      writer.offset != source_end) {
    return false_v;
  }
  vkr_store_le_u32(
      copy + VKR_MESH_COOKED_METADATA_CRC_OFFSET,
      vkr_crc32(copy + directory_offset, stream_offset - directory_offset));
  MemZero(copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET, sizeof(uint32_t));
  vkr_store_le_u32(copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET,
                   vkr_crc32(copy, VKR_MESH_COOKED_HEADER_SIZE));
  *out_data = copy;
  return true_v;
}
