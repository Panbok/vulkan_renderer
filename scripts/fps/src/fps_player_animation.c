#include "fps_player_animation.h"

#include <math.h>
#include <string.h>

static uint32_t animation_prefer(VkrCtx *ctx, VkrEntity entity,
                                 const char *primary, const char *fallback) {
  const uint32_t clip = vkr_anim_clip_find(ctx, entity, primary);
  return clip != VKR_CLIP_NONE || !fallback
             ? clip
             : vkr_anim_clip_find(ctx, entity, fallback);
}

static bool8_t animation_input_valid(const FpsPlayerAnimationInput *input) {
  return input && isfinite(input->speed) && input->speed >= 0 &&
         input->grounded <= true_v && input->crouched <= true_v &&
         input->reloading <= true_v;
}

static bool8_t animation_loop(FpsPlayerAnimationMode mode) {
  return mode <= FPS_PLAYER_ANIMATION_CROUCH_WALK ||
         mode == FPS_PLAYER_ANIMATION_JUMP_LOOP;
}

static FpsPlayerAnimationMode
animation_base(const FpsPlayerAnimation *animation,
               const FpsPlayerAnimationInput *input) {
  if (!input->grounded) {
    if (animation->clips[FPS_PLAYER_ANIMATION_JUMP_LOOP] != VKR_CLIP_NONE) {
      return FPS_PLAYER_ANIMATION_JUMP_LOOP;
    }
    if (animation->clips[FPS_PLAYER_ANIMATION_JUMP_START] != VKR_CLIP_NONE) {
      return FPS_PLAYER_ANIMATION_JUMP_START;
    }
  }
  if (input->crouched) {
    return input->speed > 0.1f ? FPS_PLAYER_ANIMATION_CROUCH_WALK
                               : FPS_PLAYER_ANIMATION_CROUCH_IDLE;
  }
  if (input->speed <= 0.1f) {
    return FPS_PLAYER_ANIMATION_IDLE;
  }
  const float32_t run_threshold =
      animation->mode == FPS_PLAYER_ANIMATION_RUN ? 2.0f : 2.2f;
  return input->speed >= run_threshold ? FPS_PLAYER_ANIMATION_RUN
                                       : FPS_PLAYER_ANIMATION_WALK;
}

static float64_t animation_rate(const FpsPlayerAnimation *animation,
                                FpsPlayerAnimationMode mode,
                                const FpsPlayerAnimationInput *input) {
  const float32_t reference = animation->reference_speeds[mode];
  return reference > 0 ? (float64_t)input->speed / reference : 1.0;
}

static bool8_t animation_select(VkrCtx *ctx, FpsPlayerAnimation *animation,
                                FpsPlayerAnimationMode mode,
                                const FpsPlayerAnimationInput *input,
                                bool8_t immediate, bool8_t retrigger) {
  const uint32_t clip = animation->clips[mode];
  if (clip == VKR_CLIP_NONE) {
    return false_v;
  }
  if (immediate) {
    if (!vkr_anim_play(ctx, animation->entity, clip, animation_loop(mode))) {
      return false_v;
    }
  } else if (mode != animation->mode || retrigger) {
    const float64_t fade = mode == FPS_PLAYER_ANIMATION_FIRE ||
                                   mode == FPS_PLAYER_ANIMATION_CROUCH_FIRE
                               ? 0.04
                               : 0.12;
    if (!vkr_anim_crossfade(ctx, animation->entity, clip, animation_loop(mode),
                            fade)) {
      return false_v;
    }
  }
  const float64_t rate = animation_rate(animation, mode, input);
  /* Reapplying an unchanged rate would discard the player's clock remainder. */
  if (vkr_anim_rate(ctx, animation->entity) != rate &&
      !vkr_anim_set_rate(ctx, animation->entity, rate)) {
    return false_v;
  }
  animation->mode = mode;
  animation->previous = *input;
  return true_v;
}

bool8_t fps_player_animation_reset(VkrCtx *ctx, FpsPlayerAnimation *animation,
                                   const FpsPlayerAnimationInput *input) {
  if (!animation || !vkr_entity_valid(animation->entity) ||
      !animation_input_valid(input)) {
    return false_v;
  }
  FpsPlayerAnimation next = *animation;
  next.mode = FPS_PLAYER_ANIMATION_IDLE;
  FpsPlayerAnimationMode mode = animation_base(&next, input);
  if (input->reloading &&
      animation->clips[FPS_PLAYER_ANIMATION_RELOAD] != VKR_CLIP_NONE) {
    mode = FPS_PLAYER_ANIMATION_RELOAD;
  }
  if (!animation_select(ctx, &next, mode, input, true_v, false_v)) {
    return false_v;
  }
  vkr_anim_set_playing(ctx, next.entity, true_v);
  *animation = next;
  return true_v;
}

bool8_t fps_player_animation_initialize(VkrCtx *ctx,
                                        FpsPlayerAnimation *animation,
                                        VkrEntity entity, float64_t fixed_dt,
                                        const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!animation || !vkr_anim_id(ctx, entity) || !isfinite(fixed_dt) ||
      fixed_dt <= 0) {
    if (error) {
      *error = "Player animation requires a live bank and positive fixed step";
    }
    return false_v;
  }
  FpsPlayerAnimation next = {.entity = entity};
  for (uint32_t i = 0; i < FPS_PLAYER_ANIMATION_MODE_COUNT; ++i) {
    next.clips[i] = VKR_CLIP_NONE;
  }
  uint32_t idle =
      animation_prefer(ctx, entity, "Rifle_Aim_Idle", "Pistol_Aim_Idle");
  if (idle == VKR_CLIP_NONE) {
    idle = vkr_anim_clip_find(ctx, entity, "Idle");
  }
  if (idle == VKR_CLIP_NONE) {
    if (error) {
      *error = "Player animation bank has no recognized idle clip";
    }
    return false_v;
  }
  next.clips[FPS_PLAYER_ANIMATION_IDLE] = idle;
  uint32_t walk = animation_prefer(ctx, entity, "Rifle_Walk", "Walk_Forward");
  if (walk != VKR_CLIP_NONE) {
    next.reference_speeds[FPS_PLAYER_ANIMATION_WALK] = 1.3f;
  } else {
    walk = idle;
  }
  next.clips[FPS_PLAYER_ANIMATION_WALK] = walk;
  uint32_t run = animation_prefer(ctx, entity, "Rifle_Run", "Run_Forward");
  if (run != VKR_CLIP_NONE) {
    next.reference_speeds[FPS_PLAYER_ANIMATION_RUN] = 3.4f;
  } else {
    run = walk;
    next.reference_speeds[FPS_PLAYER_ANIMATION_RUN] =
        next.reference_speeds[FPS_PLAYER_ANIMATION_WALK];
  }
  next.clips[FPS_PLAYER_ANIMATION_RUN] = run;
  uint32_t crouch =
      animation_prefer(ctx, entity, "Crouch_Rifle_Aim", "Crouch_Idle");
  next.clips[FPS_PLAYER_ANIMATION_CROUCH_IDLE] =
      crouch == VKR_CLIP_NONE ? idle : crouch;
  crouch = vkr_anim_clip_find(ctx, entity, "Crouch_Walk");
  if (crouch != VKR_CLIP_NONE) {
    next.reference_speeds[FPS_PLAYER_ANIMATION_CROUCH_WALK] = 0.6f;
  } else {
    crouch = next.clips[FPS_PLAYER_ANIMATION_CROUCH_IDLE];
  }
  next.clips[FPS_PLAYER_ANIMATION_CROUCH_WALK] = crouch;
  next.clips[FPS_PLAYER_ANIMATION_CROUCH_DOWN] =
      vkr_anim_clip_find(ctx, entity, "Crouch_Down");
  next.clips[FPS_PLAYER_ANIMATION_CROUCH_UP] =
      vkr_anim_clip_find(ctx, entity, "Crouch_Up");
  next.clips[FPS_PLAYER_ANIMATION_JUMP_START] =
      vkr_anim_clip_find(ctx, entity, "Jump_Start");
  next.clips[FPS_PLAYER_ANIMATION_JUMP_LOOP] =
      vkr_anim_clip_find(ctx, entity, "Jump_Loop");
  next.clips[FPS_PLAYER_ANIMATION_JUMP_LAND] =
      vkr_anim_clip_find(ctx, entity, "Jump_Land");
  next.clips[FPS_PLAYER_ANIMATION_FIRE] =
      animation_prefer(ctx, entity, "Rifle_Fire", "Pistol_Fire");
  next.clips[FPS_PLAYER_ANIMATION_CROUCH_FIRE] =
      vkr_anim_clip_find(ctx, entity, "Crouch_Rifle_Fire");
  if (next.clips[FPS_PLAYER_ANIMATION_CROUCH_FIRE] == VKR_CLIP_NONE) {
    next.clips[FPS_PLAYER_ANIMATION_CROUCH_FIRE] =
        next.clips[FPS_PLAYER_ANIMATION_FIRE];
  }
  next.clips[FPS_PLAYER_ANIMATION_RELOAD] =
      animation_prefer(ctx, entity, "Rifle_Reload", "Pistol_Reload");
  for (uint32_t i = FPS_PLAYER_ANIMATION_CROUCH_DOWN;
       i < FPS_PLAYER_ANIMATION_MODE_COUNT; ++i) {
    if (next.clips[i] != VKR_CLIP_NONE &&
        vkr_anim_clip_duration(ctx, entity, next.clips[i]) <= 0) {
      next.clips[i] = VKR_CLIP_NONE;
    }
  }
  const uint32_t reload = next.clips[FPS_PLAYER_ANIMATION_RELOAD];
  if (reload != VKR_CLIP_NONE) {
    const float64_t ticks =
        ceil((float64_t)vkr_anim_clip_duration(ctx, entity, reload) / fixed_dt);
    if (!isfinite(ticks) || ticks >= (float64_t)UINT64_MAX) {
      if (error) {
        *error = "Animation reload duration exceeds the simulation tick range";
      }
      return false_v;
    }
    next.reload_ticks = (uint64_t)Max(1.0, ticks);
  }
  const FpsPlayerAnimationInput initial = {.grounded = true_v};
  if (!fps_player_animation_reset(ctx, &next, &initial)) {
    if (error) {
      *error = "Player animation initial pose could not be selected";
    }
    return false_v;
  }
  *animation = next;
  return true_v;
}

bool8_t fps_player_animation_update(VkrCtx *ctx, FpsPlayerAnimation *animation,
                                    const FpsPlayerAnimationInput *input) {
  if (!animation || !vkr_entity_valid(animation->entity) ||
      !animation_input_valid(input) ||
      input->shot_sequence < animation->previous.shot_sequence) {
    return false_v;
  }
  FpsPlayerAnimationMode mode = animation_base(animation, input);
  bool8_t retrigger = false_v;
  const bool8_t shot = input->shot_sequence > animation->previous.shot_sequence;
  const bool8_t finished = vkr_anim_time(ctx, animation->entity) >=
                           vkr_anim_duration(ctx, animation->entity);
  if (input->reloading &&
      animation->clips[FPS_PLAYER_ANIMATION_RELOAD] != VKR_CLIP_NONE) {
    mode = FPS_PLAYER_ANIMATION_RELOAD;
    retrigger = !animation->previous.reloading;
  } else if (!input->grounded) {
    /* Physical takeoff is immediate. Jump_Start contains anticipation, so an
     * available airborne loop starts directly rather than delaying the motor.
     */
  } else if (!animation->previous.grounded &&
             animation->clips[FPS_PLAYER_ANIMATION_JUMP_LAND] !=
                 VKR_CLIP_NONE) {
    mode = FPS_PLAYER_ANIMATION_JUMP_LAND;
    retrigger = true_v;
  } else if (shot &&
             animation->clips[input->crouched ? FPS_PLAYER_ANIMATION_CROUCH_FIRE
                                              : FPS_PLAYER_ANIMATION_FIRE] !=
                 VKR_CLIP_NONE) {
    mode = input->crouched ? FPS_PLAYER_ANIMATION_CROUCH_FIRE
                           : FPS_PLAYER_ANIMATION_FIRE;
    retrigger = true_v;
  } else if (input->crouched != animation->previous.crouched &&
             animation->clips[input->crouched
                                  ? FPS_PLAYER_ANIMATION_CROUCH_DOWN
                                  : FPS_PLAYER_ANIMATION_CROUCH_UP] !=
                 VKR_CLIP_NONE) {
    mode = input->crouched ? FPS_PLAYER_ANIMATION_CROUCH_DOWN
                           : FPS_PLAYER_ANIMATION_CROUCH_UP;
    retrigger = true_v;
  } else if (!finished &&
             (animation->mode == FPS_PLAYER_ANIMATION_FIRE ||
              animation->mode == FPS_PLAYER_ANIMATION_CROUCH_FIRE ||
              animation->mode == FPS_PLAYER_ANIMATION_CROUCH_DOWN ||
              animation->mode == FPS_PLAYER_ANIMATION_CROUCH_UP ||
              animation->mode == FPS_PLAYER_ANIMATION_JUMP_LAND)) {
    mode = animation->mode;
  }
  return animation_select(ctx, animation, mode, input, false_v, retrigger);
}
