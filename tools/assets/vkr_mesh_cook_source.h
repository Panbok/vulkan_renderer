#pragma once

#include "assets/vkr_mesh_encode.h"
#include "assets/vkr_scene_light_bake.h"

bool8_t vkr_mesh_cook_source(String8 source_path, String8 output_path,
                             VkrAllocator *source_allocator,
                             VkrAllocator *scratch_allocator,
                             VkrMeshCookStats *out_stats,
                             VkrRendererError *out_error);

/* Resolves source glTF punctual-light definition ranges into the cooked node
 * metadata before the artifact is encoded. */
bool8_t vkr_mesh_cook_source_with_light_ranges(
    String8 source_path, String8 output_path,
    const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrAllocator *source_allocator,
    VkrAllocator *scratch_allocator, VkrMeshCookStats *out_stats,
    VkrRendererError *out_error);

/* Managed outputs live below an absolute bundle root. output_path must be a
 * direct child of that root. import_id contains only ASCII letters, digits,
 * '-' and '_'. All published dependency references are file-relative. The
 * caller owns the unpublished bundle and removes it on failure/cancellation.
 * An optional absolute generated_root receives content-addressed derived
 * textures shared across imports; the bundle holds clones or copies of the
 * variants it references. Empty keeps them below the bundle. */
bool8_t vkr_mesh_cook_source_managed(
    String8 source_path, String8 output_path, String8 bundle_root,
    String8 import_id, String8 generated_root,
    const VkrSceneLightRangeOverride *range_overrides,
    uint32_t range_override_count, VkrAllocator *source_allocator,
    VkrAllocator *scratch_allocator, VkrMeshCookStats *out_stats,
    VkrRendererError *out_error);

/* Names a file that receives every recorded source dependency, one path per
 * line, after each later successful cook in this process: authored inputs
 * and the generated materials and derived textures the cook wrote. vkr_bakery
 * sets it once per tool process; NULL disables it. The string is borrowed
 * until the process exits. */
void vkr_mesh_cook_set_dependency_list(const char *path);

/* Every later glTF cook in this process keeps material factors and names no
 * textures (the "deferred" texture tier); a later cook adds them. */
void vkr_mesh_cook_set_defer_textures(bool8_t defer);

/* Charts between lightmap UV islands, in texels, and the largest accepted
 * per-mesh lightmap atlas edge. */
#define VKR_MESH_COOK_LIGHTMAP_PADDING 2u
#define VKR_MESH_COOK_LIGHTMAP_MAX_SIZE 4096u

/* Every later static mesh cook in this process unwraps a lightmap UV set at
 * this density, in texels per mesh unit (ADR-087); zero disables it. */
void vkr_mesh_cook_set_lightmap_uv(float32_t texels_per_unit);

/** Later glTF cooks append a JSON line to `path` for each material whose
 * textures are final once its file is written; NULL stops. Borrowed. */
void vkr_mesh_cook_set_ready_log(const char *path);

/** Later glTF cooks write these materials' files first, in this order.
 * Borrowed; NULL restores material order. */
void vkr_mesh_cook_set_material_priority(const String8 *names, uint32_t count);

/** Later managed cooks may hard link bundle dependencies whose absolute
 * source lies below `root` where the volume cannot clone them. The caller
 * guarantees that files there are never rewritten in place (see file_link).
 * Borrowed until the process exits; NULL always copies. */
void vkr_mesh_cook_set_link_root(const char *root);

/** Later managed cooks write `path` with one line per bundle file whose
 * SHA-256 they hold: the lowercase hex digest, the byte size and the path
 * relative to the bundle root, separated by spaces. Borrowed until the
 * process exits; NULL writes nothing. */
void vkr_mesh_cook_set_digest_log(const char *path);
