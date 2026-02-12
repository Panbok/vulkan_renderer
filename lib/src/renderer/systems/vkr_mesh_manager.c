#include "renderer/systems/vkr_mesh_manager.h"

#include "containers/bitset.h"
#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
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
vkr_internal void
vkr_mesh_manager_build_geometry_key(char out_name[GEOMETRY_NAME_MAX_LENGTH],
                                    String8 mesh_path, uint32_t subset_index) {
  uint64_t hash = 14695981039346656037ull;
  hash = vkr_mesh_fnv1a_hash(mesh_path.str, mesh_path.length, hash);
  string_format(out_name, GEOMETRY_NAME_MAX_LENGTH, "mesh_%016llx_%u",
                (unsigned long long)hash, subset_index);
}

/**
 * @brief Build a stable geometry name for merged mesh buffers.
 */
vkr_internal void
vkr_mesh_manager_build_mesh_buffer_key(char out_name[GEOMETRY_NAME_MAX_LENGTH],
                                       String8 mesh_path) {
  uint64_t hash = 14695981039346656037ull;
  hash = vkr_mesh_fnv1a_hash(mesh_path.str, mesh_path.length, hash);
  string_format(out_name, GEOMETRY_NAME_MAX_LENGTH, "meshbuf_%016llx",
                (unsigned long long)hash);
}

// ============================================================================
// Mesh Asset Key (for loader-level deduplication)
// ============================================================================

/**
 * @brief Key for deduplicating mesh asset loads.
 *
 * Two load requests with identical keys will share the same loaded mesh
 * resource. The key combines mesh_path, pipeline_domain, and shader_override
 * since these affect the resulting submesh configuration.
 */
typedef struct VkrMeshAssetKey {
  String8 mesh_path;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
} VkrMeshAssetKey;

/**
 * @brief Check equality of two MeshAssetKeys.
 */
vkr_internal bool8_t vkr_mesh_asset_key_equals(const VkrMeshAssetKey *a,
                                               const VkrMeshAssetKey *b) {
  if (a->pipeline_domain != b->pipeline_domain) {
    return false_v;
  }
  if (!string8_equals(&a->mesh_path, &b->mesh_path)) {
    return false_v;
  }
  if (!string8_equals(&a->shader_override, &b->shader_override)) {
    return false_v;
  }
  return true_v;
}

vkr_internal VkrPipelineDomain vkr_mesh_manager_resolve_domain(
    VkrPipelineDomain primary, VkrPipelineDomain fallback);

/**
 * @brief Build a MeshAssetKey from a VkrMeshLoadDesc.
 */
vkr_internal VkrMeshAssetKey
vkr_mesh_asset_key_from_desc(const VkrMeshLoadDesc *desc) {
  VkrPipelineDomain domain =
      vkr_mesh_manager_resolve_domain(desc->pipeline_domain, 0);
  return (VkrMeshAssetKey){
      .mesh_path = desc->mesh_path,
      .pipeline_domain = domain,
      .shader_override = desc->shader_override,
  };
}

/**
 * @brief Find index of key in unique_keys array, or -1 if not found.
 */
vkr_internal int32_t vkr_mesh_asset_key_find(const VkrMeshAssetKey *unique_keys,
                                             uint32_t unique_count,
                                             const VkrMeshAssetKey *key) {
  for (uint32_t i = 0; i < unique_count; i++) {
    if (vkr_mesh_asset_key_equals(&unique_keys[i], key)) {
      return (int32_t)i;
    }
  }
  return -1;
}

/**
 * @brief Normalize pipeline domain with fallback semantics.
 *
 * Treats 0 as "unspecified" and resolves to fallback when provided, otherwise
 * defaults to VKR_PIPELINE_DOMAIN_WORLD for stable asset keys and pipelines.
 */
vkr_internal VkrPipelineDomain vkr_mesh_manager_resolve_domain(
    VkrPipelineDomain primary, VkrPipelineDomain fallback) {
  if (primary != 0) {
    return primary;
  }
  if (fallback != 0) {
    return fallback;
  }
  return VKR_PIPELINE_DOMAIN_WORLD;
}

vkr_internal VkrMeshAssetHandle vkr_mesh_manager_create_asset_from_handle_info(
    VkrMeshManager *manager, const VkrResourceHandleInfo *handle_info,
    const VkrMeshLoadDesc *desc, const char *key_buf,
    VkrRendererError *out_error);
vkr_internal VkrMeshAssetHandle vkr_mesh_manager_create_pending_asset_slot(
    VkrMeshManager *manager, const VkrMeshLoadDesc *desc, const char *key_buf,
    uint64_t pending_request_id, VkrRendererError *out_error);
vkr_internal bool8_t vkr_mesh_manager_build_asset_from_mesh_result(
    VkrMeshManager *manager, VkrMeshAsset *asset,
    const VkrMeshLoaderResult *mesh_result, const VkrMeshLoadDesc *desc,
    VkrRendererError *out_error);
vkr_internal bool8_t vkr_mesh_manager_sync_pending_asset(
    VkrMeshManager *manager, uint32_t slot, VkrMeshAsset *asset);
vkr_internal bool8_t vkr_mesh_manager_init_instance_state_array(
    VkrMeshManager *manager, VkrMeshInstance *instance, uint32_t submesh_count);
vkr_internal void
vkr_mesh_manager_release_instance_state_array(VkrMeshManager *manager,
                                              VkrMeshInstance *instance);
vkr_internal void
vkr_mesh_manager_refresh_instances_for_asset(VkrMeshManager *manager,
                                             uint32_t slot);

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
  return vkr_material_system_material_uses_cutout(material_system, material);
}

/**
 * @brief Compute bounding sphere for a mesh from its submesh geometries.
 * Unions all geometry AABBs then computes enclosing sphere.
 */
vkr_internal void vkr_mesh_compute_local_bounds(VkrMesh *mesh) {
  if (!mesh)
    return;

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

/**
 * @brief Update instance bounds from asset local bounds and model matrix.
 *
 * Uses max scale factor to stay conservative under non-uniform scale and
 * clears bounds when the asset has no valid bounds.
 */
vkr_internal void
vkr_mesh_manager_update_instance_bounds(VkrMeshInstance *instance,
                                        const VkrMeshAsset *asset, Mat4 model) {
  instance->bounds_valid = false_v;
  if (!asset || !asset->bounds_valid) {
    return;
  }

  instance->bounds_valid = true_v;
  instance->bounds_world_center =
      mat4_mul_vec3(model, asset->bounds_local_center);

  Vec3 col0 = vec3_new(model.m00, model.m10, model.m20);
  Vec3 col1 = vec3_new(model.m01, model.m11, model.m21);
  Vec3 col2 = vec3_new(model.m02, model.m12, model.m22);

  float32_t sx = vec3_length(col0);
  float32_t sy = vec3_length(col1);
  float32_t sz = vec3_length(col2);
  float32_t max_scale = vkr_max_f32(vkr_max_f32(sx, sy), sz);

  instance->bounds_world_radius = asset->bounds_local_radius * max_scale;
}

vkr_internal bool8_t vkr_mesh_manager_init_instance_state_array(
    VkrMeshManager *manager, VkrMeshInstance *instance,
    uint32_t submesh_count) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(instance != NULL, "Instance is NULL");

  if (submesh_count == 0) {
    return false_v;
  }

  instance->submesh_state = array_create_VkrMeshSubmeshInstanceState(
      &manager->instance_allocator, submesh_count);
  if (!instance->submesh_state.data) {
    return false_v;
  }

  for (uint32_t i = 0; i < submesh_count; ++i) {
    VkrMeshSubmeshInstanceState state = {0};
    state.instance_state.id = VKR_INVALID_ID;
    state.pipeline = VKR_PIPELINE_HANDLE_INVALID;
    state.pipeline_dirty = true_v;
    array_set_VkrMeshSubmeshInstanceState(&instance->submesh_state, i, state);
  }

  return true_v;
}

vkr_internal void
vkr_mesh_manager_release_instance_state_array(VkrMeshManager *manager,
                                              VkrMeshInstance *instance) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(instance != NULL, "Instance is NULL");

  if (!instance->submesh_state.data) {
    return;
  }

  for (uint32_t i = 0; i < instance->submesh_state.length; ++i) {
    VkrMeshSubmeshInstanceState *state =
        array_get_VkrMeshSubmeshInstanceState(&instance->submesh_state, i);
    if (!state || state->instance_state.id == VKR_INVALID_ID ||
        state->pipeline.id == 0) {
      continue;
    }

    VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_release_instance_state(
            manager->pipeline_registry, state->pipeline, state->instance_state,
            &rel_err)) {
      log_warn("MeshManager: failed to release instance state (pipeline=%u, "
               "generation=%u, state=%u, err=%d)",
               state->pipeline.id, state->pipeline.generation,
               state->instance_state.id, rel_err);
    }
    state->instance_state =
        (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID};
    state->pipeline = VKR_PIPELINE_HANDLE_INVALID;
    state->pipeline_dirty = true_v;
  }

  array_destroy_VkrMeshSubmeshInstanceState(&instance->submesh_state);
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

  if (submesh->pipeline.id != 0 &&
      submesh->instance_state.id != VKR_INVALID_ID) {
    VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        manager->pipeline_registry, submesh->pipeline, submesh->instance_state,
        &rel_err);
  }

  if (submesh->geometry.id != 0 && submesh->owns_geometry) {
    vkr_geometry_system_release(manager->geometry_system, submesh->geometry);
  }

  if (submesh->material.id != 0 && submesh->owns_material) {
    vkr_material_system_release(manager->material_system, submesh->material);
  }
  MemZero(submesh, sizeof(*submesh));
}

vkr_internal void
vkr_mesh_manager_release_asset_submesh(VkrMeshManager *manager,
                                       VkrMeshAssetSubmesh *submesh) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(submesh != NULL, "Submesh is NULL");

  if (submesh->geometry.id != 0 && submesh->owns_geometry) {
    vkr_geometry_system_release(manager->geometry_system, submesh->geometry);
  }

  if (submesh->material.id != 0 && submesh->owns_material) {
    vkr_material_system_release(manager->material_system, submesh->material);
  }

  if (submesh->shader_override.str && submesh->shader_override.length > 0) {
    vkr_allocator_free(&manager->asset_allocator, submesh->shader_override.str,
                       submesh->shader_override.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  MemZero(submesh, sizeof(*submesh));
}

vkr_internal void vkr_mesh_manager_free_asset_strings(VkrMeshManager *manager,
                                                      VkrMeshAsset *asset) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(asset != NULL, "Asset is NULL");

  if (asset->mesh_path.str && asset->mesh_path.length > 0) {
    vkr_allocator_free(&manager->asset_allocator, asset->mesh_path.str,
                       asset->mesh_path.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (asset->shader_override.str && asset->shader_override.length > 0) {
    vkr_allocator_free(&manager->asset_allocator, asset->shader_override.str,
                       asset->shader_override.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
}

vkr_internal void vkr_mesh_manager_destroy_asset_slot(VkrMeshManager *manager,
                                                      uint32_t slot,
                                                      bool8_t adjust_count) {
  assert_log(manager != NULL, "Manager is NULL");

  if (slot >= manager->mesh_assets.length) {
    return;
  }

  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  if (!asset || asset->id == 0) {
    return;
  }

  if (asset->pending_request_id != 0 && asset->mesh_path.str &&
      asset->mesh_path.length > 0) {
    VkrResourceHandleInfo tracked_info = {0};
    tracked_info.type = VKR_RESOURCE_TYPE_MESH;
    tracked_info.request_id = asset->pending_request_id;
    vkr_resource_system_unload(&tracked_info, asset->mesh_path);
    asset->pending_request_id = 0;
  }

  if (asset->key_string) {
    vkr_hash_table_remove_VkrMeshAssetEntry(&manager->asset_by_key,
                                            asset->key_string);
    vkr_allocator_free(&manager->asset_allocator, asset->key_string,
                       string_length(asset->key_string) + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    asset->key_string = NULL;
  }

  if (asset->submeshes.data && asset->submeshes.length > 0) {
    for (uint32_t i = 0; i < asset->submeshes.length; ++i) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);
      if (submesh) {
        vkr_mesh_manager_release_asset_submesh(manager, submesh);
      }
    }
    array_destroy_VkrMeshAssetSubmesh(&asset->submeshes);
  }

  vkr_mesh_manager_free_asset_strings(manager, asset);

  MemZero(asset, sizeof(*asset));
  array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                     slot);
  manager->asset_free_count++;
  if (adjust_count && manager->asset_count > 0) {
    manager->asset_count--;
  }
}

vkr_internal VkrMeshAssetHandle vkr_mesh_manager_create_pending_asset_slot(
    VkrMeshManager *manager, const VkrMeshLoadDesc *desc, const char *key_buf,
    uint64_t pending_request_id, VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Desc is NULL");
  assert_log(key_buf != NULL, "Key buffer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  uint32_t slot = 0;
  if (manager->asset_free_count > 0) {
    slot = *array_get_uint32_t(&manager->asset_free_indices,
                               manager->asset_free_count - 1);
    manager->asset_free_count--;
  } else {
    if (manager->next_asset_index >= manager->mesh_assets.length) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return VKR_MESH_ASSET_HANDLE_INVALID;
    }
    slot = manager->next_asset_index++;
  }

  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  MemZero(asset, sizeof(*asset));

  asset->id = slot + 1;
  asset->generation = manager->asset_generation_counter++;
  asset->domain = vkr_mesh_manager_resolve_domain(desc->pipeline_domain, 0);
  asset->loading_state = pending_request_id != 0
                             ? VKR_MESH_LOADING_STATE_PENDING
                             : VKR_MESH_LOADING_STATE_NOT_LOADED;
  asset->last_error = VKR_RENDERER_ERROR_NONE;
  asset->pending_request_id = pending_request_id;
  asset->ref_count = 0;

  asset->mesh_path =
      string8_duplicate(&manager->asset_allocator, &desc->mesh_path);
  if (!asset->mesh_path.str || asset->mesh_path.length == 0) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    MemZero(asset, sizeof(*asset));
    array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                       slot);
    manager->asset_free_count++;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  if (desc->shader_override.str && desc->shader_override.length > 0) {
    asset->shader_override =
        string8_duplicate(&manager->asset_allocator, &desc->shader_override);
    if (!asset->shader_override.str || asset->shader_override.length == 0) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      vkr_mesh_manager_free_asset_strings(manager, asset);
      MemZero(asset, sizeof(*asset));
      array_set_uint32_t(&manager->asset_free_indices,
                         manager->asset_free_count, slot);
      manager->asset_free_count++;
      return VKR_MESH_ASSET_HANDLE_INVALID;
    }
  }

  char *key_copy =
      vkr_allocator_alloc(&manager->asset_allocator, string_length(key_buf) + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!key_copy) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_mesh_manager_free_asset_strings(manager, asset);
    MemZero(asset, sizeof(*asset));
    array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                       slot);
    manager->asset_free_count++;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  string_copy(key_copy, key_buf);
  asset->key_string = key_copy;
  VkrMeshAssetEntry entry = {.asset_index = slot, .key = key_copy};
  if (!vkr_hash_table_insert_VkrMeshAssetEntry(&manager->asset_by_key, key_copy,
                                               entry)) {
    vkr_allocator_free(&manager->asset_allocator, key_copy,
                       string_length(key_copy) + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    asset->key_string = NULL;
    vkr_mesh_manager_free_asset_strings(manager, asset);
    MemZero(asset, sizeof(*asset));
    array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                       slot);
    manager->asset_free_count++;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  manager->asset_count++;
  return (VkrMeshAssetHandle){.id = asset->id, .generation = asset->generation};
}

vkr_internal void
vkr_mesh_manager_refresh_instances_for_asset(VkrMeshManager *manager,
                                             uint32_t slot) {
  assert_log(manager != NULL, "Manager is NULL");

  if (slot >= manager->mesh_assets.length) {
    return;
  }

  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  if (!asset || asset->id == 0) {
    return;
  }

  for (uint32_t i = 0; i < manager->mesh_instances.length; ++i) {
    VkrMeshInstance *instance =
        array_get_VkrMeshInstance(&manager->mesh_instances, i);
    if (!instance || instance->asset.id != asset->id ||
        instance->asset.generation != asset->generation) {
      continue;
    }

    if (asset->loading_state == VKR_MESH_LOADING_STATE_FAILED) {
      vkr_mesh_manager_release_instance_state_array(manager, instance);
      instance->loading_state = VKR_MESH_LOADING_STATE_FAILED;
      instance->bounds_valid = false_v;
      continue;
    }

    if (asset->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      instance->loading_state = VKR_MESH_LOADING_STATE_PENDING;
      continue;
    }

    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    if (submesh_count == 0) {
      instance->loading_state = VKR_MESH_LOADING_STATE_FAILED;
      instance->bounds_valid = false_v;
      continue;
    }

    if (!instance->submesh_state.data &&
        !vkr_mesh_manager_init_instance_state_array(manager, instance,
                                                    submesh_count)) {
      instance->loading_state = VKR_MESH_LOADING_STATE_FAILED;
      instance->bounds_valid = false_v;
      continue;
    }

    instance->loading_state = VKR_MESH_LOADING_STATE_LOADED;
    vkr_mesh_manager_update_instance_bounds(instance, asset, instance->model);
  }
}

vkr_internal bool8_t vkr_mesh_manager_sync_pending_asset(
    VkrMeshManager *manager, uint32_t slot, VkrMeshAsset *asset) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(asset != NULL, "Asset is NULL");

  if (asset->loading_state != VKR_MESH_LOADING_STATE_PENDING ||
      asset->pending_request_id == 0) {
    return true_v;
  }

  VkrResourceHandleInfo tracked_info = {
      .type = VKR_RESOURCE_TYPE_MESH,
      .request_id = asset->pending_request_id,
  };

  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState state =
      vkr_resource_system_get_state(&tracked_info, &state_error);
  if (state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
      state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
      state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
    return true_v;
  }

  if (state != VKR_RESOURCE_LOAD_STATE_READY) {
    if (asset->mesh_path.str && asset->mesh_path.length > 0) {
      vkr_resource_system_unload(&tracked_info, asset->mesh_path);
    }
    asset->pending_request_id = 0;
    asset->loading_state = VKR_MESH_LOADING_STATE_FAILED;
    asset->last_error = state_error != VKR_RENDERER_ERROR_NONE
                            ? state_error
                            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    vkr_mesh_manager_refresh_instances_for_asset(manager, slot);
    return false_v;
  }

  VkrResourceHandleInfo resolved_info = {0};
  if (!vkr_resource_system_try_get_resolved(&tracked_info, &resolved_info)) {
    return true_v;
  }
  if (resolved_info.type != VKR_RESOURCE_TYPE_MESH || !resolved_info.as.mesh) {
    if (asset->mesh_path.str && asset->mesh_path.length > 0) {
      vkr_resource_system_unload(&tracked_info, asset->mesh_path);
    }
    asset->pending_request_id = 0;
    asset->loading_state = VKR_MESH_LOADING_STATE_FAILED;
    asset->last_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    vkr_mesh_manager_refresh_instances_for_asset(manager, slot);
    return false_v;
  }

  VkrMeshLoadDesc desc = {
      .mesh_path = asset->mesh_path,
      .pipeline_domain = asset->domain,
      .shader_override = asset->shader_override,
  };

  VkrRendererError build_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_build_asset_from_mesh_result(
          manager, asset, resolved_info.as.mesh, &desc, &build_error)) {
    if (asset->mesh_path.str && asset->mesh_path.length > 0) {
      vkr_resource_system_unload(&tracked_info, asset->mesh_path);
    }
    asset->pending_request_id = 0;
    asset->loading_state = VKR_MESH_LOADING_STATE_FAILED;
    asset->last_error = build_error != VKR_RENDERER_ERROR_NONE
                            ? build_error
                            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    vkr_mesh_manager_refresh_instances_for_asset(manager, slot);
    return false_v;
  }

  if (asset->mesh_path.str && asset->mesh_path.length > 0) {
    vkr_resource_system_unload(&tracked_info, asset->mesh_path);
  }

  asset->pending_request_id = 0;
  asset->last_error = VKR_RENDERER_ERROR_NONE;
  asset->loading_state = VKR_MESH_LOADING_STATE_LOADED;
  vkr_mesh_manager_refresh_instances_for_asset(manager, slot);
  return true_v;
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

vkr_internal bool8_t vkr_mesh_manager_resolve_subset_geometries_batch(
    VkrMeshManager *manager, VkrMeshLoaderResult *mesh_result,
    uint32_t subset_count, VkrGeometryHandle *out_geometries,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(mesh_result != NULL, "Mesh result is NULL");
  assert_log(out_geometries != NULL, "Out geometries is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;
  for (uint32_t i = 0; i < subset_count; ++i) {
    out_geometries[i] = VKR_GEOMETRY_HANDLE_INVALID;
  }

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&manager->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrGeometryConfig *pending_configs = vkr_allocator_alloc(
      &manager->scratch_allocator, sizeof(VkrGeometryConfig) * subset_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *pending_subset_indices = vkr_allocator_alloc(
      &manager->scratch_allocator, sizeof(uint32_t) * subset_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!pending_configs || !pending_subset_indices) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  uint32_t pending_count = 0;
  for (uint32_t i = 0; i < subset_count; ++i) {
    VkrMeshLoaderSubset *subset =
        array_get_VkrMeshLoaderSubset(&mesh_result->subsets, i);
    if (!subset) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      goto fail_cleanup;
    }

    vkr_mesh_manager_build_geometry_key(subset->geometry_config.name,
                                        mesh_result->source_path, i);
    uint64_t name_length = string_length(subset->geometry_config.name);
    String8 geometry_name =
        string8_create((uint8_t *)subset->geometry_config.name, name_length);

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    VkrGeometryHandle geometry = vkr_geometry_system_acquire_by_name(
        manager->geometry_system, geometry_name, true_v, &geo_err);
    if (geometry.id != 0) {
      out_geometries[i] = geometry;
      continue;
    }

    if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
      *out_error = geo_err;
      goto fail_cleanup;
    }

    pending_configs[pending_count] = subset->geometry_config;
    pending_subset_indices[pending_count] = i;
    pending_count++;
  }

  if (pending_count > 0) {
    VkrGeometryHandle *pending_handles = vkr_allocator_alloc(
        &manager->scratch_allocator, sizeof(VkrGeometryHandle) * pending_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrRendererError *pending_errors = vkr_allocator_alloc(
        &manager->scratch_allocator, sizeof(VkrRendererError) * pending_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!pending_handles || !pending_errors) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      goto fail_cleanup;
    }

    vkr_geometry_system_create_batch(manager->geometry_system, pending_configs,
                                     pending_count, true_v, pending_handles,
                                     pending_errors);
    for (uint32_t i = 0; i < pending_count; ++i) {
      if (pending_handles[i].id == 0) {
        *out_error = pending_errors[i];
        if (*out_error == VKR_RENDERER_ERROR_NONE ||
            *out_error == VKR_RENDERER_ERROR_UNKNOWN) {
          *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        }
        for (uint32_t j = i + 1; j < pending_count; ++j) {
          if (pending_handles[j].id != 0) {
            vkr_geometry_system_release(manager->geometry_system,
                                        pending_handles[j]);
            pending_handles[j] = VKR_GEOMETRY_HANDLE_INVALID;
          }
        }
        goto fail_cleanup;
      }
      out_geometries[pending_subset_indices[i]] = pending_handles[i];
    }
  }

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;

fail_cleanup:
  for (uint32_t i = 0; i < subset_count; ++i) {
    if (out_geometries[i].id != 0) {
      vkr_geometry_system_release(manager->geometry_system, out_geometries[i]);
      out_geometries[i] = VKR_GEOMETRY_HANDLE_INVALID;
    }
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return false_v;
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
  manager->mesh_live_indices = array_create_uint32_t(
      &manager->allocator, manager->config.max_mesh_count);
  manager->free_indices = array_create_uint32_t(&manager->allocator,
                                                manager->config.max_mesh_count);
  manager->free_count = 0;
  manager->mesh_count = 0;
  manager->next_free_index = 0;

  for (uint32_t i = 0; i < manager->meshes.length; ++i) {
    VkrMesh empty = {0};
    array_set_VkrMesh(&manager->meshes, i, empty);
  }

  if (!vkr_dmemory_create(MB(2), MB(8), &manager->asset_dmemory)) {
    log_error("Failed to create mesh manager asset dmemory");
    return false_v;
  }
  manager->asset_allocator.ctx = &manager->asset_dmemory;
  vkr_dmemory_allocator_create(&manager->asset_allocator);

  if (!vkr_dmemory_create(MB(32), MB(128), &manager->instance_dmemory)) {
    log_error("Failed to create mesh manager instance dmemory");
    vkr_dmemory_allocator_destroy(&manager->asset_allocator);
    vkr_dmemory_destroy(&manager->asset_dmemory);
    return false_v;
  }
  manager->instance_allocator.ctx = &manager->instance_dmemory;
  vkr_dmemory_allocator_create(&manager->instance_allocator);

  uint32_t max_assets = manager->config.max_mesh_count;
  manager->mesh_assets =
      array_create_VkrMeshAsset(&manager->allocator, max_assets);
  manager->asset_free_indices =
      array_create_uint32_t(&manager->allocator, max_assets);
  manager->asset_free_count = 0;
  manager->asset_count = 0;
  manager->next_asset_index = 0;
  manager->asset_generation_counter = 1;
  manager->asset_by_key = vkr_hash_table_create_VkrMeshAssetEntry(
      &manager->allocator, max_assets * 2);

  for (uint32_t i = 0; i < manager->mesh_assets.length; ++i) {
    VkrMeshAsset empty = {0};
    array_set_VkrMeshAsset(&manager->mesh_assets, i, empty);
  }

  uint32_t max_instances = manager->config.max_mesh_count;
  manager->mesh_instances =
      array_create_VkrMeshInstance(&manager->allocator, max_instances);
  manager->instance_live_indices =
      array_create_uint32_t(&manager->allocator, max_instances);
  manager->instance_free_indices =
      array_create_uint32_t(&manager->allocator, max_instances);
  manager->instance_free_count = 0;
  manager->instance_count = 0;
  manager->next_instance_index = 0;
  manager->instance_generation_counter = 1;

  for (uint32_t i = 0; i < manager->mesh_instances.length; ++i) {
    VkrMeshInstance empty = {0};
    array_set_VkrMeshInstance(&manager->mesh_instances, i, empty);
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

  for (uint32_t i = 0; i < manager->mesh_instances.length; ++i) {
    VkrMeshInstance *inst =
        array_get_VkrMeshInstance(&manager->mesh_instances, i);
    vkr_mesh_manager_release_instance_state_array(manager, inst);
  }
  array_destroy_VkrMeshInstance(&manager->mesh_instances);
  array_destroy_uint32_t(&manager->instance_live_indices);
  array_destroy_uint32_t(&manager->instance_free_indices);
  vkr_dmemory_allocator_destroy(&manager->instance_allocator);

  for (uint32_t i = 0; i < manager->mesh_assets.length; ++i) {
    vkr_mesh_manager_destroy_asset_slot(manager, i, false_v);
  }
  array_destroy_VkrMeshAsset(&manager->mesh_assets);
  array_destroy_uint32_t(&manager->asset_free_indices);
  vkr_hash_table_destroy_VkrMeshAssetEntry(&manager->asset_by_key);
  vkr_dmemory_allocator_destroy(&manager->asset_allocator);

  array_destroy_VkrMesh(&manager->meshes);
  array_destroy_uint32_t(&manager->mesh_live_indices);
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
    bool8_t uses_full_geometry =
        (sub_desc->index_count == 0 && sub_desc->first_index == 0 &&
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
        .instance_state =
            (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID},
        .pipeline_domain =
            vkr_mesh_manager_resolve_domain(sub_desc->pipeline_domain, 0),
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

  vkr_mesh_compute_local_bounds(&new_mesh);
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

  new_mesh.live_index = manager->mesh_count;
  array_set_VkrMesh(&manager->meshes, slot, new_mesh);
  array_set_uint32_t(&manager->mesh_live_indices, new_mesh.live_index, slot);
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
  bool8_t use_merged = mesh_result->has_mesh_buffer &&
                       mesh_result->mesh_buffer.vertex_count > 0 &&
                       mesh_result->mesh_buffer.index_count > 0 &&
                       mesh_result->submeshes.length > 0 &&
                       mesh_result->submeshes.data;
  if (!use_merged &&
      (mesh_result->subsets.length == 0 || !mesh_result->subsets.data)) {
    log_error("MeshManager: mesh '%.*s' returned no subsets",
              (int)desc->mesh_path.length, desc->mesh_path.str);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }

  uint32_t subset_count = use_merged ? (uint32_t)mesh_result->submeshes.length
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
  VkrGeometryHandle *resolved_subset_geometries = NULL;
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
        VkrMeshLoaderSubmeshRange *range = &mesh_result->submeshes.data[i];
        if (!vkr_mesh_manager_material_uses_cutout(manager->material_system,
                                                   range->material_handle)) {
          opaque_index_count += range->index_count;
        }
      }

      if (opaque_index_count > 0 && opaque_index_count < total_indices) {
        build_opaque_indices = true_v;
        opaque_ranges = vkr_allocator_alloc(scratch_allocator,
                                            (uint64_t)subset_count *
                                                sizeof(VkrOpaqueRangeInfo),
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
          opaque_indices = vkr_allocator_alloc(scratch_allocator,
                                               (uint64_t)opaque_index_count *
                                                   sizeof(uint32_t),
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
            VkrMeshLoaderSubmeshRange *range = &mesh_result->submeshes.data[i];
            if (vkr_mesh_manager_material_uses_cutout(manager->material_system,
                                                      range->material_handle)) {
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
    String8 geometry_name = string8_create((uint8_t *)geometry_name_buf,
                                           string_length(geometry_name_buf));

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    merged_geometry = vkr_geometry_system_acquire_by_name(
        manager->geometry_system, geometry_name, true_v, &geo_err);
    if (merged_geometry.id == 0) {
      if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        if (out_error) {
          *out_error = geo_err;
        }
        subsets_success = false_v;
      } else {
        Vec3 union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
        Vec3 union_max =
            vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
        bool8_t has_bounds = false_v;

        for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
          VkrMeshLoaderSubmeshRange *range = &mesh_result->submeshes.data[i];
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
          String8 debug_name = string8_create((uint8_t *)geometry->name,
                                              string_length(geometry->name));
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

  if (subsets_success && !use_merged) {
    resolved_subset_geometries = vkr_allocator_alloc(
        scratch_allocator, sizeof(VkrGeometryHandle) * subset_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!resolved_subset_geometries) {
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
      subsets_success = false_v;
    } else if (!vkr_mesh_manager_resolve_subset_geometries_batch(
                   manager, mesh_result, subset_count,
                   resolved_subset_geometries, out_error)) {
      subsets_success = false_v;
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

      VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
          range->pipeline_domain, desc->pipeline_domain);

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
          .opaque_first_index = (build_opaque_indices && opaque_ranges)
                                    ? opaque_ranges[i].first_index
                                    : 0,
          .opaque_index_count = (build_opaque_indices && opaque_ranges)
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

    if (!resolved_subset_geometries) {
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      }
      subsets_success = false_v;
      break;
    }

    VkrGeometryHandle geometry = resolved_subset_geometries[i];
    if (geometry.id == 0) {
      if (out_error && *out_error == VKR_RENDERER_ERROR_NONE) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      }
      subsets_success = false_v;
      break;
    }
    resolved_subset_geometries[i] = VKR_GEOMETRY_HANDLE_INVALID;

    VkrMaterialHandle material = subset->material_handle;
    bool8_t owns_material = true_v;
    if (material.id == 0) {
      material = manager->material_system->default_material;
      owns_material = false_v;
    }

    VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
        subset->pipeline_domain, desc->pipeline_domain);

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
    if (resolved_subset_geometries) {
      for (uint32_t i = 0; i < subset_count; ++i) {
        if (resolved_subset_geometries[i].id != 0) {
          vkr_geometry_system_release(manager->geometry_system,
                                      resolved_subset_geometries[i]);
          resolved_subset_geometries[i] = VKR_GEOMETRY_HANDLE_INVALID;
        }
      }
    }
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

  uint32_t live_index = mesh->live_index;

  vkr_mesh_manager_release_handles(manager, mesh);
  array_destroy_VkrSubMesh(&mesh->submeshes);

  if (manager->mesh_count > 0 && live_index < manager->mesh_count) {
    uint32_t last_index = manager->mesh_count - 1;
    uint32_t last_slot =
        *array_get_uint32_t(&manager->mesh_live_indices, last_index);
    array_set_uint32_t(&manager->mesh_live_indices, live_index, last_slot);
    if (last_slot != index) {
      VkrMesh *moved = array_get_VkrMesh(&manager->meshes, last_slot);
      moved->live_index = live_index;
    }
    manager->mesh_count--;
  }

  MemZero(mesh, sizeof(*mesh));

  if (manager->free_count < manager->free_indices.length) {
    manager->free_indices.data[manager->free_count++] = index;
  }

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

VkrMesh *vkr_mesh_manager_get_mesh_by_live_index(VkrMeshManager *manager,
                                                 uint32_t live_index,
                                                 uint32_t *out_slot) {
  assert_log(manager != NULL, "Manager is NULL");

  if (live_index >= manager->mesh_count) {
    return NULL;
  }

  uint32_t slot = *array_get_uint32_t(&manager->mesh_live_indices, live_index);
  if (slot >= manager->meshes.length) {
    return NULL;
  }

  VkrMesh *mesh = array_get_VkrMesh(&manager->meshes, slot);
  if (!mesh || !mesh->submeshes.data || mesh->submeshes.length == 0) {
    return NULL;
  }

  if (out_slot) {
    *out_slot = slot;
  }

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

  if (submesh->pipeline.id != 0 &&
      submesh->instance_state.id != VKR_INVALID_ID) {
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

  // Allocate arrays for deduplication within each wave.
  // unique_keys: array of unique MeshAssetKeys found in the wave
  // desc_to_unique: maps wave desc index -> unique key index
  // unique_paths: mesh paths for unique keys (for resource system)
  VkrMeshAssetKey *unique_keys = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrMeshAssetKey) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *desc_to_unique =
      vkr_allocator_alloc(scratch_allocator, sizeof(uint32_t) * wave_size,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  String8 *unique_paths =
      vkr_allocator_alloc(scratch_allocator, sizeof(String8) * wave_size,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrResourceHandleInfo *handle_infos = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrResourceHandleInfo) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *load_errors = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrRendererError) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!unique_keys || !desc_to_unique || !unique_paths || !handle_infos ||
      !load_errors) {
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
  uint32_t meshes_deduplicated_total = 0;

  for (uint32_t base = 0; base < count; base += wave_size) {
    uint32_t wave_end = vkr_min_u32(base + wave_size, count);
    uint32_t wave_count = wave_end - base;

    // Build unique keys for this wave.
    uint32_t unique_count = 0;
    for (uint32_t j = 0; j < wave_count; j++) {
      VkrMeshAssetKey key = vkr_mesh_asset_key_from_desc(&descs[base + j]);
      int32_t existing_idx =
          vkr_mesh_asset_key_find(unique_keys, unique_count, &key);
      if (existing_idx >= 0) {
        // Duplicate key - reuse existing unique entry
        desc_to_unique[j] = (uint32_t)existing_idx;
      } else {
        // New unique key
        desc_to_unique[j] = unique_count;
        unique_keys[unique_count] = key;
        unique_paths[unique_count] = key.mesh_path;
        unique_count++;
      }
    }

    meshes_deduplicated_total += (wave_count - unique_count);

    // Load only unique mesh paths.
    uint32_t meshes_loaded = vkr_resource_system_load_batch_sync(
        VKR_RESOURCE_TYPE_MESH, unique_paths, unique_count, scratch_allocator,
        handle_infos, load_errors);
    meshes_loaded_total += meshes_loaded;

    for (uint32_t j = 0; j < wave_count; j++) {
      uint32_t mesh_index = VKR_INVALID_ID;
      VkrRendererError err = VKR_RENDERER_ERROR_NONE;

      uint32_t global_i = base + j;
      uint32_t unique_idx = desc_to_unique[j];

      if (vkr_mesh_manager_process_resource_handle(
              manager, &handle_infos[unique_idx], load_errors[unique_idx],
              &descs[global_i], &mesh_index, &err)) {
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
    // Only unload unique handles once.
    for (uint32_t j = 0; j < unique_count; j++) {
      if (handle_infos[j].type == VKR_RESOURCE_TYPE_MESH &&
          handle_infos[j].as.mesh) {
        vkr_resource_system_unload(&handle_infos[j], unique_paths[j]);
      }
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  log_debug(
      "Mesh manager batch: %u unique files loaded (%u duplicates skipped)",
      meshes_loaded_total, meshes_deduplicated_total);
  log_debug("Mesh manager batch complete: %u/%u mesh entries created",
            entries_created, count);
  return entries_created;
}

// ============================================================================
// Mesh Asset API
// ============================================================================

VkrMeshAssetHandle vkr_mesh_manager_acquire_asset(VkrMeshManager *manager,
                                                  String8 mesh_path,
                                                  VkrPipelineDomain domain,
                                                  String8 shader_override,
                                                  VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrPipelineDomain normalized_domain =
      vkr_mesh_manager_resolve_domain(domain, 0);

  char key_buf[512];
  const char *shader_str =
      shader_override.str ? (const char *)shader_override.str : "";
  string_format(key_buf, sizeof(key_buf), "%.*s|%u|%.*s", (int)mesh_path.length,
                mesh_path.str, (uint32_t)normalized_domain,
                (int)shader_override.length, shader_str);

  VkrMeshAssetEntry *existing =
      vkr_hash_table_get_VkrMeshAssetEntry(&manager->asset_by_key, key_buf);
  if (existing) {
    VkrMeshAsset *asset =
        array_get_VkrMeshAsset(&manager->mesh_assets, existing->asset_index);
    if (asset && asset->id != 0) {
      (void)vkr_mesh_manager_sync_pending_asset(manager, existing->asset_index,
                                                asset);
      asset->ref_count++;
      return (VkrMeshAssetHandle){.id = asset->id,
                                  .generation = asset->generation};
    }
  }

  VkrAllocator *scratch_allocator = &manager->scratch_allocator;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  VkrMeshLoadDesc desc = {
      .mesh_path = mesh_path,
      .pipeline_domain = normalized_domain,
      .shader_override = shader_override,
  };

  VkrResourceHandleInfo request_info = {0};
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, mesh_path,
                                scratch_allocator, &request_info, out_error)) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  VkrMeshAssetHandle asset_handle = vkr_mesh_manager_create_pending_asset_slot(
      manager, &desc, key_buf, request_info.request_id, out_error);
  if (asset_handle.id == 0) {
    if (request_info.request_id != 0) {
      vkr_resource_system_unload(&request_info, mesh_path);
    } else if (request_info.type == VKR_RESOURCE_TYPE_MESH &&
               request_info.as.mesh) {
      vkr_resource_system_unload(&request_info, mesh_path);
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  uint32_t slot = asset_handle.id - 1;
  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  if (!asset || asset->id != asset_handle.id ||
      asset->generation != asset_handle.generation) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    if (request_info.request_id != 0 || request_info.as.mesh) {
      vkr_resource_system_unload(&request_info, mesh_path);
    }
    vkr_mesh_manager_destroy_asset_slot(manager, slot, true_v);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  if (request_info.request_id == 0) {
    if (request_info.type != VKR_RESOURCE_TYPE_MESH || !request_info.as.mesh) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      vkr_mesh_manager_destroy_asset_slot(manager, slot, true_v);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return VKR_MESH_ASSET_HANDLE_INVALID;
    }

    VkrRendererError build_error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_mesh_manager_build_asset_from_mesh_result(
            manager, asset, request_info.as.mesh, &desc, &build_error)) {
      *out_error = build_error != VKR_RENDERER_ERROR_NONE
                       ? build_error
                       : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      vkr_resource_system_unload(&request_info, mesh_path);
      vkr_mesh_manager_destroy_asset_slot(manager, slot, true_v);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return VKR_MESH_ASSET_HANDLE_INVALID;
    }

    asset->loading_state = VKR_MESH_LOADING_STATE_LOADED;
    asset->last_error = VKR_RENDERER_ERROR_NONE;
    asset->pending_request_id = 0;
    vkr_resource_system_unload(&request_info, mesh_path);
  } else {
    asset->loading_state = VKR_MESH_LOADING_STATE_PENDING;
    asset->last_error = VKR_RENDERER_ERROR_NONE;
    asset->pending_request_id = request_info.request_id;
    (void)vkr_mesh_manager_sync_pending_asset(manager, slot, asset);
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  asset = vkr_mesh_manager_get_asset(manager, asset_handle);
  if (asset) {
    asset->ref_count++;
  }

  return asset_handle;
}

void vkr_mesh_manager_release_asset(VkrMeshManager *manager,
                                    VkrMeshAssetHandle asset) {
  assert_log(manager != NULL, "Manager is NULL");

  if (asset.id == 0) {
    return;
  }

  uint32_t slot = asset.id - 1;
  if (slot >= manager->mesh_assets.length) {
    return;
  }

  VkrMeshAsset *a = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  if (!a || a->id != asset.id || a->generation != asset.generation) {
    return;
  }

  if (a->ref_count > 0) {
    a->ref_count--;
  }
  if (a->ref_count == 0) {
    vkr_mesh_manager_destroy_asset_slot(manager, slot, true_v);
  }
}

void vkr_mesh_manager_pump_async(VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");

  for (uint32_t i = 0; i < manager->mesh_assets.length; ++i) {
    VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, i);
    if (!asset || asset->id == 0 ||
        asset->loading_state != VKR_MESH_LOADING_STATE_PENDING) {
      continue;
    }
    (void)vkr_mesh_manager_sync_pending_asset(manager, i, asset);
  }
}

VkrMeshAsset *vkr_mesh_manager_get_asset(VkrMeshManager *manager,
                                         VkrMeshAssetHandle handle) {
  assert_log(manager != NULL, "Manager is NULL");

  if (handle.id == 0) {
    return NULL;
  }

  uint32_t slot = handle.id - 1;
  if (slot >= manager->mesh_assets.length) {
    return NULL;
  }

  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  if (!asset || asset->id != handle.id ||
      asset->generation != handle.generation) {
    return NULL;
  }

  if (asset->loading_state == VKR_MESH_LOADING_STATE_PENDING) {
    (void)vkr_mesh_manager_sync_pending_asset(manager, slot, asset);
  }

  return asset;
}

uint32_t vkr_mesh_manager_asset_count(const VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  return manager->asset_count;
}

// ============================================================================
// Mesh Instance API
// ============================================================================

VkrMeshInstanceHandle vkr_mesh_manager_create_instance(
    VkrMeshManager *manager, VkrMeshAssetHandle asset_handle, Mat4 model,
    uint32_t render_id, bool8_t visible, VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrMeshAsset *asset = vkr_mesh_manager_get_asset(manager, asset_handle);
  if (!asset) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return VKR_MESH_INSTANCE_HANDLE_INVALID;
  }

  if (asset->loading_state == VKR_MESH_LOADING_STATE_FAILED) {
    *out_error = asset->last_error != VKR_RENDERER_ERROR_NONE
                     ? asset->last_error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return VKR_MESH_INSTANCE_HANDLE_INVALID;
  }

  uint32_t slot;
  if (manager->instance_free_count > 0) {
    slot = *array_get_uint32_t(&manager->instance_free_indices,
                               manager->instance_free_count - 1);
    manager->instance_free_count--;
  } else {
    if (manager->next_instance_index >= manager->mesh_instances.length) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return VKR_MESH_INSTANCE_HANDLE_INVALID;
    }
    slot = manager->next_instance_index++;
  }

  VkrMeshInstance *inst =
      array_get_VkrMeshInstance(&manager->mesh_instances, slot);
  MemZero(inst, sizeof(*inst));

  inst->asset = asset_handle;
  inst->generation = manager->instance_generation_counter++;
  inst->live_index = manager->instance_count;
  inst->model = model;
  inst->render_id = render_id;
  inst->visible = visible;
  inst->loading_state = VKR_MESH_LOADING_STATE_PENDING;

  if (asset->loading_state == VKR_MESH_LOADING_STATE_LOADED) {
    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    if (submesh_count == 0 || !vkr_mesh_manager_init_instance_state_array(
                                  manager, inst, submesh_count)) {
      *out_error = submesh_count == 0
                       ? VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED
                       : VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      MemZero(inst, sizeof(*inst));
      array_set_uint32_t(&manager->instance_free_indices,
                         manager->instance_free_count, slot);
      manager->instance_free_count++;
      return VKR_MESH_INSTANCE_HANDLE_INVALID;
    }

    inst->loading_state = VKR_MESH_LOADING_STATE_LOADED;
    vkr_mesh_manager_update_instance_bounds(inst, asset, model);
  }

  array_set_uint32_t(&manager->instance_live_indices, inst->live_index, slot);
  asset->ref_count++;
  manager->instance_count++;

  return (VkrMeshInstanceHandle){.id = slot + 1,
                                 .generation = inst->generation};
}

VkrMeshInstanceHandle vkr_mesh_manager_create_instance_from_resource(
    VkrMeshManager *manager, const VkrMeshLoadDesc *desc,
    const VkrResourceHandleInfo *handle_info, uint32_t render_id,
    bool8_t visible, VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Desc is NULL");
  assert_log(handle_info != NULL, "Handle info is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (handle_info->type != VKR_RESOURCE_TYPE_MESH ||
      (!handle_info->as.mesh && handle_info->request_id == 0)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return VKR_MESH_INSTANCE_HANDLE_INVALID;
  }

  VkrPipelineDomain normalized_domain =
      vkr_mesh_manager_resolve_domain(desc->pipeline_domain, 0);
  const char *shader_str =
      desc->shader_override.str ? (const char *)desc->shader_override.str : "";

  char key_buf[512];
  string_format(key_buf, sizeof(key_buf), "%.*s|%u|%.*s",
                (int)desc->mesh_path.length, desc->mesh_path.str,
                (uint32_t)normalized_domain, (int)desc->shader_override.length,
                shader_str);

  VkrMeshAssetHandle asset_handle = VKR_MESH_ASSET_HANDLE_INVALID;
  bool8_t created_new_asset = false_v;

  VkrMeshAssetEntry *cached =
      vkr_hash_table_get_VkrMeshAssetEntry(&manager->asset_by_key, key_buf);
  if (cached) {
    VkrMeshAsset *asset =
        array_get_VkrMeshAsset(&manager->mesh_assets, cached->asset_index);
    if (asset && asset->id != 0) {
      (void)vkr_mesh_manager_sync_pending_asset(manager, cached->asset_index,
                                                asset);
      asset_handle = (VkrMeshAssetHandle){.id = asset->id,
                                          .generation = asset->generation};
    }
  }

  if (asset_handle.id == 0) {
    VkrMeshLoadDesc normalized_desc = *desc;
    normalized_desc.pipeline_domain = normalized_domain;

    VkrRendererError asset_error = VKR_RENDERER_ERROR_NONE;
    if (handle_info->as.mesh) {
      asset_handle = vkr_mesh_manager_create_asset_from_handle_info(
          manager, handle_info, &normalized_desc, key_buf, &asset_error);
    } else if (handle_info->request_id != 0) {
      /*
       * Callers may release their tracked request right after instance creation
       * (scene async finalize does this). Retain our own deduped request ref so
       * the asset can keep resolving independently of caller lifetime.
       */
      VkrAllocatorScope scope =
          vkr_allocator_begin_scope(&manager->scratch_allocator);
      if (!vkr_allocator_scope_is_valid(&scope)) {
        asset_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      } else {
        VkrResourceHandleInfo retained_request = {0};
        VkrRendererError retain_error = VKR_RENDERER_ERROR_NONE;
        if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, desc->mesh_path,
                                      &manager->scratch_allocator,
                                      &retained_request, &retain_error)) {
          asset_error = retain_error != VKR_RENDERER_ERROR_NONE
                            ? retain_error
                            : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
        } else if (retained_request.request_id != 0) {
          asset_handle = vkr_mesh_manager_create_pending_asset_slot(
              manager, &normalized_desc, key_buf, retained_request.request_id,
              &asset_error);
          if (asset_handle.id != 0) {
            uint32_t slot = asset_handle.id - 1;
            VkrMeshAsset *asset =
                array_get_VkrMeshAsset(&manager->mesh_assets, slot);
            if (asset && asset->id == asset_handle.id) {
              asset->loading_state = VKR_MESH_LOADING_STATE_PENDING;
              asset->last_error = VKR_RENDERER_ERROR_NONE;
              asset->pending_request_id = retained_request.request_id;
              (void)vkr_mesh_manager_sync_pending_asset(manager, slot, asset);
            }
          } else {
            vkr_resource_system_unload(&retained_request, desc->mesh_path);
          }
        } else if (retained_request.as.mesh) {
          asset_handle = vkr_mesh_manager_create_asset_from_handle_info(
              manager, &retained_request, &normalized_desc, key_buf,
              &asset_error);
        } else {
          asset_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        }
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    } else {
      VkrAllocatorScope scope =
          vkr_allocator_begin_scope(&manager->scratch_allocator);
      if (!vkr_allocator_scope_is_valid(&scope)) {
        asset_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      } else {
        VkrResourceHandleInfo owned_request = {0};
        bool8_t release_owned_request = false_v;
        VkrRendererError request_error = VKR_RENDERER_ERROR_NONE;
        if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, desc->mesh_path,
                                      &manager->scratch_allocator,
                                      &owned_request, &request_error)) {
          asset_error = request_error != VKR_RENDERER_ERROR_NONE
                            ? request_error
                            : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
        } else if (owned_request.request_id != 0) {
          asset_handle = vkr_mesh_manager_create_pending_asset_slot(
              manager, &normalized_desc, key_buf, owned_request.request_id,
              &asset_error);
          if (asset_handle.id != 0) {
            uint32_t slot = asset_handle.id - 1;
            VkrMeshAsset *asset =
                array_get_VkrMeshAsset(&manager->mesh_assets, slot);
            if (asset && asset->id == asset_handle.id) {
              asset->loading_state = VKR_MESH_LOADING_STATE_PENDING;
              asset->last_error = VKR_RENDERER_ERROR_NONE;
              asset->pending_request_id = owned_request.request_id;
              (void)vkr_mesh_manager_sync_pending_asset(manager, slot, asset);
            }
          } else {
            release_owned_request = true_v;
          }
        } else if (owned_request.as.mesh) {
          asset_handle = vkr_mesh_manager_create_asset_from_handle_info(
              manager, &owned_request, &normalized_desc, key_buf, &asset_error);
          release_owned_request = true_v;
        } else {
          asset_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        }

        if (release_owned_request) {
          vkr_resource_system_unload(&owned_request, desc->mesh_path);
        }

        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    }
    if (asset_handle.id == 0) {
      *out_error = asset_error != VKR_RENDERER_ERROR_NONE
                       ? asset_error
                       : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return VKR_MESH_INSTANCE_HANDLE_INVALID;
    }
    created_new_asset = true_v;
  }

  VkrTransform transform = desc->transform;
  Mat4 model = vkr_transform_get_world(&transform);
  VkrMeshInstanceHandle instance = vkr_mesh_manager_create_instance(
      manager, asset_handle, model, render_id, visible, out_error);
  if (instance.id == 0 && created_new_asset) {
    vkr_mesh_manager_release_asset(manager, asset_handle);
  }

  return instance;
}

vkr_internal bool8_t vkr_mesh_manager_build_asset_from_mesh_result(
    VkrMeshManager *manager, VkrMeshAsset *asset,
    const VkrMeshLoaderResult *mesh_result, const VkrMeshLoadDesc *desc,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(asset != NULL, "Asset is NULL");
  assert_log(mesh_result != NULL, "Mesh result is NULL");
  assert_log(desc != NULL, "Desc is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (asset->submeshes.data && asset->submeshes.length > 0) {
    for (uint32_t i = 0; i < asset->submeshes.length; ++i) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);
      if (submesh) {
        vkr_mesh_manager_release_asset_submesh(manager, submesh);
      }
    }
    array_destroy_VkrMeshAssetSubmesh(&asset->submeshes);
  }

  asset->bounds_valid = false_v;
  asset->bounds_local_center = vec3_zero();
  asset->bounds_local_radius = 0.0f;

  bool8_t use_merged = mesh_result->has_mesh_buffer &&
                       mesh_result->mesh_buffer.vertex_count > 0 &&
                       mesh_result->mesh_buffer.index_count > 0 &&
                       mesh_result->submeshes.length > 0 &&
                       mesh_result->submeshes.data;
  if (!use_merged &&
      (mesh_result->subsets.length == 0 || !mesh_result->subsets.data)) {
    log_error("MeshManager: mesh '%.*s' returned no subsets",
              (int)desc->mesh_path.length, desc->mesh_path.str);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  uint32_t subset_count = use_merged ? (uint32_t)mesh_result->submeshes.length
                                     : (uint32_t)mesh_result->subsets.length;
  asset->submeshes =
      array_create_VkrMeshAssetSubmesh(&manager->asset_allocator, subset_count);
  if (!asset->submeshes.data) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(asset->submeshes.data, subset_count * sizeof(VkrMeshAssetSubmesh));

  VkrGeometryHandle merged_geometry = VKR_GEOMETRY_HANDLE_INVALID;
  bool8_t subsets_success = true_v;

  if (use_merged) {
    char geometry_name_buf[GEOMETRY_NAME_MAX_LENGTH] = {0};
    vkr_mesh_manager_build_mesh_buffer_key(geometry_name_buf,
                                           mesh_result->source_path);
    String8 geometry_name = string8_create((uint8_t *)geometry_name_buf,
                                           string_length(geometry_name_buf));

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    merged_geometry = vkr_geometry_system_acquire_by_name(
        manager->geometry_system, geometry_name, true_v, &geo_err);

    if (merged_geometry.id == 0) {
      if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        *out_error = geo_err;
        subsets_success = false_v;
      } else {
        Vec3 union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
        Vec3 union_max =
            vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);

        for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
          const VkrMeshLoaderSubmeshRange *range =
              &mesh_result->submeshes.data[i];
          Vec3 range_min = vec3_add(range->center, range->min_extents);
          Vec3 range_max = vec3_add(range->center, range->max_extents);
          union_min.x = vkr_min_f32(union_min.x, range_min.x);
          union_min.y = vkr_min_f32(union_min.y, range_min.y);
          union_min.z = vkr_min_f32(union_min.z, range_min.z);
          union_max.x = vkr_max_f32(union_max.x, range_max.x);
          union_max.y = vkr_max_f32(union_max.y, range_max.y);
          union_max.z = vkr_max_f32(union_max.z, range_max.z);
        }

        Vec3 center = vec3_scale(vec3_add(union_min, union_max), 0.5f);
        Vec3 min_extents = vec3_sub(union_min, center);
        Vec3 max_extents = vec3_sub(union_max, center);

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
          *out_error = geo_err;
          subsets_success = false_v;
        }
      }
    }
  }

  Vec3 bounds_union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 bounds_union_max =
      vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  bool8_t has_bounds = false_v;

  uint32_t built_count = 0;
  for (uint32_t i = 0; subsets_success && i < subset_count; ++i) {
    VkrMeshAssetSubmesh *submesh =
        array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);

    if (use_merged) {
      const VkrMeshLoaderSubmeshRange *range = &mesh_result->submeshes.data[i];

      if (i > 0 && merged_geometry.id != 0) {
        vkr_geometry_system_acquire(manager->geometry_system, merged_geometry);
      }

      VkrMaterialHandle material = range->material_handle;
      bool8_t owns_material = true_v;
      if (material.id == 0) {
        material = manager->material_system->default_material;
        owns_material = false_v;
      }
      if (owns_material && material.id != 0) {
        vkr_material_system_add_ref(manager->material_system, material);
      }

      VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
          range->pipeline_domain, desc->pipeline_domain);

      String8 shader_override = range->shader_override.str
                                    ? range->shader_override
                                    : desc->shader_override;
      String8 shader_override_copy = {0};
      if (shader_override.str && shader_override.length > 0) {
        shader_override_copy =
            string8_duplicate(&manager->asset_allocator, &shader_override);
      }

      *submesh = (VkrMeshAssetSubmesh){
          .geometry = merged_geometry,
          .material = material,
          .shader_override = shader_override_copy,
          .pipeline_domain = domain,
          .range_id = range->range_id,
          .first_index = range->first_index,
          .index_count = range->index_count,
          .vertex_offset = range->vertex_offset,
          .center = range->center,
          .min_extents = range->min_extents,
          .max_extents = range->max_extents,
          .owns_geometry = true_v,
          .owns_material = owns_material,
      };
    } else {
      const VkrMeshLoaderSubset *subset = &mesh_result->subsets.data[i];
      VkrGeometryConfig geometry_config = subset->geometry_config;

      vkr_mesh_manager_build_geometry_key(geometry_config.name,
                                          mesh_result->source_path, i);
      uint64_t name_length = string_length(geometry_config.name);
      String8 geometry_name =
          string8_create((uint8_t *)geometry_config.name, name_length);

      VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
      VkrGeometryHandle geometry = vkr_geometry_system_acquire_by_name(
          manager->geometry_system, geometry_name, true_v, &geo_err);

      if (geometry.id == 0) {
        if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
          *out_error = geo_err;
          subsets_success = false_v;
          break;
        }
        geometry = vkr_geometry_system_create(
            manager->geometry_system, &geometry_config, true_v, &geo_err);
        if (geometry.id == 0) {
          *out_error = geo_err;
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
      if (owns_material && material.id != 0) {
        vkr_material_system_add_ref(manager->material_system, material);
      }

      VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
          subset->pipeline_domain, desc->pipeline_domain);

      String8 shader_override = subset->shader_override.str
                                    ? subset->shader_override
                                    : desc->shader_override;
      String8 shader_override_copy = {0};
      if (shader_override.str && shader_override.length > 0) {
        shader_override_copy =
            string8_duplicate(&manager->asset_allocator, &shader_override);
      }

      *submesh = (VkrMeshAssetSubmesh){
          .geometry = geometry,
          .material = material,
          .shader_override = shader_override_copy,
          .pipeline_domain = domain,
          .range_id = geometry.id,
          .first_index = 0,
          .index_count = geometry_config.index_count,
          .vertex_offset = 0,
          .center = geometry_config.center,
          .min_extents = geometry_config.min_extents,
          .max_extents = geometry_config.max_extents,
          .owns_geometry = true_v,
          .owns_material = owns_material,
      };
    }

    built_count++;

    Vec3 sub_min = vec3_add(submesh->center, submesh->min_extents);
    Vec3 sub_max = vec3_add(submesh->center, submesh->max_extents);
    bounds_union_min.x = vkr_min_f32(bounds_union_min.x, sub_min.x);
    bounds_union_min.y = vkr_min_f32(bounds_union_min.y, sub_min.y);
    bounds_union_min.z = vkr_min_f32(bounds_union_min.z, sub_min.z);
    bounds_union_max.x = vkr_max_f32(bounds_union_max.x, sub_max.x);
    bounds_union_max.y = vkr_max_f32(bounds_union_max.y, sub_max.y);
    bounds_union_max.z = vkr_max_f32(bounds_union_max.z, sub_max.z);
    has_bounds = true_v;
  }

  if (!subsets_success) {
    for (uint32_t i = 0; i < built_count; ++i) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);
      if (submesh) {
        vkr_mesh_manager_release_asset_submesh(manager, submesh);
      }
    }
    array_destroy_VkrMeshAssetSubmesh(&asset->submeshes);
    asset->bounds_valid = false_v;
    asset->bounds_local_center = vec3_zero();
    asset->bounds_local_radius = 0.0f;
    return false_v;
  }

  if (has_bounds) {
    asset->bounds_valid = true_v;
    asset->bounds_local_center =
        vec3_scale(vec3_add(bounds_union_min, bounds_union_max), 0.5f);
    Vec3 half_extents = vec3_sub(bounds_union_max, asset->bounds_local_center);
    asset->bounds_local_radius = vec3_length(half_extents);
  }

  return true_v;
}

/**
 * @brief Create a mesh asset from loader result.
 *
 * Extracts submesh data from VkrResourceHandleInfo and creates a VkrMeshAsset.
 * The asset is registered in the asset_by_key hash table.
 *
 * @param manager The mesh manager.
 * @param handle_info Resource handle from mesh loader.
 * @param desc The original load descriptor (for domain/shader override).
 * @param key_buf Pre-built key string for hash table.
 * @param out_error Error code if creation fails.
 * @return Handle to the created asset, or VKR_MESH_ASSET_HANDLE_INVALID.
 */
vkr_internal VkrMeshAssetHandle vkr_mesh_manager_create_asset_from_handle_info(
    VkrMeshManager *manager, const VkrResourceHandleInfo *handle_info,
    const VkrMeshLoadDesc *desc, const char *key_buf,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(handle_info != NULL, "Handle info is NULL");
  assert_log(desc != NULL, "Desc is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  if (handle_info->type != VKR_RESOURCE_TYPE_MESH || !handle_info->as.mesh) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  VkrMeshLoaderResult *mesh_result = handle_info->as.mesh;

  bool8_t use_merged = mesh_result->has_mesh_buffer &&
                       mesh_result->mesh_buffer.vertex_count > 0 &&
                       mesh_result->mesh_buffer.index_count > 0 &&
                       mesh_result->submeshes.length > 0 &&
                       mesh_result->submeshes.data;
  if (!use_merged &&
      (mesh_result->subsets.length == 0 || !mesh_result->subsets.data)) {
    log_error("MeshManager: mesh '%.*s' returned no subsets",
              (int)desc->mesh_path.length, desc->mesh_path.str);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  uint32_t subset_count = use_merged ? (uint32_t)mesh_result->submeshes.length
                                     : (uint32_t)mesh_result->subsets.length;

  uint32_t slot;
  if (manager->asset_free_count > 0) {
    slot = *array_get_uint32_t(&manager->asset_free_indices,
                               manager->asset_free_count - 1);
    manager->asset_free_count--;
  } else {
    if (manager->next_asset_index >= manager->mesh_assets.length) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return VKR_MESH_ASSET_HANDLE_INVALID;
    }
    slot = manager->next_asset_index++;
  }

  VkrMeshAsset *asset = array_get_VkrMeshAsset(&manager->mesh_assets, slot);
  MemZero(asset, sizeof(*asset));

  uint32_t generation = manager->asset_generation_counter++;
  uint32_t id = slot + 1;

  asset->id = id;
  asset->generation = generation;
  asset->domain = vkr_mesh_manager_resolve_domain(desc->pipeline_domain, 0);
  asset->ref_count = 0;
  asset->loading_state = VKR_MESH_LOADING_STATE_NOT_LOADED;
  asset->last_error = VKR_RENDERER_ERROR_NONE;
  asset->pending_request_id = 0;

  asset->mesh_path =
      string8_duplicate(&manager->asset_allocator, &desc->mesh_path);
  if (desc->shader_override.str && desc->shader_override.length > 0) {
    asset->shader_override =
        string8_duplicate(&manager->asset_allocator, &desc->shader_override);
  }

  asset->submeshes =
      array_create_VkrMeshAssetSubmesh(&manager->asset_allocator, subset_count);
  if (!asset->submeshes.data) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_mesh_manager_free_asset_strings(manager, asset);
    MemZero(asset, sizeof(*asset));
    array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                       slot);
    manager->asset_free_count++;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }
  MemZero(asset->submeshes.data, subset_count * sizeof(VkrMeshAssetSubmesh));

  VkrGeometryHandle merged_geometry = VKR_GEOMETRY_HANDLE_INVALID;
  bool8_t subsets_success = true_v;

  if (use_merged) {
    char geometry_name_buf[GEOMETRY_NAME_MAX_LENGTH] = {0};
    vkr_mesh_manager_build_mesh_buffer_key(geometry_name_buf,
                                           mesh_result->source_path);
    String8 geometry_name = string8_create((uint8_t *)geometry_name_buf,
                                           string_length(geometry_name_buf));

    VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
    merged_geometry = vkr_geometry_system_acquire_by_name(
        manager->geometry_system, geometry_name, true_v, &geo_err);

    if (merged_geometry.id == 0) {
      if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        *out_error = geo_err;
        subsets_success = false_v;
      } else {
        Vec3 union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
        Vec3 union_max =
            vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);

        for (uint64_t i = 0; i < mesh_result->submeshes.length; ++i) {
          VkrMeshLoaderSubmeshRange *range = &mesh_result->submeshes.data[i];
          Vec3 range_min = vec3_add(range->center, range->min_extents);
          Vec3 range_max = vec3_add(range->center, range->max_extents);
          union_min.x = vkr_min_f32(union_min.x, range_min.x);
          union_min.y = vkr_min_f32(union_min.y, range_min.y);
          union_min.z = vkr_min_f32(union_min.z, range_min.z);
          union_max.x = vkr_max_f32(union_max.x, range_max.x);
          union_max.y = vkr_max_f32(union_max.y, range_max.y);
          union_max.z = vkr_max_f32(union_max.z, range_max.z);
        }

        Vec3 center = vec3_scale(vec3_add(union_min, union_max), 0.5f);
        Vec3 min_extents = vec3_sub(union_min, center);
        Vec3 max_extents = vec3_sub(union_max, center);

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
          *out_error = geo_err;
          subsets_success = false_v;
        }
      }
    }
  }

  Vec3 bounds_union_min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 bounds_union_max =
      vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  bool8_t has_bounds = false_v;

  uint32_t built_count = 0;
  for (uint32_t i = 0; subsets_success && i < subset_count; ++i) {
    VkrMeshAssetSubmesh *submesh =
        array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);

    if (use_merged) {
      VkrMeshLoaderSubmeshRange *range =
          array_get_VkrMeshLoaderSubmeshRange(&mesh_result->submeshes, i);

      if (i > 0 && merged_geometry.id != 0) {
        vkr_geometry_system_acquire(manager->geometry_system, merged_geometry);
      }

      VkrMaterialHandle material = range->material_handle;
      bool8_t owns_material = true_v;
      if (material.id == 0) {
        material = manager->material_system->default_material;
        owns_material = false_v;
      }
      if (owns_material && material.id != 0) {
        vkr_material_system_add_ref(manager->material_system, material);
      }

      VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
          range->pipeline_domain, desc->pipeline_domain);

      String8 shader_override = range->shader_override.str
                                    ? range->shader_override
                                    : desc->shader_override;
      String8 shader_override_copy = {0};
      if (shader_override.str && shader_override.length > 0) {
        shader_override_copy =
            string8_duplicate(&manager->asset_allocator, &shader_override);
      }

      *submesh = (VkrMeshAssetSubmesh){
          .geometry = merged_geometry,
          .material = material,
          .shader_override = shader_override_copy,
          .pipeline_domain = domain,
          .range_id = range->range_id,
          .first_index = range->first_index,
          .index_count = range->index_count,
          .vertex_offset = range->vertex_offset,
          .center = range->center,
          .min_extents = range->min_extents,
          .max_extents = range->max_extents,
          .owns_geometry = true_v,
          .owns_material = owns_material,
      };
    } else {
      VkrMeshLoaderSubset *subset =
          array_get_VkrMeshLoaderSubset(&mesh_result->subsets, i);

      vkr_mesh_manager_build_geometry_key(subset->geometry_config.name,
                                          mesh_result->source_path, i);
      uint64_t name_length = string_length(subset->geometry_config.name);
      String8 geometry_name =
          string8_create((uint8_t *)subset->geometry_config.name, name_length);

      VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
      VkrGeometryHandle geometry = vkr_geometry_system_acquire_by_name(
          manager->geometry_system, geometry_name, true_v, &geo_err);

      if (geometry.id == 0) {
        if (geo_err != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
          *out_error = geo_err;
          subsets_success = false_v;
          break;
        }
        geometry = vkr_geometry_system_create(manager->geometry_system,
                                              &subset->geometry_config, true_v,
                                              &geo_err);
        if (geometry.id == 0) {
          *out_error = geo_err;
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
      if (owns_material && material.id != 0) {
        vkr_material_system_add_ref(manager->material_system, material);
      }

      VkrPipelineDomain domain = vkr_mesh_manager_resolve_domain(
          subset->pipeline_domain, desc->pipeline_domain);

      String8 shader_override = subset->shader_override.str
                                    ? subset->shader_override
                                    : desc->shader_override;
      String8 shader_override_copy = {0};
      if (shader_override.str && shader_override.length > 0) {
        shader_override_copy =
            string8_duplicate(&manager->asset_allocator, &shader_override);
      }

      *submesh = (VkrMeshAssetSubmesh){
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

    built_count++;

    // Update asset bounds
    Vec3 sub_min = vec3_add(submesh->center, submesh->min_extents);
    Vec3 sub_max = vec3_add(submesh->center, submesh->max_extents);
    bounds_union_min.x = vkr_min_f32(bounds_union_min.x, sub_min.x);
    bounds_union_min.y = vkr_min_f32(bounds_union_min.y, sub_min.y);
    bounds_union_min.z = vkr_min_f32(bounds_union_min.z, sub_min.z);
    bounds_union_max.x = vkr_max_f32(bounds_union_max.x, sub_max.x);
    bounds_union_max.y = vkr_max_f32(bounds_union_max.y, sub_max.y);
    bounds_union_max.z = vkr_max_f32(bounds_union_max.z, sub_max.z);
    has_bounds = true_v;
  }

  if (!subsets_success) {
    for (uint32_t i = 0; i < built_count; ++i) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, i);
      if (submesh) {
        vkr_mesh_manager_release_asset_submesh(manager, submesh);
      }
    }
    array_destroy_VkrMeshAssetSubmesh(&asset->submeshes);
    vkr_mesh_manager_free_asset_strings(manager, asset);
    MemZero(asset, sizeof(*asset));
    array_set_uint32_t(&manager->asset_free_indices, manager->asset_free_count,
                       slot);
    manager->asset_free_count++;
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  if (has_bounds) {
    asset->bounds_valid = true_v;
    asset->bounds_local_center =
        vec3_scale(vec3_add(bounds_union_min, bounds_union_max), 0.5f);
    Vec3 half_extents = vec3_sub(bounds_union_max, asset->bounds_local_center);
    asset->bounds_local_radius = vec3_length(half_extents);
  }

  char *key_copy =
      vkr_allocator_alloc(&manager->asset_allocator, string_length(key_buf) + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!key_copy) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_mesh_manager_destroy_asset_slot(manager, slot, false_v);
    return VKR_MESH_ASSET_HANDLE_INVALID;
  }

  string_copy(key_copy, key_buf);
  asset->key_string = key_copy;
  VkrMeshAssetEntry entry = {.asset_index = slot, .key = key_copy};
  vkr_hash_table_insert_VkrMeshAssetEntry(&manager->asset_by_key, key_copy,
                                          entry);

  manager->asset_count++;

  asset->loading_state = VKR_MESH_LOADING_STATE_LOADED;
  asset->last_error = VKR_RENDERER_ERROR_NONE;
  asset->pending_request_id = 0;

  return (VkrMeshAssetHandle){.id = id, .generation = generation};
}

uint32_t vkr_mesh_manager_create_instances_batch(
    VkrMeshManager *manager, const VkrMeshLoadDesc *descs, uint32_t count,
    VkrMeshInstanceHandle *out_instances, VkrRendererError *out_errors) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(descs != NULL, "Descs is NULL");

  if (count == 0) {
    return 0;
  }

  if (out_instances) {
    for (uint32_t i = 0; i < count; i++) {
      out_instances[i] = VKR_MESH_INSTANCE_HANDLE_INVALID;
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

  VkrMeshAssetKey *unique_keys = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrMeshAssetKey) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *desc_to_unique =
      vkr_allocator_alloc(scratch_allocator, sizeof(uint32_t) * wave_size,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshAssetHandle *unique_assets = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrMeshAssetHandle) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *unique_temp_refs =
      vkr_allocator_alloc(scratch_allocator, sizeof(bool8_t) * wave_size,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *unique_errors = vkr_allocator_alloc(
      scratch_allocator, sizeof(VkrRendererError) * wave_size,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!unique_keys || !desc_to_unique || !unique_assets || !unique_temp_refs ||
      !unique_errors) {
    if (out_errors) {
      for (uint32_t i = 0; i < count; i++) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }

  uint32_t instances_created = 0;
  uint32_t assets_requested = 0;

  for (uint32_t base = 0; base < count; base += wave_size) {
    uint32_t wave_end = vkr_min_u32(base + wave_size, count);
    uint32_t wave_count = wave_end - base;

    uint32_t unique_count = 0;
    for (uint32_t j = 0; j < wave_count; j++) {
      VkrMeshAssetKey key = vkr_mesh_asset_key_from_desc(&descs[base + j]);
      int32_t existing_idx =
          vkr_mesh_asset_key_find(unique_keys, unique_count, &key);

      if (existing_idx >= 0) {
        desc_to_unique[j] = (uint32_t)existing_idx;
      } else {
        desc_to_unique[j] = unique_count;
        unique_keys[unique_count] = key;
        unique_assets[unique_count] = VKR_MESH_ASSET_HANDLE_INVALID;
        unique_temp_refs[unique_count] = false_v;
        unique_errors[unique_count] = VKR_RENDERER_ERROR_NONE;
        unique_count++;
      }
    }

    for (uint32_t j = 0; j < unique_count; ++j) {
      VkrRendererError asset_err = VKR_RENDERER_ERROR_NONE;
      VkrMeshAssetHandle asset = vkr_mesh_manager_acquire_asset(
          manager, unique_keys[j].mesh_path, unique_keys[j].pipeline_domain,
          unique_keys[j].shader_override, &asset_err);
      unique_assets[j] = asset;
      unique_temp_refs[j] = asset.id != 0;
      unique_errors[j] = asset_err;
      if (asset.id != 0) {
        assets_requested++;
      }
    }

    for (uint32_t j = 0; j < wave_count; j++) {
      uint32_t global_i = base + j;
      uint32_t unique_idx = desc_to_unique[j];
      VkrMeshAssetHandle asset_handle = unique_assets[unique_idx];

      if (asset_handle.id == 0) {
        if (out_errors) {
          VkrRendererError asset_err = unique_errors[unique_idx];
          out_errors[global_i] =
              asset_err != VKR_RENDERER_ERROR_NONE
                  ? asset_err
                  : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        }
        continue;
      }

      VkrTransform transform = descs[global_i].transform;
      Mat4 model = vkr_transform_get_world(&transform);

      VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
      VkrMeshInstanceHandle instance = vkr_mesh_manager_create_instance(
          manager, asset_handle, model, 0, true_v, &inst_err);

      if (instance.id != 0) {
        if (out_instances) {
          out_instances[global_i] = instance;
        }
        instances_created++;
      }

      if (out_errors) {
        out_errors[global_i] = inst_err;
      }
    }

    for (uint32_t j = 0; j < unique_count; ++j) {
      if (unique_temp_refs[j] && unique_assets[j].id != 0) {
        vkr_mesh_manager_release_asset(manager, unique_assets[j]);
      }
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  log_debug("Instance batch: %u instances created, %u assets requested",
            instances_created, assets_requested);
  return instances_created;
}

bool8_t vkr_mesh_manager_destroy_instance(VkrMeshManager *manager,
                                          VkrMeshInstanceHandle instance) {
  assert_log(manager != NULL, "Manager is NULL");

  if (instance.id == 0) {
    return false_v;
  }

  uint32_t slot = instance.id - 1;
  if (slot >= manager->mesh_instances.length) {
    return false_v;
  }

  VkrMeshInstance *inst =
      array_get_VkrMeshInstance(&manager->mesh_instances, slot);
  if (inst->asset.id == 0 || inst->generation != instance.generation) {
    return false_v; // Already destroyed or invalid
  }

  uint32_t live_index = inst->live_index;

  vkr_mesh_manager_release_instance_state_array(manager, inst);

  vkr_mesh_manager_release_asset(manager, inst->asset);

  if (manager->instance_count > 0 && live_index < manager->instance_count) {
    uint32_t last_index = manager->instance_count - 1;
    uint32_t last_slot =
        *array_get_uint32_t(&manager->instance_live_indices, last_index);
    array_set_uint32_t(&manager->instance_live_indices, live_index, last_slot);
    if (last_slot != slot) {
      VkrMeshInstance *moved =
          array_get_VkrMeshInstance(&manager->mesh_instances, last_slot);
      moved->live_index = live_index;
    }
    manager->instance_count--;
  }

  MemZero(inst, sizeof(*inst));
  array_set_uint32_t(&manager->instance_free_indices,
                     manager->instance_free_count, slot);
  manager->instance_free_count++;

  return true_v;
}

VkrMeshInstance *vkr_mesh_manager_get_instance(VkrMeshManager *manager,
                                               VkrMeshInstanceHandle handle) {
  assert_log(manager != NULL, "Manager is NULL");

  if (handle.id == 0) {
    return NULL;
  }

  uint32_t slot = handle.id - 1;
  if (slot >= manager->mesh_instances.length) {
    return NULL;
  }

  VkrMeshInstance *inst =
      array_get_VkrMeshInstance(&manager->mesh_instances, slot);
  if (inst->asset.id == 0 || inst->generation != handle.generation) {
    return NULL; // Invalid/destroyed instance
  }

  return inst;
}

VkrMeshInstance *vkr_mesh_manager_get_instance_by_index(VkrMeshManager *manager,
                                                        uint32_t index) {
  assert_log(manager != NULL, "Manager is NULL");

  if (index >= manager->mesh_instances.length) {
    return NULL;
  }

  VkrMeshInstance *inst =
      array_get_VkrMeshInstance(&manager->mesh_instances, index);
  if (inst->asset.id == 0) {
    return NULL; // Invalid/destroyed instance
  }

  return inst;
}

VkrMeshInstance *vkr_mesh_manager_get_instance_by_live_index(
    VkrMeshManager *manager, uint32_t live_index, uint32_t *out_slot) {
  assert_log(manager != NULL, "Manager is NULL");

  if (live_index >= manager->instance_count) {
    return NULL;
  }

  uint32_t slot =
      *array_get_uint32_t(&manager->instance_live_indices, live_index);
  if (slot >= manager->mesh_instances.length) {
    return NULL;
  }

  VkrMeshInstance *inst =
      array_get_VkrMeshInstance(&manager->mesh_instances, slot);
  if (!inst || inst->asset.id == 0) {
    return NULL;
  }

  if (out_slot) {
    *out_slot = slot;
  }

  return inst;
}

bool8_t vkr_mesh_manager_instance_set_model(VkrMeshManager *manager,
                                            VkrMeshInstanceHandle instance,
                                            Mat4 model) {
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance(manager, instance);
  if (!inst) {
    return false_v;
  }

  inst->model = model;

  VkrMeshAsset *asset = vkr_mesh_manager_get_asset(manager, inst->asset);
  vkr_mesh_manager_update_instance_bounds(inst, asset, model);

  return true_v;
}

bool8_t vkr_mesh_manager_instance_set_visible(VkrMeshManager *manager,
                                              VkrMeshInstanceHandle instance,
                                              bool8_t visible) {
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance(manager, instance);
  if (!inst) {
    return false_v;
  }
  inst->visible = visible;
  return true_v;
}

bool8_t vkr_mesh_manager_instance_set_render_id(VkrMeshManager *manager,
                                                VkrMeshInstanceHandle instance,
                                                uint32_t render_id) {
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance(manager, instance);
  if (!inst) {
    return false_v;
  }
  inst->render_id = render_id;
  return true_v;
}

bool8_t vkr_mesh_manager_instance_refresh_pipeline(
    VkrMeshManager *manager, VkrMeshInstanceHandle instance,
    uint32_t submesh_index, VkrPipelineHandle desired_pipeline,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrMeshInstance *inst = vkr_mesh_manager_get_instance(manager, instance);
  if (!inst) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (submesh_index >= inst->submesh_state.length) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrMeshSubmeshInstanceState *state = array_get_VkrMeshSubmeshInstanceState(
      &inst->submesh_state, submesh_index);

  bool8_t requires_update =
      state->pipeline_dirty || state->pipeline.id != desired_pipeline.id ||
      state->pipeline.generation != desired_pipeline.generation;

  if (!requires_update) {
    return true_v;
  }

  if (state->instance_state.id != VKR_INVALID_ID &&
      (state->pipeline.id != desired_pipeline.id ||
       state->pipeline.generation != desired_pipeline.generation)) {
    VkrRendererError rel_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        manager->pipeline_registry, state->pipeline, state->instance_state,
        &rel_err);
    state->instance_state =
        (VkrRendererInstanceStateHandle){.id = VKR_INVALID_ID};
  }

  if (state->instance_state.id == VKR_INVALID_ID) {
    VkrRendererError acq_err = VKR_RENDERER_ERROR_NONE;
    VkrRendererInstanceStateHandle new_state = {.id = VKR_INVALID_ID};
    if (!vkr_pipeline_registry_acquire_instance_state(
            manager->pipeline_registry, desired_pipeline, &new_state,
            &acq_err)) {
      *out_error = acq_err;
      return false_v;
    }
    state->instance_state = new_state;
  }

  state->pipeline = desired_pipeline;
  state->pipeline_dirty = false_v;

  return true_v;
}

uint32_t vkr_mesh_manager_instance_count(const VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  return manager->instance_count;
}

uint32_t vkr_mesh_manager_instance_capacity(const VkrMeshManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  return (uint32_t)manager->mesh_instances.length;
}
