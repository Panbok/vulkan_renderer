#pragma once

#include "fps_camera_rig.h"
#include "fps_input.h"
#include "fps_locomotion.h"
#include "fps_player_animation.h"
#include "fps_weapon.h"
#include "sdk.h"

/* Indexed by FpsCameraRigMode; V cycles through all three during play. */
extern const char *const fps_camera_mode_names[];

/* Authored behavior values of the `fps_player` script component. In third
 * person with orient_to_movement the body turns towards its movement at
 * turn_rate (radians/second) and speed changes at acceleration (metres per
 * second squared; zero is immediate). Other views strafe with the camera
 * and change speed immediately. */
#define FPS_PLAYER_FIELDS                                                      \
  VKR_FIELD(F32, move_speed, "Move speed", 5.0f, .unit = "m/s", .min = 0.0f,   \
            .max = 50.0f, .step = 0.05f)                                       \
  VKR_FIELD(F32, crouch_speed, "Crouch speed", 0.6f, .unit = "m/s",            \
            .min = 0.0f, .max = 50.0f, .step = 0.05f)                          \
  VKR_FIELD(F32, jump_speed, "Jump speed", 5.0f,                               \
            .tooltip = "Vertical takeoff speed", .unit = "m/s", .min = 0.0f,   \
            .max = 50.0f, .step = 0.05f)                                       \
  VKR_FIELD(U32, magazine, "Magazine", 12u, .tooltip = "Rounds per magazine",  \
            .min = 1.0f, .max = 1000.0f)                                       \
  VKR_FIELD(U32, reserve, "Reserve", 120u,                                     \
            .tooltip = "Reserve rounds at spawn and reset", .min = 0.0f,       \
            .max = 100000.0f)                                                  \
  VKR_FIELD(ENUM, camera_mode, "Camera mode", FPS_CAMERA_RIG_FIRST_PERSON,     \
            .tooltip = "Camera when the player attaches; V cycles modes",      \
            .names = fps_camera_mode_names)                                    \
  VKR_FIELD(F32, walk_speed, "Walk speed", 1.5f,                               \
            .tooltip = "Speed while Shift is held", .unit = "m/s",             \
            .min = 0.0f, .max = 50.0f, .step = 0.05f)                          \
  VKR_FIELD(BOOL, orient_to_movement, "Orient to movement", true_v,            \
            .tooltip = "In third person the body turns towards where it "      \
                       "moves and the camera orbits freely",                   \
            .group = "Third person")                                           \
  VKR_FIELD(F32, acceleration, "Acceleration", 14.0f,                          \
            .tooltip = "Speed change while orienting to movement; 0 is "       \
                       "immediate",                                            \
            .unit = "m/s²", .min = 0.0f, .max = 200.0f, .step = 0.1f)          \
  VKR_FIELD(ANGLE, turn_rate, "Turn rate", 9.42477796f,                        \
            .tooltip = "How fast the body turns towards its movement",         \
            .unit = "°/s", .min = 0.0f, .max = 3600.0f, .step = 1.0f)

VKR_COMPONENT_DECLARE(FpsPlayerSettings, fps_player, FPS_PLAYER_FIELDS)

FpsPlayerSettings fps_player_settings_default(void);

/* Runtime state on the player entity: weapon, motor and intent data. */
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
  VkrEntity entity;
  FpsPlayerSettings settings;
  /* Motor spawn replacing the entity's authored position, or none. */
  Vec3 spawn_foot;
  bool8_t has_spawn;
  float32_t yaw;
  /* Entity whose animation poses the body: the player entity for an
   * authored model, or the spawned model under it. NONE for none. */
  VkrEntity visual;
  /* Weapon bone in the player's animation skeleton; UINT32_MAX for none. */
  uint32_t weapon_bone;
  /* Nonzero base of the weapon identities this player allocates. */
  uint64_t instance_id;
  /* The visual is a spawned model still loading: its animation binds on the
   * first tick after it arrives. */
  bool8_t visual_loading;
} FpsPlayerConfig;

/* One existing root entity owns motor, weapon and intent data; the player
 * owns input admission. The module calls its tick and input hooks. It owns
 * neither a renderer nor an Actor registry. */
typedef struct FpsPlayer {
  VkrEntity entity;
  VkrStateType state_type;
  FpsPlayerSettings settings;
  FpsInput commands;
  FpsCameraRig camera;
  VkrEntity visual;
  /* The visual's model is still loading; see FpsPlayerConfig. */
  bool8_t visual_loading;
  /* vkr_anim_id of the visual when the animation was prepared. */
  uint64_t animation_id;
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
  VkrRayHit pending_hit;
  /* The engine's `button` and its `press` input, and `mover` and its `use`
     input (ADR-084), resolved once; NULL when the engine has none. */
  const VkrComponentDesc *button_type;
  VkrIoInput press_input;
  const VkrComponentDesc *mover_type;
  VkrIoInput use_input;
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
  /* Horizontal velocity (x, z) of the ground it last stood on, which it
     keeps while off the ground (ADR-073). */
  Vec2 carried;
  Vec3 previous_foot;
  Vec3 current_foot;
  bool8_t attached;
  bool8_t clock_running;
  bool8_t active;
  bool8_t shot_pending;
  bool8_t hit_pending;
} FpsPlayer;

/* Zero-initialize player before first attachment; reattachment requires
 * shutdown. Attach to a root entity at unit scale while the scene is paused
 * at a reset boundary. Initializes an automatic weapon (10 shots/s, reload
 * matched to the available clip), movement and a first-person camera. */
bool8_t fps_player_attach(VkrCtx *ctx, FpsPlayer *player,
                          const FpsPlayerConfig *config, const char **error);
void fps_player_shutdown(VkrCtx *ctx, FpsPlayer *player);

/* The player entity's runtime state, or NULL. */
FpsPlayerState *fps_player_state(VkrCtx *ctx, const FpsPlayer *player);

/* Tick hooks; `*error` stays valid until the next call. */
bool8_t fps_player_before_physics(VkrCtx *ctx, FpsPlayer *player, uint64_t tick,
                                  const char **error);
void fps_player_after_physics(VkrCtx *ctx, FpsPlayer *player);
/* Restores spawn state, as after a simulation reset. */
void fps_player_reset(VkrCtx *ctx, FpsPlayer *player);

/* One ordered input transition. */
void fps_player_observe(VkrCtx *ctx, FpsPlayer *player,
                        const VkrInputEvent *event);

/* Call once before the scene advances, after the input pump. Returns
 * admitted elapsed time while the scene runs, independently of input focus
 * and a display-delta clamp. Inactive input cancels held actions and
 * reload, discards pending input, and requires fresh presses. Only scene
 * pause/disable/fault stops the clock. */
float64_t fps_player_frame(VkrCtx *ctx, FpsPlayer *player, float64_t now,
                           bool8_t active);
bool8_t fps_player_camera(VkrCtx *ctx, FpsPlayer *player,
                          FpsCameraRigPose *pose);
/* Interpolated body yaw for presentation, in the camera's yaw convention. */
float32_t fps_player_render_facing(const FpsPlayer *player, float32_t alpha);
/* Poses a locomotion body at `alpha` between the last two ticks, where the
 * root renders. A failure faults the player; its next tick reports it. */
void fps_player_animate(VkrCtx *ctx, FpsPlayer *player, float32_t alpha);
