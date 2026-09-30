#pragma once

#include "fps_camera_rig.h"
#include "fps_input.h"
#include "fps_locomotion.h"
#include "fps_player_animation.h"
#include "fps_weapon.h"
#include "script/vkr_script.h"

/* Authored behavior values of the `fps_player` script component. */
typedef struct FpsPlayerSettings {
  float32_t move_speed;   // Metres/second standing.
  float32_t crouch_speed; // Metres/second crouched.
  float32_t jump_speed;   // Takeoff vertical speed, metres/second.
  uint32_t magazine;      // Rounds per magazine.
  uint32_t reserve;       // Reserve rounds at spawn and reset.
  uint32_t camera_mode;   // FpsCameraRigMode applied at attach.
  float32_t walk_speed;   // Metres/second while Shift is held.
  /* Third person with orient_to_movement: the body turns towards its
   * movement at turn_rate (radians/second) and speed changes at
   * acceleration (metres/second squared; zero is immediate). Other views
   * strafe with the camera and change speed immediately. */
  float32_t acceleration;
  float32_t turn_rate;
  bool8_t orient_to_movement;
} FpsPlayerSettings;

FpsPlayerSettings fps_player_settings_default(void);

/* One existing root entity owns motor, weapon and intent data; the player
 * owns input admission. The script host calls its tick and input hooks. Keep
 * it at a stable address and shut it down before its borrowed input/scene
 * expire. It owns neither a renderer nor an Actor registry. */
typedef struct FpsPlayerState {
  FpsWeaponState weapon;
  FpsWeaponReloadToken reload;
  uint32_t reserve_rounds;
  uint32_t held;
  float32_t yaw;
  float32_t pitch;
  Vec3 velocity;
  bool8_t grounded;
  bool8_t crouched;
} FpsPlayerState;

typedef struct FpsPlayerConfig {
  const VkrScriptApi *api;
  VkrScene *scene;
  InputState *input;
  VkrEntityId entity;
  FpsPlayerSettings settings;
  /* Motor spawn replacing the entity's authored position, or none. */
  Vec3 spawn_foot;
  bool8_t has_spawn;
  float32_t yaw;
  /* Entity whose animation player poses the body: the player entity for an
   * authored model, or the spawned model under it. Invalid for none. */
  VkrEntityId visual;
  /* Weapon bone in the player's animation skeleton; UINT32_MAX for none. */
  uint32_t weapon_bone;
  /* Nonzero base of the weapon identities this player allocates. */
  uint64_t instance_id;
} FpsPlayerConfig;

typedef struct FpsPlayer {
  const VkrScriptApi *api;
  VkrScene *scene;
  InputState *input;
  VkrEntityId entity;
  VkrComponentTypeId component;
  FpsPlayerSettings settings;
  FpsInput commands;
  FpsCameraRig camera;
  VkrEntityId visual;
  FpsPlayerAnimation animation;
  /* Speed-synchronized locomotion for banks with the mannequin's clips;
   * otherwise `animation` plays named action clips. Ticks record its input;
   * presentation poses it at the interpolated root's time: `steps` ticks
   * since reset, `pose_time` the seconds its pose reached. */
  FpsLocomotion locomotion;
  FpsLocomotionInput locomotion_input;
  uint64_t steps;
  float64_t pose_time;
  bool8_t locomotion_active;
  /* A presentation pose failed; the next tick reports `error`. */
  bool8_t pose_failed;
  Mat4 weapon_reference_inverse;
  uint32_t weapon_bone;
  bool8_t weapon_reference_valid;
  char error[192];
  FpsWeaponShot pending_shot;
  VkrPhysicsRayHit pending_hit;
  uint64_t shots_fired;
  uint64_t hits;
  uint64_t instance_id;
  float64_t epoch;
  float64_t last_frame_time;
  float32_t spawn_yaw;
  float32_t render_yaw;
  float32_t render_pitch;
  /* The body's yaw per tick, previous and current, for interpolation; the
   * camera's look yaw is separate. */
  float32_t previous_facing;
  float32_t facing;
  /* Commanded horizontal velocity (x, z) after acceleration. */
  Vec2 move_velocity;
  Vec3 previous_foot;
  Vec3 current_foot;
  bool8_t clock_running;
  bool8_t active;
  bool8_t shot_pending;
  bool8_t hit_pending;
} FpsPlayer;

/* Zero-initialize player before first attachment; reattachment requires
 * shutdown. Attach to a root entity at unit scale while the scene is paused
 * at a reset boundary. Initializes an automatic weapon (10 shots/s, reload
 * matched to the available clip), movement and a first-person camera. */
bool8_t fps_player_attach(FpsPlayer *player, const FpsPlayerConfig *config,
                          const char **error);
void fps_player_shutdown(FpsPlayer *player);

/* Tick hooks for the script host; `*error` stays valid until the next call. */
bool8_t fps_player_before_physics(FpsPlayer *player, uint64_t tick,
                                  const char **error);
void fps_player_after_physics(FpsPlayer *player);
/* Restores spawn state after a native simulation reset. */
void fps_player_reset(FpsPlayer *player);

/* One ordered input transition from the script host's input hook. */
void fps_player_observe(FpsPlayer *player, const VkrInputTransition *event);

/* Call once before scene_update, after the input pump. Returns admitted elapsed
 * time while the scene runs, independently of input focus and a display-delta
 * clamp. Inactive input cancels held actions and reload, discards pending
 * input, and requires fresh presses. Only scene pause/disable/fault stops the
 * clock. */
float64_t fps_player_frame(FpsPlayer *player, float64_t now, bool8_t active);
bool8_t fps_player_camera(FpsPlayer *player, FpsCameraRigPose *pose);
/* Interpolated body yaw for presentation, in the camera's yaw convention. */
float32_t fps_player_render_facing(const FpsPlayer *player, float32_t alpha);
/* Poses a locomotion body at `alpha` between the last two ticks, where the
 * root renders. A failure faults the player; its next tick reports it. */
void fps_player_animate(FpsPlayer *player, float32_t alpha);
