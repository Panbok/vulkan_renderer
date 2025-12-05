#include "renderer/systems/vkr_resource_system.h"
#include "memory/vkr_allocator.h"

struct VkrResourceSystem {
  VkrAllocator *allocator;
  VkrRendererFrontendHandle renderer;
  VkrJobSystem *job_system;

  // Registered loaders
  VkrResourceLoader *loaders;
  uint32_t loader_count;
  uint32_t loader_capacity;
};

/**
 * @brief Global resource system instance, Not thread-safe
 */
vkr_global VkrResourceSystem *vkr_resource_system = NULL;

bool8_t vkr_resource_system_init(VkrAllocator *allocator,
                                 VkrRendererFrontendHandle renderer,
                                 VkrJobSystem *job_system) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  if (vkr_resource_system) {
    if (vkr_resource_system->renderer != renderer) {
      log_error(
          "Resource system already initialized with different parameters");
      return false_v;
    }

    log_debug("Resource system already initialized with same parameters");
    return true_v;
  }

  vkr_resource_system = vkr_allocator_alloc(
      allocator, sizeof(VkrResourceSystem), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!vkr_resource_system) {
    log_fatal("Failed to allocate resource system");
    return false_v;
  }

  MemZero(vkr_resource_system, sizeof(*vkr_resource_system));

  vkr_resource_system->renderer = renderer;
  vkr_resource_system->allocator = allocator;
  vkr_resource_system->job_system = job_system;

  vkr_resource_system->loader_capacity =
      16; // todo: this needs to be a part of the config file
  vkr_resource_system->loaders = vkr_allocator_alloc(
      vkr_resource_system->allocator,
      sizeof(VkrResourceLoader) * vkr_resource_system->loader_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  assert_log(vkr_resource_system->loaders != NULL,
             "Failed to allocate loaders array");
  vkr_resource_system->loader_count =
      0; // loaders must be registered explicitly by resources

  for (uint32_t loader = 0; loader < vkr_resource_system->loader_capacity;
       loader++) {
    vkr_resource_system->loaders[loader].id = VKR_INVALID_ID;
  }

  return true_v;
}

bool8_t vkr_resource_system_register_loader(void *resource_system,
                                            VkrResourceLoader loader) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(resource_system != NULL, "Loader resource system context is NULL");

  if (vkr_resource_system->loader_count >=
      vkr_resource_system->loader_capacity) {
    uint32_t new_capacity = vkr_resource_system->loader_capacity * 2;
    VkrResourceLoader *new_mem =
        vkr_allocator_alloc(vkr_resource_system->allocator,
                            sizeof(VkrResourceLoader) * new_capacity,
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    assert_log(new_mem != NULL, "Failed to grow loaders array");
    MemCopy(new_mem, vkr_resource_system->loaders,
            sizeof(VkrResourceLoader) * vkr_resource_system->loader_count);
    vkr_resource_system->loaders = new_mem;
    vkr_resource_system->loader_capacity = new_capacity;
  }

  uint32_t id = vkr_resource_system->loader_count++;
  vkr_resource_system->loaders[id] = loader;

  VkrResourceLoader *dst = &vkr_resource_system->loaders[id];
  dst->id = id;
  dst->resource_system = resource_system;
  dst->renderer = vkr_resource_system->renderer;

  return true_v;
}

bool8_t vkr_resource_system_load(VkrResourceType type, String8 path,
                                 VkrAllocator *temp_alloc,
                                 VkrResourceHandleInfo *out_info,
                                 VkrRendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  out_info->type = VKR_RESOURCE_TYPE_UNKNOWN;
  out_info->loader_id = VKR_INVALID_ID;

  for (int32_t pass = 0; pass < 2; pass++) {
    for (uint32_t loader_idx = 0;
         loader_idx < vkr_resource_system->loader_count; loader_idx++) {
      VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];

      // First pass: only try loaders matching the type
      // Second pass: try any loader
      if (pass == 0 && type != VKR_RESOURCE_TYPE_UNKNOWN &&
          loader->type != type)
        continue;

      if (loader->can_load && !loader->can_load(loader, path))
        continue;

      VkrResourceHandleInfo loaded_info = {0};
      if (loader->load &&
          loader->load(loader, path, temp_alloc, &loaded_info, out_error)) {
        loaded_info.loader_id = loader->id;
        *out_info = loaded_info;
        return true_v;
      }
    }
  }

  log_warn("ResourceSystem: no loader could handle '%s'", string8_cstr(&path));

  return false_v;
}

bool8_t vkr_resource_system_load_custom(String8 custom_type, String8 path,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_info,
                                        VkrRendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(custom_type.str != NULL, "Custom type is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  out_info->type = VKR_RESOURCE_TYPE_UNKNOWN;

  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (!loader->custom_type.str)
      continue;
    // simple case-insensitive compare of C strings
    if (string8_equalsi(&loader->custom_type, &custom_type)) {
      if (loader->can_load && !loader->can_load(loader, path))
        continue;
      VkrResourceHandleInfo loaded_info = {0};
      if (loader->load &&
          loader->load(loader, path, temp_alloc, &loaded_info, out_error)) {
        loaded_info.loader_id = loader->id;
        *out_info = loaded_info;
        return true_v;
      }
    }
  }

  return false_v;
}

void vkr_resource_system_unload(const VkrResourceHandleInfo *info,
                                String8 name) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(info != NULL, "Info is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  // Prefer unloading with the loader that created the resource if available
  if (info->loader_id != VKR_INVALID_ID &&
      info->loader_id < vkr_resource_system->loader_count) {
    VkrResourceLoader *by_id = &vkr_resource_system->loaders[info->loader_id];
    if (by_id->unload && by_id->type == info->type) {
      by_id->unload(by_id, info, name);
      return;
    }
  }

  // Fallback: unload by matching type
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == info->type && loader->unload) {
      loader->unload(loader, info, name);
      return;
    }
  }

  log_warn("Resource system: no unloader for type=%u name='%s'",
           (unsigned)info->type, string8_cstr(&name));
}

uint32_t vkr_resource_system_get_loader_id(VkrResourceType type, String8 name) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == type) {
      return loader->id;
    }
  }

  return VKR_INVALID_ID;
}

VkrJobSystem *vkr_resource_system_get_job_system(void) {
  if (!vkr_resource_system) {
    return NULL;
  }
  return vkr_resource_system->job_system;
}

uint32_t vkr_resource_system_load_batch(VkrResourceType type,
                                        const String8 *paths, uint32_t count,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_handles,
                                        VkrRendererError *out_errors) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  VkrResourceLoader *batch_loader = NULL;
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == type && loader->batch_load) {
      batch_loader = loader;
      break;
    }
  }

  if (batch_loader) {
    uint32_t loaded = batch_loader->batch_load(
        batch_loader, paths, count, temp_alloc, out_handles, out_errors);
    for (uint32_t i = 0; i < count; i++) {
      if (out_handles[i].type != VKR_RESOURCE_TYPE_UNKNOWN) {
        out_handles[i].loader_id = batch_loader->id;
      }
    }
    return loaded;
  }

  // Fallback: sequential loading using single-item load
  uint32_t loaded_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!paths[i].str || paths[i].length == 0) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }

    if (vkr_resource_system_load(type, paths[i], temp_alloc, &out_handles[i],
                                 &out_errors[i])) {
      loaded_count++;
    }
  }

  return loaded_count;
}
