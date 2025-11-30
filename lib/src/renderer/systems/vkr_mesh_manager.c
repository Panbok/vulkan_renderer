#include "renderer/systems/vkr_mesh_manager.h"

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_resource_system.h"

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
  if (!array || !array->data || array->length == 0 || built_count == 0)
    return;
  if (built_count > array->length) {
    built_count = (uint32_t)array->length;
  }
  for (uint32_t i = 0; i < built_count; ++i) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(array, i);
    if (submesh) {
      vkr_mesh_manager_release_submesh(manager, submesh);
    }
  }
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

  manager->meshes =
      array_create_VkrMesh(manager->arena, manager->config.max_mesh_count);
  manager->free_indices =
      array_create_uint32_t(manager->arena, manager->config.max_mesh_count);
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
      array_create_VkrSubMesh(manager->arena, desc->submesh_count);
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

    VkrSubMesh submesh = {
        .geometry = geometry,
        .material = material,
        .pipeline = VKR_PIPELINE_HANDLE_INVALID,
        .instance_state = (VkrRendererInstanceStateHandle){0},
        .pipeline_domain = (sub_desc->pipeline_domain > 0)
                               ? sub_desc->pipeline_domain
                               : VKR_PIPELINE_DOMAIN_WORLD,
        .shader_override =
            string8_duplicate(manager->arena, &sub_desc->shader_override),
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

// Internal function to process batch results and create mesh entries
vkr_internal bool8_t vkr_mesh_manager_process_batch_result(
    VkrMeshManager *manager, VkrMeshBatchResult *batch_result,
    const VkrMeshLoadDesc *desc, uint32_t *out_index,
    VkrRendererError *out_error) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(desc != NULL, "Desc is NULL");

  if (!batch_result || !batch_result->success || !batch_result->result) {
    if (out_error) {
      *out_error = batch_result ? batch_result->error
                                : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }

  VkrMeshLoaderResult *mesh_result = batch_result->result;

  // Validate mesh result
  if (mesh_result->subsets.length == 0 || !mesh_result->subsets.data) {
    log_error("MeshManager: mesh '%.*s' returned no subsets",
              (int)desc->mesh_path.length, desc->mesh_path.str);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }

  uint32_t subset_count = (uint32_t)mesh_result->subsets.length;
  Scratch scratch = scratch_create(manager->scratch_arena);
  VkrSubMeshDesc *sub_descs = arena_alloc(
      scratch.arena, (uint64_t)subset_count * sizeof(VkrSubMeshDesc),
      ARENA_MEMORY_TAG_ARRAY);

  if (!sub_descs) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return false_v;
  }

  MemZero(sub_descs, (uint64_t)subset_count * sizeof(VkrSubMeshDesc));

  uint32_t built_count = 0;
  bool8_t subsets_success = true_v;

  // Build all subsets
  for (uint32_t i = 0; i < subset_count; ++i) {
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
    VkrGeometryHandle geometry = vkr_geometry_system_create(
        manager->geometry_system, &subset->geometry_config, true_v, &geo_err);
    if (geometry.id == 0) {
      if (out_error) {
        *out_error = geo_err;
      }
      subsets_success = false_v;
      break;
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
          string8_duplicate(manager->arena, &shader_override);
    }

    sub_descs[built_count++] = (VkrSubMeshDesc){
        .geometry = geometry,
        .material = material,
        .shader_override = shader_override_copy,
        .pipeline_domain = domain,
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
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
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

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

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
  for (uint32_t submesh_index = 0; submesh_index < mesh->submeshes.length;
       ++submesh_index) {
    VkrSubMesh *submesh = array_get_VkrSubMesh(&mesh->submeshes, submesh_index);
    submesh->last_render_frame = 0;
  }
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

void vkr_mesh_manager_set_loader_context(VkrMeshManager *manager,
                                         VkrMeshLoaderContext *context) {
  assert_log(manager != NULL, "Manager is NULL");
  manager->loader_context = context;
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

  // Batch loading requires loader_context
  if (!manager->loader_context) {
    log_error("MeshManager: loader_context not set, cannot load meshes");
    if (out_errors) {
      for (uint32_t i = 0; i < count; i++) {
        out_errors[i] = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      }
    }
    return 0;
  }

  // Collect mesh paths
  Scratch scratch = scratch_create(manager->scratch_arena);

  String8 *mesh_paths = arena_alloc(scratch.arena, sizeof(String8) * count,
                                    ARENA_MEMORY_TAG_ARRAY);
  VkrMeshBatchResult *batch_results =
      arena_alloc(scratch.arena, sizeof(VkrMeshBatchResult) * count,
                  ARENA_MEMORY_TAG_ARRAY);

  if (!mesh_paths || !batch_results) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    if (out_errors) {
      for (uint32_t i = 0; i < count; i++) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    mesh_paths[i] = descs[i].mesh_path;
  }

  // Batch load all meshes
  uint32_t meshes_loaded = vkr_mesh_loader_load_batch(
      manager->loader_context, mesh_paths, count, scratch.arena, batch_results);

  log_debug("Mesh manager batch: %u/%u meshes loaded from files", meshes_loaded,
            count);

  // Create mesh entries for each successfully loaded result
  uint32_t entries_created = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t mesh_index = VKR_INVALID_ID;
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;

    if (vkr_mesh_manager_process_batch_result(manager, &batch_results[i],
                                              &descs[i], &mesh_index, &err)) {
      if (out_indices) {
        out_indices[i] = mesh_index;
      }
      entries_created++;
    }

    if (out_errors) {
      out_errors[i] = err;
    }
  }

  // Free batch results (the arenas are freed with
  // vkr_mesh_loader_free_batch_results)
  vkr_mesh_loader_free_batch_results(batch_results, count);

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);

  log_debug("Mesh manager batch complete: %u/%u mesh entries created",
            entries_created, count);
  return entries_created;
}
