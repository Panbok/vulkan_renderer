#pragma once

#include "vkr_bakery_internal.h"

/* `vkr_bakery bake diffuse|probe|lightmap`: scene bakes that need the whole
 * scene and a renderer or GPU, so they run as explicit commands rather than
 * cached producers. */

/** Exit status of `bake diffuse` when no interpolation cell lies inside a
 * closed room; nothing is published (ADR-054). */
#define VKR_BAKERY_BAKE_NO_ROOM_CELLS 3

/** Exit status of `bake lightmap` when no scene model carries lightmap UVs;
 * nothing is published (ADR-087). */
#define VKR_BAKERY_BAKE_NO_LIGHTMAPPED_INSTANCES 4

/** `argv[0]` is "diffuse", "probe", "lightmap" or "proxies"; the remaining
 * entries are its options. */
int vkr_bakery_bake_main(const VkrBakeryConfig *config, int argc, char **argv);

/** `argv[0]` is "material" (render one managed material's thumbnail in an
 * isolated harness) or "prune" (evict old thumbnails from a workspace cache).
 */
int vkr_bakery_preview_main(const VkrBakeryConfig *config, int argc,
                            char **argv);
