#pragma once

#include "sdk.h"

/* Authored `fps_weapon` script component: a root entity the player carries. */
#define FPS_WEAPON_FIELDS                                                      \
  VKR_FIELD(U32, bone, "Bone", 0u,                                             \
            .tooltip = "Node of the player's animation skeleton that holds "   \
                       "the weapon outside first person",                      \
            .min = 0.0f, .max = 65535.0f)

VKR_COMPONENT_DECLARE(FpsWeaponBinding, fps_weapon, FPS_WEAPON_FIELDS)

/* The FPS sample module: one World-scoped instance plays the active
 * container's `fps_player`, or a player spawned at its Player Start. */
VKR_SDK_EXPORT const VkrModuleDesc *vkr_module_fps(uint32_t sdk_version);
