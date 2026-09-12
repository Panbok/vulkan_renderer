#include "renderer/resources/loaders/mesh_loader.h"
#include "filesystem/vkr_asset_path.h"
#include "core/vkr_json.h"

#include "assets/vkr_mesh_cooked.h"
#include "assets/vkr_mesh_decode.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

typedef struct VkrMeshLoaderAsyncMaterialDependency {
  uint32_t range_index;
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

vkr_internal bool8_t vkr_mesh_loader_has_vkb_extension(String8 name) {
  for (uint64_t index = name.length; index > 0u; --index) {
    if (name.str[index - 1u] == '.') {
      const String8 extension = string8_substring(&name, index, name.length);
      const String8 vkb_extension = string8_lit("vkb");
      return string8_equalsi(&extension, &vkb_extension);
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_mesh_loader_analyze_cooked(
    const VkrMeshLoaderBuffer *buffer, const VkrGeometryUploadRange *ranges,
    uint32_t range_count, VkrAllocator *scratch_allocator,
    VkrMeshLoadMetrics *metrics) {
  if (!buffer || !ranges || range_count == 0u || !scratch_allocator ||
      !metrics || buffer->index_size != sizeof(uint32_t) || !buffer->vertices ||
      !buffer->indices) {
    return false_v;
  }
  uint32_t max_range_indices = 0u;
  for (uint32_t index = 0; index < range_count; ++index) {
    max_range_indices = Max(max_range_indices, ranges[index].index_count);
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }
  uint32_t *local_indices = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)max_range_indices * sizeof(*local_indices),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!local_indices) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  uint64_t transformed = 0u;
  uint64_t fetched = 0u;
  uint64_t triangles = 0u;
  uint64_t vertices = 0u;
  uint64_t vertex_bytes = 0u;
  const uint32_t *indices = buffer->indices;
  bool8_t valid = true_v;
  for (uint32_t index = 0; index < range_count; ++index) {
    const VkrGeometryUploadRange *range = &ranges[index];
    if (range->index_count == 0u || range->index_count % 3u != 0u ||
        range->first_index > buffer->index_count ||
        range->index_count > buffer->index_count - range->first_index) {
      valid = false_v;
      break;
    }
    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0u;
    for (uint32_t element = 0; element < range->index_count; ++element) {
      const uint32_t vertex = indices[range->first_index + element];
      if (vertex >= buffer->vertex_count) {
        valid = false_v;
        break;
      }
      min_vertex = Min(min_vertex, vertex);
      max_vertex = Max(max_vertex, vertex);
      local_indices[element] = vertex;
    }
    if (!valid) {
      break;
    }
    const uint32_t range_vertices = max_vertex - min_vertex + 1u;
    for (uint32_t element = 0; element < range->index_count; ++element) {
      local_indices[element] -= min_vertex;
    }
    VkrMeshoptAnalysis analysis = {0};
    if (vkr_meshopt_analyze_range(local_indices, range->index_count,
                                  range_vertices, buffer->vertex_size,
                                  &analysis) != 0) {
      valid = false_v;
      break;
    }
    transformed += analysis.vertices_transformed;
    fetched += analysis.bytes_fetched;
    triangles += range->index_count / 3u;
    vertices += range_vertices;
    vertex_bytes += (uint64_t)range_vertices * buffer->vertex_size;
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!valid) {
    return false_v;
  }
  metrics->analyzed_triangles = triangles;
  metrics->analyzed_vertices = vertices;
  metrics->analyzed_vertex_bytes_before = vertex_bytes;
  metrics->analyzed_vertex_bytes_after = vertex_bytes;
  metrics->vertices_transformed_before = transformed;
  metrics->vertices_transformed_after = transformed;
  metrics->bytes_fetched_before = fetched;
  metrics->bytes_fetched_after = fetched;
  return true_v;
}

vkr_internal void vkr_mesh_loader_destroy_result(VkrMeshLoaderContext *context,
                                                 VkrMeshLoaderResult *result) {
  if (!context || !result) {
    return;
  }
  for (uint64_t index = 0; index < result->material_handles.length; ++index) {
    VkrMaterialHandle material = result->material_handles.data[index];
    if (material.id != 0u && context->material_system) {
      vkr_material_system_release(context->material_system, material);
      result->material_handles.data[index] = VKR_MATERIAL_HANDLE_INVALID;
    }
  }
  void *pool_chunk = result->pool_chunk;
  result->pool_chunk = NULL;
  if (result->arena) {
    vkr_allocator_release_global_accounting(&result->allocator);
    arena_destroy(result->arena);
    result->arena = NULL;
  }
  if (pool_chunk && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
}

vkr_internal bool8_t vkr_mesh_loader_create_result(
    VkrMeshLoaderContext *context, VkrMeshLoaderResult **out_result,
    VkrRendererError *out_error) {
  *out_result = NULL;
  if (!context || !context->arena_pool || !context->arena_pool->initialized) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }
  void *pool_chunk = vkr_arena_pool_acquire(context->arena_pool);
  if (!pool_chunk) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  Arena *arena =
      arena_create_from_buffer(pool_chunk, context->arena_pool->chunk_size);
  if (!arena) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }
  VkrMeshLoaderResult *result = vkr_allocator_alloc(
      &allocator, sizeof(*result), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(result, sizeof(*result));
  result->arena = arena;
  result->pool_chunk = pool_chunk;
  result->allocator = allocator;
  *out_result = result;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_cooked(
    VkrMeshLoaderContext *context, String8 name,
    VkrAllocator *scratch_allocator, VkrMeshLoaderResult **out_result,
    VkrRendererError *out_error) {
  *out_result = NULL;
  if (!vkr_mesh_loader_has_vkb_extension(name)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  VkrMeshLoaderResult *result = NULL;
  if (!vkr_mesh_loader_create_result(context, &result, out_error)) {
    return false_v;
  }
  FilePath path = vkr_asset_path_file(scratch_allocator, name);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  uint8_t *artifact = NULL;
  uint64_t artifact_size = 0u;
  if (file_open(&path, mode, &file) != FILE_ERROR_NONE ||
      file_read_all(&file, scratch_allocator, &artifact, &artifact_size) !=
          FILE_ERROR_NONE) {
    file_close(&file);
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }
  file_close(&file);

  VkrMeshCookedDecoded decoded = {0};
  if (!vkr_mesh_cooked_decode(&result->allocator, scratch_allocator, artifact,
                              artifact_size, &decoded)) {
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  if (!vkr_mesh_cooked_apply_material_remap(scratch_allocator, name, &decoded)) {
    log_error(
        "Mesh '%.*s': material remap is unreadable, malformed or incomplete",
        (int)name.length, name.str);
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  Array_VkrMaterialHandle material_handles =
      array_create_VkrMaterialHandle(&result->allocator, decoded.ranges.length);
  if (decoded.ranges.length && !material_handles.data) {
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  for (uint64_t index = 0; index < material_handles.length; ++index)
    material_handles.data[index] = VKR_MATERIAL_HANDLE_INVALID;
  result->material_handles = material_handles;
  result->source_path = string8_duplicate(&result->allocator, &name);
  if (!result->source_path.str) {
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  result->root_transform = vkr_transform_identity();
  result->source = decoded.source;
  result->has_mesh_buffer = decoded.ranges.length != 0u;
  result->mesh_buffer = (VkrMeshLoaderBuffer){
      .vertex_size = decoded.mesh_buffer.vertex_size,
      .vertex_count = decoded.mesh_buffer.vertex_count,
      .vertices = (void *)decoded.mesh_buffer.vertices,
      .index_size = decoded.mesh_buffer.index_size,
      .index_count = decoded.mesh_buffer.index_count,
      .indices = (void *)decoded.mesh_buffer.indices,
      .vertex_layout = decoded.mesh_buffer.vertex_layout,
      .decodes = (VkrGpuGeometryDecodeRecord *)decoded.mesh_buffer.decodes,
      .decode_count = decoded.mesh_buffer.decode_count,
      .quantization = decoded.quantization,
  };
  for (uint64_t index = 0; index < decoded.ranges.length; ++index) {
    VkrGeometryUploadRange *range = &decoded.ranges.data[index];
    if (!range->material_name.length) {
      continue;
    }
    String8 resolved =
        vkr_asset_path_resolve(&result->allocator, name, range->material_name);
    if (!resolved.str) {
      vkr_mesh_loader_destroy_result(context, result);
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return false_v;
    }
    range->material_name = resolved;
  }
  result->submeshes = decoded.ranges;
  result->load_metrics = (VkrMeshLoadMetrics){
      .source_bytes = decoded.source_bytes,
      .cooked_bytes = decoded.cooked_bytes,
      .decoded_bytes = decoded.decoded_bytes,
      .upload_bytes = decoded.decoded_bytes,
      .vertex_count = decoded.mesh_buffer.vertex_count,
      .index_count = decoded.mesh_buffer.index_count,
      .range_count = (uint32_t)decoded.ranges.length,
      .preparation = VKR_MESH_PREPARATION_COOKED,
  };
  if (result->has_mesh_buffer &&
      !vkr_mesh_loader_analyze_cooked(
          &result->mesh_buffer, result->submeshes.data,
          (uint32_t)result->submeshes.length, scratch_allocator,
          &result->load_metrics)) {
    vkr_mesh_loader_destroy_result(context, result);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  *out_result = result;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_resolve_materials(
    VkrMeshLoaderContext *context, VkrMeshLoaderResult *result,
    VkrAllocator *scratch_allocator, VkrRendererError *out_error) {
  for (uint64_t index = 0; index < result->submeshes.length; ++index) {
    const VkrGeometryUploadRange *range = &result->submeshes.data[index];
    if (!range->material_name.str || range->material_name.length == 0u) {
      continue;
    }
    VkrResourceHandleInfo material = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_MATERIAL,
                                       range->material_name, scratch_allocator,
                                       &material, &error) ||
        material.type != VKR_RESOURCE_TYPE_MATERIAL) {
      *out_error = error == VKR_RENDERER_ERROR_NONE
                       ? VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED
                       : error;
      return false_v;
    }
    result->material_handles.data[index] = material.as.material;
    vkr_material_system_add_ref(context->material_system, material.as.material);
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_can_load(VkrResourceLoader *self,
                                              String8 name) {
  (void)self;
  return name.str && vkr_mesh_loader_has_vkb_extension(name);
}

vkr_internal bool8_t vkr_mesh_loader_load(VkrResourceLoader *self, String8 name,
                                          VkrAllocator *scratch_allocator,
                                          VkrResourceHandleInfo *out_handle,
                                          VkrRendererError *out_error) {
  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  VkrMeshLoaderResult *result = NULL;
  if (!context || !scratch_allocator || !out_handle || !out_error ||
      !vkr_mesh_loader_read_cooked(context, name, scratch_allocator, &result,
                                   out_error) ||
      !vkr_mesh_loader_resolve_materials(context, result, scratch_allocator,
                                         out_error)) {
    if (result) {
      vkr_mesh_loader_destroy_result(context, result);
    }
    return false_v;
  }
  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = result;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *scratch_allocator,
    void **out_payload, VkrRendererError *out_error) {
  *out_payload = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;
  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  if (!context || !scratch_allocator || !context->arena_pool ||
      !context->arena_pool->initialized) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }

  VkrMeshLoaderAsyncPayload *payload = vkr_allocator_alloc_ts(
      &context->async_allocator, sizeof(*payload),
      VKR_ALLOCATOR_MEMORY_TAG_STRUCT, context->async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));
  payload->context = context;
  if (!vkr_mesh_loader_read_cooked(context, name, scratch_allocator,
                                   &payload->result, out_error)) {
    vkr_allocator_free_ts(&context->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          context->async_mutex);
    return false_v;
  }

  for (uint64_t index = 0; index < payload->result->submeshes.length; ++index) {
    const VkrGeometryUploadRange *range =
        &payload->result->submeshes.data[index];
    if (range->material_name.str && range->material_name.length) {
      ++payload->dependency_count;
    }
  }
  if (payload->dependency_count) {
    const uint64_t dependency_size =
        (uint64_t)payload->dependency_count * sizeof(*payload->dependencies);
    payload->dependencies = vkr_allocator_alloc_ts(
        &context->async_allocator, dependency_size,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, context->async_mutex);
    if (!payload->dependencies) {
      vkr_mesh_loader_destroy_result(context, payload->result);
      vkr_allocator_free_ts(&context->async_allocator, payload,
                            sizeof(*payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                            context->async_mutex);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    MemZero(payload->dependencies, dependency_size);
    uint32_t dependency_index = 0u;
    for (uint64_t index = 0; index < payload->result->submeshes.length;
         ++index) {
      const VkrGeometryUploadRange *range =
          &payload->result->submeshes.data[index];
      if (!range->material_name.str || !range->material_name.length) {
        continue;
      }
      VkrMeshLoaderAsyncMaterialDependency *dependency =
          &payload->dependencies[dependency_index++];
      dependency->range_index = (uint32_t)index;
      dependency->material_path = range->material_name;
      dependency->request_info.type = VKR_RESOURCE_TYPE_MATERIAL;
      dependency->request_info.loader_id = VKR_INVALID_ID;
      dependency->request_info.load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
      VkrRendererError material_error = VKR_RENDERER_ERROR_NONE;
      (void)vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL, dependency->material_path,
          scratch_allocator, &dependency->request_info, &material_error);
    }
  }
  *out_payload = payload;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  (void)name;
  VkrMeshLoaderAsyncPayload *async_payload = payload;
  if (!async_payload || !async_payload->context || !async_payload->result ||
      !out_handle || !out_error) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }
  for (uint32_t index = 0; index < async_payload->dependency_count; ++index) {
    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    const VkrResourceLoadState state = vkr_resource_system_get_state(
        &async_payload->dependencies[index].request_info, &dependency_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
  }
  for (uint32_t index = 0; index < async_payload->dependency_count; ++index) {
    VkrMeshLoaderAsyncMaterialDependency *dependency =
        &async_payload->dependencies[index];
    VkrResourceHandleInfo resolved = {0};
    if (!vkr_resource_system_try_get_resolved(&dependency->request_info,
                                              &resolved) ||
        resolved.type != VKR_RESOURCE_TYPE_MATERIAL ||
        resolved.as.material.id == 0u ||
        dependency->range_index >= async_payload->result->submeshes.length) {
      continue;
    }
    async_payload->result->material_handles.data[dependency->range_index] =
        resolved.as.material;
    if (async_payload->context->material_system) {
      vkr_material_system_add_ref(async_payload->context->material_system,
                                  resolved.as.material);
    }
  }
  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = async_payload->result;
  async_payload->ownership_transferred = true_v;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  (void)payload;
  if (!out_cost) {
    return false_v;
  }
  MemZero(out_cost, sizeof(*out_cost));
  return true_v;
}

vkr_internal void vkr_mesh_loader_release_async_payload(VkrResourceLoader *self,
                                                        void *payload) {
  (void)self;
  VkrMeshLoaderAsyncPayload *async_payload = payload;
  if (!async_payload) {
    return;
  }
  for (uint32_t index = 0; index < async_payload->dependency_count; ++index) {
    VkrMeshLoaderAsyncMaterialDependency *dependency =
        &async_payload->dependencies[index];
    if (dependency->request_info.request_id && dependency->material_path.str &&
        dependency->material_path.length) {
      vkr_resource_system_unload(&dependency->request_info,
                                 dependency->material_path);
    }
  }
  if (!async_payload->ownership_transferred && async_payload->result) {
    vkr_mesh_loader_destroy_result(async_payload->context,
                                   async_payload->result);
  }
  if (async_payload->dependencies) {
    vkr_allocator_free_ts(
        &async_payload->context->async_allocator, async_payload->dependencies,
        (uint64_t)async_payload->dependency_count *
            sizeof(*async_payload->dependencies),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, async_payload->context->async_mutex);
  }
  vkr_allocator_free_ts(&async_payload->context->async_allocator, async_payload,
                        sizeof(*async_payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        async_payload->context->async_mutex);
}

vkr_internal void vkr_mesh_loader_unload(VkrResourceLoader *self,
                                         const VkrResourceHandleInfo *handle,
                                         String8 name) {
  (void)name;
  if (self && handle && handle->type == VKR_RESOURCE_TYPE_MESH) {
    vkr_mesh_loader_destroy_result(
        (VkrMeshLoaderContext *)self->resource_system, handle->as.mesh);
  }
}

vkr_internal uint32_t vkr_mesh_loader_batch_load(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *scratch_allocator, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  uint32_t loaded = 0u;
  for (uint32_t index = 0; index < count; ++index) {
    MemZero(&out_handles[index], sizeof(out_handles[index]));
    if (vkr_mesh_loader_load(self, paths[index], scratch_allocator,
                             &out_handles[index], &out_errors[index])) {
      ++loaded;
    }
  }
  return loaded;
}

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context) {
  return (VkrResourceLoader){
      .type = VKR_RESOURCE_TYPE_MESH,
      .resource_system = context,
      .can_load = vkr_mesh_loader_can_load,
      .load = vkr_mesh_loader_load,
      .prepare_async = vkr_mesh_loader_prepare_async,
      .finalize_async = vkr_mesh_loader_finalize_async,
      .estimate_async_finalize_cost =
          vkr_mesh_loader_estimate_async_finalize_cost,
      .release_async_payload = vkr_mesh_loader_release_async_payload,
      .unload = vkr_mesh_loader_unload,
      .batch_load = vkr_mesh_loader_batch_load,
  };
}
