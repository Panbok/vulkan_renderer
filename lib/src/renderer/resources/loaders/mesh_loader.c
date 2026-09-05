#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/loaders/mesh_loader_gltf.h"
#include "renderer/resources/loaders/vkr_mesh_cooked.h"
#include "renderer/resources/loaders/vkr_meshoptimizer_bridge.h"

#include "containers/str.h"
#include "containers/vector.h"
#include "core/logger.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

/**
 * @brief Result of loading a single mesh in a batch operation.
 */
typedef struct VkrMeshBatchResult {
  VkrMeshLoaderResult *result;
  VkrRendererError error;
  bool8_t success;
} VkrMeshBatchResult;

typedef struct VkrMeshLoaderAsyncMaterialDependency {
  bool8_t use_merged;
  uint32_t index;
  String8 material_path;
  VkrResourceHandleInfo request_info;
} VkrMeshLoaderAsyncMaterialDependency;

typedef struct VkrMeshLoaderAsyncPayload {
  VkrMeshLoaderContext *context;
  VkrMeshLoaderResult *result;
  VkrMeshLoaderAsyncMaterialDependency *dependencies;
  uint32_t dependency_count;
  bool8_t ownership_transferred;
} VkrMeshLoaderAsyncPayload;

Vector(Vec2);
Vector(Vec3);
Vector(VkrVertex3d);
Vector(VkrMeshLoaderSubset);
Vector(VkrMeshLoaderSubmeshRange);
#define DEFAULT_SHADER string8_lit("shader.default.world")

typedef struct VkrMeshSourceDependency {
  String8 path;
} VkrMeshSourceDependency;
Vector(VkrMeshSourceDependency);

typedef struct VkrMeshLoaderMaterialDef {
  String8 name;
  String8 shader_name;
  Vec4 diffuse_color;
  Vec4 specular_color;
  Vec3 emission_color;
  float32_t shininess;
  float32_t alpha_cutoff;
  bool8_t alpha_cutoff_set;
  bool8_t cutout;
  String8 diffuse_map;
  String8 specular_map;
  String8 normal_map;
  String8 generated_path;
  VkrMaterialHandle material_handle;
  bool8_t generated;
} VkrMeshLoaderMaterialDef;
Vector(VkrMeshLoaderMaterialDef);

typedef struct VkrMeshLoaderSubsetBuilder {
  Vector_VkrVertex3d vertices;
  Vector_uint32_t indices;
  String8 name;
  String8 material_name;
  bool8_t material_is_path;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
} VkrMeshLoaderSubsetBuilder;

typedef struct VkrMeshLoaderMaterialBucket {
  String8 material_name;
  VkrMeshLoaderSubsetBuilder builder;
} VkrMeshLoaderMaterialBucket;
Vector(VkrMeshLoaderMaterialBucket);

typedef struct VkrMeshLoaderState {
  VkrMeshLoaderContext *context;
  VkrAllocator *load_allocator;
  VkrAllocator *temp_allocator;
  VkrAllocator *scratch_allocator;

  Vector_Vec3 positions;
  Vector_Vec3 normals;
  Vector_Vec2 texcoords;
  Vector_VkrMeshLoaderSubset subsets;
  Vector_VkrMeshLoaderMaterialDef materials;
  Vector_VkrMeshLoaderMaterialBucket material_buckets;
  Vector_VkrVertex3d merged_vertices;
  Vector_uint32_t merged_indices;
  Vector_VkrMeshLoaderSubmeshRange merged_submeshes;
  Vector_VkrMeshSourceDependency source_dependencies;
  VkrMeshLoaderBuffer merged_buffer;
  uint32_t current_bucket;

  String8 source_path;
  String8 source_dir;
  String8 source_stem;
  String8 source_extension;
  String8 material_dir;

  VkrRendererError *out_error;
} VkrMeshLoaderState;

typedef struct VkrMeshLoaderVertexRef {
  int32_t position;
  int32_t texcoord;
  int32_t normal;
} VkrMeshLoaderVertexRef;

typedef struct VkrMeshLoadJobPayload {
  String8 mesh_path;
  VkrMeshLoaderContext *context;
  VkrAllocator *result_allocator;
  VkrMeshLoaderResult *result;
  VkrRendererError *error;
  bool8_t *success;
} VkrMeshLoadJobPayload;

vkr_internal bool8_t vkr_mesh_loader_builder_init(
    VkrMeshLoaderSubsetBuilder *builder, VkrAllocator *allocator);
vkr_internal bool8_t vkr_mesh_loader_prepare_async(VkrResourceLoader *self,
                                                   String8 name,
                                                   VkrAllocator *temp_alloc,
                                                   void **out_payload,
                                                   VkrRendererError *out_error);
vkr_internal bool8_t vkr_mesh_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error);
vkr_internal bool8_t vkr_mesh_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost);
vkr_internal void vkr_mesh_loader_release_async_payload(VkrResourceLoader *self,
                                                        void *payload);
vkr_internal uint32_t vkr_mesh_loader_add_material_bucket(
    VkrMeshLoaderState *state, const String8 *material_name);
vkr_internal uint32_t vkr_mesh_loader_find_material_bucket(
    VkrMeshLoaderState *state, const String8 *material_name);
vkr_internal bool8_t vkr_mesh_loader_set_current_material(
    VkrMeshLoaderState *state, const String8 *material_name);
vkr_internal VkrMeshLoaderSubsetBuilder *
vkr_mesh_loader_get_current_builder(VkrMeshLoaderState *state);
vkr_internal void
vkr_mesh_loader_prepare_merged_buffer(VkrMeshLoaderState *state);
vkr_internal String8 vkr_mesh_loader_make_material_dir(VkrAllocator *allocator,
                                                       const String8 *stem);
vkr_internal bool8_t vkr_mesh_loader_read_file_to_string(
    VkrAllocator *allocator, String8 file_path, String8 *out_content,
    VkrRendererError *out_error);
vkr_internal void vkr_mesh_loader_parse_next_line(String8 *file_str,
                                                  uint64_t *offset,
                                                  String8 *out_line);
vkr_internal bool8_t vkr_mesh_loader_parse_vec3_line(String8 *line,
                                                     uint32_t prefix_len,
                                                     Vec3 *out_vec);
vkr_internal bool8_t vkr_mesh_loader_finalize_builder(
    VkrMeshLoaderState *state, VkrMeshLoaderSubsetBuilder *builder);
vkr_internal bool8_t
vkr_mesh_loader_finalize_all_buckets(VkrMeshLoaderState *state);
vkr_internal bool8_t vkr_mesh_loader_parse_obj(VkrMeshLoaderState *state);
vkr_internal bool8_t vkr_mesh_loader_parse_source(VkrMeshLoaderState *state);
vkr_internal bool8_t vkr_mesh_loader_accept_gltf_primitive(
    void *user_data, const VkrMeshLoaderGltfPrimitive *primitive);
vkr_internal bool8_t vkr_mesh_loader_path_is_absolute(String8 path);
vkr_internal bool8_t vkr_mesh_loader_capture_source_dependency(
    VkrMeshLoaderState *state, String8 path);
vkr_internal bool8_t vkr_mesh_loader_collect_source_dependencies(
    VkrMeshLoaderState *state, Vector_String8 *paths);
vkr_internal void vkr_mesh_loader_cleanup_arenas(VkrMeshLoaderResult **results,
                                                 Arena **arenas,
                                                 void **pool_chunks,
                                                 uint32_t count,
                                                 VkrArenaPool *arena_pool);
vkr_internal void vkr_mesh_loader_set_all_errors(VkrMeshBatchResult *results,
                                                 uint32_t count,
                                                 VkrRendererError error);
vkr_internal void
vkr_mesh_loader_destroy_result(VkrMeshLoaderContext *context,
                               VkrMeshLoaderResult *result,
                               bool8_t release_material_handles);

vkr_internal String8 vkr_mesh_loader_get_extension(VkrAllocator *allocator,
                                                   String8 path) {
  if (!allocator || !path.str || path.length == 0) {
    return (String8){0};
  }

  for (uint64_t i = path.length; i > 0; --i) {
    if (path.str[i - 1] == '.') {
      String8 ext = string8_substring(&path, i, path.length);
      return string8_duplicate(allocator, &ext);
    }
  }

  return (String8){0};
}

vkr_internal uint64_t
vkr_mesh_loader_source_dependency_bytes(const VkrMeshLoaderState *state) {
  uint64_t total = 0;
  for (uint64_t i = 0; i < state->source_dependencies.length; ++i) {
    const String8 path = state->source_dependencies.data[i].path;
    FilePath file_path = file_path_create(
        string8_cstr(&path), state->scratch_allocator,
        vkr_mesh_loader_path_is_absolute(path) ? FILE_PATH_TYPE_ABSOLUTE
                                               : FILE_PATH_TYPE_RELATIVE);
    FileStats stats = {0};
    if (file_stats(&file_path, &stats) != FILE_ERROR_NONE ||
        UINT64_MAX - total < stats.size) {
      return 0;
    }
    total += stats.size;
  }
  return total;
}

vkr_internal bool8_t vkr_mesh_loader_analyze_buffer(
    const VkrMeshLoaderBuffer *buffer, const VkrMeshLoaderSubmeshRange *ranges,
    uint32_t range_count, VkrAllocator *scratch_allocator, bool8_t before,
    VkrMeshLoadMetrics *metrics) {
  if (!buffer || !ranges || range_count == 0 || !scratch_allocator ||
      !metrics || buffer->index_size != sizeof(uint32_t) || !buffer->vertices ||
      !buffer->indices) {
    return false_v;
  }
  uint32_t max_range_indices = 0;
  for (uint32_t i = 0; i < range_count; ++i) {
    max_range_indices = Max(max_range_indices, ranges[i].index_count);
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }
  uint32_t *local_indices = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)max_range_indices * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!local_indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  uint64_t transformed = 0;
  uint64_t fetched = 0;
  uint64_t triangles = 0;
  uint64_t analyzed_vertices = 0;
  uint64_t analyzed_vertex_bytes = 0;
  const uint32_t *indices = buffer->indices;
  bool8_t valid = true_v;
  for (uint32_t i = 0; i < range_count; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &ranges[i];
    if (range->index_count == 0 || range->index_count % 3u != 0 ||
        range->first_index > buffer->index_count ||
        range->index_count > buffer->index_count - range->first_index) {
      valid = false_v;
      break;
    }
    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      const uint32_t index = indices[range->first_index + j];
      if (index >= buffer->vertex_count) {
        valid = false_v;
        break;
      }
      min_vertex = Min(min_vertex, index);
      max_vertex = Max(max_vertex, index);
      local_indices[j] = index;
    }
    if (!valid) {
      break;
    }
    const uint32_t vertex_count = max_vertex - min_vertex + 1u;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      local_indices[j] -= min_vertex;
    }
    VkrMeshoptAnalysis analysis = {0};
    if (vkr_meshopt_analyze_range(local_indices, range->index_count,
                                  vertex_count, buffer->vertex_size,
                                  &analysis) != 0) {
      valid = false_v;
      break;
    }
    transformed += analysis.vertices_transformed;
    fetched += analysis.bytes_fetched;
    triangles += range->index_count / 3u;
    analyzed_vertices += vertex_count;
    analyzed_vertex_bytes += (uint64_t)vertex_count * buffer->vertex_size;
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!valid) {
    return false_v;
  }
  if (before) {
    metrics->analyzed_triangles = triangles;
    metrics->analyzed_vertices = analyzed_vertices;
    metrics->analyzed_vertex_bytes_before = analyzed_vertex_bytes;
    metrics->vertices_transformed_before = transformed;
    metrics->bytes_fetched_before = fetched;
  } else {
    metrics->analyzed_vertex_bytes_after = analyzed_vertex_bytes;
    metrics->vertices_transformed_after = transformed;
    metrics->bytes_fetched_after = fetched;
  }
  return true_v;
}

vkr_internal void
vkr_mesh_loader_complete_cooked_analysis(VkrMeshLoadMetrics *metrics) {
  metrics->vertices_transformed_after = metrics->vertices_transformed_before;
  metrics->bytes_fetched_after = metrics->bytes_fetched_before;
  metrics->analyzed_vertex_bytes_after = metrics->analyzed_vertex_bytes_before;
}

vkr_internal bool8_t vkr_mesh_loader_commit_merged_buffer(
    const VkrMeshLoaderState *state, VkrAllocator *result_allocator,
    VkrAllocator *scratch_allocator, VkrMeshLoaderBuffer *out_buffer,
    Array_VkrMeshLoaderSubmeshRange *out_ranges) {
  const VkrMeshLoaderBuffer *source = &state->merged_buffer;
  const uint32_t range_count = (uint32_t)state->merged_submeshes.length;
  if (!result_allocator || !scratch_allocator || !out_buffer || !out_ranges ||
      source->vertex_size != sizeof(VkrVertex3d) ||
      source->index_size != sizeof(uint32_t) || source->vertex_count == 0 ||
      source->index_count == 0 || !source->vertices || !source->indices ||
      range_count == 0) {
    log_error("MeshLoader: merged mesh state is incomplete");
    return false_v;
  }
  const uint64_t working_vertex_bytes =
      (uint64_t)source->vertex_count * sizeof(VkrVertex3d);
  const uint64_t packed_vertex_bytes =
      (uint64_t)source->vertex_count * sizeof(VkrPackedStaticVertex);
  const uint64_t index_bytes =
      (uint64_t)source->index_count * source->index_size;
  VkrVertex3d *working_vertices = vkr_allocator_alloc(
      scratch_allocator, working_vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrPackedStaticVertex *vertices = vkr_allocator_alloc(
      result_allocator, packed_vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(result_allocator, index_bytes,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrGpuGeometryDecodeRecord *decodes = vkr_allocator_alloc(
      result_allocator,
      (uint64_t)range_count * sizeof(VkrGpuGeometryDecodeRecord),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  Array_VkrMeshLoaderSubmeshRange ranges =
      array_create_VkrMeshLoaderSubmeshRange(result_allocator, range_count);
  if (!working_vertices || !vertices || !indices || !decodes || !ranges.data) {
    log_error("MeshLoader: failed to allocate optimized mesh (%u vertices, %u "
              "indices, %u ranges)",
              source->vertex_count, source->index_count, range_count);
    return false_v;
  }

  uint32_t output_vertex_count = source->vertex_count;
  uint32_t max_range_indices = 0;
  for (uint32_t i = 0; i < range_count; ++i) {
    max_range_indices =
        Max(max_range_indices, state->merged_submeshes.data[i].index_count);
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_error("MeshLoader: failed to acquire optimization scratch scope");
    return false_v;
  }
  uint32_t *local_indices = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)max_range_indices * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!local_indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    log_error("MeshLoader: failed to allocate %u optimization indices",
              max_range_indices);
    return false_v;
  }
  const VkrVertex3d *source_vertices = source->vertices;
  const uint32_t *source_indices = source->indices;
  uint32_t source_vertex_cursor = 0;
  uint32_t output_vertex_cursor = 0;
  uint32_t index_cursor = 0;
  bool8_t valid = true_v;
  for (uint32_t i = 0; i < range_count; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &state->merged_submeshes.data[i];
    if (range->first_index != index_cursor || range->index_count == 0 ||
        range->index_count % 3u != 0 ||
        range->index_count > source->index_count - index_cursor) {
      valid = false_v;
      break;
    }
    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      const uint32_t index = source_indices[index_cursor + j];
      if (index >= source->vertex_count) {
        valid = false_v;
        break;
      }
      min_vertex = Min(min_vertex, index);
      max_vertex = Max(max_vertex, index);
      local_indices[j] = index;
    }
    if (!valid || min_vertex != source_vertex_cursor) {
      valid = false_v;
      break;
    }
    const uint32_t source_range_vertices = max_vertex - min_vertex + 1u;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      local_indices[j] -= min_vertex;
    }
    const size_t optimized_count = vkr_meshopt_optimize_range(
        working_vertices + output_vertex_cursor, indices + index_cursor,
        source_vertices + source_vertex_cursor, local_indices,
        source_range_vertices, range->index_count, sizeof(VkrVertex3d));
    if (optimized_count == 0 || optimized_count > source_range_vertices) {
      valid = false_v;
      break;
    }
    for (uint32_t j = 0; j < range->index_count; ++j) {
      indices[index_cursor + j] += output_vertex_cursor;
    }
    source_vertex_cursor += source_range_vertices;
    output_vertex_cursor += (uint32_t)optimized_count;
    index_cursor += range->index_count;
  }
  valid = valid && source_vertex_cursor == source->vertex_count &&
          index_cursor == source->index_count;
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!valid) {
    log_error("MeshLoader: optimized mesh ranges are not contiguous (%u/%u "
              "vertices, %u/%u indices)",
              source_vertex_cursor, source->vertex_count, index_cursor,
              source->index_count);
    return false_v;
  }
  output_vertex_count = output_vertex_cursor;

  const VkrGeometryQuantizationBudgets budgets =
      vkr_packed_geometry_default_budgets();
  VkrGeometryQuantizationMetrics quantization = {0};
  uint32_t packed_vertex_cursor = 0;
  for (uint32_t i = 0; i < range_count; ++i) {
    const VkrMeshLoaderSubmeshRange *source_range =
        &state->merged_submeshes.data[i];
    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0;
    for (uint32_t j = 0; j < source_range->index_count; ++j) {
      const uint32_t index = indices[source_range->first_index + j];
      min_vertex = Min(min_vertex, index);
      max_vertex = Max(max_vertex, index);
    }
    if (min_vertex != packed_vertex_cursor || max_vertex >= output_vertex_count)
      return false_v;
    const uint32_t range_vertex_count = max_vertex - min_vertex + 1u;
    Vec3 range_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
    Vec3 range_max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
    for (uint32_t j = 0; j < range_vertex_count; ++j) {
      const Vec3 position =
          vkr_vertex_unpack_vec3(working_vertices[min_vertex + j].position);
      range_min.x = Min(range_min.x, position.x);
      range_min.y = Min(range_min.y, position.y);
      range_min.z = Min(range_min.z, position.z);
      range_max.x = Max(range_max.x, position.x);
      range_max.y = Max(range_max.y, position.y);
      range_max.z = Max(range_max.z, position.z);
    }
    VkrGeometryQuantizationMetrics range_quantization = {0};
    if (!vkr_packed_geometry_pack(working_vertices + min_vertex,
                                  range_vertex_count, range_min, range_max,
                                  &budgets, vertices + min_vertex, &decodes[i],
                                  &range_quantization)) {
      log_error("MeshLoader: failed to quantize range %u (%u vertices)", i,
                range_vertex_count);
      return false_v;
    }
    quantization.position_max =
        Max(quantization.position_max, range_quantization.position_max);
    quantization.normal_degrees_max = Max(
        quantization.normal_degrees_max, range_quantization.normal_degrees_max);
    quantization.tangent_degrees_max =
        Max(quantization.tangent_degrees_max,
            range_quantization.tangent_degrees_max);
    quantization.uv_max = Max(quantization.uv_max, range_quantization.uv_max);
    quantization.color_max =
        Max(quantization.color_max, range_quantization.color_max);
    VkrMeshLoaderSubmeshRange range = *source_range;
    range.decode_index = i;
    range.material_name =
        string8_duplicate(result_allocator, &source_range->material_name);
    range.shader_override =
        string8_duplicate(result_allocator, &source_range->shader_override);
    range.material_handle = VKR_MATERIAL_HANDLE_INVALID;
    array_set_VkrMeshLoaderSubmeshRange(&ranges, i, range);
    packed_vertex_cursor += range_vertex_count;
  }
  if (packed_vertex_cursor != output_vertex_count)
    return false_v;
  *out_buffer = (VkrMeshLoaderBuffer){
      .vertex_size = sizeof(VkrPackedStaticVertex),
      .vertex_count = output_vertex_count,
      .vertices = vertices,
      .index_size = sizeof(uint32_t),
      .index_count = source->index_count,
      .indices = indices,
      .vertex_layout = VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1,
      .decodes = decodes,
      .decode_count = range_count,
      .quantization = quantization,
  };
  *out_ranges = ranges;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_file_to_string(
    VkrAllocator *allocator, String8 file_path, String8 *out_content,
    VkrRendererError *out_error) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(out_content != NULL, "Out content is NULL");

  FilePath fp = file_path_create(string8_cstr(&file_path), allocator,
                                 vkr_mesh_loader_path_is_absolute(file_path)
                                     ? FILE_PATH_TYPE_ABSOLUTE
                                     : FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("MeshLoader: failed to open '%s'", fp.path.str);
    return false_v;
  }

  FileError read_err = file_read_string(&fh, allocator, out_content);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("MeshLoader: failed to read '%s'", fp.path.str);
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_path_is_absolute(String8 path) {
  return (path.length > 0 && (path.str[0] == '/' || path.str[0] == '\\')) ||
         (path.length > 1 && path.str[1] == ':');
}

vkr_internal bool8_t vkr_mesh_loader_dependency_path_equals(String8 lhs,
                                                            String8 rhs) {
#if defined(PLATFORM_WINDOWS)
  return string8_equalsi(&lhs, &rhs);
#else
  return string8_equals(&lhs, &rhs);
#endif
}

vkr_internal bool8_t vkr_mesh_loader_capture_source_dependency(
    VkrMeshLoaderState *state, String8 path) {
  if (!state || !path.str || path.length == 0) {
    return false_v;
  }

  for (uint64_t i = 0; i < state->source_dependencies.length; ++i) {
    VkrMeshSourceDependency *existing =
        vector_get_VkrMeshSourceDependency(&state->source_dependencies, i);
    if (existing &&
        vkr_mesh_loader_dependency_path_equals(existing->path, path)) {
      return true_v;
    }
  }

  String8 owned_path = string8_duplicate(state->load_allocator, &path);
  if (!owned_path.str) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  FilePathType type = vkr_mesh_loader_path_is_absolute(path)
                          ? FILE_PATH_TYPE_ABSOLUTE
                          : FILE_PATH_TYPE_RELATIVE;
  FilePath file_path =
      file_path_create(string8_cstr(&owned_path), state->load_allocator, type);
  if (!file_exists(&file_path)) {
    return false_v;
  }

  VkrMeshSourceDependency dependency = {.path = owned_path};
  if (!vector_push_VkrMeshSourceDependency(&state->source_dependencies,
                                           dependency)) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_collect_source_dependencies(
    VkrMeshLoaderState *state, Vector_String8 *paths) {
  if (!state || !paths) {
    return false_v;
  }

  for (uint64_t i = 0; i < paths->length; ++i) {
    String8 *path = vector_get_String8(paths, i);
    if (!path || !path->str || path->length == 0) {
      return false_v;
    }
    if (!vkr_mesh_loader_capture_source_dependency(state, *path)) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_state_create(
    VkrMeshLoaderContext *context, VkrAllocator *load_allocator,
    VkrAllocator *temp_allocator, VkrAllocator *scratch_allocator, String8 name,
    VkrRendererError *out_error, VkrMeshLoaderState *out_state) {

  VkrMeshLoaderState state = {
      .context = context,
      .load_allocator = load_allocator,
      .temp_allocator = temp_allocator,
      .scratch_allocator = scratch_allocator,
      .positions = {.allocator = load_allocator},
      .normals = {.allocator = load_allocator},
      .texcoords = {.allocator = load_allocator},
      .subsets = {.allocator = load_allocator},
      .materials = {.allocator = load_allocator},
      .material_buckets = {.allocator = load_allocator},
      .merged_vertices = {.allocator = load_allocator},
      .merged_indices = {.allocator = load_allocator},
      .merged_submeshes = {.allocator = load_allocator},
      .source_dependencies = {.allocator = load_allocator},
      .merged_buffer = {0},
      .current_bucket = 0,
      .source_path = {0},
      .source_dir = {0},
      .source_stem = {0},
      .source_extension = {0},
      .out_error = out_error,
  };

  state.source_path = string8_duplicate(state.load_allocator, &name);
  state.source_dir = file_path_get_directory(state.load_allocator, name);
  state.source_stem = string8_get_stem(state.load_allocator, name);
  state.source_extension =
      vkr_mesh_loader_get_extension(state.load_allocator, name);
  state.material_dir = vkr_mesh_loader_make_material_dir(state.load_allocator,
                                                         &state.source_stem);
  if (!state.source_path.str || !state.source_stem.str ||
      !state.material_dir.str ||
      vkr_mesh_loader_add_material_bucket(&state, NULL) == VKR_INVALID_ID) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  *out_state = state;
  return true_v;
}

vkr_internal VkrMeshLoaderVertexRef
vkr_mesh_loader_parse_vertex_ref(const String8 *token) {
  VkrMeshLoaderVertexRef ref = {0, 0, 0};
  if (!token || !token->str || token->length == 0)
    return ref;

  int32_t values[3] = {0};
  uint32_t current = 0;
  int32_t sign = 1;
  int32_t value = 0;
  bool8_t has_digits = false_v;

  for (uint64_t i = 0; i < token->length; ++i) {
    uint8_t ch = token->str[i];
    if (ch == '-') {
      if (has_digits)
        continue;
      sign = -1;
    } else if (ch == '/') {
      if (current < 3)
        values[current++] = has_digits ? sign * value : 0;
      sign = 1;
      value = 0;
      has_digits = false_v;
    } else if (ch >= '0' && ch <= '9') {
      value = value * 10 + (int32_t)(ch - '0');
      has_digits = true_v;
    }
  }

  if (current < 3)
    values[current++] = has_digits ? sign * value : 0;

  ref.position = values[0];
  ref.texcoord = (current >= 2) ? values[1] : 0;
  ref.normal = (current >= 3) ? values[2] : 0;

  return ref;
}

vkr_internal uint32_t vkr_mesh_loader_fix_index(int32_t value, uint32_t count) {
  if (value > 0)
    return (uint32_t)(value - 1);

  if (value < 0) {
    int64_t resolved = (int64_t)count + value;
    return resolved < 0 ? 0 : (uint32_t)resolved;
  }

  return 0;
}

vkr_internal bool8_t vkr_mesh_loader_builder_init(
    VkrMeshLoaderSubsetBuilder *builder, VkrAllocator *allocator) {
  builder->vertices = (Vector_VkrVertex3d){.allocator = allocator};
  builder->indices = (Vector_uint32_t){.allocator = allocator};
  builder->name = vkr_string8_duplicate_cstr(allocator, "default");
  builder->material_name = (String8){0};
  builder->material_is_path = false_v;
  builder->pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD;
  builder->shader_override = (String8){0};
  return builder->name.str != NULL;
}

vkr_internal uint32_t vkr_mesh_loader_find_material_bucket(
    VkrMeshLoaderState *state, const String8 *material_name) {
  if (!state || !state->material_buckets.data ||
      state->material_buckets.length == 0) {
    return VKR_INVALID_ID;
  }

  if (!material_name || !material_name->str || material_name->length == 0) {
    return 0;
  }

  for (uint64_t i = 0; i < state->material_buckets.length; ++i) {
    VkrMeshLoaderMaterialBucket *bucket =
        vector_get_VkrMeshLoaderMaterialBucket(&state->material_buckets, i);
    if (!bucket || !bucket->material_name.str)
      continue;
    if (string8_equalsi(&bucket->material_name, material_name)) {
      return (uint32_t)i;
    }
  }

  return VKR_INVALID_ID;
}

vkr_internal uint32_t vkr_mesh_loader_add_material_bucket(
    VkrMeshLoaderState *state, const String8 *material_name) {
  assert_log(state != NULL, "State is NULL");

  VkrMeshLoaderMaterialBucket bucket = {0};
  if (material_name && material_name->str && material_name->length > 0) {
    bucket.material_name =
        string8_duplicate(state->load_allocator, material_name);
  }
  if ((material_name && material_name->length && !bucket.material_name.str) ||
      !vkr_mesh_loader_builder_init(&bucket.builder, state->load_allocator))
    return VKR_INVALID_ID;
  if (bucket.material_name.str) {
    bucket.builder.material_name = bucket.material_name;
    bucket.builder.material_is_path = false_v;
    bucket.builder.name = bucket.material_name;
  }

  if (!vector_push_VkrMeshLoaderMaterialBucket(&state->material_buckets,
                                               bucket))
    return VKR_INVALID_ID;
  return (uint32_t)(state->material_buckets.length - 1);
}

vkr_internal bool8_t vkr_mesh_loader_set_current_material(
    VkrMeshLoaderState *state, const String8 *material_name) {
  assert_log(state != NULL, "State is NULL");

  uint32_t index = vkr_mesh_loader_find_material_bucket(state, material_name);
  if (index == VKR_INVALID_ID) {
    index = vkr_mesh_loader_add_material_bucket(state, material_name);
  }
  if (index == VKR_INVALID_ID) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  state->current_bucket = index;
  return true_v;
}

vkr_internal VkrMeshLoaderSubsetBuilder *
vkr_mesh_loader_get_current_builder(VkrMeshLoaderState *state) {
  if (!state || state->material_buckets.length == 0) {
    return NULL;
  }

  if (state->current_bucket >= state->material_buckets.length) {
    state->current_bucket = 0;
  }

  VkrMeshLoaderMaterialBucket *bucket = vector_get_VkrMeshLoaderMaterialBucket(
      &state->material_buckets, state->current_bucket);
  return bucket ? &bucket->builder : NULL;
}

vkr_internal void
vkr_mesh_loader_prepare_merged_buffer(VkrMeshLoaderState *state) {
  if (!state) {
    return;
  }

  state->merged_buffer.vertex_size = sizeof(VkrVertex3d);
  state->merged_buffer.vertex_count = (uint32_t)state->merged_vertices.length;
  state->merged_buffer.vertices = state->merged_vertices.data;
  state->merged_buffer.index_size = sizeof(uint32_t);
  state->merged_buffer.index_count = (uint32_t)state->merged_indices.length;
  state->merged_buffer.indices = state->merged_indices.data;
}

vkr_internal VkrMeshLoaderMaterialDef *
vkr_mesh_loader_find_material(Vector_VkrMeshLoaderMaterialDef *materials,
                              const String8 *name) {
  assert_log(materials != NULL, "Materials is NULL");

  if (!name->str || name->length == 0)
    return NULL;

  for (uint64_t i = 0; i < materials->length; ++i) {
    VkrMeshLoaderMaterialDef *def =
        vector_get_VkrMeshLoaderMaterialDef(materials, i);
    if (string8_equalsi(&def->name, name))
      return def;
  }

  return NULL;
}

vkr_internal String8 vkr_mesh_loader_make_material_dir(VkrAllocator *allocator,
                                                       const String8 *stem) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(stem != NULL, "Stem is NULL");
  return string8_create_formatted(allocator, "assets/materials/%.*s",
                                  (int32_t)stem->length, stem->str);
}

vkr_internal String8 vkr_mesh_loader_make_material_path(
    VkrAllocator *allocator, const String8 *stem, const String8 *material) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(stem != NULL, "Stem is NULL");
  assert_log(material != NULL, "Material is NULL");

  return string8_create_formatted(allocator, "assets/materials/%.*s/%.*s.mt",
                                  (int32_t)stem->length, stem->str,
                                  (int32_t)material->length, material->str);
}

vkr_internal String8 vkr_mesh_loader_texture_basename(VkrAllocator *allocator,
                                                      const String8 *token) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token->str || token->length == 0)
    return (String8){0};

  uint64_t start = 0;
  for (uint64_t i = token->length; i > 0; --i) {
    uint8_t ch = token->str[i - 1];
    if (ch == '/' || ch == '\\') {
      start = i;
      break;
    }
  }

  String8 view = string8_substring(token, start, token->length);
  return string8_duplicate(allocator, &view);
}

vkr_internal String8 vkr_mesh_loader_texture_path(VkrAllocator *allocator,
                                                  const String8 *token) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token->str || token->length == 0)
    return (String8){0};

  String8 file = vkr_mesh_loader_texture_basename(allocator, token);
  if (!file.str)
    return (String8){0};

  return string8_create_formatted(allocator, "assets/textures/%.*s",
                                  (int32_t)file.length, file.str);
}

vkr_internal bool8_t vkr_mesh_loader_write_material_file(
    VkrMeshLoaderState *state, VkrMeshLoaderMaterialDef *material,
    String8 relative_path) {
  assert_log(state != NULL, "State is NULL");
  assert_log(material != NULL, "Material is NULL");
  assert_log(relative_path.str != NULL, "Relative path is NULL");

  FilePath dir_path =
      file_path_create(string8_cstr(&state->material_dir),
                       state->load_allocator, FILE_PATH_TYPE_RELATIVE);
  if (!file_ensure_directory(state->load_allocator, &dir_path.path)) {
    log_error("MeshLoader: failed to create material directory '%s'",
              string8_cstr(&state->material_dir));

    return false_v;
  }

  FilePath file_path =
      file_path_create((const char *)relative_path.str, state->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to open material file '%s': %s",
              file_path.path.str, file_get_error_string(ferr).str);
    return false_v;
  }

  String8 shader_value = (material->shader_name.length > 0)
                             ? material->shader_name
                             : DEFAULT_SHADER;
  float32_t alpha_cutoff =
      material->alpha_cutoff_set
          ? material->alpha_cutoff
          : (material->cutout ? VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT : 0.0f);
  String8 alpha_cutoff_line = {0};
  if (alpha_cutoff > 0.0f) {
    alpha_cutoff_line = string8_create_formatted(
        state->load_allocator, "alpha_cutoff=%f", alpha_cutoff);
  }

  String8 lines[] = {
      string8_create_formatted(state->load_allocator, "name=%.*s",
                               (int32_t)material->name.length,
                               material->name.str),
      string8_create_formatted(state->load_allocator, "diffuse_texture=%.*s",
                               (int32_t)material->diffuse_map.length,
                               material->diffuse_map.str),
      string8_create_formatted(state->load_allocator,
                               "diffuse_colorspace=srgb"),
      string8_create_formatted(
          state->load_allocator, "diffuse_color=%f,%f,%f,%f",
          material->diffuse_color.x, material->diffuse_color.y,
          material->diffuse_color.z, material->diffuse_color.w),
      string8_create_formatted(state->load_allocator, "specular_texture=%.*s",
                               (int32_t)material->specular_map.length,
                               material->specular_map.str),
      string8_create_formatted(state->load_allocator,
                               "specular_colorspace=linear"),
      string8_create_formatted(
          state->load_allocator, "specular_color=%f,%f,%f,%f",
          material->specular_color.x, material->specular_color.y,
          material->specular_color.z, material->specular_color.w),
      string8_create_formatted(state->load_allocator, "norm_texture=%.*s",
                               (int32_t)material->normal_map.length,
                               material->normal_map.str),
      string8_create_formatted(state->load_allocator,
                               "normal_colorspace=linear"),
      string8_create_formatted(state->load_allocator, "shininess=%f",
                               material->shininess),
      string8_create_formatted(state->load_allocator, "emission_color=%f,%f,%f",
                               material->emission_color.x,
                               material->emission_color.y,
                               material->emission_color.z),
      alpha_cutoff_line,
      string8_create_formatted(state->load_allocator, "shader=%.*s",
                               (int32_t)shader_value.length,
                               (const char *)shader_value.str),
      string8_create_formatted(state->load_allocator, "pipeline=%s", "world"),
  };

  for (uint32_t i = 0; i < ArrayCount(lines); ++i) {
    if (lines[i].length == 0)
      continue;
    FileError werr = file_write_line(&fh, &lines[i]);
    if (werr != FILE_ERROR_NONE) {
      log_error("MeshLoader: failed to write material '%s': %s",
                file_path.path.str, file_get_error_string(werr).str);
      file_close(&fh);
      return false_v;
    }
  }

  file_close(&fh);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_resolve_material(
    VkrMeshLoaderState *state, const String8 *material_name, String8 *out_path,
    VkrMaterialHandle *out_handle) {
  assert_log(state != NULL, "State is NULL");

  if (!material_name->str || material_name->length == 0)
    return false_v;

  VkrMeshLoaderMaterialDef *mat =
      vkr_mesh_loader_find_material(&state->materials, material_name);
  if (!mat) {
    log_warn("MeshLoader: unknown material '%.*s'",
             (int32_t)material_name->length, material_name->str);
    return false_v;
  }

  if (!mat->generated_path.str) {
    mat->generated_path = vkr_mesh_loader_make_material_path(
        state->load_allocator, &state->source_stem, material_name);
  }

  if (!mat->generated) {
    if (!vkr_mesh_loader_write_material_file(state, mat, mat->generated_path))
      return false_v;
    mat->generated = true_v;
  }

  if (!vkr_mesh_loader_capture_source_dependency(state, mat->generated_path)) {
    return false_v;
  }

  if (out_path)
    *out_path = mat->generated_path;
  if (out_handle)
    *out_handle = VKR_MATERIAL_HANDLE_INVALID;

  return true_v;
}

vkr_internal void vkr_mesh_loader_compute_bounds(const VkrVertex3d *vertices,
                                                 uint32_t count, Vec3 *out_min,
                                                 Vec3 *out_max,
                                                 Vec3 *out_center) {
  Vec3 min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);

  for (uint32_t i = 0; i < count; ++i) {
    Vec3 pos = vkr_vertex_unpack_vec3(vertices[i].position);
    min.x = vkr_min_f32(min.x, pos.x);
    min.y = vkr_min_f32(min.y, pos.y);
    min.z = vkr_min_f32(min.z, pos.z);
    max.x = vkr_max_f32(max.x, pos.x);
    max.y = vkr_max_f32(max.y, pos.y);
    max.z = vkr_max_f32(max.z, pos.z);
  }

  *out_min = min;
  *out_max = max;
  *out_center = vec3_scale(vec3_add(min, max), 0.5f);
}

vkr_internal bool8_t vkr_mesh_loader_finalize_builder(
    VkrMeshLoaderState *state, VkrMeshLoaderSubsetBuilder *builder) {
  assert_log(state != NULL, "State is NULL");
  assert_log(builder != NULL, "Builder is NULL");

  if (builder->indices.length == 0 || builder->vertices.length == 0) {
    vector_clear_VkrVertex3d(&builder->vertices);
    vector_clear_uint32_t(&builder->indices);
    return true_v;
  }

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(state->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    log_error("MeshLoader: failed to acquire temporary scope");
    return false_v;
  }
  uint32_t index_count = (uint32_t)builder->indices.length;
  uint32_t vertex_count = (uint32_t)builder->vertices.length;

  VkrVertex3d *dedup_vertices = NULL;
  uint32_t dedup_vertex_count = 0;
  uint32_t *indices_copy = vkr_allocator_alloc(
      state->scratch_allocator, (uint64_t)index_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!indices_copy) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemCopy(indices_copy, builder->indices.data,
          (uint64_t)builder->indices.length * sizeof(uint32_t));

  if (!vkr_geometry_system_deduplicate_vertices(
          state->context->geometry_system, state->scratch_allocator,
          builder->vertices.data, vertex_count, indices_copy, index_count,
          &dedup_vertices, &dedup_vertex_count)) {
    vkr_allocator_free(state->scratch_allocator, indices_copy,
                       (uint64_t)index_count * sizeof(uint32_t),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    log_error("MeshLoader: deduplication failed for subset");
    return false_v;
  }

  vkr_geometry_system_generate_tangents(state->scratch_allocator,
                                        dedup_vertices, dedup_vertex_count,
                                        indices_copy, index_count);

  Vec3 min, max, center;
  vkr_mesh_loader_compute_bounds(dedup_vertices, dedup_vertex_count, &min, &max,
                                 &center);

  uint32_t vertex_base = (uint32_t)state->merged_vertices.length;
  uint32_t index_base = (uint32_t)state->merged_indices.length;
  if (state->merged_vertices.length + dedup_vertex_count > UINT32_MAX ||
      state->merged_indices.length + index_count > UINT32_MAX ||
      !vector_reserve_VkrVertex3d(&state->merged_vertices,
                                  state->merged_vertices.length +
                                      dedup_vertex_count) ||
      !vector_reserve_uint32_t(&state->merged_indices,
                               state->merged_indices.length + index_count) ||
      !vector_reserve_VkrMeshLoaderSubmeshRange(
          &state->merged_submeshes, state->merged_submeshes.length + 1u)) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemCopy(state->merged_vertices.data + vertex_base, dedup_vertices,
          (uint64_t)dedup_vertex_count * sizeof(*dedup_vertices));
  state->merged_vertices.length += dedup_vertex_count;
  for (uint32_t i = 0; i < index_count; ++i)
    state->merged_indices.data[state->merged_indices.length++] =
        indices_copy[i] + vertex_base;

  String8 material_path = {0};
  VkrMaterialHandle mat_handle = VKR_MATERIAL_HANDLE_INVALID;
  if (builder->material_name.str) {
    if (builder->material_is_path) {
      material_path =
          string8_duplicate(state->load_allocator, &builder->material_name);
    } else {
      vkr_mesh_loader_resolve_material(state, &builder->material_name,
                                       &material_path, &mat_handle);
    }
  }

  VkrMeshLoaderSubmeshRange range = {
      .range_id = (uint32_t)state->merged_submeshes.length,
      .first_index = index_base,
      .index_count = index_count,
      .vertex_offset = 0,
      .center = center,
      .min_extents = min,
      .max_extents = max,
      .material_name = string8_duplicate(state->load_allocator, &material_path),
      .shader_override =
          string8_duplicate(state->load_allocator, &builder->shader_override),
      .pipeline_domain = builder->pipeline_domain,
      .material_handle = mat_handle,
  };
  state->merged_submeshes.data[state->merged_submeshes.length++] = range;

  vkr_allocator_free(state->scratch_allocator, indices_copy,
                     (uint64_t)index_count * sizeof(uint32_t),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vector_clear_VkrVertex3d(&builder->vertices);
  vector_clear_uint32_t(&builder->indices);
  return true_v;
}

vkr_internal bool8_t
vkr_mesh_loader_finalize_all_buckets(VkrMeshLoaderState *state) {
  assert_log(state != NULL, "State is NULL");

  if (state->material_buckets.length == 0) {
    return true_v;
  }

  for (uint64_t i = 0; i < state->material_buckets.length; ++i) {
    VkrMeshLoaderMaterialBucket *bucket =
        vector_get_VkrMeshLoaderMaterialBucket(&state->material_buckets, i);
    if (!bucket)
      continue;
    if (!vkr_mesh_loader_finalize_builder(state, &bucket->builder)) {
      return false_v;
    }
  }

  vkr_mesh_loader_prepare_merged_buffer(state);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_push_face(
    VkrMeshLoaderState *state, VkrMeshLoaderSubsetBuilder *builder,
    uint32_t token_count, String8 *tokens) {
  assert_log(state != NULL, "State is NULL");
  assert_log(builder != NULL, "Builder is NULL");
  assert_log(tokens != NULL, "Tokens is NULL");

  if (token_count < 3)
    return true_v;

  const uint64_t index_count = (uint64_t)(token_count - 2u) * 3u;
  if (builder->vertices.length + token_count > UINT32_MAX ||
      builder->indices.length + index_count > UINT32_MAX ||
      !vector_reserve_VkrVertex3d(&builder->vertices,
                                  builder->vertices.length + token_count) ||
      !vector_reserve_uint32_t(&builder->indices,
                               builder->indices.length + index_count)) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  uint32_t first_index = (uint32_t)builder->vertices.length;

  for (uint32_t i = 0; i < token_count; ++i) {
    VkrMeshLoaderVertexRef ref = vkr_mesh_loader_parse_vertex_ref(&tokens[i]);
    uint32_t pos_idx = vkr_mesh_loader_fix_index(
        ref.position, (uint32_t)state->positions.length);
    uint32_t tex_idx = vkr_mesh_loader_fix_index(
        ref.texcoord, (uint32_t)state->texcoords.length);
    uint32_t norm_idx =
        vkr_mesh_loader_fix_index(ref.normal, (uint32_t)state->normals.length);

    VkrVertex3d vert = {0};
    vert.position =
        vkr_vertex_pack_vec3((pos_idx < state->positions.length)
                                 ? *vector_get_Vec3(&state->positions, pos_idx)
                                 : vec3_zero());
    vert.texcoord = (tex_idx < state->texcoords.length)
                        ? *vector_get_Vec2(&state->texcoords, tex_idx)
                        : vec2_zero();
    vert.normal =
        vkr_vertex_pack_vec3((norm_idx < state->normals.length)
                                 ? *vector_get_Vec3(&state->normals, norm_idx)
                                 : vec3_new(0.0f, 1.0f, 0.0f));
    vert.colour = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
    vert.tangent = vec4_zero();

    builder->vertices.data[builder->vertices.length++] = vert;
  }

  for (uint32_t tri = 0; tri < token_count - 2; ++tri) {
    builder->indices.data[builder->indices.length++] = first_index;
    builder->indices.data[builder->indices.length++] = first_index + tri + 1;
    builder->indices.data[builder->indices.length++] = first_index + tri + 2;
  }
  return true_v;
}

vkr_internal void vkr_mesh_loader_parse_next_line(String8 *file_str,
                                                  uint64_t *offset,
                                                  String8 *out_line) {
  uint64_t line_end = *offset;
  while (line_end < file_str->length && file_str->str[line_end] != '\n' &&
         file_str->str[line_end] != '\r') {
    line_end++;
  }
  *out_line = string8_substring(file_str, *offset, line_end);
  *offset = line_end;
  while (*offset < file_str->length &&
         (file_str->str[*offset] == '\n' || file_str->str[*offset] == '\r')) {
    (*offset)++;
  }
  string8_trim(out_line);
}

vkr_internal bool8_t vkr_mesh_loader_parse_vec3_line(String8 *line,
                                                     uint32_t prefix_len,
                                                     Vec3 *out_vec) {
  String8 coords = vkr_string8_trimmed_suffix(line, prefix_len);
  String8 tokens[3];
  uint32_t count = string8_split_whitespace(&coords, tokens, 3);
  if (count < 3)
    return false_v;

  float32_t x = 0, y = 0, z = 0;
  string8_to_f32(&tokens[0], &x);
  string8_to_f32(&tokens[1], &y);
  string8_to_f32(&tokens[2], &z);
  *out_vec = vec3_new(x, y, z);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_mtl(VkrMeshLoaderState *state,
                                               String8 rel_path) {
  assert_log(state != NULL, "State is NULL");

  if (!rel_path.str || rel_path.length == 0)
    return false_v;

  String8 full_path =
      file_path_join(state->load_allocator, state->source_dir, rel_path);
  if (!vkr_mesh_loader_capture_source_dependency(state, full_path)) {
    return false_v;
  }
  String8 file_str = {0};
  if (!vkr_mesh_loader_read_file_to_string(state->load_allocator, full_path,
                                           &file_str, NULL))
    return false_v;

  VkrMeshLoaderMaterialDef *current = NULL;
  uint64_t offset = 0;
  while (offset < file_str.length) {
    String8 line = {0};
    vkr_mesh_loader_parse_next_line(&file_str, &offset, &line);

    if (line.length == 0 || line.str[0] == '#')
      continue;

    if (vkr_string8_starts_with(&line, "newmtl")) {
      String8 mat_name = vkr_string8_trimmed_suffix(&line, 6);
      VkrMeshLoaderMaterialDef def = {
          .name = string8_duplicate(state->load_allocator, &mat_name),
          .shader_name = DEFAULT_SHADER,
          .diffuse_color = vec4_new(1, 1, 1, 1),
          .specular_color = vec4_new(1, 1, 1, 1),
          .emission_color = vec3_new(0, 0, 0),
          .shininess = 8.0f,
          .alpha_cutoff = 0.0f,
          .alpha_cutoff_set = false_v,
          .cutout = false_v,
      };
      if (!def.name.str ||
          !vector_push_VkrMeshLoaderMaterialDef(&state->materials, def)) {
        *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }
      current = vector_get_VkrMeshLoaderMaterialDef(
          &state->materials, state->materials.length - 1);
      continue;
    }

    if (!current)
      continue;

    Vec3 vec3_val;
    if (vkr_string8_starts_with(&line, "Kd")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->diffuse_color.x = vec3_val.x;
        current->diffuse_color.y = vec3_val.y;
        current->diffuse_color.z = vec3_val.z;
      }
    } else if (vkr_string8_starts_with(&line, "Ke")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->emission_color = vec3_val;
      }
    } else if (vkr_string8_starts_with(&line, "Ks")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->specular_color.x = vec3_val.x;
        current->specular_color.y = vec3_val.y;
        current->specular_color.z = vec3_val.z;
      }
    } else if (vkr_string8_starts_with(&line, "Ns")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 2);
      float32_t shininess = 0.0f;
      string8_to_f32(&value, &shininess);
      if (shininess > 0.0f)
        current->shininess = shininess;
    } else if (vkr_string8_starts_with(&line, "map_Kd")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->diffuse_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "map_d")) {
      current->cutout = true_v;
    } else if (vkr_string8_starts_with(&line, "map_Ks")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->specular_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "map_bump") ||
               vkr_string8_starts_with(&line, "bump")) {
      uint64_t idx = vkr_string8_starts_with(&line, "map_bump") ? 8 : 4;
      String8 value = vkr_string8_trimmed_suffix(&line, idx);
      current->normal_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "shader")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->shader_name = string8_duplicate(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "alpha_cutoff")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 12);
      float32_t cutoff = 0.0f;
      if (string8_to_f32(&value, &cutoff)) {
        current->alpha_cutoff = cutoff < 0.0f ? 0.0f : cutoff;
        current->alpha_cutoff_set = true_v;
      }
    } else if (vkr_string8_starts_with(&line, "cutout")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      bool8_t cutout = false_v;
      if (string8_to_bool(&value, &cutout)) {
        current->cutout = cutout;
      }
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_obj(VkrMeshLoaderState *state) {
  String8 file_str = {0};
  if (!vkr_mesh_loader_read_file_to_string(state->load_allocator,
                                           state->source_path, &file_str,
                                           state->out_error))
    return false_v;

  uint64_t offset = 0;
  while (offset < file_str.length) {
    String8 line = {0};
    vkr_mesh_loader_parse_next_line(&file_str, &offset, &line);

    if (line.length == 0 || line.str[0] == '#')
      continue;

    Vec3 vec3_val;
    if (vkr_string8_starts_with(&line, "v ")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 1, &vec3_val) &&
          !vector_push_Vec3(&state->positions, vec3_val)) {
        *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "vn")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val) &&
          !vector_push_Vec3(&state->normals, vec3_val)) {
        *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "vt")) {
      String8 coords = vkr_string8_trimmed_suffix(&line, 2);
      String8 tokens[2];
      if (string8_split_whitespace(&coords, tokens, 2) >= 2) {
        float32_t u = 0, v = 0;
        string8_to_f32(&tokens[0], &u);
        string8_to_f32(&tokens[1], &v);
        if (!vector_push_Vec2(&state->texcoords, vec2_new(u, v))) {
          *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
          return false_v;
        }
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "mtllib")) {
      String8 path = vkr_string8_trimmed_suffix(&line, 6);
      if (!vkr_mesh_loader_parse_mtl(state, path))
        return false_v;
      continue;
    }

    if (vkr_string8_starts_with(&line, "usemtl")) {
      String8 material_name = vkr_string8_trimmed_suffix(&line, 6);
      if (!vkr_mesh_loader_set_current_material(state, &material_name))
        return false_v;
      continue;
    }

    if (vkr_string8_starts_with(&line, "o ") ||
        vkr_string8_starts_with(&line, "g ")) {
      continue;
    }

    if (vkr_string8_starts_with(&line, "f ")) {
      String8 face = vkr_string8_trimmed_suffix(&line, 1);
      String8 tokens[64];
      uint32_t count = string8_split_whitespace(&face, tokens, 64);
      if (count >= 3) {
        VkrMeshLoaderSubsetBuilder *builder =
            vkr_mesh_loader_get_current_builder(state);
        if (!builder ||
            !vkr_mesh_loader_push_face(state, builder, count, tokens))
          return false_v;
      }
      continue;
    }
  }

  return vkr_mesh_loader_finalize_all_buckets(state);
}

vkr_internal bool8_t vkr_mesh_loader_accept_gltf_primitive(
    void *user_data, const VkrMeshLoaderGltfPrimitive *primitive) {
  if (!user_data || !primitive || primitive->vertex_count == 0 ||
      primitive->index_count == 0 || !primitive->vertices ||
      !primitive->indices) {
    return false_v;
  }

  VkrMeshLoaderState *state = (VkrMeshLoaderState *)user_data;
  uint32_t bucket_index =
      vkr_mesh_loader_find_material_bucket(state, &primitive->material_path);
  if (bucket_index == VKR_INVALID_ID) {
    bucket_index =
        vkr_mesh_loader_add_material_bucket(state, &primitive->material_path);
  }

  if (bucket_index == VKR_INVALID_ID) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  VkrMeshLoaderMaterialBucket *bucket = vector_get_VkrMeshLoaderMaterialBucket(
      &state->material_buckets, bucket_index);
  if (!bucket) {
    return false_v;
  }

  VkrMeshLoaderSubsetBuilder *builder = &bucket->builder;
  builder->material_name = bucket->material_name;
  builder->material_is_path = true_v;
  builder->pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD;
  builder->shader_override = (String8){0};

  const uint64_t vertex_count =
      builder->vertices.length + primitive->vertex_count;
  const uint64_t index_count = builder->indices.length + primitive->index_count;
  if (vertex_count > UINT32_MAX || index_count > UINT32_MAX ||
      !vector_reserve_VkrVertex3d(&builder->vertices, vertex_count) ||
      !vector_reserve_uint32_t(&builder->indices, index_count)) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  const uint32_t index_base = (uint32_t)builder->vertices.length;
  MemCopy(builder->vertices.data + index_base, primitive->vertices,
          (uint64_t)primitive->vertex_count * sizeof(*primitive->vertices));
  builder->vertices.length = vertex_count;
  for (uint32_t i = 0; i < primitive->index_count; ++i)
    builder->indices.data[builder->indices.length++] =
        primitive->indices[i] + index_base;

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_source(VkrMeshLoaderState *state) {
  if (!state) {
    return false_v;
  }

  vector_clear_VkrMeshSourceDependency(&state->source_dependencies);
  if (!vkr_mesh_loader_capture_source_dependency(state, state->source_path)) {
    if (*state->out_error == VKR_RENDERER_ERROR_NONE)
      *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  String8 obj_ext = string8_lit("obj");
  if (string8_equalsi(&state->source_extension, &obj_ext)) {
    return vkr_mesh_loader_parse_obj(state);
  }

  String8 gltf_ext = string8_lit("gltf");
  String8 glb_ext = string8_lit("glb");
  if (string8_equalsi(&state->source_extension, &gltf_ext) ||
      string8_equalsi(&state->source_extension, &glb_ext)) {
    Vector_String8 dependency_paths = {.allocator = state->load_allocator};
    VkrMeshLoaderGltfParseInfo parse_info = {
        .source_path = state->source_path,
        .source_dir = state->source_dir,
        .source_stem = state->source_stem,
        .load_allocator = state->load_allocator,
        .scratch_allocator = state->scratch_allocator,
        .out_error = state->out_error,
        .on_primitive = vkr_mesh_loader_accept_gltf_primitive,
        .user_data = state,
        .out_dependency_paths = &dependency_paths,
        .out_generated_material_paths = &dependency_paths,
    };
    if (!vkr_mesh_loader_gltf_parse(&parse_info)) {
      return false_v;
    }
    if (!vkr_mesh_loader_finalize_all_buckets(state)) {
      return false_v;
    }

    return vkr_mesh_loader_collect_source_dependencies(state,
                                                       &dependency_paths);
  }

  if (state->out_error) {
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  log_error("MeshLoader: unsupported mesh extension '%.*s'",
            (int32_t)state->source_extension.length,
            state->source_extension.str);
  return false_v;
}

bool8_t vkr_mesh_cook_source(String8 source_path, String8 output_path,
                             VkrAllocator *source_allocator,
                             VkrAllocator *scratch_allocator,
                             VkrMeshCookStats *out_stats,
                             VkrRendererError *out_error) {
  if (!source_path.str || source_path.length == 0 || !output_path.str ||
      output_path.length == 0 || !source_allocator || !scratch_allocator ||
      source_allocator->ctx == scratch_allocator->ctx || !out_error) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  if (out_stats) {
    *out_stats = (VkrMeshCookStats){0};
  }
  String8 output_extension =
      vkr_mesh_loader_get_extension(scratch_allocator, output_path);
  String8 vkb_extension = string8_lit("vkb");
  if (!string8_equalsi(&output_extension, &vkb_extension)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrGeometrySystem geometry_system = {0};
  VkrMeshLoaderContext context = {.geometry_system = &geometry_system};
  VkrMeshLoaderState state = {0};
  if (!vkr_mesh_loader_state_create(&context, source_allocator,
                                    scratch_allocator, scratch_allocator,
                                    source_path, out_error, &state))
    return false_v;
  if (!vkr_mesh_loader_parse_source(&state) ||
      state.merged_buffer.vertex_count == 0 ||
      state.merged_buffer.index_count == 0 ||
      state.merged_submeshes.length == 0 ||
      state.merged_submeshes.length > UINT32_MAX ||
      state.source_dependencies.length == 0 ||
      state.source_dependencies.length > UINT32_MAX) {
    if (*out_error == VKR_RENDERER_ERROR_NONE) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }

  uint32_t dependency_count = (uint32_t)state.source_dependencies.length;
  String8 *dependency_paths = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)dependency_count * sizeof(String8),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dependency_paths) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  for (uint32_t i = 0; i < dependency_count; ++i) {
    dependency_paths[i] = state.source_dependencies.data[i].path;
  }

  VkrMeshCookedEncodeInfo encode_info = {
      .source_path = source_path,
      .dependency_paths = dependency_paths,
      .dependency_count = dependency_count,
      .mesh_buffer = state.merged_buffer,
      .ranges = state.merged_submeshes.data,
      .range_count = (uint32_t)state.merged_submeshes.length,
      .budgets = vkr_packed_geometry_default_budgets(),
  };
  uint8_t *artifact = NULL;
  uint64_t artifact_size = 0;
  if (!vkr_mesh_cooked_encode(scratch_allocator, &encode_info, &artifact,
                              &artifact_size) ||
      !vkr_mesh_cooked_write_atomic(scratch_allocator, output_path, artifact,
                                    artifact_size)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  if (out_stats) {
    out_stats->cooked_bytes = artifact_size;
    out_stats->decoded_bytes =
        (uint64_t)state.merged_buffer.vertex_count *
            sizeof(VkrPackedStaticVertex) +
        (uint64_t)state.merged_buffer.index_count * sizeof(uint32_t) +
        (uint64_t)state.merged_submeshes.length *
            sizeof(VkrGpuGeometryDecodeRecord);
    out_stats->vertex_count = state.merged_buffer.vertex_count;
    out_stats->index_count = state.merged_buffer.index_count;
    out_stats->range_count = (uint32_t)state.merged_submeshes.length;
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_can_load(VkrResourceLoader *self,
                                              String8 name) {
  (void)self;
  if (!name.str || name.length == 0)
    return false_v;

  for (uint64_t i = name.length; i > 0; --i) {
    if (name.str[i - 1] == '.') {
      String8 ext = string8_substring(&name, i, name.length);
      String8 obj_ext = string8_lit("obj");
      String8 gltf_ext = string8_lit("gltf");
      String8 glb_ext = string8_lit("glb");
      String8 vkb_ext = string8_lit("vkb");
      return string8_equalsi(&ext, &obj_ext) ||
             string8_equalsi(&ext, &gltf_ext) ||
             string8_equalsi(&ext, &glb_ext) || string8_equalsi(&ext, &vkb_ext);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_mesh_load_job_run_inner(
    VkrMeshLoadJobPayload *job, VkrAllocator *parse_allocator,
    VkrAllocator *scratch_allocator) {
  *job->success = false_v;
  *job->error = VKR_RENDERER_ERROR_NONE;

  VkrMeshLoaderState state = {0};
  if (!vkr_mesh_loader_state_create(job->context, parse_allocator,
                                    scratch_allocator, scratch_allocator,
                                    job->mesh_path, job->error, &state))
    return false_v;

  String8 vkb_ext = string8_lit("vkb");
  if (string8_equalsi(&state.source_extension, &vkb_ext)) {
    FilePath file_path =
        file_path_create(string8_cstr(&state.source_path), scratch_allocator,
                         vkr_mesh_loader_path_is_absolute(state.source_path)
                             ? FILE_PATH_TYPE_ABSOLUTE
                             : FILE_PATH_TYPE_RELATIVE);
    FileMode mode = bitset8_create();
    bitset8_set(&mode, FILE_MODE_READ);
    bitset8_set(&mode, FILE_MODE_BINARY);
    FileHandle file = {0};
    uint8_t *artifact = NULL;
    uint64_t artifact_size = 0;
    if (file_open(&file_path, mode, &file) != FILE_ERROR_NONE ||
        file_read_all(&file, scratch_allocator, &artifact, &artifact_size) !=
            FILE_ERROR_NONE) {
      file_close(&file);
      *job->error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }
    file_close(&file);

    VkrMeshCookedDecoded decoded = {0};
    if (!vkr_mesh_cooked_decode(job->result_allocator, scratch_allocator,
                                artifact, artifact_size, &decoded)) {
      *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      log_error("MeshLoader: invalid cooked mesh '%.*s'",
                (int32_t)job->mesh_path.length, job->mesh_path.str);
      return false_v;
    }
    job->result->source_path =
        string8_duplicate(job->result_allocator, &job->mesh_path);
    job->result->root_transform = vkr_transform_identity();
    job->result->has_mesh_buffer = true_v;
    job->result->mesh_buffer = decoded.mesh_buffer;
    job->result->submeshes = decoded.ranges;
    job->result->subsets = (Array_VkrMeshLoaderSubset){0};
    job->result->load_metrics = (VkrMeshLoadMetrics){
        .source_bytes = decoded.source_bytes,
        .cooked_bytes = decoded.cooked_bytes,
        .decoded_bytes = decoded.decoded_bytes,
        .upload_bytes = decoded.decoded_bytes,
        .vertex_count = decoded.mesh_buffer.vertex_count,
        .index_count = decoded.mesh_buffer.index_count,
        .range_count = (uint32_t)decoded.ranges.length,
        .preparation = VKR_MESH_PREPARATION_COOKED,
    };
    if (!vkr_mesh_loader_analyze_buffer(
            &decoded.mesh_buffer, decoded.ranges.data,
            (uint32_t)decoded.ranges.length, scratch_allocator, true_v,
            &job->result->load_metrics)) {
      *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return false_v;
    }
    vkr_mesh_loader_complete_cooked_analysis(&job->result->load_metrics);
    *job->success = true_v;
    return true_v;
  }

  if (!vkr_mesh_loader_parse_source(&state)) {
    return false_v;
  }

  const bool8_t has_mesh_buffer = state.merged_buffer.vertex_count > 0 &&
                                  state.merged_buffer.index_count > 0 &&
                                  state.merged_submeshes.length > 0;
  if (!has_mesh_buffer) {
    *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrMeshLoadMetrics load_metrics = {
      .source_bytes = vkr_mesh_loader_source_dependency_bytes(&state),
      .vertex_count = state.merged_buffer.vertex_count,
      .index_count = state.merged_buffer.index_count,
      .range_count = (uint32_t)state.merged_submeshes.length,
      .preparation = VKR_MESH_PREPARATION_SOURCE,
      .runtime_optimized = true_v,
  };
  if (!vkr_mesh_loader_analyze_buffer(
          &state.merged_buffer, state.merged_submeshes.data,
          (uint32_t)state.merged_submeshes.length, scratch_allocator, true_v,
          &load_metrics)) {
    *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrMeshLoaderBuffer result_buffer = {0};
  Array_VkrMeshLoaderSubmeshRange result_ranges = {0};
  if (!vkr_mesh_loader_commit_merged_buffer(&state, job->result_allocator,
                                            scratch_allocator, &result_buffer,
                                            &result_ranges)) {
    *job->error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  const uint64_t decoded_bytes =
      (uint64_t)result_buffer.vertex_count * result_buffer.vertex_size +
      (uint64_t)result_buffer.index_count * result_buffer.index_size +
      (uint64_t)result_buffer.decode_count * sizeof(VkrGpuGeometryDecodeRecord);
  load_metrics.decoded_bytes = decoded_bytes;
  load_metrics.upload_bytes = decoded_bytes;
  load_metrics.vertex_count = result_buffer.vertex_count;
  if (!vkr_mesh_loader_analyze_buffer(
          &result_buffer, result_ranges.data, (uint32_t)result_ranges.length,
          scratch_allocator, false_v, &load_metrics)) {
    *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  job->result->source_path =
      string8_duplicate(job->result_allocator, &job->mesh_path);
  job->result->root_transform = vkr_transform_identity();
  job->result->has_mesh_buffer = true_v;
  job->result->mesh_buffer = result_buffer;
  job->result->submeshes = result_ranges;
  job->result->subsets = (Array_VkrMeshLoaderSubset){0};
  job->result->load_metrics = load_metrics;

  *job->success = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_load_job_run(VkrJobContext *ctx, void *payload) {
  VkrMeshLoadJobPayload *job = payload;
  if (!ctx || !ctx->allocator || !job) {
    return false_v;
  }
  Arena *parse_arena = arena_create(GB(4), MB(8));
  if (!parse_arena) {
    *job->success = false_v;
    *job->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  Arena *scratch_arena = arena_create(GB(4), MB(8));
  if (!scratch_arena) {
    arena_destroy(parse_arena);
    *job->success = false_v;
    *job->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  VkrAllocator parse_allocator = {.ctx = parse_arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  vkr_allocator_arena(&parse_allocator);
  vkr_allocator_arena(&scratch_allocator);
  const bool8_t result =
      vkr_mesh_load_job_run_inner(job, &parse_allocator, &scratch_allocator);
  vkr_allocator_release_global_accounting(&scratch_allocator);
  vkr_allocator_release_global_accounting(&parse_allocator);
  arena_destroy(scratch_arena);
  arena_destroy(parse_arena);
  return result;
}

vkr_internal void
vkr_mesh_loader_destroy_result(VkrMeshLoaderContext *context,
                               VkrMeshLoaderResult *result,
                               bool8_t release_material_handles) {
  if (!context || !result) {
    return;
  }

  if (release_material_handles && context->material_system) {
    if (result->has_mesh_buffer && result->submeshes.data) {
      for (uint64_t i = 0; i < result->submeshes.length; i++) {
        VkrMeshLoaderSubmeshRange *range = &result->submeshes.data[i];
        if (range->material_handle.id != 0) {
          vkr_material_system_release(context->material_system,
                                      range->material_handle);
          range->material_handle = VKR_MATERIAL_HANDLE_INVALID;
        }
      }
    } else if (result->subsets.data) {
      for (uint64_t i = 0; i < result->subsets.length; i++) {
        VkrMeshLoaderSubset *subset = &result->subsets.data[i];
        if (subset->material_handle.id != 0) {
          vkr_material_system_release(context->material_system,
                                      subset->material_handle);
          subset->material_handle = VKR_MATERIAL_HANDLE_INVALID;
        }
      }
    }
  }

  void *cached_pool_chunk = result->pool_chunk;
  result->pool_chunk = NULL;

  if (result->arena) {
    vkr_allocator_release_global_accounting(&result->allocator);
    arena_destroy(result->arena);
    result->arena = NULL;
  }

  if (cached_pool_chunk)
    vkr_arena_pool_release(context->arena_pool, cached_pool_chunk);
}

vkr_internal void vkr_mesh_loader_cleanup_arenas(VkrMeshLoaderResult **results,
                                                 Arena **arenas,
                                                 void **pool_chunks,
                                                 uint32_t count,
                                                 VkrArenaPool *arena_pool) {
  for (uint32_t i = 0; i < count; i++) {
    if (results[i])
      vkr_allocator_release_global_accounting(&results[i]->allocator);
    if (arenas[i])
      arena_destroy(arenas[i]);
    if (pool_chunks[i] && arena_pool)
      vkr_arena_pool_release(arena_pool, pool_chunks[i]);
  }
}

vkr_internal void vkr_mesh_loader_set_all_errors(VkrMeshBatchResult *results,
                                                 uint32_t count,
                                                 VkrRendererError error) {
  for (uint32_t i = 0; i < count; i++)
    results[i].error = error;
}

vkr_internal uint32_t vkr_mesh_loader_load_batch(
    VkrMeshLoaderContext *context, const String8 *mesh_paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrMeshBatchResult *out_results) {
  assert_log(context != NULL, "Context is NULL");
  assert_log(mesh_paths != NULL, "Mesh paths is NULL");
  assert_log(out_results != NULL, "Out results is NULL");

  if (count == 0)
    return 0;

  for (uint32_t i = 0; i < count; i++) {
    out_results[i].result = NULL;
    out_results[i].error = VKR_RENDERER_ERROR_NONE;
    out_results[i].success = false_v;
  }

  VkrJobSystem *job_sys = context->job_system;
  (void)job_sys;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_results[i].error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // Allocate per-mesh structures
  VkrMeshLoaderResult **results = (VkrMeshLoaderResult **)vkr_allocator_alloc(
      temp_alloc, sizeof(VkrMeshLoaderResult *) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  Arena **result_arenas = (Arena **)vkr_allocator_alloc(
      temp_alloc, sizeof(Arena *) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void **pool_chunks = (void **)vkr_allocator_alloc(
      temp_alloc, sizeof(void *) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrAllocator *result_allocators = vkr_allocator_alloc(
      temp_alloc, sizeof(VkrAllocator) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *errors =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrRendererError) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *success = vkr_allocator_alloc(temp_alloc, sizeof(bool8_t) * count,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshLoadJobPayload *payloads =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMeshLoadJobPayload) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!results || !result_arenas || !pool_chunks || !result_allocators ||
      !errors || !success || !payloads) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_set_all_errors(out_results, count,
                                   VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return 0;
  }

  VkrArenaPool *arena_pool = context->arena_pool;
  if (!arena_pool || !arena_pool->initialized) {
    log_error("Mesh loader requires arena_pool to be initialized");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_set_all_errors(out_results, count,
                                   VKR_RENDERER_ERROR_INITIALIZATION_FAILED);
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    pool_chunks[i] = vkr_arena_pool_acquire(arena_pool);
    if (!pool_chunks[i]) {
      log_error("Arena pool exhausted at mesh %u/%u", i, count);
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    result_arenas[i] =
        arena_create_from_buffer(pool_chunks[i], arena_pool->chunk_size);
    if (!result_arenas[i]) {
      vkr_arena_pool_release(arena_pool, pool_chunks[i]);
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    result_allocators[i].ctx = result_arenas[i];
    vkr_allocator_arena(&result_allocators[i]);

    results[i] =
        vkr_allocator_alloc(&result_allocators[i], sizeof(VkrMeshLoaderResult),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!results[i]) {
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i + 1,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    MemZero(results[i], sizeof(VkrMeshLoaderResult));
    results[i]->arena = result_arenas[i];
    results[i]->pool_chunk = pool_chunks[i];
    results[i]->allocator = result_allocators[i];

    errors[i] = VKR_RENDERER_ERROR_NONE;
    success[i] = false_v;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!mesh_paths[i].str || mesh_paths[i].length == 0)
      continue;

    payloads[i] = (VkrMeshLoadJobPayload){
        .mesh_path = mesh_paths[i],
        .context = context,
        .result_allocator = &results[i]->allocator,
        .result = results[i],
        .error = &errors[i],
        .success = &success[i],
    };

    VkrJobContext fake_ctx = {.system = NULL,
                              .worker_index = 0,
                              .thread_id = 0,
                              .allocator = temp_alloc,
                              .scope = temp_scope};
    vkr_mesh_load_job_run(&fake_ctx, &payloads[i]);
  }

  uint32_t total_materials = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!success[i] || !results[i])
      continue;

    if (results[i]->has_mesh_buffer) {
      for (uint64_t j = 0; j < results[i]->submeshes.length; j++) {
        VkrMeshLoaderSubmeshRange *range = &results[i]->submeshes.data[j];
        if (range->material_name.str && range->material_name.length > 0)
          total_materials++;
      }
    } else {
      for (uint64_t j = 0; j < results[i]->subsets.length; j++) {
        VkrMeshLoaderSubset *subset = &results[i]->subsets.data[j];
        if (subset->material_name.str && subset->material_name.length > 0)
          total_materials++;
      }
    }
  }

  log_debug("Mesh batch: %u meshes loaded, %u total materials to load", count,
            total_materials);

  if (total_materials > 0) {
    String8 *all_material_paths =
        vkr_allocator_alloc(temp_alloc, sizeof(String8) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrMaterialHandle *all_material_handles = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrMaterialHandle) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrRendererError *all_material_errors = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrRendererError) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *material_mesh_index =
        vkr_allocator_alloc(temp_alloc, sizeof(uint32_t) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *material_subset_index =
        vkr_allocator_alloc(temp_alloc, sizeof(uint32_t) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    if (!all_material_paths || !all_material_handles || !all_material_errors ||
        !material_mesh_index || !material_subset_index) {
      for (uint32_t i = 0; i < count; i++) {
        if (success[i] && results[i]) {
          out_results[i].result = results[i];
          out_results[i].error = VKR_RENDERER_ERROR_NONE;
          out_results[i].success = true_v;
        } else {
          out_results[i].error = errors[i];
        }
      }
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return count;
    }

    uint32_t mat_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (!success[i] || !results[i])
        continue;

      if (results[i]->has_mesh_buffer) {
        for (uint64_t j = 0; j < results[i]->submeshes.length; j++) {
          VkrMeshLoaderSubmeshRange *range = &results[i]->submeshes.data[j];
          if (range->material_name.str && range->material_name.length > 0) {
            all_material_paths[mat_idx] = range->material_name;
            material_mesh_index[mat_idx] = i;
            material_subset_index[mat_idx] = (uint32_t)j;
            mat_idx++;
          }
        }
      } else {
        for (uint64_t j = 0; j < results[i]->subsets.length; j++) {
          VkrMeshLoaderSubset *subset = &results[i]->subsets.data[j];
          if (subset->material_name.str && subset->material_name.length > 0) {
            all_material_paths[mat_idx] = subset->material_name;
            material_mesh_index[mat_idx] = i;
            material_subset_index[mat_idx] = (uint32_t)j;
            mat_idx++;
          }
        }
      }
    }

    VkrResourceHandleInfo *material_handle_infos = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrResourceHandleInfo) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!material_handle_infos) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    uint32_t materials_loaded = vkr_resource_system_load_batch_sync(
        VKR_RESOURCE_TYPE_MATERIAL, all_material_paths, total_materials,
        temp_alloc, material_handle_infos, all_material_errors);

    log_debug("Mesh batch: %u/%u materials loaded", materials_loaded,
              total_materials);

    for (uint32_t m = 0; m < total_materials; m++) {
      VkrMaterialHandle mat_handle = VKR_MATERIAL_HANDLE_INVALID;
      if (material_handle_infos[m].type == VKR_RESOURCE_TYPE_MATERIAL) {
        mat_handle = material_handle_infos[m].as.material;
        all_material_handles[m] = mat_handle;
      } else {
        all_material_handles[m] = VKR_MATERIAL_HANDLE_INVALID;
      }

      if (mat_handle.id != 0) {
        uint32_t mesh_idx = material_mesh_index[m];
        uint32_t subset_idx = material_subset_index[m];
        if (results[mesh_idx]->has_mesh_buffer) {
          VkrMeshLoaderSubmeshRange *range =
              &results[mesh_idx]->submeshes.data[subset_idx];
          range->material_handle = mat_handle;
        } else {
          VkrMeshLoaderSubset *subset =
              &results[mesh_idx]->subsets.data[subset_idx];
          subset->material_handle = mat_handle;
        }
        vkr_material_system_add_ref(context->material_system, mat_handle);
      }
    }
  }

  uint32_t loaded_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (success[i] && results[i]) {
      out_results[i].result = results[i];
      out_results[i].error = VKR_RENDERER_ERROR_NONE;
      out_results[i].success = true_v;
      loaded_count++;
    } else {
      out_results[i].error = errors[i];
      out_results[i].success = false_v;
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  log_debug("Mesh batch complete: %u/%u meshes loaded successfully",
            loaded_count, count);
  return loaded_count;
}

vkr_internal bool8_t vkr_mesh_loader_load(VkrResourceLoader *self, String8 name,
                                          VkrAllocator *temp_alloc,
                                          VkrResourceHandleInfo *out_handle,
                                          VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;

  VkrMeshBatchResult batch_result = {0};
  uint32_t loaded =
      vkr_mesh_loader_load_batch(context, &name, 1, temp_alloc, &batch_result);

  if (loaded == 0 || !batch_result.success) {
    *out_error = batch_result.error != VKR_RENDERER_ERROR_NONE
                     ? batch_result.error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = batch_result.result;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_payload != NULL, "Out payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_payload = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  if (!context || !context->arena_pool || !context->arena_pool->initialized) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }

  VkrMeshLoaderAsyncPayload *payload =
      (VkrMeshLoaderAsyncPayload *)vkr_allocator_alloc_ts(
          &context->async_allocator, sizeof(*payload),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, context->async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));
  payload->context = context;

  void *pool_chunk = vkr_arena_pool_acquire(context->arena_pool);
  if (!pool_chunk) {
    vkr_allocator_free_ts(&context->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          context->async_mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  Arena *result_arena =
      arena_create_from_buffer(pool_chunk, context->arena_pool->chunk_size);
  if (!result_arena) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
    vkr_allocator_free_ts(&context->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          context->async_mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrAllocator result_allocator = {.ctx = result_arena};
  vkr_allocator_arena(&result_allocator);
  VkrMeshLoaderResult *result = vkr_allocator_alloc(
      &result_allocator, sizeof(*result), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    arena_destroy(result_arena);
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
    vkr_allocator_free_ts(&context->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          context->async_mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(result, sizeof(*result));
  result->arena = result_arena;
  result->pool_chunk = pool_chunk;
  result->allocator = result_allocator;
  payload->result = result;

  bool8_t job_success = false_v;
  VkrRendererError job_error = VKR_RENDERER_ERROR_NONE;
  VkrMeshLoadJobPayload job_payload = {
      .mesh_path = name,
      .context = context,
      .result_allocator = &result->allocator,
      .result = result,
      .error = &job_error,
      .success = &job_success,
  };

  VkrAllocatorScope parse_scope = vkr_allocator_begin_scope(temp_alloc);
  VkrJobContext fake_ctx = {.system = NULL,
                            .worker_index = 0,
                            .thread_id = 0,
                            .allocator = temp_alloc,
                            .scope = parse_scope};
  vkr_mesh_load_job_run(&fake_ctx, &job_payload);
  vkr_allocator_end_scope(&parse_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!job_success) {
    const VkrRendererError effective_error =
        job_error != VKR_RENDERER_ERROR_NONE
            ? job_error
            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    log_error("MeshLoader: failed to prepare '%.*s' (error %u)",
              (int32_t)name.length, name.str, (uint32_t)effective_error);
    vkr_mesh_loader_destroy_result(context, result, false_v);
    payload->result = NULL;
    vkr_allocator_free_ts(&context->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          context->async_mutex);
    *out_error = effective_error;
    return false_v;
  }

  uint32_t dependency_count = 0;
  if (result->has_mesh_buffer) {
    for (uint64_t i = 0; i < result->submeshes.length; i++) {
      VkrMeshLoaderSubmeshRange *range = &result->submeshes.data[i];
      if (range->material_name.str && range->material_name.length > 0) {
        dependency_count++;
      }
    }
  } else {
    for (uint64_t i = 0; i < result->subsets.length; i++) {
      VkrMeshLoaderSubset *subset = &result->subsets.data[i];
      if (subset->material_name.str && subset->material_name.length > 0) {
        dependency_count++;
      }
    }
  }

  payload->dependency_count = dependency_count;
  if (dependency_count > 0) {
    uint64_t dep_size =
        sizeof(VkrMeshLoaderAsyncMaterialDependency) * dependency_count;
    payload->dependencies =
        (VkrMeshLoaderAsyncMaterialDependency *)vkr_allocator_alloc_ts(
            &context->async_allocator, dep_size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY,
            context->async_mutex);
    if (!payload->dependencies) {
      vkr_mesh_loader_destroy_result(context, result, false_v);
      payload->result = NULL;
      vkr_allocator_free_ts(&context->async_allocator, payload,
                            sizeof(*payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                            context->async_mutex);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    MemZero(payload->dependencies,
            sizeof(VkrMeshLoaderAsyncMaterialDependency) * dependency_count);

    uint32_t dep_index = 0;
    if (result->has_mesh_buffer) {
      for (uint64_t i = 0; i < result->submeshes.length; i++) {
        VkrMeshLoaderSubmeshRange *range = &result->submeshes.data[i];
        if (!range->material_name.str || range->material_name.length == 0) {
          continue;
        }

        VkrMeshLoaderAsyncMaterialDependency *dep =
            &payload->dependencies[dep_index++];
        dep->use_merged = true_v;
        dep->index = (uint32_t)i;
        dep->material_path = range->material_name;
        MemZero(&dep->request_info, sizeof(dep->request_info));
        dep->request_info.type = VKR_RESOURCE_TYPE_MATERIAL;
        dep->request_info.loader_id = VKR_INVALID_ID;
        dep->request_info.load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
        dep->request_info.last_error = VKR_RENDERER_ERROR_NONE;

        VkrRendererError material_error = VKR_RENDERER_ERROR_NONE;
        (void)vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                                       dep->material_path, temp_alloc,
                                       &dep->request_info, &material_error);
      }
    } else {
      for (uint64_t i = 0; i < result->subsets.length; i++) {
        VkrMeshLoaderSubset *subset = &result->subsets.data[i];
        if (!subset->material_name.str || subset->material_name.length == 0) {
          continue;
        }

        VkrMeshLoaderAsyncMaterialDependency *dep =
            &payload->dependencies[dep_index++];
        dep->use_merged = false_v;
        dep->index = (uint32_t)i;
        dep->material_path = subset->material_name;
        MemZero(&dep->request_info, sizeof(dep->request_info));
        dep->request_info.type = VKR_RESOURCE_TYPE_MATERIAL;
        dep->request_info.loader_id = VKR_INVALID_ID;
        dep->request_info.load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
        dep->request_info.last_error = VKR_RENDERER_ERROR_NONE;

        VkrRendererError material_error = VKR_RENDERER_ERROR_NONE;
        (void)vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                                       dep->material_path, temp_alloc,
                                       &dep->request_info, &material_error);
      }
    }
  }

  payload->ownership_transferred = false_v;
  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMeshLoaderAsyncPayload *async_payload =
      (VkrMeshLoaderAsyncPayload *)payload;
  VkrMeshLoaderContext *context = async_payload->context;
  if (!context || !async_payload->result) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  for (uint32_t i = 0; i < async_payload->dependency_count; ++i) {
    VkrMeshLoaderAsyncMaterialDependency *dep = &async_payload->dependencies[i];
    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&dep->request_info, &dependency_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
  }

  for (uint32_t i = 0; i < async_payload->dependency_count; ++i) {
    VkrMeshLoaderAsyncMaterialDependency *dep = &async_payload->dependencies[i];
    VkrResourceHandleInfo resolved_info = {0};
    if (!vkr_resource_system_try_get_resolved(&dep->request_info,
                                              &resolved_info)) {
      continue;
    }
    if (resolved_info.type != VKR_RESOURCE_TYPE_MATERIAL ||
        resolved_info.as.material.id == 0) {
      continue;
    }

    if (dep->use_merged) {
      if (dep->index >= async_payload->result->submeshes.length) {
        continue;
      }
      VkrMeshLoaderSubmeshRange *range =
          &async_payload->result->submeshes.data[dep->index];
      if (range->material_handle.id == 0) {
        range->material_handle = resolved_info.as.material;
        if (context->material_system) {
          vkr_material_system_add_ref(context->material_system,
                                      range->material_handle);
        }
      }
    } else {
      if (dep->index >= async_payload->result->subsets.length) {
        continue;
      }
      VkrMeshLoaderSubset *subset =
          &async_payload->result->subsets.data[dep->index];
      if (subset->material_handle.id == 0) {
        subset->material_handle = resolved_info.as.material;
        if (context->material_system) {
          vkr_material_system_add_ref(context->material_system,
                                      subset->material_handle);
        }
      }
    }
  }

  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = async_payload->result;
  *out_error = VKR_RENDERER_ERROR_NONE;
  async_payload->ownership_transferred = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  (void)payload;
  assert_log(out_cost != NULL, "Out cost is NULL");

  // Mesh finalize publishes CPU-side payload and resolves dependencies.
  // GPU uploads happen later when mesh assets are acquired by mesh systems.
  MemZero(out_cost, sizeof(*out_cost));
  return true_v;
}

vkr_internal void vkr_mesh_loader_release_async_payload(VkrResourceLoader *self,
                                                        void *payload) {
  assert_log(self != NULL, "Self is NULL");
  if (!payload) {
    return;
  }

  VkrMeshLoaderAsyncPayload *async_payload =
      (VkrMeshLoaderAsyncPayload *)payload;

  for (uint32_t i = 0; i < async_payload->dependency_count; ++i) {
    VkrMeshLoaderAsyncMaterialDependency *dep = &async_payload->dependencies[i];
    if (dep->request_info.request_id != 0 && dep->material_path.str &&
        dep->material_path.length > 0) {
      vkr_resource_system_unload(&dep->request_info, dep->material_path);
    }
  }

  if (!async_payload->ownership_transferred && async_payload->result) {
    vkr_mesh_loader_destroy_result(async_payload->context,
                                   async_payload->result, true_v);
    async_payload->result = NULL;
  }

  if (async_payload->dependencies) {
    vkr_allocator_free_ts(
        &async_payload->context->async_allocator, async_payload->dependencies,
        sizeof(VkrMeshLoaderAsyncMaterialDependency) *
            async_payload->dependency_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, async_payload->context->async_mutex);
    async_payload->dependencies = NULL;
  }

  vkr_allocator_free_ts(&async_payload->context->async_allocator, async_payload,
                        sizeof(*async_payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        async_payload->context->async_mutex);
}

vkr_internal void vkr_mesh_loader_unload(VkrResourceLoader *self,
                                         const VkrResourceHandleInfo *handle,
                                         String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  (void)name;

  if (handle->type != VKR_RESOURCE_TYPE_MESH) {
    log_warn("MeshLoader: attempted to unload non-mesh resource");
    return;
  }

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  VkrMeshLoaderResult *result = handle->as.mesh;

  if (!result)
    return;

  vkr_mesh_loader_destroy_result(context, result, true_v);
}

vkr_internal uint32_t vkr_mesh_loader_batch_load(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;

  VkrMeshBatchResult *batch_results =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMeshBatchResult) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!batch_results) {
    for (uint32_t i = 0; i < count; i++)
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return 0;
  }

  uint32_t loaded = vkr_mesh_loader_load_batch(context, paths, count,
                                               temp_alloc, batch_results);

  for (uint32_t i = 0; i < count; i++) {
    if (batch_results[i].success && batch_results[i].result) {
      out_handles[i].type = VKR_RESOURCE_TYPE_MESH;
      out_handles[i].loader_id = self->id;
      out_handles[i].as.mesh = batch_results[i].result;
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
    } else {
      out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
      out_handles[i].loader_id = VKR_INVALID_ID;
      out_errors[i] = batch_results[i].error;
    }
  }

  return loaded;
}

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MESH;
  loader.resource_system = context;
  loader.can_load = vkr_mesh_loader_can_load;
  loader.load = vkr_mesh_loader_load;
  loader.prepare_async = vkr_mesh_loader_prepare_async;
  loader.finalize_async = vkr_mesh_loader_finalize_async;
  loader.estimate_async_finalize_cost =
      vkr_mesh_loader_estimate_async_finalize_cost;
  loader.release_async_payload = vkr_mesh_loader_release_async_payload;
  loader.unload = vkr_mesh_loader_unload;
  loader.batch_load = vkr_mesh_loader_batch_load;
  return loader;
}
