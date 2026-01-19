#include "renderer/systems/vkr_mesh_manager.h"

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_resource_system.h"

/**
 * @brief FNV-1a hash helper for stable geometry keys.
 *
 * Uses mesh path bytes to generate a deterministic key so repeated loads of the
 * same mesh can reuse geometry handles instead of creating duplicates.
 */
vkr_internal uint64_t vkr_mesh_fnv1a_hash(const uint8_t *data, uint64_t length,
                                         uint64_t seed) {
  uint64_t hash = seed;
  if (!data || length == 0) {
    return hash;
  }
  for (uint64_t i = 0; i < length; ++i) {
    hash ^= (uint64_t)data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

/**
 * @brief Build a stable geometry name for mesh subset deduplication.
 *
 * The key is derived from the mesh path plus subset index, which preserves
 * distinct submeshes while allowing identical mesh instances to share geometry.
 */
vkr_internal void vkr_mesh_manager_build_geometry_key(
    char out_name[GEOMETRY_NAME_MAX_LENGTH], String8 mesh_path,
    uint32_t subset_index) {
  uint64_t hash = 14695981039346656037ull;
  hash = vkr_mesh_fnv1a_hash(mesh_path.str, mesh_path.length, hash);
  string_format(out_name, GEOMETRY_NAME_MAX_LENGTH, "mesh_%016llx_%u",
                (unsigned long long)hash, subset_index);
}

/**
 * @brief Build a stable geometry name for merged mesh buffers.
 */
vkr_internal void vkr_mesh_manager_build_mesh_buffer_key(
    char out_name[GEOMETRY_NAME_MAX_LENGTH], String8 mesh_path) {
  uint64_t hash = 14695981039346656037ull;
  hash = vkr_mesh_fnv1a_hash(mesh_path.str, mesh_path.length, hash);
  string_format(out_name, GEOMETRY_NAME_MAX_LENGTH, "meshbuf_%016llx",
                (unsigned long long)hash);
}

typedef struct VkrOpaqueRangeInfo {
  uint32_t first_index;
  uint32_t index_count;
} VkrOpaqueRangeInfo;

vkr_internal bool8_t vkr_mesh_manager_material_uses_cutout(
    VkrMaterialSystem *material_system, VkrMaterialHandle handle) {
  if (!material_system || handle.id == 0) {
    return false_v;
  }

  VkrMaterial *material =
      vkr_material_system_get_by_handle(material_system, handle);
  if (!material && material_system->default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        material_system, material_system->default_material);
  }
  if (!material || material->alpha_cutoff <= 0.0f) {
    return false_v;
  }

  VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  return diffuse_tex->enabled && diffuse_tex->handle.id != 0;
}

/**
 * @brief Compute bounding sphere for a mesh from its submesh geometries.
 * Unions all geometry AABBs then computes enclosing sphere.
 */
vkr_internal void vkr_mesh_compute_local_bounds(VkrMesh *mesh,
                                                VkrGeometrySystem *geo_system) {
  if (!mesh)
    return;
  (void)geo_system;

  if (mesh->submeshes.length == 0) {
    mesh->bounds_valid = false_v;
    return;
  }

  // Initialize union AABB with first valid geometry
  Vec3 union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 union_max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  bool8_t has_valid_geometry = false_v;

  for (uint32_t i = 0; i < mesh->submeshes.length; i++) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, i);
    if (!submesh || submesh->geometry.id == 0 || submesh->index_count == 0) {
      continue;
    }

    // Submesh bounds store center + min/max extents (relative to center).
    Vec3 geo_min = vec3_add(submesh->center, submesh->min_extents);
    Vec3 geo_max = vec3_add(submesh->center, submesh->max_extents);

    // Union with current bounds
    union_min.x = vkr_min_f32(union_min.x, geo_min.x);
    union_min.y = vkr_min_f32(union_min.y, geo_min.y);
    union_min.z = vkr_min_f32(union_min.z, geo_min.z);
    union_max.x = vkr_max_f32(union_max.x, geo_max.x);
    union_max.y = vkr_max_f32(union_max.y, geo_max.y);
    union_max.z = vkr_max_f32(union_max.z, geo_max.z);

    has_valid_geometry = true_v;
  }

  if (!has_valid_geometry) {
    mesh->bounds_valid = false_v;
    return;
  }

  // Compute bounding sphere from AABB
  mesh->bounds_local_center = vec3_scale(vec3_add(union_min, union_max), 0.5f);
  Vec3 half_extents = vec3_scale(vec3_sub(union_max, union_min), 0.5f);
  mesh->bounds_local_radius = vec3_length(half_extents);
  mesh->bounds_valid = true_v;
}

vkr_internal uint32_t vkr_mesh_manager_batch_wave_size(const VkrMeshManager *m,
                                                       uint32_t count) {
  // Loading a mesh uses a chunk-per-mesh arena pool. Keep batch windows bounded
  // to avoid exhausting the pool (and avoid deadlock now that acquire blocks).
  uint32_t wave = 1;
  if (m && m->loader_context && m->loader_context->arena_pool &&
      m->loader_context->arena_pool->initialized) {
    wave = m->loader_context->arena_pool->pool.chunk_count;
  }

  if (wave == 0) {
    wave = 1;
  }

  if (count > 0) {
    wave = vkr_min_u32(wave, count);
  }

  return wave;
}

/**
 * @brief Update world-space bounding sphere from local bounds and model matrix.
 * Handles non-uniform scale conservatively using max scale factor.
 */
vkr_internal void vkr_mesh_update_world_bounds(VkrMesh *mesh) {
  if (!mesh->bounds_valid) {
    return;
  }

  // Transform center to world space
  mesh->bounds_world_center =
      mat4_mul_vec3(mesh->model, mesh->bounds_local_center);

  // Compute max scale factor from matrix columns (handles non-uniform scale)
  Vec3 col0 = vec3_new(mesh->model.m00, mesh->model.m10, mesh->model.m20);
  Vec3 col1 = vec3_new(mesh->model.m01, mesh->model.m11, mesh->model.m21);
  Vec3 col2 = vec3_new(mesh->model.m02, mesh->model.m12, mesh->model.m22);

  float32_t sx = vec3_length(col0);
  float32_t sy = vec3_length(col1);
  float32_t sz = vec3_length(col2);
  float32_t max_scale = vkr_max_f32(vkr_max_f32(sx, sy), sz);

  mesh->bounds_world_radius = mesh->bounds_local_radius * max_scale;
}

vkr_internal bool8_t vkr_mesh_manager_resolve_geometry(
    VkrMeshManager *manager, const VkrSubMeshDesc *desc,
    VkrGeometryHandle *out_handle, bool8_t *out_owned,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_owned != NULL, "Out owned is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (desc->geometry.id != 0) {
    if (desc->owns_geometry) {
      vkr_geometry_system_acquire(manager->geometry_system, desc->geometry);
    }
    *out_handle = desc->geometry;
    *out_owned = desc->owns_geometry;
    return true_v;
  }

  if (desc->geometry_name.str && desc->geometry_name.length > 0) {
    *out_handle = vkr_geometry_system_acquire_by_name(
        manager->geometry_system, desc->geometry_name, false_v, out_error);
    if (out_handle->id != 0) {
      *out_owned = true_v;
      return true_v;
    }
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  return false_v;
}

vkr_internal bool8_t vkr_mesh_manager_resolve_material(
    VkrMeshManager *manager, const VkrSubMeshDesc *desc,
    VkrMaterialHandle *out_handle, bool8_t *out_owned,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_owned != NULL, "Out owned is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (desc->material.id != 0) {
    if (desc->owns_material) {
      vkr_material_system_add_ref(manager->material_system, desc->material);
    }
    *out_handle = desc->material;
    *out_owned = desc->owns_material;
    return true_v;
  }

  if (desc->material_name.str && desc->material_name.length > 0) {
    *out_handle = vkr_material_system_acquire(
        manager->material_system, desc->material_name, false_v, out_error);
    if (out_handle->id != 0) {
      *out_owned = true_v;
      return true_v;
    }
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  return false_v;
}

vkr_internal void vkr_mesh_manager_release_submesh(VkrMeshManager *manager,
                                                   VkrSubMesh *submesh) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(submesh != NULL, "Submesh is NULL");

  if (submesh->pipeline.id != 0) {
    VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        manager->pipeline_registry, submesh->pipeline, submesh->instance_state,
        &rel_err);
    submesh->pipeline = VKR_PIPELINE_HANDLE_INVALID;
    submesh->instance_state = (VkrRendererInstanceStateHandle){0};
  }

  if (submesh->geometry.id != 0 && submesh->owns_geometry) {
    vkr_geometry_system_release(manager->geometry_system, submesh->geometry);
    submesh->geometry = VKR_GEOMETRY_HANDLE_INVALID;
  }

  if (submesh->material.id != 0 && submesh->owns_material) {
    vkr_material_system_release(manager->material_system, submesh->material);
    submesh->material = VKR_MATERIAL_HANDLE_INVALID;
  }

  submesh->pipeline_dirty = true_v;
  submesh->last_render_frame = 0;
  MemZero(submesh, sizeof(*submesh));
}

vkr_internal void vkr_mesh_manager_release_handles(VkrMeshManager *manager,
                                                   VkrMesh *mesh) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(mesh != NULL, "Mesh is NULL");

  if (!mesh->submeshes.data || mesh->submeshes.length == 0)
    return;

  for (uint32_t i = 0; i < mesh->submeshes.length; ++i) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, i);
    vkr_mesh_manager_release_submesh(manager, submesh);
  }
}

vkr_internal void vkr_mesh_manager_cleanup_submesh_array(
    VkrMeshManager *manager, Array_VkrSubMesh *array, uint32_t built_count) {
  assert_log(manager != NULL, "Manager is NULL");
  if (!array || !array->data || array->length == 0) {
    return;
  }
  if (built_count > array->length) {
    built_count = (uint32_t)array->length;
  }
  for (uint32_t i = 0; i < built_count; ++i) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(array, i);
    if (submesh) {
      vkr_mesh_manager_release_submesh(manager, submesh);
    }
  }
  array_destroy_VkrSubMesh(array);
}

bool8_t vkr_mesh_manager_init(VkrMeshManager *manager,
                              VkrGeometrySystem *geometry_system,
                              VkrMaterialSystem *material_system,
                              VkrPipelineRegistry *pipeline_registry,
                              const VkrMeshManagerConfig *config) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(geometry_system != NULL, "Geometry system is NULL");
  assert_log(material_system != NULL, "Material system is NULL");
  assert_log(pipeline_registry != NULL, "Pipeline registry is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(config->max_mesh_count > 0, "Max mesh count is 0");

  MemZero(manager, sizeof(*manager));

  ArenaFlags mesh_arena_flags = bitset8_create();
  bitset8_set(&mesh_arena_flags, ARENA_FLAG_LARGE_PAGES);
  manager->arena = arena_create(MB(6), MB(6), mesh_arena_flags);
  if (!manager->arena) {
    log_fatal("Failed to create mesh manager arena!");
    return false_v;
  }

  manager->scratch_arena = arena_create(MB(3), KB(64));
  if (!manager->scratch_arena) {
    log_fatal("Failed to create mesh manager scratch arena!");
    return false_v;
  }

  manager->geometry_system = geometry_system;
  manager->material_system = material_system;
  manager->pipeline_registry = pipeline_registry;

  manager->config =
      config ? *config : (VkrMeshManagerConfig){.max_mesh_count = 1024};

  if (manager->config.max_mesh_count == 0) {
    manager->config.max_mesh_count = 1;
  }

  manager->allocator.ctx = manager->arena;
  vkr_allocator_arena(&manager->allocator);
  manager->scratch_allocator.ctx = manager->scratch_arena;
  vkr_allocator_arena(&manager->scratch_allocator);

  manager->meshes =
      array_create_VkrMesh(&manager->allocator, manager->config.max_mesh_count);
  manager->free_indices = array_create_uint32_t(&manager->allocator,
                                                manager->config.max_mesh_count);
  manager->free_count = 0;
  manager->mesh_count = 0;
  manager->next_free_index = 0;

  for (uint32_t i = 0; i < manager->meshes.length; ++i) {
    VkrMesh empty = {0};
    array_set_VkrMesh(&manager->meshes, i, empty);
  }

  return true_v;
}

void vkr_mesh_manager_shutdown(VkrMeshManager *manager) {
  if (!manager)
    return;

  for (uint32_t i = 0; i < manager->meshes.length; ++i) {
    VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, i);
    if (mesh->submeshes.data && mesh->submeshes.length > 0) {
      vkr_mesh_manager_release_handles(manager, mesh);
      array_destroy_VkrSubMesh(&mesh->submeshes);
      MemZero(mesh, sizeof(*mesh));
    }
  }

  manager->free_count = 0;
  manager->mesh_count = 0;
  manager->next_free_index = 0;

  array_destroy_VkrMesh(&manager->meshes);
  array_destroy_uint32_t(&manager->free_indices);
  arena_destroy(manager->arena);
  arena_destroy(manager->scratch_arena);
}

bool8_t vkr_mesh_manager_create(VkrMeshManager *manager,
                                const VkrMeshDesc *desc,
                                VkrRendererError *out_error,
                                VkrMesh **out_mesh) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Mesh desc is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  uint32_t index = 0;
  if (!vkr_mesh_manager_add(manager, desc, &index, out_error)) {
    return false_v;
  }

  vkr_mesh_manager_update_model(manager, index);

  VkrMesh *mesh = vkr_mesh_manager_get(manager, index);
  if (!mesh) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (out_mesh) {
    *out_mesh = mesh;
  }

  mesh->loading_state = VKR_MESH_LOADING_STATE_LOADED;

  return true_v;
}

bool8_t vkr_mesh_manager_add(VkrMeshManager *manager, const VkrMeshDesc *desc,
                             uint32_t *out_index, VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Mesh desc is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!desc->submeshes || desc->submesh_count == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  Array_VkrSubMesh submesh_array =
      array_create_VkrSubMesh(&manager->allocator, desc->submesh_count);
  if (!submesh_array.data) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(submesh_array.data,
          submesh_array.length * sizeof(*submesh_array.data));

  uint32_t built_count = 0;
  VkrRendererError submesh_err = VKR_RENDERER_ERROR_NONE;
  for (uint32_t submesh_index = 0; submesh_index < desc->submesh_count;
       ++submesh_index) {
    const VkrSubMeshDesc *sub_desc = desc->submeshes + submesh_index;
    VkrGeometryHandle geometry = VKR_GEOMETRY_HANDLE_INVALID;
    bool8_t owns_geometry = sub_desc->owns_geometry;
    if (!vkr_mesh_manager_resolve_geometry(manager, sub_desc, &geometry,
                                           &owns_geometry, &submesh_err)) {
      vkr_mesh_manager_cleanup_submesh_array(manager, &submesh_array,
                                             built_count);
      *out_error = submesh_err;
      return false_v;
    }

    VkrMaterialHandle material = VKR_MATERIAL_HANDLE_INVALID;
    bool8_t owns_material = sub_desc->owns_material;
    if (!vkr_mesh_manager_resolve_material(manager, sub_desc, &material,
                                           &owns_material, &submesh_err)) {
      if (geometry.id != 0 && owns_geometry) {
        vkr_geometry_system_release(manager->geometry_system, geometry);
      }
      vkr_mesh_manager_cleanup_submesh_array(manager, &submesh_array,
                                             built_count);
      *out_error = submesh_err;
      return false_v;
    }

    uint32_t range_id = sub_desc->range_id;
    uint32_t first_index = sub_desc->first_index;
    uint32_t index_count = sub_desc->index_count;
    int32_t vertex_offset = sub_desc->vertex_offset;
    Vec3 center = sub_desc->center;
    Vec3 min_extents = sub_desc->min_extents;
    Vec3 max_extents = sub_desc->max_extents;
    bool8_t uses_full_geometry = (sub_desc->index_count == 0 &&
                                  sub_desc->first_index == 0 &&
                                  sub_desc->vertex_offset == 0);

    if (index_count == 0) {
      VkrGeometry *geo =
          vkr_geometry_system_get_by_handle(manager->geometry_system, geometry);
      if (geo) {
        first_index = 0;
        index_count = geo->index_count;
        vertex_offset = 0;
        center = geo->center;
        min_extents = geo->min_extents;
        max_extents = geo->max_extents;
      }
    }

    if (range_id == 0 && uses_full_geometry) {
      range_id = geometry.id;
    }

    VkrSubMesh submesh = {
        .geometry = geometry,
        .material = material,
        .pipeline = VKR_PIPELINE_HANDLE_INVALID,
        .instance_state = (VkrRendererInstanceStateHandle){0},
        .pipeline_domain = (sub_desc->pipeline_domain > 0)
                               ? sub_desc->pipeline_domain
                               : VKR_PIPELINE_DOMAIN_WORLD,
        .shader_override =
            string8_duplicate(&manager->allocator, &sub_desc->shader_override),
        .range_id = range_id,
        .first_index = first_index,
        .index_count = index_count,
        .vertex_offset = vertex_offset,
        .opaque_first_index = sub_desc->opaque_first_index,
        .opaque_index_count = sub_desc->opaque_index_count,
        .opaque_vertex_offset = sub_desc->opaque_vertex_offset,
        .center = center,
        .min_extents = min_extents,
        .max_extents = max_extents,
        .pipeline_dirty = true_v,
        .owns_geometry = owns_geometry,
        .owns_material = owns_material,
        .last_render_frame = 0,
    };
    if (!submesh.shader_override.str || submesh.shader_override.length == 0) {
      submesh.shader_override = string8_lit("shader.default.world");
    }

    array_set_VkrSubMesh(&submesh_array, submesh_index, submesh);
    built_count++;
  }

  VkrMesh new_mesh = {0};
  new_mesh.transform = desc->transform;
  new_mesh.model = vkr_transform_get_world(&new_mesh.transform);
  new_mesh.submeshes = submesh_array;
  new_mesh.render_id = 0;
  new_mesh.visible = true_v;
  new_mesh.loading_state = VKR_MESH_LOADING_STATE_LOADED;

  vkr_mesh_compute_local_bounds(&new_mesh, manager->geometry_system);
  vkr_mesh_update_world_bounds(&new_mesh);

  uint32_t slot = VKR_INVALID_ID;
  if (manager->free_count > 0) {
    slot = manager->free_indices.data[manager->free_count - 1];
    manager->free_count--;
  } else {
    slot = manager->next_free_index;
    if (slot >= manager->meshes.length) {
      vkr_mesh_manager_cleanup_submesh_array(manager, &submesh_array,
                                             built_count);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    manager->next_free_index++;
  }

  array_set_VkrMesh(&manager->meshes, slot, new_mesh);
  manager->mesh_count++;

  if (out_index) {
    *out_index = slot;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_mesh_manager_load(VkrMeshManager *manager,
                              const VkrMeshLoadDesc *desc,
                              uint32_t *out_first_index,
                              uint32_t *out_mesh_count,
                              VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Mesh load desc is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // Use batch loader with count=1 for consistency
  uint32_t mesh_index = VKR_INVALID_ID;
  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  uint32_t loaded =
      vkr_mesh_manager_load_batch(manager, desc, 1, &mesh_index, &err);

  *out_error = err;
  if (loaded == 0 || mesh_index == VKR_INVALID_ID) {
    return false_v;
  }

  if (out_first_index) {
    *out_first_index = mesh_index;
  }

  if (out_mesh_count) {
    // Get the mesh to find how many subsets it has
    VkrMesh *mesh = vkr_mesh_manager_get(manager, mesh_index);
    if (mesh) {
      *out_mesh_count = vkr_mesh_manager_submesh_count(mesh);
    } else {
      log_error("Loaded mesh %u cannot be retrieved", mesh_index);
      *out_mesh_count = 0;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_manager_process_resource_handle(
    VkrMeshManager *manager, const VkrResourceHandleInfo *handle_info,
    VkrRendererError error, const VkrMeshLoadDesc *desc, uint32_t *out_index,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Desc is NULL");

  if (!handle_info || handle_info->type != VKR_RESOURCE_TYPE_MESH ||
      !handle_info->as.mesh) {
    if (out_error) {
      *out_error = error != VKR_RENDERER_ERROR_NONE
                       ? error
                       : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }

  VkrMeshLoaderResult *mesh_result = handle_info->as.mesh;

  // Validate mesh result
  bool8_t use_merged =
      mesh_result->has_mesh_buffer &&
      mesh_result->mesh_buffer.vertex_count > 0 &&
      mesh_result->mesh_buffer.index_count > 0 &&
      mesh_result->submeshes.length > 0 && mesh_result->submeshes.data;
  if (!use_merged &&
      (mesh_result->subsets.length == 0 || !mesh_result->subsets.data)) {
    log_error("MeshManager: mesh '%.*s' returned no subsets",
              (int)desc->mesh_path.length, desc->mesh_path.str);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }

  uint32_t subset_count = use_merged
                              ? (uint32_t)mesh_result->submeshes.length
                              : (uint32_t)mesh_result->subsets.length;
  VkrAllocator *scratch_allocator = &manager->scratch_allocator;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return false_v;
  }

  VkrSubMeshDesc *sub_descs = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)subset_count * sizeof(VkrSubMeshDesc),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!sub_descs) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  MemZero(sub_descs, (uint64_t)subset_count * sizeof(VkrSubMeshDesc));

  uint32_t built_count = 0;
  bool8_t subsets_success = true_v;
  VkrGeometryHandle merged_geometry = VKR_GEOMETRY_HANDLE_INVALID;
  VkrOpaqueRangeInfo *opaque_ranges = NULL;
  uint32_t *opaque_indices = NULL;
  uint32_t opaque_index_count = 0;
  bool8_t build_opaque_indices = false_v;

  if (use_merged) {
    if (mesh_result->mesh_buffer.index_size != sizeof(uint32_t)) {
      log_warn(
          "MeshManager: merged buffer index size %u; opaque compaction skipped",
          mesh_result->mesh_buffer.index_size);
    } else {
      uint32_t total_indices = mesh_result->mesh_buffer.index_count;
      for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
        VkrMeshLoaderSubmeshRange *range =
            &mesh_result->submeshes.data[i];
        if (!vkr_mesh_manager_material_uses_cutout(manager->material_system,
                                                   range->material_handle)) {
          opaque_index_count += range->index_count;
        }
      }

      if (opaque_index_count > 0 && opaque_index_count < total_indices) {
        build_opaque_indices = true_v;
        opaque_ranges = vkr_allocator_alloc(
            scratch_allocator,
            (uint64_t)subset_count * sizeof(VkrOpaqueRangeInfo),
            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        if (!opaque_ranges) {
          if (out_error) {
            *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
          }
          subsets_success = false_v;
        } else {
          MemZero(opaque_ranges,
                  (uint64_t)subset_count * sizeof(VkrOpaqueRangeInfo));
        }

        if (subsets_success) {
          opaque_indices = vkr_allocator_alloc(
              scratch_allocator,
              (uint64_t)opaque_index_count * sizeof(uint32_t),
              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
          if (!opaque_indices) {
            if (out_error) {
              *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
            }
            subsets_success = false_v;
          }
        }

        if (subsets_success) {
          uint32_t *src_indices = (uint32_t *)mesh_result->mesh_buffer.indices;
          uint32_t opaque_write = 0;
          for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
            VkrMeshLoaderSubmeshRange *range =
                &mesh_result->submeshes.data[i];
            if (vkr_mesh_manager_material_uses_cutout(
                    manager->material_system, range->material_handle)) {
              continue;
            }
            if (opaque_write + range->index_count > opaque_index_count) {
              log_warn("MeshManager: opaque index buffer overflow");
              subsets_success = false_v;
              break;
            }

            if (opaque_ranges) {
              opaque_ranges[i].first_index = opaque_write;
              opaque_ranges[i].index_count = range->index_count;
            }
            MemCopy(opaque_indices + opaque_write,
                    src_indices + range->first_index,
                    (uint64_t)range->index_count * sizeof(uint32_t));
            opaque_write += range->index_count;
          }
          if (opaque_write != opaque_index_count) {
            log_warn("MeshManager: opaque index count mismatch (%u vs %u)",
                     opaque_write, opaque_index_count);
          }
        }
      }
    }

    char geometry_name_buf[GEOMETRY_NAME_MAX_LENGTH] = {0};
    vkr_mesh_manager_build_mesh_buffer_key(geometry_name_buf,
                                           mesh_result->source_path);
    String8 geometry_name = string8_create(
        (uint8_t *)geometry_name_buf, string_length(geometry_name_buf));

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    merged_geometry =
        vkr_geometry_system_acquire_by_name(manager->geometry_system,
                                            geometry_name, true_v, &geo_err);
    if (merged_geometry.id == 0) {
      if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        if (out_error) {
          *out_error = geo_err;
        }
        subsets_success = false_v;
      } else {
        Vec3 union_min =
            vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
        Vec3 union_max =
            vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
        bool8_t has_bounds = false_v;

        for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
          VkrMeshLoaderSubmeshRange *range =
              &mesh_result->submeshes.data[i];
          Vec3 range_min = vec3_add(range->center, range->min_extents);
          Vec3 range_max = vec3_add(range->center, range->max_extents);
          union_min.x = vkr_min_f32(union_min.x, range_min.x);
          union_min.y = vkr_min_f32(union_min.y, range_min.y);
          union_min.z = vkr_min_f32(union_min.z, range_min.z);
          union_max.x = vkr_max_f32(union_max.x, range_max.x);
          union_max.y = vkr_max_f32(union_max.y, range_max.y);
          union_max.z = vkr_max_f32(union_max.z, range_max.z);
          has_bounds = true_v;
        }

        Vec3 center = vec3_zero();
        Vec3 min_extents = vec3_zero();
        Vec3 max_extents = vec3_zero();
        if (has_bounds) {
          center = vec3_scale(vec3_add(union_min, union_max), 0.5f);
          min_extents = vec3_sub(union_min, center);
          max_extents = vec3_sub(union_max, center);
        }

        VkrGeometryConfig cfg = {0};
        cfg.vertex_size = mesh_result->mesh_buffer.vertex_size;
        cfg.vertex_count = mesh_result->mesh_buffer.vertex_count;
        cfg.vertices = mesh_result->mesh_buffer.vertices;
        cfg.index_size = mesh_result->mesh_buffer.index_size;
        cfg.index_count = mesh_result->mesh_buffer.index_count;
        cfg.indices = mesh_result->mesh_buffer.indices;
        cfg.center = center;
        cfg.min_extents = min_extents;
        cfg.max_extents = max_extents;
        string_copy(cfg.name, geometry_name_buf);

        merged_geometry = vkr_geometry_system_create(manager->geometry_system,
                                                     &cfg, true_v, &geo_err);
        if (merged_geometry.id == 0) {
          if (out_error) {
            *out_error = geo_err;
          }
          subsets_success = false_v;
        }
      }
    }

    if (subsets_success && build_opaque_indices && merged_geometry.id != 0) {
      VkrGeometry *geometry = vkr_geometry_system_get_by_handle(
          manager->geometry_system, merged_geometry);
      if (geometry) {
        geometry->opaque_index_count = opaque_index_count;
        if (!geometry->opaque_index_buffer.handle && opaque_indices) {
          String8 debug_name = string8_create(
              (uint8_t *)geometry->name, string_length(geometry->name));
          VkrRendererError opaque_err = VKR_RENDERER_ERROR_NONE;
          geometry->opaque_index_buffer = vkr_index_buffer_create(
              manager->geometry_system->renderer, opaque_indices,
              geometry->index_buffer.type, opaque_index_count, debug_name,
              &opaque_err);
          if (opaque_err != VKR_RENDERER_ERROR_NONE) {
            log_warn("MeshManager: failed to create opaque index buffer '%s'",
                     geometry->name);
            geometry->opaque_index_buffer = (VkrIndexBuffer){0};
            geometry->opaque_index_count = 0;
            build_opaque_indices = false_v;
          }
        }
      }
    }
  }

  // Build all subsets
  for (uint32_t i = 0; subsets_success && i < subset_count; ++i) {
    if (use_merged) {
      VkrMeshLoaderSubmeshRange *range =
          array_get_VkrMeshLoaderSubmeshRange(&mesh_result->submeshes, i);
      if (!range) {
        if (out_error) {
          *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        }
        subsets_success = false_v;
        break;
      }

      if (i > 0 && merged_geometry.id != 0) {
        vkr_geometry_system_acquire(manager->geometry_system, merged_geometry);
      }

      VkrMaterialHandle material = range->material_handle;
      bool8_t owns_material = true_v;
      if (material.id == 0) {
        material = manager->material_system->default_material;
        owns_material = false_v;
      }

      VkrPipelineDomain domain = range->pipeline_domain;
      if (domain == 0) {
        domain = desc->pipeline_domain != 0 ? desc->pipeline_domain
                                            : VKR_PIPELINE_DOMAIN_WORLD;
      }

      String8 shader_override = range->shader_override.str
                                    ? range->shader_override
                                    : desc->shader_override;
      String8 shader_override_copy = {0};
      if (shader_override.str && shader_override.length > 0) {
        shader_override_copy =
            string8_duplicate(&manager->allocator, &shader_override);
      }

      sub_descs[built_count++] = (VkrSubMeshDesc){
          .geometry = merged_geometry,
          .material = material,
          .shader_override = shader_override_copy,
          .pipeline_domain = domain,
          .range_id = range->range_id,
          .first_index = range->first_index,
          .index_count = range->index_count,
          .vertex_offset = range->vertex_offset,
          .opaque_first_index =
              (build_opaque_indices && opaque_ranges)
                  ? opaque_ranges[i].first_index
                  : 0,
          .opaque_index_count =
              (build_opaque_indices && opaque_ranges)
                  ? opaque_ranges[i].index_count
                  : 0,
          .opaque_vertex_offset = range->vertex_offset,
          .center = range->center,
          .min_extents = range->min_extents,
          .max_extents = range->max_extents,
          .owns_geometry = true_v,
          .owns_material = owns_material,
      };
      continue;
    }

    VkrMeshLoaderSubset *subset =
        array_get_VkrMeshLoaderSubset(&mesh_result->subsets, i);
    if (!subset) {
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      }
      subsets_success = false_v;
      break;
    }

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    vkr_mesh_manager_build_geometry_key(subset->geometry_config.name,
                                        mesh_result->source_path, i);
    uint64_t name_length = string_length(subset->geometry_config.name);
    String8 geometry_name = string8_create(
        (uint8_t *)subset->geometry_config.name, name_length);
    VkrGeometryHandle geometry =
        vkr_geometry_system_acquire_by_name(manager->geometry_system,
                                            geometry_name, true_v, &geo_err);
    if (geometry.id == 0) {
      if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        if (out_error) {
          *out_error = geo_err;
        }
        subsets_success = false_v;
        break;
      }
      geometry = vkr_geometry_system_create(manager->geometry_system,
                                            &subset->geometry_config, true_v,
                                            &geo_err);
      if (geometry.id == 0) {
        if (out_error) {
          *out_error = geo_err;
        }
        subsets_success = false_v;
        break;
      }
    }

    VkrMaterialHandle material = subset->material_handle;
    bool8_t owns_material = true_v;
    if (material.id == 0) {
      material = manager->material_system->default_material;
      owns_material = false_v;
    }

    VkrPipelineDomain domain = subset->pipeline_domain;
    if (domain == 0) {
      domain = desc->pipeline_domain != 0 ? desc->pipeline_domain
                                          : VKR_PIPELINE_DOMAIN_WORLD;
    }

    String8 shader_override = subset->shader_override.str
                                  ? subset->shader_override
                                  : desc->shader_override;
    String8 shader_override_copy = {0};
    if (shader_override.str && shader_override.length > 0) {
      shader_override_copy =
          string8_duplicate(&manager->allocator, &shader_override);
    }

    sub_descs[built_count++] = (VkrSubMeshDesc){
        .geometry = geometry,
        .material = material,
        .shader_override = shader_override_copy,
        .pipeline_domain = domain,
        .range_id = geometry.id,
        .first_index = 0,
        .index_count = subset->geometry_config.index_count,
        .vertex_offset = 0,
        .center = subset->geometry_config.center,
        .min_extents = subset->geometry_config.min_extents,
        .max_extents = subset->geometry_config.max_extents,
        .owns_geometry = true_v,
        .owns_material = owns_material,
    };
  }

  // Check if all subsets were built successfully
  if (!subsets_success || built_count != subset_count) {
    // Clean up any geometry that was created
    for (uint32_t i = 0; i < built_count; ++i) {
      if (sub_descs[i].geometry.id != 0) {
        vkr_geometry_system_release(manager->geometry_system,
                                    sub_descs[i].geometry);
      }
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  // All subsets built successfully, create the mesh
  VkrMeshDesc mesh_desc = {
      .transform = desc->transform,
      .submeshes = sub_descs,
      .submesh_count = subset_count,
  };

  uint32_t mesh_index = VKR_INVALID_ID;
  bool8_t result = false_v;
  if (vkr_mesh_manager_add(manager, &mesh_desc, &mesh_index, out_error)) {
    vkr_mesh_manager_update_model(manager, mesh_index);
    if (out_index) {
      *out_index = mesh_index;
    }
    result = true_v;
  }

  // Clean up geometry handles (ownership transferred to mesh manager)
  for (uint32_t i = 0; i < built_count; ++i) {
    if (sub_descs[i].geometry.id != 0) {
      vkr_geometry_system_release(manager->geometry_system,
                                  sub_descs[i].geometry);
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (result) {
    VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, mesh_index);
    if (mesh) {
      mesh->loading_state = VKR_MESH_LOADING_STATE_LOADED;
    }
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_NONE;
    }
  }

  return result;
}

bool8_t vkr_mesh_manager_remove(VkrMeshManager *manager, uint32_t index) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return false_v;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0) {
    return false_v;
  }

  vkr_mesh_manager_release_handles(manager, mesh);
  array_destroy_VkrSubMesh(&mesh->submeshes);
  MemZero(mesh, sizeof(*mesh));

  if (manager->free_count < manager->free_indices.length) {
    manager->free_indices.data[manager->free_count++] = index;
  }

  if (manager->mesh_count > 0)
    manager->mesh_count--;

  return true_v;
}

VkrMesh *vkr_mesh_manager_get(VkrMeshManager *manager, uint32_t index) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return NULL;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return NULL;

  return mesh;
}

uint32_t vkr_mesh_manager_count(const VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  return manager->mesh_count;
}

uint32_t vkr_mesh_manager_capacity(const VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  return manager->meshes.length;
}

bool8_t vkr_mesh_manager_set_submesh_material(VkrMeshManager *manager,
                                              uint32_t mesh_index,
                                              uint32_t submesh_index,
                                              VkrMaterialHandle material,
                                              VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(mesh_index < manager->meshes.length, "Index is out of bounds");
  assert_log(material.id != 0, "Material is invalid");
  assert_log(out_error != NULL, "Out error is NULL");

  if (mesh_index >= manager->meshes.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, mesh_index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (submesh_index >= mesh->submeshes.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, submesh_index);

  vkr_material_system_add_ref(manager->material_system, material);

  if (submesh->material.id != 0 && submesh->owns_material) {
    vkr_material_system_release(manager->material_system, submesh->material);
  }

  submesh->material = material;
  submesh->owns_material = true_v;
  submesh->pipeline_dirty = true_v;
  submesh->last_render_frame = 0;

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_mesh_manager_refresh_pipeline(VkrMeshManager *manager,
                                          uint32_t mesh_index,
                                          uint32_t submesh_index,
                                          VkrPipelineHandle desired_pipeline,
                                          VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (mesh_index >= manager->meshes.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, mesh_index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (submesh_index >= mesh->submeshes.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, submesh_index);
  if (!submesh) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  bool8_t requires_update =
      submesh->pipeline_dirty || submesh->pipeline.id != desired_pipeline.id ||
      submesh->pipeline.generation != desired_pipeline.generation;

  if (!requires_update) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  if (submesh->pipeline.id != 0) {
    VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        manager->pipeline_registry, submesh->pipeline, submesh->instance_state,
        &rel_err);
  }

  VkrRendererError acq_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          manager->pipeline_registry, desired_pipeline,
          &submesh->instance_state, &acq_err)) {
    *out_error = acq_err;
    return false_v;
  }

  submesh->pipeline = desired_pipeline;
  submesh->pipeline_dirty = false_v;
  submesh->last_render_frame = 0;

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_mesh_manager_update_model(VkrMeshManager *manager, uint32_t index) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return;

  mesh->model = vkr_transform_get_world(&mesh->transform);

  vkr_mesh_update_world_bounds(mesh);

  for (uint32_t submesh_index = 0; submesh_index < mesh->submeshes.length;
       ++submesh_index) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, submesh_index);
    submesh->last_render_frame = 0;
  }
}

bool8_t vkr_mesh_manager_set_model(VkrMeshManager *manager, uint32_t index,
                                   Mat4 model) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return false_v;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return false_v;

  mesh->model = model;

  vkr_mesh_update_world_bounds(mesh);

  // Reset instance cache for all submeshes
  for (uint32_t submesh_index = 0; submesh_index < mesh->submeshes.length;
       ++submesh_index) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, submesh_index);
    submesh->last_render_frame = 0;
  }

  return true_v;
}

bool8_t vkr_mesh_manager_set_visible(VkrMeshManager *manager, uint32_t index,
                                     bool8_t visible) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return false_v;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return false_v;

  mesh->visible = visible;

  return true_v;
}

bool8_t vkr_mesh_manager_set_render_id(VkrMeshManager *manager, uint32_t index,
                                       uint32_t render_id) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->meshes.length)
    return false_v;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return false_v;

  mesh->render_id = render_id;

  return true_v;
}

uint32_t vkr_mesh_manager_submesh_count(const VkrMesh *mesh) {
  if (!mesh || !mesh->submeshes.data)
    return 0;
  return (uint32_t)mesh->submeshes.length;
}

VkrSubMesh *vkr_mesh_manager_get_submesh(VkrMeshManager *manager,
                                         uint32_t mesh_index,
                                         uint32_t submesh_index) {
  assert_log(manager != NULL, "Manager is NULL");

  if (mesh_index >= manager->meshes.length)
    return NULL;

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, mesh_index);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0)
    return NULL;

  if (submesh_index >= mesh->submeshes.length)
    return NULL;

  return array_get_VkrSubMesh(&mesh->submeshes, submesh_index);
}

uint32_t vkr_mesh_manager_load_batch(VkrMeshManager *manager,
                                     const VkrMeshLoadDesc *descs,
                                     uint32_t count, uint32_t *out_indices,
                                     VkrRendererError *out_errors) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(descs != NULL, "Descs is NULL");

  if (count == 0) {
    return 0;
  }

  // Initialize outputs
  if (out_indices) {
    for (uint32_t i = 0; i < count; i++) {
      out_indices[i] = VKR_INVALID_ID;
    }
  }
  if (out_errors) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
    }
  }

  uint32_t wave_size = vkr_mesh_manager_batch_wave_size(manager, count);

  VkrAllocator *scratch_allocator = &manager->scratch_allocator;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    if (out_errors) {
      for (uint32_t i = 0; i < count; i++) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    return 0;
  }

  String8 *mesh_paths =
      vkr_allocator_alloc(scratch_allocator, sizeof(String8) * wave_size,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrResourceHandleInfo *handle_infos = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrResourceHandleInfo) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *load_errors = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrRendererError) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!mesh_paths || !handle_infos || !load_errors) {
    if (out_errors) {
      for (uint32_t i = 0; i < count; i++) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }

  uint32_t entries_created = 0;
  uint32_t meshes_loaded_total = 0;

  for (uint32_t base = 0; base < count; base += wave_size) {
    uint32_t wave_end = vkr_min_u32(base + wave_size, count);
    uint32_t wave_count = wave_end - base;

    for (uint32_t j = 0; j < wave_count; j++) {
      mesh_paths[j] = descs[base + j].mesh_path;
    }

    uint32_t meshes_loaded = vkr_resource_system_load_batch(
        VKR_RESOURCE_TYPE_MESH, mesh_paths, wave_count, scratch_allocator,
        handle_infos, load_errors);
    meshes_loaded_total += meshes_loaded;

    for (uint32_t j = 0; j < wave_count; j++) {
      uint32_t mesh_index = VKR_INVALID_ID;
      VkrRendererError err = VKR_RENDERER_ERROR_NONE;

      uint32_t global_i = base + j;
      if (vkr_mesh_manager_process_resource_handle(manager, &handle_infos[j],
                                                   load_errors[j],
                                                   &descs[global_i], &mesh_index,
                                                   &err)) {
        if (out_indices) {
          out_indices[global_i] = mesh_index;
        }
        entries_created++;
      }

      if (out_errors) {
        out_errors[global_i] = err;
      }
    }

    // Unload mesh resources for this wave to release arena pool chunks.
    for (uint32_t j = 0; j < wave_count; j++) {
      if (handle_infos[j].type == VKR_RESOURCE_TYPE_MESH &&
          handle_infos[j].as.mesh) {
        vkr_resource_system_unload(&handle_infos[j], mesh_paths[j]);
      }
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  log_debug("Mesh manager batch: %u/%u meshes loaded from files",
            meshes_loaded_total, count);
  log_debug("Mesh manager batch complete: %u/%u mesh entries created",
            entries_created, count);
  return entries_created;
}
