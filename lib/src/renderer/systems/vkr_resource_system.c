#include "renderer/systems/vkr_resource_system.h"

vkr_global VkrResourceSystem *vkr_resource_system = NULL;

bool8_t vkr_resource_system_init(Arena *arena,
                                 RendererFrontendHandle renderer) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  if (vkr_resource_system) {
    log_warn("Resource system already initialized");
    return true_v;
  }

  vkr_resource_system =
      arena_alloc(arena, sizeof(VkrResourceSystem), ARENA_MEMORY_TAG_RENDERER);
  if (!vkr_resource_system) {
    log_fatal("Failed to allocate resource system");
    return false_v;
  }

  MemZero(vkr_resource_system, sizeof(*vkr_resource_system));

  vkr_resource_system->arena = arena;
  vkr_resource_system->renderer = renderer;

  vkr_resource_system->loader_capacity =
      16; // todo: this needs to be a part of the config file
  vkr_resource_system->loaders = arena_alloc(
      vkr_resource_system->arena,
      sizeof(VkrResourceLoader) * vkr_resource_system->loader_capacity,
      ARENA_MEMORY_TAG_RENDERER);
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
  assert_log(resource_system != NULL, "Resource system is NULL");

  if (vkr_resource_system->loader_count >=
      vkr_resource_system->loader_capacity) {
    uint32_t new_capacity = vkr_resource_system->loader_capacity * 2;
    VkrResourceLoader *new_mem = arena_alloc(
        vkr_resource_system->arena, sizeof(VkrResourceLoader) * new_capacity,
        ARENA_MEMORY_TAG_RENDERER);
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
                                 Arena *temp_arena,
                                 VkrResourceHandleInfo *out_info,
                                 RendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  out_info->type = VKR_RESOURCE_TYPE_UNKNOWN;
  out_info->loader_id = VKR_INVALID_ID;

  // Try loaders matching the provided type first
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (type != VKR_RESOURCE_TYPE_UNKNOWN && loader->type != type)
      continue;
    if (loader->can_load && !loader->can_load(loader, path))
      continue;

    VkrResourceHandleInfo loaded_info = {0};
    if (loader->load &&
        loader->load(loader, path, temp_arena, &loaded_info, out_error)) {
      loaded_info.loader_id = loader->id;
      *out_info = loaded_info;
      return true_v;
    }
  }

  // Fallback: try any loader that can load
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->can_load && !loader->can_load(loader, path))
      continue;
    VkrResourceHandleInfo loaded_info = {0};
    if (loader->load &&
        loader->load(loader, path, temp_arena, &loaded_info, out_error)) {
      loaded_info.loader_id = loader->id;
      *out_info = loaded_info;
      return true_v;
    }
  }

  log_warn("ResourceSystem: no loader could handle '%s'", string8_cstr(&path));

  return false_v;
}

bool8_t vkr_resource_system_load_custom(String8 custom_type, String8 path,
                                        Arena *temp_arena,
                                        VkrResourceHandleInfo *out_info,
                                        RendererError *out_error) {
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
          loader->load(loader, path, temp_arena, &loaded_info, out_error)) {
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
    if (loader->type == type && string8_equalsi(&loader->type_path, &name)) {
      return loader->id;
    }
  }

  return VKR_INVALID_ID;
}