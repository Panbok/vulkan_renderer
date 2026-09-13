#pragma once

#include "containers/str.h"
#include "math/mat.h"
#include "memory/vkr_allocator.h"

#define VKR_ANIMATION_MAX_NODES 65536u
#define VKR_ANIMATION_MAX_CLIPS 4096u
#define VKR_ANIMATION_MAX_KEYS 16777216u
#define VKR_ANIMATION_NO_NODE UINT32_MAX

typedef enum VkrAnimationPath {
  VKR_ANIMATION_TRANSLATION = 0,
  VKR_ANIMATION_ROTATION,
  VKR_ANIMATION_SCALE,
} VkrAnimationPath;

typedef enum VkrAnimationInterpolation {
  VKR_ANIMATION_STEP = 0,
  VKR_ANIMATION_LINEAR,
  VKR_ANIMATION_CUBIC_SPLINE,
} VkrAnimationInterpolation;

typedef struct VkrAnimationTrs {
  Vec3 translation;
  Vec4 rotation; /* glTF quaternion order: x, y, z, w. */
  Vec3 scale;
} VkrAnimationTrs;

typedef struct VkrAnimationNode {
  String8 name;
  uint32_t parent;
  Mat4 local;
  VkrAnimationTrs rest;
  bool8_t matrix_authored;
} VkrAnimationNode;

typedef struct VkrAnimationSkin {
  String8 name;
  uint32_t skeleton_node;
  uint32_t joint_count;
  uint32_t *joints; /* Indices into nodes, in source skin.joints order. */
  Mat4 *inverse_bind;
} VkrAnimationSkin;

typedef struct VkrAnimationChannel {
  uint32_t node;
  VkrAnimationPath path;
  VkrAnimationInterpolation interpolation;
  uint32_t key_count;
  float32_t *times; /* Strictly increasing, nonnegative seconds. */
  /* One Vec4 per key, or in/value/out triplets for cubic. Unused TRS w is 0. */
  Vec4 *values;
} VkrAnimationChannel;

typedef struct VkrAnimationClip {
  String8 name;
  float32_t
      duration; /* Maximum channel endpoint, preserving source time zero. */
  uint32_t channel_count;
  VkrAnimationChannel *channels;
} VkrAnimationClip;

/* Result-arena-owned immutable bank. Node indices preserve source glTF indices.
 * A skin binding is independent of shared mesh geometry; this bank carries no
 * vertex influences or implicit mesh-to-skin assignment. */
typedef struct VkrAnimationAsset {
  uint64_t
      source_fingerprint; /* JSON and loaded source buffers, not a mesh ID. */
  uint32_t node_count;
  uint32_t skin_count;
  uint32_t clip_count;
  VkrAnimationNode *nodes;
  uint32_t *node_order; /* Permutation with parents before children. */
  VkrAnimationSkin *skins;
  VkrAnimationClip *clips;
} VkrAnimationAsset;

/* Cold-boundary validation. Error strings are static; scratch may be released
 * on return. Validated banks are immutable for every sampler borrow. */
bool8_t vkr_animation_validate(const VkrAnimationAsset *asset,
                               VkrAllocator *scratch, const char **error);

/* Clamp a finite query time to channel endpoints. No implicit looping or
 * event/control state. Arrays have node_count entries; no allocation occurs.
 * Call only with a successfully validated immutable asset. */
bool8_t vkr_animation_sample(const VkrAnimationAsset *asset, uint32_t clip,
                             float64_t seconds, VkrAnimationTrs *local_pose);
bool8_t vkr_animation_global_pose(const VkrAnimationAsset *asset,
                                  const VkrAnimationTrs *local_pose,
                                  Mat4 *global_pose);
/* Asset-space palette: global_pose[joints[j]] * inverse_bind[j]. The renderer
 * applies the character wrapper transform, not the mesh-node transform again.
 */
bool8_t vkr_animation_skin_palette(const VkrAnimationAsset *asset,
                                   uint32_t skin, const Mat4 *global_pose,
                                   Mat4 *palette);
