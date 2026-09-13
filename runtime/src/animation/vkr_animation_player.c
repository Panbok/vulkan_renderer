#include "vkr_animation_player.h"

#include "memory/arena.h"
#include <math.h>

typedef struct VkrAnimationBlendAccumulator {
  float64_t translation[3];
  float64_t rotation[4];
  float64_t scale[3];
  Vec4 reference;
} VkrAnimationBlendAccumulator;

typedef struct VkrAnimationCrossfade {
  float64_t source_time;
  float64_t source_compensation;
  float64_t elapsed;
  float64_t compensation;
  float64_t duration;
  uint32_t source_clip;
  bool8_t source_loop;
  bool8_t captured;
  bool8_t active;
} VkrAnimationCrossfade;

struct s_VkrAnimationPlayer {
  Arena *arena;
  const VkrAnimationAsset *asset;
  VkrAnimationTrs *local[2];
  VkrAnimationTrs *sample;
  VkrAnimationTrs *fade_snapshot;
  VkrAnimationBlendAccumulator *blend;
  Mat4 *global[2];
  Mat4 *palettes[2];
  uint64_t *skin_offsets;
  float64_t time;
  float64_t compensation;
  float64_t rate;
  uint64_t generation;
  uint64_t discontinuity;
  uint32_t clip;
  uint32_t current;
  bool8_t loop;
  bool8_t playing;
  bool8_t external_pose;
  VkrAnimationCrossfade fade;
  bool8_t checkpoint_active;
  bool8_t checkpoint_published;
};

struct s_VkrAnimationPlayerCheckpoint {
  VkrAnimationPlayer *player;
  VkrAnimationPlayer saved;
};
_Static_assert(sizeof(VkrAnimationPlayerCheckpoint) <= KB(1),
               "Scene reset checkpoint reserve");

static bool8_t animation_player_build_pose(VkrAnimationPlayer *player,
                                           uint32_t next) {
  if (!vkr_animation_global_pose(player->asset, player->local[next],
                                 player->global[next])) {
    return false_v;
  }
  for (uint32_t skin = 0; skin < player->asset->skin_count; ++skin) {
    if (!vkr_animation_skin_palette(player->asset, skin, player->global[next],
                                    player->palettes[next] +
                                        player->skin_offsets[skin])) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t animation_player_commit(VkrAnimationPlayer *player,
                                       bool8_t discontinuity) {
  if (player->generation == UINT64_MAX ||
      (discontinuity && player->discontinuity == UINT64_MAX)) {
    return false_v;
  }
  const uint32_t next = player->current ^ 1u;
  if (!animation_player_build_pose(player, next)) {
    return false_v;
  }
  player->current = next;
  player->checkpoint_published |= player->checkpoint_active;
  player->generation++;
  player->discontinuity += discontinuity ? 1u : 0u;
  return true_v;
}

static bool8_t animation_player_publish(VkrAnimationPlayer *player,
                                        uint32_t clip, float64_t time,
                                        bool8_t loop, bool8_t discontinuity) {
  if (!vkr_animation_sample(player->asset, clip, time,
                            player->local[player->current ^ 1u]) ||
      !animation_player_commit(player, discontinuity)) {
    return false_v;
  }
  player->clip = clip;
  player->time = time;
  player->loop = loop;
  player->external_pose = false_v;
  return true_v;
}

static void animation_player_accumulate(VkrAnimationPlayer *player,
                                        const VkrAnimationTrs *local,
                                        float64_t weight, bool8_t first) {
  for (uint32_t node = 0; node < player->asset->node_count; ++node) {
    if (player->asset->nodes[node].matrix_authored) {
      continue;
    }
    VkrAnimationBlendAccumulator *blend = &player->blend[node];
    if (first) {
      *blend =
          (VkrAnimationBlendAccumulator){.reference = local[node].rotation};
    }
    const VkrAnimationTrs *pose = &local[node];
    blend->translation[0] += (float64_t)pose->translation.x * weight;
    blend->translation[1] += (float64_t)pose->translation.y * weight;
    blend->translation[2] += (float64_t)pose->translation.z * weight;
    blend->scale[0] += (float64_t)pose->scale.x * weight;
    blend->scale[1] += (float64_t)pose->scale.y * weight;
    blend->scale[2] += (float64_t)pose->scale.z * weight;
    const float64_t dot = (float64_t)pose->rotation.x * blend->reference.x +
                          (float64_t)pose->rotation.y * blend->reference.y +
                          (float64_t)pose->rotation.z * blend->reference.z +
                          (float64_t)pose->rotation.w * blend->reference.w;
    const float64_t rotation_weight = dot < 0.0 ? -weight : weight;
    blend->rotation[0] += (float64_t)pose->rotation.x * rotation_weight;
    blend->rotation[1] += (float64_t)pose->rotation.y * rotation_weight;
    blend->rotation[2] += (float64_t)pose->rotation.z * rotation_weight;
    blend->rotation[3] += (float64_t)pose->rotation.w * rotation_weight;
  }
}

static bool8_t animation_player_finish_blend(VkrAnimationPlayer *player,
                                             bool8_t discontinuity) {
  VkrAnimationTrs *local = player->local[player->current ^ 1u];
  for (uint32_t node = 0; node < player->asset->node_count; ++node) {
    if (player->asset->nodes[node].matrix_authored) {
      local[node] = player->asset->nodes[node].rest;
      continue;
    }
    const VkrAnimationBlendAccumulator *blend = &player->blend[node];
    const float64_t length = sqrt(blend->rotation[0] * blend->rotation[0] +
                                  blend->rotation[1] * blend->rotation[1] +
                                  blend->rotation[2] * blend->rotation[2] +
                                  blend->rotation[3] * blend->rotation[3]);
    if (!isfinite(length) || length <= 0.0) {
      return false_v;
    }
    local[node] = (VkrAnimationTrs){
        .translation = {(float32_t)blend->translation[0],
                        (float32_t)blend->translation[1],
                        (float32_t)blend->translation[2]},
        .rotation = {(float32_t)(blend->rotation[0] / length),
                     (float32_t)(blend->rotation[1] / length),
                     (float32_t)(blend->rotation[2] / length),
                     (float32_t)(blend->rotation[3] / length)},
        .scale = {(float32_t)blend->scale[0], (float32_t)blend->scale[1],
                  (float32_t)blend->scale[2]},
    };
  }
  return animation_player_commit(player, discontinuity);
}

VkrAnimationPlayer *vkr_animation_player_create(const VkrAnimationAsset *asset,
                                                VkrAllocator *scratch,
                                                uint32_t clip, bool8_t loop,
                                                float64_t rate, bool8_t playing,
                                                const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!isfinite(rate) || !vkr_animation_validate(asset, scratch, error)) {
    if (error && !*error) {
      *error = "animation player requires a finite rate";
    }
    return NULL;
  }
  if (clip >= asset->clip_count) {
    if (error) {
      *error = "animation player clip is out of range";
    }
    return NULL;
  }
  uint64_t joint_count = 0;
  for (uint32_t skin = 0; skin < asset->skin_count; ++skin) {
    joint_count += asset->skins[skin].joint_count;
  }
  /* All local accumulation, interruption snapshots and candidate poses have
     player lifetime. Allocate their full capacity before publishing views. */
  uint64_t base_size =
      ARENA_HEADER_SIZE + sizeof(VkrAnimationPlayer) +
      (uint64_t)asset->node_count *
          (4u * sizeof(VkrAnimationTrs) + sizeof(VkrAnimationBlendAccumulator) +
           2u * sizeof(Mat4)) +
      (uint64_t)asset->skin_count * sizeof(uint64_t) + 16u * MaxAlign();
  if (base_size > SIZE_MAX ||
      joint_count > (SIZE_MAX - base_size) / (2u * sizeof(Mat4))) {
    if (error) {
      *error = "animation player pose capacity overflow";
    }
    return NULL;
  }
  uint64_t bytes = base_size + joint_count * 2u * sizeof(Mat4);
  Arena *arena = arena_create(Max(bytes, KB(64)), KB(4));
  VkrAnimationPlayer *player = NULL;
  if (!arena) {
    goto cleanup;
  }
  player = arena_alloc(arena, sizeof(*player), ARENA_MEMORY_TAG_STRUCT);
  if (!player) {
    goto cleanup;
  }
  *player = (VkrAnimationPlayer){.arena = arena,
                                 .asset = asset,
                                 .rate = rate,
                                 .playing = playing ? true_v : false_v};
  const uint64_t local_bytes =
      (uint64_t)asset->node_count * sizeof(VkrAnimationTrs);
  player->sample = arena_alloc(arena, local_bytes, ARENA_MEMORY_TAG_ARRAY);
  player->fade_snapshot =
      arena_alloc(arena, local_bytes, ARENA_MEMORY_TAG_ARRAY);
  player->blend =
      arena_alloc(arena, (uint64_t)asset->node_count * sizeof(*player->blend),
                  ARENA_MEMORY_TAG_ARRAY);
  player->skin_offsets = arena_alloc(
      arena, (uint64_t)asset->skin_count * sizeof(*player->skin_offsets),
      ARENA_MEMORY_TAG_ARRAY);
  for (uint32_t i = 0; i < 2; ++i) {
    player->local[i] = arena_alloc(arena, local_bytes, ARENA_MEMORY_TAG_ARRAY);
    player->global[i] =
        arena_alloc(arena, (uint64_t)asset->node_count * sizeof(Mat4),
                    ARENA_MEMORY_TAG_ARRAY);
    if (joint_count) {
      player->palettes[i] = arena_alloc(arena, joint_count * sizeof(Mat4),
                                        ARENA_MEMORY_TAG_ARRAY);
    }
    if (!player->local[i] || !player->global[i] ||
        (joint_count && !player->palettes[i])) {
      goto cleanup;
    }
  }
  if (!player->sample || !player->fade_snapshot || !player->blend ||
      (asset->skin_count && !player->skin_offsets)) {
    goto cleanup;
  }
  uint64_t offset = 0;
  for (uint32_t skin = 0; skin < asset->skin_count; ++skin) {
    player->skin_offsets[skin] = offset;
    offset += asset->skins[skin].joint_count;
  }
  if (!animation_player_publish(player, clip, 0.0, loop, true_v)) {
    goto cleanup;
  }
  return player;

cleanup:
  if (arena) {
    arena_destroy(arena);
  }
  if (error) {
    *error = "animation player allocation or initial pose evaluation failed";
  }
  return NULL;
}

void vkr_animation_player_destroy(VkrAnimationPlayer *player) {
  if (player) {
    arena_destroy(player->arena);
  }
}

static bool8_t animation_player_step_clock(const VkrAnimationPlayer *player,
                                           uint32_t clip, bool8_t loop,
                                           float64_t delta, float64_t *time,
                                           float64_t *compensation,
                                           bool8_t *discontinuity) {
  float64_t corrected = delta - *compensation;
  float64_t next = *time + corrected;
  if (!isfinite(delta) || !isfinite(corrected) || !isfinite(next)) {
    return false_v;
  }
  float64_t remainder = (next - *time) - corrected;
  const float64_t duration = player->asset->clips[clip].duration;
  if (loop && duration > 0.0) {
    if (next < 0.0 || next >= duration) {
      *discontinuity = true_v;
      next = fmod(next, duration);
      if (next < 0.0) {
        next += duration;
      }
      if (next >= duration) {
        next = 0.0;
      }
      remainder = 0.0;
    }
  } else if (next < 0.0 || next > duration) {
    next = next < 0.0 ? 0.0 : duration;
    remainder = 0.0;
  }
  *time = next;
  *compensation = remainder;
  return true_v;
}

bool8_t vkr_animation_player_sample_blend(VkrAnimationPlayer *player,
                                          const VkrAnimationSample *samples,
                                          uint32_t count,
                                          bool8_t discontinuity) {
  if (player && player->checkpoint_published) {
    return false_v;
  }
  if (!player || !samples || !count ||
      count > VKR_ANIMATION_BLEND_SAMPLE_CAPACITY) {
    return false_v;
  }
  float64_t total = 0.0;
  for (uint32_t i = 0u; i < count; ++i) {
    if (samples[i].clip >= player->asset->clip_count ||
        !isfinite(samples[i].time) || !isfinite(samples[i].weight) ||
        samples[i].weight < 0.0f) {
      return false_v;
    }
    total += samples[i].weight;
  }
  if (total <= 0.0 || !isfinite(total)) {
    return false_v;
  }
  bool8_t first = true_v;
  for (uint32_t i = 0u; i < count; ++i) {
    if (samples[i].weight == 0.0f) {
      continue;
    }
    if (!vkr_animation_sample(player->asset, samples[i].clip, samples[i].time,
                              player->sample)) {
      return false_v;
    }
    animation_player_accumulate(player, player->sample,
                                (float64_t)samples[i].weight / total, first);
    first = false_v;
  }
  if (!animation_player_finish_blend(player, discontinuity)) {
    return false_v;
  }
  player->fade = (VkrAnimationCrossfade){0};
  player->external_pose = true_v;
  return true_v;
}

bool8_t vkr_animation_player_crossfade(VkrAnimationPlayer *player,
                                       uint32_t clip, bool8_t loop,
                                       float64_t duration) {
  if (player && player->checkpoint_active) {
    return false_v;
  }
  if (!player || clip >= player->asset->clip_count || !isfinite(duration) ||
      duration < 0.0) {
    return false_v;
  }
  if (duration == 0.0) {
    return vkr_animation_player_select_clip(player, clip, loop);
  }
  /* Validate the destination endpoint before changing controls or the retained
     interruption snapshot. The current displayed pose is left untouched. */
  const uint32_t next = player->current ^ 1u;
  if (!vkr_animation_sample(player->asset, clip, 0.0, player->local[next]) ||
      !animation_player_build_pose(player, next)) {
    return false_v;
  }
  const bool8_t captured = player->fade.active || player->external_pose;
  if (captured) {
    MemCopy(player->fade_snapshot, player->local[player->current],
            (uint64_t)player->asset->node_count *
                sizeof(*player->fade_snapshot));
  }
  player->fade = (VkrAnimationCrossfade){
      .source_time = player->time,
      .source_compensation = player->compensation,
      .source_clip = player->clip,
      .source_loop = player->loop,
      .duration = duration,
      .captured = captured,
      .active = true_v,
  };
  player->clip = clip;
  player->time = 0.0;
  player->compensation = 0.0;
  player->loop = loop ? true_v : false_v;
  player->external_pose = false_v;
  return true_v;
}

bool8_t vkr_animation_player_advance(VkrAnimationPlayer *player, float64_t dt) {
  if (player && player->checkpoint_active) {
    return false_v;
  }
  if (!player || !isfinite(dt) || dt < 0.0) {
    return false_v;
  }
  if (!player->playing || dt == 0.0 ||
      (!player->fade.active && player->rate == 0.0)) {
    return true_v;
  }
  float64_t time = player->time;
  float64_t compensation = player->compensation;
  bool8_t discontinuity = false_v;
  const float64_t delta = dt * player->rate;
  if (!animation_player_step_clock(player, player->clip, player->loop, delta,
                                   &time, &compensation, &discontinuity)) {
    return false_v;
  }
  if (!player->fade.active) {
    if (!animation_player_publish(player, player->clip, time, player->loop,
                                  discontinuity)) {
      return false_v;
    }
    player->compensation = compensation;
    return true_v;
  }
  VkrAnimationCrossfade fade = player->fade;
  const float64_t corrected = dt - fade.compensation;
  const float64_t elapsed = fade.elapsed + corrected;
  fade.compensation = (elapsed - fade.elapsed) - corrected;
  if (!isfinite(elapsed) || elapsed >= fade.duration) {
    fade.elapsed = fade.duration;
    fade.compensation = 0.0;
  } else {
    fade.elapsed = elapsed;
  }
  const float64_t alpha = fade.elapsed / fade.duration;
  if (alpha < 1.0) {
    if (!fade.captured &&
        !animation_player_step_clock(
            player, fade.source_clip, fade.source_loop, delta,
            &fade.source_time, &fade.source_compensation, &discontinuity)) {
      return false_v;
    }
    const VkrAnimationTrs *source = player->fade_snapshot;
    if (!fade.captured) {
      if (!vkr_animation_sample(player->asset, fade.source_clip,
                                fade.source_time, player->sample)) {
        return false_v;
      }
      source = player->sample;
    }
    animation_player_accumulate(player, source, 1.0 - alpha, true_v);
  }
  if (!vkr_animation_sample(player->asset, player->clip, time,
                            player->sample)) {
    return false_v;
  }
  animation_player_accumulate(player, player->sample, alpha, alpha >= 1.0);
  if (!animation_player_finish_blend(player, discontinuity)) {
    return false_v;
  }
  fade.active = alpha < 1.0;
  player->fade = fade;
  player->time = time;
  player->compensation = compensation;
  player->external_pose = false_v;
  return true_v;
}

bool8_t vkr_animation_player_seek(VkrAnimationPlayer *player,
                                  float64_t seconds) {
  if (player && player->checkpoint_published) {
    return false_v;
  }
  if (!player || !isfinite(seconds)) {
    return false_v;
  }
  float64_t duration = player->asset->clips[player->clip].duration;
  float64_t time = seconds < 0.0        ? 0.0
                   : seconds > duration ? duration
                                        : seconds;
  if (!animation_player_publish(player, player->clip, time, player->loop,
                                true_v)) {
    return false_v;
  }
  player->compensation = 0.0;
  player->fade = (VkrAnimationCrossfade){0};
  return true_v;
}

bool8_t vkr_animation_player_select_clip(VkrAnimationPlayer *player,
                                         uint32_t clip, bool8_t loop) {
  if (player && player->checkpoint_active) {
    return false_v;
  }
  if (!player || clip >= player->asset->clip_count ||
      !animation_player_publish(player, clip, 0.0, loop, true_v)) {
    return false_v;
  }
  player->compensation = 0.0;
  player->fade = (VkrAnimationCrossfade){0};
  return true_v;
}

void vkr_animation_player_set_playing(VkrAnimationPlayer *player,
                                      bool8_t playing) {
  if (player) {
    player->playing = playing ? true_v : false_v;
  }
}

bool8_t vkr_animation_player_set_rate(VkrAnimationPlayer *player,
                                      float64_t rate) {
  if (!player || !isfinite(rate)) {
    return false_v;
  }
  player->rate = rate;
  player->compensation = 0.0;
  return true_v;
}

float64_t vkr_animation_player_time(const VkrAnimationPlayer *player) {
  return player ? player->time : 0.0;
}

uint32_t vkr_animation_player_clip(const VkrAnimationPlayer *player) {
  return player ? player->clip : UINT32_MAX;
}

bool8_t vkr_animation_player_playing(const VkrAnimationPlayer *player) {
  return player ? player->playing : false_v;
}

const Mat4 *vkr_animation_player_global_pose(const VkrAnimationPlayer *player) {
  return player ? player->global[player->current] : NULL;
}

const Mat4 *vkr_animation_player_skin_palette(const VkrAnimationPlayer *player,
                                              uint32_t skin) {
  if (!player || skin >= player->asset->skin_count) {
    return NULL;
  }
  return player->palettes[player->current] + player->skin_offsets[skin];
}

uint64_t vkr_animation_player_generation(const VkrAnimationPlayer *player) {
  return player ? player->generation : 0;
}

uint64_t vkr_animation_player_discontinuity(const VkrAnimationPlayer *player) {
  return player ? player->discontinuity : 0;
}

const VkrAnimationAsset *
vkr_animation_player_asset(const VkrAnimationPlayer *player) {
  return player ? player->asset : NULL;
}

float64_t vkr_animation_player_rate(const VkrAnimationPlayer *player) {
  return player ? player->rate : 0.0;
}

float64_t vkr_animation_player_duration(const VkrAnimationPlayer *player) {
  return player ? player->asset->clips[player->clip].duration : 0.0;
}

bool8_t
vkr_animation_player_crossfade_active(const VkrAnimationPlayer *player) {
  return player && player->fade.active;
}

float64_t
vkr_animation_player_crossfade_duration(const VkrAnimationPlayer *player) {
  return player ? player->fade.duration : 0.0;
}

float64_t
vkr_animation_player_crossfade_progress(const VkrAnimationPlayer *player) {
  return player && player->fade.duration > 0.0
             ? player->fade.elapsed / player->fade.duration
             : 0.0;
}

bool8_t vkr_animation_player_loop(const VkrAnimationPlayer *player) {
  return player ? player->loop : false_v;
}

bool8_t vkr_animation_player_override_globals(VkrAnimationPlayer *player,
                                              const uint32_t *nodes,
                                              const Mat4 *matrices,
                                              uint32_t count) {
  if (player && player->checkpoint_active) {
    return false_v;
  }
  if (!player || (count && (!nodes || !matrices)) ||
      count > player->asset->node_count || player->generation == UINT64_MAX) {
    return false_v;
  }
  if (!count) {
    return true_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (nodes[i] >= player->asset->node_count) {
      return false_v;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (nodes[i] == nodes[j]) {
        return false_v;
      }
    }
    for (uint32_t j = 0; j < 16; ++j) {
      if (!isfinite(matrices[i].elements[j])) {
        return false_v;
      }
    }
    if (fabsf(matrices[i].elements[3]) > 1e-6f ||
        fabsf(matrices[i].elements[7]) > 1e-6f ||
        fabsf(matrices[i].elements[11]) > 1e-6f ||
        fabsf(matrices[i].elements[15] - 1) > 1e-6f) {
      return false_v;
    }
  }
  const uint32_t next = player->current ^ 1u;
  MemCopy(player->local[next], player->local[player->current],
          (uint64_t)player->asset->node_count * sizeof(VkrAnimationTrs));
  for (uint32_t order = 0; order < player->asset->node_count; ++order) {
    const uint32_t n = player->asset->node_order[order];
    uint32_t override = 0;
    while (override < count && nodes[override] != n) {
      override++;
    }
    if (override < count) {
      player->global[next][n] = matrices[override];
    } else {
      const VkrAnimationNode *node = &player->asset->nodes[n];
      const VkrAnimationTrs *trs = &player->local[next][n];
      Mat4 local = node->matrix_authored
                       ? node->local
                       : mat4_mul(mat4_from_vkr_quat_pos(trs->rotation,
                                                         trs->translation),
                                  mat4_scale(trs->scale));
      player->global[next][n] =
          node->parent == UINT32_MAX
              ? local
              : mat4_mul(player->global[next][node->parent], local);
    }
    for (uint32_t j = 0; j < 16; ++j) {
      if (!isfinite(player->global[next][n].elements[j])) {
        return false_v;
      }
    }
  }
  for (uint32_t skin = 0; skin < player->asset->skin_count; ++skin) {
    if (!vkr_animation_skin_palette(player->asset, skin, player->global[next],
                                    player->palettes[next] +
                                        player->skin_offsets[skin])) {
      return false_v;
    }
  }
  player->current = next;
  player->generation++;
  return true_v;
}

VkrAnimationPlayerCheckpoint *
vkr_animation_player_checkpoint_begin(VkrAnimationPlayer *player,
                                      Arena *arena) {
  if (!player || !arena || player->checkpoint_active) {
    return NULL;
  }
  VkrAnimationPlayerCheckpoint *checkpoint =
      arena_alloc(arena, sizeof(*checkpoint), ARENA_MEMORY_TAG_STRUCT);
  if (!checkpoint) {
    return NULL;
  }
  *checkpoint =
      (VkrAnimationPlayerCheckpoint){.player = player, .saved = *player};
  player->checkpoint_active = true_v;
  player->checkpoint_published = false_v;
  return checkpoint;
}

void vkr_animation_player_checkpoint_finish(
    VkrAnimationPlayerCheckpoint *checkpoint, bool8_t commit) {
  if (!checkpoint || !checkpoint->player) {
    return;
  }
  if (!commit) {
    *checkpoint->player = checkpoint->saved;
  } else {
    checkpoint->player->checkpoint_active = false_v;
    checkpoint->player->checkpoint_published = false_v;
  }
  checkpoint->player = NULL;
}
