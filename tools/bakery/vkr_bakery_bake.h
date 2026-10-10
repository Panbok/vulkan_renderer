#pragma once

#include "vkr_bakery_internal.h"

/* `vkr_bakery bake diffuse|probe|lightmap`: scene bakes that need the whole
 * scene and a renderer or GPU, so they run as explicit commands rather than
 * cached producers. */

/** Exit status of `bake diffuse` when the scene has no geometry to place
 * probe bricks near; nothing is published (ADR-054). */
#define VKR_BAKERY_BAKE_NO_VOLUME 3

/** Exit status of `bake lightmap` when there is nothing to bake on this host:
 * no scene model carries lightmap UVs (ADR-087), or the host's desktop
 * pipeline bakes only lamp light and the scene has no static lamps
 * (ADR-104); nothing is published. */
#define VKR_BAKERY_BAKE_NO_LIGHTMAPPED_INSTANCES 4

/** Exit status of `bake diffuse` when the scene's bounds need more
 * indirection entries or bricks than a volume holds at the requested
 * spacing; nothing is published (ADR-054). */
#define VKR_BAKERY_BAKE_VOLUME_TOO_LARGE 5

/** `argv[0]` is "diffuse", "probe", "lightmap" or "proxies"; the remaining
 * entries are its options. */
int vkr_bakery_bake_main(const VkrBakeryConfig *config, int argc, char **argv);

/** `argv[0]` is "material" (render one managed material's thumbnail in an
 * isolated harness) or "prune" (evict old thumbnails from a workspace cache).
 */
int vkr_bakery_preview_main(const VkrBakeryConfig *config, int argc,
                            char **argv);
