#pragma once

#include "script/vkr_script.h"

/* Authored `fps_weapon` script component: a root entity the player carries. */
typedef struct FpsWeaponBinding {
  uint32_t bone; // Node of the player's animation skeleton holding the weapon.
} FpsWeaponBinding;

/* Script components users attach to entities; FpsPlayerSettings values. */
extern const VkrTypeDesc fps_player_type;
extern const VkrTypeDesc fps_weapon_type;

VKR_SCRIPT_EXPORT const VkrScriptModuleDesc *
vkr_script_module_fps(const VkrScriptApi *api);
