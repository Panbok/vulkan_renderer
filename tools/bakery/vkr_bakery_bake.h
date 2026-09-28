#pragma once

#include "vkr_bakery_internal.h"

/* `vkr_bakery bake diffuse|probe`: scene bakes that need the whole scene and a
 * renderer, so they run as explicit commands rather than cached producers. */

/** Exit status of `bake diffuse` when no interpolation cell lies inside a
 * closed room; nothing is published (ADR-054). */
#define VKR_BAKERY_BAKE_NO_ROOM_CELLS 3

/** `argv[0]` is "diffuse" or "probe"; the remaining entries are its options. */
int vkr_bakery_bake_main(const VkrBakeryConfig *config, int argc, char **argv);

/** `argv[0]` is "material" (render one managed material's thumbnail in an
 * isolated harness) or "prune" (evict old thumbnails from a workspace cache).
 */
int vkr_bakery_preview_main(const VkrBakeryConfig *config, int argc,
                            char **argv);
