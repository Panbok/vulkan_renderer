#pragma once

#include "defines.h"
#include "vkr_frame_input.h"

/* Selection of the diffuse volume's dynamic layer (ADR-119): each frame
   names the light the bake lacks, the probes in its reach that trace it, and
   the probes it has left, which clear. The renderer traces on the desktop
   pipeline with ray queries. */

struct VkrScene;
struct VkrMaterialSystem;
struct VkrRenderAssets;

/* Share of a traced probe's new light that replaces its stored light. */
#define VKR_DYNAMIC_VOLUME_BLEND 0.25f

/** Selection state of one scene volume, owned by the runtime. */
typedef struct VkrDynamicVolumeSelection {
  /* The volume this state follows; another resets it. */
  const void *volume;
  uint32_t brick_count;
  /* Bricks whose probes may hold dynamic light, one bit each. */
  uint64_t *active_bricks;
  uint64_t *next_bricks;
  uint32_t brick_words;
  /* This frame's active probes, reused from frame to frame. */
  uint32_t *active_probes;
  uint32_t active_probe_capacity;
  /* Round-robin position in the active probes. */
  uint32_t cursor;
  uint32_t sequence;
} VkrDynamicVolumeSelection;

/** One frame's update, borrowed by the frame packet until it completes. */
typedef struct VkrDynamicVolumeFrame {
  VkrDynamicVolumeInput input;
  uint32_t trace_probes[VKR_DYNAMIC_VOLUME_TRACE_PROBES_MAX];
  uint32_t clear_probes[VKR_DYNAMIC_VOLUME_CLEAR_PROBES_MAX];
  VkrPointLight point_lights[VKR_DYNAMIC_VOLUME_LIGHTS_MAX];
  float32_t point_light_signs[VKR_DYNAMIC_VOLUME_LIGHTS_MAX];
  VkrRectangleLight rectangle_lights[VKR_DYNAMIC_VOLUME_RECTANGLE_LIGHTS_MAX];
  float32_t rectangle_light_signs[VKR_DYNAMIC_VOLUME_RECTANGLE_LIGHTS_MAX];
} VkrDynamicVolumeFrame;

void vkr_dynamic_volume_selection_destroy(VkrDynamicVolumeSelection *selection);

/**
 * Fills `out` with the frame's update of `scene`'s volume from its dynamic
 * lights and the emissive rows of `world` past its static candidates,
 * tracing at most `probe_budget` probes, the next in turn. False
 * when there is nothing to trace or clear, or the volume has no dynamic
 * layer; the frame then carries no update.
 */
bool8_t vkr_dynamic_volume_select(VkrDynamicVolumeSelection *selection,
                                  const struct VkrScene *scene,
                                  const VkrWorldPassPayload *world,
                                  struct VkrMaterialSystem *materials,
                                  uint32_t probe_budget,
                                  VkrDynamicVolumeFrame *out);

/**
 * Lays the runtime grid over `scene` when its document's volume is not baked
 * yet (`"diffuse_volume": {"runtime_grid": true}`): a uniform level-0 grid of
 * at most 1024 bricks over the static
 * candidates of `world`, replaced when they outgrow it. True when the scene
 * holds a grid afterwards; false for a baked volume, a volume left
 * disabled, or a failure.
 */
bool8_t vkr_dynamic_volume_grid_update(struct VkrScene *scene,
                                       struct VkrRenderAssets *assets,
                                       const VkrWorldPassPayload *world);
