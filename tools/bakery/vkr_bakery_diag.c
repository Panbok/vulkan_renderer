#include "vkr_bakery_diag.h"

#include <string.h>

#define VKR_BAKERY_E VKR_BAKERY_SEVERITY_ERROR
#define VKR_BAKERY_W VKR_BAKERY_SEVERITY_WARNING

/* Indexed by VkrBakeryDiag; order must match the enum. */
vkr_internal const VkrBakeryDiagInfo vkr_bakery_diags[VKR_BAKERY_DIAG_COUNT] = {
    {"VKR-CLI-0001", VKR_BAKERY_E,
     "Invalid command-line usage: an unknown option, a missing value or a "
     "missing required argument."},
    {"VKR-CLI-0002", VKR_BAKERY_E, "Unknown subcommand."},
    {"VKR-REC-0001", VKR_BAKERY_E,
     "A recipe contains a field its producer does not accept."},
    {"VKR-REC-0002", VKR_BAKERY_E,
     "A recipe field has a value of the wrong type or outside its range."},
    {"VKR-REC-0003", VKR_BAKERY_E,
     "A recipe, manifest or request file cannot be read or is not valid "
     "JSON."},
    {"VKR-REC-0004", VKR_BAKERY_E,
     "No producer handles this source; set \"producer\" in its recipe."},
    {"VKR-IDX-0001", VKR_BAKERY_E,
     "A source or a file it references does not exist."},
    {"VKR-IDX-0002", VKR_BAKERY_E, "A source file cannot be read or hashed."},
    {"VKR-CACHE-0001", VKR_BAKERY_E,
     "The cache directory cannot be created or written; set --cache or "
     "VKR_BAKERY_CACHE."},
    {"VKR-CACHE-0002", VKR_BAKERY_E,
     "A product could not be moved into the content-addressed store."},
    {"VKR-CACHE-0003", VKR_BAKERY_E,
     "A product could not be published to its destination."},
    {"VKR-CACHE-0004", VKR_BAKERY_W,
     "A cached product failed verification and was rebuilt."},
    {"VKR-SCHED-0001", VKR_BAKERY_E,
     "An isolated action exited abnormally; the tail of its stderr is "
     "attached."},
    {"VKR-SCHED-0002", VKR_BAKERY_E, "An action exceeded its time limit."},
    {"VKR-SCHED-0003", VKR_BAKERY_E, "An action process could not start."},
    {"VKR-SCHED-0004", VKR_BAKERY_E,
     "An action finished without writing a declared product."},
    {"VKR-TEX-0001", VKR_BAKERY_E, "Texture encoding failed."},
    {"VKR-MESH-0001", VKR_BAKERY_E, "Mesh cooking failed."},
    {"VKR-MESH-0002", VKR_BAKERY_E,
     "A model references a buffer or image that does not exist."},
    {"VKR-FONT-0001", VKR_BAKERY_E, "Font cooking failed."},
    {"VKR-ANIM-0001", VKR_BAKERY_E, "Animation cooking failed."},
    {"VKR-COLL-0001", VKR_BAKERY_E, "Collision cooking failed."},
    {"VKR-TABLE-0001", VKR_BAKERY_E, "Renderer table generation failed."},
    {"VKR-TABLE-0002", VKR_BAKERY_W,
     "A renderer table changed; rebuild the renderer to use it."},
    {"VKR-BAKE-0001", VKR_BAKERY_E, "Diffuse volume baking failed."},
    {"VKR-BAKE-0002", VKR_BAKERY_W,
     "The scene has no closed-room cell, so no diffuse volume is "
     "published."},
    {"VKR-PROBE-0001", VKR_BAKERY_E, "Reflection probe capture failed."},
    {"VKR-SHD-0001", VKR_BAKERY_W,
     "The Metal offline toolchain is unavailable; the runtime compiles Metal "
     "source instead of loading a metallib. Install it with `xcodebuild "
     "-downloadComponent MetalToolchain`."},
    {"VKR-SHD-0100", VKR_BAKERY_E, "Shader compilation error."},
    {"VKR-SHD-0101", VKR_BAKERY_W, "Shader compilation warning."},
    {"VKR-SHD-0102", VKR_BAKERY_E,
     "A requested shader entry is not in the library recipe."},
    {"VKR-SCRIPT-0100", VKR_BAKERY_E, "Script compilation error."},
    {"VKR-SCRIPT-0101", VKR_BAKERY_W, "Script compilation warning."},
    {"VKR-SCRIPT-0102", VKR_BAKERY_E, "No C compiler was found for scripts."},
    {"VKR-PROJ-0001", VKR_BAKERY_E, "A project operation failed."},
    {"VKR-PROJ-0002", VKR_BAKERY_E, "A project request is invalid."},
    {"VKR-BUNDLE-0001", VKR_BAKERY_E, "Bundle creation failed."},
    {"VKR-BUNDLE-0002", VKR_BAKERY_E,
     "The platform runtime executable for the bundle was not found."},
    {"VKR-SERVE-0001", VKR_BAKERY_E, "The build daemon failed."},
};

const VkrBakeryDiagInfo *vkr_bakery_diag_info(VkrBakeryDiag diag) {
  return (uint32_t)diag < VKR_BAKERY_DIAG_COUNT ? &vkr_bakery_diags[diag]
                                                : NULL;
}

const VkrBakeryDiagInfo *vkr_bakery_diag_find(const char *code) {
  for (uint32_t i = 0u; i < VKR_BAKERY_DIAG_COUNT; ++i) {
    if (strcmp(vkr_bakery_diags[i].code, code) == 0) {
      return &vkr_bakery_diags[i];
    }
  }
  return NULL;
}

uint32_t vkr_bakery_diag_count(void) { return VKR_BAKERY_DIAG_COUNT; }

const VkrBakeryDiagInfo *vkr_bakery_diag_at(uint32_t index) {
  return vkr_bakery_diag_info((VkrBakeryDiag)index);
}
