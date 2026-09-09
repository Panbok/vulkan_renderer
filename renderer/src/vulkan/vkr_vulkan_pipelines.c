#include "vulkan/vkr_vulkan_internal.h"
#include <spirv_reflect.h>

vkr_internal bool8_t
vkr_vk_create_packet_pipelines(VkrVulkanRenderer *renderer);
vkr_internal bool8_t vkr_vk_create_ibl_pipelines(VkrVulkanRenderer *renderer);
vkr_internal bool8_t
vkr_vk_create_atmosphere_pipelines(VkrVulkanRenderer *renderer);
vkr_internal bool8_t
vkr_vk_create_deferred_pipelines(VkrVulkanRenderer *renderer);

bool8_t vkr_vk_pipeline_cache_initialize(VkrVulkanRenderer *renderer) {
  const char *path = getenv("VKR_PIPELINE_CACHE_PATH");
  const size_t path_length = path ? strlen(path) : 0u;
  if (path_length >= sizeof(renderer->pipeline_cache_path)) {
    log_error("Vulkan pipeline cache path exceeds %zu bytes",
              sizeof(renderer->pipeline_cache_path) - 1u);
    return false_v;
  }
  if (path_length) {
    MemCopy(renderer->pipeline_cache_path, path, path_length + 1u);
    log_info("Pipeline cache path: %s", renderer->pipeline_cache_path);
  }
  void *initial_data = NULL;
  size_t initial_size = 0u;
  if (path_length) {
    FILE *file = fopen(renderer->pipeline_cache_path, "rb");
    if (file) {
      if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end > 0 && (uint64_t)end <= MB(64) &&
            fseek(file, 0, SEEK_SET) == 0) {
          initial_size = (size_t)end;
          initial_data = vkr_allocator_alloc(renderer->allocator, initial_size,
                                             VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
          if (!initial_data ||
              fread(initial_data, 1u, initial_size, file) != initial_size) {
            if (initial_data) {
              vkr_allocator_free(renderer->allocator, initial_data,
                                 initial_size,
                                 VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
            }
            initial_data = NULL;
            initial_size = 0u;
          }
        }
      }
      fclose(file);
    }
  }
  VkPipelineCacheCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = initial_size,
      .pInitialData = initial_data,
  };
  VkResult result =
      vkCreatePipelineCache(vkr_vulkan_device_handle(renderer->device), &info,
                            NULL, &renderer->pipeline_cache);
  if (initial_data) {
    vkr_allocator_free(renderer->allocator, initial_data, initial_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (result != VK_SUCCESS && initial_size) {
    info.initialDataSize = 0u;
    info.pInitialData = NULL;
    result = vkCreatePipelineCache(vkr_vulkan_device_handle(renderer->device),
                                   &info, NULL, &renderer->pipeline_cache);
  }
  if (result != VK_SUCCESS)
    return false_v;
  if (initial_size)
    log_info("Loaded pipeline cache data: %zu bytes", initial_size);
  log_info("Initialized Vulkan pipeline cache with %s data",
           initial_size ? "persisted" : "empty");
  return true_v;
}

void vkr_vk_pipeline_cache_shutdown(VkrVulkanRenderer *renderer) {
  if (!renderer->pipeline_cache)
    return;
  VkDevice device = vkr_vulkan_device_handle(renderer->device);
  if (renderer->pipeline_cache_path[0]) {
    size_t size = 0u;
    if (vkGetPipelineCacheData(device, renderer->pipeline_cache, &size, NULL) ==
            VK_SUCCESS &&
        size > 0u) {
      const size_t allocation_size = size;
      void *data = vkr_allocator_alloc(renderer->allocator, allocation_size,
                                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (data && vkGetPipelineCacheData(device, renderer->pipeline_cache,
                                         &size, data) == VK_SUCCESS) {
        char temporary[sizeof(renderer->pipeline_cache_path) + 5u];
        const int written = snprintf(temporary, sizeof(temporary), "%s.tmp",
                                     renderer->pipeline_cache_path);
        FILE *file = written > 0 && (size_t)written < sizeof(temporary)
                         ? fopen(temporary, "wb")
                         : NULL;
        const bool8_t wrote = file && fwrite(data, 1u, size, file) == size;
        const bool8_t closed = file && fclose(file) == 0;
        if (wrote && closed) {
          const FilePath temporary_path = {
              .path = string8_create_from_cstr((const uint8_t *)temporary,
                                               string_length(temporary)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          const FilePath cache_path = {
              .path = string8_create_from_cstr(
                  (const uint8_t *)renderer->pipeline_cache_path,
                  string_length(renderer->pipeline_cache_path)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          if (file_rename(&temporary_path, &cache_path, true_v) ==
              FILE_ERROR_NONE) {
            log_info("Saved pipeline cache data: %zu bytes -> %s", size,
                     renderer->pipeline_cache_path);
          } else {
            (void)file_remove(&temporary_path);
          }
        }
      }
      if (data) {
        vkr_allocator_free(renderer->allocator, data, allocation_size,
                           VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      }
    }
  }
  vkDestroyPipelineCache(device, renderer->pipeline_cache, NULL);
  renderer->pipeline_cache = VK_NULL_HANDLE;
}

vkr_internal SpvReflectBlockVariable *
vkr_vk_reflect_member(SpvReflectBlockVariable *parent, const char *name) {
  if (!parent || !name)
    return NULL;
  for (uint32_t i = 0; i < parent->member_count; ++i) {
    SpvReflectBlockVariable *member = &parent->members[i];
    if (member->name && string_equals(member->name, name))
      return member;
  }
  return NULL;
}

vkr_internal bool8_t vkr_vk_reflect_member_offset(
    SpvReflectBlockVariable *parent, const char *name, uint32_t offset,
    SpvReflectBlockVariable **out_member) {
  SpvReflectBlockVariable *member = vkr_vk_reflect_member(parent, name);
  if (out_member)
    *out_member = member;
  if (!member || member->offset != offset) {
    log_error("Vulkan shader ABI member %s is %s (offset %u, "
              "expected %u)",
              name, member ? "misaligned" : "missing",
              member ? member->offset : UINT32_MAX, offset);
    return false_v;
  }
  return true_v;
}

vkr_internal uint32_t
vkr_vk_reflected_struct_size(const SpvReflectBlockVariable *value) {
  uint32_t size = 0u;
  if (!value)
    return size;
  for (uint32_t i = 0; i < value->member_count; ++i) {
    const SpvReflectBlockVariable *member = &value->members[i];
    const uint32_t member_size = Max(member->size, member->padded_size);
    if (member->offset <= UINT32_MAX - member_size)
      size = Max(size, member->offset + member_size);
  }
  return size;
}

vkr_internal bool8_t vkr_vk_validate_reflected_gpu_abi(
    SpvReflectBlockVariable *value, VkrGpuAbiRecordId id) {
  const VkrGpuAbiRecord *record = vkr_gpu_abi_record(id);
  if (!value || !record ||
      vkr_vk_reflected_struct_size(value) != record->expected_size)
    return false_v;
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < record->field_count; ++i) {
    const VkrGpuAbiField *field = &record->fields[i];
    valid &= vkr_vk_reflect_member_offset(value, field->shader_name,
                                          field->expected_offset, NULL);
  }
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_packet_root_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_WORLD_TEMPORAL_FRAG_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid = spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "world_temporal_fragment", &count, NULL) ==
                      SPV_REFLECT_RESULT_SUCCESS &&
                  count == 1u &&
                  spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "world_temporal_fragment", &count, blocks) ==
                      SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  if (!root || root->member_count == 0u) {
    valid = false_v;
  } else {
    SpvReflectBlockVariable *geometry_rows = NULL;
    SpvReflectBlockVariable *visible_rows = NULL;
    SpvReflectBlockVariable *vertices = NULL;
    SpvReflectBlockVariable *frame = NULL;
    SpvReflectBlockVariable *materials = NULL;
    SpvReflectBlockVariable *instances = NULL;
    SpvReflectBlockVariable *point_light_data = NULL;
    SpvReflectBlockVariable *local_shadow_views = NULL;
    SpvReflectBlockVariable *ltc = NULL;
    SpvReflectBlockVariable *sheen = NULL;
    SpvReflectBlockVariable *anisotropy = NULL;
    SpvReflectBlockVariable *fog = NULL;
    SpvReflectBlockVariable *froxel_fog = NULL;
    valid &= vkr_vk_reflect_member_offset(
        root, "geometry_rows", offsetof(VkrVulkanPacketDrawRoot, geometry_rows),
        &geometry_rows);
    valid &= vkr_vk_reflect_member_offset(
        root, "visible_rows", offsetof(VkrVulkanPacketDrawRoot, visible_rows),
        &visible_rows);
    valid &= vkr_vk_reflect_member_offset(
        root, "vertices", offsetof(VkrVulkanPacketDrawRoot, vertices),
        &vertices);
    valid &= vkr_vk_reflect_member_offset(
        root, "frame", offsetof(VkrVulkanPacketDrawRoot, frame), &frame);
    valid &= vkr_vk_reflect_member_offset(
        root, "visible_row_index",
        offsetof(VkrVulkanPacketDrawRoot, visible_row_index), NULL);
    valid &= vkr_vk_reflect_member_offset(
        root, "flags", offsetof(VkrVulkanPacketDrawRoot, flags), NULL);
    valid &= vkr_vk_reflect_member_offset(
        root, "reserved", offsetof(VkrVulkanPacketDrawRoot, reserved), NULL);
    valid &=
        vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanPacketDrawRoot);

    valid &= vkr_vk_reflect_member_offset(
        frame, "materials", offsetof(VkrVulkanPacketFrameRoot, materials),
        &materials);
    valid &= vkr_vk_reflect_member_offset(
        frame, "instances", offsetof(VkrVulkanPacketFrameRoot, instances),
        &instances);
    valid &= vkr_vk_reflect_member_offset(
        frame, "instance_address_padding",
        offsetof(VkrVulkanPacketFrameRoot, instance_address_padding), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "view_projection",
        offsetof(VkrVulkanPacketFrameRoot, view_projection), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "transmission_texture",
        offsetof(VkrVulkanPacketFrameRoot, transmission_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "transmission_sampler",
        offsetof(VkrVulkanPacketFrameRoot, transmission_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "flags", offsetof(VkrVulkanPacketFrameRoot, flags), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "shadow_debug_mode",
        offsetof(VkrVulkanPacketFrameRoot, shadow_debug_mode), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "point_light_data",
        offsetof(VkrVulkanPacketFrameRoot, point_light_data),
        &point_light_data);
    valid &= vkr_vk_reflect_member_offset(
        frame, "point_light_grid_origin_cell_size",
        offsetof(VkrVulkanPacketFrameRoot, point_light_grid_origin_cell_size),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "view", offsetof(VkrVulkanPacketFrameRoot, view), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "shadow_pcf_uniform_early_out",
        offsetof(VkrVulkanPacketFrameRoot, shadow_pcf_uniform_early_out), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "ibl_probes", offsetof(VkrVulkanPacketFrameRoot, ibl_probes),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "temporal_draw_state",
        offsetof(VkrVulkanPacketFrameRoot, temporal_draw_state), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "local_shadow_views",
        offsetof(VkrVulkanPacketFrameRoot, local_shadow_views),
        &local_shadow_views);
    valid &= vkr_vk_reflect_member_offset(
        frame, "local_shadow_texture",
        offsetof(VkrVulkanPacketFrameRoot, local_shadow_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "dfg_texture", offsetof(VkrVulkanPacketFrameRoot, dfg_texture),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "dfg_sampler", offsetof(VkrVulkanPacketFrameRoot, dfg_sampler),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "diffuse_volume_texture",
        offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "diffuse_volume_origin",
        offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_origin), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "diffuse_volume_inverse_spacing",
        offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_inverse_spacing),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "diffuse_volume_dimensions",
        offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_dimensions), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "ltc", offsetof(VkrVulkanPacketFrameRoot, ltc), &ltc);
    SpvReflectBlockVariable *ltc_lights =
        ltc ? vkr_vk_reflect_member(ltc, "lights") : NULL;
    valid &= vkr_vk_reflect_member_offset(
        ltc, "lights", offsetof(VkrVulkanLtc, lights), &ltc_lights);
    valid &= vkr_vk_reflect_member_offset(
        ltc, "matrix_texture", offsetof(VkrVulkanLtc, matrix_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        ltc, "amplitude_texture", offsetof(VkrVulkanLtc, amplitude_texture),
        NULL);
    valid &= vkr_vk_reflect_member_offset(ltc, "count",
                                          offsetof(VkrVulkanLtc, count), NULL);
    valid &= vkr_vk_reflect_member_offset(
        ltc, "sampler", offsetof(VkrVulkanLtc, sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        ltc, "reserved", offsetof(VkrVulkanLtc, reserved), NULL);
    valid &= ltc && vkr_vk_reflected_struct_size(ltc) == sizeof(VkrVulkanLtc);
    valid &= vkr_vk_reflect_member_offset(
        frame, "sheen", offsetof(VkrVulkanPacketFrameRoot, sheen), &sheen);
    valid &= vkr_vk_reflect_member_offset(
        frame, "anisotropy", offsetof(VkrVulkanPacketFrameRoot, anisotropy),
        &anisotropy);
    valid &= vkr_vk_reflect_member_offset(
        sheen, "directional_albedo_texture",
        offsetof(VkrVulkanSheen, directional_albedo_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(sheen, "ltc_matrix_texture",
                                           offsetof(VkrVulkanSheen,
                                                    ltc_matrix_texture),
                                           NULL);
    valid &= vkr_vk_reflect_member_offset(sheen, "ltc_amplitude_texture",
                                           offsetof(VkrVulkanSheen,
                                                    ltc_amplitude_texture),
                                           NULL);
    valid &= vkr_vk_reflect_member_offset(sheen, "sampler",
                                           offsetof(VkrVulkanSheen, sampler),
                                           NULL);
    valid &= vkr_vk_reflect_member_offset(
        sheen, "ltc_matrix_texture_b",
        offsetof(VkrVulkanSheen, ltc_matrix_texture_b), NULL);
    valid &= vkr_vk_reflect_member_offset(
        sheen, "ltc_amplitude_texture_b",
        offsetof(VkrVulkanSheen, ltc_amplitude_texture_b), NULL);
    valid &= vkr_vk_reflect_member_offset(sheen, "reserved",
                                           offsetof(VkrVulkanSheen, reserved),
                                           NULL);
    valid &= sheen && vkr_vk_reflected_struct_size(sheen) ==
                          sizeof(VkrVulkanSheen);
    valid &= vkr_vk_reflect_member_offset(
        anisotropy, "table0_texture",
        offsetof(VkrVulkanAnisotropy, table0_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        anisotropy, "table1_texture",
        offsetof(VkrVulkanAnisotropy, table1_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        anisotropy, "table2_texture",
        offsetof(VkrVulkanAnisotropy, table2_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        anisotropy, "sampler_index",
        offsetof(VkrVulkanAnisotropy, sampler_index), NULL);
    valid &= anisotropy && vkr_vk_reflected_struct_size(anisotropy) ==
                                sizeof(VkrVulkanAnisotropy);
    valid &= vkr_vk_reflect_member_offset(
        frame, "fog", offsetof(VkrVulkanPacketFrameRoot, fog), &fog);
    valid &= vkr_vk_reflect_member_offset(
        fog, "color_density", offsetof(VkrFogGpuParams, color_density), NULL);
    valid &= vkr_vk_reflect_member_offset(
        fog, "height_distance", offsetof(VkrFogGpuParams, height_distance),
        NULL);
    valid &=
        fog && vkr_vk_reflected_struct_size(fog) == sizeof(VkrFogGpuParams);
    valid &= vkr_vk_reflect_member_offset(
        frame, "froxel_fog", offsetof(VkrVulkanPacketFrameRoot, froxel_fog),
        &froxel_fog);
    valid &= vkr_vk_reflect_member_offset(
        frame, "froxel_integrated_texture",
        offsetof(VkrVulkanPacketFrameRoot, froxel_integrated_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        frame, "froxel_sampler",
        offsetof(VkrVulkanPacketFrameRoot, froxel_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "inverse_view_projection",
        offsetof(VkrFroxelFogGpuParams, inverse_view_projection), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "previous_view_projection",
        offsetof(VkrFroxelFogGpuParams, previous_view_projection), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "previous_view",
        offsetof(VkrFroxelFogGpuParams, previous_view), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "color_density",
        offsetof(VkrFroxelFogGpuParams, color_density), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "grid_dimensions_cell_pixels",
        offsetof(VkrFroxelFogGpuParams, grid_dimensions_cell_pixels), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "selected_local_indices_count",
        offsetof(VkrFroxelFogGpuParams, selected_local_indices_count), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "boxes", offsetof(VkrFroxelFogGpuParams, boxes), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "current_view_projection",
        offsetof(VkrFroxelFogGpuParams, current_view_projection), NULL);
    valid &= vkr_vk_reflect_member_offset(
        froxel_fog, "inverse_raster_view_projection",
        offsetof(VkrFroxelFogGpuParams, inverse_raster_view_projection), NULL);
    valid &= froxel_fog && vkr_vk_reflected_struct_size(froxel_fog) ==
                               sizeof(VkrFroxelFogGpuParams);
    valid &= vkr_vk_validate_reflected_gpu_abi(local_shadow_views,
                                               VKR_GPU_ABI_LOCAL_SHADOW_VIEW);
    valid &= vkr_vk_validate_reflected_gpu_abi(ltc_lights,
                                               VKR_GPU_ABI_RECTANGLE_LIGHT_ROW);
    valid &= frame && vkr_vk_reflected_struct_size(frame) ==
                          sizeof(VkrVulkanPacketFrameRoot);

    const VkrGpuAbiRecord *vertex_abi =
        vkr_gpu_abi_record(VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &=
        vertices && materials && vertex_abi &&
        vkr_vk_reflected_struct_size(vertices) == vertex_abi->expected_size &&
        vkr_vk_reflected_struct_size(materials) ==
            sizeof(VkrVulkanMaterialGpuRow);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vertices, VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &= vkr_vk_validate_reflected_gpu_abi(geometry_rows,
                                               VKR_GPU_ABI_GEOMETRY_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(visible_rows,
                                               VKR_GPU_ABI_VISIBLE_DRAW_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(point_light_data,
                                               VKR_GPU_ABI_POINT_LIGHT_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(instances, VKR_GPU_ABI_INSTANCE);
    valid &= vkr_vk_reflect_member_offset(
        materials, "base_color_texture",
        offsetof(VkrVulkanMaterialGpuRow, base_color_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "base_color_sampler",
        offsetof(VkrVulkanMaterialGpuRow, base_color_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_id",
        offsetof(VkrVulkanMaterialGpuRow, material_id), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "alpha_mode", offsetof(VkrVulkanMaterialGpuRow, alpha_mode),
        NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_emissive",
        offsetof(VkrVulkanMaterialGpuRow, material_emissive), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_surface",
        offsetof(VkrVulkanMaterialGpuRow, material_surface), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_attenuation_color",
        offsetof(VkrVulkanMaterialGpuRow, material_attenuation_color), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_clearcoat",
        offsetof(VkrVulkanMaterialGpuRow, material_clearcoat), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_texture",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_roughness_texture",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_roughness_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_normal_texture",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_normal_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_sampler",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_roughness_sampler",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_roughness_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "clearcoat_normal_sampler",
        offsetof(VkrVulkanMaterialGpuRow, clearcoat_normal_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_sheen",
        offsetof(VkrVulkanMaterialGpuRow, material_sheen), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "sheen_color_texture",
        offsetof(VkrVulkanMaterialGpuRow, sheen_color_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "sheen_roughness_texture",
        offsetof(VkrVulkanMaterialGpuRow, sheen_roughness_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "sheen_color_sampler",
        offsetof(VkrVulkanMaterialGpuRow, sheen_color_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "sheen_roughness_sampler",
        offsetof(VkrVulkanMaterialGpuRow, sheen_roughness_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_anisotropy",
        offsetof(VkrVulkanMaterialGpuRow, material_anisotropy), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "anisotropy_texture",
        offsetof(VkrVulkanMaterialGpuRow, anisotropy_texture), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "anisotropy_sampler",
        offsetof(VkrVulkanMaterialGpuRow, anisotropy_sampler), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_diffuse_transmission",
        offsetof(VkrVulkanMaterialGpuRow, material_diffuse_transmission), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "material_subsurface",
        offsetof(VkrVulkanMaterialGpuRow, material_subsurface), NULL);
    valid &= vkr_vk_reflect_member_offset(
        materials, "temporal_reactivity",
        offsetof(VkrVulkanMaterialGpuRow, temporal_reactivity), NULL);
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

typedef struct VkrVulkanReflectedField {
  const char *name;
  uint32_t offset;
} VkrVulkanReflectedField;

#define VKR_VULKAN_REFLECTED_FIELD(type, member)                               \
  {#member, (uint32_t)offsetof(type, member)}

vkr_internal bool8_t vkr_vk_validate_root_abi_with_gpu_record(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry,
    const VkrVulkanReflectedField *fields, uint32_t field_count,
    uint32_t expected_size, const char *gpu_field,
    VkrGpuAbiRecordId gpu_record_id) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;

  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *gpu_record = NULL;
  for (uint32_t i = 0u; i < field_count; ++i) {
    SpvReflectBlockVariable **out_member =
        gpu_field && string_equals(fields[i].name, gpu_field) ? &gpu_record
                                                              : NULL;
    valid &= vkr_vk_reflect_member_offset(root, fields[i].name,
                                          fields[i].offset, out_member);
  }
  if (gpu_field)
    valid &= vkr_vk_validate_reflected_gpu_abi(gpu_record, gpu_record_id);
  valid &= AlignPow2(vkr_vk_reflected_struct_size(root), MaxAlign()) ==
           expected_size;
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t vkr_vk_validate_root_abi(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry,
    const VkrVulkanReflectedField *fields, uint32_t field_count,
    uint32_t expected_size) {
  return vkr_vk_validate_root_abi_with_gpu_record(
      renderer, shader, entry, fields, field_count, expected_size, NULL,
      VKR_GPU_ABI_RECORD_COUNT);
}

/* Froxel entries use their own typed push record so the three native roots
 * remain visible to reflection. Validate that record directly rather than
 * treating `root_address` as the packet draw-root push field. */
vkr_internal bool8_t vkr_vk_validate_froxel_root_abi(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry,
    const VkrVulkanReflectedField *fields, uint32_t field_count,
    uint32_t expected_size, bool8_t requires_storage_3d) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root_address =
      valid ? vkr_vk_reflect_member(blocks[0], "root_address") : NULL;
  SpvReflectBlockVariable *root =
      root_address ? vkr_vk_reflect_member(root_address, "frame") : NULL;
  for (uint32_t i = 0u; i < field_count; ++i)
    valid &= vkr_vk_reflect_member_offset(root, fields[i].name,
                                          fields[i].offset, NULL);
  valid &= AlignPow2(vkr_vk_reflected_struct_size(root), MaxAlign()) ==
           expected_size;

  uint32_t descriptor_count = 0u;
  valid &= spvReflectEnumerateEntryPointDescriptorBindings(
               &module, entry, &descriptor_count, NULL) ==
           SPV_REFLECT_RESULT_SUCCESS;
  SpvReflectDescriptorBinding **descriptors =
      descriptor_count
          ? vkr_allocator_alloc(renderer->allocator,
                                descriptor_count * sizeof(*descriptors),
                                VKR_ALLOCATOR_MEMORY_TAG_RENDERER)
          : NULL;
  if (descriptor_count && !descriptors) {
    spvReflectDestroyShaderModule(&module);
    return false_v;
  }
  if (descriptor_count)
    valid &= spvReflectEnumerateEntryPointDescriptorBindings(
                 &module, entry, &descriptor_count, descriptors) ==
             SPV_REFLECT_RESULT_SUCCESS;
  bool8_t sampled_3d = false_v, storage_3d = false_v;
  for (uint32_t i = 0u; i < descriptor_count; ++i) {
    const SpvReflectDescriptorBinding *binding = descriptors[i];
    if (!binding || binding->set != 0u)
      continue;
    if (binding->binding == 0u &&
        binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
        binding->image.dim == SpvDim3D)
      sampled_3d = true_v;
    if (binding->binding == 1u &&
        binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
        binding->image.dim == SpvDim3D)
      storage_3d = true_v;
  }
  if (descriptors)
    vkr_allocator_free(renderer->allocator, descriptors,
                       descriptor_count * sizeof(*descriptors),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  spvReflectDestroyShaderModule(&module);
  return valid && sampled_3d && (!requires_storage_3d || storage_3d);
}

vkr_internal bool8_t
vkr_vk_validate_transmission_material_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;

  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid = spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "vk_transmission_shade", &count, NULL) ==
                      SPV_REFLECT_RESULT_SUCCESS &&
                  count == 1u &&
                  spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "vk_transmission_shade", &count, blocks) ==
                      SPV_REFLECT_RESULT_SUCCESS;
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *transmission = NULL;
  valid &= vkr_vk_reflect_member_offset(
      root, "transmission_materials",
      offsetof(VkrVulkanTransmissionRoot, transmission_materials),
      &transmission);
  valid &= transmission && vkr_vk_reflected_struct_size(transmission) ==
                               sizeof(VkrVulkanTransmissionMaterialGpuRow);
  valid &= vkr_vk_reflect_member_offset(
      transmission, "transmission_texture",
      offsetof(VkrVulkanTransmissionMaterialGpuRow, transmission_texture),
      NULL);
  valid &= vkr_vk_reflect_member_offset(
      transmission, "thickness_texture",
      offsetof(VkrVulkanTransmissionMaterialGpuRow, thickness_texture), NULL);
  valid &= vkr_vk_reflect_member_offset(
      transmission, "transmission_sampler",
      offsetof(VkrVulkanTransmissionMaterialGpuRow, transmission_sampler),
      NULL);
  valid &= vkr_vk_reflect_member_offset(
      transmission, "thickness_sampler",
      offsetof(VkrVulkanTransmissionMaterialGpuRow, thickness_sampler), NULL);
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_transmission_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField shade_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, materials),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 transmission_materials),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, opaque_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, opaque_mip_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, geometry_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, vertices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, indices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, compaction_state),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, pixel_list),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, compact_counts),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 frame_address_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, feedback_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, feedback_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, output_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, layer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 previous_transforms),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 previous_transform_address_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 current_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 previous_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, motion_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, history_valid),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot,
                                 previous_frame_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, visible_capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, geometry_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, material_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, instance_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, pixel_capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, compact_layer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, compact_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionRoot, reserved),
  };
  static const VkrVulkanReflectedField compact_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, pixel_list),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 covered_pixels),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 overflow_counts),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 indirect_arguments),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, materials),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot,
                                 destination_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, layer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCompactRoot, reserved),
  };
  static const VkrVulkanReflectedField coverage_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCoverageRoot,
                                 covered_pixels),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCoverageRoot,
                                 vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCoverageRoot, layer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCoverageRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTransmissionCoverageRoot, reserved),
  };
  return vkr_vk_validate_transmission_material_abi(renderer) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV,
             "vk_transmission_shade", shade_fields, ArrayCount(shade_fields),
             sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer,
             VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_COMP_SPV,
             "vk_transmission_shade_partitioned", shade_fields,
             ArrayCount(shade_fields), sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_COMP_SPV,
             "vk_transmission_shade_production", shade_fields,
             ArrayCount(shade_fields), sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer,
             VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_TEMPORAL_COMP_SPV,
             "vk_transmission_shade_production_temporal", shade_fields,
             ArrayCount(shade_fields), sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer,
             VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_COMP_SPV,
             "vk_transmission_shade_partitioned_production", shade_fields,
             ArrayCount(shade_fields), sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer,
             VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_TEMPORAL_COMP_SPV,
             "vk_transmission_shade_partitioned_production_temporal",
             shade_fields, ArrayCount(shade_fields),
             sizeof(VkrVulkanTransmissionRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_CLEAR_COMP_SPV,
             "vk_transmission_compact_clear", compact_fields,
             ArrayCount(compact_fields),
             sizeof(VkrVulkanTransmissionCompactRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_COMP_SPV,
             "vk_transmission_compact", compact_fields,
             ArrayCount(compact_fields),
             sizeof(VkrVulkanTransmissionCompactRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_FINALIZE_COMP_SPV,
             "vk_transmission_compact_finalize", compact_fields,
             ArrayCount(compact_fields),
             sizeof(VkrVulkanTransmissionCompactRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_TRANSMISSION_COVERAGE_COMP_SPV,
             "vk_transmission_coverage", coverage_fields,
             ArrayCount(coverage_fields),
             sizeof(VkrVulkanTransmissionCoverageRoot));
}

vkr_internal bool8_t
vkr_vk_validate_gtao_root_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_GTAO_EVALUATE_COMP_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;

  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid = spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "vk_gtao_evaluate", &count, NULL) ==
                      SPV_REFLECT_RESULT_SUCCESS &&
                  count == 1u &&
                  spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "vk_gtao_evaluate", &count, blocks) ==
                      SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *params = NULL;
  static const VkrVulkanReflectedField root_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, destination_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, edges_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, point_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, source_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, destination_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanGtaoRoot, reserved),
  };
  static const VkrVulkanReflectedField param_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, view),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, viewport_width),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, viewport_height),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, depth_mip_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, reserved_u32_0),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, viewport_pixel_size_x),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, viewport_pixel_size_y),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m22),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m23),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m32),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m33),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m00),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m11),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m02),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m12),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m03),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, projection_m13),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, effect_radius),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, radius_multiplier),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, falloff_range),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, falloff_mul),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, falloff_add),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, depth_mip_falloff_mul),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, sample_distribution_power),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, final_value_power),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, depth_mip_sampling_offset),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, denoise_blur_beta),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, reserved_float0),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, reserved_float1),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, slice_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, steps_per_slice),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, noise_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrGtaoGpuParams, reserved0),
  };
  for (uint32_t i = 0u; i < ArrayCount(root_fields); ++i) {
    SpvReflectBlockVariable **out_member = i == 0u ? &params : NULL;
    valid &= vkr_vk_reflect_member_offset(root, root_fields[i].name,
                                          root_fields[i].offset, out_member);
  }
  for (uint32_t i = 0u; i < ArrayCount(param_fields); ++i)
    valid &= vkr_vk_reflect_member_offset(params, param_fields[i].name,
                                          param_fields[i].offset, NULL);
  valid &= vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanGtaoRoot);
  valid &= vkr_vk_reflected_struct_size(params) == sizeof(VkrGtaoGpuParams);
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_ibl_sh_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, destination),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, source_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, source_face_size),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, source_mip),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, window_band_0),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, window_band_1),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, window_band_2),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanIblShRoot, reserved),
  };
  return vkr_vk_validate_root_abi(renderer, VKR_VULKAN_PACKET_IBL_SH_COMP_SPV,
                                  "ibl_sh", fields, ArrayCount(fields),
                                  sizeof(VkrVulkanIblShRoot));
}

vkr_internal bool8_t vkr_vk_validate_motion_blur_root_abi(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *params = vkr_vk_reflect_member(root, "params");
  static const VkrVulkanReflectedField motion_blur_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source0),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source1),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source2),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source3),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source4),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, destination0),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, source_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMotionBlurRoot, reserved),
  };
  for (uint32_t i = 0u; i < ArrayCount(motion_blur_fields); ++i)
    valid &= vkr_vk_reflect_member_offset(root, motion_blur_fields[i].name,
                                          motion_blur_fields[i].offset, NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "motion", offsetof(VkrMotionBlurGpuParams, motion), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "depth_uv", offsetof(VkrMotionBlurGpuParams, depth_uv), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "dimensions", offsetof(VkrMotionBlurGpuParams, dimensions), NULL);
  valid &=
      vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanMotionBlurRoot);
  valid &=
      vkr_vk_reflected_struct_size(params) == sizeof(VkrMotionBlurGpuParams);
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t vkr_vk_validate_subsurface_root_abi(VkrVulkanRenderer *renderer,
                                                  const char *shader,
                                                  const char *entry) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *params = vkr_vk_reflect_member(root, "params");
  static const VkrVulkanReflectedField subsurface_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, frame_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, visible_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, hdr),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, source),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, depth),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, normal),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, vbuffer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, profile_bank),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, albedo),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, specular),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, clearcoat),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, sheen),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, anisotropy),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, destination),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, source_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot, reserved),
  };
  for (uint32_t i = 0u; i < ArrayCount(subsurface_fields); ++i)
    valid &= vkr_vk_reflect_member_offset(root, subsurface_fields[i].name,
                                          subsurface_fields[i].offset, NULL);
  valid &= vkr_vk_reflect_member_offset(params, "projection",
                                        offsetof(VkrSubsurfaceGpuParams, projection), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "dimensions", offsetof(VkrSubsurfaceGpuParams, dimensions), NULL);
  valid &= vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanSubsurfaceRoot);
  valid &= vkr_vk_reflected_struct_size(params) == sizeof(VkrSubsurfaceGpuParams);
  spvReflectDestroyShaderModule(&module);
  return valid;
}
vkr_internal bool8_t vkr_vk_validate_dof_root_abi(VkrVulkanRenderer *renderer,
                                                  const char *shader,
                                                  const char *entry) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry, &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *params = vkr_vk_reflect_member(root, "params");
  static const VkrVulkanReflectedField dof_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source0),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source1),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source2),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source3),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source4),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, destination0),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, destination1),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanDofRoot, source_sampler),
  };
  for (uint32_t i = 0u; i < ArrayCount(dof_fields); ++i)
    valid &= vkr_vk_reflect_member_offset(root, dof_fields[i].name,
                                          dof_fields[i].offset, NULL);
  valid &= vkr_vk_reflect_member_offset(params, "lens",
                                        offsetof(VkrDofGpuParams, lens), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "depth_uv", offsetof(VkrDofGpuParams, depth_uv), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "dimensions", offsetof(VkrDofGpuParams, dimensions), NULL);
  valid &= vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanDofRoot);
  valid &= vkr_vk_reflected_struct_size(params) == sizeof(VkrDofGpuParams);
  spvReflectDestroyShaderModule(&module);
  return valid;
}
vkr_internal bool8_t
vkr_vk_validate_deferred_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField cull_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, candidates),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, classifications),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, visible),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, states),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, commands),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, view_projections),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, frustum_planes),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, candidate_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, view_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, candidate_capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, command_partition_capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, hzb_textures),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, hzb_mip_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, hzb_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, hzb_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, hzb_depth_epsilon),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, camera_required_flags),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, shadow_required_flags),
  };
  static const VkrVulkanReflectedField resolve_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, geometry_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, reserved_address),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, materials),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, vertices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, indices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, compaction_state),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, current_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot,
                                 previous_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, previous_transforms),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, emissive_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, debug_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, motion_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, visible_capacity),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, geometry_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, material_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, instance_count),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, render_mode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, history_valid),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, previous_frame_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, sheen_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, anisotropy_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, reserved_tail),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, sky_reprojection),
  };
  static const VkrVulkanReflectedField temporal_resolve_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 pre_transmission_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, motion_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 history_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 history_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 history_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 history_surface_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 output_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 output_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 output_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 output_surface_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, history_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, history_valid),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, render_mode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 camera_stationary),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 transmission_visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 transmission_instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 transmission_vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 transmission_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 transmission_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 scene_history_mode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 current_jitter_pixels),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                                 previous_jitter_pixels),
  };
  static const VkrVulkanReflectedField lighting_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, frame),
      {"frame_address_padding",
       (uint32_t)offsetof(VkrVulkanLightingRoot, frame_padding)},
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot,
                                 inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot,
                                 gtao_visibility_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, solar_disk_radiance),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, direct_source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, ssgi_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sheen_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, anisotropy_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, subsurface_source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, subsurface_profile_count),
  };
  static const VkrVulkanReflectedField atmosphere_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, transmittance_sample),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot,
                                 transmittance_storage),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot,
                                 multiple_scattering_sample),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot,
                                 multiple_scattering_storage),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, source_storage),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, sun_output),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, face_size),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, reserved),
  };
  static const VkrVulkanReflectedField picking_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, opaque_visible),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, transmission_visible),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, opaque_instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, transmission_instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, opaque_vbuffer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, transmission_vbuffer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, output_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, pixel),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, transmission_layer),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPickingRoot, use_transmission),
  };
  static const VkrVulkanReflectedField hzb_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, destination_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, destination_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_is_depth),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, reserved),
  };
  static const VkrVulkanReflectedField ssr_depth_base_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot,
                                 destination_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, receiver_texture),
  };
  static const VkrVulkanReflectedField ssr_depth_mip_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot,
                                 source_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot,
                                 destination_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, source_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, destination_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, reserved),
  };
  static const VkrVulkanReflectedField ssr_trace_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, depth_pyramid_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, receiver_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, destination_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, source_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, reserved),
  };
  static const VkrVulkanReflectedField ssr_temporal_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, raw_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, receiver_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, motion_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 history_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 history_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 history_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 output_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 output_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                                 output_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, linear_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, gtao_visibility_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, sheen_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, anisotropy_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, reserved),
  };
  static const VkrVulkanReflectedField ssr_composite_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, frame_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot,
                                 inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, reflection_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot,
                                 gtao_visibility_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, linear_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot,
                                 clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, sheen_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot,
                                 anisotropy_texture),
  };
  static const VkrVulkanReflectedField ssgi_depth_base_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot,
                                 destination_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, reserved),
  };
  static const VkrVulkanReflectedField ssgi_depth_mip_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot,
                                 source_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot,
                                 destination_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, source_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, destination_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, reserved),
  };
  static const VkrVulkanReflectedField ssgi_trace_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, depth_pyramid_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, direct_source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, destination_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, reserved),
  };
  static const VkrVulkanReflectedField ssgi_temporal_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, instances),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, raw_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, motion_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 history_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 history_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 history_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 output_color_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 output_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                                 output_identity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, linear_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, reserved),
  };
  static const VkrVulkanReflectedField ssgi_composite_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, frame_padding),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                                 inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                                 reflection_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, albedo_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, normal_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                                 history_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, specular_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, linear_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                                 clearcoat_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, sheen_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                                 anisotropy_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, visible_rows),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, subsurface_source_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, subsurface_profile_count),
  };
  static const VkrVulkanReflectedField fog_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, inverse_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, camera_position),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, target_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, extent),
  };
  static const VkrVulkanReflectedField froxel_inject_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, output_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_valid),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, reserved),
  };
  static const VkrVulkanReflectedField froxel_integrate_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot,
                                 scattering_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot,
                                 scattering_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot,
                                 integrated_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelIntegrateRoot, reserved),
  };
  static const VkrVulkanReflectedField froxel_apply_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, frame),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, params),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, integrated_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, integrated_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, target_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, reserved),
  };
  static const VkrVulkanReflectedField sdsm_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, reduce_state),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, vbuffer_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, reserved),
  };
  bool8_t valid = true_v;
  valid &= vkr_vk_validate_subsurface_root_abi(
      renderer, VKR_VULKAN_PACKET_SUBSURFACE_GATHER_COMP_SPV, "vk_subsurface_gather");
  static const char *const motion_blur_shaders[] = {
      VKR_VULKAN_PACKET_MOTION_BLUR_TILE_MAX_COMP_SPV,
      VKR_VULKAN_PACKET_MOTION_BLUR_NEIGHBOR_MAX_COMP_SPV,
      VKR_VULKAN_PACKET_MOTION_BLUR_RECONSTRUCT_COMP_SPV,
  };
  static const char *const motion_blur_entries[] = {
      "vk_motion_blur_tile_max",
      "vk_motion_blur_neighbor_max",
      "vk_motion_blur_reconstruct",
  };
  _Static_assert(ArrayCount(motion_blur_shaders) == ArrayCount(motion_blur_entries),
                 "motion_blur reflection entries must match their modules");
  for (uint32_t i = 0u; i < ArrayCount(motion_blur_shaders); ++i)
    valid &= vkr_vk_validate_motion_blur_root_abi(
        renderer, motion_blur_shaders[i], motion_blur_entries[i]);
  static const char *const dof_shaders[] = {
      VKR_VULKAN_PACKET_DOF_COC_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_DILATE_HORIZONTAL_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_DILATE_VERTICAL_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_GATHER_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_COMPOSITE_COMP_SPV,
  };
  static const char *const dof_entries[] = {
      "vk_dof_coc",
      "vk_dof_dilate_horizontal",
      "vk_dof_dilate_vertical",
      "vk_dof_prefilter",
      "vk_dof_gather",
      "vk_dof_composite",
  };
  _Static_assert(ArrayCount(dof_shaders) == ArrayCount(dof_entries),
                 "dof reflection entries must match their modules");
  for (uint32_t i = 0u; i < ArrayCount(dof_shaders); ++i)
    valid &= vkr_vk_validate_dof_root_abi(renderer, dof_shaders[i],
                                          dof_entries[i]);
  static const char *const cull_shaders[] = {
      VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV,
      VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV,
      VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV,
  };
  static const char *const cull_entries[] = {
      "vk_gpu_draw_classify",
      "vk_gpu_draw_prefix",
      "vk_gpu_draw_encode",
  };
  for (uint32_t i = 0u; i < ArrayCount(cull_shaders); ++i)
    valid &= vkr_vk_validate_root_abi_with_gpu_record(
        renderer, cull_shaders[i], cull_entries[i], cull_fields,
        ArrayCount(cull_fields), sizeof(VkrVulkanCullRoot), "candidates",
        VKR_GPU_ABI_CANDIDATE_DRAW_ROW);
  static const char *const resolve_shaders[] = {
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_NONE_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_DEBUG_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_DEBUG_COMP_SPV,
  };
  for (uint32_t i = 0u; i < ArrayCount(resolve_shaders); ++i)
    valid &= vkr_vk_validate_root_abi(
        renderer, resolve_shaders[i], "vk_gbuffer_resolve", resolve_fields,
        ArrayCount(resolve_fields), sizeof(VkrVulkanResolveRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV,
      "vk_deferred_lighting", lighting_fields, ArrayCount(lighting_fields),
      sizeof(VkrVulkanLightingRoot));
  static const char *const atmosphere_shaders[] = {
      VKR_VULKAN_PACKET_ATMOSPHERE_TRANSMITTANCE_COMP_SPV,
      VKR_VULKAN_PACKET_ATMOSPHERE_MULTIPLE_SCATTERING_COMP_SPV,
      VKR_VULKAN_PACKET_ATMOSPHERE_SOURCE_COMP_SPV,
      VKR_VULKAN_PACKET_ATMOSPHERE_SUN_COMP_SPV,
  };
  static const char *const atmosphere_entries[] = {
      "atmosphere_transmittance_compute",
      "atmosphere_multiple_scattering_compute",
      "atmosphere_source_compute",
      "atmosphere_sun_compute",
  };
  for (uint32_t i = 0u; i < ArrayCount(atmosphere_shaders); ++i)
    valid &= vkr_vk_validate_root_abi(renderer, atmosphere_shaders[i],
                                      atmosphere_entries[i], atmosphere_fields,
                                      ArrayCount(atmosphere_fields),
                                      sizeof(VkrVulkanAtmosphereRoot));
  static const VkrVulkanReflectedField fsr31_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, scene_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 pre_transmission_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, validity_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 opaque_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 transmission_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 output_depth_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, reactive_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 composition_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                                 transmission_enabled),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, scene_stationary),
  };
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_FSR31_PREPARE_COMP_SPV, "vk_fsr31_prepare",
      fsr31_fields, ArrayCount(fsr31_fields),
      sizeof(VkrVulkanFsr31PrepareRoot));
  static const VkrVulkanReflectedField fsr31_stabilize_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, output_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, history_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, reactive_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, scene_stationary),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, output_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, render_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, jitter_pixels),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, reserved),
  };
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_FSR31_STABILIZE_COMP_SPV,
      "vk_fsr31_stabilize", fsr31_stabilize_fields,
      ArrayCount(fsr31_stabilize_fields), sizeof(VkrVulkanFsr31StabilizeRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_TEMPORAL_RESOLVE_COMP_SPV,
      "vk_temporal_resolve", temporal_resolve_fields,
      ArrayCount(temporal_resolve_fields),
      sizeof(VkrVulkanTemporalResolveRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV,
      "vk_picking_resolve", picking_fields, ArrayCount(picking_fields),
      sizeof(VkrVulkanPickingRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV, "vk_hzb_build",
      hzb_fields, ArrayCount(hzb_fields), sizeof(VkrVulkanHzbRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSR_DEPTH_BASE_COMP_SPV, "vk_ssr_depth_base",
      ssr_depth_base_fields, ArrayCount(ssr_depth_base_fields),
      sizeof(VkrVulkanSsrDepthBaseRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSR_DEPTH_MIP_COMP_SPV, "vk_ssr_depth_mip",
      ssr_depth_mip_fields, ArrayCount(ssr_depth_mip_fields),
      sizeof(VkrVulkanSsrDepthMipRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSR_TRACE_COMP_SPV, "vk_ssr_trace",
      ssr_trace_fields, ArrayCount(ssr_trace_fields),
      sizeof(VkrVulkanSsrTraceRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV, "vk_ssr_temporal",
      ssr_temporal_fields, ArrayCount(ssr_temporal_fields),
      sizeof(VkrVulkanSsrTemporalRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSR_COMPOSITE_COMP_SPV, "vk_ssr_composite",
      ssr_composite_fields, ArrayCount(ssr_composite_fields),
      sizeof(VkrVulkanSsrCompositeRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSGI_DEPTH_BASE_COMP_SPV,
      "ssgi_depth_base_compute", ssgi_depth_base_fields,
      ArrayCount(ssgi_depth_base_fields), sizeof(VkrVulkanSsgiDepthBaseRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSGI_DEPTH_MIP_COMP_SPV,
      "ssgi_depth_mip_compute", ssgi_depth_mip_fields,
      ArrayCount(ssgi_depth_mip_fields), sizeof(VkrVulkanSsgiDepthMipRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSGI_TRACE_COMP_SPV, "ssgi_trace_compute",
      ssgi_trace_fields, ArrayCount(ssgi_trace_fields),
      sizeof(VkrVulkanSsgiTraceRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSGI_TEMPORAL_COMP_SPV,
      "ssgi_temporal_compute", ssgi_temporal_fields,
      ArrayCount(ssgi_temporal_fields), sizeof(VkrVulkanSsgiTemporalRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SSGI_COMPOSITE_COMP_SPV,
      "ssgi_composite_compute", ssgi_composite_fields,
      ArrayCount(ssgi_composite_fields), sizeof(VkrVulkanSsgiCompositeRoot));
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_FOG_APPLY_COMP_SPV, "fog_apply_compute",
      fog_fields, ArrayCount(fog_fields), sizeof(VkrVulkanFogRoot));
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_INJECT_COMP_SPV,
      "froxel_inject_compute", froxel_inject_fields,
      ArrayCount(froxel_inject_fields), sizeof(VkrVulkanFroxelInjectRoot),
      true_v);
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_INTEGRATE_COMP_SPV,
      "froxel_integrate_compute", froxel_integrate_fields,
      ArrayCount(froxel_integrate_fields), sizeof(VkrVulkanFroxelIntegrateRoot),
      true_v);
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_APPLY_COMP_SPV, "froxel_apply_compute",
      froxel_apply_fields, ArrayCount(froxel_apply_fields),
      sizeof(VkrVulkanFroxelApplyRoot), false_v);
  valid &= vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV, "vk_sdsm_reduce",
      sdsm_fields, ArrayCount(sdsm_fields), sizeof(VkrVulkanSdsmRoot));
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_editor_overlay_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, vertices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, decode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot,
                                 model_view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, color),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, first_vertex),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, decode_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, object_id),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, reserved),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanEditorOverlayRoot, display_output),
  };
  return vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_EDITOR_OVERLAY_VERT_SPV,
      "editor_overlay_vertex", fields, ArrayCount(fields),
      sizeof(VkrVulkanEditorOverlayRoot));
}

vkr_internal bool8_t
vkr_vk_validate_fullscreen_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot,
                                 transmission_texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot,
                                 transmission_sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot,
                                 point_light_grid_origin_cell_size),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot, exposure_state),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot, color_grading),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketUtilityRoot, display_output),
  };
  return vkr_vk_validate_root_abi_with_gpu_record(
      renderer, VKR_VULKAN_PACKET_FULLSCREEN_FRAG_SPV, "fullscreen_fragment",
      fields, ArrayCount(fields), sizeof(VkrVulkanPacketUtilityRoot),
      "color_grading", VKR_GPU_ABI_COLOR_GRADING);
}

vkr_internal bool8_t vkr_vk_validate_ui_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, vertices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, texture),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, sampler),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, target_unit_range),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, rect_extent),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, mode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, flags),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, corner_radii),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, display_output),
  };
  return vkr_vk_validate_root_abi(renderer, VKR_VULKAN_PACKET_UI_VERT_SPV,
                                  "ui_vertex", fields, ArrayCount(fields),
                                  sizeof(VkrVulkanUiRoot)) &&
         vkr_vk_validate_root_abi(
             renderer, VKR_VULKAN_PACKET_UI_RECT_VERT_SPV, "ui_rect_vertex",
             fields, ArrayCount(fields), sizeof(VkrVulkanUiRoot));
}

#undef VKR_VULKAN_REFLECTED_FIELD

vkr_internal bool8_t vkr_vk_create_shader_module(VkrVulkanRenderer *renderer,
                                                 const char *path,
                                                 VkShaderModule *out_module) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0 || (size % sizeof(uint32_t)) != 0) {
    return false_v;
  }
  VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = (const uint32_t *)bytes,
  };
  const VkResult result = vkCreateShaderModule(vkr_vk_renderer_device(renderer),
                                               &module_info, NULL, out_module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result == VK_SUCCESS;
}

bool8_t vkr_vk_create_pipelines(VkrVulkanRenderer *renderer) {
  if (!vkr_vk_validate_editor_overlay_root_abi(renderer) ||
      !vkr_vk_validate_fullscreen_root_abi(renderer) ||
      !vkr_vk_validate_ui_root_abi(renderer) ||
      !vkr_vk_validate_packet_root_abi(renderer) ||
      !vkr_vk_validate_gtao_root_abi(renderer) ||
      !vkr_vk_validate_ibl_sh_root_abi(renderer) ||
      !vkr_vk_validate_transmission_root_abi(renderer) ||
      !vkr_vk_validate_deferred_root_abi(renderer)) {
    return false_v;
  }
  const VkrVulkanDescriptorLayout *resource_layout =
      vkr_vulkan_device_resource_layout(renderer->device);
  const VkrVulkanDescriptorLayout *sampler_layout =
      vkr_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorSetLayout layouts[] = {
      resource_layout->handle,
      sampler_layout->handle,
  };
  VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                    VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0u,
      .size = sizeof(VkrVulkanPushConstants),
  };
  VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = ArrayCount(layouts),
      .pSetLayouts = layouts,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  VkDevice device = vkr_vk_renderer_device(renderer);
  if (vkCreatePipelineLayout(device, &layout_info, NULL,
                             &renderer->pipeline_layout) != VK_SUCCESS) {
    return false_v;
  }
  return vkr_vk_create_packet_pipelines(renderer) &&
         vkr_vk_create_ibl_pipelines(renderer) &&
         vkr_vk_create_atmosphere_pipelines(renderer) &&
         vkr_vk_create_deferred_pipelines(renderer);
}

vkr_internal bool8_t vkr_vk_create_packet_pipeline_at(
    VkrVulkanRenderer *renderer, VkrVulkanPacketPipeline pipeline,
    VkrVulkanPacketShader vertex_shader, VkrVulkanPacketShader fragment_shader,
    VkFormat color_format, VkFormat depth_format, bool8_t depth_test,
    bool8_t depth_write, bool8_t blend_enabled, bool8_t depth_bias,
    VkPipeline *out_pipeline) {
  if (!out_pipeline)
    return false_v;
  // A fragment shader of VKR_VULKAN_PACKET_SHADER_COUNT builds a
  // depth-only pipeline with no fragment stage, which is what a shadow cascade
  // rendering opaque geometry should use.
  const bool8_t depth_only = fragment_shader == VKR_VULKAN_PACKET_SHADER_COUNT;
  const VkPipelineShaderStageCreateInfo stages[] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = renderer->packet_shaders[vertex_shader],
          .pName =
              vertex_shader == VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX
                  ? "world_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_VERTEX
                  ? "world_temporal_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX
                  ? "editor_overlay_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX
                  ? "text_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_UI_VERTEX
                  ? "ui_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_UI_RECT_VERTEX
                  ? "ui_rect_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX
                  ? "vk_visibility_vertex"
                  : "fullscreen_vertex",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = depth_only ? VK_NULL_HANDLE
                               : renderer->packet_shaders[fragment_shader],
          .pName =
              depth_only ? ""
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_FRAGMENT
                  ? "world_temporal_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_WORLD_FRAGMENT
                  ? "world_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_PICKING_FRAGMENT
                  ? "picking_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_FRAGMENT
                  ? "editor_overlay_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_PICKING_FRAGMENT
                  ? "editor_overlay_picking_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT
                  ? "text_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_TEXT_PICKING_FRAGMENT
                  ? "text_picking_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT
                  ? "ui_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_UI_RECT_FRAGMENT
                  ? "ui_rect_fragment"
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT
                  ? "vk_visibility_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT
                  ? "vk_visibility_opaque_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT
                  ? "vk_visibility_shadow_fragment"
                  : "fullscreen_fragment",
      },
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = depth_bias,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth_test,
      .depthWriteEnable = depth_write,
      .depthCompareOp =
          depth_write ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL,
  };
  const bool8_t temporal_targets =
      pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND ||
      pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT;
  const bool8_t temporal_writes =
      pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND;
  const VkPipelineColorBlendAttachmentState color_attachments[] = {
      {
          .blendEnable = blend_enabled,
          .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
          .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                            VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      },
      {.colorWriteMask =
           temporal_writes ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           : 0u},
      {.colorWriteMask =
           temporal_writes ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           : 0u},
      {.colorWriteMask =
           temporal_writes ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           : 0u},
  };
  const uint32_t color_attachment_count = color_format == VK_FORMAT_UNDEFINED
                                              ? 0u
                                          : temporal_targets ? 4u
                                                             : 1u;
  const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = color_attachment_count,
      .pAttachments = color_attachment_count ? color_attachments : NULL,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,   VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_CULL_MODE,  VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
  };
  const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = depth_bias ? ArrayCount(dynamic_states) : 4u,
      .pDynamicStates = dynamic_states,
  };
  const VkFormat color_formats[] = {
      color_format,
      VK_FORMAT_R32G32_UINT,
      VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_R16G16_SFLOAT,
  };
  const VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = color_attachment_count,
      .pColorAttachmentFormats = color_attachment_count ? color_formats : NULL,
      .depthAttachmentFormat = depth_format,
  };
  const VkGraphicsPipelineCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = depth_only ? 1u : (uint32_t)ArrayCount(stages),
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pDepthStencilState =
          depth_format != VK_FORMAT_UNDEFINED ? &depth_stencil : NULL,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = renderer->pipeline_layout,
  };
  const VkResult result = vkCreateGraphicsPipelines(
      vkr_vk_renderer_device(renderer), renderer->pipeline_cache, 1u,
      &create_info, NULL, out_pipeline);
  if (result != VK_SUCCESS) {
    log_error("Vulkan packet pipeline %u creation failed: %d", pipeline,
              result);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_create_packet_pipeline(
    VkrVulkanRenderer *renderer, VkrVulkanPacketPipeline pipeline,
    VkrVulkanPacketShader vertex_shader, VkrVulkanPacketShader fragment_shader,
    VkFormat color_format, VkFormat depth_format, bool8_t depth_test,
    bool8_t depth_write, bool8_t blend_enabled, bool8_t depth_bias) {
  return vkr_vk_create_packet_pipeline_at(
      renderer, pipeline, vertex_shader, fragment_shader, color_format,
      depth_format, depth_test, depth_write, blend_enabled, depth_bias,
      &renderer->packet_pipelines[pipeline]);
}

vkr_internal bool8_t vkr_vk_create_presentation_pipeline(
    VkrVulkanRenderer *renderer, VkrVulkanPacketPipeline pipeline,
    VkrVulkanPacketShader vertex_shader, VkrVulkanPacketShader fragment_shader,
    VkFormat color_format, bool8_t blend_enabled, VkPipeline *out_pipeline) {
  if (!vkr_vk_create_packet_pipeline_at(
          renderer, pipeline, vertex_shader, fragment_shader, color_format,
          VK_FORMAT_UNDEFINED, false_v, false_v, blend_enabled, false_v,
          out_pipeline))
    return false_v;
  return true_v;
}

bool8_t vkr_vk_recreate_presentation_pipelines(VkrVulkanRenderer *renderer,
                                                VkFormat color_format) {
  const VkrVulkanPacketPipeline pipelines[] = {
      VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY,
      VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
      VKR_VULKAN_PACKET_PIPELINE_UI,
      VKR_VULKAN_PACKET_PIPELINE_UI_RECT,
  };
  const VkrVulkanPacketShader vertex_shaders[] = {
      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX,
      VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
      VKR_VULKAN_PACKET_SHADER_UI_VERTEX,
      VKR_VULKAN_PACKET_SHADER_UI_RECT_VERTEX,
  };
  const VkrVulkanPacketShader fragment_shaders[] = {
      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_FRAGMENT,
      VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
      VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT,
      VKR_VULKAN_PACKET_SHADER_UI_RECT_FRAGMENT,
  };
  const bool8_t blends[] = {false_v, false_v, true_v, true_v};
  VkPipeline replacements[ArrayCount(pipelines)] = {0};
  for (uint32_t i = 0u; i < ArrayCount(pipelines); ++i) {
    if (vkr_vk_create_presentation_pipeline(
            renderer, pipelines[i], vertex_shaders[i], fragment_shaders[i],
            color_format, blends[i], &replacements[i]))
      continue;
    for (uint32_t j = 0u; j < i; ++j)
      vkDestroyPipeline(vkr_vk_renderer_device(renderer), replacements[j], NULL);
    return false_v;
  }
  for (uint32_t i = 0u; i < ArrayCount(pipelines); ++i) {
    VkPipeline old = renderer->packet_pipelines[pipelines[i]];
    renderer->packet_pipelines[pipelines[i]] = replacements[i];
    if (old)
      vkDestroyPipeline(vkr_vk_renderer_device(renderer), old, NULL);
  }
  return true_v;
}

vkr_internal bool8_t
vkr_vk_create_packet_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[VKR_VULKAN_PACKET_SHADER_COUNT] = {
      VKR_VULKAN_PACKET_WORLD_VERT_SPV,
      VKR_VULKAN_PACKET_WORLD_TEMPORAL_VERT_SPV,
      VKR_VULKAN_PACKET_WORLD_FRAG_SPV,
      VKR_VULKAN_PACKET_WORLD_TEMPORAL_FRAG_SPV,
      VKR_VULKAN_PACKET_PICKING_FRAG_SPV,
      VKR_VULKAN_PACKET_FULLSCREEN_VERT_SPV,
      VKR_VULKAN_PACKET_FULLSCREEN_FRAG_SPV,
      VKR_VULKAN_PACKET_TEXT_VERT_SPV,
      VKR_VULKAN_PACKET_TEXT_FRAG_SPV,
      VKR_VULKAN_PACKET_TEXT_PICKING_FRAG_SPV,
      VKR_VULKAN_PACKET_UI_VERT_SPV,
      VKR_VULKAN_PACKET_UI_FRAG_SPV,
      VKR_VULKAN_PACKET_UI_RECT_VERT_SPV,
      VKR_VULKAN_PACKET_UI_RECT_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_VERT_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_OPAQUE_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_SHADOW_FRAG_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_VERT_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_FRAG_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_PICKING_FRAG_SPV,
  };
  for (uint32_t i = 0u; i < VKR_VULKAN_PACKET_SHADER_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->packet_shaders[i]))
      return false_v;
  }
  const VkFormat presentation_format =
      renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN
          ? VK_FORMAT_R8G8B8A8_SRGB
          : renderer->window_target.format;
  return vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY,
             VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_FRAGMENT,
             presentation_format, VK_FORMAT_UNDEFINED, false_v, false_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY_PICKING,
             VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_PICKING_FRAGMENT,
             VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, false_v, false_v, false_v,
             false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_PICKING,
             VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX,
             VKR_VULKAN_PACKET_SHADER_PICKING_FRAGMENT, VK_FORMAT_R32_UINT,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND,
             VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_VERTEX,
             VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
             presentation_format, VK_FORMAT_UNDEFINED, false_v, false_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_UI,
             VKR_VULKAN_PACKET_SHADER_UI_VERTEX,
             VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT, presentation_format,
             VK_FORMAT_UNDEFINED, false_v, false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
             VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
             VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_TEXT_PICKING_FRAGMENT, VK_FORMAT_R32_UINT,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_UI_RECT,
             VKR_VULKAN_PACKET_SHADER_UI_RECT_VERTEX,
             VKR_VULKAN_PACKET_SHADER_UI_RECT_FRAGMENT, presentation_format,
             VK_FORMAT_UNDEFINED, false_v, false_v, true_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT,
             VK_FORMAT_R32G32_UINT, VK_FORMAT_D32_SFLOAT, true_v, true_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT,
             VK_FORMAT_R32G32_UINT, VK_FORMAT_D32_SFLOAT, true_v, true_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT,
             VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v,
             true_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE,
             VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
             VKR_VULKAN_PACKET_SHADER_COUNT, VK_FORMAT_UNDEFINED,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, true_v);
}

vkr_internal bool8_t vkr_vk_create_ibl_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
      VKR_VULKAN_PACKET_IBL_EQUIRECT_COMP_SPV,
      VKR_VULKAN_PACKET_IBL_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_IBL_SH_COMP_SPV,
  };
  vkr_local_persist const char *const entries[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
      "ibl_equirect",
      "ibl_prefilter",
      "ibl_sh",
  };
  for (uint32_t i = 0u; i < VKR_VULKAN_IBL_PIPELINE_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->ibl_shaders[i]))
      return false_v;
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = renderer->ibl_shaders[i],
        .pName = entries[i],
    };
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = stage,
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(vkr_vk_renderer_device(renderer),
                                 renderer->pipeline_cache, 1u, &info, NULL,
                                 &renderer->ibl_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_vk_create_atmosphere_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char
      *const paths[VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT] = {
          VKR_VULKAN_PACKET_ATMOSPHERE_TRANSMITTANCE_COMP_SPV,
          VKR_VULKAN_PACKET_ATMOSPHERE_MULTIPLE_SCATTERING_COMP_SPV,
          VKR_VULKAN_PACKET_ATMOSPHERE_SOURCE_COMP_SPV,
          VKR_VULKAN_PACKET_ATMOSPHERE_SUN_COMP_SPV,
      };
  vkr_local_persist const char
      *const entries[VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT] = {
          "atmosphere_transmittance_compute",
          "atmosphere_multiple_scattering_compute",
          "atmosphere_source_compute",
          "atmosphere_sun_compute",
      };
  for (uint32_t i = 0u; i < VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->atmosphere_shaders[i]))
      return false_v;
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = renderer->atmosphere_shaders[i],
                  .pName = entries[i]},
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(
            vkr_vk_renderer_device(renderer), renderer->pipeline_cache, 1u,
            &info, NULL, &renderer->atmosphere_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_vk_create_deferred_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[] = {
      VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV,
      VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV,
      VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV,
      VKR_VULKAN_PACKET_TEMPORAL_TRANSFORM_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_NONE_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_DEBUG_COMP_SPV,
      VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_DEBUG_COMP_SPV,
      VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV,
      VKR_VULKAN_PACKET_TEMPORAL_RESOLVE_COMP_SPV,
      VKR_VULKAN_PACKET_FSR31_PREPARE_COMP_SPV,
      VKR_VULKAN_PACKET_FSR31_STABILIZE_COMP_SPV,
      VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV,
      VKR_VULKAN_PACKET_SSR_DEPTH_BASE_COMP_SPV,
      VKR_VULKAN_PACKET_SSR_DEPTH_MIP_COMP_SPV,
      VKR_VULKAN_PACKET_SSR_TRACE_COMP_SPV,
      VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV,
      VKR_VULKAN_PACKET_SSR_COMPOSITE_COMP_SPV,
      VKR_VULKAN_PACKET_SSGI_DEPTH_BASE_COMP_SPV,
      VKR_VULKAN_PACKET_SSGI_DEPTH_MIP_COMP_SPV,
      VKR_VULKAN_PACKET_SSGI_TRACE_COMP_SPV,
      VKR_VULKAN_PACKET_SSGI_TEMPORAL_COMP_SPV,
      VKR_VULKAN_PACKET_SSGI_COMPOSITE_COMP_SPV,
      VKR_VULKAN_PACKET_FOG_APPLY_COMP_SPV,
      VKR_VULKAN_PACKET_FROXEL_INJECT_COMP_SPV,
      VKR_VULKAN_PACKET_FROXEL_INTEGRATE_COMP_SPV,
      VKR_VULKAN_PACKET_FROXEL_APPLY_COMP_SPV,
      VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV,
      VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_TEMPORAL_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_TEMPORAL_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_CLEAR_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_FINALIZE_COMP_SPV,
      VKR_VULKAN_PACKET_TRANSMISSION_COVERAGE_COMP_SPV,
      VKR_VULKAN_PACKET_EXPOSURE_CLEAR_COMP_SPV,
      VKR_VULKAN_PACKET_EXPOSURE_HISTOGRAM_COMP_SPV,
      VKR_VULKAN_PACKET_EXPOSURE_RESOLVE_COMP_SPV,
      VKR_VULKAN_PACKET_SUBSURFACE_GATHER_COMP_SPV,
      VKR_VULKAN_PACKET_MOTION_BLUR_TILE_MAX_COMP_SPV,
      VKR_VULKAN_PACKET_MOTION_BLUR_NEIGHBOR_MAX_COMP_SPV,
      VKR_VULKAN_PACKET_MOTION_BLUR_RECONSTRUCT_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_COC_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_DILATE_HORIZONTAL_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_DILATE_VERTICAL_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_GATHER_COMP_SPV,
      VKR_VULKAN_PACKET_DOF_COMPOSITE_COMP_SPV,
      VKR_VULKAN_PACKET_BLOOM_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_TENT13_COMP_SPV,
      VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_BOX4_COMP_SPV,
      VKR_VULKAN_PACKET_BLOOM_UPSAMPLE_COMP_SPV,
      VKR_VULKAN_PACKET_BLOOM_COMBINE_COMP_SPV,
      VKR_VULKAN_PACKET_GTAO_DEPTH_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_GTAO_DEPTH_MIP_COMP_SPV,
      VKR_VULKAN_PACKET_GTAO_EVALUATE_COMP_SPV,
      VKR_VULKAN_PACKET_GTAO_DENOISE_COMP_SPV,
  };
  vkr_local_persist const char
      *const entries[] = {
          "vk_gpu_draw_classify",
          "vk_gpu_draw_prefix",
          "vk_gpu_draw_encode",
          "vk_temporal_transform",
          "vk_gbuffer_resolve",
          "vk_gbuffer_resolve",
          "vk_gbuffer_resolve",
          "vk_gbuffer_resolve",
          "vk_deferred_lighting",
          "vk_temporal_resolve",
          "vk_fsr31_prepare",
          "vk_fsr31_stabilize",
          "vk_hzb_build",
          "vk_ssr_depth_base",
          "vk_ssr_depth_mip",
          "vk_ssr_trace",
          "vk_ssr_temporal",
          "vk_ssr_composite",
          "ssgi_depth_base_compute",
          "ssgi_depth_mip_compute",
          "ssgi_trace_compute",
          "ssgi_temporal_compute",
          "ssgi_composite_compute",
          "fog_apply_compute",
          "froxel_inject_compute",
          "froxel_integrate_compute",
          "froxel_apply_compute",
          "vk_sdsm_reduce",
          "vk_picking_resolve",
          "vk_transmission_shade",
          "vk_transmission_shade_partitioned",
          "vk_transmission_shade_production",
          "vk_transmission_shade_production_temporal",
          "vk_transmission_shade_partitioned_production",
          "vk_transmission_shade_partitioned_production_temporal",
          "vk_transmission_compact_clear",
          "vk_transmission_compact",
          "vk_transmission_compact_finalize",
          "vk_transmission_coverage",
          "vk_exposure_clear",
          "vk_exposure_histogram",
          "vk_exposure_resolve",
          "vk_subsurface_gather",
          "vk_motion_blur_tile_max",
          "vk_motion_blur_neighbor_max",
          "vk_motion_blur_reconstruct",
          "vk_dof_coc",
          "vk_dof_dilate_horizontal",
          "vk_dof_dilate_vertical",
          "vk_dof_prefilter",
          "vk_dof_gather",
          "vk_dof_composite",
          "vk_bloom_prefilter",
          "vk_bloom_downsample_tent13",
          "vk_bloom_downsample_box4",
          "vk_bloom_upsample",
          "vk_bloom_combine",
          "vk_gtao_depth_prefilter",
          "vk_gtao_depth_mip",
          "vk_gtao_evaluate",
          "vk_gtao_denoise",
      };
  _Static_assert(ArrayCount(paths) == VKR_VULKAN_DEFERRED_PIPELINE_COUNT &&
                     ArrayCount(entries) == ArrayCount(paths),
                 "Deferred pipelines require one module and entry per kind");
  for (uint32_t i = 0u; i < VKR_VULKAN_DEFERRED_PIPELINE_COUNT; ++i) {
    if (!vkr_vk_create_shader_module(renderer, paths[i],
                                     &renderer->deferred_shaders[i]))
      return false_v;
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = renderer->deferred_shaders[i],
        .pName = entries[i],
    };
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = stage,
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(
            vkr_vk_renderer_device(renderer), renderer->pipeline_cache, 1u,
            &info, NULL, &renderer->deferred_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}
