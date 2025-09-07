#include "renderer/systems/vkr_resource_system.h"

bool8_t vkr_resource_system_init(Arena *arena, RendererFrontendHandle renderer,
                                 VkrResourceSystem *out_system) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_system != NULL, "Out system is NULL");

  MemZero(out_system, sizeof(*out_system));
  out_system->arena = arena;
  out_system->renderer = renderer;

  out_system->loader_capacity = 16;
  out_system->loaders =
      arena_alloc(out_system->arena,
                  sizeof(VkrResourceLoader) * out_system->loader_capacity,
                  ARENA_MEMORY_TAG_RENDERER);
  assert_log(out_system->loaders != NULL, "Failed to allocate loaders array");
  out_system->loader_count =
      0; // loaders must be registered explicitly by resources

  return true_v;
}

void vkr_resource_system_shutdown(VkrResourceSystem *system) {
  if (!system)
    return;

  MemZero(system, sizeof(*system));
}

bool8_t vkr_resource_system_register_loader(VkrResourceSystem *system,
                                            void *resource_system,
                                            VkrResourceLoader loader) {
  assert_log(system != NULL, "System is NULL");
  assert_log(resource_system != NULL, "Resource system is NULL");

  if (system->loader_count >= system->loader_capacity) {
    uint32_t new_capacity = system->loader_capacity * 2;
    VkrResourceLoader *new_mem =
        arena_alloc(system->arena, sizeof(VkrResourceLoader) * new_capacity,
                    ARENA_MEMORY_TAG_RENDERER);
    assert_log(new_mem != NULL, "Failed to grow loaders array");
    MemCopy(new_mem, system->loaders,
            sizeof(VkrResourceLoader) * system->loader_count);
    system->loaders = new_mem;
    system->loader_capacity = new_capacity;
  }

  loader.id = system->loader_count;
  system->loaders[system->loader_count++] = loader;

  loader.resource_system = resource_system;
  loader.system = system;
  loader.renderer = system->renderer;

  return true_v;
}

bool8_t vkr_resource_system_load(VkrResourceSystem *system,
                                 VkrResourceType type, String8 path,
                                 Arena *temp_arena,
                                 VkrResourceHandleInfo *out_info,
                                 RendererError *out_error) {
  assert_log(system != NULL, "Resource system is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  out_info->type = VKR_RESOURCE_TYPE_UNKNOWN;

  // Try loaders matching the provided type first
  for (uint32_t i = 0; i < system->loader_count; i++) {
    VkrResourceLoader *loader = &system->loaders[i];
    if (type != VKR_RESOURCE_TYPE_UNKNOWN && loader->type != type)
      continue;
    if (loader->can_load && !loader->can_load(loader, path))
      continue;

    VkrResourceHandleInfo loaded_info = {0};
    if (loader->load &&
        loader->load(loader, path, temp_arena, &loaded_info, out_error)) {
      *out_info = loaded_info;
      return true_v;
    }
  }

  // Fallback: try any loader that can load
  for (uint32_t i = 0; i < system->loader_count; i++) {
    VkrResourceLoader *loader = &system->loaders[i];
    if (loader->can_load && !loader->can_load(loader, path))
      continue;
    VkrResourceHandleInfo loaded_info = {0};
    if (loader->load &&
        loader->load(loader, path, temp_arena, &loaded_info, out_error)) {
      *out_info = loaded_info;
      return true_v;
    }
  }

  log_warn("ResourceSystem: no loader could handle '%s'", string8_cstr(&path));

  return false_v;
}

bool8_t vkr_resource_system_load_custom(VkrResourceSystem *system,
                                        const char *custom_type, String8 path,
                                        Arena *temp_arena,
                                        VkrResourceHandleInfo *out_info,
                                        RendererError *out_error) {
  assert_log(custom_type != NULL, "Custom type is NULL");
  for (uint32_t i = 0; i < system->loader_count; i++) {
    VkrResourceLoader *loader = &system->loaders[i];
    if (!loader->custom_type)
      continue;
    // simple case-insensitive compare of C strings
    if (string_equalsi(loader->custom_type, custom_type)) {
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

void vkr_resource_system_unload(VkrResourceSystem *system, VkrResourceType type,
                                const VkrResourceHandleInfo *info,
                                String8 name) {
  assert_log(system != NULL, "Resource system is NULL");
  assert_log(info != NULL, "Info is NULL");
  (void)type; // type can be implied from info->type
  for (uint32_t i = 0; i < system->loader_count; i++) {
    VkrResourceLoader *loader = &system->loaders[i];
    if (loader->type == info->type && loader->unload) {
      loader->unload(loader, info, name);
      return;
    }
  }
}
