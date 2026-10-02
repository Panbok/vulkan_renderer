#pragma once

#include "sdk.h"

/* Directions of the locomotion loops, relative to the body's facing. */
typedef enum FpsLocomotionDirection {
  FPS_LOCOMOTION_FORWARD,
  FPS_LOCOMOTION_BACKWARD,
  FPS_LOCOMOTION_LEFT,
  FPS_LOCOMOTION_RIGHT,
  FPS_LOCOMOTION_DIRECTION_COUNT
} FpsLocomotionDirection;

/* One clip of the bank: UINT32_MAX when the bank lacks it. Speed is the
 * loop's ground speed in metres per second, so speed * duration is the
 * distance one cycle covers. */
typedef struct FpsLocomotionClip {
  uint32_t clip;
  float32_t speed;
  float32_t duration;
} FpsLocomotionClip;

/* Idle plus walk, optional jog and an optional forward run per direction. */
typedef struct FpsLocomotionGaits {
  FpsLocomotionClip idle;
  FpsLocomotionClip walk[FPS_LOCOMOTION_DIRECTION_COUNT];
  FpsLocomotionClip jog[FPS_LOCOMOTION_DIRECTION_COUNT];
  FpsLocomotionClip run;
} FpsLocomotionGaits;

/* Velocity in the body's frame and support state, supplied every tick. */
typedef struct FpsLocomotionInput {
  float32_t forward; /* Metres/second along the facing. */
  float32_t right;   /* Metres/second to the body's right. */
  float32_t up;      /* Vertical metres/second. */
  bool8_t grounded;
  bool8_t crouched;
} FpsLocomotionInput;

/*
 * Speed-synchronized locomotion for a bank with the mannequin's clip names
 * (Idle, Walk_*, Jog_*, Run_Fwd, Crouch_*, Jump_*), on caller-owned clocks
 * through vkr_anim_blend. Every loop starts on the same footfall,
 * so one normalized phase drives them all; the phase advances by distance
 * over the blended stride, which keeps planted feet still at any speed.
 * Jumps play Jump_Start into Jump_Loop while airborne and Jump_Land on
 * touchdown after a real fall. The player stays paused so the scene's own
 * clock never replaces the pose.
 *
 * Caller-owned fixed storage naming the entity whose animation it poses;
 * nothing allocates. It is the only writer of that animation's pose.
 */
typedef struct FpsLocomotion {
  VkrEntity entity;
  FpsLocomotionGaits stand;
  FpsLocomotionGaits crouch;
  FpsLocomotionClip jump_start;
  FpsLocomotionClip jump_loop;
  FpsLocomotionClip jump_land;
  float64_t phase;
  float64_t idle_time;
  float64_t crouch_idle_time;
  float64_t air_time;
  float64_t start_time;
  float64_t loop_time;
  /* Negative while no landing plays. */
  float64_t land_time;
  float32_t forward;
  float32_t right;
  float32_t crouch_weight;
  float32_t air_weight;
  float32_t land_weight;
  bool8_t airborne;
  bool8_t jumped;
} FpsLocomotion;

/* True when the bank holds at least Idle, Walk_Fwd and Jump_Loop; missing
 * directions, gaits and crouch clips fall back to what exists. */
bool8_t fps_locomotion_supported(VkrCtx *ctx, VkrEntity entity);

/* Resolves clips by name (speeds from the mannequin's clip table), pauses
 * the player and poses Idle. */
bool8_t fps_locomotion_initialize(VkrCtx *ctx, FpsLocomotion *locomotion,
                                  VkrEntity entity, const char **error);

/* Standing idle with fresh clocks, as after a reset. */
bool8_t fps_locomotion_reset(VkrCtx *ctx, FpsLocomotion *locomotion);

/* Advances `dt` seconds and poses the player. The caller advances it per
 * presented frame by the time its interpolated root advanced, so planted
 * feet stay under the rendered body. */
bool8_t fps_locomotion_update(VkrCtx *ctx, FpsLocomotion *locomotion,
                              const FpsLocomotionInput *input, float64_t dt);
