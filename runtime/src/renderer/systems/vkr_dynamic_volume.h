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
  /* This frame's active bricks in brick order, each with the position of
     its first probe among the frame's active probes, and every brick's
     probe count (its valid probes; all of them for a runtime grid), so the
     round-robin cursor expands only the probes it traces. */
  uint32_t *active_list;
  uint32_t *active_first;
  uint8_t *brick_probes;
  /* Cells of 4 x 4 x 4 indirection entries (x fastest): cell c lists the
     bricks that overlap it, cell_bricks[cell_first[c]] up to
     cell_bricks[cell_first[c + 1]], and cell_unmarked[c] counts those this
     frame has not marked, so a source skips a cell whose bricks are all
     active. */
  uint32_t cell_dimensions[3];
  uint32_t *cell_first;
  uint32_t *cell_bricks;
  uint32_t *cell_unmarked;
  /* Past the light cap, each kept point light slot's luminous power and a
     min-heap of the slots by it (VKR_DYNAMIC_VOLUME_LIGHTS_MAX each). */
  float32_t *point_power;
  uint32_t *point_heap;
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

/**
 * Sets the scene's light groups that left the bake
 * (`VkrSceneLightGroups.left_bake`): those with a static light that differs
 * from the volume's bake record of it, a static light the bake did not hold,
 * or a record no light matches. Groups leave only where lamp lightmaps hold
 * the static lamps, which cannot take one lamp's light away, and only from
 * a `traced` volume, one the dynamic layer updates, since nothing else would
 * light them instead. Call before the frame weighs its layers and gathers
 * its lights.
 */
void vkr_dynamic_volume_update_baked_groups(struct VkrScene *scene,
                                            bool8_t traced);
