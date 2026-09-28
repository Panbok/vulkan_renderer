#include "vkr_bakery_internal.h"

#if VKR_BAKERY_WITH_COOKERS
#include "vkr_vkt_packer.h"
#endif

#include <string.h>

/* Producer registry. Asset producers wrap cooker libraries and exist only in
 * builds with VKR_BUILD_TOOLS; shader producers are always present because the
 * renderer build compiles its shaders through vkr_bakery. */

// =============================================================================
// Registry
// =============================================================================

vkr_internal const VkrBakeryProducer *const vkr_bakery_producers[] = {
#if VKR_BAKERY_WITH_COOKERS
    &vkr_bakery_producer_texture,
    &vkr_bakery_producer_mesh,
    &vkr_bakery_producer_animation,
    &vkr_bakery_producer_collision,
    &vkr_bakery_producer_font,
    &vkr_bakery_producer_table,
#endif
    &vkr_bakery_producer_shader_spirv,
    &vkr_bakery_producer_shader_msl,
    &vkr_bakery_producer_shader_concat,
    &vkr_bakery_producer_shader_metallib,
    &vkr_bakery_producer_shader_manifest,
    &vkr_bakery_producer_script_object,
    &vkr_bakery_producer_script_library,
};

const VkrBakeryProducer *vkr_bakery_producer_find(const char *id) {
  for (uint32_t i = 0u; i < ArrayCount(vkr_bakery_producers); ++i) {
    if (strcmp(vkr_bakery_producers[i]->id, id) == 0) {
      return vkr_bakery_producers[i];
    }
  }
  return NULL;
}

uint32_t vkr_bakery_producer_count(void) {
  return ArrayCount(vkr_bakery_producers);
}

const VkrBakeryProducer *vkr_bakery_producer_at(uint32_t index) {
  return index < ArrayCount(vkr_bakery_producers) ? vkr_bakery_producers[index]
                                                  : NULL;
}

const VkrBakeryProducer *vkr_bakery_producer_for_source(const char *path) {
#if VKR_BAKERY_WITH_COOKERS
  char extension[16];
  vkr_bakery_path_extension(path, extension, sizeof(extension));
  if (vkr_vkt_is_supported_source(path)) {
    return &vkr_bakery_producer_texture;
  }
  if (strcmp(extension, ".gltf") == 0 || strcmp(extension, ".glb") == 0 ||
      strcmp(extension, ".obj") == 0) {
    return &vkr_bakery_producer_mesh;
  }
  if (strcmp(extension, ".fontcfg") == 0) {
    return &vkr_bakery_producer_font;
  }
#else
  (void)path;
#endif
  return NULL;
}
