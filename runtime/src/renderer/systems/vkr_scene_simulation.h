#pragma once

#include "defines.h"

#define VKR_SCENE_SIMULATION_FIXED_DT (1.0 / 60.0)

struct VkrScene;

/* Called on the scene's owning thread. Tick IDs start at one. State writes,
 * queries and native physics intents are allowed; structural ECS/scene edits,
 * recursive updates and callback replacement are forbidden until return.
 * after_physics runs after native contact dispatch has returned. */
typedef bool8_t (*VkrSceneTickCallback)(struct VkrScene *scene, uint64_t tick,
                                        void *context);
typedef void (*VkrSceneResetCallback)(struct VkrScene *scene, void *context);

typedef struct VkrSceneSimulationCallbacks {
  VkrSceneTickCallback before_physics;
  VkrSceneTickCallback after_physics;
  /* Infallible restore of caller-owned gameplay state after native reset. */
  VkrSceneResetCallback reset;
  void *context;
} VkrSceneSimulationCallbacks;

typedef enum VkrSceneSimulationPhase {
  VKR_SCENE_SIMULATION_IDLE,
  VKR_SCENE_SIMULATION_BEFORE_PHYSICS,
  VKR_SCENE_SIMULATION_PHYSICS,
  VKR_SCENE_SIMULATION_AFTER_PHYSICS,
  VKR_SCENE_SIMULATION_RESET,
} VkrSceneSimulationPhase;

typedef struct VkrSceneSimulation {
  VkrSceneSimulationPhase phase;
  VkrSceneSimulationCallbacks callbacks;
  float64_t accumulator;
  uint64_t completed_ticks;
  uint64_t animation_ticks;
  uint32_t overload_updates;
  bool8_t enabled;
  bool8_t active;
  bool8_t in_callback;
  bool8_t faulted;
  /* A failing callback may provide a message valid through its return.
   * The coordinator copies it before publishing the fault. */
  const char *error;
  char error_storage[192];
} VkrSceneSimulation;

/* Configure at a paused, reset boundary (tick zero). The context is borrowed
 * until replacement or scene shutdown. NULL disables callbacks. No allocation.
 */
bool8_t
vkr_scene_simulation_configure(struct VkrScene *scene,
                               const VkrSceneSimulationCallbacks *callbacks,
                               const char **error);
/* Releases only the matching borrowed context at a paused boundary. Does not
 * reset native/gameplay state or erase clock/debt/fault diagnostics. */
bool8_t vkr_scene_simulation_detach(struct VkrScene *scene, void *context);
uint64_t vkr_scene_simulation_completed_ticks(const struct VkrScene *scene);

/* Scene/physics coordinator internals. Use scene_update and the existing
 * physics pause/step/reset entry points to drive the shared clock. */
void vkr_scene_simulation_update(struct VkrScene *scene, float64_t dt);
bool8_t vkr_scene_simulation_tick(struct VkrScene *scene, const char **error);
void vkr_scene_simulation_reset_state(struct VkrScene *scene);
