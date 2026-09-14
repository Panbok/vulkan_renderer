#pragma once

#include "animation/vkr_animation_player.h"

typedef enum VkrPlayerAnimationMode {
  VKR_PLAYER_ANIMATION_IDLE,
  VKR_PLAYER_ANIMATION_WALK,
  VKR_PLAYER_ANIMATION_RUN,
  VKR_PLAYER_ANIMATION_CROUCH_IDLE,
  VKR_PLAYER_ANIMATION_CROUCH_WALK,
  VKR_PLAYER_ANIMATION_CROUCH_DOWN,
  VKR_PLAYER_ANIMATION_CROUCH_UP,
  VKR_PLAYER_ANIMATION_JUMP_START,
  VKR_PLAYER_ANIMATION_JUMP_LOOP,
  VKR_PLAYER_ANIMATION_JUMP_LAND,
  VKR_PLAYER_ANIMATION_FIRE,
  VKR_PLAYER_ANIMATION_CROUCH_FIRE,
  VKR_PLAYER_ANIMATION_RELOAD,
  VKR_PLAYER_ANIMATION_MODE_COUNT
} VkrPlayerAnimationMode;

typedef struct VkrPlayerAnimationInput {
  /* Horizontal solved speed, metres/second. Shot sequence counts accepted
   * shots, not trigger attempts. It may decrease only through reset. */
  float32_t speed;
  uint64_t shot_sequence;
  bool8_t grounded;
  bool8_t crouched;
  bool8_t reloading;
} VkrPlayerAnimationInput;

/* Caller-owned fixed storage. Treat fields as read-only after initialize.
 * Borrows the scene-owned player and its immutable asset until detach; discard
 * this value before either expires. This is the only playback-control writer;
 * do not also install a graph or drive the same player from the editor.
 * No function allocates or advances time. The scene remains the sole clock. */
typedef struct VkrPlayerAnimation {
  VkrAnimationPlayer *player;
  uint32_t clips[VKR_PLAYER_ANIMATION_MODE_COUNT];
  float32_t reference_speeds[VKR_PLAYER_ANIMATION_MODE_COUNT];
  VkrPlayerAnimationInput previous;
  VkrPlayerAnimationMode mode;
  /* Zero when the bank has no reload clip. Otherwise ceil(duration/fixed_dt),
   * at least one; use this duration for the weapon's authoritative deadline. */
  uint64_t reload_ticks;
} VkrPlayerAnimation;

/* Cold name resolution; never persists bank-local clip indices to assets.
 * Requires Idle, Rifle_Aim_Idle or Pistol_Aim_Idle. Missing action clips are
 * skipped and missing locomotion clips fall back to available idle/walk poses.
 * Initializes a standing idle pose with shot sequence zero. */
bool8_t vkr_player_animation_initialize(VkrPlayerAnimation *animation,
                                        VkrAnimationPlayer *player,
                                        float64_t fixed_dt, const char **error);

/* Selects the current base/reload pose immediately and establishes new input
 * history, including a fresh shot sequence. Does not replay an old action. */
bool8_t vkr_player_animation_reset(VkrPlayerAnimation *animation,
                                   const VkrPlayerAnimationInput *input);

/* Call once in the before-physics hook after gameplay/motor decisions, before
 * the scene advances animation. Accepted fire retriggers an interruptible
 * one-shot; reload intent owns its clip until completion/cancellation. Airborne
 * and crouch changes interrupt ordinary actions. These are whole-body clips:
 * no additive recoil, upper-body masking or movement authority is implied. */
bool8_t vkr_player_animation_update(VkrPlayerAnimation *animation,
                                    const VkrPlayerAnimationInput *input);
