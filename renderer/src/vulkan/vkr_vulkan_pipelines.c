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

/* Reflection walks one module at a time during single-threaded renderer
   creation. Naming that module turns a bare member name into an actionable
   shader/host mismatch. */
vkr_global const char *vkr_vk_reflect_shader = "?";

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
    log_error("%s: shader ABI member %s is %s (offset %u, expected %u)",
              vkr_vk_reflect_shader, name, member ? "misaligned" : "missing",
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

typedef struct VkrVulkanReflectedField {
  const char *name;
  uint32_t offset;
} VkrVulkanReflectedField;

#define VKR_VULKAN_REFLECTED_FIELD(type, member)                               \
  {#member, (uint32_t)offsetof(type, member)}

vkr_global const VkrVulkanReflectedField s_vk_packet_draw_root_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, geometry_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, visible_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, vertices),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, frame),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, visible_row_index),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, flags),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketDrawRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_packet_frame_root_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, materials),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, instances),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               instance_address_padding),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, transmission_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, transmission_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, flags),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, shadow_debug_mode),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, point_light_data),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               point_light_grid_origin_cell_size),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, view),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               shadow_pcf_uniform_early_out),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, ibl_probes),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, temporal_draw_state),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, local_shadow_views),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, local_shadow_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               local_shadow_transmission),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, dfg_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, dfg_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               diffuse_volume_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, diffuse_volume_origin),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               diffuse_volume_inverse_spacing),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               diffuse_volume_dimensions),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, ltc),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, sheen),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, anisotropy),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, fog),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, froxel_fog),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot,
                               froxel_integrated_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanPacketFrameRoot, froxel_sampler),
};

vkr_global const VkrVulkanReflectedField
    s_vk_frame_local_shadow_transmission_fields[] = {
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission,
                                   depth0_texture),
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission,
                                   color0_texture),
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission,
                                   depth1_texture),
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission,
                                   color1_texture),
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission,
                                   overflow_texture),
        VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmission, extent),
};

vkr_global const VkrVulkanReflectedField s_vk_frame_ltc_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, lights),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, matrix_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, amplitude_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, count),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLtc, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_frame_sheen_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, directional_albedo_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, ltc_matrix_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, ltc_amplitude_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, ltc_matrix_texture_b),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, ltc_amplitude_texture_b),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSheen, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_frame_anisotropy_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnisotropy, table0_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnisotropy, table1_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnisotropy, table2_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnisotropy, sampler_index),
};

vkr_global const VkrVulkanReflectedField s_vk_frame_fog_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrFogGpuParams, color_density),
    VKR_VULKAN_REFLECTED_FIELD(VkrFogGpuParams, height_distance),
};

vkr_global const VkrVulkanReflectedField s_vk_frame_froxel_fog_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, inverse_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, previous_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, previous_view),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, color_density),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams,
                               grid_dimensions_cell_pixels),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams,
                               selected_local_indices_count),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, boxes),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams, current_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrFroxelFogGpuParams,
                               inverse_raster_view_projection),
};

vkr_global const VkrVulkanReflectedField s_vk_material_row_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, base_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, base_color_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_id),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, alpha_mode),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_emissive),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_surface),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               material_attenuation_color),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_clearcoat),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               clearcoat_roughness_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               clearcoat_normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, clearcoat_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               clearcoat_roughness_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               clearcoat_normal_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_sheen),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, sheen_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               sheen_roughness_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, sheen_color_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               sheen_roughness_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_anisotropy),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, anisotropy_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, anisotropy_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow,
                               material_diffuse_transmission),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, material_subsurface),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanMaterialGpuRow, temporal_reactivity),
};

/* Validates each field offset of `parent` and its exact reflected size. A
 * missing `parent` fails and reports every field as missing. */
vkr_internal bool8_t vkr_vk_validate_reflected_fields(
    SpvReflectBlockVariable *parent, const VkrVulkanReflectedField *fields,
    uint32_t field_count, uint32_t expected_size) {
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < field_count; ++i) {
    valid &= vkr_vk_reflect_member_offset(parent, fields[i].name,
                                          fields[i].offset, NULL);
  }
  return valid && parent &&
         vkr_vk_reflected_struct_size(parent) == expected_size;
}

vkr_internal bool8_t
vkr_vk_validate_packet_root_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_WORLD_TEMPORAL_FRAG_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
    SpvReflectBlockVariable *frame = vkr_vk_reflect_member(root, "frame");
    SpvReflectBlockVariable *vertices = vkr_vk_reflect_member(root, "vertices");
    SpvReflectBlockVariable *materials =
        vkr_vk_reflect_member(frame, "materials");
    SpvReflectBlockVariable *ltc = vkr_vk_reflect_member(frame, "ltc");
    valid &= vkr_vk_validate_reflected_fields(
        root, s_vk_packet_draw_root_fields,
        ArrayCount(s_vk_packet_draw_root_fields),
        sizeof(VkrVulkanPacketDrawRoot));
    valid &= vkr_vk_validate_reflected_fields(
        frame, s_vk_packet_frame_root_fields,
        ArrayCount(s_vk_packet_frame_root_fields),
        sizeof(VkrVulkanPacketFrameRoot));
    valid &= vkr_vk_validate_reflected_fields(
        vkr_vk_reflect_member(frame, "local_shadow_transmission"),
        s_vk_frame_local_shadow_transmission_fields,
        ArrayCount(s_vk_frame_local_shadow_transmission_fields),
        sizeof(VkrVulkanLocalShadowTransmission));
    valid &= vkr_vk_validate_reflected_fields(ltc, s_vk_frame_ltc_fields,
                                              ArrayCount(s_vk_frame_ltc_fields),
                                              sizeof(VkrVulkanLtc));
    valid &= vkr_vk_validate_reflected_fields(
        vkr_vk_reflect_member(frame, "sheen"), s_vk_frame_sheen_fields,
        ArrayCount(s_vk_frame_sheen_fields), sizeof(VkrVulkanSheen));
    valid &= vkr_vk_validate_reflected_fields(
        vkr_vk_reflect_member(frame, "anisotropy"),
        s_vk_frame_anisotropy_fields, ArrayCount(s_vk_frame_anisotropy_fields),
        sizeof(VkrVulkanAnisotropy));
    valid &= vkr_vk_validate_reflected_fields(
        vkr_vk_reflect_member(frame, "fog"), s_vk_frame_fog_fields,
        ArrayCount(s_vk_frame_fog_fields), sizeof(VkrFogGpuParams));
    valid &= vkr_vk_validate_reflected_fields(
        vkr_vk_reflect_member(frame, "froxel_fog"),
        s_vk_frame_froxel_fog_fields, ArrayCount(s_vk_frame_froxel_fog_fields),
        sizeof(VkrFroxelFogGpuParams));
    valid &= vkr_vk_validate_reflected_fields(
        materials, s_vk_material_row_fields,
        ArrayCount(s_vk_material_row_fields), sizeof(VkrVulkanMaterialGpuRow));
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(frame, "local_shadow_views"),
        VKR_GPU_ABI_LOCAL_SHADOW_VIEW);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(ltc, "lights"), VKR_GPU_ABI_RECTANGLE_LIGHT_ROW);
    const VkrGpuAbiRecord *vertex_abi =
        vkr_gpu_abi_record(VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &=
        vertices && vertex_abi &&
        vkr_vk_reflected_struct_size(vertices) == vertex_abi->expected_size;
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vertices, VKR_GPU_ABI_PACKED_STATIC_VERTEX);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(root, "geometry_rows"), VKR_GPU_ABI_GEOMETRY_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(root, "visible_rows"),
        VKR_GPU_ABI_VISIBLE_DRAW_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(frame, "point_light_data"),
        VKR_GPU_ABI_POINT_LIGHT_ROW);
    valid &= vkr_vk_validate_reflected_gpu_abi(
        vkr_vk_reflect_member(frame, "instances"), VKR_GPU_ABI_INSTANCE);
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_internal bool8_t vkr_vk_validate_root_abi_with_gpu_record(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry,
    const VkrVulkanReflectedField *fields, uint32_t field_count,
    uint32_t expected_size, const char *gpu_field,
    VkrGpuAbiRecordId gpu_record_id) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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

vkr_internal bool8_t vkr_vk_validate_subsurface_root_abi(
    VkrVulkanRenderer *renderer, const char *shader, const char *entry) {
  FilePath shader_path =
      file_path_create(shader, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSubsurfaceRoot,
                                 inverse_view_projection),
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
  valid &= vkr_vk_reflect_member_offset(
      params, "projection", offsetof(VkrSubsurfaceGpuParams, projection), NULL);
  valid &= vkr_vk_reflect_member_offset(
      params, "dimensions", offsetof(VkrSubsurfaceGpuParams, dimensions), NULL);
  valid &=
      vkr_vk_reflected_struct_size(root) == sizeof(VkrVulkanSubsurfaceRoot);
  valid &=
      vkr_vk_reflected_struct_size(params) == sizeof(VkrSubsurfaceGpuParams);
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
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
vkr_vk_validate_ssr_reprojection_abi(VkrVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  vkr_vk_reflect_shader =
      shader_path.path.str ? (const char *)shader_path.path.str : "?";
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
                      &module, "vk_ssr_temporal", &count, NULL) ==
                      SPV_REFLECT_RESULT_SUCCESS &&
                  count == 1u &&
                  spvReflectEnumerateEntryPointPushConstantBlocks(
                      &module, "vk_ssr_temporal", &count, blocks) ==
                      SPV_REFLECT_RESULT_SUCCESS;
  valid &= blocks[0] && blocks[0]->size == sizeof(VkrVulkanPushConstants);
  SpvReflectBlockVariable *root =
      valid ? vkr_vk_reflect_member(blocks[0], "root") : NULL;
  SpvReflectBlockVariable *reprojection =
      vkr_vk_reflect_member(root, "reprojection");
  valid &= vkr_vk_reflect_member_offset(
      reprojection, "inverse_view",
      offsetof(VkrSsrReprojectionGpuParams, inverse_view), NULL);
  valid &= vkr_vk_reflect_member_offset(
      reprojection, "previous_view",
      offsetof(VkrSsrReprojectionGpuParams, previous_view), NULL);
  valid &= vkr_vk_reflected_struct_size(reprojection) ==
           sizeof(VkrSsrReprojectionGpuParams);
  SpvReflectBlockVariable *transforms =
      vkr_vk_reflect_member(root, "previous_transforms");
  static const VkrVulkanReflectedField transform_fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrTemporalTransformGPU, model),
      VKR_VULKAN_REFLECTED_FIELD(VkrTemporalTransformGPU, generation),
      VKR_VULKAN_REFLECTED_FIELD(VkrTemporalTransformGPU, frame_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrTemporalTransformGPU, valid),
      VKR_VULKAN_REFLECTED_FIELD(VkrTemporalTransformGPU, reserved),
  };
  for (uint32_t i = 0u; i < ArrayCount(transform_fields); ++i)
    valid &= vkr_vk_reflect_member_offset(transforms, transform_fields[i].name,
                                          transform_fields[i].offset, NULL);
  valid &= vkr_vk_reflected_struct_size(transforms) ==
           sizeof(VkrTemporalTransformGPU);
  spvReflectDestroyShaderModule(&module);
  return valid;
}

vkr_global const VkrVulkanReflectedField s_vk_cull_fields[] = {
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
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, local_shadow_first_view),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, transmission_first_view),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, transmission_required_flags),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, local_shadow_excluded_flags),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCullRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_local_transmission_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmissionRoot, raster),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmissionRoot,
                               transmission_materials),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmissionRoot,
                               previous_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmissionRoot, reserved),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLocalShadowTransmissionRoot,
                               light_position),
};

vkr_global const VkrVulkanReflectedField s_vk_resolve_fields[] = {
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
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanResolveRoot, previous_view_projection),
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

vkr_global const VkrVulkanReflectedField s_vk_temporal_resolve_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, visible_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, instances),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, scene_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot,
                               pre_transmission_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, motion_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, validity_texture),
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
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanTemporalResolveRoot, camera_stationary),
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

vkr_global const VkrVulkanReflectedField s_vk_lighting_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, frame),
    {"frame_address_padding",
     (uint32_t)offsetof(VkrVulkanLightingRoot, frame_padding)},
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, inverse_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, albedo_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, specular_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, scene_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_view_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_mode),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, gtao_visibility_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sky_radiance),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, direct_source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, ssgi_enabled),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, sheen_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, anisotropy_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, visible_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot,
                               subsurface_source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanLightingRoot, subsurface_profile_count),
};

vkr_global const VkrVulkanReflectedField s_vk_atmosphere_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, transmittance_sample),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, transmittance_storage),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot,
                               multiple_scattering_sample),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot,
                               multiple_scattering_storage),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, source_storage),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, face_size),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAtmosphereRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_picking_fields[] = {
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

vkr_global const VkrVulkanReflectedField s_vk_hzb_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, destination_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, destination_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, source_is_depth),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanHzbRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssr_depth_base_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot,
                               destination_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthBaseRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssr_depth_mip_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, source_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot,
                               destination_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, source_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, destination_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrDepthMipRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssr_trace_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, specular_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, depth_pyramid_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, destination_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, source_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, hit_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTraceRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssr_temporal_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, reprojection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, visible_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, instances),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, previous_transforms),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, previous_frame_index),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, raw_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, hit_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, history_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, history_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                               history_identity_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, output_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, output_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot,
                               output_identity_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, specular_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrTemporalRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssr_composite_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, frame),
    {"frame_address_padding",
     (uint32_t)offsetof(VkrVulkanSsrCompositeRoot, frame_padding)},
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
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, sheen_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsrCompositeRoot, anisotropy_texture),
};

vkr_global const VkrVulkanReflectedField s_vk_ssgi_depth_base_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot,
                               destination_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthBaseRoot, receiver_texture),
};

vkr_global const VkrVulkanReflectedField s_vk_ssgi_depth_mip_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, source_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot,
                               destination_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, source_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, destination_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiDepthMipRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_ssgi_trace_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, albedo_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, depth_pyramid_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, direct_source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, destination_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTraceRoot, receiver_texture),
};

vkr_global const VkrVulkanReflectedField s_vk_ssgi_temporal_fields[] = {
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
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, output_color_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, output_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot,
                               output_identity_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, linear_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, reserved),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiTemporalRoot, receiver_texture),
};

vkr_global const VkrVulkanReflectedField s_vk_ssgi_composite_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, frame),
    {"frame_address_padding",
     (uint32_t)offsetof(VkrVulkanSsgiCompositeRoot, frame_padding)},
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                               inverse_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, scene_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, reflection_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, albedo_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, normal_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                               history_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, specular_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, linear_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, clearcoat_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, sheen_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, anisotropy_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, visible_rows),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                               subsurface_source_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot,
                               subsurface_profile_count),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, receiver_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSsgiCompositeRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_fog_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, inverse_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, camera_position),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, target_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, sky),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, aerial_perspective_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, reserved),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, frame),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFogRoot, frame_reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_sky_build_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkyBuildRoot, sky),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkyBuildRoot, output_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkyBuildRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_cloud_noise_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudNoiseRoot, base_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudNoiseRoot, detail_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudNoiseRoot, weather_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudNoiseRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_cloud_trace_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot,
                               previous_view_projection),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, sky),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, frame),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, history_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, history_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, output_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, depth_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, frame_index),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, history_valid),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanCloudTraceRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_froxel_inject_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, frame),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, output_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, history_valid),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelInjectRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_froxel_integrate_fields[] = {
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

vkr_global const VkrVulkanReflectedField s_vk_froxel_apply_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, frame),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, params),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, integrated_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, integrated_sampler),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, target_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot,
                               aerial_perspective_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFroxelApplyRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_skinning_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, bind_vertices),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, influences),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, palette),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, output),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, vertex_count),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, palette_count),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, reserved0),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSkinningRoot, reserved1),
};

vkr_global const VkrVulkanReflectedField s_vk_sdsm_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, reduce_state),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, vbuffer_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanSdsmRoot, reserved),
};

vkr_global const VkrVulkanReflectedField s_vk_fsr31_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, scene_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                               pre_transmission_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, validity_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, opaque_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot,
                               transmission_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, output_depth_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, reactive_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, composition_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, transmission_enabled),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31PrepareRoot, scene_stationary),
};

vkr_global const VkrVulkanReflectedField s_vk_fsr31_stabilize_fields[] = {
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, output_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, history_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, reactive_texture),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, scene_stationary),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, output_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, render_extent),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, jitter_pixels),
    VKR_VULKAN_REFLECTED_FIELD(VkrVulkanFsr31StabilizeRoot, reserved),
};

typedef struct VkrVulkanReflectedRoot {
  const char *shader;
  const char *entry;
  const VkrVulkanReflectedField *fields;
  uint32_t field_count;
  uint32_t size;
  /* Optional root member whose pointee must match a shared GPU ABI record. */
  const char *gpu_field;
  VkrGpuAbiRecordId gpu_record;
} VkrVulkanReflectedRoot;

#define VKR_VULKAN_REFLECTED_ROOT(shader_path, entry_name, field_table,        \
                                  root_type)                                   \
  {.shader = (shader_path),                                                    \
   .entry = (entry_name),                                                      \
   .fields = (field_table),                                                    \
   .field_count = ArrayCount(field_table),                                     \
   .size = sizeof(root_type),                                                  \
   .gpu_record = VKR_GPU_ABI_RECORD_COUNT}

/* Deferred, post and compute roots validated by field offset and size. */
vkr_global const VkrVulkanReflectedRoot s_vk_deferred_roots[] = {
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SKINNING_COMP_SPV,
                              "vk_skinning", s_vk_skinning_fields,
                              VkrVulkanSkinningRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV,
                              "vk_deferred_lighting", s_vk_lighting_fields,
                              VkrVulkanLightingRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_FSR31_PREPARE_COMP_SPV,
                              "vk_fsr31_prepare", s_vk_fsr31_fields,
                              VkrVulkanFsr31PrepareRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_FSR31_STABILIZE_COMP_SPV,
                              "vk_fsr31_stabilize", s_vk_fsr31_stabilize_fields,
                              VkrVulkanFsr31StabilizeRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_TEMPORAL_RESOLVE_COMP_SPV, "vk_temporal_resolve",
        s_vk_temporal_resolve_fields, VkrVulkanTemporalResolveRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV,
                              "vk_picking_resolve", s_vk_picking_fields,
                              VkrVulkanPickingRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV,
                              "vk_hzb_build", s_vk_hzb_fields,
                              VkrVulkanHzbRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSR_DEPTH_BASE_COMP_SPV,
                              "vk_ssr_depth_base", s_vk_ssr_depth_base_fields,
                              VkrVulkanSsrDepthBaseRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSR_DEPTH_MIP_COMP_SPV,
                              "vk_ssr_depth_mip", s_vk_ssr_depth_mip_fields,
                              VkrVulkanSsrDepthMipRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSR_TRACE_COMP_SPV,
                              "vk_ssr_trace", s_vk_ssr_trace_fields,
                              VkrVulkanSsrTraceRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV,
                              "vk_ssr_temporal", s_vk_ssr_temporal_fields,
                              VkrVulkanSsrTemporalRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSR_COMPOSITE_COMP_SPV,
                              "vk_ssr_composite", s_vk_ssr_composite_fields,
                              VkrVulkanSsrCompositeRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_SSGI_DEPTH_BASE_COMP_SPV, "ssgi_depth_base_compute",
        s_vk_ssgi_depth_base_fields, VkrVulkanSsgiDepthBaseRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_SSGI_DEPTH_MIP_COMP_SPV, "ssgi_depth_mip_compute",
        s_vk_ssgi_depth_mip_fields, VkrVulkanSsgiDepthMipRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SSGI_TRACE_COMP_SPV,
                              "ssgi_trace_compute", s_vk_ssgi_trace_fields,
                              VkrVulkanSsgiTraceRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_SSGI_TEMPORAL_COMP_SPV, "ssgi_temporal_compute",
        s_vk_ssgi_temporal_fields, VkrVulkanSsgiTemporalRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_SSGI_COMPOSITE_COMP_SPV, "ssgi_composite_compute",
        s_vk_ssgi_composite_fields, VkrVulkanSsgiCompositeRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SKY_VIEW_COMP_SPV,
                              "sky_view_compute", s_vk_sky_build_fields,
                              VkrVulkanSkyBuildRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_AERIAL_PERSPECTIVE_COMP_SPV,
                              "aerial_perspective_compute",
                              s_vk_sky_build_fields, VkrVulkanSkyBuildRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_CLOUD_BASE_NOISE_COMP_SPV,
                              "cloud_base_noise_compute",
                              s_vk_cloud_noise_fields, VkrVulkanCloudNoiseRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_CLOUD_DETAIL_NOISE_COMP_SPV,
                              "cloud_detail_noise_compute",
                              s_vk_cloud_noise_fields, VkrVulkanCloudNoiseRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_CLOUD_WEATHER_COMP_SPV,
                              "cloud_weather_compute", s_vk_cloud_noise_fields,
                              VkrVulkanCloudNoiseRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_CLOUD_SHADOW_COMP_SPV,
                              "cloud_shadow_compute", s_vk_sky_build_fields,
                              VkrVulkanSkyBuildRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_CLOUD_TRACE_COMP_SPV,
                              "cloud_trace_compute", s_vk_cloud_trace_fields,
                              VkrVulkanCloudTraceRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_FOG_APPLY_COMP_SPV,
                              "fog_apply_compute", s_vk_fog_fields,
                              VkrVulkanFogRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV,
                              "vk_sdsm_reduce", s_vk_sdsm_fields,
                              VkrVulkanSdsmRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_VERT_SPV,
        "vk_local_shadow_transmission_vertex", s_vk_local_transmission_fields,
        VkrVulkanLocalShadowTransmissionRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_FRAG_SPV,
        "vk_local_shadow_transmission_fragment", s_vk_local_transmission_fields,
        VkrVulkanLocalShadowTransmissionRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAG_SPV,
        "vk_local_shadow_transmission_overflow_fragment",
        s_vk_local_transmission_fields, VkrVulkanLocalShadowTransmissionRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_GBUFFER_RESOLVE_NONE_COMP_SPV,
                              "vk_gbuffer_resolve", s_vk_resolve_fields,
                              VkrVulkanResolveRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_COMP_SPV,
        "vk_gbuffer_resolve", s_vk_resolve_fields, VkrVulkanResolveRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_GBUFFER_RESOLVE_DEBUG_COMP_SPV,
                              "vk_gbuffer_resolve", s_vk_resolve_fields,
                              VkrVulkanResolveRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_DEBUG_COMP_SPV,
        "vk_gbuffer_resolve", s_vk_resolve_fields, VkrVulkanResolveRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_ATMOSPHERE_TRANSMITTANCE_COMP_SPV,
        "atmosphere_transmittance_compute", s_vk_atmosphere_fields,
        VkrVulkanAtmosphereRoot),
    VKR_VULKAN_REFLECTED_ROOT(
        VKR_VULKAN_PACKET_ATMOSPHERE_MULTIPLE_SCATTERING_COMP_SPV,
        "atmosphere_multiple_scattering_compute", s_vk_atmosphere_fields,
        VkrVulkanAtmosphereRoot),
    VKR_VULKAN_REFLECTED_ROOT(VKR_VULKAN_PACKET_ATMOSPHERE_SOURCE_COMP_SPV,
                              "atmosphere_source_compute",
                              s_vk_atmosphere_fields, VkrVulkanAtmosphereRoot),
    {.shader = VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV,
     .entry = "vk_gpu_draw_classify",
     .fields = s_vk_cull_fields,
     .field_count = ArrayCount(s_vk_cull_fields),
     .size = sizeof(VkrVulkanCullRoot),
     .gpu_field = "candidates",
     .gpu_record = VKR_GPU_ABI_CANDIDATE_DRAW_ROW},
    {.shader = VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV,
     .entry = "vk_gpu_draw_prefix",
     .fields = s_vk_cull_fields,
     .field_count = ArrayCount(s_vk_cull_fields),
     .size = sizeof(VkrVulkanCullRoot),
     .gpu_field = "candidates",
     .gpu_record = VKR_GPU_ABI_CANDIDATE_DRAW_ROW},
    {.shader = VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV,
     .entry = "vk_gpu_draw_encode",
     .fields = s_vk_cull_fields,
     .field_count = ArrayCount(s_vk_cull_fields),
     .size = sizeof(VkrVulkanCullRoot),
     .gpu_field = "candidates",
     .gpu_record = VKR_GPU_ABI_CANDIDATE_DRAW_ROW},
};

vkr_global const char *const s_vk_motion_blur_shaders[] = {
    VKR_VULKAN_PACKET_MOTION_BLUR_TILE_MAX_COMP_SPV,
    VKR_VULKAN_PACKET_MOTION_BLUR_NEIGHBOR_MAX_COMP_SPV,
    VKR_VULKAN_PACKET_MOTION_BLUR_RECONSTRUCT_COMP_SPV,
};

vkr_global const char *const s_vk_motion_blur_entries[] = {
    "vk_motion_blur_tile_max",
    "vk_motion_blur_neighbor_max",
    "vk_motion_blur_reconstruct",
};

_Static_assert(ArrayCount(s_vk_motion_blur_shaders) ==
                   ArrayCount(s_vk_motion_blur_entries),
               "motion_blur reflection entries must match their modules");

vkr_global const char *const s_vk_dof_shaders[] = {
    VKR_VULKAN_PACKET_DOF_COC_COMP_SPV,
    VKR_VULKAN_PACKET_DOF_DILATE_HORIZONTAL_COMP_SPV,
    VKR_VULKAN_PACKET_DOF_DILATE_VERTICAL_COMP_SPV,
    VKR_VULKAN_PACKET_DOF_PREFILTER_COMP_SPV,
    VKR_VULKAN_PACKET_DOF_GATHER_COMP_SPV,
    VKR_VULKAN_PACKET_DOF_COMPOSITE_COMP_SPV,
};

vkr_global const char *const s_vk_dof_entries[] = {
    "vk_dof_coc",
    "vk_dof_dilate_horizontal",
    "vk_dof_dilate_vertical",
    "vk_dof_prefilter",
    "vk_dof_gather",
    "vk_dof_composite",
};

_Static_assert(ArrayCount(s_vk_dof_shaders) == ArrayCount(s_vk_dof_entries),
               "dof reflection entries must match their modules");

vkr_internal bool8_t
vkr_vk_validate_deferred_root_abi(VkrVulkanRenderer *renderer) {
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < ArrayCount(s_vk_deferred_roots); ++i) {
    const VkrVulkanReflectedRoot *root = &s_vk_deferred_roots[i];
    valid &= vkr_vk_validate_root_abi_with_gpu_record(
        renderer, root->shader, root->entry, root->fields, root->field_count,
        root->size, root->gpu_field, root->gpu_record);
  }
  valid &= vkr_vk_validate_subsurface_root_abi(
      renderer, VKR_VULKAN_PACKET_SUBSURFACE_GATHER_COMP_SPV,
      "vk_subsurface_gather");
  for (uint32_t i = 0u; i < ArrayCount(s_vk_motion_blur_shaders); ++i) {
    valid &= vkr_vk_validate_motion_blur_root_abi(
        renderer, s_vk_motion_blur_shaders[i], s_vk_motion_blur_entries[i]);
  }
  for (uint32_t i = 0u; i < ArrayCount(s_vk_dof_shaders); ++i) {
    valid &= vkr_vk_validate_dof_root_abi(renderer, s_vk_dof_shaders[i],
                                          s_vk_dof_entries[i]);
  }
  valid &= vkr_vk_validate_ssr_reprojection_abi(renderer);
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_INJECT_COMP_SPV,
      "froxel_inject_compute", s_vk_froxel_inject_fields,
      ArrayCount(s_vk_froxel_inject_fields), sizeof(VkrVulkanFroxelInjectRoot),
      true_v);
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_INTEGRATE_COMP_SPV,
      "froxel_integrate_compute", s_vk_froxel_integrate_fields,
      ArrayCount(s_vk_froxel_integrate_fields),
      sizeof(VkrVulkanFroxelIntegrateRoot), true_v);
  valid &= vkr_vk_validate_froxel_root_abi(
      renderer, VKR_VULKAN_PACKET_FROXEL_APPLY_COMP_SPV, "froxel_apply_compute",
      s_vk_froxel_apply_fields, ArrayCount(s_vk_froxel_apply_fields),
      sizeof(VkrVulkanFroxelApplyRoot), false_v);
  return valid;
}

vkr_internal bool8_t
vkr_vk_validate_animation_preview_root_abi(VkrVulkanRenderer *renderer) {
  static const VkrVulkanReflectedField fields[] = {
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, model),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot,
                                 view_projection),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, vertices),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, decode),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot,
                                 deformation_address),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, reserved),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, tint),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, first_vertex),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, decode_index),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanAnimationPreviewRoot, reserved_tail),
  };
  return vkr_vk_validate_root_abi(
      renderer, VKR_VULKAN_PACKET_ANIMATION_PREVIEW_VERT_SPV,
      "animation_preview_vertex", fields, ArrayCount(fields),
      sizeof(VkrVulkanAnimationPreviewRoot));
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
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, flags),
      VKR_VULKAN_REFLECTED_FIELD(VkrVulkanUiRoot, display_output),
  };
  return vkr_vk_validate_root_abi(renderer, VKR_VULKAN_PACKET_UI_VERT_SPV,
                                  "ui_vertex", fields, ArrayCount(fields),
                                  sizeof(VkrVulkanUiRoot));
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

bool8_t vkr_vk_validate_shader_abi(VkrVulkanRenderer *renderer) {
  return vkr_vk_validate_animation_preview_root_abi(renderer) &&
         vkr_vk_validate_editor_overlay_root_abi(renderer) &&
         vkr_vk_validate_fullscreen_root_abi(renderer) &&
         vkr_vk_validate_ui_root_abi(renderer) &&
         vkr_vk_validate_packet_root_abi(renderer) &&
         vkr_vk_validate_gtao_root_abi(renderer) &&
         vkr_vk_validate_ibl_sh_root_abi(renderer) &&
         vkr_vk_validate_transmission_root_abi(renderer) &&
         vkr_vk_validate_deferred_root_abi(renderer);
}

bool8_t vkr_vk_create_pipelines(VkrVulkanRenderer *renderer) {
  if (!vkr_vk_validate_shader_abi(renderer)) {
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
              : vertex_shader ==
                      VKR_VULKAN_PACKET_SHADER_ANIMATION_PREVIEW_VERTEX
                  ? "animation_preview_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX
                  ? "editor_overlay_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX
                  ? "text_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_UI_VERTEX
                  ? "ui_vertex"
              : vertex_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX
                  ? "vk_visibility_vertex"
              : vertex_shader ==
                      VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_VERTEX
                  ? "vk_local_shadow_transmission_vertex"
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
                      VKR_VULKAN_PACKET_SHADER_ANIMATION_PREVIEW_FRAGMENT
                  ? "animation_preview_fragment"
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
              : fragment_shader == VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT
                  ? "vk_visibility_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT
                  ? "vk_visibility_opaque_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT
                  ? "vk_visibility_shadow_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_FRAGMENT
                  ? "vk_local_shadow_transmission_fragment"
              : fragment_shader ==
                      VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAGMENT
                  ? "vk_local_shadow_transmission_overflow_fragment"
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
  if (!vkr_vk_create_packet_pipeline_at(renderer, pipeline, vertex_shader,
                                        fragment_shader, color_format,
                                        VK_FORMAT_UNDEFINED, false_v, false_v,
                                        blend_enabled, false_v, out_pipeline))
    return false_v;
  return true_v;
}

bool8_t vkr_vk_recreate_presentation_pipelines(VkrVulkanRenderer *renderer,
                                               VkFormat color_format) {
  const VkrVulkanPacketPipeline pipelines[] = {
      VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY,
      VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
      VKR_VULKAN_PACKET_PIPELINE_UI,
  };
  const VkrVulkanPacketShader vertex_shaders[] = {
      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX,
      VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
      VKR_VULKAN_PACKET_SHADER_UI_VERTEX,
  };
  const VkrVulkanPacketShader fragment_shaders[] = {
      VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_FRAGMENT,
      VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
      VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT,
  };
  const bool8_t blends[] = {false_v, false_v, true_v};
  VkPipeline replacements[ArrayCount(pipelines)] = {0};
  for (uint32_t i = 0u; i < ArrayCount(pipelines); ++i) {
    if (vkr_vk_create_presentation_pipeline(
            renderer, pipelines[i], vertex_shaders[i], fragment_shaders[i],
            color_format, blends[i], &replacements[i]))
      continue;
    for (uint32_t j = 0u; j < i; ++j)
      vkDestroyPipeline(vkr_vk_renderer_device(renderer), replacements[j],
                        NULL);
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
      VKR_VULKAN_PACKET_VISIBILITY_VERT_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_OPAQUE_FRAG_SPV,
      VKR_VULKAN_PACKET_VISIBILITY_SHADOW_FRAG_SPV,
      VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_VERT_SPV,
      VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_FRAG_SPV,
      VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAG_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_VERT_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_FRAG_SPV,
      VKR_VULKAN_PACKET_EDITOR_OVERLAY_PICKING_FRAG_SPV,
      VKR_VULKAN_PACKET_ANIMATION_PREVIEW_VERT_SPV,
      VKR_VULKAN_PACKET_ANIMATION_PREVIEW_FRAG_SPV,
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
             renderer, VKR_VULKAN_PACKET_PIPELINE_ANIMATION_PREVIEW,
             VKR_VULKAN_PACKET_SHADER_ANIMATION_PREVIEW_VERTEX,
             VKR_VULKAN_PACKET_SHADER_ANIMATION_PREVIEW_FRAGMENT,
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true_v, true_v,
             false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
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
             renderer, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_DISPLAY_LINEAR,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, false_v,
             false_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT, presentation_format,
             VK_FORMAT_UNDEFINED, false_v, false_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(renderer, VKR_VULKAN_PACKET_PIPELINE_UI,
                                       VKR_VULKAN_PACKET_SHADER_UI_VERTEX,
                                       VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT,
                                       presentation_format, VK_FORMAT_UNDEFINED,
                                       false_v, false_v, true_v, false_v) &&
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
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, true_v) &&
         vkr_vk_create_packet_pipeline(
             renderer, VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION,
             VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_VERTEX,
             VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             true_v, false_v, false_v) &&
         vkr_vk_create_packet_pipeline(
             renderer,
             VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION_OVERFLOW,
             VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_VERTEX,
             VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAGMENT,
             VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v,
             false_v);
}

vkr_internal bool8_t vkr_vk_create_ibl_pipelines(VkrVulkanRenderer *renderer) {
  vkr_local_persist const char *const paths[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
      VKR_VULKAN_PACKET_IBL_PREFILTER_COMP_SPV,
      VKR_VULKAN_PACKET_IBL_SH_COMP_SPV,
  };
  vkr_local_persist const char *const entries[VKR_VULKAN_IBL_PIPELINE_COUNT] = {
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
      };
  vkr_local_persist const char
      *const entries[VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT] = {
          "atmosphere_transmittance_compute",
          "atmosphere_multiple_scattering_compute",
          "atmosphere_source_compute",
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
      VKR_VULKAN_PACKET_SKINNING_COMP_SPV,
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
      VKR_VULKAN_PACKET_SKY_VIEW_COMP_SPV,
      VKR_VULKAN_PACKET_AERIAL_PERSPECTIVE_COMP_SPV,
      VKR_VULKAN_PACKET_CLOUD_BASE_NOISE_COMP_SPV,
      VKR_VULKAN_PACKET_CLOUD_DETAIL_NOISE_COMP_SPV,
      VKR_VULKAN_PACKET_CLOUD_WEATHER_COMP_SPV,
      VKR_VULKAN_PACKET_CLOUD_SHADOW_COMP_SPV,
      VKR_VULKAN_PACKET_CLOUD_TRACE_COMP_SPV,
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
  vkr_local_persist const char *const entries[] = {
      "vk_gpu_draw_classify",
      "vk_gpu_draw_prefix",
      "vk_gpu_draw_encode",
      "vk_skinning",
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
      "sky_view_compute",
      "aerial_perspective_compute",
      "cloud_base_noise_compute",
      "cloud_detail_noise_compute",
      "cloud_weather_compute",
      "cloud_shadow_compute",
      "cloud_trace_compute",
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
