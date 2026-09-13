#pragma once
#include "assets/vkr_collision_cooked.h"

/* Imports static reference geometry from one glTF subtree (node index), or
 * the default scene (UINT32_MAX), baking node transforms into the asset.
 * Selected subtree coordinates are relative to the selected node. Skinned
 * primitives are rejected: use bone collider authoring for animated bodies. */
bool8_t vkr_collision_import_gltf(VkrAllocator *result, VkrAllocator *scratch,
                                  String8 path, uint32_t node,
                                  VkrCollisionKind kind,
                                  VkrCollisionGeometry *geometry,
                                  const char **error);
