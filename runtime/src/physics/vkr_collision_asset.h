#pragma once
#include "assets/vkr_collision_cooked.h"

typedef struct s_VkrCollisionAsset VkrCollisionAsset;

/* Cold, synchronous load. Relative paths use the caller's asset root/current
 * directory; pass resolved absolute paths for managed assets. No GPU resources.
 * Immutable geometry is borrowed until the final close. Calls are serialized.
 */
VkrCollisionAsset *vkr_collision_asset_open(String8 path, const char **error);
bool8_t vkr_collision_asset_retain(VkrCollisionAsset *asset);
void vkr_collision_asset_close(VkrCollisionAsset *asset);
const VkrCollisionGeometry *
vkr_collision_asset_geometry(const VkrCollisionAsset *asset);
