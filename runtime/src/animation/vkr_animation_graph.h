#pragma once

#include "animation/vkr_animation_player.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"

#define VKR_ANIMATION_GRAPH_NODES 32u
#define VKR_ANIMATION_GRAPH_PARAMETERS 8u
#define VKR_ANIMATION_GRAPH_STATES 16u
#define VKR_ANIMATION_GRAPH_TRANSITIONS 32u
#define VKR_ANIMATION_GRAPH_SAMPLES 8u
#define VKR_ANIMATION_GRAPH_TRIANGLES 12u

typedef enum VkrAnimationGraphNodeKind {
  VKR_ANIMATION_GRAPH_CLIP,
  VKR_ANIMATION_GRAPH_BLEND2,
  VKR_ANIMATION_GRAPH_BLENDSPACE1D,
  VKR_ANIMATION_GRAPH_BLENDSPACE2D
} VkrAnimationGraphNodeKind;

typedef struct VkrAnimationGraphSample {
  uint32_t clip;
  float32_t x;
  float32_t y;
} VkrAnimationGraphSample;

typedef struct VkrAnimationGraphTriangle {
  uint32_t samples[3];
} VkrAnimationGraphTriangle;

typedef struct VkrAnimationGraphNode {
  VkrAnimationGraphNodeKind kind;
  uint32_t clip;
  uint32_t inputs[2];
  uint32_t parameter_x;
  uint32_t parameter_y;
  uint32_t sample_count;
  uint32_t triangle_count;
  VkrAnimationGraphSample samples[VKR_ANIMATION_GRAPH_SAMPLES];
  VkrAnimationGraphTriangle triangles[VKR_ANIMATION_GRAPH_TRIANGLES];
} VkrAnimationGraphNode;

typedef struct VkrAnimationGraphState {
  uint32_t root;
  float64_t cycle_seconds;
} VkrAnimationGraphState;

typedef enum VkrAnimationGraphComparison {
  VKR_ANIMATION_GRAPH_GREATER_EQUAL,
  VKR_ANIMATION_GRAPH_LESS_EQUAL
} VkrAnimationGraphComparison;

typedef struct VkrAnimationGraphTransition {
  uint32_t from;
  uint32_t to;
  uint32_t parameter;
  VkrAnimationGraphComparison comparison;
  float32_t threshold;
  float64_t duration;
  /* Negative disables the exit gate; otherwise normalized time in [0,1]. */
  float64_t exit_time;
} VkrAnimationGraphTransition;

typedef struct VkrAnimationGraph {
  uint32_t node_count;
  uint32_t parameter_count;
  uint32_t root;
  uint32_t state_count;
  uint32_t initial_state;
  uint32_t transition_count;
  float64_t cycle_seconds;
  float32_t parameters[VKR_ANIMATION_GRAPH_PARAMETERS];
  VkrAnimationGraphNode nodes[VKR_ANIMATION_GRAPH_NODES];
  VkrAnimationGraphState states[VKR_ANIMATION_GRAPH_STATES];
  VkrAnimationGraphTransition transitions[VKR_ANIMATION_GRAPH_TRANSITIONS];
} VkrAnimationGraph;

/* Caller-owned fixed storage. Configuration is copied on initialize; the asset
 * is borrowed until the instance is discarded. No evaluation allocates. */
typedef struct VkrAnimationGraphInstance {
  VkrAnimationGraph graph;
  const VkrAnimationAsset *asset;
  float32_t parameters[VKR_ANIMATION_GRAPH_PARAMETERS];
  float64_t time;
  float64_t state_time;
  float64_t target_time;
  float64_t transition_time;
  float64_t time_compensation;
  float64_t state_compensation;
  float64_t target_compensation;
  float64_t transition_compensation;
  uint32_t state;
  uint32_t transition;
  bool8_t transitioning;
  bool8_t discontinuity;
} VkrAnimationGraphInstance;

bool8_t vkr_animation_graph_validate(const VkrAnimationGraph *graph,
                                     const VkrAnimationAsset *asset,
                                     const char **error);
bool8_t vkr_animation_graph_initialize(VkrAnimationGraphInstance *instance,
                                       const VkrAnimationGraph *graph,
                                       const VkrAnimationAsset *asset,
                                       const char **error);
bool8_t vkr_animation_graph_set_parameter(VkrAnimationGraphInstance *instance,
                                          uint32_t parameter, float32_t value);
/* Clips in each root share normalized phase; cycle_seconds controls speed.
 * State transitions are ordered and cannot interrupt an active crossfade.
 * Advance accepts nonnegative seconds. Seek resets to the initial state and
 * replays with current parameter values, not an unavailable input history.
 * At most 128 state/transition boundaries are processed per call; excessive
 * catch-up or zero-duration cycles fail without skipping control work.
 * Failed evaluation preserves instance clocks and the player's last pose. */
bool8_t vkr_animation_graph_evaluate(VkrAnimationGraphInstance *instance,
                                     VkrAnimationPlayer *player);
bool8_t vkr_animation_graph_advance(VkrAnimationGraphInstance *instance,
                                    VkrAnimationPlayer *player, float64_t dt);
bool8_t vkr_animation_graph_seek(VkrAnimationGraphInstance *instance,
                                 VkrAnimationPlayer *player, float64_t seconds);
bool8_t vkr_animation_graph_reset(VkrAnimationGraphInstance *instance,
                                  VkrAnimationPlayer *player);
/* Version 1 self-contained JSON. A null asset validates structure only;
 * initialize always requires an asset and validates clip references.
 * The writer appends to the supplied existing JSON writer. */
bool8_t vkr_animation_graph_read_json(VkrJsonReader *reader,
                                      const VkrAnimationAsset *asset,
                                      VkrAnimationGraph *out,
                                      const char **error);
bool8_t vkr_animation_graph_write_json(VkrJsonWriter *writer,
                                       const VkrAnimationGraph *graph);
