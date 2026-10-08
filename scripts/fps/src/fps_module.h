#pragma once

#include "sdk.h"

/* Authored `fps_weapon` script component: a root entity the player carries. */
#define FPS_WEAPON_FIELDS                                                      \
  VKR_FIELD(U32, bone, "Bone", 0u,                                             \
            .tooltip = "Node of the player's animation skeleton that holds "   \
                       "the weapon outside first person",                      \
            .min = 0.0f, .max = 65535.0f)

VKR_COMPONENT_DECLARE(FpsWeaponBinding, fps_weapon, FPS_WEAPON_FIELDS)

/* Authored `fps_ladder`: on a trigger brush in front of a climbable wall,
   reaching above the floor it leads to; the player climbs it while it faces
   it (fps_player.c). */
#define FPS_LADDER_FIELDS                                                      \
  VKR_FIELD(F32, climb_speed, "Climb speed", 2.5f, .unit = "m/s", .min = 0.1f, \
            .max = 20.0f)

VKR_COMPONENT_DECLARE(FpsLadder, fps_ladder, FPS_LADDER_FIELDS)

/* `door`: a sliding door that entity IO opens and closes (fps_door.c). */
#define FPS_DOOR_FIELDS                                                        \
  VKR_FIELD(VEC3, offset, "Open offset", vec3_new(0.0f, 2.5f, 0.0f),           \
            .unit = "m",                                                       \
            .tooltip = "How far the open door stands from the "                \
                       "closed one, in its parent's space")                    \
  VKR_FIELD(F32, speed, "Speed", 1.5f, .unit = "m/s", .min = 0.01f,            \
            .max = 100.0f)                                                     \
  VKR_FIELD(BOOL, locked, "Locked", false_v,                                   \
            .tooltip = "A locked door ignores Open")

VKR_COMPONENT_DECLARE(FpsDoor, door, FPS_DOOR_FIELDS)
const VkrBehaviorDesc *fps_door_behavior(void);

/* The FPS sample module: one World-scoped instance plays the active
 * container's `fps_player`, or a player spawned at its Player Start. */
VKR_SDK_EXPORT const VkrModuleDesc *vkr_module_fps(uint32_t sdk_version);
