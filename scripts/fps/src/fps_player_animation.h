#pragma once

#include "sdk.h"

typedef enum FpsPlayerAnimationMode {
  FPS_PLAYER_ANIMATION_IDLE,
  FPS_PLAYER_ANIMATION_WALK,
  FPS_PLAYER_ANIMATION_RUN,
  FPS_PLAYER_ANIMATION_CROUCH_IDLE,
  FPS_PLAYER_ANIMATION_CROUCH_WALK,
  FPS_PLAYER_ANIMATION_CROUCH_DOWN,
  FPS_PLAYER_ANIMATION_CROUCH_UP,
  FPS_PLAYER_ANIMATION_JUMP_START,
  FPS_PLAYER_ANIMATION_JUMP_LOOP,
  FPS_PLAYER_ANIMATION_JUMP_LAND,
  FPS_PLAYER_ANIMATION_FIRE,
  FPS_PLAYER_ANIMATION_CROUCH_FIRE,
  FPS_PLAYER_ANIMATION_RELOAD,
  FPS_PLAYER_ANIMATION_MODE_COUNT
} FpsPlayerAnimationMode;

typedef struct FpsPlayerAnimationInput {
  /* Horizontal solved speed, metres/second. Shot sequence counts accepted
   * shots, not trigger attempts. It may decrease only through reset. */
  float32_t speed;
  uint64_t shot_sequence;
  bool8_t grounded;
  bool8_t crouched;
  bool8_t reloading;
} FpsPlayerAnimationInput;

/* Caller-owned fixed storage. Treat fields as read-only after initialize.
 * Names the entity whose animation it drives; discard this value when that
 * animation changes. This is the only playback-control writer; do not also
 * install a graph or drive the same animation from the editor. No function
 * allocates or advances time. The scene remains the sole clock. */
typedef struct FpsPlayerAnimation {
  VkrEntity entity;
  uint32_t clips[FPS_PLAYER_ANIMATION_MODE_COUNT];
  float32_t reference_speeds[FPS_PLAYER_ANIMATION_MODE_COUNT];
  FpsPlayerAnimationInput previous;
  FpsPlayerAnimationMode mode;
  /* Zero when the bank has no reload clip. Otherwise ceil(duration/fixed_dt),
   * at least one; use this duration for the weapon's authoritative deadline. */
  uint64_t reload_ticks;
} FpsPlayerAnimation;

/* Cold name resolution; never persists bank-local clip indices to assets.
 * Requires Idle, Rifle_Aim_Idle or Pistol_Aim_Idle. Missing action clips are
 * skipped and missing locomotion clips fall back to available idle/walk poses.
 * Initializes a standing idle pose with shot sequence zero. */
bool8_t fps_player_animation_initialize(VkrCtx *ctx,
                                        FpsPlayerAnimation *animation,
                                        VkrEntity entity, float64_t fixed_dt,
                                        const char **error);

/* Selects the current base/reload pose immediately and establishes new input
 * history, including a fresh shot sequence. Does not replay an old action. */
bool8_t fps_player_animation_reset(VkrCtx *ctx, FpsPlayerAnimation *animation,
                                   const FpsPlayerAnimationInput *input);

/* Call once in the before-physics hook after gameplay/motor decisions, before
 * the scene advances animation. Accepted fire retriggers an interruptible
 * one-shot; reload intent owns its clip until completion/cancellation. Airborne
 * and crouch changes interrupt ordinary actions. These are whole-body clips:
 * no additive recoil, upper-body masking or movement authority is implied. */
bool8_t fps_player_animation_update(VkrCtx *ctx, FpsPlayerAnimation *animation,
                                    const FpsPlayerAnimationInput *input);
