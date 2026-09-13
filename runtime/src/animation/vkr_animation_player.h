#pragma once

#include "assets/vkr_animation.h"

typedef struct s_VkrAnimationPlayer VkrAnimationPlayer;

enum { VKR_ANIMATION_BLEND_SAMPLE_CAPACITY = 32 };

typedef struct VkrAnimationSample {
  uint32_t clip;
  float64_t time;
  float32_t weight;
} VkrAnimationSample;

/* Borrows an immutable asset until destroy. Owns a separately destroyable
 * arena; scratch is borrowed only for creation-time validation. No mutation
 * allocates. Initial time is zero, including negative playback rates. */
VkrAnimationPlayer *vkr_animation_player_create(const VkrAnimationAsset *asset,
                                                VkrAllocator *scratch,
                                                uint32_t clip, bool8_t loop,
                                                float64_t rate, bool8_t playing,
                                                const char **error);
void vkr_animation_player_destroy(VkrAnimationPlayer *player);
/* Immutable asset borrowed for the lifetime of this player. */
const VkrAnimationAsset *
vkr_animation_player_asset(const VkrAnimationPlayer *player);

/* Invalid input or failed pose evaluation preserves all published state.
 * Advance accepts finite, nonnegative elapsed seconds. Looping uses
 * [0,duration), including reverse playback; nonlooping clamps and retains
 * playing intent. Seek clamps to [0,duration] even when looping, permitting
 * endpoint inspection. Selecting a clip resets time to zero and retains
 * rate/playing intent. */
bool8_t vkr_animation_player_advance(VkrAnimationPlayer *player, float64_t dt);
bool8_t vkr_animation_player_seek(VkrAnimationPlayer *player,
                                  float64_t seconds);
bool8_t vkr_animation_player_select_clip(VkrAnimationPlayer *player,
                                         uint32_t clip, bool8_t loop);
void vkr_animation_player_set_playing(VkrAnimationPlayer *player,
                                      bool8_t playing);
bool8_t vkr_animation_player_set_rate(VkrAnimationPlayer *player,
                                      float64_t rate);
/* Blend sampled local TRS. All entries require valid clips, finite seconds and
 * finite nonnegative weights; zero-weight entries are not evaluated. Positive
 * weights are normalized. Translation/scale use weighted means; rotations use
 * a normalized quaternion sum after flipping to the first positive sample's
 * hemisphere (nlerp, not spherical interpolation). Matrix-authored nodes retain
 * their authored matrices. No allocations occur and every failure retains the
 * displayed local/global/palette pose and playback controls. Success cancels a
 * fade but leaves clip/time/rate/playing controls unchanged: an external graph
 * or timeline owns these explicit sample clocks. */
bool8_t vkr_animation_player_sample_blend(VkrAnimationPlayer *player,
                                          const VkrAnimationSample *samples,
                                          uint32_t count,
                                          bool8_t discontinuity);

/* Crossfade to target clip time zero. Duration zero selects immediately.
 * Positive durations preserve the displayed pose on entry and advance both
 * source and target clocks at the playback rate. Interruptions and externally
 * blended poses use a captured displayed local pose as the new fade source.
 * Elapsed fade seconds advance only while playing, independently of rate;
 * rate zero freezes clip clocks but still advances a playing transition.
 * During a fade clip/time name the target. Successful seek/select cancel it. */
bool8_t vkr_animation_player_crossfade(VkrAnimationPlayer *player,
                                       uint32_t clip, bool8_t loop,
                                       float64_t duration);
bool8_t vkr_animation_player_crossfade_active(const VkrAnimationPlayer *player);
float64_t
vkr_animation_player_crossfade_duration(const VkrAnimationPlayer *player);
float64_t
vkr_animation_player_crossfade_progress(const VkrAnimationPlayer *player);
float64_t vkr_animation_player_rate(const VkrAnimationPlayer *player);
bool8_t vkr_animation_player_loop(const VkrAnimationPlayer *player);
float64_t vkr_animation_player_duration(const VkrAnimationPlayer *player);
float64_t vkr_animation_player_time(const VkrAnimationPlayer *player);
uint32_t vkr_animation_player_clip(const VkrAnimationPlayer *player);
bool8_t vkr_animation_player_playing(const VkrAnimationPlayer *player);

/* Views expire at the next successful pose mutation or destroy. Global view has
 * asset.node_count matrices; palette has asset.skins[skin].joint_count
 * matrices. Generation increments on published poses. Discontinuity increments
 * on seek, selection, loop wrap or an explicitly discontinuous blend. Neither
 * identifies a previous rendered pose:
 * a render consumer must retain its own last-submitted transform history. Root
 * motion is retained in the pose; no authored scene transform is modified. */
const Mat4 *vkr_animation_player_global_pose(const VkrAnimationPlayer *player);
const Mat4 *vkr_animation_player_skin_palette(const VkrAnimationPlayer *player,
                                              uint32_t skin);
uint64_t vkr_animation_player_generation(const VkrAnimationPlayer *player);
uint64_t vkr_animation_player_discontinuity(const VkrAnimationPlayer *player);

/* Physics supplies global matrices in animation-wrapper space. Overrides and
 * their descendants rebuild palettes transactionally in existing pose storage;
 * authored assets and sampled local TRS remain unchanged. Next sampling
 * replaces overrides. This is not an animation/ragdoll blend-back controller.
 */
bool8_t vkr_animation_player_override_globals(VkrAnimationPlayer *player,
                                              const uint32_t *nodes,
                                              const Mat4 *matrices,
                                              uint32_t count);

/* Cold reset transaction. Token storage belongs to the supplied arena. Between
 * begin and finish only one seek/sample_blend is permitted; it writes the
 * inactive pose slot. Cancel restores playback/crossfade state and the
 * untouched original pose slot without resampling. Finish before releasing the
 * arena. */
typedef struct s_VkrAnimationPlayerCheckpoint VkrAnimationPlayerCheckpoint;
VkrAnimationPlayerCheckpoint *
vkr_animation_player_checkpoint_begin(VkrAnimationPlayer *player,
                                      struct Arena *arena);
void vkr_animation_player_checkpoint_finish(
    VkrAnimationPlayerCheckpoint *checkpoint, bool8_t commit);
