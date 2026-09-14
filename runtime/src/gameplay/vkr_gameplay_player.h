#pragma once

#include "core/input.h"
#include "gameplay/vkr_camera_rig.h"
#include "gameplay/vkr_gameplay_input.h"
#include "gameplay/vkr_player_animation.h"
#include "gameplay/vkr_weapon.h"
#include "renderer/systems/vkr_scene_physics.h"

/* Example C composition: one existing entity owns motor, weapon and intent
 * data; this client owns input admission and scene callback bindings. Keep it
 * at a stable address, shut down before its borrowed input/scene are destroyed.
 * It deliberately owns neither a renderer nor an Actor registry. */
typedef struct VkrPlayerState {
  VkrWeaponState weapon;
  VkrWeaponReloadToken reload;
  uint32_t reserve_rounds;
  uint32_t held;
  float32_t yaw;
  float32_t pitch;
  Vec3 velocity;
  bool8_t grounded;
  bool8_t crouched;
} VkrPlayerState;

typedef struct VkrGameplayPlayer {
  VkrScene *scene;
  InputState *input;
  VkrEntityId entity;
  VkrComponentTypeId component;
  VkrGameplayInput commands;
  VkrCameraRig camera;
  VkrPlayerAnimation animation;
  Mat4 weapon_reference_inverse;
  bool8_t weapon_reference_valid;
  char error[192];
  VkrWeaponShot pending_shot;
  VkrPhysicsRayHit pending_hit;
  uint64_t shots_fired;
  uint64_t hits;
  uint64_t instance_id;
  float64_t epoch;
  float64_t last_frame_time;
  float32_t spawn_yaw;
  float32_t render_yaw;
  float32_t render_pitch;
  Vec3 previous_foot;
  Vec3 current_foot;
  bool8_t active;
  bool8_t shot_pending;
  bool8_t hit_pending;
} VkrGameplayPlayer;

/* Zero-initialize player before first attachment; reattachment requires
 * shutdown. Attach to a root entity at unit scale while scene is paused/reset.
 * Existing callbacks must be absent. Initializes an example automatic weapon
 * (12 rounds, 10 shots/s, reload matched to the available clip), movement (5
 * m/s) and first-person camera. */
bool8_t vkr_gameplay_player_attach(VkrGameplayPlayer *player, VkrScene *scene,
                                   InputState *input, VkrEntityId entity,
                                   uint64_t instance_id, float32_t yaw,
                                   const char **error);
void vkr_gameplay_player_shutdown(VkrGameplayPlayer *player);
/* Call once before scene_update, after the input pump. Returns admitted elapsed
 * time, independently of a display-delta clamp. Inactive input cancels held
 * actions and reload, discards pending input, and requires fresh presses. */
float64_t vkr_gameplay_player_frame(VkrGameplayPlayer *player, float64_t now,
                                    bool8_t active);
bool8_t vkr_gameplay_player_camera(VkrGameplayPlayer *player,
                                   VkrCameraRigPose *pose);
