#include "fps_locomotion.h"
#include "fps_mannequin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOCOMOTION_SAMPLE_MAX VKR_ANIM_BLEND_MAX
/* Below this ground speed the body stands. */
#define LOCOMOTION_STILL 0.05f
/* Seconds of the velocity, crouch and airborne blends. */
#define LOCOMOTION_VELOCITY_TIME 0.08
#define LOCOMOTION_CROUCH_TIME 0.2
#define LOCOMOTION_AIR_IN_TIME 0.12
#define LOCOMOTION_AIR_OUT_TIME 0.06
/* A landing plays only after this long in the air. */
#define LOCOMOTION_LAND_MIN_AIR 0.25
/* Jump_Land touches down this far in: it opens with the legs reaching. */
#define LOCOMOTION_LAND_OFFSET 0.1
/* Seconds a landing or a jump start takes to hand over to what follows. */
#define LOCOMOTION_LAND_EXIT 0.25
#define LOCOMOTION_START_EXIT 0.15

static const char *const s_direction_names[FPS_LOCOMOTION_DIRECTION_COUNT] = {
    "Fwd", "Bwd", "Left", "Right"};

// =============================================================================
// Clips
// =============================================================================

/* A clip with a positive duration by name, or VKR_CLIP_NONE. */
static uint32_t locomotion_find(VkrCtx *ctx, VkrEntity entity,
                                const char *name) {
  const uint32_t clip = vkr_anim_clip_find(ctx, entity, name);
  return clip != VKR_CLIP_NONE &&
                 vkr_anim_clip_duration(ctx, entity, clip) > 0.0f
             ? clip
             : VKR_CLIP_NONE;
}

/* A clip by name with its ground speed from the mannequin's clip table. */
static FpsLocomotionClip locomotion_clip(VkrCtx *ctx, VkrEntity entity,
                                         const char *name) {
  FpsLocomotionClip clip = {.clip = locomotion_find(ctx, entity, name)};
  if (clip.clip == VKR_CLIP_NONE) {
    return clip;
  }
  clip.duration = vkr_anim_clip_duration(ctx, entity, clip.clip);
  for (uint32_t i = 0; i < ArrayCount(fps_mannequin_clips); ++i) {
    if (strcmp(fps_mannequin_clips[i].name, name) == 0) {
      clip.speed = fps_mannequin_clips[i].speed;
      break;
    }
  }
  return clip;
}

static bool8_t locomotion_has(const FpsLocomotionClip *clip) {
  return clip->clip != VKR_CLIP_NONE;
}

/* A moving loop needs a speed to derive its stride. */
static bool8_t locomotion_moving(const FpsLocomotionClip *clip) {
  return locomotion_has(clip) && clip->speed > 0.0f;
}

static FpsLocomotionGaits locomotion_gaits(VkrCtx *ctx, VkrEntity entity,
                                           bool8_t crouched) {
  char name[64];
  FpsLocomotionGaits gaits;
  gaits.idle = locomotion_clip(ctx, entity, crouched ? "Crouch_Idle" : "Idle");
  for (uint32_t d = 0; d < FPS_LOCOMOTION_DIRECTION_COUNT; ++d) {
    snprintf(name, sizeof(name), crouched ? "Crouch_Walk_%s" : "Walk_%s",
             s_direction_names[d]);
    gaits.walk[d] = locomotion_clip(ctx, entity, name);
    gaits.jog[d] = (FpsLocomotionClip){.clip = VKR_CLIP_NONE};
    if (!crouched) {
      snprintf(name, sizeof(name), "Jog_%s", s_direction_names[d]);
      gaits.jog[d] = locomotion_clip(ctx, entity, name);
    }
  }
  gaits.run = crouched ? (FpsLocomotionClip){.clip = VKR_CLIP_NONE}
                       : locomotion_clip(ctx, entity, "Run_Fwd");
  return gaits;
}

bool8_t fps_locomotion_supported(VkrCtx *ctx, VkrEntity entity) {
  if (!vkr_anim_id(ctx, entity)) {
    return false_v;
  }
  const FpsLocomotionClip walk = locomotion_clip(ctx, entity, "Walk_Fwd");
  return locomotion_find(ctx, entity, "Idle") != VKR_CLIP_NONE &&
         locomotion_moving(&walk) &&
         locomotion_find(ctx, entity, "Jump_Loop") != VKR_CLIP_NONE;
}

// =============================================================================
// Sample mix
// =============================================================================

typedef struct LocomotionMix {
  VkrAnimSample samples[LOCOMOTION_SAMPLE_MAX];
  /* Per sample: the loop duration its time follows through the shared
   * phase, or zero for samples on their own clocks. */
  float32_t phase_duration[LOCOMOTION_SAMPLE_MAX];
  uint32_t count;
  /* Weighted stride sum and weight of the phase-driven loops. */
  float64_t stride;
  float64_t moving;
} LocomotionMix;

static void mix_add(LocomotionMix *mix, const FpsLocomotionClip *clip,
                    float64_t time, float32_t weight, bool8_t phased) {
  if (!locomotion_has(clip) || !(weight > 1e-4f) ||
      mix->count == LOCOMOTION_SAMPLE_MAX) {
    return;
  }
  mix->samples[mix->count] =
      (VkrAnimSample){.clip = clip->clip, .time = time, .weight = weight};
  mix->phase_duration[mix->count] = phased ? clip->duration : 0.0f;
  mix->count++;
  if (phased) {
    mix->stride += (float64_t)weight * clip->speed * clip->duration;
    mix->moving += weight;
  }
}

/* One direction's gait at ground speed `speed`: idle into walk, walk into
 * jog, jog into the forward run, then the fastest loop, faster. Returns
 * the idle share it leaves to the caller. */
static float32_t mix_direction(LocomotionMix *mix,
                               const FpsLocomotionGaits *gaits,
                               FpsLocomotionDirection direction,
                               float32_t speed, float32_t weight) {
  const FpsLocomotionClip *walk = &gaits->walk[direction];
  if (!locomotion_moving(walk)) {
    walk = &gaits->walk[FPS_LOCOMOTION_FORWARD];
  }
  const FpsLocomotionClip *jog = &gaits->jog[direction];
  const FpsLocomotionClip *run =
      direction == FPS_LOCOMOTION_FORWARD ? &gaits->run : NULL;
  const bool8_t has_jog = locomotion_moving(jog) && jog->speed > walk->speed;
  const bool8_t has_run = run && locomotion_moving(run) &&
                          run->speed > (has_jog ? jog->speed : walk->speed);
  if (speed <= walk->speed) {
    const float32_t t = speed / walk->speed;
    mix_add(mix, walk, 0.0, weight * t, true_v);
    return weight * (1.0f - t);
  }
  if (has_jog && speed <= jog->speed) {
    const float32_t t = (speed - walk->speed) / (jog->speed - walk->speed);
    mix_add(mix, walk, 0.0, weight * (1.0f - t), true_v);
    mix_add(mix, jog, 0.0, weight * t, true_v);
    return 0.0f;
  }
  const FpsLocomotionClip *below = has_jog ? jog : walk;
  if (has_run && speed <= run->speed) {
    const float32_t t = (speed - below->speed) / (run->speed - below->speed);
    mix_add(mix, below, 0.0, weight * (1.0f - t), true_v);
    mix_add(mix, run, 0.0, weight * t, true_v);
    return 0.0f;
  }
  mix_add(mix, has_run ? run : below, 0.0, weight, true_v);
  return 0.0f;
}

/* Ground locomotion of one stance, scaled by `weight`. */
static void mix_ground(LocomotionMix *mix, const FpsLocomotion *locomotion,
                       const FpsLocomotionGaits *gaits, float64_t idle_time,
                       float32_t weight) {
  if (!(weight > 1e-4f)) {
    return;
  }
  const float32_t forward = locomotion->forward;
  const float32_t right = locomotion->right;
  const float32_t speed = sqrtf(forward * forward + right * right);
  float32_t idle = weight;
  if (speed > LOCOMOTION_STILL) {
    /* Direction shares from the velocity's components, summing to one. */
    const float32_t total = fabsf(forward) + fabsf(right);
    const float32_t shares[FPS_LOCOMOTION_DIRECTION_COUNT] = {
        Max(0.0f, forward) / total, Max(0.0f, -forward) / total,
        Max(0.0f, -right) / total, Max(0.0f, right) / total};
    idle = 0.0f;
    for (uint32_t d = 0; d < FPS_LOCOMOTION_DIRECTION_COUNT; ++d) {
      if (shares[d] > 0.0f) {
        idle += mix_direction(mix, gaits, (FpsLocomotionDirection)d, speed,
                              weight * shares[d]);
      }
    }
  }
  const FpsLocomotionClip *idle_clip =
      locomotion_has(&gaits->idle) ? &gaits->idle : &locomotion->stand.idle;
  mix_add(mix, idle_clip, fmod(idle_time, (float64_t)idle_clip->duration), idle,
          false_v);
}

static float32_t approach(float32_t value, float32_t target, float64_t dt,
                          float64_t seconds) {
  const float32_t step = (float32_t)(dt / seconds);
  return value < target ? Min(target, value + step) : Max(target, value - step);
}

static float32_t locomotion_smoothstep(float64_t edge0, float64_t edge1,
                                       float64_t x) {
  const float64_t t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return (float32_t)(t * t * (3.0 - 2.0 * t));
}

// =============================================================================
// Public API
// =============================================================================

bool8_t fps_locomotion_reset(VkrCtx *ctx, FpsLocomotion *locomotion) {
  if (!locomotion || !vkr_entity_valid(locomotion->entity)) {
    return false_v;
  }
  locomotion->phase = 0.0;
  locomotion->idle_time = 0.0;
  locomotion->crouch_idle_time = 0.0;
  locomotion->air_time = 0.0;
  locomotion->start_time = 0.0;
  locomotion->loop_time = 0.0;
  locomotion->land_time = -1.0;
  locomotion->forward = 0.0f;
  locomotion->right = 0.0f;
  locomotion->crouch_weight = 0.0f;
  locomotion->air_weight = 0.0f;
  locomotion->land_weight = 0.0f;
  locomotion->airborne = false_v;
  locomotion->jumped = false_v;
  vkr_anim_set_playing(ctx, locomotion->entity, false_v);
  const VkrAnimSample idle = {
      .clip = locomotion->stand.idle.clip, .time = 0.0, .weight = 1.0f};
  return vkr_anim_blend(ctx, locomotion->entity, &idle, 1, true_v);
}

bool8_t fps_locomotion_initialize(VkrCtx *ctx, FpsLocomotion *locomotion,
                                  VkrEntity entity, const char **error) {
  if (!locomotion || !fps_locomotion_supported(ctx, entity)) {
    *error = "Locomotion needs a bank with Idle, Walk_Fwd and Jump_Loop";
    return false_v;
  }
  *locomotion = (FpsLocomotion){
      .entity = entity,
      .stand = locomotion_gaits(ctx, entity, false_v),
      .crouch = locomotion_gaits(ctx, entity, true_v),
      .jump_start = locomotion_clip(ctx, entity, "Jump_Start"),
      .jump_loop = locomotion_clip(ctx, entity, "Jump_Loop"),
      .jump_land = locomotion_clip(ctx, entity, "Jump_Land"),
  };
  if (!fps_locomotion_reset(ctx, locomotion)) {
    *error = "Locomotion could not pose the idle";
    return false_v;
  }
  return true_v;
}

bool8_t fps_locomotion_update(VkrCtx *ctx, FpsLocomotion *locomotion,
                              const FpsLocomotionInput *input, float64_t dt) {
  if (!locomotion || !vkr_entity_valid(locomotion->entity) || !input ||
      !isfinite(dt) || dt < 0.0 || !isfinite(input->forward) ||
      !isfinite(input->right) || !isfinite(input->up)) {
    return false_v;
  }
  const float32_t follow =
      (float32_t)(1.0 - exp(-dt / LOCOMOTION_VELOCITY_TIME));
  locomotion->forward += (input->forward - locomotion->forward) * follow;
  locomotion->right += (input->right - locomotion->right) * follow;
  locomotion->crouch_weight =
      approach(locomotion->crouch_weight, input->crouched ? 1.0f : 0.0f, dt,
               LOCOMOTION_CROUCH_TIME);
  locomotion->idle_time += dt;
  locomotion->crouch_idle_time += dt;

  // Airborne and landing state from actual support.
  if (!input->grounded) {
    if (!locomotion->airborne) {
      locomotion->airborne = true_v;
      locomotion->jumped = input->up > 0.5f;
      locomotion->air_time = 0.0;
      locomotion->start_time = 0.0;
      locomotion->loop_time = 0.0;
      locomotion->land_time = -1.0;
      locomotion->land_weight = 0.0f;
    } else {
      locomotion->air_time += dt;
    }
  } else if (locomotion->airborne) {
    locomotion->airborne = false_v;
    if (locomotion->air_time >= LOCOMOTION_LAND_MIN_AIR &&
        locomotion_has(&locomotion->jump_land)) {
      locomotion->land_time = LOCOMOTION_LAND_OFFSET;
      locomotion->land_weight = 1.0f;
    }
  }
  locomotion->air_weight = approach(
      locomotion->air_weight, locomotion->airborne ? 1.0f : 0.0f, dt,
      locomotion->airborne ? LOCOMOTION_AIR_IN_TIME : LOCOMOTION_AIR_OUT_TIME);
  const float32_t speed = sqrtf(locomotion->forward * locomotion->forward +
                                locomotion->right * locomotion->right);
  if (locomotion->land_time >= 0.0) {
    const float64_t exit_at =
        locomotion->jump_land.duration - LOCOMOTION_LAND_EXIT;
    if (locomotion->land_time >= exit_at || speed > 0.5f) {
      locomotion->land_weight =
          approach(locomotion->land_weight, 0.0f, dt, LOCOMOTION_LAND_EXIT);
    }
    if (locomotion->land_weight <= 0.0f ||
        locomotion->land_time >= locomotion->jump_land.duration) {
      locomotion->land_time = -1.0;
      locomotion->land_weight = 0.0f;
    }
  }

  // Air, landing and ground shares sum to one.
  LocomotionMix mix = {0};
  const float32_t air = locomotion->air_weight;
  const float32_t landing =
      locomotion->land_time >= 0.0 ? locomotion->land_weight : 0.0f;
  const float32_t land = (1.0f - air) * landing;
  const float32_t ground = (1.0f - air) * (1.0f - landing);
  const float32_t crouch = locomotion->crouch_weight;
  mix_ground(&mix, locomotion, &locomotion->stand, locomotion->idle_time,
             ground * (1.0f - crouch));
  mix_ground(&mix, locomotion, &locomotion->crouch,
             locomotion->crouch_idle_time, ground * crouch);

  // The shared phase advances by the distance covered over the blended
  // stride, then every phase-driven loop samples at it.
  if (mix.moving > 0.0 && mix.stride > 0.0 && speed > LOCOMOTION_STILL) {
    const float64_t stride = mix.stride / mix.moving;
    locomotion->phase = fmod(locomotion->phase + speed * dt / stride, 1.0);
  }
  for (uint32_t i = 0; i < mix.count; ++i) {
    if (mix.phase_duration[i] > 0.0f) {
      mix.samples[i].time = locomotion->phase * mix.phase_duration[i];
    }
  }

  if (air > 0.0f) {
    const FpsLocomotionClip *start = &locomotion->jump_start;
    const FpsLocomotionClip *loop = &locomotion->jump_loop;
    float32_t start_share = 0.0f;
    if (locomotion->jumped && locomotion_has(start) &&
        locomotion->start_time < start->duration) {
      start_share =
          1.0f - locomotion_smoothstep(start->duration - LOCOMOTION_START_EXIT,
                                       start->duration, locomotion->start_time);
      mix_add(&mix, start, locomotion->start_time, air * start_share, false_v);
    }
    mix_add(&mix, loop, fmod(locomotion->loop_time, (float64_t)loop->duration),
            air * (1.0f - start_share), false_v);
  }
  if (locomotion->airborne) {
    locomotion->start_time += dt;
    locomotion->loop_time += dt;
  }
  if (land > 0.0f) {
    mix_add(&mix, &locomotion->jump_land, locomotion->land_time, land, false_v);
  }
  if (locomotion->land_time >= 0.0) {
    locomotion->land_time += dt;
  }
  if (!mix.count) {
    mix_add(&mix, &locomotion->stand.idle, 0.0, 1.0f, false_v);
  }
  return vkr_anim_blend(ctx, locomotion->entity, mix.samples, mix.count,
                        false_v);
}
