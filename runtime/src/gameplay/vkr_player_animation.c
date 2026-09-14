#include "gameplay/vkr_player_animation.h"

#include <math.h>
#include <string.h>

static uint32_t animation_clip(const VkrAnimationAsset *asset,
                               const char *name) {
  const uint64_t length = strlen(name);
  for (uint32_t i = 0; i < asset->clip_count; ++i) {
    if (asset->clips[i].name.length == length &&
        MemCompare(asset->clips[i].name.str, name, length) == 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

static uint32_t animation_prefer(const VkrAnimationAsset *asset,
                                 const char *primary, const char *fallback) {
  const uint32_t clip = animation_clip(asset, primary);
  return clip != UINT32_MAX || !fallback ? clip
                                         : animation_clip(asset, fallback);
}

static bool8_t animation_input_valid(const VkrPlayerAnimationInput *input) {
  return input && isfinite(input->speed) && input->speed >= 0 &&
         input->grounded <= true_v && input->crouched <= true_v &&
         input->reloading <= true_v;
}

static bool8_t animation_loop(VkrPlayerAnimationMode mode) {
  return mode <= VKR_PLAYER_ANIMATION_CROUCH_WALK ||
         mode == VKR_PLAYER_ANIMATION_JUMP_LOOP;
}

static VkrPlayerAnimationMode
animation_base(const VkrPlayerAnimation *animation,
               const VkrPlayerAnimationInput *input) {
  if (!input->grounded) {
    if (animation->clips[VKR_PLAYER_ANIMATION_JUMP_LOOP] != UINT32_MAX) {
      return VKR_PLAYER_ANIMATION_JUMP_LOOP;
    }
    if (animation->clips[VKR_PLAYER_ANIMATION_JUMP_START] != UINT32_MAX) {
      return VKR_PLAYER_ANIMATION_JUMP_START;
    }
  }
  if (input->crouched) {
    return input->speed > 0.1f ? VKR_PLAYER_ANIMATION_CROUCH_WALK
                               : VKR_PLAYER_ANIMATION_CROUCH_IDLE;
  }
  if (input->speed <= 0.1f) {
    return VKR_PLAYER_ANIMATION_IDLE;
  }
  const float32_t run_threshold =
      animation->mode == VKR_PLAYER_ANIMATION_RUN ? 2.0f : 2.2f;
  return input->speed >= run_threshold ? VKR_PLAYER_ANIMATION_RUN
                                       : VKR_PLAYER_ANIMATION_WALK;
}

static float64_t animation_rate(const VkrPlayerAnimation *animation,
                                VkrPlayerAnimationMode mode,
                                const VkrPlayerAnimationInput *input) {
  const float32_t reference = animation->reference_speeds[mode];
  return reference > 0 ? (float64_t)input->speed / reference : 1.0;
}

static bool8_t animation_select(VkrPlayerAnimation *animation,
                                VkrPlayerAnimationMode mode,
                                const VkrPlayerAnimationInput *input,
                                bool8_t immediate, bool8_t retrigger) {
  const uint32_t clip = animation->clips[mode];
  if (clip == UINT32_MAX) {
    return false_v;
  }
  if (immediate) {
    if (!vkr_animation_player_select_clip(animation->player, clip,
                                          animation_loop(mode))) {
      return false_v;
    }
  } else if (mode != animation->mode || retrigger) {
    const float64_t fade = mode == VKR_PLAYER_ANIMATION_FIRE ||
                                   mode == VKR_PLAYER_ANIMATION_CROUCH_FIRE
                               ? 0.04
                               : 0.12;
    if (!vkr_animation_player_crossfade(animation->player, clip,
                                        animation_loop(mode), fade)) {
      return false_v;
    }
  }
  const float64_t rate = animation_rate(animation, mode, input);
  /* Reapplying an unchanged rate would discard the player's clock remainder. */
  if (vkr_animation_player_rate(animation->player) != rate &&
      !vkr_animation_player_set_rate(animation->player, rate)) {
    return false_v;
  }
  animation->mode = mode;
  animation->previous = *input;
  return true_v;
}

bool8_t vkr_player_animation_reset(VkrPlayerAnimation *animation,
                                   const VkrPlayerAnimationInput *input) {
  if (!animation || !animation->player || !animation_input_valid(input)) {
    return false_v;
  }
  VkrPlayerAnimation next = *animation;
  next.mode = VKR_PLAYER_ANIMATION_IDLE;
  VkrPlayerAnimationMode mode = animation_base(&next, input);
  if (input->reloading &&
      animation->clips[VKR_PLAYER_ANIMATION_RELOAD] != UINT32_MAX) {
    mode = VKR_PLAYER_ANIMATION_RELOAD;
  }
  if (!animation_select(&next, mode, input, true_v, false_v)) {
    return false_v;
  }
  vkr_animation_player_set_playing(next.player, true_v);
  *animation = next;
  return true_v;
}

bool8_t vkr_player_animation_initialize(VkrPlayerAnimation *animation,
                                        VkrAnimationPlayer *player,
                                        float64_t fixed_dt,
                                        const char **error) {
  if (error) {
    *error = NULL;
  }
  const VkrAnimationAsset *asset = vkr_animation_player_asset(player);
  if (!animation || !asset || !isfinite(fixed_dt) || fixed_dt <= 0) {
    if (error) {
      *error = "Player animation requires a live bank and positive fixed step";
    }
    return false_v;
  }
  VkrPlayerAnimation next = {.player = player};
  for (uint32_t i = 0; i < VKR_PLAYER_ANIMATION_MODE_COUNT; ++i) {
    next.clips[i] = UINT32_MAX;
  }
  uint32_t idle = animation_prefer(asset, "Rifle_Aim_Idle", "Pistol_Aim_Idle");
  if (idle == UINT32_MAX) {
    idle = animation_clip(asset, "Idle");
  }
  if (idle == UINT32_MAX) {
    if (error) {
      *error = "Player animation bank has no recognized idle clip";
    }
    return false_v;
  }
  next.clips[VKR_PLAYER_ANIMATION_IDLE] = idle;
  uint32_t walk = animation_prefer(asset, "Rifle_Walk", "Walk_Forward");
  if (walk != UINT32_MAX) {
    next.reference_speeds[VKR_PLAYER_ANIMATION_WALK] = 1.3f;
  } else {
    walk = idle;
  }
  next.clips[VKR_PLAYER_ANIMATION_WALK] = walk;
  uint32_t run = animation_prefer(asset, "Rifle_Run", "Run_Forward");
  if (run != UINT32_MAX) {
    next.reference_speeds[VKR_PLAYER_ANIMATION_RUN] = 3.4f;
  } else {
    run = walk;
    next.reference_speeds[VKR_PLAYER_ANIMATION_RUN] =
        next.reference_speeds[VKR_PLAYER_ANIMATION_WALK];
  }
  next.clips[VKR_PLAYER_ANIMATION_RUN] = run;
  uint32_t crouch = animation_prefer(asset, "Crouch_Rifle_Aim", "Crouch_Idle");
  next.clips[VKR_PLAYER_ANIMATION_CROUCH_IDLE] =
      crouch == UINT32_MAX ? idle : crouch;
  crouch = animation_clip(asset, "Crouch_Walk");
  if (crouch != UINT32_MAX) {
    next.reference_speeds[VKR_PLAYER_ANIMATION_CROUCH_WALK] = 0.6f;
  } else {
    crouch = next.clips[VKR_PLAYER_ANIMATION_CROUCH_IDLE];
  }
  next.clips[VKR_PLAYER_ANIMATION_CROUCH_WALK] = crouch;
  next.clips[VKR_PLAYER_ANIMATION_CROUCH_DOWN] =
      animation_clip(asset, "Crouch_Down");
  next.clips[VKR_PLAYER_ANIMATION_CROUCH_UP] =
      animation_clip(asset, "Crouch_Up");
  next.clips[VKR_PLAYER_ANIMATION_JUMP_START] =
      animation_clip(asset, "Jump_Start");
  next.clips[VKR_PLAYER_ANIMATION_JUMP_LOOP] =
      animation_clip(asset, "Jump_Loop");
  next.clips[VKR_PLAYER_ANIMATION_JUMP_LAND] =
      animation_clip(asset, "Jump_Land");
  next.clips[VKR_PLAYER_ANIMATION_FIRE] =
      animation_prefer(asset, "Rifle_Fire", "Pistol_Fire");
  next.clips[VKR_PLAYER_ANIMATION_CROUCH_FIRE] =
      animation_clip(asset, "Crouch_Rifle_Fire");
  if (next.clips[VKR_PLAYER_ANIMATION_CROUCH_FIRE] == UINT32_MAX) {
    next.clips[VKR_PLAYER_ANIMATION_CROUCH_FIRE] =
        next.clips[VKR_PLAYER_ANIMATION_FIRE];
  }
  next.clips[VKR_PLAYER_ANIMATION_RELOAD] =
      animation_prefer(asset, "Rifle_Reload", "Pistol_Reload");
  for (uint32_t i = VKR_PLAYER_ANIMATION_CROUCH_DOWN;
       i < VKR_PLAYER_ANIMATION_MODE_COUNT; ++i) {
    if (next.clips[i] != UINT32_MAX &&
        asset->clips[next.clips[i]].duration <= 0) {
      next.clips[i] = UINT32_MAX;
    }
  }
  const uint32_t reload = next.clips[VKR_PLAYER_ANIMATION_RELOAD];
  if (reload != UINT32_MAX) {
    const float64_t ticks =
        ceil((float64_t)asset->clips[reload].duration / fixed_dt);
    if (!isfinite(ticks) || ticks >= (float64_t)UINT64_MAX) {
      if (error) {
        *error = "Animation reload duration exceeds the simulation tick range";
      }
      return false_v;
    }
    next.reload_ticks = (uint64_t)Max(1.0, ticks);
  }
  const VkrPlayerAnimationInput initial = {.grounded = true_v};
  if (!vkr_player_animation_reset(&next, &initial)) {
    if (error) {
      *error = "Player animation initial pose could not be selected";
    }
    return false_v;
  }
  *animation = next;
  return true_v;
}

bool8_t vkr_player_animation_update(VkrPlayerAnimation *animation,
                                    const VkrPlayerAnimationInput *input) {
  if (!animation || !animation->player || !animation_input_valid(input) ||
      input->shot_sequence < animation->previous.shot_sequence) {
    return false_v;
  }
  VkrPlayerAnimationMode mode = animation_base(animation, input);
  bool8_t retrigger = false_v;
  const bool8_t shot = input->shot_sequence > animation->previous.shot_sequence;
  const bool8_t finished = vkr_animation_player_time(animation->player) >=
                           vkr_animation_player_duration(animation->player);
  if (input->reloading &&
      animation->clips[VKR_PLAYER_ANIMATION_RELOAD] != UINT32_MAX) {
    mode = VKR_PLAYER_ANIMATION_RELOAD;
    retrigger = !animation->previous.reloading;
  } else if (!input->grounded) {
    /* Physical takeoff is immediate. Jump_Start contains anticipation, so an
     * available airborne loop starts directly rather than delaying the motor.
     */
  } else if (!animation->previous.grounded &&
             animation->clips[VKR_PLAYER_ANIMATION_JUMP_LAND] != UINT32_MAX) {
    mode = VKR_PLAYER_ANIMATION_JUMP_LAND;
    retrigger = true_v;
  } else if (shot &&
             animation->clips[input->crouched ? VKR_PLAYER_ANIMATION_CROUCH_FIRE
                                              : VKR_PLAYER_ANIMATION_FIRE] !=
                 UINT32_MAX) {
    mode = input->crouched ? VKR_PLAYER_ANIMATION_CROUCH_FIRE
                           : VKR_PLAYER_ANIMATION_FIRE;
    retrigger = true_v;
  } else if (input->crouched != animation->previous.crouched &&
             animation->clips[input->crouched
                                  ? VKR_PLAYER_ANIMATION_CROUCH_DOWN
                                  : VKR_PLAYER_ANIMATION_CROUCH_UP] !=
                 UINT32_MAX) {
    mode = input->crouched ? VKR_PLAYER_ANIMATION_CROUCH_DOWN
                           : VKR_PLAYER_ANIMATION_CROUCH_UP;
    retrigger = true_v;
  } else if (!finished &&
             (animation->mode == VKR_PLAYER_ANIMATION_FIRE ||
              animation->mode == VKR_PLAYER_ANIMATION_CROUCH_FIRE ||
              animation->mode == VKR_PLAYER_ANIMATION_CROUCH_DOWN ||
              animation->mode == VKR_PLAYER_ANIMATION_CROUCH_UP ||
              animation->mode == VKR_PLAYER_ANIMATION_JUMP_LAND)) {
    mode = animation->mode;
  }
  return animation_select(animation, mode, input, false_v, retrigger);
}
