#include "renderer/passes/internal/vkr_pass_draw_utils.h"

#include "core/logger.h"
#include "renderer/systems/vkr_geometry_system.h"

/**
 * @brief Resolve the effective indexed draw range and optional opaque index
 * buffer override.
 *
 * The caller supplies both base and opaque index metadata because this helper
 * is shared by asset-backed and non-instanced mesh submesh types.
 */
static VkrPassDrawRange vkr_pass_packet_build_draw_range(
    RendererFrontend *rf, VkrGeometryHandle geometry, uint32_t index_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t opaque_index_count,
    uint32_t opaque_first_index, int32_t opaque_vertex_offset,
    bool8_t allow_opaque) {
  VkrPassDrawRange range = {
      .index_buffer = NULL,
      .index_count = index_count,
      .first_index = first_index,
      .vertex_offset = vertex_offset,
  };

  if (range.index_count == 0) {
    range.index_count = UINT32_MAX;
    range.first_index = 0;
    range.vertex_offset = 0;
  }

  if (allow_opaque && opaque_index_count > 0) {
    VkrGeometry *resolved =
        vkr_geometry_system_get_by_handle(&rf->geometry_system, geometry);
    if (resolved && resolved->opaque_index_buffer.handle) {
      range.index_buffer = &resolved->opaque_index_buffer;
      range.index_count = opaque_index_count;
      range.first_index = opaque_first_index;
      range.vertex_offset = opaque_vertex_offset;
    }
  }

  return range;
}

bool8_t vkr_pass_packet_upload_instances(RendererFrontend *rf,
                                         const VkrInstanceDataGPU *instances,
                                         uint32_t instance_count,
                                         uint32_t *out_base_instance) {
  if (!rf || !out_base_instance) {
    return false_v;
  }

  *out_base_instance = 0;
  if (instance_count == 0) {
    return true_v;
  }
  if (!instances) {
    return false_v;
  }
  if (!rf->instance_buffer_pool.initialized) {
    log_error("Instance buffer pool is not initialized");
    return false_v;
  }

  VkrInstanceDataGPU *dst = NULL;
  if (!vkr_instance_buffer_alloc(&rf->instance_buffer_pool, instance_count,
                                 out_base_instance, &dst)) {
    return false_v;
  }

  MemCopy(dst, instances,
          sizeof(VkrInstanceDataGPU) * (uint64_t)instance_count);
  vkr_instance_buffer_flush_range(&rf->instance_buffer_pool, *out_base_instance,
                                  instance_count);
  return true_v;
}

bool8_t vkr_pass_packet_resolve_instance(
    RendererFrontend *rf, VkrMeshHandle mesh, uint32_t submesh_index,
    VkrMeshInstance **out_instance, VkrMeshAsset **out_asset,
    VkrMeshAssetSubmesh **out_submesh,
    VkrMeshSubmeshInstanceState **out_instance_state) {
  if (!rf || mesh.id == 0 || !out_instance || !out_asset || !out_submesh ||
      !out_instance_state) {
    return false_v;
  }

  VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance(&rf->mesh_manager, mesh);
  if (!instance || !instance->visible ||
      instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }

  VkrMeshAsset *asset =
      vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
  if (!asset || submesh_index >= asset->submeshes.length) {
    return false_v;
  }

  VkrMeshAssetSubmesh *submesh =
      array_get_VkrMeshAssetSubmesh(&asset->submeshes, submesh_index);
  if (!submesh) {
    return false_v;
  }

  if (submesh_index >= instance->submesh_state.length) {
    return false_v;
  }

  VkrMeshSubmeshInstanceState *state = array_get_VkrMeshSubmeshInstanceState(
      &instance->submesh_state, submesh_index);
  if (!state) {
    return false_v;
  }

  *out_instance = instance;
  *out_asset = asset;
  *out_submesh = submesh;
  *out_instance_state = state;
  return true_v;
}

bool8_t vkr_pass_packet_handle_is_instance(VkrMeshHandle mesh) {
  return mesh.generation != 0;
}

bool8_t vkr_pass_packet_resolve_mesh(RendererFrontend *rf, VkrMeshHandle mesh,
                                     uint32_t submesh_index, VkrMesh **out_mesh,
                                     VkrSubMesh **out_submesh) {
  if (!rf || mesh.id == 0 || !out_mesh || !out_submesh) {
    return false_v;
  }

  if (mesh.generation != 0) {
    return false_v;
  }

  uint32_t mesh_index = mesh.id - 1u;
  VkrMesh *mesh_entry = vkr_mesh_manager_get(&rf->mesh_manager, mesh_index);
  if (!mesh_entry || !mesh_entry->visible ||
      mesh_entry->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }

  VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(&rf->mesh_manager,
                                                     mesh_index, submesh_index);
  if (!submesh) {
    return false_v;
  }

  *out_mesh = mesh_entry;
  *out_submesh = submesh;
  return true_v;
}

bool8_t vkr_pass_packet_resolve_draw_range(RendererFrontend *rf,
                                           const VkrMeshAssetSubmesh *submesh,
                                           bool8_t allow_opaque,
                                           VkrPassDrawRange *out_range) {
  if (!rf || !submesh || !out_range) {
    return false_v;
  }

  *out_range = vkr_pass_packet_build_draw_range(
      rf, submesh->geometry, submesh->index_count, submesh->first_index,
      submesh->vertex_offset, submesh->opaque_index_count,
      submesh->opaque_first_index, submesh->opaque_vertex_offset, allow_opaque);
  return true_v;
}

bool8_t vkr_pass_packet_resolve_draw_range_mesh(RendererFrontend *rf,
                                                const VkrSubMesh *submesh,
                                                bool8_t allow_opaque,
                                                VkrPassDrawRange *out_range) {
  if (!rf || !submesh || !out_range) {
    return false_v;
  }

  *out_range = vkr_pass_packet_build_draw_range(
      rf, submesh->geometry, submesh->index_count, submesh->first_index,
      submesh->vertex_offset, submesh->opaque_index_count,
      submesh->opaque_first_index, submesh->opaque_vertex_offset, allow_opaque);
  return true_v;
}

const char *
vkr_pass_packet_default_shader_for_domain(VkrPipelineDomain domain) {
  switch (domain) {
  case VKR_PIPELINE_DOMAIN_UI:
    return "shader.default.ui";
  case VKR_PIPELINE_DOMAIN_SKYBOX:
    return "shader.default.skybox";
  case VKR_PIPELINE_DOMAIN_SHADOW:
    return "shader.shadow.opaque";
  case VKR_PIPELINE_DOMAIN_PICKING:
  case VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT:
  case VKR_PIPELINE_DOMAIN_PICKING_OVERLAY:
    return "shader.picking";
  case VKR_PIPELINE_DOMAIN_WORLD:
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
  case VKR_PIPELINE_DOMAIN_WORLD_OVERLAY:
  default:
    return "shader.default.world";
  }
}

bool8_t vkr_pass_packet_resolve_pipeline(RendererFrontend *rf,
                                         VkrPipelineDomain domain,
                                         const VkrMaterial *material,
                                         VkrPipelineHandle pipeline_override,
                                         VkrPipelineHandle *out_pipeline) {
  if (!rf || !out_pipeline) {
    return false_v;
  }

  const char *fallback = vkr_pass_packet_default_shader_for_domain(domain);
  const char *shader_name = fallback;
  const char *pipeline_shader = fallback;
  bool8_t use_domain_pipeline = false_v;

  if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT ||
      domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY) {
    use_domain_pipeline = true_v;
  } else if (material && material->shader_name &&
             material->shader_name[0] != '\0') {
    bool8_t allow_material_shader = false_v;
    if (domain == VKR_PIPELINE_DOMAIN_WORLD) {
      allow_material_shader =
          (material->pipeline_id == VKR_INVALID_ID) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD);
    } else {
      allow_material_shader = (material->pipeline_id == (uint32_t)domain);
    }
    if (allow_material_shader) {
      shader_name = material->shader_name;
      pipeline_shader = material->shader_name;
    }
  }

  if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
    shader_name = fallback;
    pipeline_shader = fallback;
    if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
      return false_v;
    }
  }

  if (pipeline_override.id != 0) {
    VkrPipeline *pipeline = NULL;
    if (vkr_pipeline_registry_get_pipeline(&rf->pipeline_registry,
                                           pipeline_override, &pipeline) &&
        pipeline && pipeline->domain == domain) {
      *out_pipeline = pipeline_override;
      return true_v;
    }
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  const char *lookup_name = use_domain_pipeline ? NULL : pipeline_shader;
  if (!vkr_pipeline_registry_get_pipeline_for_material(
          &rf->pipeline_registry, lookup_name, (uint32_t)domain, out_pipeline,
          &err)) {
    return false_v;
  }

  return true_v;
}
