#include "vulkan/vkr_vulkan_internal.h"

/* Dynamic diffuse volume layer on the desktop pipeline (ADR-119): one BLAS
   per traced submesh, built from positions decoded for it; one TLAS per
   frame over the packed candidate rows; and the probe trace that blends the
   light the bake lacks into the volume's dynamic SH texture. Everything here
   needs VK_KHR_ray_query; without it the volume stays baked. */

/* Rays end this far from their probe, in metres. */
#define VKR_VULKAN_VOLUME_RAY_LENGTH 64.0f
/* A slot's TLAS refits in place between rebuilds (ADR-119). */
#define VKR_VULKAN_TLAS_BUILD_FLAGS                                            \
  (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |                 \
   VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)
/* BLASes are traced often and compacted once built (ADR-119). */
#define VKR_VULKAN_BLAS_BUILD_FLAGS                                            \
  (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |                 \
   VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR)

vkr_internal const VkrVulkanRayQueryFunctions *
vkr_vk_ray_query(const VkrVulkanRenderer *renderer) {
  return vkr_vulkan_device_ray_query_functions(renderer->device);
}

bool8_t vkr_vk_volume_trace_enabled(const VkrVulkanRenderer *renderer) {
  return renderer && vkr_vk_ray_query(renderer) &&
         renderer->volume_pipelines[VKR_VULKAN_VOLUME_PIPELINE_TRACE];
}

void vkr_vk_destroy_volume_pipelines(VkrVulkanRenderer *renderer,
                                     VkDevice device) {
  for (uint32_t i = 0u; i < VKR_VULKAN_VOLUME_PIPELINE_COUNT; ++i) {
    if (renderer->volume_pipelines[i]) {
      vkDestroyPipeline(device, renderer->volume_pipelines[i], NULL);
    }
    if (renderer->volume_shaders[i]) {
      vkDestroyShaderModule(device, renderer->volume_shaders[i], NULL);
    }
    renderer->volume_pipelines[i] = VK_NULL_HANDLE;
    renderer->volume_shaders[i] = VK_NULL_HANDLE;
  }
}

/* -------------------------------------------------------------------------
   Ray rows: written as candidates pack, read when the TLAS is built.
   ------------------------------------------------------------------------- */

bool8_t vkr_vk_reserve_ray_rows(VkrVulkanRenderer *renderer,
                                VkrVulkanFrameSlot *slot, uint32_t count) {
  if (count <= slot->ray_row_capacity) {
    return true_v;
  }
  uint32_t capacity = Max(slot->ray_row_capacity, 1024u);
  while (capacity < count) {
    capacity *= 2u;
  }
  capacity = Min(capacity, VKR_GPU_DRAW_CANDIDATE_CAPACITY);
  if (capacity < count) {
    return false_v;
  }
  VkrVulkanRayRow *rows = vkr_allocator_realloc(
      renderer->allocator, slot->ray_rows,
      (uint64_t)slot->ray_row_capacity * sizeof(*rows),
      (uint64_t)capacity * sizeof(*rows), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!rows) {
    return false_v;
  }
  slot->ray_rows = rows;
  slot->ray_row_capacity = capacity;
  return true_v;
}

void vkr_vk_record_ray_row(VkrVulkanFrameSlot *slot, uint32_t row,
                           const VkrWorldDrawCandidate *candidate,
                           const VkrVulkanPublishedMaterial *material) {
  if (row >= slot->ray_row_capacity) {
    return;
  }
  const Mat4 model = candidate->instance.model;
  VkrVulkanRayRow *ray = &slot->ray_rows[row];
  *ray = (VkrVulkanRayRow){
      .geometry = candidate->geometry,
      .submesh_index = candidate->submesh_index,
      .flags =
          (material->alpha_mode != VKR_MATERIAL_ALPHA_CUTOUT
               ? VKR_VULKAN_RAY_ROW_OPAQUE
               : 0u) |
          (candidate->instance.skinning_index != 0u ? VKR_VULKAN_RAY_ROW_SKIP
                                                    : 0u) |
          (material->double_sided ? VKR_VULKAN_RAY_ROW_DOUBLE_SIDED : 0u),
      .transform = {model.m00, model.m01, model.m02, model.m03, model.m10,
                    model.m11, model.m12, model.m13, model.m20, model.m21,
                    model.m22, model.m23},
  };
}

/* -------------------------------------------------------------------------
   Acceleration structure storage.
   ------------------------------------------------------------------------- */

vkr_internal void vkr_vk_destroy_blas(VkrVulkanRenderer *renderer,
                                      VkrVulkanBlas *blas) {
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  if (blas->handle && functions) {
    functions->destroy_acceleration_structure(vkr_vk_renderer_device(renderer),
                                              blas->handle, NULL);
  }
  if (blas->buffer.handle) {
    vkr_vk_destroy_buffer(renderer, &blas->buffer);
  }
  MemZero(blas, sizeof(*blas));
}

/* A geometry's BLASes go with it once its last use, which a TLAS that
   names them extends, has completed. */
void vkr_vk_destroy_geometry_blas(VkrVulkanRenderer *renderer,
                                  VkrVulkanPublishedGeometry *geometry) {
  if (!geometry->blas) {
    return;
  }
  renderer->blas_generation++;
  for (uint32_t i = 0u; i < geometry->submesh_count; ++i) {
    vkr_vk_destroy_blas(renderer, &geometry->blas[i]);
  }
  vkr_allocator_free(renderer->allocator, geometry->blas,
                     (uint64_t)geometry->submesh_count *
                         sizeof(*geometry->blas),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  geometry->blas = NULL;
}

vkr_internal void vkr_vk_destroy_structure(VkrVulkanRenderer *renderer,
                                           VkAccelerationStructureKHR handle,
                                           VkrVulkanBuffer *buffer) {
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  if (handle && functions) {
    functions->destroy_acceleration_structure(vkr_vk_renderer_device(renderer),
                                              handle, NULL);
  }
  if (buffer->handle) {
    vkr_vk_destroy_buffer(renderer, buffer);
  }
}

/* Destroys the slot's retired structures, and the copies of a frame that
   never submitted; the slot's previous frame has completed. */
vkr_internal void vkr_vk_collect_blas_compaction(VkrVulkanRenderer *renderer,
                                                 VkrVulkanFrameSlot *slot) {
  if (!slot->volume_lists) {
    return;
  }
  for (uint32_t i = 0u; i < slot->blas_retired_count; ++i) {
    vkr_vk_destroy_structure(renderer,
                             slot->volume_lists->blas_retired[i].handle,
                             &slot->volume_lists->blas_retired[i].buffer);
  }
  slot->blas_retired_count = 0u;
  for (uint32_t i = 0u; i < slot->blas_compaction_count; ++i) {
    vkr_vk_destroy_structure(renderer,
                             slot->volume_lists->blas_compactions[i].handle,
                             &slot->volume_lists->blas_compactions[i].buffer);
  }
  slot->blas_compaction_count = 0u;
}

void vkr_vk_destroy_volume_slot(VkrVulkanRenderer *renderer,
                                VkrVulkanFrameSlot *slot) {
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  vkr_vk_collect_blas_compaction(renderer, slot);
  if (slot->volume_lists) {
    vkr_allocator_free(renderer->allocator, slot->volume_lists,
                       sizeof(*slot->volume_lists),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    slot->volume_lists = NULL;
  }
  if (slot->blas_size_pool) {
    vkDestroyQueryPool(vkr_vk_renderer_device(renderer), slot->blas_size_pool,
                       NULL);
  }
  slot->blas_size_pool = VK_NULL_HANDLE;
  if (slot->volume_timing_pool) {
    vkDestroyQueryPool(vkr_vk_renderer_device(renderer),
                       slot->volume_timing_pool, NULL);
  }
  slot->volume_timing_pool = VK_NULL_HANDLE;
  if (slot->tlas && functions) {
    functions->destroy_acceleration_structure(vkr_vk_renderer_device(renderer),
                                              slot->tlas, NULL);
  }
  slot->tlas = VK_NULL_HANDLE;
  VkrVulkanBuffer *buffers[] = {
      &slot->tlas_buffer,  &slot->tlas_instances,
      &slot->tlas_scratch, &slot->blas_positions,
      &slot->blas_scratch, &slot->volume_probe_relight};
  for (uint32_t i = 0u; i < ArrayCount(buffers); ++i) {
    if (buffers[i]->handle) {
      vkr_vk_destroy_buffer(renderer, buffers[i]);
    }
  }
  if (slot->ray_rows) {
    vkr_allocator_free(renderer->allocator, slot->ray_rows,
                       (uint64_t)slot->ray_row_capacity *
                           sizeof(*slot->ray_rows),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  slot->ray_rows = NULL;
  slot->ray_row_capacity = 0u;
}

/* Grows a slot buffer to at least `size` bytes. The slot's previous frame
   has completed when it is prepared again, so the old buffer goes now. */
vkr_internal bool8_t vkr_vk_ensure_volume_buffer(VkrVulkanRenderer *renderer,
                                                 VkrVulkanBuffer *buffer,
                                                 VkrVulkanMemoryClass memory,
                                                 VkrGpuAllocationOwner owner,
                                                 VkDeviceSize size,
                                                 VkBufferUsageFlags usage) {
  if (buffer->handle && buffer->size >= size) {
    return true_v;
  }
  if (buffer->handle) {
    vkr_vk_destroy_buffer(renderer, buffer);
  }
  VkDeviceSize capacity = 64u * 1024u;
  while (capacity < size) {
    capacity *= 2u;
  }
  return vkr_vk_create_buffer(renderer, memory, owner, capacity,
                              usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              buffer);
}

vkr_internal VkDeviceSize vkr_vk_align_size(VkDeviceSize value,
                                            VkDeviceSize alignment) {
  return (value + alignment - 1u) / alignment * alignment;
}

vkr_internal VkAccelerationStructureGeometryKHR
vkr_vk_blas_geometry(VkDeviceAddress positions, uint32_t vertex_count) {
  return (VkAccelerationStructureGeometryKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
      .geometry.triangles =
          {
              .sType =
                  VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
              .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
              .vertexData = {.deviceAddress = positions},
              .vertexStride = 3u * sizeof(float32_t),
              .maxVertex = vertex_count ? vertex_count - 1u : 0u,
              .indexType = VK_INDEX_TYPE_NONE_KHR,
          },
  };
}

/* -------------------------------------------------------------------------
   Volume.Build: decode and build the frame's missing BLASes, then the TLAS
   over every packed candidate row whose BLAS exists.
   ------------------------------------------------------------------------- */

vkr_internal bool8_t
vkr_vk_volume_frame_active(const VkrVulkanRenderer *renderer) {
  return renderer->prepared_frame.dynamic_volume_active &&
         vkr_vk_volume_trace_enabled(renderer);
}

/* Queues `geometry`'s submesh for a BLAS build within the frame's budget;
   false once the budget is spent. */
vkr_internal bool8_t vkr_vk_queue_blas(VkrVulkanRenderer *renderer,
                                       VkrVulkanFrameSlot *slot,
                                       VkrVulkanPublishedGeometry *geometry,
                                       uint32_t submesh_index,
                                       uint32_t *triangle_budget) {
  const VkrVulkanSubmeshRange *submesh = &geometry->submeshes[submesh_index];
  const uint32_t triangles = submesh->index_count / 3u;
  if (slot->blas_job_count >= VKR_VULKAN_BLAS_BUILDS_PER_FRAME ||
      triangles == 0u ||
      (slot->blas_job_count > 0u && triangles > *triangle_budget)) {
    return false_v;
  }
  if (!geometry->blas) {
    geometry->blas = vkr_allocator_alloc(renderer->allocator,
                                         (uint64_t)geometry->submesh_count *
                                             sizeof(*geometry->blas),
                                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!geometry->blas) {
      return false_v;
    }
    MemZero(geometry->blas,
            (uint64_t)geometry->submesh_count * sizeof(*geometry->blas));
  }
  *triangle_budget -= Min(triangles, *triangle_budget);
  slot->volume_lists->blas_jobs[slot->blas_job_count++] = (VkrVulkanBlasJob){
      .blas = &geometry->blas[submesh_index],
      .geometry = geometry->handle,
      .submesh_index = submesh_index,
      .geometry_index = geometry->handle.id - 1u,
      .first_index = geometry->gpu_row.first_index + submesh->first_index,
      .index_count = triangles * 3u,
      .vertex_offset = submesh->vertex_offset,
      .decode_index = submesh->decode_index,
  };
  return true_v;
}

/* Sizes, allocates and creates every queued BLAS, and lays the jobs'
   positions and scratch out in the slot's buffers. */
vkr_internal bool8_t vkr_vk_prepare_blas_jobs(VkrVulkanRenderer *renderer,
                                              VkrVulkanFrameSlot *slot) {
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  VkDevice device = vkr_vk_renderer_device(renderer);
  VkDeviceSize positions_size = 0u;
  VkDeviceSize scratch_size = 0u;
  for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
    VkrVulkanBlasJob *job = &slot->volume_lists->blas_jobs[i];
    const uint32_t triangles = job->index_count / 3u;
    VkAccelerationStructureGeometryKHR geometry =
        vkr_vk_blas_geometry(0u, job->index_count);
    const VkAccelerationStructureBuildGeometryInfoKHR info = {
        .sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VKR_VULKAN_BLAS_BUILD_FLAGS,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1u,
        .pGeometries = &geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    functions->get_build_sizes(device,
                               VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                               &info, &triangles, &sizes);
    if (!vkr_vk_create_buffer(
            renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
            VKR_GPU_ALLOCATION_OWNER_MESH, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &job->blas->buffer)) {
      return false_v;
    }
    const VkAccelerationStructureCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = job->blas->buffer.handle,
        .size = sizes.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };
    if (functions->create_acceleration_structure(
            device, &create, NULL, &job->blas->handle) != VK_SUCCESS) {
      return false_v;
    }
    const VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = job->blas->handle,
    };
    job->blas->address = functions->get_device_address(device, &address);
    job->positions_offset = positions_size;
    positions_size +=
        vkr_vk_align_size((VkDeviceSize)job->index_count * 12u, 256u);
    job->scratch_offset = scratch_size;
    scratch_size +=
        vkr_vk_align_size(sizes.buildScratchSize, functions->scratch_alignment);
  }
  /* Once its scene has built, a slot holds no build memory; its previous
     frame, the buffers' last use, has completed. */
  if (!slot->blas_job_count) {
    if (slot->blas_positions.handle) {
      vkr_vk_destroy_buffer(renderer, &slot->blas_positions);
    }
    if (slot->blas_scratch.handle) {
      vkr_vk_destroy_buffer(renderer, &slot->blas_scratch);
    }
    return true_v;
  }
  if (!vkr_vk_ensure_volume_buffer(
          renderer, &slot->blas_positions, VKR_VULKAN_MEMORY_CLASS_DEVICE,
          VKR_GPU_ALLOCATION_OWNER_MESH, positions_size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) ||
      !vkr_vk_ensure_volume_buffer(renderer, &slot->blas_scratch,
                                   VKR_VULKAN_MEMORY_CLASS_DEVICE,
                                   VKR_GPU_ALLOCATION_OWNER_MESH,
                                   scratch_size + functions->scratch_alignment,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
    VkrVulkanBlasJob *job = &slot->volume_lists->blas_jobs[i];
    VkrVulkanVolumeDecodeRoot *root = vkr_vk_frame_upload_allocate(
        slot, sizeof(*root), _Alignof(VkrVulkanVolumeDecodeRoot),
        &job->decode_root, NULL);
    if (!root) {
      return false_v;
    }
    *root = (VkrVulkanVolumeDecodeRoot){
        .geometry_rows = slot->gpu_geometry_rows,
        .positions = slot->blas_positions.address + job->positions_offset,
        .indices = renderer->geometry_megabuffer.indices.address,
        .vertices = renderer->geometry_megabuffer.vertices.address,
        .geometry_index = job->geometry_index,
        .first_index = job->first_index,
        .index_count = job->index_count,
        .vertex_offset = job->vertex_offset,
        .decode_index = job->decode_index,
    };
  }
  return true_v;
}

/* Fills the slot's instance buffer with every packed row whose BLAS exists
   or builds this frame, queueing builds for those that do not. */
vkr_internal bool8_t vkr_vk_prepare_tlas_instances(VkrVulkanRenderer *renderer,
                                                   VkrVulkanFrameSlot *slot) {
  const uint32_t rows = Min(slot->gpu_candidate_count, slot->ray_row_capacity);
  if (!vkr_vk_ensure_volume_buffer(
          renderer, &slot->tlas_instances, VKR_VULKAN_MEMORY_CLASS_UPLOAD,
          VKR_GPU_ALLOCATION_OWNER_INSTANCE,
          (VkDeviceSize)Max(rows, 1u) *
              sizeof(VkAccelerationStructureInstanceKHR),
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) ||
      !slot->tlas_instances.allocation.mapped) {
    return false_v;
  }
  VkAccelerationStructureInstanceKHR *instances =
      slot->tlas_instances.allocation.mapped;
  uint32_t triangle_budget = VKR_VULKAN_BLAS_TRIANGLES_PER_FRAME;
  uint32_t count = 0u;
  for (uint32_t row = 0u; row < rows; ++row) {
    const VkrVulkanRayRow *ray = &slot->ray_rows[row];
    if (ray->flags & VKR_VULKAN_RAY_ROW_SKIP) {
      continue;
    }
    VkrVulkanPublishedGeometry *geometry =
        vkr_vk_resolve_geometry(renderer, ray->geometry);
    if (!geometry || geometry->pending_initialization_count ||
        ray->submesh_index >= geometry->submesh_count) {
      continue;
    }
    VkrVulkanBlas *blas =
        geometry->blas ? &geometry->blas[ray->submesh_index] : NULL;
    /* A BLAS a cancelled frame created was never built. */
    if (blas && blas->handle && !blas->built && !blas->queued) {
      vkr_vk_destroy_blas(renderer, blas);
      renderer->blas_generation++;
    }
    if (!blas || (!blas->built && !blas->queued)) {
      if (!vkr_vk_queue_blas(renderer, slot, geometry, ray->submesh_index,
                             &triangle_budget)) {
        continue;
      }
      blas = &geometry->blas[ray->submesh_index];
      blas->queued = true_v;
    }
    geometry->last_use_submit_value = renderer->submit_value + 1u;
    VkAccelerationStructureInstanceKHR *instance = &instances[count++];
    MemCopy(instance->transform.matrix, ray->transform,
            sizeof(instance->transform.matrix));
    instance->instanceCustomIndex = row;
    instance->mask = 0xFFu;
    /* Ray queries use no binding table; the offset tells the trace a
       double-sided material. */
    instance->instanceShaderBindingTableRecordOffset =
        (ray->flags & VKR_VULKAN_RAY_ROW_DOUBLE_SIDED) ? 1u : 0u;
    instance->flags =
        VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
        ((ray->flags & VKR_VULKAN_RAY_ROW_OPAQUE)
             ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR
             : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
    /* The reference is the BLAS's address, filled once jobs exist. */
    instance->accelerationStructureReference = (uint64_t)(uintptr_t)blas;
  }
  const bool8_t jobs_ready = vkr_vk_prepare_blas_jobs(renderer, slot);
  if (slot->blas_job_count) {
    renderer->blas_generation++;
  }
  for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
    slot->volume_lists->blas_jobs[i].blas->queued = false_v;
  }
  if (!jobs_ready) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrVulkanBlas *blas = (const VkrVulkanBlas *)(uintptr_t)instances[i]
                                    .accelerationStructureReference;
    instances[i].accelerationStructureReference = blas->address;
  }
  slot->tlas_instance_count = count;
  return vkr_vk_flush(renderer, &slot->tlas_instances.allocation, 0u,
                      (VkDeviceSize)count *
                          sizeof(VkAccelerationStructureInstanceKHR));
}

vkr_internal VkAccelerationStructureGeometryKHR
vkr_vk_tlas_geometry(VkDeviceAddress instances) {
  return (VkAccelerationStructureGeometryKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry.instances =
          {
              .sType =
                  VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
              .data = {.deviceAddress = instances},
          },
  };
}

/* Sizes the slot's TLAS for its instances, recreating it when it is too
   small. */
vkr_internal bool8_t vkr_vk_prepare_tlas(VkrVulkanRenderer *renderer,
                                         VkrVulkanFrameSlot *slot) {
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  VkDevice device = vkr_vk_renderer_device(renderer);
  VkAccelerationStructureGeometryKHR geometry =
      vkr_vk_tlas_geometry(slot->tlas_instances.address);
  const VkAccelerationStructureBuildGeometryInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
      .flags = VKR_VULKAN_TLAS_BUILD_FLAGS,
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = 1u,
      .pGeometries = &geometry,
  };
  VkAccelerationStructureBuildSizesInfoKHR sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
  };
  const uint32_t count = Max(slot->tlas_instance_count, 1u);
  /* The same instances over the same BLASes refit; anything else, a new
     structure or the interval's end, rebuilds. */
  slot->tlas_update =
      slot->tlas && slot->tlas_built_count == slot->tlas_instance_count &&
      slot->tlas_instance_count != 0u &&
      slot->tlas_built_generation == renderer->blas_generation &&
      slot->tlas_uses_since_build < VKR_VULKAN_TLAS_REBUILD_INTERVAL;
  functions->get_build_sizes(device,
                             VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                             &info, &count, &sizes);
  if (!slot->tlas || slot->tlas_buffer.size < sizes.accelerationStructureSize) {
    if (slot->tlas) {
      functions->destroy_acceleration_structure(device, slot->tlas, NULL);
    }
    slot->tlas = VK_NULL_HANDLE;
    slot->tlas_update = false_v;
    if (!vkr_vk_ensure_volume_buffer(
            renderer, &slot->tlas_buffer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
            VKR_GPU_ALLOCATION_OWNER_INSTANCE, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) {
      return false_v;
    }
    const VkAccelerationStructureCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = slot->tlas_buffer.handle,
        .size = slot->tlas_buffer.size,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };
    if (functions->create_acceleration_structure(device, &create, NULL,
                                                 &slot->tlas) != VK_SUCCESS) {
      return false_v;
    }
    const VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = slot->tlas,
    };
    slot->tlas_address = functions->get_device_address(device, &address);
    /* The slot's descriptor names its TLAS; the slot's previous frame has
       completed, so no recorded trace reads the descriptor it replaces. */
    const VkDescriptorGetInfoEXT get_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .data.accelerationStructure = slot->tlas_address,
    };
    const VkrVulkanDescriptorLayout *layout =
        vkr_vulkan_device_resource_layout(renderer->device);
    const VkDeviceSize offset =
        layout->acceleration_structure_offset +
        (VkDeviceSize)renderer->active_frame_slot * functions->descriptor_size;
    vkr_vulkan_device_get_descriptor(renderer->device)(
        device, &get_info, functions->descriptor_size,
        (uint8_t *)renderer->resource_descriptors.allocation.mapped + offset);
    if (!vkr_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                           &renderer->resource_descriptors, offset,
                           functions->descriptor_size)) {
      return false_v;
    }
  }
  return vkr_vk_ensure_volume_buffer(
      renderer, &slot->tlas_scratch, VKR_VULKAN_MEMORY_CLASS_DEVICE,
      VKR_GPU_ALLOCATION_OWNER_INSTANCE,
      Max(sizes.buildScratchSize, sizes.updateScratchSize) +
          functions->scratch_alignment,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

/* The BLAS of `geometry`'s `submesh`, if it still exists. */
vkr_internal VkrVulkanBlas *vkr_vk_volume_blas(VkrVulkanRenderer *renderer,
                                               VkrGeometryHandle handle,
                                               uint32_t submesh) {
  VkrVulkanPublishedGeometry *geometry =
      vkr_vk_resolve_geometry(renderer, handle);
  return geometry && geometry->blas && submesh < geometry->submesh_count
             ? &geometry->blas[submesh]
             : NULL;
}

/* Reads the compacted sizes of the BLASes this slot built last, now that
   their frame has completed, and creates each smaller copy's structure;
   the build pass records the copies and the commit swaps them in. */
vkr_internal bool8_t vkr_vk_prepare_blas_compaction(VkrVulkanRenderer *renderer,
                                                    VkrVulkanFrameSlot *slot) {
  vkr_vk_collect_blas_compaction(renderer, slot);
  const uint32_t count = slot->blas_size_count;
  const bool8_t submitted = slot->blas_sizes_submitted;
  slot->blas_size_count = 0u;
  slot->blas_sizes_submitted = false_v;
  if (!count || !submitted || !slot->blas_size_pool) {
    return true_v;
  }
  VkDevice device = vkr_vk_renderer_device(renderer);
  uint64_t sizes[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
  if (vkGetQueryPoolResults(device, slot->blas_size_pool, 0u, count,
                            (size_t)count * sizeof(uint64_t), sizes,
                            sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return true_v;
  }
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  for (uint32_t i = 0u; i < count; ++i) {
    VkrVulkanBlas *blas =
        vkr_vk_volume_blas(renderer, slot->volume_lists->blas_size_geometry[i],
                           slot->volume_lists->blas_size_submesh[i]);
    if (!blas || !blas->built ||
        blas->handle != slot->volume_lists->blas_size_source[i] ||
        sizes[i] == 0u || sizes[i] >= blas->buffer.size) {
      continue;
    }
    /* The copy reads the original in this frame. */
    vkr_vk_resolve_geometry(renderer, slot->volume_lists->blas_size_geometry[i])
        ->last_use_submit_value = renderer->submit_value + 1u;
    VkrVulkanBlasCompaction *compaction =
        &slot->volume_lists->blas_compactions[slot->blas_compaction_count];
    *compaction = (VkrVulkanBlasCompaction){
        .geometry = slot->volume_lists->blas_size_geometry[i],
        .submesh_index = slot->volume_lists->blas_size_submesh[i],
        .source = blas->handle,
    };
    if (!vkr_vk_create_buffer(
            renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
            VKR_GPU_ALLOCATION_OWNER_MESH, sizes[i],
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &compaction->buffer)) {
      return false_v;
    }
    const VkAccelerationStructureCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = compaction->buffer.handle,
        .size = sizes[i],
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };
    if (functions->create_acceleration_structure(
            device, &create, NULL, &compaction->handle) != VK_SUCCESS) {
      vkr_vk_destroy_buffer(renderer, &compaction->buffer);
      return false_v;
    }
    const VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = compaction->handle,
    };
    compaction->address = functions->get_device_address(device, &address);
    slot->blas_compaction_count++;
  }
  return true_v;
}

/* Moves the probe budget toward what VKR_VULKAN_VOLUME_BUDGET_MS affords at
   the cost the slot's last frame measured: the build's time comes first,
   the trace's per-probe time sets the rest. */
vkr_internal void vkr_vk_update_volume_budget(VkrVulkanRenderer *renderer,
                                              VkrVulkanFrameSlot *slot) {
  const bool8_t submitted = slot->volume_timing_submitted;
  const uint32_t probes = slot->volume_timing_probes;
  slot->volume_timing_submitted = false_v;
  if (renderer->volume_probe_budget <= 0.0f) {
    renderer->volume_probe_budget = (float32_t)VKR_VULKAN_VOLUME_PROBES_START;
  }
  if (!submitted || !probes || !slot->volume_timing_pool) {
    return;
  }
  uint64_t stamps[3] = {0u, 0u, 0u};
  if (vkGetQueryPoolResults(vkr_vk_renderer_device(renderer),
                            slot->volume_timing_pool, 0u, 3u, sizeof(stamps),
                            stamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
      stamps[1] < stamps[0] || stamps[2] < stamps[1]) {
    return;
  }
  const float64_t period_ms =
      (float64_t)vkr_vulkan_device_properties(renderer->device)
          ->properties.limits.timestampPeriod /
      1000000.0;
  const float64_t build_ms = (float64_t)(stamps[1] - stamps[0]) * period_ms;
  const float64_t trace_ms = (float64_t)(stamps[2] - stamps[1]) * period_ms;
  if (trace_ms <= 0.0) {
    return;
  }
  const float64_t affordable =
      Max((float64_t)VKR_VULKAN_VOLUME_BUDGET_MS - build_ms, 0.0) /
      (trace_ms / (float64_t)probes);
  const float64_t next = (float64_t)renderer->volume_probe_budget +
                         0.25 * (affordable - renderer->volume_probe_budget);
  renderer->volume_probe_budget =
      (float32_t)Max(Min(next, (float64_t)VKR_DYNAMIC_VOLUME_TRACE_PROBES_MAX),
                     (float64_t)VKR_VULKAN_VOLUME_PROBES_MIN);
}

bool8_t vkr_vk_prepare_volume_build(VkrVulkanRenderer *renderer,
                                    const VkrRgPass *pass) {
  (void)pass;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  vkr_vk_update_volume_budget(renderer, slot);
  slot->blas_job_count = 0u;
  slot->tlas_instance_count = 0u;
  slot->volume_build_ready = false_v;
  slot->volume_timing_recorded = false_v;
  slot->volume_timing_probes = 0u;
  if (!vkr_vk_volume_frame_active(renderer)) {
    return true_v;
  }
  if (!slot->volume_lists) {
    slot->volume_lists =
        vkr_allocator_alloc(renderer->allocator, sizeof(*slot->volume_lists),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!slot->volume_lists) {
      return false_v;
    }
    MemZero(slot->volume_lists, sizeof(*slot->volume_lists));
  }
  if (!slot->volume_timing_pool) {
    const VkQueryPoolCreateInfo timing = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 3u,
    };
    if (vkCreateQueryPool(vkr_vk_renderer_device(renderer), &timing, NULL,
                          &slot->volume_timing_pool) != VK_SUCCESS) {
      return false_v;
    }
  }
  if (!vkr_vk_prepare_blas_compaction(renderer, slot) ||
      !vkr_vk_prepare_tlas_instances(renderer, slot) ||
      !vkr_vk_prepare_tlas(renderer, slot)) {
    return false_v;
  }
  /* The BLASes this frame builds report their compacted sizes. */
  if (slot->blas_job_count && !slot->blas_size_pool) {
    const VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        .queryCount = VKR_VULKAN_BLAS_BUILDS_PER_FRAME,
    };
    if (vkCreateQueryPool(vkr_vk_renderer_device(renderer), &info, NULL,
                          &slot->blas_size_pool) != VK_SUCCESS) {
      return false_v;
    }
  }
  for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
    slot->volume_lists->blas_size_geometry[i] =
        slot->volume_lists->blas_jobs[i].geometry;
    slot->volume_lists->blas_size_submesh[i] =
        slot->volume_lists->blas_jobs[i].submesh_index;
    slot->volume_lists->blas_size_source[i] =
        slot->volume_lists->blas_jobs[i].blas->handle;
  }
  slot->blas_size_count = slot->blas_job_count;
  slot->volume_build_ready = true_v;
  return true_v;
}

vkr_internal void vkr_vk_volume_memory_barrier(VkCommandBuffer command,
                                               VkPipelineStageFlags2 src_stage,
                                               VkAccessFlags2 src_access,
                                               VkPipelineStageFlags2 dst_stage,
                                               VkAccessFlags2 dst_access) {
  const VkMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1u,
      .pMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

vkr_internal void vkr_vk_volume_push(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command, uint64_t root) {
  const VkrVulkanPushConstants push = {.root = root};
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
}

void vkr_vk_record_volume_build(VkrVulkanRenderer *renderer,
                                VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->volume_build_ready) {
    return;
  }
  const VkrVulkanRayQueryFunctions *functions = vkr_vk_ray_query(renderer);
  vkCmdResetQueryPool(command, slot->volume_timing_pool, 0u, 3u);
  vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       slot->volume_timing_pool, 0u);
  /* Earlier frames built BLASes this frame's TLAS names and wrote the
     scratch this one reuses. */
  vkr_vk_volume_memory_barrier(
      command,
      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  /* Compact the BLASes earlier frames built; this frame's TLAS still names
     the originals, which its commit retires. */
  for (uint32_t i = 0u; i < slot->blas_compaction_count; ++i) {
    const VkCopyAccelerationStructureInfoKHR copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = slot->volume_lists->blas_compactions[i].source,
        .dst = slot->volume_lists->blas_compactions[i].handle,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
    };
    functions->cmd_copy(command, &copy);
  }
  if (slot->blas_job_count) {
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        renderer->volume_pipelines[VKR_VULKAN_VOLUME_PIPELINE_BLAS_DECODE]);
    for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
      const VkrVulkanBlasJob *job = &slot->volume_lists->blas_jobs[i];
      vkr_vk_volume_push(renderer, command, job->decode_root);
      vkCmdDispatch(command, (job->index_count + 63u) / 64u, 1u, 1u);
    }
    vkr_vk_volume_memory_barrier(
        command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_SHADER_READ_BIT);
    VkAccelerationStructureGeometryKHR
        geometries[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
    VkAccelerationStructureBuildGeometryInfoKHR
        infos[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
    VkAccelerationStructureBuildRangeInfoKHR
        ranges[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
    const VkAccelerationStructureBuildRangeInfoKHR
        *range_pointers[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
    const VkDeviceAddress scratch = vkr_vk_align_size(
        slot->blas_scratch.address, functions->scratch_alignment);
    for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
      const VkrVulkanBlasJob *job = &slot->volume_lists->blas_jobs[i];
      geometries[i] = vkr_vk_blas_geometry(slot->blas_positions.address +
                                               job->positions_offset,
                                           job->index_count);
      infos[i] = (VkAccelerationStructureBuildGeometryInfoKHR){
          .sType =
              VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = VKR_VULKAN_BLAS_BUILD_FLAGS,
          .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
          .dstAccelerationStructure = job->blas->handle,
          .geometryCount = 1u,
          .pGeometries = &geometries[i],
          .scratchData = {.deviceAddress = scratch + job->scratch_offset},
      };
      ranges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
          .primitiveCount = job->index_count / 3u,
      };
      range_pointers[i] = &ranges[i];
    }
    functions->cmd_build(command, slot->blas_job_count, infos, range_pointers);
    vkr_vk_volume_memory_barrier(
        command, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureKHR built[VKR_VULKAN_BLAS_BUILDS_PER_FRAME];
    for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
      built[i] = slot->volume_lists->blas_jobs[i].blas->handle;
    }
    vkCmdResetQueryPool(command, slot->blas_size_pool, 0u,
                        slot->blas_job_count);
    functions->cmd_write_properties(
        command, slot->blas_job_count, built,
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        slot->blas_size_pool, 0u);
  } else if (slot->blas_compaction_count) {
    vkr_vk_volume_memory_barrier(
        command, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  }
  /* The previous frame's trace read the TLAS this one rebuilds; the slot
     owns its TLAS, so only this frame's build and trace order here. */
  VkAccelerationStructureGeometryKHR geometry =
      vkr_vk_tlas_geometry(slot->tlas_instances.address);
  const VkAccelerationStructureBuildGeometryInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
      .flags = VKR_VULKAN_TLAS_BUILD_FLAGS,
      .mode = slot->tlas_update
                  ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                  : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .srcAccelerationStructure =
          slot->tlas_update ? slot->tlas : VK_NULL_HANDLE,
      .dstAccelerationStructure = slot->tlas,
      .geometryCount = 1u,
      .pGeometries = &geometry,
      .scratchData = {.deviceAddress =
                          vkr_vk_align_size(slot->tlas_scratch.address,
                                            functions->scratch_alignment)},
  };
  const VkAccelerationStructureBuildRangeInfoKHR range = {
      .primitiveCount = slot->tlas_instance_count,
  };
  const VkAccelerationStructureBuildRangeInfoKHR *range_pointer = &range;
  functions->cmd_build(command, 1u, &info, &range_pointer);
  vkr_vk_volume_memory_barrier(
      command, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       slot->volume_timing_pool, 1u);
}

/* -------------------------------------------------------------------------
   Volume.Trace: clear the probes the light left, then trace the selected
   probes into the dynamic texture.
   ------------------------------------------------------------------------- */

vkr_internal VkrVulkanPublishedTexture *
vkr_vk_volume_texture(VkrVulkanRenderer *renderer, VkrTextureHandle handle) {
  VkrVulkanPublishedTexture *texture =
      vkr_vk_published_texture(renderer, handle, NULL);
  return texture && !texture->initialization_pending ? texture : NULL;
}

bool8_t vkr_vk_prepare_volume_trace(VkrVulkanRenderer *renderer,
                                    const VkrRgPass *pass) {
  (void)pass;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->volume_trace_root = 0u;
  slot->volume_trace_count = 0u;
  slot->volume_clear_count = 0u;
  slot->volume_dynamic_clear = false_v;
  slot->volume_grid_clear = false_v;
  slot->volume_grid_probes_image = VK_NULL_HANDLE;
  slot->volume_grid_moments_image = VK_NULL_HANDLE;
  slot->volume_relight_count = 0u;
  slot->probe_relight = 0u;
  if (!slot->volume_build_ready) {
    return true_v;
  }
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrDynamicVolumeInput *input = packet->input.dynamic_volume;
  const VkrFrameLighting *lighting = packet->input.lighting;
  const VkrDiffuseVolumeBinding *volume = &lighting->diffuse_volume;
  VkrVulkanPublishedTexture *dynamic =
      vkr_vk_volume_texture(renderer, volume->dynamic_sh);
  VkrVulkanPublishedTexture *bricks =
      vkr_vk_volume_texture(renderer, volume->bricks);
  if (!dynamic || !dynamic->storage_slot_count || !bricks) {
    return true_v;
  }
  /* The trace resolves the volume itself: a runtime grid joins lookups only
     after its first trace has cleared it. */
  const VkrTextureHandle volume_handles[3] = {volume->indirection,
                                              volume->probes, volume->moments};
  uint32_t volume_textures[3] = {0u, 0u, 0u};
  uint32_t volume_samplers[3] = {0u, 0u, 0u};
  for (uint32_t i = 0u; i < 3u; ++i) {
    if (!volume_handles[i].id ||
        !vkr_vk_resolve_sampled_pair(renderer, volume_handles[i],
                                     &volume_textures[i],
                                     &volume_samplers[i])) {
      return true_v;
    }
  }
  const bool8_t grid = volume->runtime_grid;
  VkrVulkanPublishedTexture *grid_probes =
      grid ? vkr_vk_volume_texture(renderer, volume->probes) : NULL;
  VkrVulkanPublishedTexture *grid_moments =
      grid ? vkr_vk_volume_texture(renderer, volume->moments) : NULL;
  if (grid && (!grid_probes || !grid_probes->storage_slot_count ||
               !grid_moments || !grid_moments->storage_slot_count)) {
    return true_v;
  }

  uint64_t trace_probes = 0u;
  uint64_t clear_probes = 0u;
  uint64_t lights = 0u;
  uint64_t rectangle_lights = 0u;
  uint32_t *trace = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)Max(input->trace_probe_count, 1u) * sizeof(uint32_t), 16u,
      &trace_probes, NULL);
  uint32_t *clear = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)Max(input->clear_probe_count, 1u) * sizeof(uint32_t), 16u,
      &clear_probes, NULL);
  VkrGpuPointLightRow *light_rows = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)Max(input->point_light_count, 1u) * sizeof(*light_rows),
      16u, &lights, NULL);
  VkrGpuRectangleLightRow *rectangle_rows = vkr_vk_frame_upload_allocate(
      slot,
      (uint64_t)Max(input->rectangle_light_count, 1u) * sizeof(*rectangle_rows),
      16u, &rectangle_lights, NULL);
  if (!trace || !clear || !light_rows || !rectangle_rows) {
    return false_v;
  }
  /* Baked reflection probes take the layer's light at their capture
     origins; a grid's probes have no baked light to relight against. */
  const uint32_t relight_count = grid ? 0u : slot->ibl_probe_count;
  if (relight_count &&
      !vkr_vk_ensure_volume_buffer(
          renderer, &slot->volume_probe_relight, VKR_VULKAN_MEMORY_CLASS_DEVICE,
          VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH,
          (VkDeviceSize)relight_count * sizeof(float32_t),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    return false_v;
  }
  MemCopy(trace, input->trace_probes,
          (uint64_t)input->trace_probe_count * sizeof(uint32_t));
  MemCopy(clear, input->clear_probes,
          (uint64_t)input->clear_probe_count * sizeof(uint32_t));
  /* Rows carry each light's sign in its intensity, in physical units. */
  for (uint32_t i = 0u; i < input->point_light_count; ++i) {
    vkr_point_light_pack(&input->point_lights[i], input->point_light_signs[i],
                         &light_rows[i]);
  }
  for (uint32_t i = 0u; i < input->rectangle_light_count; ++i) {
    vkr_rectangle_light_pack(&input->rectangle_lights[i],
                             input->rectangle_light_signs[i],
                             &rectangle_rows[i]);
  }

  const uint32_t rows =
      (volume->probe_count + VKR_DIFFUSE_VOLUME_ROW_PROBES - 1u) /
      VKR_DIFFUSE_VOLUME_ROW_PROBES;
  const uint32_t tile_rows =
      (volume->probe_count + VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES - 1u) /
      VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES;
  if (dynamic->image.width != VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u ||
      dynamic->image.height != rows) {
    return true_v;
  }
  uint64_t root_address = 0u;
  VkrVulkanVolumeTraceRoot *root = vkr_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrVulkanVolumeTraceRoot), &root_address,
      NULL);
  if (!root) {
    return false_v;
  }
  *root = (VkrVulkanVolumeTraceRoot){
      .tlas_slot = renderer->active_frame_slot,
      .candidates = slot->gpu_candidate_buffer->buffer.address,
      .instances = slot->gpu_candidate_instances,
      .geometry_rows = slot->gpu_geometry_rows,
      .materials = renderer->materials.address,
      .trace_probes = trace_probes,
      .clear_probes = clear_probes,
      .lights = lights,
      .rectangle_lights = rectangle_lights,
      .indices = renderer->geometry_megabuffer.indices.address,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .indirection_texture = volume_textures[0],
      .probe_texture = volume_textures[1],
      .moment_texture = volume_textures[2],
      .moment_sampler = volume_samplers[2],
      .bricks_texture = bricks->sampled_slot.index,
      .dynamic_storage = dynamic->storage_slots[0].index,
      .trace_count = input->trace_probe_count,
      .clear_count = input->clear_probe_count,
      .light_count = input->point_light_count,
      .rectangle_light_count = input->rectangle_light_count,
      /* A grid holds every emitter's light; a bake holds the static
         ones'. */
      .static_candidate_count = grid ? 0u : slot->gpu_static_candidate_count,
      .sequence = input->sequence,
      .origin_spacing = {volume->origin.x, volume->origin.y, volume->origin.z,
                         volume->spacing},
      .dimensions = {volume->dimensions[0], volume->dimensions[1],
                     volume->dimensions[2], tile_rows},
      .blend = input->blend,
      .ray_length = VKR_VULKAN_VOLUME_RAY_LENGTH,
      .flags = (grid ? VKR_VULKAN_VOLUME_TRACE_GRID : 0u) |
               (volume->band_count > volume->layer_count
                    ? VKR_VULKAN_VOLUME_TRACE_DIRECT_BANDS
                    : 0u),
      .probe_storage = grid ? grid_probes->storage_slots[0].index : 0u,
      .moment_storage = grid ? grid_moments->storage_slots[0].index : 0u,
      .sky_slot = slot->sh_global_slot,
      .sky_scale = lighting->ibl_intensity * lighting->ibl_diffuse_intensity,
      .moment_clamp = 2.0f * volume->spacing,
      .sky_sh = renderer->sh_coefficients.address,
      .ibl_probes = relight_count ? slot->ibl_probes : 0u,
      .probe_relight = relight_count ? slot->volume_probe_relight.address : 0u,
      .ibl_probe_count = relight_count,
  };
  /* The sun and sky are a bake's; a grid traces them (physical, unlike the
     pre-exposed frame constants). */
  if (grid && lighting->directional_enabled) {
    const Vec3 toward =
        vec3_normalize(vec3_negate(lighting->directional_direction));
    root->sun_direction = (Vec4){toward.x, toward.y, toward.z, 1.0f};
    root->sun_radiance = (Vec4){
        lighting->directional_color.x * lighting->directional_intensity,
        lighting->directional_color.y * lighting->directional_intensity,
        lighting->directional_color.z * lighting->directional_intensity, 0.0f};
  }
  dynamic->last_use_submit_value = renderer->submit_value + 1u;
  bricks->last_use_submit_value = renderer->submit_value + 1u;
  slot->volume_trace_root = root_address;
  slot->volume_relight_count = relight_count;
  slot->probe_relight = root->probe_relight;
  slot->volume_trace_count = input->trace_probe_count;
  slot->volume_clear_count = input->clear_probe_count;
  slot->volume_dynamic_image = dynamic->image.handle;
  slot->volume_dynamic_handle = volume->dynamic_sh;
  /* A new texture's contents are undefined until cleared whole. */
  slot->volume_dynamic_clear =
      renderer->volume_dynamic_cleared.id != volume->dynamic_sh.id ||
      renderer->volume_dynamic_cleared.generation !=
          volume->dynamic_sh.generation;
  if (grid) {
    grid_probes->last_use_submit_value = renderer->submit_value + 1u;
    grid_moments->last_use_submit_value = renderer->submit_value + 1u;
    slot->volume_grid_probes_image = grid_probes->image.handle;
    slot->volume_grid_moments_image = grid_moments->image.handle;
    slot->volume_grid_handle = volume->probes;
    slot->volume_grid_moment_clamp = root->moment_clamp;
    slot->volume_grid_clear =
        renderer->volume_grid_cleared.id != volume->probes.id ||
        renderer->volume_grid_cleared.generation != volume->probes.generation;
  }
  return true_v;
}

vkr_internal void vkr_vk_volume_image_barrier(VkCommandBuffer command,
                                              VkImage image,
                                              VkPipelineStageFlags2 src_stage,
                                              VkAccessFlags2 src_access,
                                              VkPipelineStageFlags2 dst_stage,
                                              VkAccessFlags2 dst_access) {
  const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1u,
              .layerCount = 1u,
          },
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

/* Clears `image` to `value` ahead of the trace's reads and writes. */
vkr_internal void vkr_vk_volume_clear_image(VkCommandBuffer command,
                                            VkImage image,
                                            const VkClearColorValue *value) {
  const VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1u,
      .layerCount = 1u,
  };
  vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_GENERAL, value, 1u,
                       &range);
  vkr_vk_volume_image_barrier(command, image, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void vkr_vk_record_volume_trace(VkrVulkanRenderer *renderer,
                                VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->volume_trace_root) {
    return;
  }
  /* The dynamic texture, and a grid's probe records and moments: earlier
     frames' lighting read what this frame writes. */
  const VkImage images[3] = {slot->volume_dynamic_image,
                             slot->volume_grid_probes_image,
                             slot->volume_grid_moments_image};
  for (uint32_t i = 0u; i < ArrayCount(images); ++i) {
    if (!images[i]) {
      continue;
    }
    vkr_vk_volume_image_barrier(command, images[i],
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                0u,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
  }
  if (slot->volume_dynamic_clear) {
    const VkClearColorValue zero = {0};
    vkr_vk_volume_clear_image(command, slot->volume_dynamic_image, &zero);
  }
  /* A new grid's probes stand unmoved and valid, and see nothing within
     the moment clamp. */
  if (slot->volume_grid_clear) {
    const float32_t clamp = slot->volume_grid_moment_clamp;
    const VkClearColorValue unmoved = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    const VkClearColorValue open = {
        .float32 = {clamp, clamp * clamp, 0.0f, 0.0f}};
    vkr_vk_volume_clear_image(command, slot->volume_grid_probes_image,
                              &unmoved);
    vkr_vk_volume_clear_image(command, slot->volume_grid_moments_image, &open);
  }
  vkr_vk_volume_push(renderer, command, slot->volume_trace_root);
  if (slot->volume_clear_count) {
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        renderer->volume_pipelines[VKR_VULKAN_VOLUME_PIPELINE_CLEAR]);
    vkCmdDispatch(command, (slot->volume_clear_count + 63u) / 64u, 1u, 1u);
    vkr_vk_volume_image_barrier(command, slot->volume_dynamic_image,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  }
  if (slot->volume_trace_count) {
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        renderer->volume_pipelines[VKR_VULKAN_VOLUME_PIPELINE_TRACE]);
    vkCmdDispatch(command, slot->volume_trace_count, 1u, 1u);
    vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         slot->volume_timing_pool, 2u);
    slot->volume_timing_recorded = true_v;
  }
  /* This frame's lookups and later frames read what the trace wrote. */
  for (uint32_t i = 0u; i < ArrayCount(images); ++i) {
    if (!images[i]) {
      continue;
    }
    vkr_vk_volume_image_barrier(command, images[i],
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  }
  /* The probes' relight reads the layer the trace wrote; this frame's
     lighting reads the factors. */
  if (slot->volume_relight_count) {
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        renderer->volume_pipelines[VKR_VULKAN_VOLUME_PIPELINE_PROBE_RELIGHT]);
    vkCmdDispatch(command, (slot->volume_relight_count + 63u) / 64u, 1u, 1u);
    vkr_vk_volume_memory_barrier(command,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  }
}

void vkr_vk_commit_volume_trace(VkrVulkanRenderer *renderer,
                                VkrVulkanFrameSlot *slot) {
  /* The submitted frame builds its BLASes before any later frame reads
     them, in queue order behind the next build's barrier. */
  if (slot->volume_build_ready) {
    for (uint32_t i = 0u; i < slot->blas_job_count; ++i) {
      slot->volume_lists->blas_jobs[i].blas->built = true_v;
    }
    slot->tlas_uses_since_build =
        slot->tlas_update ? slot->tlas_uses_since_build + 1u : 0u;
    slot->tlas_built_count = slot->tlas_instance_count;
    slot->tlas_built_generation = renderer->blas_generation;
    slot->volume_timing_submitted = slot->volume_timing_recorded;
    slot->volume_timing_probes = slot->volume_trace_count;
    slot->blas_sizes_submitted = slot->blas_size_count != 0u;
    /* Later frames trace the compacted copies; frames in flight still name
       the originals, which go when this slot comes round again. */
    for (uint32_t i = 0u; i < slot->blas_compaction_count; ++i) {
      VkrVulkanBlasCompaction *compaction =
          &slot->volume_lists->blas_compactions[i];
      VkrVulkanBlas *blas = vkr_vk_volume_blas(renderer, compaction->geometry,
                                               compaction->submesh_index);
      VkrVulkanRetiredBlas *retired =
          &slot->volume_lists->blas_retired[slot->blas_retired_count++];
      if (blas && blas->handle == compaction->source) {
        renderer->blas_generation++;
        *retired = (VkrVulkanRetiredBlas){.handle = blas->handle,
                                          .buffer = blas->buffer};
        blas->handle = compaction->handle;
        blas->buffer = compaction->buffer;
        blas->address = compaction->address;
      } else {
        *retired = (VkrVulkanRetiredBlas){.handle = compaction->handle,
                                          .buffer = compaction->buffer};
      }
    }
    slot->blas_compaction_count = 0u;
  }
  if (slot->volume_trace_root && slot->volume_dynamic_clear) {
    renderer->volume_dynamic_cleared = slot->volume_dynamic_handle;
  }
  if (slot->volume_trace_root && slot->volume_grid_clear) {
    renderer->volume_grid_cleared = slot->volume_grid_handle;
  }
  slot->volume_dynamic_clear = false_v;
  slot->volume_grid_clear = false_v;
}
